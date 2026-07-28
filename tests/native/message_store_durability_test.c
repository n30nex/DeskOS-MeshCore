#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/semphr.h"
#include "mesh/message_store.h"
#include "storage/retained_blob_store.h"

#define TEST_SCHEMA 5U
#define TEST_SCHEMA_V4 4U
#define TEST_SCHEMA_V3 3U
#define TEST_SCHEMA_V2 2U
#define TEST_SCHEMA_V1 1U
#define TEST_TEXT_LEN_V1 96U

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    char direction[4];
    char author[D1L_MESSAGE_AUTHOR_LEN];
    char text[D1L_MESSAGE_TEXT_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool delivered;
} test_entry_v4_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    uint32_t epoch;
    uint32_t content_revision;
    uint64_t clear_lineage;
    test_entry_v4_t entries[D1L_MESSAGE_STORE_CAPACITY];
} test_blob_v4_t;

_Static_assert(sizeof(test_entry_v4_t) == 188U,
               "native message v4 entry layout changed");
_Static_assert(offsetof(test_blob_v4_t, entries) == 40U,
               "native message v4 entries offset changed");
_Static_assert(sizeof(test_blob_v4_t) == 3048U,
               "native message v4 blob layout changed");

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    uint32_t epoch;
    uint32_t content_revision;
    uint64_t clear_lineage;
    d1l_message_entry_t entries[D1L_MESSAGE_STORE_CAPACITY];
} test_blob_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    uint32_t epoch;
    uint32_t content_revision;
    test_entry_v4_t entries[D1L_MESSAGE_STORE_CAPACITY];
} test_blob_v3_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    test_entry_v4_t entries[D1L_MESSAGE_STORE_CAPACITY];
} test_blob_v2_t;

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    char direction[4];
    char author[D1L_MESSAGE_AUTHOR_LEN];
    char text[TEST_TEXT_LEN_V1];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    bool delivered;
} test_entry_v1_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    test_entry_v1_t entries[D1L_MESSAGE_STORE_CAPACITY];
} test_blob_v1_t;

typedef struct {
    bool valid;
    size_t len;
    uint8_t data[sizeof(test_blob_t)];
} test_slot_t;

static test_slot_t s_sd;
static test_slot_t s_nvs;
static bool s_sd_enabled;
static uint32_t s_generation;
static uint32_t s_backend_state_count;
static uint32_t s_flip_generation_on_backend_state_call;
static uint32_t s_sd_read_count;
static uint32_t s_sd_write_count;
static uint32_t s_nvs_write_count;
static uint32_t s_sd_erase_count;
static uint32_t s_nvs_erase_count;
static esp_err_t s_fail_next_sd_read;
static esp_err_t s_fail_all_sd_reads;
static esp_err_t s_fail_next_nvs_write;
static bool s_replace_during_sd_write;
static bool s_fail_next_zero_wait_take;
static test_blob_t s_replacement;
static int64_t s_now_us;
static uint32_t s_random_state;

static void slot_write(test_slot_t *slot, const void *src, size_t len)
{
    assert(slot != NULL);
    assert(src != NULL);
    assert(len <= sizeof(slot->data));
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->data, src, len);
    slot->len = len;
    slot->valid = true;
}

static esp_err_t slot_read(const test_slot_t *slot, void *dst, size_t *len_inout)
{
    assert(dst != NULL);
    assert(len_inout != NULL);
    if (!slot->valid) {
        return ESP_ERR_NOT_FOUND;
    }
    if (*len_inout < slot->len) {
        *len_inout = slot->len;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, slot->data, slot->len);
    *len_inout = slot->len;
    return ESP_OK;
}

static void fill_entry(d1l_message_entry_t *entry, uint32_t seq,
                       const char *prefix)
{
    memset(entry, 0, sizeof(*entry));
    entry->seq = seq;
    entry->uptime_ms = seq * 100U;
    (void)snprintf(entry->direction, sizeof(entry->direction), "rx");
    (void)snprintf(entry->author, sizeof(entry->author), "Node");
    (void)snprintf(entry->text, sizeof(entry->text), "%s-%lu", prefix,
                   (unsigned long)seq);
    entry->rssi_dbm = -60;
    entry->snr_tenths = 40;
    entry->path_hash_bytes = 1U;
    entry->path_hops = 1U;
    entry->delivered = true;
    entry->channel_id = UINT64_C(1);
}

static void fill_entry_v4(test_entry_v4_t *entry, uint32_t seq,
                          const char *prefix)
{
    memset(entry, 0, sizeof(*entry));
    entry->seq = seq;
    entry->uptime_ms = seq * 100U;
    (void)snprintf(entry->direction, sizeof(entry->direction), "rx");
    (void)snprintf(entry->author, sizeof(entry->author), "Node");
    (void)snprintf(entry->text, sizeof(entry->text), "%s-%lu", prefix,
                   (unsigned long)seq);
    entry->rssi_dbm = -60;
    entry->snr_tenths = 40;
    entry->path_hash_bytes = 1U;
    entry->path_hops = 1U;
    entry->delivered = true;
}

static test_blob_t make_blob(uint32_t total_written, uint32_t first_seq,
                             uint32_t count, const char *prefix)
{
    assert(count <= D1L_MESSAGE_STORE_CAPACITY);
    test_blob_t blob = {
        .schema = TEST_SCHEMA,
        .epoch = 1U,
        .content_revision = total_written == UINT32_MAX ?
                            UINT32_MAX : total_written + 1U,
        .next_seq = total_written + 1U,
        .total_written = total_written,
        .dropped_oldest = total_written >= count ? total_written - count : 0U,
        .head = count % D1L_MESSAGE_STORE_CAPACITY,
        .count = count,
    };
    for (uint32_t i = 0U; i < count; ++i) {
        fill_entry(&blob.entries[i], first_seq + i, prefix);
    }
    return blob;
}

static const test_blob_t *slot_blob(const test_slot_t *slot)
{
    assert(slot->valid);
    assert(slot->len == sizeof(test_blob_t));
    return (const test_blob_t *)slot->data;
}

static bool blob_contains(const test_blob_t *blob, const char *text)
{
    for (uint32_t i = 0U; i < blob->count; ++i) {
        if (strcmp(blob->entries[i].text, text) == 0) {
            return true;
        }
    }
    return false;
}

static void set_sd_enabled(bool enabled)
{
    if (s_sd_enabled != enabled) {
        s_sd_enabled = enabled;
        s_generation++;
    }
}

static void reset_mock(bool sd_enabled)
{
    memset(&s_sd, 0, sizeof(s_sd));
    memset(&s_nvs, 0, sizeof(s_nvs));
    memset(&s_replacement, 0, sizeof(s_replacement));
    s_sd_enabled = sd_enabled;
    s_generation = 0U;
    s_backend_state_count = 0U;
    s_flip_generation_on_backend_state_call = 0U;
    s_sd_read_count = 0U;
    s_sd_write_count = 0U;
    s_nvs_write_count = 0U;
    s_sd_erase_count = 0U;
    s_nvs_erase_count = 0U;
    s_fail_next_sd_read = ESP_OK;
    s_fail_all_sd_reads = ESP_OK;
    s_fail_next_nvs_write = ESP_OK;
    s_replace_during_sd_write = false;
    s_fail_next_zero_wait_take = false;
    s_now_us = 1000000LL;
    s_random_state = 0x13579BDFU;
}

