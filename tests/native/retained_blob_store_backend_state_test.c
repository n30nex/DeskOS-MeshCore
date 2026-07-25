#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"
#include "hal/rp2040_bridge.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "mesh/packet_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "storage/factory_reset.h"
#include "storage/retained_blob_store.h"

#define TEST_RETAINED_PARTITION "d1l_retained"
#define TEST_RETAINED_META_PARTITION "d1l_ret_meta"
#define TEST_RETAINED_META_SIZE 0x1000U
#define TEST_RETAINED_NVS_SIZE 0x1f000U
#define TEST_RETAINED_META_MAGIC 0x44314C52U
#define TEST_RETAINED_META_LEGACY_VERSION 1U
#define TEST_RETAINED_META_VERSION 2U
#define TEST_NVS_ENTRY_COUNT 16U
#define TEST_NVS_HANDLE_COUNT 4U
#define TEST_NVS_NAME_MAX 31U
#define TEST_NVS_BLOB_MAX 32768U
#define TEST_NVS_EVENT_COUNT 512U
#define TEST_NVS_TOTAL_ENTRIES 512U
#define TEST_SD_FILE_COUNT 96U
#define TEST_SD_FILE_DATA_MAX 32768U
#define TEST_SD_EVENT_COUNT 1024U

typedef enum {
    TEST_NVS_EVENT_INIT_PARTITION = 0,
    TEST_NVS_EVENT_OPEN_DEDICATED,
    TEST_NVS_EVENT_OPEN_LEGACY,
    TEST_NVS_EVENT_GET_DEDICATED,
    TEST_NVS_EVENT_GET_LEGACY,
    TEST_NVS_EVENT_SET_DEDICATED,
    TEST_NVS_EVENT_SET_LEGACY,
    TEST_NVS_EVENT_ERASE_DEDICATED,
    TEST_NVS_EVENT_ERASE_LEGACY,
    TEST_NVS_EVENT_COMMIT_DEDICATED,
    TEST_NVS_EVENT_COMMIT_LEGACY,
    TEST_NVS_EVENT_FIND_META,
    TEST_NVS_EVENT_FIND_RETAINED,
    TEST_NVS_EVENT_READ_META,
    TEST_NVS_EVENT_READ_RETAINED,
    TEST_NVS_EVENT_ERASE_META,
    TEST_NVS_EVENT_ERASE_RETAINED,
    TEST_NVS_EVENT_WRITE_META,
} test_nvs_event_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t magic_inverse;
    uint32_t version_inverse;
} test_retained_marker_t;

typedef enum {
    TEST_NVS_PENDING_NONE = 0,
    TEST_NVS_PENDING_SET,
    TEST_NVS_PENDING_ERASE,
} test_nvs_pending_t;

typedef struct {
    bool used;
    bool dedicated;
    char namespace_name[TEST_NVS_NAME_MAX + 1U];
    char key[TEST_NVS_NAME_MAX + 1U];
    uint8_t blob[TEST_NVS_BLOB_MAX];
    size_t blob_len;
} test_nvs_entry_t;

typedef struct {
    bool active;
    bool dedicated;
    char namespace_name[TEST_NVS_NAME_MAX + 1U];
    test_nvs_pending_t pending;
    char pending_key[TEST_NVS_NAME_MAX + 1U];
    uint8_t pending_blob[TEST_NVS_BLOB_MAX];
    size_t pending_blob_len;
} test_nvs_handle_t;

typedef enum {
    TEST_SD_EVENT_STAT = 0,
    TEST_SD_EVENT_READ,
    TEST_SD_EVENT_WRITE,
    TEST_SD_EVENT_DELETE,
    TEST_SD_EVENT_RENAME,
    TEST_SD_EVENT_LINEAGE_COMMIT,
} test_sd_event_kind_t;

typedef struct {
    bool used;
    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
    uint8_t data[TEST_SD_FILE_DATA_MAX];
    size_t length;
} test_sd_file_t;

typedef struct {
    test_sd_event_kind_t kind;
    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
} test_sd_event_t;

static bool s_toggle_backend_during_write;
static bool s_toggle_backend_during_delete;
static bool s_worker_should_yield;
static bool s_chunked_read_case;
static uint32_t s_chunked_read_count;
static bool s_chunked_write_yield_case;
static uint32_t s_chunked_write_count;
static uint32_t s_rename_count;
static uint32_t s_delete_count;
static bool s_nvs_enabled;
static esp_err_t s_partition_init_result = ESP_OK;
static esp_err_t s_anchor_commit_result = ESP_OK;
static esp_err_t s_sentinel_commit_result = ESP_OK;
static esp_err_t s_dedicated_commit_result = ESP_OK;
static esp_err_t s_legacy_commit_result = ESP_OK;
static esp_err_t s_nvs_stats_result = ESP_OK;
static size_t s_nvs_get_attempt;
static size_t s_nvs_get_fail_on_attempt;
static size_t s_meta_write_attempt;
static size_t s_meta_write_fail_on_attempt;
static esp_err_t s_meta_erase_result = ESP_OK;
static bool s_meta_erase_partial;
static char s_init_partition[TEST_NVS_NAME_MAX + 1U];
static test_nvs_entry_t s_nvs_entries[TEST_NVS_ENTRY_COUNT];
static test_nvs_handle_t s_nvs_handles[TEST_NVS_HANDLE_COUNT];
static test_nvs_event_t s_nvs_events[TEST_NVS_EVENT_COUNT];
static size_t s_nvs_event_count;
static uint8_t s_retained_meta[TEST_RETAINED_META_SIZE];
static bool s_retained_partition_erased;
static size_t s_retained_partition_read_count;
static bool s_sd_file_mode;
static bool s_replace_card_during_delete;
static int64_t s_now_us;
static test_sd_file_t s_sd_files[TEST_SD_FILE_COUNT];
static test_sd_event_t s_sd_events[TEST_SD_EVENT_COUNT];
static size_t s_sd_event_count;
static const esp_partition_t s_meta_partition = {
    .type = ESP_PARTITION_TYPE_DATA,
    .subtype = 0x40U,
    .address = 0x7e0000U,
    .size = TEST_RETAINED_META_SIZE,
    .label = TEST_RETAINED_META_PARTITION,
};
static const esp_partition_t s_retained_partition = {
    .type = ESP_PARTITION_TYPE_DATA,
    .subtype = ESP_PARTITION_SUBTYPE_DATA_NVS,
    .address = 0x7e1000U,
    .size = TEST_RETAINED_NVS_SIZE,
    .label = TEST_RETAINED_PARTITION,
};

static test_retained_marker_t valid_retained_marker(void)
{
    const test_retained_marker_t marker = {
        .magic = TEST_RETAINED_META_MAGIC,
        .version = TEST_RETAINED_META_VERSION,
        .magic_inverse = ~TEST_RETAINED_META_MAGIC,
        .version_inverse = ~TEST_RETAINED_META_VERSION,
    };
    return marker;
}

static test_retained_marker_t legacy_retained_marker(void)
{
    test_retained_marker_t marker = valid_retained_marker();
    marker.version = TEST_RETAINED_META_LEGACY_VERSION;
    marker.version_inverse = ~marker.version;
    return marker;
}

static void seed_valid_retained_markers(void)
{
    const test_retained_marker_t marker = valid_retained_marker();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    memcpy(s_retained_meta, &marker, sizeof(marker));
    memcpy(s_retained_meta + sizeof(s_retained_meta) - sizeof(marker),
           &marker, sizeof(marker));
}

static void seed_nvs_blob(bool dedicated, const char *namespace_name,
                          const char *key, const void *blob, size_t blob_len);

static void seed_valid_default_sentinel(void)
{
    const test_retained_marker_t marker = valid_retained_marker();
    seed_nvs_blob(false, "d1l_ret_meta", "initialized",
                  &marker, sizeof(marker));
}

static void seed_valid_dedicated_anchor(void)
{
    const test_retained_marker_t marker = valid_retained_marker();
    seed_nvs_blob(true, "d1l_ret_meta", "anchor",
                  &marker, sizeof(marker));
}

static void copy_name(char *dst, const char *src)
{
    assert(dst);
    assert(src);
    assert(strlen(src) <= TEST_NVS_NAME_MAX);
    strcpy(dst, src);
}

static void note_sd_event(test_sd_event_kind_t kind, const char *path)
{
    assert(path);
    assert(strlen(path) <= D1L_RP2040_FILE_PATH_MAX);
    assert(s_sd_event_count < TEST_SD_EVENT_COUNT);
    s_sd_events[s_sd_event_count].kind = kind;
    strcpy(s_sd_events[s_sd_event_count].path, path);
    s_sd_event_count++;
}

static test_sd_file_t *find_sd_file(const char *path)
{
    assert(path);
    for (size_t i = 0U; i < TEST_SD_FILE_COUNT; ++i) {
        if (s_sd_files[i].used && strcmp(s_sd_files[i].path, path) == 0) {
            return &s_sd_files[i];
        }
    }
    return NULL;
}

static test_sd_file_t *allocate_sd_file(const char *path)
{
    test_sd_file_t *file = find_sd_file(path);
    if (file) {
        return file;
    }
    for (size_t i = 0U; i < TEST_SD_FILE_COUNT; ++i) {
        if (!s_sd_files[i].used) {
            s_sd_files[i].used = true;
            strcpy(s_sd_files[i].path, path);
            return &s_sd_files[i];
        }
    }
    assert(false && "native SD file capacity exhausted");
    return NULL;
}

static void seed_sd_file(const char *path, const void *data, size_t length)
{
    assert(data || length == 0U);
    assert(length <= TEST_SD_FILE_DATA_MAX);
    test_sd_file_t *file = allocate_sd_file(path);
    memset(file->data, 0, sizeof(file->data));
    if (length > 0U) {
        memcpy(file->data, data, length);
    }
    file->length = length;
}

static void reset_sd_files(void)
{
    memset(s_sd_files, 0, sizeof(s_sd_files));
    memset(s_sd_events, 0, sizeof(s_sd_events));
    s_sd_event_count = 0U;
    s_sd_file_mode = true;
    s_replace_card_during_delete = false;
}

static void reset_sd_events(void)
{
    memset(s_sd_events, 0, sizeof(s_sd_events));
    s_sd_event_count = 0U;
}