static void advance_ms(uint32_t milliseconds)
{
    s_now_us += (int64_t)milliseconds * 1000LL;
}

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

uint32_t esp_random(void)
{
    s_random_state = s_random_state * 1664525U + 1013904223U;
    return s_random_state;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    return buffer;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks_to_wait)
{
    (void)handle;
    if (ticks_to_wait == 0U && s_fail_next_zero_wait_take) {
        s_fail_next_zero_wait_take = false;
        return 0;
    }
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    (void)handle;
    return pdTRUE;
}

bool d1l_retained_blob_store_backend_state(
    d1l_retained_blob_store_id_t store_id,
    d1l_retained_blob_store_backend_state_t *out_state)
{
    if (store_id != D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES || !out_state) {
        return false;
    }
    s_backend_state_count++;
    if (s_flip_generation_on_backend_state_call == s_backend_state_count) {
        s_generation++;
    }
    out_state->enabled = s_sd_enabled;
    out_state->generation = s_generation;
    return true;
}

esp_err_t d1l_retained_blob_store_read_sd_primary(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES);
    assert(strcmp(key, "public") == 0);
    s_sd_read_count++;
    if (!s_sd_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_fail_next_sd_read != ESP_OK) {
        const esp_err_t ret = s_fail_next_sd_read;
        s_fail_next_sd_read = ESP_OK;
        return ret;
    }
    if (s_fail_all_sd_reads != ESP_OK) {
        return s_fail_all_sd_reads;
    }
    return slot_read(&s_sd, dst, len_inout);
}

esp_err_t d1l_retained_blob_store_read_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES);
    assert(strcmp(key, "public") == 0);
    return slot_read(&s_nvs, dst, len_inout);
}

esp_err_t d1l_retained_blob_store_write_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len, uint32_t expected_generation)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES);
    assert(strcmp(key, "public") == 0);
    s_sd_write_count++;
    if (!s_sd_enabled || expected_generation != s_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_replace_during_sd_write) {
        s_replace_during_sd_write = false;
        set_sd_enabled(false);
        slot_write(&s_sd, &s_replacement, sizeof(s_replacement));
        set_sd_enabled(true);
    }
    if (!s_sd_enabled || expected_generation != s_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    slot_write(&s_sd, src, len);
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_write_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES);
    assert(strcmp(key, "public") == 0);
    s_nvs_write_count++;
    if (s_fail_next_nvs_write != ESP_OK) {
        const esp_err_t ret = s_fail_next_nvs_write;
        s_fail_next_nvs_write = ESP_OK;
        return ret;
    }
    slot_write(&s_nvs, src, len);
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_erase_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id, const char *key,
    uint32_t expected_generation)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES);
    assert(strcmp(key, "public") == 0);
    s_sd_erase_count++;
    if (!s_sd_enabled || expected_generation != s_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_sd, 0, sizeof(s_sd));
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_erase_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES);
    assert(strcmp(key, "public") == 0);
    s_nvs_erase_count++;
    memset(&s_nvs, 0, sizeof(s_nvs));
    return ESP_OK;
}

static void test_sd_success_nvs_failure_retries_only_nvs(void)
{
    reset_mock(true);
    assert(d1l_message_store_init() == ESP_OK);
    s_fail_next_nvs_write = ESP_FAIL;
    assert(d1l_message_store_append_public(
               "rx", "Node", "mirror-retry", -60, 40, 1U, 1U, true) ==
           ESP_FAIL);
    assert(s_sd_write_count == 1U);
    assert(s_nvs_write_count == 1U);
    d1l_message_store_stats_t stats = d1l_message_store_stats();
    assert(!stats.sd_primary_dirty);
    assert(stats.nvs_fallback_dirty);
    assert(stats.persistence_dirty);
    assert(stats.nvs_fallback_last_error == ESP_FAIL);

    assert(d1l_message_store_flush() == ESP_FAIL);
    assert(s_sd_write_count == 1U && s_nvs_write_count == 1U);
    advance_ms(D1L_MESSAGE_STORE_PERSIST_RETRY_MS);
    assert(d1l_message_store_flush() == ESP_OK);
    assert(s_sd_write_count == 1U);
    assert(s_nvs_write_count == 2U);
    stats = d1l_message_store_stats();
    assert(!stats.persistence_dirty);
    assert(!stats.nvs_fallback_dirty);
    assert(blob_contains(slot_blob(&s_nvs), "mirror-retry"));

    set_sd_enabled(false);
    assert(d1l_message_store_init() == ESP_OK);
    d1l_message_entry_t restored = {0};
    assert(d1l_message_store_copy_recent(&restored, 1U) == 1U);
    assert(strcmp(restored.text, "mirror-retry") == 0);
}

static void test_late_sd_merges_live_overlay_before_write(void)
{
    reset_mock(false);
    test_blob_t fallback = make_blob(4U, 1U, 4U, "base");
    test_blob_t primary = make_blob(20U, 5U, 16U, "sd");
    slot_write(&s_nvs, &fallback, sizeof(fallback));
    slot_write(&s_sd, &primary, sizeof(primary));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_append_public(
               "rx", "Node", "live-overlay", -60, 40, 1U, 1U, true) ==
           ESP_OK);
    assert(s_sd_write_count == 0U);

    set_sd_enabled(true);
    assert(d1l_message_store_flush_if_due() == ESP_OK);
    assert(s_sd_read_count == 1U);
    assert(s_sd_write_count == 1U);
    const test_blob_t *committed = slot_blob(&s_sd);
    assert(committed->total_written == 21U);
    assert(committed->next_seq == 22U);
    assert(blob_contains(committed, "live-overlay"));
    assert(blob_contains(committed, "sd-20"));
    assert(!d1l_message_store_stats().persistence_dirty);
}

static void test_unsampled_replacement_generation_is_reconciled(void)
{
    reset_mock(true);
    test_blob_t initial = make_blob(1U, 1U, 1U, "initial");
    slot_write(&s_sd, &initial, sizeof(initial));
    slot_write(&s_nvs, &initial, sizeof(initial));
    assert(d1l_message_store_init() == ESP_OK);

    set_sd_enabled(false);
    test_blob_t replacement = make_blob(10U, 10U, 1U, "replacement");
    slot_write(&s_sd, &replacement, sizeof(replacement));
    set_sd_enabled(true);
    s_sd_read_count = 0U;
    s_sd_write_count = 0U;
    assert(d1l_message_store_append_public(
               "rx", "Node", "local-after-swap", -60, 40, 1U, 1U, true) ==
           ESP_OK);
    assert(s_sd_read_count == 1U);
    assert(s_sd_write_count == 1U);
    const test_blob_t *committed = slot_blob(&s_sd);
    assert(committed->total_written == 11U);
    assert(blob_contains(committed, "replacement-10"));
    assert(blob_contains(committed, "local-after-swap"));
}