static size_t first_sd_event(test_sd_event_kind_t kind, const char *path)
{
    for (size_t i = 0U; i < s_sd_event_count; ++i) {
        if (s_sd_events[i].kind == kind &&
            strcmp(s_sd_events[i].path, path) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t count_sd_event(test_sd_event_kind_t kind, const char *path)
{
    size_t count = 0U;
    for (size_t i = 0U; i < s_sd_event_count; ++i) {
        if (s_sd_events[i].kind == kind &&
            strcmp(s_sd_events[i].path, path) == 0) {
            count++;
        }
    }
    return count;
}

static size_t first_sd_event_after(test_sd_event_kind_t kind,
                                   const char *path, size_t after)
{
    for (size_t i = after + 1U; i < s_sd_event_count; ++i) {
        if (s_sd_events[i].kind == kind &&
            strcmp(s_sd_events[i].path, path) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void note_nvs_event(test_nvs_event_t event)
{
    assert(s_nvs_event_count < TEST_NVS_EVENT_COUNT);
    s_nvs_events[s_nvs_event_count++] = event;
}

static size_t count_nvs_event(test_nvs_event_t event)
{
    size_t count = 0U;
    for (size_t i = 0; i < s_nvs_event_count; ++i) {
        if (s_nvs_events[i] == event) {
            count++;
        }
    }
    return count;
}

static size_t first_nvs_event(test_nvs_event_t event)
{
    for (size_t i = 0; i < s_nvs_event_count; ++i) {
        if (s_nvs_events[i] == event) {
            return i;
        }
    }
    return SIZE_MAX;
}

static test_nvs_entry_t *find_nvs_entry(bool dedicated,
                                        const char *namespace_name,
                                        const char *key)
{
    for (size_t i = 0; i < TEST_NVS_ENTRY_COUNT; ++i) {
        test_nvs_entry_t *entry = &s_nvs_entries[i];
        if (entry->used && entry->dedicated == dedicated &&
            strcmp(entry->namespace_name, namespace_name) == 0 &&
            strcmp(entry->key, key) == 0) {
            return entry;
        }
    }
    return NULL;
}

static test_nvs_entry_t *allocate_nvs_entry(bool dedicated,
                                            const char *namespace_name,
                                            const char *key)
{
    test_nvs_entry_t *entry = find_nvs_entry(dedicated, namespace_name, key);
    if (entry) {
        return entry;
    }
    for (size_t i = 0; i < TEST_NVS_ENTRY_COUNT; ++i) {
        entry = &s_nvs_entries[i];
        if (!entry->used) {
            entry->used = true;
            entry->dedicated = dedicated;
            copy_name(entry->namespace_name, namespace_name);
            copy_name(entry->key, key);
            return entry;
        }
    }
    assert(false && "native NVS entry capacity exhausted");
    return NULL;
}

static void seed_nvs_blob(bool dedicated, const char *namespace_name,
                          const char *key, const void *blob, size_t blob_len)
{
    assert(blob);
    assert(blob_len <= TEST_NVS_BLOB_MAX);
    test_nvs_entry_t *entry = allocate_nvs_entry(dedicated, namespace_name, key);
    memcpy(entry->blob, blob, blob_len);
    entry->blob_len = blob_len;
    if (dedicated) {
        s_retained_partition_erased = false;
    }
}

static void assert_nvs_blob(bool dedicated, const char *namespace_name,
                            const char *key, const void *blob, size_t blob_len)
{
    test_nvs_entry_t *entry = find_nvs_entry(dedicated, namespace_name, key);
    assert(entry);
    assert(entry->blob_len == blob_len);
    assert(memcmp(entry->blob, blob, blob_len) == 0);
}

static void clear_nvs_case(void)
{
    memset(s_nvs_entries, 0, sizeof(s_nvs_entries));
    memset(s_nvs_handles, 0, sizeof(s_nvs_handles));
    memset(s_nvs_events, 0, sizeof(s_nvs_events));
    s_nvs_event_count = 0U;
    s_dedicated_commit_result = ESP_OK;
    s_anchor_commit_result = ESP_OK;
    s_sentinel_commit_result = ESP_OK;
    s_legacy_commit_result = ESP_OK;
    s_nvs_stats_result = ESP_OK;
    s_nvs_get_attempt = 0U;
    s_nvs_get_fail_on_attempt = 0U;
    s_partition_init_result = ESP_OK;
    s_meta_write_attempt = 0U;
    s_meta_write_fail_on_attempt = 0U;
    s_meta_erase_result = ESP_OK;
    s_meta_erase_partial = false;
    s_retained_partition_erased = false;
    s_retained_partition_read_count = 0U;
    seed_valid_retained_markers();
}

const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t type,
    esp_partition_subtype_t subtype,
    const char *label)
{
    if (type != ESP_PARTITION_TYPE_DATA || !label) {
        return NULL;
    }
    if (strcmp(label, TEST_RETAINED_META_PARTITION) == 0 &&
        (subtype == ESP_PARTITION_SUBTYPE_ANY || subtype == 0x40U)) {
        note_nvs_event(TEST_NVS_EVENT_FIND_META);
        return &s_meta_partition;
    }
    if (strcmp(label, TEST_RETAINED_PARTITION) == 0 &&
        (subtype == ESP_PARTITION_SUBTYPE_ANY ||
         subtype == ESP_PARTITION_SUBTYPE_DATA_NVS)) {
        note_nvs_event(TEST_NVS_EVENT_FIND_RETAINED);
        return &s_retained_partition;
    }
    return NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *partition,
                             size_t src_offset,
                             void *dst,
                             size_t size)
{
    if (!partition || !dst || src_offset > partition->size ||
        size > partition->size - src_offset) {
        return ESP_ERR_INVALID_ARG;
    }
    if (partition == &s_meta_partition) {
        note_nvs_event(TEST_NVS_EVENT_READ_META);
        memcpy(dst, s_retained_meta + src_offset, size);
        return ESP_OK;
    }
    if (partition == &s_retained_partition) {
        if (s_retained_partition_read_count++ == 0U) {
            note_nvs_event(TEST_NVS_EVENT_READ_RETAINED);
        }
        memset(dst, s_retained_partition_erased ? 0xff : 0x00, size);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t esp_partition_write(const esp_partition_t *partition,
                              size_t dst_offset,
                              const void *src,
                              size_t size)
{
    if (partition != &s_meta_partition || !src ||
        dst_offset > sizeof(s_retained_meta) ||
        size > sizeof(s_retained_meta) - dst_offset) {
        return ESP_ERR_INVALID_ARG;
    }
    note_nvs_event(TEST_NVS_EVENT_WRITE_META);
    s_meta_write_attempt++;
    const uint8_t *source = (const uint8_t *)src;
    for (size_t i = 0U; i < size; ++i) {
        if ((s_retained_meta[dst_offset + i] & source[i]) != source[i]) {
            return ESP_FAIL;
        }
    }
    const size_t write_size =
        s_meta_write_fail_on_attempt == s_meta_write_attempt ? size / 2U : size;
    for (size_t i = 0U; i < write_size; ++i) {
        s_retained_meta[dst_offset + i] &= source[i];
    }
    if (s_meta_write_fail_on_attempt == s_meta_write_attempt) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset,
                                    size_t size)
{
    if (!partition || offset != 0U || size != partition->size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (partition == &s_meta_partition) {
        note_nvs_event(TEST_NVS_EVENT_ERASE_META);
        if (s_meta_erase_result != ESP_OK) {
            if (s_meta_erase_partial) {
                memset(s_retained_meta, 0xff,
                       sizeof(s_retained_meta) / 2U);
            }
            return s_meta_erase_result;
        }
        memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
        return ESP_OK;
    }
    if (partition == &s_retained_partition) {
        note_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED);
        for (size_t i = 0U; i < TEST_NVS_ENTRY_COUNT; ++i) {
            if (s_nvs_entries[i].used && s_nvs_entries[i].dedicated) {
                memset(&s_nvs_entries[i], 0, sizeof(s_nvs_entries[i]));
            }
        }
        s_retained_partition_erased = true;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t open_nvs_handle(bool dedicated, const char *namespace_name,
                                 nvs_open_mode_t open_mode,
                                 nvs_handle_t *out_handle)
{
    if (!s_nvs_enabled) {
        return ESP_FAIL;
    }
    if (!namespace_name || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    note_nvs_event(dedicated ? TEST_NVS_EVENT_OPEN_DEDICATED
                             : TEST_NVS_EVENT_OPEN_LEGACY);
    if (open_mode == NVS_READONLY) {
        bool namespace_found = false;
        for (size_t i = 0U; i < TEST_NVS_ENTRY_COUNT; ++i) {
            namespace_found = namespace_found ||
                (s_nvs_entries[i].used &&
                 s_nvs_entries[i].dedicated == dedicated &&
                 strcmp(s_nvs_entries[i].namespace_name,
                        namespace_name) == 0);
        }
        if (!namespace_found) {
            return ESP_ERR_NVS_NOT_FOUND;
        }
    }
    for (size_t i = 0; i < TEST_NVS_HANDLE_COUNT; ++i) {
        test_nvs_handle_t *handle = &s_nvs_handles[i];
        if (!handle->active) {
            memset(handle, 0, sizeof(*handle));
            handle->active = true;
            handle->dedicated = dedicated;
            copy_name(handle->namespace_name, namespace_name);
            *out_handle = (nvs_handle_t)(i + 1U);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

static test_nvs_handle_t *get_nvs_handle(nvs_handle_t handle)
{
    if (handle == 0U || handle > TEST_NVS_HANDLE_COUNT) {
        return NULL;
    }
    test_nvs_handle_t *result = &s_nvs_handles[handle - 1U];
    return result->active ? result : NULL;
}

esp_err_t nvs_flash_init_partition(const char *partition_label)
{
    if (!s_nvs_enabled || !partition_label) {
        return ESP_FAIL;
    }
    copy_name(s_init_partition, partition_label);
    note_nvs_event(TEST_NVS_EVENT_INIT_PARTITION);
    esp_err_t ret = s_partition_init_result;
    return ret;
}

esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *nvs_stats)
{
    if (!part_name || strcmp(part_name, TEST_RETAINED_PARTITION) != 0 ||
        !nvs_stats) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(nvs_stats, 0, sizeof(*nvs_stats));
    if (s_nvs_stats_result != ESP_OK) {
        return s_nvs_stats_result;
    }
    for (size_t i = 0U; i < TEST_NVS_ENTRY_COUNT; ++i) {
        if (!s_nvs_entries[i].used || !s_nvs_entries[i].dedicated) {
            continue;
        }
        nvs_stats->used_entries++;
        bool namespace_seen = false;
        for (size_t j = 0U; j < i; ++j) {
            if (s_nvs_entries[j].used && s_nvs_entries[j].dedicated &&
                strcmp(s_nvs_entries[j].namespace_name,
                       s_nvs_entries[i].namespace_name) == 0) {
                namespace_seen = true;
                break;
            }
        }
        if (!namespace_seen) {
            nvs_stats->namespace_count++;
        }
    }
    nvs_stats->total_entries = TEST_NVS_TOTAL_ENTRIES;
    nvs_stats->free_entries = TEST_NVS_TOTAL_ENTRIES - nvs_stats->used_entries;
    nvs_stats->available_entries = nvs_stats->free_entries - 4U;
    return ESP_OK;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    assert(open_mode == NVS_READONLY || open_mode == NVS_READWRITE);
    return open_nvs_handle(false, namespace_name, open_mode, out_handle);
}

esp_err_t nvs_open_from_partition(const char *part_name,
                                  const char *namespace_name,
                                  nvs_open_mode_t open_mode,
                                  nvs_handle_t *out_handle)
{
    assert(open_mode == NVS_READONLY || open_mode == NVS_READWRITE);
    if (!part_name || strcmp(part_name, TEST_RETAINED_PARTITION) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return open_nvs_handle(true, namespace_name, open_mode, out_handle);
}

void nvs_close(nvs_handle_t handle)
{
    test_nvs_handle_t *context = get_nvs_handle(handle);
    assert(context);
    memset(context, 0, sizeof(*context));
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length)
{
    test_nvs_handle_t *context = get_nvs_handle(handle);
    assert(context);
    assert(key);
    assert(length);
    note_nvs_event(context->dedicated ? TEST_NVS_EVENT_GET_DEDICATED
                                      : TEST_NVS_EVENT_GET_LEGACY);
    s_nvs_get_attempt++;
    if (s_nvs_get_fail_on_attempt != 0U &&
        s_nvs_get_attempt == s_nvs_get_fail_on_attempt) {
        return ESP_ERR_INVALID_STATE;
    }
    test_nvs_entry_t *entry = find_nvs_entry(
        context->dedicated, context->namespace_name, key);
    if (!entry) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!out_value) {
        *length = entry->blob_len;
        return ESP_OK;
    }
    if (*length < entry->blob_len) {
        *length = entry->blob_len;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, entry->blob, entry->blob_len);
    *length = entry->blob_len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length)
{
    test_nvs_handle_t *context = get_nvs_handle(handle);
    assert(context);
    assert(key);
    assert(value);
    assert(length <= TEST_NVS_BLOB_MAX);
    note_nvs_event(context->dedicated ? TEST_NVS_EVENT_SET_DEDICATED
                                      : TEST_NVS_EVENT_SET_LEGACY);
    context->pending = TEST_NVS_PENDING_SET;
    copy_name(context->pending_key, key);
    memcpy(context->pending_blob, value, length);
    context->pending_blob_len = length;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    test_nvs_handle_t *context = get_nvs_handle(handle);
    assert(context);
    assert(key);
    note_nvs_event(context->dedicated ? TEST_NVS_EVENT_ERASE_DEDICATED
                                      : TEST_NVS_EVENT_ERASE_LEGACY);
    if (!find_nvs_entry(context->dedicated, context->namespace_name, key)) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    context->pending = TEST_NVS_PENDING_ERASE;
    copy_name(context->pending_key, key);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    test_nvs_handle_t *context = get_nvs_handle(handle);
    assert(context);
    note_nvs_event(context->dedicated ? TEST_NVS_EVENT_COMMIT_DEDICATED
                                      : TEST_NVS_EVENT_COMMIT_LEGACY);
    const bool anchor_commit = context->dedicated &&
        strcmp(context->namespace_name, "d1l_ret_meta") == 0;
    const bool sentinel_commit = !context->dedicated &&
        strcmp(context->namespace_name, "d1l_ret_meta") == 0;
    const esp_err_t configured_result =
        anchor_commit ? s_anchor_commit_result :
        sentinel_commit ? s_sentinel_commit_result :
        context->dedicated ? s_dedicated_commit_result :
                             s_legacy_commit_result;
    if (configured_result != ESP_OK) {
        return configured_result;
    }
    if (context->pending == TEST_NVS_PENDING_SET) {
        test_nvs_entry_t *entry = allocate_nvs_entry(
            context->dedicated, context->namespace_name, context->pending_key);
        memcpy(entry->blob, context->pending_blob, context->pending_blob_len);
        entry->blob_len = context->pending_blob_len;
        if (context->dedicated) {
            s_retained_partition_erased = false;
        }
    } else if (context->pending == TEST_NVS_PENDING_ERASE) {
        test_nvs_entry_t *entry = find_nvs_entry(
            context->dedicated, context->namespace_name, context->pending_key);
        assert(entry);
        memset(entry, 0, sizeof(*entry));
    }
    if (s_sd_file_mode && context->pending == TEST_NVS_PENDING_SET &&
        strcmp(context->namespace_name, "d1l_reset") == 0 &&
        strncmp(context->pending_key, "sd_", 3U) == 0) {
        note_sd_event(TEST_SD_EVENT_LINEAGE_COMMIT, context->pending_key);
    }
    context->pending = TEST_NVS_PENDING_NONE;
    return ESP_OK;
}

bool d1l_route_store_persistence_should_yield(void)
{
    return s_worker_should_yield;
}

int64_t esp_timer_get_time(void)
{
    s_now_us += 1000LL;
    return s_now_us;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    return buffer;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks_to_wait)
{
    (void)handle;
    (void)ticks_to_wait;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    (void)handle;
    return pdTRUE;
}

esp_err_t d1l_rp2040_bridge_file_stat(const char *path,
                                      d1l_rp2040_file_result_t *out_result,
                                      uint32_t timeout_ms)
{
    (void)path;
    (void)timeout_ms;
    if (s_chunked_read_case) {
        assert(out_result);
        memset(out_result, 0, sizeof(*out_result));
        out_result->exists = true;
        out_result->size = D1L_RP2040_FILE_CHUNK_MAX * 2U;
        return ESP_OK;
    }
    if (s_sd_file_mode) {
        assert(path);
        assert(out_result);
        note_sd_event(TEST_SD_EVENT_STAT, path);
        memset(out_result, 0, sizeof(*out_result));
        test_sd_file_t *file = find_sd_file(path);
        if (!file) {
            strcpy(out_result->err, "not_found");
            out_result->last_error = ESP_ERR_NOT_FOUND;
            return ESP_ERR_NOT_FOUND;
        }
        out_result->ok = true;
        out_result->exists = true;
        out_result->size = (uint32_t)file->length;
        out_result->last_error = ESP_OK;
        return ESP_OK;
    }
    (void)out_result;
    return ESP_FAIL;
}

esp_err_t d1l_rp2040_bridge_file_read(const char *path, uint32_t offset,
                                      uint8_t *out_data, size_t max_len,
                                      d1l_rp2040_file_result_t *out_result,
                                      uint32_t timeout_ms)
{
    (void)path;
    (void)timeout_ms;
    if (s_chunked_read_case) {
        assert(out_data);
        assert(out_result);
        const size_t total = D1L_RP2040_FILE_CHUNK_MAX * 2U;
        assert(offset < total);
        const size_t remaining = total - offset;
        const size_t length = remaining < max_len ? remaining : max_len;
        memset(out_data, 0x5a, length);
        memset(out_result, 0, sizeof(*out_result));
        out_result->offset = offset;
        out_result->length = (uint32_t)length;
        out_result->eof = offset + length == total;
        ++s_chunked_read_count;
        s_worker_should_yield = true;
        return ESP_OK;
    }
    if (s_sd_file_mode) {
        assert(path);
        assert(out_data);
        assert(out_result);
        note_sd_event(TEST_SD_EVENT_READ, path);
        memset(out_result, 0, sizeof(*out_result));
        out_result->offset = offset;
        test_sd_file_t *file = find_sd_file(path);
        if (!file) {
            strcpy(out_result->err, "not_found");
            out_result->last_error = ESP_ERR_NOT_FOUND;
            return ESP_ERR_NOT_FOUND;
        }
        if (offset > file->length) {
            strcpy(out_result->err, "range");
            out_result->last_error = ESP_ERR_INVALID_SIZE;
            return ESP_ERR_INVALID_SIZE;
        }
        const size_t available = file->length - offset;
        const size_t length = available < max_len ? available : max_len;
        if (length > 0U) {
            memcpy(out_data, file->data + offset, length);
        }
        out_result->ok = true;
        out_result->exists = true;
        out_result->size = (uint32_t)file->length;
        out_result->length = (uint32_t)length;
        out_result->eof = offset + length == file->length;
        out_result->last_error = ESP_OK;
        return ESP_OK;
    }
    (void)offset;
    (void)out_data;
    (void)max_len;
    (void)out_result;
    return ESP_FAIL;
}

esp_err_t d1l_rp2040_bridge_file_write(const char *path, uint32_t offset,
                                       const uint8_t *data, size_t len,
                                       bool truncate,
                                       d1l_rp2040_file_result_t *out_result,
                                       uint32_t timeout_ms)
{
    (void)path;
    (void)offset;
    (void)data;
    (void)len;
    (void)truncate;
    assert(out_result);
    (void)timeout_ms;
    if (s_sd_file_mode) {
        assert(path);
        assert(data || len == 0U);
        note_sd_event(TEST_SD_EVENT_WRITE, path);
        memset(out_result, 0, sizeof(*out_result));
        test_sd_file_t *file = allocate_sd_file(path);
        if (truncate) {
            file->length = 0U;
        }
        if (offset > file->length || offset + len > sizeof(file->data)) {
            strcpy(out_result->err, "range");
            out_result->last_error = ESP_ERR_INVALID_SIZE;
            return ESP_ERR_INVALID_SIZE;
        }
        if (len > 0U) {
            memcpy(file->data + offset, data, len);
        }
        if (file->length < offset + len) {
            file->length = offset + len;
        }
        out_result->ok = true;
        out_result->exists = true;
        out_result->size = (uint32_t)file->length;
        out_result->offset = offset;
        out_result->length = (uint32_t)len;
        out_result->last_error = ESP_OK;
    } else {
        out_result->length = (uint32_t)len;
    }
    if (s_chunked_write_yield_case) {
        ++s_chunked_write_count;
        s_worker_should_yield = true;
    }
    if (s_toggle_backend_during_write) {
        s_toggle_backend_during_write = false;
        d1l_retained_blob_store_note_sd_backend(false, false, false,
                                                0U, 0U, 0U);
        d1l_retained_blob_store_note_sd_backend(
            true, true, true, D1L_RP2040_FILE_LINE_MAX,
            D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX);
    }
    return ESP_OK;
}

esp_err_t d1l_rp2040_bridge_file_delete(const char *path,
                                        d1l_rp2040_file_result_t *out_result,
                                        uint32_t timeout_ms)
{
    assert(path);
    assert(out_result);
    (void)timeout_ms;
    s_delete_count++;
    esp_err_t result = ESP_OK;
    if (s_sd_file_mode) {
        note_sd_event(TEST_SD_EVENT_DELETE, path);
        memset(out_result, 0, sizeof(*out_result));
        test_sd_file_t *file = find_sd_file(path);
        if (file) {
            memset(file, 0, sizeof(*file));
            out_result->ok = true;
            out_result->last_error = ESP_OK;
        } else {
            strcpy(out_result->err, "not_found");
            out_result->last_error = ESP_ERR_NOT_FOUND;
            result = ESP_ERR_NOT_FOUND;
        }
    }
    if (s_replace_card_during_delete) {
        static const uint8_t replacement_data[] = "replacement-card";
        s_replace_card_during_delete = false;
        memset(s_sd_files, 0, sizeof(s_sd_files));
        seed_sd_file("replacement/keep.bin", replacement_data,
                     sizeof(replacement_data));
        d1l_retained_blob_store_note_sd_backend(false, false, false,
                                                0U, 0U, 0U);
        d1l_retained_blob_store_note_sd_backend(
            true, true, true, D1L_RP2040_FILE_LINE_MAX,
            D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX);
    }
    if (s_toggle_backend_during_delete) {
        s_toggle_backend_during_delete = false;
        d1l_retained_blob_store_note_sd_backend(false, false, false,
                                                0U, 0U, 0U);
        d1l_retained_blob_store_note_sd_backend(
            true, true, true, D1L_RP2040_FILE_LINE_MAX,
            D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX);
    }
    return result;
}

esp_err_t d1l_rp2040_bridge_file_rename(const char *from_path, const char *to_path,
                                        bool replace,
                                        d1l_rp2040_file_result_t *out_result,
                                        uint32_t timeout_ms)
{
    assert(from_path);
    assert(to_path);
    assert(out_result);
    (void)timeout_ms;
    s_rename_count++;
    if (s_sd_file_mode) {
        note_sd_event(TEST_SD_EVENT_RENAME, to_path);
        memset(out_result, 0, sizeof(*out_result));
        test_sd_file_t *source = find_sd_file(from_path);
        test_sd_file_t *target = find_sd_file(to_path);
        if (!source) {
            strcpy(out_result->err, "not_found");
            out_result->last_error = ESP_ERR_NOT_FOUND;
            return ESP_ERR_NOT_FOUND;
        }
        if (target && !replace) {
            strcpy(out_result->err, "exists");
            out_result->last_error = ESP_ERR_INVALID_STATE;
            return ESP_ERR_INVALID_STATE;
        }
        if (!target) {
            target = allocate_sd_file(to_path);
        }
        const size_t length = source->length;
        memcpy(target->data, source->data, length);
        target->length = length;
        memset(source, 0, sizeof(*source));
        out_result->ok = true;
        out_result->exists = true;
        out_result->size = (uint32_t)length;
        out_result->last_error = ESP_OK;
    }
    return ESP_OK;
}

static d1l_retained_blob_store_backend_state_t state_for(
    d1l_retained_blob_store_id_t store_id)
{
    d1l_retained_blob_store_backend_state_t state = {0};
    assert(d1l_retained_blob_store_backend_state(store_id, &state));
    return state;
}

static void assert_all_states(bool enabled, uint32_t generation)
{
    for (int id = 0; id < D1L_RETAINED_BLOB_STORE_COUNT; ++id) {
        const d1l_retained_blob_store_backend_state_t state =
            state_for((d1l_retained_blob_store_id_t)id);
        assert(state.enabled == enabled);
        assert(state.generation == generation);
        assert(d1l_retained_blob_store_uses_sd(
                   (d1l_retained_blob_store_id_t)id) == enabled);
    }
}

static void test_release_profile_sd_admission(void)
{
#ifndef EXPECT_PROFILE_SD_ENABLED
#error "EXPECT_PROFILE_SD_ENABLED must bind the retained-store profile case"
#endif
    static const uint8_t payload[] = "profile-bound retained state";
    uint8_t readback[sizeof(payload)] = {0};
    size_t readback_len = sizeof(readback);

    s_nvs_enabled = true;
    reset_sd_files();
    clear_nvs_case();
    seed_valid_dedicated_anchor();
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_ready());
    assert(d1l_release_feature_available(
        D1L_RELEASE_FEATURE_RETAINED_NVS));
    assert(d1l_retained_blob_store_is_available(
        D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES));

    static const struct {
        bool data_ready;
        bool file_ops_supported;
        bool atomic_rename_supported;
        uint32_t file_line_max;
        uint32_t file_chunk_max;
        uint32_t path_max;
    } incomplete_prerequisites[] = {
        {false, true, true, D1L_RP2040_FILE_LINE_MAX,
         D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX},
        {true, false, true, D1L_RP2040_FILE_LINE_MAX,
         D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX},
        {true, true, false, D1L_RP2040_FILE_LINE_MAX,
         D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX},
        {true, true, true, D1L_RP2040_FILE_LINE_MAX - 1U,
         D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX},
        {true, true, true, D1L_RP2040_FILE_LINE_MAX,
         D1L_RP2040_FILE_CHUNK_MAX - 1U, D1L_RP2040_FILE_PATH_MAX},
        {true, true, true, D1L_RP2040_FILE_LINE_MAX,
         D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX - 1U},
    };
    for (size_t i = 0U;
         i < sizeof(incomplete_prerequisites) /
                 sizeof(incomplete_prerequisites[0]);
         ++i) {
        d1l_retained_blob_store_note_sd_backend(
            incomplete_prerequisites[i].data_ready,
            incomplete_prerequisites[i].file_ops_supported,
            incomplete_prerequisites[i].atomic_rename_supported,
            incomplete_prerequisites[i].file_line_max,
            incomplete_prerequisites[i].file_chunk_max,
            incomplete_prerequisites[i].path_max);
        assert_all_states(false, 0U);
    }

    d1l_retained_blob_store_note_sd_backend(
        true, true, true, D1L_RP2040_FILE_LINE_MAX,
        D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX);

#if EXPECT_PROFILE_SD_ENABLED
    assert(d1l_release_feature_available(
        D1L_RELEASE_FEATURE_SD_HISTORY));
    assert_all_states(true, 1U);
    assert(strcmp(d1l_retained_blob_store_backend_name(
                      D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES),
                  "sd") == 0);
#else
    assert(!d1l_release_feature_available(
        D1L_RELEASE_FEATURE_SD_HISTORY));
    assert_all_states(false, 0U);
    assert(strcmp(d1l_retained_blob_store_backend_name(
                      D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES),
                  "nvs") == 0);
    assert(d1l_retained_blob_store_write_sd_primary(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
               payload, sizeof(payload)) == ESP_ERR_INVALID_STATE);
    assert(s_sd_event_count == 0U);
#endif

    assert(d1l_retained_blob_store_write(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
               payload, sizeof(payload)) == ESP_OK);
    assert_nvs_blob(true, "d1l_messages", "public",
                    payload, sizeof(payload));
#if EXPECT_PROFILE_SD_ENABLED
    assert(s_rename_count > 0U);
#else
    assert(s_sd_event_count == 0U);
#endif

    assert(d1l_retained_blob_store_read(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
               readback, &readback_len) == ESP_OK);
    assert(readback_len == sizeof(payload));
    assert(memcmp(readback, payload, sizeof(payload)) == 0);

    assert(d1l_retained_blob_store_erase(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES,
               "public") == ESP_OK);
#if EXPECT_PROFILE_SD_ENABLED
    assert(s_delete_count > 0U);
#else
    assert(s_sd_event_count == 0U);
#endif
}

static void test_retained_partition_init(void)
{
    s_nvs_enabled = true;
    clear_nvs_case();
    seed_valid_dedicated_anchor();
    memset(s_init_partition, 0, sizeof(s_init_partition));

    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(strcmp(s_init_partition, TEST_RETAINED_PARTITION) == 0);
    assert(count_nvs_event(TEST_NVS_EVENT_INIT_PARTITION) == 1U);
    assert(d1l_retained_blob_store_nvs_marker_ready());
    assert(d1l_retained_blob_store_nvs_anchor_ready());
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    assert(!d1l_retained_blob_store_nvs_initialized_this_boot());
}

static void test_first_use_initializes_only_new_retained_region(void)
{
    static const char legacy_blob[] = "legacy survives first use";

    clear_nvs_case();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    s_retained_partition_erased = true;
    seed_nvs_blob(false, "d1l_messages", "public",
                  legacy_blob, sizeof(legacy_blob));

    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_marker_ready());
    assert(d1l_retained_blob_store_nvs_anchor_ready());
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    assert(d1l_retained_blob_store_nvs_initialized_this_boot());
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_READ_RETAINED) > 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_WRITE_META) == 2U);
    assert(first_nvs_event(TEST_NVS_EVENT_READ_RETAINED) <
           first_nvs_event(TEST_NVS_EVENT_WRITE_META));
    assert(first_nvs_event(TEST_NVS_EVENT_WRITE_META) <
           first_nvs_event(TEST_NVS_EVENT_INIT_PARTITION));
    assert(first_nvs_event(TEST_NVS_EVENT_INIT_PARTITION) <
           first_nvs_event(TEST_NVS_EVENT_SET_DEDICATED));
    assert_nvs_blob(true, "d1l_messages", "public",
                    legacy_blob, sizeof(legacy_blob));
    assert(find_nvs_entry(false, "d1l_messages", "public") == NULL);
}

static void test_first_marker_recovers_init_power_loss_window(void)
{
    const test_retained_marker_t marker = valid_retained_marker();

    clear_nvs_case();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    memcpy(s_retained_meta, &marker, sizeof(marker));
    s_retained_partition_erased = true;

    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_marker_ready());
    assert(d1l_retained_blob_store_nvs_anchor_ready());
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    assert(d1l_retained_blob_store_nvs_ready());
    assert(count_nvs_event(TEST_NVS_EVENT_INIT_PARTITION) == 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_WRITE_META) == 1U);
    test_retained_marker_t repaired = {0};
    memcpy(&repaired,
           s_retained_meta + sizeof(s_retained_meta) - sizeof(repaired),
           sizeof(repaired));
    assert(repaired.magic == TEST_RETAINED_META_MAGIC);
    assert(repaired.version == TEST_RETAINED_META_VERSION);
}

static void test_version_one_marker_candidate_upgrades_after_sentinel(void)
{
    const test_retained_marker_t legacy = legacy_retained_marker();
    const size_t last_offset = sizeof(s_retained_meta) - sizeof(legacy);

    clear_nvs_case();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    memcpy(s_retained_meta, &legacy, sizeof(legacy));
    memcpy(s_retained_meta + last_offset, &legacy, sizeof(legacy));
    s_retained_partition_erased = true;

    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_marker_ready());
    assert(d1l_retained_blob_store_nvs_markers_complete());
    assert(d1l_retained_blob_store_nvs_anchor_ready());
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    assert(!d1l_retained_blob_store_nvs_external_init_required());
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_WRITE_META) == 2U);
    assert(first_nvs_event(TEST_NVS_EVENT_COMMIT_DEDICATED) <
           first_nvs_event(TEST_NVS_EVENT_COMMIT_LEGACY));
    assert(first_nvs_event(TEST_NVS_EVENT_COMMIT_LEGACY) <
           first_nvs_event(TEST_NVS_EVENT_ERASE_META));

    test_retained_marker_t upgraded_first = {0};
    test_retained_marker_t upgraded_last = {0};
    memcpy(&upgraded_first, s_retained_meta, sizeof(upgraded_first));
    memcpy(&upgraded_last, s_retained_meta + last_offset,
           sizeof(upgraded_last));
    assert(upgraded_first.version == TEST_RETAINED_META_VERSION);
    assert(upgraded_last.version == TEST_RETAINED_META_VERSION);
}

static void test_populated_version_one_upgrade_survives_finalize_failures(void)
{
    static const char payload[] = "published v1 retained payload";
    static const struct {
        const char *namespace_name;
        const char *key;
    } stores[] = {
        {"d1l_messages", "public"},
        {"d1l_dms", "threads"},
        {"d1l_routes", "routes_v2"},
        {"d1l_packets", "ring"},
    };
    const test_retained_marker_t legacy = legacy_retained_marker();
    const size_t last_offset = sizeof(s_retained_meta) - sizeof(legacy);

    clear_nvs_case();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    memcpy(s_retained_meta, &legacy, sizeof(legacy));
    memcpy(s_retained_meta + last_offset, &legacy, sizeof(legacy));
    for (size_t i = 0U; i < sizeof(stores) / sizeof(stores[0]); ++i) {
        seed_nvs_blob(true, stores[i].namespace_name, stores[i].key,
                      payload, sizeof(payload));
        seed_nvs_blob(false, stores[i].namespace_name, stores[i].key,
                      payload, sizeof(payload));
    }
    s_sentinel_commit_result = ESP_FAIL;

    assert(d1l_retained_blob_store_init() == ESP_FAIL);
    assert(d1l_retained_blob_store_nvs_anchor_ready());
    assert(!d1l_retained_blob_store_nvs_sentinel_ready());
    assert(!d1l_retained_blob_store_nvs_markers_complete());
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 0U);
    for (size_t i = 0U; i < sizeof(stores) / sizeof(stores[0]); ++i) {
        assert_nvs_blob(true, stores[i].namespace_name, stores[i].key,
                        payload, sizeof(payload));
        assert(find_nvs_entry(false, stores[i].namespace_name,
                              stores[i].key) == NULL);
    }

    s_sentinel_commit_result = ESP_OK;
    s_meta_erase_result = ESP_FAIL;
    assert(d1l_retained_blob_store_init() == ESP_FAIL);
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    assert(!d1l_retained_blob_store_nvs_markers_complete());
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 1U);
    for (size_t i = 0U; i < sizeof(stores) / sizeof(stores[0]); ++i) {
        assert_nvs_blob(true, stores[i].namespace_name, stores[i].key,
                        payload, sizeof(payload));
    }

    s_meta_erase_result = ESP_OK;
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_markers_complete());
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    for (size_t i = 0U; i < sizeof(stores) / sizeof(stores[0]); ++i) {
        assert_nvs_blob(true, stores[i].namespace_name, stores[i].key,
                        payload, sizeof(payload));
    }
}

static void test_marker_loss_recovers_valid_nvs_and_unsafe_states_fail_closed(void)
{
    static const char dedicated_blob[] = "must survive marker failure";
    const test_retained_marker_t valid = valid_retained_marker();
    const size_t last_offset = sizeof(s_retained_meta) - sizeof(valid);

    clear_nvs_case();
    seed_nvs_blob(true, "d1l_messages", "public",
                  dedicated_blob, sizeof(dedicated_blob));
    seed_valid_dedicated_anchor();
    seed_valid_default_sentinel();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_marker_ready());
    assert(d1l_retained_blob_store_nvs_anchor_ready());
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    assert(count_nvs_event(TEST_NVS_EVENT_INIT_PARTITION) == 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_WRITE_META) == 2U);
    assert_nvs_blob(true, "d1l_messages", "public",
                    dedicated_blob, sizeof(dedicated_blob));

    /* A corrupt companion slot is rebuilt only after this boot commits the
     * default sentinel, so power loss during metadata repair remains owned. */
    clear_nvs_case();
    seed_valid_dedicated_anchor();
    seed_nvs_blob(true, "d1l_messages", "public",
                  dedicated_blob, sizeof(dedicated_blob));
    memcpy(s_retained_meta, &valid, sizeof(valid));
    test_retained_marker_t corrupt_companion = valid;
    corrupt_companion.magic ^= 1U;
    memcpy(s_retained_meta + last_offset,
           &corrupt_companion, sizeof(corrupt_companion));
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    assert(d1l_retained_blob_store_nvs_markers_complete());
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_WRITE_META) == 2U);
    assert(first_nvs_event(TEST_NVS_EVENT_COMMIT_LEGACY) <
           first_nvs_event(TEST_NVS_EVENT_ERASE_META));
    assert_nvs_blob(true, "d1l_messages", "public",
                    dedicated_blob, sizeof(dedicated_blob));

    clear_nvs_case();
    seed_nvs_blob(true, "d1l_messages", "public",
                  dedicated_blob, sizeof(dedicated_blob));
    seed_valid_dedicated_anchor();
    seed_valid_default_sentinel();
    test_retained_marker_t corrupt = valid;
    corrupt.magic ^= 1U;
    memcpy(s_retained_meta, &corrupt, sizeof(corrupt));
    memcpy(s_retained_meta + last_offset, &corrupt, sizeof(corrupt));
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(count_nvs_event(TEST_NVS_EVENT_INIT_PARTITION) == 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 1U);
    assert_nvs_blob(true, "d1l_messages", "public",
                    dedicated_blob, sizeof(dedicated_blob));

    clear_nvs_case();
    seed_nvs_blob(true, "d1l_messages", "public",
                  dedicated_blob, sizeof(dedicated_blob));
    test_retained_marker_t future = valid;
    future.version = 3U;
    future.version_inverse = ~future.version;
    memcpy(s_retained_meta, &future, sizeof(future));
    memcpy(s_retained_meta + last_offset, &future, sizeof(future));
    assert(d1l_retained_blob_store_init() == ESP_ERR_INVALID_STATE);
    assert(count_nvs_event(TEST_NVS_EVENT_INIT_PARTITION) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert_nvs_blob(true, "d1l_messages", "public",
                    dedicated_blob, sizeof(dedicated_blob));

    clear_nvs_case();
    seed_nvs_blob(true, "d1l_messages", "public",
                  dedicated_blob, sizeof(dedicated_blob));
    memcpy(s_retained_meta, &valid, sizeof(valid));
    test_retained_marker_t mixed_future = valid;
    mixed_future.version = 3U;
    mixed_future.version_inverse = ~mixed_future.version;
    memcpy(s_retained_meta + last_offset,
           &mixed_future, sizeof(mixed_future));
    assert(d1l_retained_blob_store_init() == ESP_ERR_INVALID_STATE);
    assert(count_nvs_event(TEST_NVS_EVENT_INIT_PARTITION) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert_nvs_blob(true, "d1l_messages", "public",
                    dedicated_blob, sizeof(dedicated_blob));

    /* A completion sentinel beside unreadable retained NVS proves this is not
     * a predecessor app tail, so neither partition may be erased. */
    clear_nvs_case();
    seed_nvs_blob(true, "d1l_messages", "public",
                  dedicated_blob, sizeof(dedicated_blob));
    seed_valid_dedicated_anchor();
    seed_valid_default_sentinel();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    s_partition_init_result = ESP_FAIL;
    assert(d1l_retained_blob_store_init() == ESP_FAIL);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 0U);
    assert_nvs_blob(true, "d1l_messages", "public",
                    dedicated_blob, sizeof(dedicated_blob));
}