static void test_read_failure_never_overwrites_sd_but_nvs_advances(void)
{
    reset_mock(true);
    test_blob_t fallback = make_blob(2U, 1U, 2U, "base");
    test_blob_t primary = make_blob(8U, 8U, 1U, "preserve");
    slot_write(&s_nvs, &fallback, sizeof(fallback));
    slot_write(&s_sd, &primary, sizeof(primary));
    s_fail_all_sd_reads = ESP_FAIL;
    assert(d1l_message_store_init() == ESP_OK);
    assert(s_sd_write_count == 0U && s_sd_erase_count == 0U);
    assert(d1l_message_store_stats().sd_primary_reconcile_pending);

    assert(d1l_message_store_append_public(
               "rx", "Node", "nvs-survivor", -60, 40, 1U, 1U, true) ==
           ESP_FAIL);
    assert(s_sd_write_count == 0U);
    assert(s_nvs_write_count == 1U);
    assert(blob_contains(slot_blob(&s_nvs), "nvs-survivor"));
    assert(blob_contains(slot_blob(&s_sd), "preserve-8"));
    assert(d1l_message_store_stats().sd_primary_reconcile_pending);
}

static void test_generation_edge_during_write_preserves_replacement(void)
{
    reset_mock(true);
    test_blob_t initial = make_blob(1U, 1U, 1U, "initial");
    slot_write(&s_sd, &initial, sizeof(initial));
    slot_write(&s_nvs, &initial, sizeof(initial));
    assert(d1l_message_store_init() == ESP_OK);
    s_replacement = make_blob(10U, 10U, 1U, "swap-card");
    s_replace_during_sd_write = true;

    assert(d1l_message_store_append_public(
               "rx", "Node", "local-during-write", -60, 40, 1U, 1U, true) ==
           ESP_ERR_INVALID_STATE);
    assert(blob_contains(slot_blob(&s_sd), "swap-card-10"));
    d1l_message_store_stats_t stats = d1l_message_store_stats();
    assert(stats.sd_primary_dirty);
    assert(stats.sd_primary_reconcile_pending);
    assert(stats.persistence_dirty);

    advance_ms(D1L_MESSAGE_STORE_PERSIST_RETRY_MS);
    assert(d1l_message_store_flush() == ESP_OK);
    const test_blob_t *committed = slot_blob(&s_sd);
    assert(committed->total_written == 11U);
    assert(blob_contains(committed, "swap-card-10"));
    assert(blob_contains(committed, "local-during-write"));
    assert(!d1l_message_store_stats().persistence_dirty);
}

static void test_generation_flip_after_merge_rolls_back_before_nvs_write(void)
{
    reset_mock(false);
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_append_public(
               "rx", "Node", "local-before-swap", -60, 40,
               1U, 1U, true) == ESP_OK);
    assert(d1l_message_store_append_public_volatile(
               "rx", "UI", "preview-survives-reconcile", -60, 40,
               1U, 1U, true) == ESP_OK);
    test_blob_t stale = make_blob(1U, 1U, 1U, "stale-card-a");
    stale.epoch = 7U;
    slot_write(&s_sd, &stale, sizeof(stale));
    set_sd_enabled(true);
    const test_slot_t nvs_before = s_nvs;
    s_backend_state_count = 0U;
    s_flip_generation_on_backend_state_call = 4U;
    s_sd_write_count = 0U;
    s_nvs_write_count = 0U;

    assert(d1l_message_store_flush() == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U && s_nvs_write_count == 0U);
    assert(memcmp(&s_nvs, &nvs_before, sizeof(s_nvs)) == 0);
    assert(blob_contains(slot_blob(&s_nvs), "local-before-swap"));
    assert(!blob_contains(slot_blob(&s_nvs), "stale-card-a-1"));
    assert(d1l_message_store_stats().sd_primary_reconcile_pending);
    d1l_message_entry_t visible[2] = {0};
    assert(d1l_message_store_copy_recent(visible, 2U) == 2U);
    assert(strcmp(visible[1].text, "preview-survives-reconcile") == 0);
}

static void test_init_generation_flip_restores_nvs_before_loaded_commit(void)
{
    reset_mock(true);
    test_blob_t local = make_blob(1U, 1U, 1U, "init-local");
    test_blob_t stale = make_blob(1U, 1U, 1U, "init-stale-card");
    stale.epoch = 9U;
    slot_write(&s_nvs, &local, sizeof(local));
    slot_write(&s_sd, &stale, sizeof(stale));
    const test_slot_t nvs_before = s_nvs;
    s_backend_state_count = 0U;
    s_flip_generation_on_backend_state_call = 3U;

    assert(d1l_message_store_init() == ESP_OK);
    assert(memcmp(&s_nvs, &nvs_before, sizeof(s_nvs)) == 0);
    d1l_message_entry_t row = {0};
    assert(d1l_message_store_copy_recent(&row, 1U) == 1U);
    assert(strcmp(row.text, "init-local-1") == 0);
    assert(d1l_message_store_stats().sd_primary_reconcile_pending);

    memset(&s_sd, 0, sizeof(s_sd));
    s_flip_generation_on_backend_state_call = 0U;
    advance_ms(D1L_MESSAGE_STORE_PERSIST_RETRY_MS);
    assert(d1l_message_store_flush() == ESP_OK);
    assert(blob_contains(slot_blob(&s_sd), "init-local-1"));
    assert(!blob_contains(slot_blob(&s_sd), "init-stale-card-1"));
}

static void test_corrupt_sd_stays_pending_while_valid_nvs_advances(void)
{
    reset_mock(true);
    test_blob_t fallback = make_blob(2U, 1U, 2U, "base");
    test_blob_t corrupt_sd = make_blob(8U, 8U, 1U, "corrupt");
    corrupt_sd.entries[0].author[0] = '\x01';
    slot_write(&s_nvs, &fallback, sizeof(fallback));
    slot_write(&s_sd, &corrupt_sd, sizeof(corrupt_sd));

    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_stats().sd_primary_reconcile_pending);
    assert(s_sd_write_count == 0U && s_sd_erase_count == 0U);
    assert(d1l_message_store_append_public(
               "rx", "Node", "corrupt-sd-survivor", -60, 40,
               1U, 1U, true) == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U && s_sd_erase_count == 0U);
    assert(s_nvs_write_count == 1U);
    assert(blob_contains(slot_blob(&s_nvs), "corrupt-sd-survivor"));
    assert(d1l_message_store_stats().sd_primary_reconcile_pending);
}