static void test_nonblank_unowned_region_requires_external_initialization(void)
{
    static const char legacy_blob[] = "predecessor retained history";

    clear_nvs_case();
    memset(s_retained_meta, 0xa5, sizeof(s_retained_meta));
    s_retained_partition_erased = false;
    seed_nvs_blob(false, "d1l_messages", "public",
                  legacy_blob, sizeof(legacy_blob));

    assert(d1l_retained_blob_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_retained_blob_store_nvs_ready());
    assert(d1l_retained_blob_store_nvs_external_init_required());
    assert(!d1l_retained_blob_store_nvs_marker_ready());
    assert(!d1l_retained_blob_store_nvs_anchor_ready());
    assert(!d1l_retained_blob_store_nvs_sentinel_ready());
    assert(count_nvs_event(TEST_NVS_EVENT_INIT_PARTITION) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_WRITE_META) == 0U);
    assert_nvs_blob(false, "d1l_messages", "public",
                    legacy_blob, sizeof(legacy_blob));

    /* The installer has separately verified the predecessor layout and owns
     * this one scoped erase; firmware sees only a blank first-use region. */
    assert(esp_partition_erase_range(&s_retained_partition, 0U,
                                     s_retained_partition.size) == ESP_OK);
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(!d1l_retained_blob_store_nvs_external_init_required());
    assert(d1l_retained_blob_store_nvs_markers_complete());
    assert(d1l_retained_blob_store_nvs_anchor_ready());
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    assert(d1l_retained_blob_store_nvs_initialized_this_boot());
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_WRITE_META) == 2U);
    assert_nvs_blob(true, "d1l_messages", "public",
                    legacy_blob, sizeof(legacy_blob));
    assert(find_nvs_entry(false, "d1l_messages", "public") == NULL);
    assert(find_nvs_entry(false, "d1l_ret_meta", "initialized") != NULL);
}

static void test_anchor_only_claim_loss_fails_closed(void)
{
    static const char orphaned_blob[] = "anchor-only multi-fault state";

    clear_nvs_case();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    seed_valid_dedicated_anchor();
    seed_nvs_blob(true, "d1l_messages", "public",
                  orphaned_blob, sizeof(orphaned_blob));

    assert(d1l_retained_blob_store_init() == ESP_ERR_INVALID_STATE);
    assert(d1l_retained_blob_store_nvs_external_init_required());
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_INIT_PARTITION) == 0U);
    assert_nvs_blob(true, "d1l_messages", "public",
                    orphaned_blob, sizeof(orphaned_blob));

    /* A default sentinel owns the region but cannot recreate a missing
     * dedicated anchor; recovery stays fail-closed without an explicit erase. */
    clear_nvs_case();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    seed_valid_default_sentinel();
    seed_nvs_blob(true, "d1l_messages", "public",
                  orphaned_blob, sizeof(orphaned_blob));

    assert(d1l_retained_blob_store_init() == ESP_ERR_INVALID_STATE);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 0U);
    assert_nvs_blob(true, "d1l_messages", "public",
                    orphaned_blob, sizeof(orphaned_blob));
}

static void test_completion_sentinel_blocks_blank_established_partition(void)
{
    clear_nvs_case();
    s_retained_partition_erased = true;

    assert(d1l_retained_blob_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_retained_blob_store_nvs_ready());
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 0U);

    clear_nvs_case();
    seed_valid_default_sentinel();
    s_retained_partition_erased = true;

    assert(d1l_retained_blob_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_retained_blob_store_nvs_ready());
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 0U);

    /* A last-slot marker cannot be the marker-first initialization window. */
    clear_nvs_case();
    const test_retained_marker_t marker = valid_retained_marker();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    memcpy(s_retained_meta + sizeof(s_retained_meta) - sizeof(marker),
           &marker, sizeof(marker));
    s_retained_partition_erased = true;

    assert(d1l_retained_blob_store_init() == ESP_ERR_INVALID_STATE);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_WRITE_META) == 0U);
}

static void test_anchor_commit_failure_is_retryable_and_fail_closed(void)
{
    clear_nvs_case();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    s_retained_partition_erased = true;
    s_anchor_commit_result = ESP_FAIL;

    assert(d1l_retained_blob_store_init() == ESP_FAIL);
    assert(d1l_retained_blob_store_nvs_marker_ready());
    assert(!d1l_retained_blob_store_nvs_anchor_ready());
    assert(!d1l_retained_blob_store_nvs_sentinel_ready());
    assert(!d1l_retained_blob_store_nvs_ready());
    assert(count_nvs_event(TEST_NVS_EVENT_WRITE_META) == 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);

    s_anchor_commit_result = ESP_OK;
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_marker_ready());
    assert(d1l_retained_blob_store_nvs_anchor_ready());
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    assert(d1l_retained_blob_store_nvs_ready());
}

static void test_marker_flash_failures_are_retryable_and_ordered(void)
{
    /* A partial first-marker write cannot initialize or mutate retained NVS. */
    clear_nvs_case();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    s_retained_partition_erased = true;
    s_meta_write_fail_on_attempt = 1U;

    assert(d1l_retained_blob_store_init() == ESP_FAIL);
    assert(!d1l_retained_blob_store_nvs_marker_ready());
    assert(!d1l_retained_blob_store_nvs_anchor_ready());
    assert(count_nvs_event(TEST_NVS_EVENT_INIT_PARTITION) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_RETAINED) == 0U);

    s_meta_write_fail_on_attempt = 0U;
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_markers_complete());

    /* A partial second marker leaves marker 1 plus the committed anchor. The
     * retry commits the default sentinel before erasing/rebuilding metadata. */
    clear_nvs_case();
    memset(s_retained_meta, 0xff, sizeof(s_retained_meta));
    s_retained_partition_erased = true;
    s_meta_write_fail_on_attempt = 2U;

    assert(d1l_retained_blob_store_init() == ESP_FAIL);
    assert(d1l_retained_blob_store_nvs_marker_ready());
    assert(!d1l_retained_blob_store_nvs_markers_complete());
    assert(d1l_retained_blob_store_nvs_anchor_ready());
    assert(!d1l_retained_blob_store_nvs_sentinel_ready());

    s_meta_write_fail_on_attempt = 0U;
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_markers_complete());
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    assert(first_nvs_event(TEST_NVS_EVENT_COMMIT_LEGACY) <
           first_nvs_event(TEST_NVS_EVENT_ERASE_META));

    /* A partial metadata-sector erase fails before marker 1 or NVS init. */
    clear_nvs_case();
    memset(s_retained_meta, 0xa5, sizeof(s_retained_meta));
    s_retained_partition_erased = true;
    s_meta_erase_result = ESP_FAIL;
    s_meta_erase_partial = true;

    assert(d1l_retained_blob_store_init() == ESP_FAIL);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_META) == 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_WRITE_META) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_INIT_PARTITION) == 0U);

    s_meta_erase_result = ESP_OK;
    s_meta_erase_partial = false;
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_markers_complete());
}

static void test_sentinel_commit_failure_is_retryable_and_fail_closed(void)
{
    clear_nvs_case();
    seed_valid_dedicated_anchor();
    s_sentinel_commit_result = ESP_FAIL;

    assert(d1l_retained_blob_store_init() == ESP_FAIL);
    assert(d1l_retained_blob_store_nvs_marker_ready());
    assert(d1l_retained_blob_store_nvs_anchor_ready());
    assert(!d1l_retained_blob_store_nvs_sentinel_ready());
    assert(!d1l_retained_blob_store_nvs_ready());
    assert(d1l_retained_blob_store_nvs_migration_error() == ESP_FAIL);

    s_sentinel_commit_result = ESP_OK;
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_marker_ready());
    assert(d1l_retained_blob_store_nvs_anchor_ready());
    assert(d1l_retained_blob_store_nvs_sentinel_ready());
    assert(d1l_retained_blob_store_nvs_ready());
}

static void test_partition_init_proactively_migrates_known_legacy_key(void)
{
    static const char legacy_blob[] = "boot-time public history";

    clear_nvs_case();
    seed_valid_dedicated_anchor();
    seed_nvs_blob(false, "d1l_messages", "public",
                  legacy_blob, sizeof(legacy_blob));
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_ready());
    assert(d1l_retained_blob_store_nvs_error() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_migration_error() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_migrated_keys() == 1U);
    assert_nvs_blob(true, "d1l_messages", "public",
                    legacy_blob, sizeof(legacy_blob));
    assert(find_nvs_entry(false, "d1l_messages", "public") == NULL);
    assert(first_nvs_event(TEST_NVS_EVENT_COMMIT_DEDICATED) <
           first_nvs_event(TEST_NVS_EVENT_ERASE_LEGACY));
}

static void test_divergent_upgrade_copies_fail_closed(void)
{
    static const char dedicated_blob[] = "candidate retained value";
    static const char legacy_blob[] = "newer downgrade value";
    char readback[64] = {0};
    size_t readback_len = sizeof(readback);

    clear_nvs_case();
    seed_valid_dedicated_anchor();
    d1l_retained_blob_store_note_sd_backend(false, false, false,
                                             0U, 0U, 0U);
    seed_nvs_blob(true, "d1l_messages", "public",
                  dedicated_blob, sizeof(dedicated_blob));
    seed_nvs_blob(false, "d1l_messages", "public",
                  legacy_blob, sizeof(legacy_blob));

    assert(d1l_retained_blob_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_retained_blob_store_nvs_ready());
    assert(d1l_retained_blob_store_nvs_error() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_migration_error() ==
           ESP_ERR_INVALID_STATE);
    assert(strcmp(d1l_retained_blob_store_backend_name(
                      D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES),
                  "unavailable") == 0);
    assert(!d1l_retained_blob_store_is_available(
        D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES));
    assert_nvs_blob(true, "d1l_messages", "public",
                    dedicated_blob, sizeof(dedicated_blob));
    assert_nvs_blob(false, "d1l_messages", "public",
                    legacy_blob, sizeof(legacy_blob));
    assert(d1l_retained_blob_store_read_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
               readback, &readback_len) == ESP_ERR_INVALID_STATE);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_LEGACY) == 0U);

    clear_nvs_case();
    seed_valid_dedicated_anchor();
    seed_nvs_blob(true, "d1l_messages", "public",
                  dedicated_blob, sizeof(dedicated_blob));
    seed_nvs_blob(false, "d1l_messages", "public",
                  dedicated_blob, sizeof(dedicated_blob));
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_ready());
    assert(strcmp(d1l_retained_blob_store_backend_name(
                      D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES),
                  "nvs") == 0);
    assert(d1l_retained_blob_store_is_available(
        D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES));
    assert(find_nvs_entry(false, "d1l_messages", "public") == NULL);
}