static void test_corrupt_sources_are_never_erased_on_init(void)
{
    reset_mock(true);
    test_blob_t corrupt_nvs = make_blob(1U, 1U, 1U, "bad-nvs");
    test_blob_t corrupt_sd = make_blob(1U, 1U, 1U, "bad-sd");
    corrupt_nvs.head = D1L_MESSAGE_STORE_CAPACITY;
    corrupt_sd.entries[0].text[0] = '\x01';
    slot_write(&s_nvs, &corrupt_nvs, sizeof(corrupt_nvs));
    slot_write(&s_sd, &corrupt_sd, sizeof(corrupt_sd));
    assert(d1l_message_store_init() == ESP_ERR_INVALID_STATE);
    const d1l_message_store_stats_t stats = d1l_message_store_stats();
    assert(!stats.loaded);
    assert(stats.persistence_dirty);
    assert(stats.persistence_fail_count > 0U);
    assert(stats.nvs_fallback_dirty);
    assert(stats.nvs_fallback_last_error == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U && s_nvs_write_count == 0U);
    assert(s_sd_erase_count == 0U && s_nvs_erase_count == 0U);
    assert(s_sd.valid && s_nvs.valid);
}

static void test_periodic_flush_retries_transient_failed_initialization(void)
{
    reset_mock(true);
    test_blob_t recovery = make_blob(1U, 1U, 1U, "recovered-sd");
    slot_write(&s_sd, &recovery, sizeof(recovery));
    s_fail_next_sd_read = ESP_FAIL;
    assert(d1l_message_store_init() == ESP_FAIL);
    assert(!d1l_message_store_stats().loaded);
    assert(s_sd_write_count == 0U && s_nvs_write_count == 0U);

    /* The periodic/forced worker path must be able to recover without an
     * append, UI query, or reboot after the transient media read clears. */
    assert(d1l_message_store_flush_if_due() == ESP_OK);
    const d1l_message_store_stats_t stats = d1l_message_store_stats();
    assert(stats.loaded);
    assert(!stats.persistence_dirty);
    assert(blob_contains(slot_blob(&s_nvs), "recovered-sd-1"));
}

static void test_v1_fallback_remains_readable_and_rewrites_as_v5(void)
{
    reset_mock(false);
    test_blob_v1_t legacy = {
        .schema = TEST_SCHEMA_V1,
        .next_seq = 2U,
        .total_written = 1U,
        .head = 1U,
        .count = 1U,
    };
    legacy.entries[0].seq = 1U;
    legacy.entries[0].uptime_ms = 100U;
    (void)snprintf(legacy.entries[0].direction,
                   sizeof(legacy.entries[0].direction), "rx");
    (void)snprintf(legacy.entries[0].author,
                   sizeof(legacy.entries[0].author), "Legacy");
    (void)snprintf(legacy.entries[0].text,
                   sizeof(legacy.entries[0].text), "legacy-public");
    legacy.entries[0].delivered = true;
    slot_write(&s_nvs, &legacy, sizeof(legacy));
    assert(d1l_message_store_init() == ESP_OK);
    d1l_message_entry_t restored = {0};
    assert(d1l_message_store_copy_recent(&restored, 1U) == 1U);
    assert(strcmp(restored.text, "legacy-public") == 0);
    assert(d1l_message_store_stats().nvs_fallback_dirty);
    assert(d1l_message_store_flush() == ESP_OK);
    assert(slot_blob(&s_nvs)->schema == TEST_SCHEMA);
    assert(blob_contains(slot_blob(&s_nvs), "legacy-public"));
}

static void test_v2_fallback_migrates_to_v5_without_lineage(void)
{
    reset_mock(false);
    test_blob_v2_t legacy = {
        .schema = TEST_SCHEMA_V2,
        .next_seq = 2U,
        .total_written = 1U,
        .head = 1U,
        .count = 1U,
    };
    fill_entry_v4(&legacy.entries[0], 1U, "v2-public");
    slot_write(&s_nvs, &legacy, sizeof(legacy));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_stats().nvs_fallback_dirty);
    assert(d1l_message_store_flush() == ESP_OK);
    assert(slot_blob(&s_nvs)->schema == TEST_SCHEMA);
    assert(slot_blob(&s_nvs)->epoch == 1U);
    assert(slot_blob(&s_nvs)->clear_lineage == 0U);
    assert(blob_contains(slot_blob(&s_nvs), "v2-public-1"));
}

static void test_v3_fallback_migrates_to_v5_without_inventing_clear(void)
{
    reset_mock(false);
    test_blob_v3_t legacy = {
        .schema = TEST_SCHEMA_V3,
        .next_seq = 2U,
        .total_written = 1U,
        .head = 1U,
        .count = 1U,
        .epoch = 77U,
        .content_revision = 9U,
    };
    fill_entry_v4(&legacy.entries[0], 1U, "v3-public");
    slot_write(&s_nvs, &legacy, sizeof(legacy));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_stats().nvs_fallback_dirty);
    assert(d1l_message_store_flush() == ESP_OK);
    assert(slot_blob(&s_nvs)->schema == TEST_SCHEMA);
    assert(slot_blob(&s_nvs)->epoch == 77U);
    assert(slot_blob(&s_nvs)->clear_lineage == 0U);
    assert(blob_contains(slot_blob(&s_nvs), "v3-public-1"));
}

static void test_v4_fallback_migrates_to_v5_public_identity(void)
{
    reset_mock(false);
    test_blob_v4_t legacy = {
        .schema = TEST_SCHEMA_V4,
        .next_seq = 2U,
        .total_written = 1U,
        .head = 1U,
        .count = 1U,
        .epoch = 3U,
        .content_revision = 4U,
        .clear_lineage = UINT64_C(0x1122334455667788),
    };
    fill_entry_v4(&legacy.entries[0], 1U, "v4-public");
    const char utf8_text[] = {
        'C', 'a', 'f', (char)0xc3, (char)0xa9, ' ',
        (char)0xe6, (char)0x9d, (char)0xb1,
        (char)0xe4, (char)0xba, (char)0xac, ' ',
        (char)0xf0, (char)0x9f, (char)0x98, (char)0x80, '\0'
    };
    memcpy(legacy.entries[0].text, utf8_text, sizeof(utf8_text));
    slot_write(&s_nvs, &legacy, sizeof(legacy));
    assert(d1l_message_store_init() == ESP_OK);
    d1l_message_entry_t restored = {0};
    assert(d1l_message_store_copy_recent(&restored, 1U) == 1U);
    assert(restored.channel_id == UINT64_C(1));
    assert(memcmp(restored.text, utf8_text, sizeof(utf8_text)) == 0);
    assert(d1l_message_store_stats().nvs_fallback_dirty);
    assert(d1l_message_store_flush() == ESP_OK);
    assert(slot_blob(&s_nvs)->schema == TEST_SCHEMA);
    assert(slot_blob(&s_nvs)->entries[0].channel_id == UINT64_C(1));
    assert(memcmp(slot_blob(&s_nvs)->entries[0].text, utf8_text,
                  sizeof(utf8_text)) == 0);
    assert(d1l_message_store_init() == ESP_OK);
    memset(&restored, 0, sizeof(restored));
    assert(d1l_message_store_copy_recent(&restored, 1U) == 1U);
    assert(restored.channel_id == UINT64_C(1));
    assert(memcmp(restored.text, utf8_text, sizeof(utf8_text)) == 0);
}

static void test_mixed_channel_history_isolation_and_public_only_clear(void)
{
    reset_mock(false);
    assert(d1l_message_store_init() == ESP_OK);
    uint32_t seq = 0U;
    assert(d1l_message_store_append_channel(
               UINT64_C(1), "rx", "Public", "public-one", -60, 40,
               1U, 1U, true, &seq) == ESP_OK && seq == 1U);
    assert(d1l_message_store_append_channel(
               UINT64_C(2), "rx", "Alpha", "alpha-one", -61, 30,
               1U, 1U, true, &seq) == ESP_OK && seq == 2U);
    assert(d1l_message_store_append_channel(
               UINT64_C(1), "tx", "Self", "public-two", 0, 0,
               1U, 0U, false, &seq) == ESP_OK && seq == 3U);
    assert(d1l_message_store_append_channel(
               UINT64_C(3), "rx", "Beta", "beta-only", -62, 20,
               1U, 1U, true, &seq) == ESP_OK && seq == 4U);
    assert(d1l_message_store_append_channel(
               UINT64_C(2), "tx", "Self", "alpha-two", 0, 0,
               1U, 0U, false, &seq) == ESP_OK && seq == 5U);

    d1l_message_store_stats_t stats = d1l_message_store_stats();
    assert(stats.count == 5U && stats.public_count == 2U &&
           stats.next_seq == 6U);
    d1l_message_entry_t rows[D1L_MESSAGE_STORE_CAPACITY + 1U] = {0};
    assert(d1l_message_store_copy_recent(
               rows, D1L_MESSAGE_STORE_CAPACITY) == 2U);
    assert(rows[0].channel_id == UINT64_C(1) &&
           strcmp(rows[0].text, "public-one") == 0);
    assert(rows[1].channel_id == UINT64_C(1) &&
           strcmp(rows[1].text, "public-two") == 0);
    assert(d1l_message_store_copy_channel_recent(
               UINT64_C(2), rows, D1L_MESSAGE_STORE_CAPACITY) == 2U);
    assert(strcmp(rows[0].text, "alpha-one") == 0 &&
           strcmp(rows[1].text, "alpha-two") == 0);

    size_t total = 99U;
    assert(d1l_message_store_query_page(
               rows, D1L_MESSAGE_STORE_CAPACITY, 0U, "alpha", &total) ==
           0U);
    assert(total == 0U);
    assert(d1l_message_store_query_channel_page(
               UINT64_C(2), rows, 1U, 1U, "alpha", &total) == 1U);
    assert(total == 2U && strcmp(rows[0].text, "alpha-one") == 0);

    assert(d1l_message_store_append_channel_volatile(
               UINT64_C(2), "rx", "Alpha", "alpha-preview", -63, 10,
               1U, 1U, true) == ESP_OK);
    assert(d1l_message_store_copy_recent(
               rows, D1L_MESSAGE_STORE_CAPACITY + 1U) == 2U);
    assert(d1l_message_store_copy_channel_recent(
               UINT64_C(2), rows, D1L_MESSAGE_STORE_CAPACITY + 1U) == 3U);
    assert(strcmp(rows[2].text, "alpha-preview") == 0);
    size_t retained_count = 0U;
    d1l_message_retained_snapshot_t retained_snapshot = {0};
    assert(d1l_message_store_snapshot_retained(
               rows, D1L_MESSAGE_STORE_CAPACITY, &retained_count,
               &retained_snapshot) == ESP_OK);
    assert(retained_count == 5U &&
           retained_snapshot.persistence_revision != 0U &&
           retained_snapshot.epoch == stats.epoch &&
           retained_snapshot.next_seq == stats.next_seq &&
           retained_snapshot.clear_lineage == stats.clear_lineage);
    for (size_t i = 0U; i < retained_count; ++i) {
        assert(strcmp(rows[i].text, "alpha-preview") != 0);
    }

    const uint64_t lineage_before = stats.clear_lineage;
    assert(d1l_message_store_clear_channel(UINT64_C(1)) == ESP_OK);
    stats = d1l_message_store_stats();
    assert(stats.count == 3U && stats.public_count == 0U &&
           stats.next_seq == 6U && stats.clear_lineage != lineage_before);
    assert(d1l_message_store_copy_recent(
               rows, D1L_MESSAGE_STORE_CAPACITY) == 0U);
    assert(d1l_message_store_copy_channel_recent(
               UINT64_C(2), rows, D1L_MESSAGE_STORE_CAPACITY) == 3U);
    assert(d1l_message_store_copy_channel_recent(
               UINT64_C(3), rows, D1L_MESSAGE_STORE_CAPACITY) == 1U);

    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_copy_channel_recent(
               UINT64_C(2), rows, D1L_MESSAGE_STORE_CAPACITY) == 2U);
    assert(d1l_message_store_append_channel(
               UINT64_C(1), "rx", "Public", "public-after-clear", -60,
               40, 1U, 1U, true, &seq) == ESP_OK && seq == 6U);
    assert(d1l_message_store_copy_recent(rows, 1U) == 1U);
    assert(rows[0].seq == 6U &&
           strcmp(rows[0].text, "public-after-clear") == 0);
}

static void test_offline_clear_tombstone_blocks_sd_resurrection(void)
{
    reset_mock(false);
    test_blob_t old = make_blob(1U, 1U, 1U, "must-stay-cleared");
    slot_write(&s_nvs, &old, sizeof(old));
    slot_write(&s_sd, &old, sizeof(old));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_clear() == ESP_OK);
    assert(slot_blob(&s_nvs)->epoch == 2U);
    assert(slot_blob(&s_nvs)->clear_lineage != 0U);
    assert(slot_blob(&s_nvs)->count == 0U);

    set_sd_enabled(true);
    assert(d1l_message_store_flush_if_due() == ESP_OK);
    d1l_message_entry_t restored = {0};
    assert(d1l_message_store_copy_recent(&restored, 1U) == 0U);
    assert(slot_blob(&s_sd)->epoch == 2U);
    assert(slot_blob(&s_sd)->count == 0U);
    assert(!blob_contains(slot_blob(&s_nvs), "must-stay-cleared-1"));
}