static void test_dedicated_read_precedes_legacy(void)
{
    static const char dedicated_blob[] = "dedicated routes";
    static const char legacy_blob[] = "stale legacy routes";
    char readback[64] = {0};
    size_t readback_len = sizeof(readback);

    clear_nvs_case();
    seed_valid_dedicated_anchor();
    seed_nvs_blob(true, "d1l_routes", "routes_v2",
                  dedicated_blob, sizeof(dedicated_blob));
    seed_nvs_blob(false, "d1l_routes", "routes_v2",
                  legacy_blob, sizeof(legacy_blob));

    assert(d1l_retained_blob_store_read_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_ROUTES, "routes_v2",
               readback, &readback_len) == ESP_OK);
    assert(readback_len == sizeof(dedicated_blob));
    assert(memcmp(readback, dedicated_blob, sizeof(dedicated_blob)) == 0);
    assert(first_nvs_event(TEST_NVS_EVENT_OPEN_DEDICATED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_GET_DEDICATED) == 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_OPEN_LEGACY) == 0U);
    assert_nvs_blob(false, "d1l_routes", "routes_v2",
                    legacy_blob, sizeof(legacy_blob));
}

static void test_legacy_read_migrates_after_dedicated_miss(void)
{
    static const char legacy_blob[] = "legacy packet ring";
    char readback[64] = {0};
    size_t readback_len = sizeof(readback);

    clear_nvs_case();
    seed_nvs_blob(false, "d1l_packets", "ring",
                  legacy_blob, sizeof(legacy_blob));

    assert(d1l_retained_blob_store_read_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_PACKET_LOG, "ring",
               readback, &readback_len) == ESP_OK);
    assert(readback_len == sizeof(legacy_blob));
    assert(memcmp(readback, legacy_blob, sizeof(legacy_blob)) == 0);
    assert_nvs_blob(true, "d1l_packets", "ring",
                    legacy_blob, sizeof(legacy_blob));
    assert(find_nvs_entry(false, "d1l_packets", "ring") == NULL);

    const size_t dedicated_open = first_nvs_event(TEST_NVS_EVENT_OPEN_DEDICATED);
    const size_t legacy_open = first_nvs_event(TEST_NVS_EVENT_OPEN_LEGACY);
    const size_t legacy_get = first_nvs_event(TEST_NVS_EVENT_GET_LEGACY);
    const size_t dedicated_set = first_nvs_event(TEST_NVS_EVENT_SET_DEDICATED);
    const size_t dedicated_commit =
        first_nvs_event(TEST_NVS_EVENT_COMMIT_DEDICATED);
    const size_t legacy_erase = first_nvs_event(TEST_NVS_EVENT_ERASE_LEGACY);
    assert(dedicated_open < legacy_open);
    assert(legacy_open <= legacy_get);
    assert(legacy_get < dedicated_set);
    assert(dedicated_set < dedicated_commit);
    assert(dedicated_commit < legacy_erase);
    assert(count_nvs_event(TEST_NVS_EVENT_SET_LEGACY) == 0U);
}

static void test_dedicated_write_reclaims_legacy_only_after_commit(void)
{
    static const char legacy_blob[] = "old dms";
    static const char new_blob[] = "new dms";

    clear_nvs_case();
    seed_nvs_blob(false, "d1l_dms", "messages_v2",
                  legacy_blob, sizeof(legacy_blob));

    assert(d1l_retained_blob_store_write_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_DM_MESSAGES, "messages_v2",
               new_blob, sizeof(new_blob)) == ESP_OK);
    assert_nvs_blob(true, "d1l_dms", "messages_v2",
                    new_blob, sizeof(new_blob));
    assert(find_nvs_entry(false, "d1l_dms", "messages_v2") == NULL);
    assert(first_nvs_event(TEST_NVS_EVENT_OPEN_DEDICATED) == 0U);
    assert(first_nvs_event(TEST_NVS_EVENT_COMMIT_DEDICATED) <
           first_nvs_event(TEST_NVS_EVENT_ERASE_LEGACY));
    assert(count_nvs_event(TEST_NVS_EVENT_SET_LEGACY) == 0U);

    /* A failed dedicated commit must leave the only committed legacy copy
     * intact and must not even attempt the destructive legacy erase. */
    clear_nvs_case();
    seed_nvs_blob(false, "d1l_dms", "messages_v2",
                  legacy_blob, sizeof(legacy_blob));
    s_dedicated_commit_result = ESP_FAIL;
    assert(d1l_retained_blob_store_write_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_DM_MESSAGES, "messages_v2",
               new_blob, sizeof(new_blob)) == ESP_FAIL);
    assert_nvs_blob(false, "d1l_dms", "messages_v2",
                    legacy_blob, sizeof(legacy_blob));
    assert(find_nvs_entry(true, "d1l_dms", "messages_v2") == NULL);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_LEGACY) == 0U);
}

static void test_erase_clears_dedicated_and_legacy_copies(void)
{
    static const char dedicated_blob[] = "new public";
    static const char legacy_blob[] = "old public";

    clear_nvs_case();
    seed_nvs_blob(true, "d1l_messages", "messages_v2",
                  dedicated_blob, sizeof(dedicated_blob));
    seed_nvs_blob(false, "d1l_messages", "messages_v2",
                  legacy_blob, sizeof(legacy_blob));
    assert(d1l_retained_blob_store_erase_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES,
               "messages_v2") == ESP_OK);
    assert(find_nvs_entry(true, "d1l_messages", "messages_v2") == NULL);
    assert(find_nvs_entry(false, "d1l_messages", "messages_v2") == NULL);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_DEDICATED) == 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_LEGACY) == 1U);
    assert(first_nvs_event(TEST_NVS_EVENT_ERASE_LEGACY) <
           first_nvs_event(TEST_NVS_EVENT_ERASE_DEDICATED));

    /* A missing dedicated copy is not a reason to strand a legacy blob. */
    clear_nvs_case();
    seed_nvs_blob(false, "d1l_messages", "messages_v2",
                  legacy_blob, sizeof(legacy_blob));
    assert(d1l_retained_blob_store_erase_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES,
               "messages_v2") == ESP_OK);
    assert(find_nvs_entry(false, "d1l_messages", "messages_v2") == NULL);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_DEDICATED) == 0U);
    assert(count_nvs_event(TEST_NVS_EVENT_ERASE_LEGACY) == 1U);
}

static void test_nvs_capacity_and_write_amplification_telemetry(void)
{
    static const char public_blob[] = "telemetry public";
    static const char dm_blob[] = "telemetry dm";
    d1l_retained_blob_store_nvs_telemetry_t telemetry = {0};

    clear_nvs_case();
    seed_valid_dedicated_anchor();
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_nvs_telemetry(&telemetry));
    assert(telemetry.capacity_valid);
    assert(telemetry.capacity_error == ESP_OK);
    assert(telemetry.total_entries == TEST_NVS_TOTAL_ENTRIES);
    assert(telemetry.used_entries == 1U);
    assert(telemetry.namespace_count == 1U);
    assert(telemetry.write_attempt_count == 0U);

    assert(d1l_retained_blob_store_write_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
               public_blob, sizeof(public_blob)) == ESP_OK);
    s_dedicated_commit_result = ESP_FAIL;
    assert(d1l_retained_blob_store_write_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_DM_MESSAGES, "messages_v2",
               dm_blob, sizeof(dm_blob)) == ESP_FAIL);
    s_dedicated_commit_result = ESP_OK;
    assert(d1l_retained_blob_store_erase_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public") == ESP_OK);

    memset(&telemetry, 0, sizeof(telemetry));
    assert(d1l_retained_blob_store_nvs_telemetry(&telemetry));
    assert(telemetry.capacity_valid);
    assert(telemetry.used_entries == 1U);
    assert(telemetry.free_entries == TEST_NVS_TOTAL_ENTRIES - 1U);
    assert(telemetry.available_entries == TEST_NVS_TOTAL_ENTRIES - 5U);
    assert(telemetry.available_entries != telemetry.free_entries);
    assert(telemetry.write_attempt_count == 2U);
    assert(telemetry.write_commit_count == 1U);
    assert(telemetry.write_fail_count == 1U);
    assert(telemetry.write_bytes_attempted ==
           sizeof(public_blob) + sizeof(dm_blob));
    assert(telemetry.write_bytes_committed == sizeof(public_blob));
    assert(telemetry.erase_attempt_count == 1U);
    assert(telemetry.erase_commit_count == 1U);
    assert(telemetry.erase_fail_count == 0U);
    assert(telemetry.last_error == ESP_FAIL);

    const d1l_retained_blob_store_nvs_store_telemetry_t *messages =
        &telemetry.stores[D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES];
    assert(messages->write_attempt_count == 1U);
    assert(messages->write_commit_count == 1U);
    assert(messages->write_fail_count == 0U);
    assert(messages->write_bytes_committed == sizeof(public_blob));
    assert(messages->erase_attempt_count == 1U);
    assert(messages->erase_commit_count == 1U);

    const d1l_retained_blob_store_nvs_store_telemetry_t *dms =
        &telemetry.stores[D1L_RETAINED_BLOB_STORE_DM_MESSAGES];
    assert(dms->write_attempt_count == 1U);
    assert(dms->write_commit_count == 0U);
    assert(dms->write_fail_count == 1U);
    assert(dms->write_bytes_committed == 0U);
    assert(dms->last_error == ESP_FAIL);

    s_nvs_stats_result = ESP_ERR_INVALID_STATE;
    memset(&telemetry, 0, sizeof(telemetry));
    assert(d1l_retained_blob_store_nvs_telemetry(&telemetry));
    assert(!telemetry.capacity_valid);
    assert(telemetry.capacity_error == ESP_ERR_INVALID_STATE);
    assert(telemetry.used_entries == 0U);
    assert(telemetry.total_entries == 0U);
    assert(telemetry.write_attempt_count == 2U);
    assert(telemetry.write_commit_count == 1U);
    s_nvs_stats_result = ESP_OK;
}

static void test_unchanged_nvs_fallback_write_skips_flash_commit(void)
{
    static const char original[] = "same retained snapshot";
    static const char replacement[] = "new! retained snapshot";
    static const uint32_t duplicate_requests = 64U;
    d1l_retained_blob_store_nvs_telemetry_t telemetry = {0};

    _Static_assert(sizeof(original) == sizeof(replacement),
                   "same-length replacement must exercise byte comparison");

    clear_nvs_case();
    seed_valid_dedicated_anchor();
    assert(d1l_retained_blob_store_init() == ESP_OK);

    assert(d1l_retained_blob_store_write_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
               original, sizeof(original)) == ESP_OK);
    const size_t sets_after_first =
        count_nvs_event(TEST_NVS_EVENT_SET_DEDICATED);
    const size_t commits_after_first =
        count_nvs_event(TEST_NVS_EVENT_COMMIT_DEDICATED);

    for (uint32_t i = 0U; i < duplicate_requests; ++i) {
        assert(d1l_retained_blob_store_write_nvs_fallback(
                   D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
                   original, sizeof(original)) == ESP_OK);
    }
    assert(count_nvs_event(TEST_NVS_EVENT_SET_DEDICATED) ==
           sets_after_first);
    assert(count_nvs_event(TEST_NVS_EVENT_COMMIT_DEDICATED) ==
           commits_after_first);

    assert(d1l_retained_blob_store_nvs_telemetry(&telemetry));
    assert(telemetry.write_attempt_count == 1U + duplicate_requests);
    assert(telemetry.write_commit_count == 1U);
    assert(telemetry.write_fail_count == 0U);
    assert(telemetry.write_bytes_attempted ==
           sizeof(original) * (1U + duplicate_requests));
    assert(telemetry.write_bytes_committed == sizeof(original));

    assert(d1l_retained_blob_store_write_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
               replacement, sizeof(replacement)) == ESP_OK);
    assert(count_nvs_event(TEST_NVS_EVENT_SET_DEDICATED) ==
           sets_after_first + 1U);
    assert(count_nvs_event(TEST_NVS_EVENT_COMMIT_DEDICATED) ==
           commits_after_first + 1U);

    memset(&telemetry, 0, sizeof(telemetry));
    assert(d1l_retained_blob_store_nvs_telemetry(&telemetry));
    assert(telemetry.write_attempt_count == 2U + duplicate_requests);
    assert(telemetry.write_commit_count == 2U);
    assert(telemetry.write_fail_count == 0U);
    assert(telemetry.write_bytes_attempted ==
           sizeof(original) * (2U + duplicate_requests));
    assert(telemetry.write_bytes_committed == sizeof(original) * 2U);
}