static void test_offline_public_clear_preserves_private_and_blocks_resurrection(void)
{
    reset_mock(true);
    assert(d1l_message_store_init() == ESP_OK);
    uint32_t seq = 0U;
    assert(d1l_message_store_append_channel(
               UINT64_C(1), "rx", "Public", "public-stale", -60,
               40, 1U, 1U, true, &seq) == ESP_OK && seq == 1U);
    assert(d1l_message_store_append_channel(
               UINT64_C(2), "rx", "Private", "private-keep", -61, 30,
               1U, 1U, true, &seq) == ESP_OK && seq == 2U);
    assert(blob_contains(slot_blob(&s_sd), "public-stale"));
    assert(blob_contains(slot_blob(&s_sd), "private-keep"));

    set_sd_enabled(false);
    assert(d1l_message_store_clear_channel(UINT64_C(1)) == ESP_OK);
    d1l_message_entry_t rows[D1L_MESSAGE_STORE_CAPACITY] = {0};
    assert(d1l_message_store_copy_recent(
               rows, D1L_MESSAGE_STORE_CAPACITY) == 0U);
    assert(d1l_message_store_copy_channel_recent(
               UINT64_C(2), rows, D1L_MESSAGE_STORE_CAPACITY) == 1U);
    assert(strcmp(rows[0].text, "private-keep") == 0);

    set_sd_enabled(true);
    assert(d1l_message_store_flush_if_due() == ESP_OK);
    assert(d1l_message_store_copy_recent(
               rows, D1L_MESSAGE_STORE_CAPACITY) == 0U);
    assert(d1l_message_store_copy_channel_recent(
               UINT64_C(2), rows, D1L_MESSAGE_STORE_CAPACITY) == 1U);
    assert(strcmp(rows[0].text, "private-keep") == 0);
    assert(!blob_contains(slot_blob(&s_sd), "public-stale"));
    assert(blob_contains(slot_blob(&s_sd), "private-keep"));
    assert(slot_blob(&s_sd)->clear_lineage ==
           slot_blob(&s_nvs)->clear_lineage);
}

static void test_global_clear_never_reuses_channel_sequence(void)
{
    reset_mock(true);
    assert(d1l_message_store_init() == ESP_OK);
    uint32_t seq = 0U;
    assert(d1l_message_store_append_channel(
               UINT64_C(2), "tx", "Self", "before-global-clear", 0, 0,
               1U, 0U, false, &seq) == ESP_OK && seq == 1U);
    const uint32_t next_before = d1l_message_store_stats().next_seq;
    assert(next_before == 2U);
    assert(d1l_message_store_clear() == ESP_OK);
    assert(d1l_message_store_stats().next_seq == next_before);
    assert(d1l_message_store_append_channel(
               UINT64_C(2), "rx", "Peer", "after-global-clear", -60, 40,
               1U, 1U, true, &seq) == ESP_OK && seq == next_before);
}

static void test_replacement_same_seq_conflict_is_resequenced(void)
{
    reset_mock(true);
    test_blob_t card_a = make_blob(1U, 1U, 1U, "card-a");
    test_blob_t card_b = make_blob(1U, 1U, 1U, "card-b");
    slot_write(&s_nvs, &card_a, sizeof(card_a));
    slot_write(&s_sd, &card_b, sizeof(card_b));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_flush() == ESP_OK);
    const d1l_message_store_stats_t stats = d1l_message_store_stats();
    assert(stats.count == 2U);
    assert(stats.total_written == 2U);
    assert(stats.dropped_oldest == 0U);
    assert(stats.next_seq == 3U);
    assert(!stats.sd_primary_reconcile_pending);
    assert(blob_contains(slot_blob(&s_sd), "card-a-1"));
    assert(blob_contains(slot_blob(&s_sd), "card-b-1"));
    assert(blob_contains(slot_blob(&s_nvs), "card-a-1"));
    assert(blob_contains(slot_blob(&s_nvs), "card-b-1"));
}

static void test_valid_legacy_nvs_dominates_foreign_clear_lineage(void)
{
    reset_mock(true);
    test_blob_t legacy_local = make_blob(1U, 1U, 1U, "legacy-local");
    test_blob_t cleared_card = make_blob(1U, 1U, 1U, "after-clear");
    cleared_card.clear_lineage = 0x1122334455667788ULL;
    slot_write(&s_nvs, &legacy_local, sizeof(legacy_local));
    slot_write(&s_sd, &cleared_card, sizeof(cleared_card));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_flush() == ESP_OK);
    assert(slot_blob(&s_nvs)->clear_lineage == 0U);
    assert(slot_blob(&s_sd)->clear_lineage == 0U);
    assert(blob_contains(slot_blob(&s_nvs), "legacy-local-1"));
    assert(!blob_contains(slot_blob(&s_nvs), "after-clear-1"));

    reset_mock(true);
    legacy_local = make_blob(0U, 1U, 0U, "empty-local");
    cleared_card = make_blob(1U, 1U, 1U, "foreign-over-empty");
    cleared_card.clear_lineage = 0x0102030405060708ULL;
    slot_write(&s_nvs, &legacy_local, sizeof(legacy_local));
    slot_write(&s_sd, &cleared_card, sizeof(cleared_card));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_flush() == ESP_OK);
    assert(slot_blob(&s_sd)->clear_lineage == 0U);
    assert(slot_blob(&s_sd)->count == 0U);
    assert(!blob_contains(slot_blob(&s_sd), "foreign-over-empty-1"));
}

static void test_sd_clear_lineage_can_seed_only_when_nvs_is_absent(void)
{
    reset_mock(true);
    test_blob_t cleared_card = make_blob(1U, 1U, 1U, "sd-seed");
    cleared_card.clear_lineage = 0x8877665544332211ULL;
    slot_write(&s_sd, &cleared_card, sizeof(cleared_card));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_flush() == ESP_OK);
    assert(slot_blob(&s_nvs)->clear_lineage == cleared_card.clear_lineage);
    assert(blob_contains(slot_blob(&s_nvs), "sd-seed-1"));

    reset_mock(false);
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_append_public(
               "rx", "Node", "new-local-nvs", -60, 40,
               1U, 1U, true) == ESP_OK);
    cleared_card = make_blob(1U, 1U, 1U, "must-not-replace-local");
    cleared_card.epoch = UINT32_MAX;
    cleared_card.clear_lineage = 0xCAFEBABEDEADBEEFULL;
    slot_write(&s_sd, &cleared_card, sizeof(cleared_card));
    set_sd_enabled(true);
    assert(d1l_message_store_flush_if_due() == ESP_OK);
    assert(slot_blob(&s_sd)->clear_lineage == 0U);
    assert(blob_contains(slot_blob(&s_sd), "new-local-nvs"));
    assert(!blob_contains(slot_blob(&s_sd), "must-not-replace-local-1"));
}

static void test_same_epoch_late_card_preserves_baseline_and_live_rows(void)
{
    reset_mock(false);
    test_blob_t baseline = make_blob(1U, 1U, 1U, "baseline");
    test_blob_t replacement = make_blob(3U, 1U, 3U, "replacement");
    slot_write(&s_nvs, &baseline, sizeof(baseline));
    slot_write(&s_sd, &replacement, sizeof(replacement));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_append_public(
               "rx", "Node", "live-local", -60, 40, 1U, 1U, true) ==
           ESP_OK);

    set_sd_enabled(true);
    assert(d1l_message_store_flush_if_due() == ESP_OK);
    const test_blob_t *committed = slot_blob(&s_sd);
    assert(committed->count == 5U);
    assert(blob_contains(committed, "baseline-1"));
    assert(blob_contains(committed, "replacement-1"));
    assert(blob_contains(committed, "replacement-2"));
    assert(blob_contains(committed, "replacement-3"));
    assert(blob_contains(committed, "live-local"));
    assert(blob_contains(slot_blob(&s_nvs), "baseline-1"));
    assert(blob_contains(slot_blob(&s_nvs), "live-local"));
}