static void test_nvs_unchanged_compare_failure_fails_safe_to_write(void)
{
    static const char retained[] = "retained snapshot";
    d1l_retained_blob_store_nvs_telemetry_t telemetry = {0};

    clear_nvs_case();
    seed_valid_dedicated_anchor();
    assert(d1l_retained_blob_store_init() == ESP_OK);
    assert(d1l_retained_blob_store_write_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
               retained, sizeof(retained)) == ESP_OK);

    size_t expected_sets = count_nvs_event(TEST_NVS_EVENT_SET_DEDICATED);
    size_t expected_commits =
        count_nvs_event(TEST_NVS_EVENT_COMMIT_DEDICATED);

    s_nvs_get_fail_on_attempt = s_nvs_get_attempt + 1U;
    assert(d1l_retained_blob_store_write_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
               retained, sizeof(retained)) == ESP_OK);
    expected_sets++;
    expected_commits++;
    assert(count_nvs_event(TEST_NVS_EVENT_SET_DEDICATED) == expected_sets);
    assert(count_nvs_event(TEST_NVS_EVENT_COMMIT_DEDICATED) ==
           expected_commits);

    s_nvs_get_fail_on_attempt = s_nvs_get_attempt + 2U;
    assert(d1l_retained_blob_store_write_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
               retained, sizeof(retained)) == ESP_OK);
    expected_sets++;
    expected_commits++;
    assert(count_nvs_event(TEST_NVS_EVENT_SET_DEDICATED) == expected_sets);
    assert(count_nvs_event(TEST_NVS_EVENT_COMMIT_DEDICATED) ==
           expected_commits);

    assert(d1l_retained_blob_store_nvs_telemetry(&telemetry));
    assert(telemetry.write_attempt_count == 3U);
    assert(telemetry.write_commit_count == 3U);
    assert(telemetry.write_fail_count == 0U);
    assert(telemetry.write_bytes_attempted == sizeof(retained) * 3U);
    assert(telemetry.write_bytes_committed == sizeof(retained) * 3U);
}

static void test_partition_init_failure_never_falls_back_or_resurrects(void)
{
    static const char dedicated_blob[] = "dedicated current";
    static const char legacy_blob[] = "legacy stale";
    static const char replacement[] = "must not write";
    char readback[64] = {0};
    size_t readback_len = sizeof(readback);

    clear_nvs_case();
    seed_valid_dedicated_anchor();
    seed_nvs_blob(true, "d1l_routes", "routes_v2",
                  dedicated_blob, sizeof(dedicated_blob));
    seed_nvs_blob(false, "d1l_routes", "routes_v2",
                  legacy_blob, sizeof(legacy_blob));
    s_partition_init_result = ESP_FAIL;
    assert(d1l_retained_blob_store_init() == ESP_FAIL);
    assert(!d1l_retained_blob_store_nvs_ready());
    assert(d1l_retained_blob_store_nvs_error() == ESP_FAIL);
    assert(d1l_retained_blob_store_read_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_ROUTES, "routes_v2",
               readback, &readback_len) == ESP_FAIL);
    assert(d1l_retained_blob_store_write_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_ROUTES, "routes_v2",
               replacement, sizeof(replacement)) == ESP_FAIL);
    assert(d1l_retained_blob_store_erase_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_ROUTES, "routes_v2") == ESP_FAIL);
    assert_nvs_blob(true, "d1l_routes", "routes_v2",
                    dedicated_blob, sizeof(dedicated_blob));
    assert_nvs_blob(false, "d1l_routes", "routes_v2",
                    legacy_blob, sizeof(legacy_blob));

    s_partition_init_result = ESP_OK;
    seed_nvs_blob(false, "d1l_routes", "routes_v2",
                  dedicated_blob, sizeof(dedicated_blob));
    assert(d1l_retained_blob_store_init() == ESP_OK);
    readback_len = sizeof(readback);
    assert(d1l_retained_blob_store_read_nvs_fallback(
               D1L_RETAINED_BLOB_STORE_ROUTES, "routes_v2",
               readback, &readback_len) == ESP_OK);
    assert(readback_len == sizeof(dedicated_blob));
    assert(memcmp(readback, dedicated_blob, sizeof(dedicated_blob)) == 0);
}

static void test_background_read_yields_between_chunks_without_degrading_sd(void)
{
    uint8_t readback[D1L_RP2040_FILE_CHUNK_MAX * 2U] = {0};
    size_t readback_len = sizeof(readback);
    d1l_retained_blob_store_sd_stats_t before = {0};
    d1l_retained_blob_store_sd_stats_t after = {0};
    assert(d1l_retained_blob_store_sd_stats(
        D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, &before));

    s_chunked_read_case = true;
    s_worker_should_yield = false;
    s_chunked_read_count = 0U;
    assert(d1l_retained_blob_store_read_sd_primary(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES,
               "public", readback, &readback_len) == ESP_ERR_NOT_FINISHED);
    assert(s_chunked_read_count == 1U);
    assert(d1l_retained_blob_store_sd_stats(
        D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, &after));
    assert(after.sd_read_fail_count == before.sd_read_fail_count);
    assert(after.sd_last_error == before.sd_last_error);
    assert(after.sd_degraded_latched == before.sd_degraded_latched);

    s_worker_should_yield = false;
    s_chunked_read_case = false;
}

static void test_background_write_yields_before_rename_without_degrading_sd(void)
{
    uint8_t payload[D1L_RP2040_FILE_CHUNK_MAX * 2U] = {0};
    d1l_retained_blob_store_backend_state_t state = {0};
    d1l_retained_blob_store_sd_stats_t before = {0};
    d1l_retained_blob_store_sd_stats_t after = {0};
    assert(d1l_retained_blob_store_backend_state(
        D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, &state));
    assert(d1l_retained_blob_store_sd_stats(
        D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, &before));
    const uint32_t rename_count_before = s_rename_count;
    const uint32_t delete_count_before = s_delete_count;

    s_chunked_write_yield_case = true;
    s_worker_should_yield = false;
    s_chunked_write_count = 0U;
    assert(d1l_retained_blob_store_write_sd_primary_guarded(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES,
               "public", payload, sizeof(payload), state.generation) ==
           ESP_ERR_NOT_FINISHED);
    assert(s_chunked_write_count == 1U);
    assert(s_rename_count == rename_count_before);
    assert(s_delete_count == delete_count_before);
    assert(d1l_retained_blob_store_sd_stats(
        D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, &after));
    assert(after.sd_write_fail_count == before.sd_write_fail_count);
    assert(after.sd_rename_fail_count == before.sd_rename_fail_count);
    assert(after.sd_last_error == before.sd_last_error);
    assert(after.sd_degraded_latched == before.sd_degraded_latched);

    s_chunked_write_yield_case = false;
    s_worker_should_yield = false;
    assert(d1l_retained_blob_store_write_sd_primary_guarded(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES,
               "public", payload, sizeof(payload), state.generation) ==
           ESP_OK);
    assert(s_rename_count == rename_count_before + 1U);
    assert(s_delete_count == delete_count_before);
    s_rename_count = rename_count_before;
    s_delete_count = delete_count_before;
}

static uint32_t assert_reset_lineage(
    d1l_factory_reset_sd_store_t store, bool expected_active)
{
    bool active = !expected_active;
    uint32_t generation = 0U;
    assert(d1l_factory_reset_sd_lineage_snapshot(
               store, &active, &generation) == ESP_OK);
    assert(active == expected_active);
    assert(generation != 0U);
    return generation;
}

static void assert_exact_marker(const char *path,
                                d1l_factory_reset_sd_store_t store,
                                uint32_t generation)
{
    test_sd_file_t *marker = find_sd_file(path);
    assert(marker);
    assert(d1l_factory_reset_sd_media_marker_matches(
        (const d1l_factory_reset_sd_media_marker_t *)marker->data,
        marker->length, store, generation));
}