static void test_empty_same_epoch_late_card_does_not_double_count_live_rows(void)
{
    reset_mock(false);
    assert(d1l_message_store_init() == ESP_OK);
    char text[32];
    for (uint32_t i = 1U; i <= 20U; ++i) {
        (void)snprintf(text, sizeof(text), "offline-%lu",
                       (unsigned long)i);
        assert(d1l_message_store_append_public(
                   "rx", "Node", text, -60, 40, 1U, 1U, true) == ESP_OK);
    }
    d1l_message_store_stats_t stats = d1l_message_store_stats();
    assert(stats.total_written == 20U);
    assert(stats.dropped_oldest == 4U);
    assert(stats.count == D1L_MESSAGE_STORE_CAPACITY);

    test_blob_t empty = make_blob(0U, 1U, 0U, "empty");
    slot_write(&s_sd, &empty, sizeof(empty));
    set_sd_enabled(true);
    assert(d1l_message_store_flush_if_due() == ESP_OK);
    stats = d1l_message_store_stats();
    assert(stats.total_written == 20U);
    assert(stats.dropped_oldest == 4U);
    assert(stats.count == D1L_MESSAGE_STORE_CAPACITY);
    assert(blob_contains(slot_blob(&s_sd), "offline-20"));
}

static void test_rebooted_local_clear_lineage_dominates_foreign_card(void)
{
    reset_mock(false);
    test_blob_t old = make_blob(1U, 1U, 1U, "pre-clear");
    slot_write(&s_nvs, &old, sizeof(old));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_clear() == ESP_OK);
    assert(d1l_message_store_append_public(
               "rx", "Node", "post-clear-local", -60, 40,
               1U, 1U, true) == ESP_OK);
    const uint64_t local_lineage = slot_blob(&s_nvs)->clear_lineage;
    assert(local_lineage != 0U);

    /* Reboot before insertion proves authority comes from persisted NVS, not
     * transient mutation tracking. */
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_stats().clear_lineage == local_lineage);

    test_blob_t foreign = make_blob(1U, 1U, 1U, "same-epoch-card");
    foreign.epoch = d1l_message_store_stats().epoch;
    foreign.clear_lineage = 0xA5A5A5A55A5A5A5AULL;
    slot_write(&s_sd, &foreign, sizeof(foreign));
    set_sd_enabled(true);
    assert(d1l_message_store_flush_if_due() == ESP_OK);
    assert(slot_blob(&s_sd)->clear_lineage == local_lineage);
    assert(slot_blob(&s_nvs)->clear_lineage == local_lineage);
    assert(!blob_contains(slot_blob(&s_sd), "same-epoch-card-1"));
    assert(!blob_contains(slot_blob(&s_nvs), "same-epoch-card-1"));
    assert(blob_contains(slot_blob(&s_sd), "post-clear-local"));
    assert(blob_contains(slot_blob(&s_nvs), "post-clear-local"));

    foreign = make_blob(1U, 1U, 1U, "higher-epoch-card");
    foreign.epoch = UINT32_MAX;
    foreign.clear_lineage = 0x5A5A5A5AA5A5A5A5ULL;
    slot_write(&s_sd, &foreign, sizeof(foreign));
    set_sd_enabled(false);
    set_sd_enabled(true);
    assert(d1l_message_store_flush_if_due() == ESP_OK);
    assert(slot_blob(&s_sd)->clear_lineage == local_lineage);
    assert(!blob_contains(slot_blob(&s_sd), "higher-epoch-card-1"));
    assert(blob_contains(slot_blob(&s_sd), "post-clear-local"));
}

static void test_exhausted_sequence_rejects_append_and_clear_rotates_lineage(void)
{
    reset_mock(true);
    test_blob_t exhausted = make_blob(1U, 1U, 1U, "exhausted");
    exhausted.next_seq = UINT32_MAX;
    slot_write(&s_nvs, &exhausted, sizeof(exhausted));
    slot_write(&s_sd, &exhausted, sizeof(exhausted));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_append_public(
               "rx", "Node", "must-not-wrap", -60, 40,
               1U, 1U, true) == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U && s_nvs_write_count == 0U);
    assert(slot_blob(&s_sd)->next_seq == UINT32_MAX);
    assert(slot_blob(&s_nvs)->next_seq == UINT32_MAX);

    reset_mock(true);
    exhausted = make_blob(1U, 1U, 1U, "max-epoch");
    exhausted.epoch = UINT32_MAX;
    slot_write(&s_nvs, &exhausted, sizeof(exhausted));
    slot_write(&s_sd, &exhausted, sizeof(exhausted));
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_clear() == ESP_OK);
    assert(slot_blob(&s_sd)->epoch == 1U);
    assert(slot_blob(&s_sd)->clear_lineage != 0U);
    assert(slot_blob(&s_sd)->count == 0U);
    assert(slot_blob(&s_nvs)->clear_lineage ==
           slot_blob(&s_sd)->clear_lineage);
}

static void test_higher_epoch_replay_exhaustion_preserves_both_copies(void)
{
    reset_mock(false);
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_append_public(
               "rx", "Node", "offline-local", -60, 40,
               1U, 1U, true) == ESP_OK);

    test_blob_t higher = make_blob(1U, 1U, 1U, "higher-max-seq");
    higher.epoch = 2U;
    higher.next_seq = UINT32_MAX;
    slot_write(&s_sd, &higher, sizeof(higher));
    const test_slot_t sd_before = s_sd;
    const test_slot_t nvs_before = s_nvs;
    set_sd_enabled(true);
    s_sd_write_count = 0U;
    s_nvs_write_count = 0U;

    assert(d1l_message_store_flush() == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U && s_nvs_write_count == 0U);
    assert(memcmp(&s_sd, &sd_before, sizeof(s_sd)) == 0);
    assert(memcmp(&s_nvs, &nvs_before, sizeof(s_nvs)) == 0);
    assert(d1l_message_store_stats().sd_primary_reconcile_pending);
}

static void test_total_counter_exhaustion_rejects_append_without_writes(void)
{
    reset_mock(true);
    test_blob_t exhausted = make_blob(1U, 1U, 1U, "max-total");
    exhausted.total_written = UINT32_MAX;
    exhausted.dropped_oldest = UINT32_MAX - 1U;
    exhausted.content_revision = UINT32_MAX;
    exhausted.next_seq = 2U;
    slot_write(&s_nvs, &exhausted, sizeof(exhausted));
    slot_write(&s_sd, &exhausted, sizeof(exhausted));
    assert(d1l_message_store_init() == ESP_OK);
    s_sd_write_count = 0U;
    s_nvs_write_count = 0U;
    assert(d1l_message_store_append_public(
               "rx", "Node", "must-not-overflow-total", -60, 40,
               1U, 1U, true) == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U && s_nvs_write_count == 0U);
    assert(slot_blob(&s_sd)->total_written == UINT32_MAX);
    assert(slot_blob(&s_nvs)->total_written == UINT32_MAX);
}