static void test_factory_reset_sd_recovery_end_to_end(void)
{
    static const uint8_t stale[] = "pre-reset-owned-data";
    static const uint8_t replacement[] = "post-reset-data";
    static const uint8_t unrelated[] = "operator-owned-unrelated-data";
    static const char public_marker_path[] =
        "stores/messages/public/reset_lineage_v1.bin";
    static const char public_marker_temp[] =
        "stores/messages/public/reset_lineage_v1.tmp";
    static const char dm_marker_path[] =
        "stores/messages/dm/reset_lineage_v1.bin";
    static const char route_marker_path[] =
        "stores/routes/reset_lineage_v1.bin";
    static const char packet_marker_path[] =
        "stores/packet_log/reset_lineage_v1.bin";

    clear_nvs_case();
    seed_valid_default_sentinel();
    seed_valid_dedicated_anchor();
    assert(d1l_retained_blob_store_init() == ESP_OK);
    reset_sd_files();
    s_worker_should_yield = false;

    /* The production coordinator creates one exact durable generation for
     * every removable store while no card access is performed. */
    d1l_factory_reset_status_t reset_status = {0};
    assert(d1l_factory_reset_request(&reset_status) == ESP_OK);
    assert(d1l_factory_reset_resume(&reset_status) == ESP_OK);
    assert(reset_status.phase == D1L_FACTORY_RESET_PHASE_COMPLETE);
    const uint32_t reset_generation = reset_status.sd_lineage_generation;
    assert(reset_generation != 0U);
    assert(d1l_factory_reset_resume(&reset_status) == ESP_OK);
    for (uint32_t store = 0U; store < D1L_FACTORY_RESET_SD_STORE_COUNT;
         ++store) {
        assert(assert_reset_lineage(
                   (d1l_factory_reset_sd_store_t)store, true) ==
               reset_generation);
    }

    /* Start on a freshly observed card. A missing marker and then a marker
     * for the wrong generation are both non-ready while the onboard fence is
     * active. */
    d1l_retained_blob_store_note_sd_backend(false, false, false,
                                            0U, 0U, 0U);
    d1l_retained_blob_store_note_sd_backend(
        true, true, true, D1L_RP2040_FILE_LINE_MAX,
        D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX);
    d1l_retained_blob_store_backend_state_t public_backend = {0};
    assert(d1l_retained_blob_store_backend_state(
        D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, &public_backend));
    assert(public_backend.enabled);
    bool ready = true;
    assert(d1l_retained_blob_store_sd_media_lineage_ready(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES,
               public_backend.generation, &ready) == ESP_OK);
    assert(!ready);

    d1l_factory_reset_sd_media_marker_t wrong_marker = {0};
    d1l_factory_reset_sd_media_marker_init(
        &wrong_marker, D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES,
        reset_generation + 1U);
    seed_sd_file(public_marker_path, &wrong_marker, sizeof(wrong_marker));
    assert(d1l_retained_blob_store_sd_media_lineage_ready(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES,
               public_backend.generation, &ready) == ESP_OK);
    assert(!ready);

    /* Recovery purges only exact firmware-owned paths. It preserves nearby
     * unknown files, installs the replacement and exact marker atomically,
     * reads the marker back, and only then clears the onboard fence. */
    seed_sd_file("stores/messages/public/public.bin", stale, sizeof(stale));
    seed_sd_file("stores/messages/public/public.tmp", stale, sizeof(stale));
    seed_sd_file("stores/messages/public/operator-notes.bin",
                 unrelated, sizeof(unrelated));
    reset_sd_events();
    assert(d1l_retained_blob_store_write_sd_primary_guarded(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES, "public",
               replacement, sizeof(replacement), public_backend.generation) ==
           ESP_OK);
    assert(find_sd_file("stores/messages/public/public.bin"));
    assert(!find_sd_file("stores/messages/public/public.tmp"));
    assert(find_sd_file("stores/messages/public/operator-notes.bin"));
    assert_exact_marker(public_marker_path,
                        D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES,
                        reset_generation);
    assert(assert_reset_lineage(
               D1L_FACTORY_RESET_SD_STORE_PUBLIC_MESSAGES, false) ==
           reset_generation);
    assert(d1l_retained_blob_store_sd_media_lineage_ready(
               D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES,
               public_backend.generation, &ready) == ESP_OK);
    assert(ready);

    const size_t marker_write = first_sd_event(
        TEST_SD_EVENT_WRITE, public_marker_temp);
    const size_t marker_rename = first_sd_event(
        TEST_SD_EVENT_RENAME, public_marker_path);
    const size_t marker_readback = first_sd_event_after(
        TEST_SD_EVENT_READ, public_marker_path, marker_rename);
    const size_t fence_clear = first_sd_event(
        TEST_SD_EVENT_LINEAGE_COMMIT, "sd_public_v1");
    assert(marker_write < marker_rename);
    assert(marker_rename < marker_readback);
    assert(marker_readback < fence_clear);

    /* A card-generation edge inside purge aborts recovery. The replacement
     * card is not mistaken for the old card and the onboard fence remains
     * active until a full retry on the new generation succeeds. */
    seed_sd_file("stores/messages/dm/threads.bin", stale, sizeof(stale));
    seed_sd_file("stores/messages/dm/threads.tmp", stale, sizeof(stale));
    d1l_retained_blob_store_backend_state_t dm_backend = {0};
    assert(d1l_retained_blob_store_backend_state(
        D1L_RETAINED_BLOB_STORE_DM_MESSAGES, &dm_backend));
    s_replace_card_during_delete = true;
    assert(d1l_retained_blob_store_write_sd_primary_guarded(
               D1L_RETAINED_BLOB_STORE_DM_MESSAGES, "threads",
               replacement, sizeof(replacement), dm_backend.generation) ==
           ESP_ERR_INVALID_STATE);
    assert(find_sd_file("replacement/keep.bin"));
    assert(assert_reset_lineage(
               D1L_FACTORY_RESET_SD_STORE_DM_MESSAGES, true) ==
           reset_generation);
    assert(d1l_retained_blob_store_backend_state(
        D1L_RETAINED_BLOB_STORE_DM_MESSAGES, &dm_backend));
    assert(d1l_retained_blob_store_write_sd_primary_guarded(
               D1L_RETAINED_BLOB_STORE_DM_MESSAGES, "threads",
               replacement, sizeof(replacement), dm_backend.generation) ==
           ESP_OK);
    assert(find_sd_file("replacement/keep.bin"));
    assert_exact_marker(dm_marker_path,
                        D1L_FACTORY_RESET_SD_STORE_DM_MESSAGES,
                        reset_generation);
    assert(assert_reset_lineage(
               D1L_FACTORY_RESET_SD_STORE_DM_MESSAGES, false) ==
           reset_generation);

    /* Route recovery owns both the current and legacy alias, but nothing
     * else in the directory. Exercise both exact paths through the same
     * production purge/finalize coordinator. */
    seed_sd_file("stores/routes/routes_v2.bin", stale, sizeof(stale));
    seed_sd_file("stores/routes/routes_v2.tmp", stale, sizeof(stale));
    seed_sd_file("stores/routes/routes.bin", stale, sizeof(stale));
    seed_sd_file("stores/routes/routes.tmp", stale, sizeof(stale));
    seed_sd_file("stores/routes/operator-route.bin",
                 unrelated, sizeof(unrelated));
    d1l_retained_blob_store_backend_state_t route_backend = {0};
    assert(d1l_retained_blob_store_backend_state(
        D1L_RETAINED_BLOB_STORE_ROUTES, &route_backend));
    assert(d1l_retained_blob_store_write_sd_primary_guarded(
               D1L_RETAINED_BLOB_STORE_ROUTES, "routes_v2",
               replacement, sizeof(replacement), route_backend.generation) ==
           ESP_OK);
    assert(find_sd_file("stores/routes/routes_v2.bin"));
    assert(!find_sd_file("stores/routes/routes_v2.tmp"));
    assert(!find_sd_file("stores/routes/routes.bin"));
    assert(!find_sd_file("stores/routes/routes.tmp"));
    assert(find_sd_file("stores/routes/operator-route.bin"));
    assert_exact_marker(route_marker_path,
                        D1L_FACTORY_RESET_SD_STORE_ROUTES,
                        reset_generation);
    assert(assert_reset_lineage(
               D1L_FACTORY_RESET_SD_STORE_ROUTES, false) ==
           reset_generation);

    /* Packet recovery runs the real packet coordinator. It removes every
     * one of the 64 deterministic history segments before the retained-store
     * packet completion helper installs and verifies the marker. */
    for (uint32_t segment = 0U;
         segment < D1L_PACKET_LOG_SD_SEGMENT_COUNT; ++segment) {
        char path[D1L_RP2040_FILE_PATH_MAX + 1U];
        const int written = snprintf(path, sizeof(path),
                                     "stores/packet_log/segments/s%02lu.bin",
                                     (unsigned long)segment);
        assert(written > 0 && (size_t)written < sizeof(path));
        seed_sd_file(path, stale, sizeof(stale));
    }
    seed_sd_file("stores/packet_log/ring.bin", stale, sizeof(stale));
    seed_sd_file("stores/packet_log/ring.tmp", stale, sizeof(stale));
    seed_sd_file("stores/packet_log/operator-export.bin",
                 unrelated, sizeof(unrelated));
    reset_sd_events();
    assert(d1l_packet_log_init() == ESP_OK);
    for (uint32_t segment = 0U;
         segment < D1L_PACKET_LOG_SD_SEGMENT_COUNT; ++segment) {
        char path[D1L_RP2040_FILE_PATH_MAX + 1U];
        const int written = snprintf(path, sizeof(path),
                                     "stores/packet_log/segments/s%02lu.bin",
                                     (unsigned long)segment);
        assert(written > 0 && (size_t)written < sizeof(path));
        assert(!find_sd_file(path));
        assert(count_sd_event(TEST_SD_EVENT_DELETE, path) == 1U);
    }
    assert(find_sd_file("stores/packet_log/operator-export.bin"));
    assert_exact_marker(packet_marker_path,
                        D1L_FACTORY_RESET_SD_STORE_PACKET_LOG,
                        reset_generation);
    assert(assert_reset_lineage(
               D1L_FACTORY_RESET_SD_STORE_PACKET_LOG, false) ==
           reset_generation);
    d1l_retained_blob_store_backend_state_t packet_backend = {0};
    assert(d1l_retained_blob_store_backend_state(
        D1L_RETAINED_BLOB_STORE_PACKET_LOG, &packet_backend));
    assert(d1l_retained_blob_store_sd_media_lineage_ready(
               D1L_RETAINED_BLOB_STORE_PACKET_LOG,
               packet_backend.generation, &ready) == ESP_OK);
    assert(ready);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "profile-gate") == 0) {
        test_release_profile_sd_admission();
        puts("native retained SD profile admission: ok");
        return 0;
    }

    /* Production initializes default NVS before enabling any SD backend; the
     * lineage gate must be able to prove that no reset generation exists. */
    s_nvs_enabled = true;
    assert_all_states(false, 0U);

    d1l_retained_blob_store_backend_state_t invalid = {
        .enabled = true,
        .generation = 99U,
    };
    assert(!d1l_retained_blob_store_backend_state(
        D1L_RETAINED_BLOB_STORE_COUNT, &invalid));
    assert(invalid.enabled && invalid.generation == 99U);
    assert(!d1l_retained_blob_store_backend_state(
        D1L_RETAINED_BLOB_STORE_ROUTES, NULL));

    d1l_retained_blob_store_note_sd_backend(false, false, false, 0U, 0U, 0U);
    assert_all_states(false, 0U);

    d1l_retained_blob_store_note_sd_backend(
        true, true, true, D1L_RP2040_FILE_LINE_MAX,
        D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX);
    assert_all_states(true, 1U);

    test_background_read_yields_between_chunks_without_degrading_sd();
    test_background_write_yields_before_rename_without_degrading_sd();

    d1l_retained_blob_store_note_sd_backend(
        true, true, true, D1L_RP2040_FILE_LINE_MAX,
        D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX);
    assert_all_states(true, 1U);

    /* A complete false -> true cycle remains visible to a later consumer even
     * when it never sampled the intermediate disabled state. */
    d1l_retained_blob_store_note_sd_backend(false, false, false, 0U, 0U, 0U);
    d1l_retained_blob_store_note_sd_backend(
        true, true, true, D1L_RP2040_FILE_LINE_MAX,
        D1L_RP2040_FILE_CHUNK_MAX, D1L_RP2040_FILE_PATH_MAX);
    assert_all_states(true, 3U);

    static const uint8_t payload[] = "guarded retained blob";
    s_toggle_backend_during_write = true;
    assert(d1l_retained_blob_store_write_sd_primary_guarded(
               D1L_RETAINED_BLOB_STORE_ROUTES, "routes_v2",
               payload, sizeof(payload), 3U) == ESP_ERR_INVALID_STATE);
    assert_all_states(true, 5U);
    assert(s_rename_count == 0U);
    assert(s_delete_count == 0U);

    assert(d1l_retained_blob_store_write_sd_primary_guarded(
               D1L_RETAINED_BLOB_STORE_ROUTES, "routes_v2",
               payload, sizeof(payload), 5U) == ESP_OK);
    assert(s_rename_count == 1U);

    /* Generic Public/DM/packet split writers share the same guarded internal
     * commit point even though they do not supply an external generation. */
    s_toggle_backend_during_write = true;
    assert(d1l_retained_blob_store_write_split(
               D1L_RETAINED_BLOB_STORE_PACKET_LOG, "ring",
               payload, sizeof(payload), payload, sizeof(payload)) ==
           ESP_ERR_INVALID_STATE);
    assert_all_states(true, 7U);
    assert(s_rename_count == 1U);
    assert(s_delete_count == 0U);

    s_toggle_backend_during_delete = true;
    assert(d1l_retained_blob_store_erase_sd_primary_guarded(
               D1L_RETAINED_BLOB_STORE_ROUTES, "routes_v2", 7U) ==
           ESP_ERR_INVALID_STATE);
    assert_all_states(true, 9U);
    assert(s_delete_count == 1U);

    test_retained_partition_init();
    test_first_use_initializes_only_new_retained_region();
    test_first_marker_recovers_init_power_loss_window();
    test_version_one_marker_candidate_upgrades_after_sentinel();
    test_populated_version_one_upgrade_survives_finalize_failures();
    test_marker_loss_recovers_valid_nvs_and_unsafe_states_fail_closed();
    test_nonblank_unowned_region_requires_external_initialization();
    test_anchor_only_claim_loss_fails_closed();
    test_completion_sentinel_blocks_blank_established_partition();
    test_anchor_commit_failure_is_retryable_and_fail_closed();
    test_marker_flash_failures_are_retryable_and_ordered();
    test_sentinel_commit_failure_is_retryable_and_fail_closed();
    test_partition_init_proactively_migrates_known_legacy_key();
    test_divergent_upgrade_copies_fail_closed();
    test_dedicated_read_precedes_legacy();
    test_legacy_read_migrates_after_dedicated_miss();
    test_dedicated_write_reclaims_legacy_only_after_commit();
    test_erase_clears_dedicated_and_legacy_copies();
    test_nvs_capacity_and_write_amplification_telemetry();
    test_unchanged_nvs_fallback_write_skips_flash_commit();
    test_nvs_unchanged_compare_failure_fails_safe_to_write();
    test_partition_init_failure_never_falls_back_or_resurrects();
    test_factory_reset_sd_recovery_end_to_end();

    puts("native retained backend generation and NVS partition: ok");
    return 0;
}