static void test_utf8_round_trip_and_invalid_input_have_no_side_effects(void)
{
    reset_mock(false);
    assert(d1l_message_store_init() == ESP_OK);
    const char mixed[] = {
        'C', 'a', 'f', (char)0xc3, (char)0xa9, ' ',
        (char)0xe6, (char)0x9d, (char)0xb1,
        (char)0xe4, (char)0xba, (char)0xac, ' ',
        (char)0xf0, (char)0x9f, (char)0x98, (char)0x80,
        ' ', '"', 'x', '\\', 'y', '"', '\0'
    };
    assert(d1l_message_store_append_public(
               "rx", "Node", mixed, -60, 40, 1U, 1U, true) == ESP_OK);

    char exact[D1L_MESSAGE_TEXT_LEN] = {0};
    for (size_t i = 0U; i < D1L_MESSAGE_MAX_BYTES; i += 3U) {
        exact[i] = (char)0xe2;
        exact[i + 1U] = (char)0x82;
        exact[i + 2U] = (char)0xac;
    }
    assert(d1l_message_store_append_public(
               "tx", "Node", exact, 0, 0, 1U, 0U, false) == ESP_OK);

    const d1l_message_store_stats_t before = d1l_message_store_stats();
    const uint32_t nvs_writes_before = s_nvs_write_count;
    const char invalid[] = {(char)0xc0, (char)0xaf, '\0'};
    assert(d1l_message_store_append_public(
               "rx", "Node", invalid, -60, 40, 1U, 1U, true) ==
           ESP_ERR_INVALID_ARG);
    char oversized[D1L_MESSAGE_TEXT_LEN + 1U];
    memset(oversized, 'q', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';
    assert(d1l_message_store_append_public(
               "rx", "Node", oversized, -60, 40, 1U, 1U, true) ==
           ESP_ERR_INVALID_SIZE);
    const d1l_message_store_stats_t after = d1l_message_store_stats();
    assert(after.count == before.count);
    assert(after.total_written == before.total_written);
    assert(s_nvs_write_count == nvs_writes_before);

    assert(d1l_message_store_init() == ESP_OK);
    d1l_message_entry_t rows[2] = {0};
    assert(d1l_message_store_copy_recent(rows, 2U) == 2U);
    const bool first_is_exact = strcmp(rows[0].text, exact) == 0;
    assert(first_is_exact || strcmp(rows[1].text, exact) == 0);
    assert(memcmp(rows[first_is_exact ? 1U : 0U].text,
                  mixed, sizeof(mixed)) == 0);
}

static void test_deferred_append_is_zero_wait_visible_and_worker_owned(void)
{
    reset_mock(false);
    assert(d1l_message_store_init() == ESP_OK);
    s_sd_write_count = 0U;
    s_nvs_write_count = 0U;

    const d1l_message_store_stats_t before = d1l_message_store_stats();
    uint32_t message_seq = UINT32_MAX;
    s_fail_next_zero_wait_take = true;
    assert(d1l_message_store_append_channel_deferred(
               UINT64_C(1), "tx", "D1L", "deferred-public",
               0, 0, 1U, 0U, false, &message_seq) == ESP_ERR_TIMEOUT);
    assert(message_seq == 0U);
    d1l_message_store_stats_t after = d1l_message_store_stats();
    assert(after.count == before.count);
    assert(after.total_written == before.total_written);
    assert(s_sd_write_count == 0U);
    assert(s_nvs_write_count == 0U);

    assert(d1l_message_store_append_channel_deferred(
               UINT64_C(1), "tx", "D1L", "deferred-public",
               0, 0, 1U, 0U, false, &message_seq) == ESP_OK);
    assert(message_seq != 0U);
    after = d1l_message_store_stats();
    assert(after.count == before.count + 1U);
    assert(after.total_written == before.total_written + 1U);
    assert(after.persistence_dirty);
    assert(after.nvs_fallback_dirty);
    assert(s_sd_write_count == 0U);
    assert(s_nvs_write_count == 0U);

    d1l_message_entry_t visible = {0};
    assert(d1l_message_store_copy_recent(&visible, 1U) == 1U);
    assert(visible.seq == message_seq);
    assert(visible.channel_id == UINT64_C(1));
    assert(strcmp(visible.direction, "tx") == 0);
    assert(strcmp(visible.author, "D1L") == 0);
    assert(strcmp(visible.text, "deferred-public") == 0);

    assert(d1l_message_store_flush() == ESP_OK);
    assert(s_sd_write_count == 0U);
    assert(s_nvs_write_count == 1U);
    assert(!d1l_message_store_stats().persistence_dirty);
    assert(blob_contains(slot_blob(&s_nvs), "deferred-public"));
}

int main(void)
{
    test_sd_success_nvs_failure_retries_only_nvs();
    test_late_sd_merges_live_overlay_before_write();
    test_unsampled_replacement_generation_is_reconciled();
    test_read_failure_never_overwrites_sd_but_nvs_advances();
    test_generation_edge_during_write_preserves_replacement();
    test_generation_flip_after_merge_rolls_back_before_nvs_write();
    test_init_generation_flip_restores_nvs_before_loaded_commit();
    test_corrupt_sd_stays_pending_while_valid_nvs_advances();
    test_corrupt_sources_are_never_erased_on_init();
    test_periodic_flush_retries_transient_failed_initialization();
    test_v1_fallback_remains_readable_and_rewrites_as_v5();
    test_v2_fallback_migrates_to_v5_without_lineage();
    test_v3_fallback_migrates_to_v5_without_inventing_clear();
    test_v4_fallback_migrates_to_v5_public_identity();
    test_mixed_channel_history_isolation_and_public_only_clear();
    test_offline_clear_tombstone_blocks_sd_resurrection();
    test_offline_public_clear_preserves_private_and_blocks_resurrection();
    test_global_clear_never_reuses_channel_sequence();
    test_replacement_same_seq_conflict_is_resequenced();
    test_valid_legacy_nvs_dominates_foreign_clear_lineage();
    test_sd_clear_lineage_can_seed_only_when_nvs_is_absent();
    test_same_epoch_late_card_preserves_baseline_and_live_rows();
    test_empty_same_epoch_late_card_does_not_double_count_live_rows();
    test_rebooted_local_clear_lineage_dominates_foreign_card();
    test_exhausted_sequence_rejects_append_and_clear_rotates_lineage();
    test_higher_epoch_replay_exhaustion_preserves_both_copies();
    test_total_counter_exhaustion_rejects_append_without_writes();
    test_utf8_round_trip_and_invalid_input_have_no_side_effects();
    test_deferred_append_is_zero_wait_visible_and_worker_owned();
    puts("native Public message-store durability: ok");
    return 0;
}
