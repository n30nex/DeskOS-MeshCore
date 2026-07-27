#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/semphr.h"
#include "mesh/dm_store.h"
#include "storage/retained_blob_store.h"

#define MOCK_BLOB_MAX 8192U
#define TEST_LEGACY_CONTACT_ALIAS_LEN 24U
#define TEST_SCHEMA_V7 7U
#define TEST_SCHEMA_V6 6U
#define TEST_SCHEMA_V5 5U
#define TEST_SCHEMA_V4 4U
#define TEST_SCHEMA_V3 3U
#define TEST_SCHEMA_V2 2U

typedef struct {
    bool valid;
    size_t len;
    uint8_t data[MOCK_BLOB_MAX];
} mock_blob_t;

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
    d1l_dm_entry_t entries[D1L_DM_STORE_CAPACITY];
} test_blob_v6_t;

typedef test_blob_v6_t test_blob_v7_t;
typedef test_blob_v6_t test_blob_v6_wide_t;

typedef struct {
    uint32_t seq;
    uint64_t delivery_session_id;
    uint32_t uptime_ms;
    char contact_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char contact_alias[TEST_LEGACY_CONTACT_ALIAS_LEN];
    char direction[D1L_DM_DIRECTION_LEN];
    char text[D1L_MESSAGE_TEXT_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t attempt;
    bool delivered;
    bool acked;
    uint32_t ack_hash;
    uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    uint8_t ack_dispatch_count;
    uint8_t ack_dispatch_kind;
    d1l_dm_ack_state_t ack_state;
    esp_err_t ack_last_error;
    bool identity_digest_valid;
    d1l_dm_delivery_state_t delivery_state;
    d1l_dm_delivery_reason_t delivery_reason;
    esp_err_t delivery_last_error;
    uint8_t delivery_retry_count;
    uint32_t delivery_revision;
} test_entry_v6_narrow_t;

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
    test_entry_v6_narrow_t entries[D1L_DM_STORE_CAPACITY];
} test_blob_v6_narrow_t;

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    char contact_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char contact_alias[TEST_LEGACY_CONTACT_ALIAS_LEN];
    char direction[D1L_DM_DIRECTION_LEN];
    char text[D1L_MESSAGE_TEXT_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t attempt;
    bool delivered;
    bool acked;
    uint32_t ack_hash;
    uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    uint8_t ack_dispatch_count;
    uint8_t ack_dispatch_kind;
    d1l_dm_ack_state_t ack_state;
    esp_err_t ack_last_error;
    bool identity_digest_valid;
} test_entry_v5_t;

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
    test_entry_v5_t entries[D1L_DM_STORE_CAPACITY];
} test_blob_v5_t;

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    char contact_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char contact_alias[TEST_LEGACY_CONTACT_ALIAS_LEN];
    char direction[D1L_DM_DIRECTION_LEN];
    char text[D1L_MESSAGE_TEXT_LEN];
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hash_bytes;
    uint8_t path_hops;
    uint8_t attempt;
    bool delivered;
    bool acked;
    uint32_t ack_hash;
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
    test_entry_v4_t entries[D1L_DM_STORE_CAPACITY];
} test_blob_v4_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    uint32_t epoch;
    uint32_t content_revision;
    test_entry_v4_t entries[D1L_DM_STORE_CAPACITY];
} test_blob_v3_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    test_entry_v4_t entries[D1L_DM_STORE_CAPACITY];
} test_blob_v2_t;

typedef enum {
    OP_READ_SD = 1,
    OP_WRITE_SD,
    OP_WRITE_NVS,
} mock_operation_t;

static mock_blob_t s_sd;
static mock_blob_t s_nvs;
static bool s_sd_enabled;
static uint32_t s_backend_generation;
static uint32_t s_backend_state_count;
static uint32_t s_flip_generation_on_backend_state_call;
static esp_err_t s_sd_read_error;
static esp_err_t s_sd_write_error;
static esp_err_t s_nvs_write_error;
static bool s_fail_nvs_once;
static bool s_change_generation_during_read;
static bool s_change_generation_during_write;
static uint32_t s_sd_read_count;
static uint32_t s_sd_write_count;
static uint32_t s_nvs_write_count;
static mock_operation_t s_operations[64];
static size_t s_operation_count;
static int64_t s_now_us;
static uint32_t s_random_state;

static void reset_backend(void)
{
    memset(&s_sd, 0, sizeof(s_sd));
    memset(&s_nvs, 0, sizeof(s_nvs));
    s_sd_enabled = false;
    s_backend_generation = 0U;
    s_backend_state_count = 0U;
    s_flip_generation_on_backend_state_call = 0U;
    s_sd_read_error = ESP_OK;
    s_sd_write_error = ESP_OK;
    s_nvs_write_error = ESP_OK;
    s_fail_nvs_once = false;
    s_change_generation_during_read = false;
    s_change_generation_during_write = false;
    s_sd_read_count = 0U;
    s_sd_write_count = 0U;
    s_nvs_write_count = 0U;
    memset(s_operations, 0, sizeof(s_operations));
    s_operation_count = 0U;
    s_now_us = 1000000LL;
    s_random_state = 0x2468ACE1U;
}

static void note_operation(mock_operation_t operation)
{
    assert(s_operation_count < sizeof(s_operations) / sizeof(s_operations[0]));
    s_operations[s_operation_count++] = operation;
}

static esp_err_t blob_read(const mock_blob_t *blob, void *dst,
                           size_t *len_inout)
{
    if (!blob->valid) {
        return ESP_ERR_NOT_FOUND;
    }
    if (*len_inout < blob->len) {
        *len_inout = blob->len;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, blob->data, blob->len);
    *len_inout = blob->len;
    return ESP_OK;
}

static esp_err_t blob_write(mock_blob_t *blob, const void *src, size_t len)
{
    assert(src != NULL);
    assert(len > 0U && len <= sizeof(blob->data));
    memcpy(blob->data, src, len);
    blob->len = len;
    blob->valid = true;
    return ESP_OK;
}

static d1l_dm_entry_t make_entry(uint32_t seq, const char *text,
                                 const char *direction, uint32_t ack_hash)
{
    d1l_dm_entry_t entry = {
        .seq = seq,
        .uptime_ms = seq * 100U,
        .rssi_dbm = -60,
        .snr_tenths = 40,
        .path_hash_bytes = 1U,
        .path_hops = 1U,
        .attempt = 1U,
        .delivered = true,
        .acked = false,
        .ack_hash = ack_hash,
    };
    (void)snprintf(entry.contact_fingerprint,
                   sizeof(entry.contact_fingerprint),
                   "0123456789abcdef");
    (void)snprintf(entry.contact_alias, sizeof(entry.contact_alias), "Node");
    (void)snprintf(entry.direction, sizeof(entry.direction), "%s", direction);
    (void)snprintf(entry.text, sizeof(entry.text), "%s", text);
    if (strcmp(direction, "tx") == 0) {
        entry.delivery_session_id =
            UINT64_C(0xd100000000000000) | (uint64_t)seq;
        entry.delivery_state = D1L_DM_DELIVERY_QUEUED;
        entry.delivery_reason = D1L_DM_DELIVERY_REASON_ENQUEUED;
        entry.delivery_revision = 1U;
        entry.delivered = false;
    } else {
        entry.delivery_state = D1L_DM_DELIVERY_NOT_APPLICABLE;
        entry.delivery_reason = D1L_DM_DELIVERY_REASON_NONE;
        entry.delivery_revision = 0U;
    }
    return entry;
}

static test_entry_v4_t make_legacy_entry(uint32_t seq, const char *text,
                                         const char *direction,
                                         uint32_t ack_hash)
{
    const d1l_dm_entry_t current = make_entry(seq, text, direction, ack_hash);
    test_entry_v4_t legacy = {
        .seq = current.seq,
        .uptime_ms = current.uptime_ms,
        .rssi_dbm = current.rssi_dbm,
        .snr_tenths = current.snr_tenths,
        .path_hash_bytes = current.path_hash_bytes,
        .path_hops = current.path_hops,
        .attempt = current.attempt,
        .delivered = current.delivered,
        .acked = current.acked,
        .ack_hash = current.ack_hash,
    };
    memcpy(legacy.contact_fingerprint, current.contact_fingerprint,
           sizeof(legacy.contact_fingerprint));
    memcpy(legacy.contact_alias, current.contact_alias,
           sizeof(legacy.contact_alias));
    memcpy(legacy.direction, current.direction, sizeof(legacy.direction));
    memcpy(legacy.text, current.text, sizeof(legacy.text));
    return legacy;
}

static test_entry_v5_t make_v5_entry(uint32_t seq, const char *text,
                                     const char *direction,
                                     uint32_t ack_hash, bool acked)
{
    const d1l_dm_entry_t current =
        make_entry(seq, text, direction, ack_hash);
    test_entry_v5_t legacy = {
        .seq = current.seq,
        .uptime_ms = current.uptime_ms,
        .rssi_dbm = current.rssi_dbm,
        .snr_tenths = current.snr_tenths,
        .path_hash_bytes = current.path_hash_bytes,
        .path_hops = current.path_hops,
        .attempt = current.attempt,
        .delivered = true,
        .acked = acked,
        .ack_hash = current.ack_hash,
        .ack_state = D1L_DM_ACK_STATE_LEGACY_UNVERIFIED,
        .ack_last_error = ESP_OK,
    };
    memcpy(legacy.contact_fingerprint, current.contact_fingerprint,
           sizeof(legacy.contact_fingerprint));
    memcpy(legacy.contact_alias, current.contact_alias,
           sizeof(legacy.contact_alias));
    memcpy(legacy.direction, current.direction, sizeof(legacy.direction));
    memcpy(legacy.text, current.text, sizeof(legacy.text));
    return legacy;
}

static test_entry_v6_narrow_t make_v6_narrow_entry(
    const d1l_dm_entry_t *current)
{
    assert(current != NULL);
    test_entry_v6_narrow_t narrow = {
        .seq = current->seq,
        .delivery_session_id = current->delivery_session_id,
        .uptime_ms = current->uptime_ms,
        .rssi_dbm = current->rssi_dbm,
        .snr_tenths = current->snr_tenths,
        .path_hash_bytes = current->path_hash_bytes,
        .path_hops = current->path_hops,
        .attempt = current->attempt,
        .delivered = current->delivered,
        .acked = current->acked,
        .ack_hash = current->ack_hash,
        .ack_dispatch_count = current->ack_dispatch_count,
        .ack_dispatch_kind = current->ack_dispatch_kind,
        .ack_state = current->ack_state,
        .ack_last_error = current->ack_last_error,
        .identity_digest_valid = current->identity_digest_valid,
        .delivery_state = current->delivery_state,
        .delivery_reason = current->delivery_reason,
        .delivery_last_error = current->delivery_last_error,
        .delivery_retry_count = current->delivery_retry_count,
        .delivery_revision = current->delivery_revision,
    };
    memcpy(narrow.contact_fingerprint, current->contact_fingerprint,
           sizeof(narrow.contact_fingerprint));
    memcpy(narrow.contact_alias, current->contact_alias,
           sizeof(narrow.contact_alias));
    memcpy(narrow.direction, current->direction, sizeof(narrow.direction));
    memcpy(narrow.text, current->text, sizeof(narrow.text));
    memcpy(narrow.identity_digest, current->identity_digest,
           sizeof(narrow.identity_digest));
    return narrow;
}

static test_blob_v6_t make_v3(uint32_t epoch, uint32_t revision,
                               const char *text)
{
    test_blob_v6_t blob = {
        .schema = TEST_SCHEMA_V7,
        .epoch = epoch,
        .content_revision = revision,
        .next_seq = text ? 2U : 1U,
        .total_written = text ? 1U : 0U,
        .head = text ? 1U : 0U,
        .count = text ? 1U : 0U,
    };
    if (text) {
        blob.entries[0] = make_entry(1U, text, "rx", 0U);
    }
    return blob;
}

static void seed_v3(mock_blob_t *dest, uint32_t epoch, uint32_t revision,
                    const char *text)
{
    const test_blob_v6_t blob = make_v3(epoch, revision, text);
    assert(blob_write(dest, &blob, sizeof(blob)) == ESP_OK);
}

static void seed_full_v3(mock_blob_t *dest, const char *prefix)
{
    test_blob_v6_t blob = {
        .schema = TEST_SCHEMA_V7,
        .epoch = 1U,
        .content_revision = 17U,
        .next_seq = D1L_DM_STORE_CAPACITY + 1U,
        .total_written = D1L_DM_STORE_CAPACITY,
        .head = 0U,
        .count = D1L_DM_STORE_CAPACITY,
    };
    char text[32];
    for (uint32_t i = 0U; i < D1L_DM_STORE_CAPACITY; ++i) {
        (void)snprintf(text, sizeof(text), "%s-%lu", prefix,
                       (unsigned long)(i + 1U));
        blob.entries[i] = make_entry(i + 1U, text, "rx", i + 1U);
    }
    assert(blob_write(dest, &blob, sizeof(blob)) == ESP_OK);
}

static void seed_range_v3(mock_blob_t *dest, uint32_t total_written,
                          uint32_t first_seq, uint32_t count,
                          const char *prefix)
{
    assert(count <= D1L_DM_STORE_CAPACITY);
    test_blob_v6_t blob = {
        .schema = TEST_SCHEMA_V7,
        .epoch = 1U,
        .content_revision = total_written + 1U,
        .next_seq = total_written + 1U,
        .total_written = total_written,
        .dropped_oldest = total_written - count,
        .head = count % D1L_DM_STORE_CAPACITY,
        .count = count,
    };
    char text[32];
    for (uint32_t i = 0U; i < count; ++i) {
        (void)snprintf(text, sizeof(text), "%s-%lu", prefix,
                       (unsigned long)(first_seq + i));
        blob.entries[i] = make_entry(first_seq + i, text, "rx",
                                     first_seq + i);
    }
    assert(blob_write(dest, &blob, sizeof(blob)) == ESP_OK);
}

static void fill_identity_digest(
    uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES], uint8_t value)
{
    memset(digest, value, D1L_DM_IDENTITY_DIGEST_BYTES);
}

static void set_ack_metadata(d1l_dm_entry_t *entry, uint8_t digest_value,
                             d1l_dm_ack_state_t state, uint8_t count,
                             uint8_t kind, esp_err_t error)
{
    assert(entry != NULL);
    fill_identity_digest(entry->identity_digest, digest_value);
    entry->identity_digest_valid = true;
    entry->ack_state = state;
    entry->ack_dispatch_count = count;
    entry->ack_dispatch_kind = kind;
    entry->ack_last_error = error;
}

static void seed_ack_state(mock_blob_t *dest, uint32_t revision,
                           d1l_dm_ack_state_t state, uint8_t count,
                           esp_err_t error)
{
    test_blob_v6_t blob = make_v3(1U, revision, "ack-row");
    set_ack_metadata(&blob.entries[0], 0xA5U, state, count,
                     count == 0U ? 0U : 1U, error);
    assert(blob_write(dest, &blob, sizeof(blob)) == ESP_OK);
}

static bool blob_contains(const mock_blob_t *blob, const char *text)
{
    const size_t text_len = strlen(text);
    if (!blob->valid || text_len > blob->len) {
        return false;
    }
    for (size_t i = 0U; i + text_len <= blob->len; ++i) {
        if (memcmp(blob->data + i, text, text_len) == 0) {
            return true;
        }
    }
    return false;
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
    (void)ticks_to_wait;
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
    assert(store_id == D1L_RETAINED_BLOB_STORE_DM_MESSAGES);
    assert(out_state != NULL);
    s_backend_state_count++;
    if (s_flip_generation_on_backend_state_call == s_backend_state_count) {
        s_backend_generation++;
    }
    out_state->enabled = s_sd_enabled;
    out_state->generation = s_backend_generation;
    return true;
}

esp_err_t d1l_retained_blob_store_read_sd_primary(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_DM_MESSAGES);
    assert(strcmp(key, "threads") == 0);
    assert(s_sd_enabled);
    s_sd_read_count++;
    note_operation(OP_READ_SD);
    if (s_change_generation_during_read) {
        s_change_generation_during_read = false;
        s_backend_generation++;
    }
    return s_sd_read_error == ESP_OK ?
        blob_read(&s_sd, dst, len_inout) : s_sd_read_error;
}

esp_err_t d1l_retained_blob_store_read_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_DM_MESSAGES);
    assert(strcmp(key, "threads") == 0);
    return blob_read(&s_nvs, dst, len_inout);
}

esp_err_t d1l_retained_blob_store_write_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len, uint32_t expected_generation)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_DM_MESSAGES);
    assert(strcmp(key, "threads") == 0);
    s_sd_write_count++;
    note_operation(OP_WRITE_SD);
    if (!s_sd_enabled || expected_generation != s_backend_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = s_sd_write_error == ESP_OK ?
        blob_write(&s_sd, src, len) : s_sd_write_error;
    if (s_change_generation_during_write) {
        s_change_generation_during_write = false;
        s_backend_generation++;
    }
    return ret;
}

esp_err_t d1l_retained_blob_store_write_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_DM_MESSAGES);
    assert(strcmp(key, "threads") == 0);
    s_nvs_write_count++;
    note_operation(OP_WRITE_NVS);
    if (s_fail_nvs_once) {
        s_fail_nvs_once = false;
        return ESP_FAIL;
    }
    return s_nvs_write_error == ESP_OK ?
        blob_write(&s_nvs, src, len) : s_nvs_write_error;
}

esp_err_t d1l_retained_blob_store_erase_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_DM_MESSAGES);
    assert(strcmp(key, "threads") == 0);
    memset(&s_nvs, 0, sizeof(s_nvs));
    return ESP_OK;
}

static void test_late_ready_merges_before_primary_write(void)
{
    reset_backend();
    seed_v3(&s_nvs, 1U, 2U, "nvs-row");
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "live-row", -50, 50, 1U, 1U, 1U,
                               true, false, 0U) == ESP_OK);
    assert(s_sd_write_count == 0U);

    seed_v3(&s_sd, 1U, 2U, "sd-row");
    s_sd_enabled = true;
    s_backend_generation = 1U;
    s_operation_count = 0U;
    assert(d1l_dm_store_flush_if_due() == ESP_OK);
    assert(s_sd_read_count == 1U);
    assert(s_sd_write_count == 1U);
    assert(s_operation_count >= 2U);
    assert(s_operations[0] == OP_READ_SD);
    assert(s_operations[1] == OP_WRITE_SD);
    assert(blob_contains(&s_sd, "sd-row"));
    assert(blob_contains(&s_sd, "nvs-row"));
    assert(blob_contains(&s_sd, "live-row"));
    assert(blob_contains(&s_nvs, "sd-row"));
    assert(!d1l_dm_store_stats().persistence_dirty);
}

static void test_replacement_generation_forces_read_before_write(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 3U;
    seed_v3(&s_sd, 1U, 2U, "card-a");
    seed_v3(&s_nvs, 1U, 2U, "card-a");
    assert(d1l_dm_store_init() == ESP_OK);

    seed_v3(&s_sd, 1U, 4U, "card-b");
    s_backend_generation = 5U;
    s_operation_count = 0U;
    assert(d1l_dm_store_flush_if_due() == ESP_OK);
    assert(s_operations[0] == OP_READ_SD);
    assert(blob_contains(&s_sd, "card-b"));
    assert(blob_contains(&s_nvs, "card-b"));
    assert(d1l_dm_store_stats().sd_backend_generation == 5U);
}

static void test_nvs_failure_after_sd_success_stays_retryable(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_init() == ESP_OK);
    s_fail_nvs_once = true;
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "tx",
                               "retry-row", -50, 50, 1U, 1U, 1U,
                               false, false, 0x1234U) == ESP_FAIL);
    d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    assert(!stats.sd_primary_dirty);
    assert(stats.nvs_fallback_dirty);
    assert(stats.persistence_dirty);
    assert(stats.sd_primary_commit_count == 1U);
    assert(stats.nvs_fallback_fail_count == 1U);
    const uint32_t sd_writes = s_sd_write_count;
    const uint32_t nvs_writes = s_nvs_write_count;
    assert(d1l_dm_store_flush() == ESP_FAIL);
    assert(s_sd_write_count == sd_writes);
    assert(s_nvs_write_count == nvs_writes);

    s_now_us += (int64_t)D1L_DM_STORE_PERSIST_RETRY_INTERVAL_MS * 1000LL;
    assert(d1l_dm_store_flush_if_due() == ESP_OK);
    assert(s_sd_write_count == sd_writes);
    assert(s_nvs_write_count == nvs_writes + 1U);
    stats = d1l_dm_store_stats();
    assert(!stats.persistence_dirty);
    assert(!stats.nvs_fallback_dirty);
    assert(blob_contains(&s_nvs, "retry-row"));
}

static void test_read_failure_never_writes_sd_and_nvs_can_advance(void)
{
    reset_backend();
    seed_v3(&s_nvs, 1U, 2U, "fallback-row");
    assert(d1l_dm_store_init() == ESP_OK);
    s_sd_enabled = true;
    s_backend_generation = 1U;
    s_sd_read_error = ESP_FAIL;
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "new-fallback", -50, 50, 1U, 1U, 1U,
                               true, false, 0U) == ESP_FAIL);
    assert(s_sd_read_count == 1U);
    assert(s_sd_write_count == 0U);
    assert(blob_contains(&s_nvs, "new-fallback"));
    const d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    assert(stats.sd_primary_reconcile_pending);
    assert(stats.persistence_dirty);
    assert(!stats.nvs_fallback_dirty);
}

static void test_corruption_and_generation_change_fail_closed(void)
{
    reset_backend();
    seed_v3(&s_nvs, 1U, 2U, "fallback-row");
    assert(d1l_dm_store_init() == ESP_OK);
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_v3(&s_sd, 1U, 2U, "corrupt-row");
    ((test_blob_v6_t *)s_sd.data)->next_seq = 1U;
    assert(d1l_dm_store_flush_if_due() == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U);
    assert(d1l_dm_store_stats().sd_primary_reconcile_pending);

    s_now_us += (int64_t)D1L_DM_STORE_PERSIST_RETRY_INTERVAL_MS * 1000LL;
    seed_v3(&s_sd, 1U, 2U, "replacement-row");
    s_change_generation_during_read = true;
    assert(d1l_dm_store_flush_if_due() == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U);
    assert(d1l_dm_store_stats().sd_primary_reconcile_pending);
}

static void test_generation_change_after_guarded_write_stays_pending(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_init() == ESP_OK);
    s_change_generation_during_write = true;
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "write-edge", -50, 50, 1U, 1U, 1U,
                               true, false, 0U) == ESP_ERR_INVALID_STATE);
    const d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    assert(stats.sd_primary_dirty);
    assert(stats.sd_primary_reconcile_pending);
    assert(stats.persistence_dirty);
}

static void test_generation_flip_after_merge_rolls_back_before_nvs_write(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "local-before-swap", -50, 50,
                               1U, 1U, 1U, true, false, 0U) == ESP_OK);
    seed_v3(&s_sd, 7U, 2U, "stale-card-a");
    s_sd_enabled = true;
    s_backend_generation = 1U;
    const mock_blob_t nvs_before = s_nvs;
    s_backend_state_count = 0U;
    s_flip_generation_on_backend_state_call = 4U;
    s_sd_write_count = 0U;
    s_nvs_write_count = 0U;

    assert(d1l_dm_store_flush() == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U && s_nvs_write_count == 0U);
    assert(memcmp(&s_nvs, &nvs_before, sizeof(s_nvs)) == 0);
    assert(blob_contains(&s_nvs, "local-before-swap"));
    assert(!blob_contains(&s_nvs, "stale-card-a"));
    assert(d1l_dm_store_stats().sd_primary_reconcile_pending);
}

static void test_init_generation_flip_restores_nvs_before_loaded_commit(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_v3(&s_nvs, 1U, 2U, "init-local");
    seed_v3(&s_sd, 9U, 2U, "init-stale-card");
    const mock_blob_t nvs_before = s_nvs;
    s_backend_state_count = 0U;
    s_flip_generation_on_backend_state_call = 3U;

    assert(d1l_dm_store_init() == ESP_OK);
    assert(memcmp(&s_nvs, &nvs_before, sizeof(s_nvs)) == 0);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(strcmp(row.text, "init-local") == 0);
    assert(d1l_dm_store_stats().sd_primary_reconcile_pending);

    memset(&s_sd, 0, sizeof(s_sd));
    s_flip_generation_on_backend_state_call = 0U;
    s_now_us += (int64_t)D1L_DM_STORE_PERSIST_RETRY_INTERVAL_MS * 1000LL;
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(blob_contains(&s_sd, "init-local"));
    assert(!blob_contains(&s_sd, "init-stale-card"));
}

static void test_v2_migrates_without_init_erase(void)
{
    reset_backend();
    test_blob_v2_t old = {
        .schema = TEST_SCHEMA_V2,
        .next_seq = 2U,
        .total_written = 1U,
        .head = 1U,
        .count = 1U,
    };
    old.entries[0] = make_legacy_entry(1U, "v2-row", "rx", 0U);
    assert(blob_write(&s_nvs, &old, sizeof(old)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_stats().nvs_fallback_dirty);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(strcmp(row.text, "v2-row") == 0);
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(((const test_blob_v7_t *)s_nvs.data)->schema == TEST_SCHEMA_V7);
    assert(((const test_blob_v6_t *)s_nvs.data)->clear_lineage == 0U);
}

static void test_v2_non_ascii_text_fails_closed_without_migration(void)
{
    reset_backend();
    test_blob_v2_t old = {
        .schema = TEST_SCHEMA_V2,
        .next_seq = 2U,
        .total_written = 1U,
        .head = 1U,
        .count = 1U,
    };
    old.entries[0] = make_legacy_entry(1U, "legacy-ascii", "rx", 0U);
    old.entries[0].text[0] = (char)0xc3;
    old.entries[0].text[1] = (char)0xa9;
    old.entries[0].text[2] = '\0';
    assert(blob_write(&s_nvs, &old, sizeof(old)) == ESP_OK);
    const mock_blob_t before = s_nvs;

    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_dm_store_stats().loaded);
    assert(s_nvs_write_count == 0U);
    assert(memcmp(&s_nvs, &before, sizeof(s_nvs)) == 0);
}

static void test_v3_migrates_without_inventing_clear_lineage(void)
{
    reset_backend();
    test_blob_v3_t old = {
        .schema = TEST_SCHEMA_V3,
        .next_seq = 2U,
        .total_written = 1U,
        .head = 1U,
        .count = 1U,
        .epoch = 44U,
        .content_revision = 8U,
    };
    old.entries[0] = make_legacy_entry(1U, "v3-row", "rx", 0U);
    assert(blob_write(&s_nvs, &old, sizeof(old)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_stats().nvs_fallback_dirty);
    assert(d1l_dm_store_flush() == ESP_OK);
    const test_blob_v6_t *migrated = (const test_blob_v6_t *)s_nvs.data;
    assert(migrated->schema == TEST_SCHEMA_V7);
    assert(migrated->epoch == 44U);
    assert(migrated->clear_lineage == 0U);
    assert(blob_contains(&s_nvs, "v3-row"));
}

static void test_volatile_preview_never_marks_persistence_dirty(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    const d1l_dm_store_stats_t before = d1l_dm_store_stats();
    assert(d1l_dm_store_append_volatile(
               "0123456789abcdef", "UI Canary", "rx", "preview",
               -50, 50, 1U, 1U, 1U, true, false, 0U) == ESP_OK);
    const d1l_dm_store_stats_t after = d1l_dm_store_stats();
    assert(after.persistence_dirty == before.persistence_dirty);
    assert(after.persistence_revision == before.persistence_revision);
    assert(s_sd_write_count == 0U && s_nvs_write_count == 0U);
}

static void test_append_path_limits_match_reboot_validator(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "invalid-path", -50, 50,
                               4U, 1U, 1U, true, false, 0U) ==
           ESP_ERR_INVALID_ARG);
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "invalid-product", -50, 50,
                               3U, 22U, 1U, true, false, 0U) ==
           ESP_ERR_INVALID_ARG);
    assert(s_nvs_write_count == 0U && s_sd_write_count == 0U);

    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "valid-path-boundary", -50, 50,
                               2U, 32U, 1U, true, false, 0U) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_OK);
    const d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    assert(stats.loaded);
    assert(stats.count == 1U);
    assert(blob_contains(&s_nvs, "valid-path-boundary"));
}

static void test_corrupt_only_source_blocks_append_without_overwrite(void)
{
    reset_backend();
    seed_v3(&s_nvs, 1U, 2U, "salvage-me");
    ((test_blob_v6_t *)s_nvs.data)->next_seq = 1U;
    const size_t before_len = s_nvs.len;
    uint8_t before[MOCK_BLOB_MAX] = {0};
    memcpy(before, s_nvs.data, before_len);

    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    const d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    assert(!stats.loaded);
    assert(stats.persistence_dirty);
    assert(stats.persistence_fail_count > 0U);
    assert(stats.nvs_fallback_dirty);
    assert(stats.nvs_fallback_last_error == ESP_ERR_INVALID_STATE);
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "must-not-overwrite", -50, 50, 1U, 1U, 1U,
                               true, false, 0U) == ESP_ERR_INVALID_STATE);
    assert(s_nvs_write_count == 0U);
    assert(s_nvs.len == before_len);
    assert(memcmp(s_nvs.data, before, before_len) == 0);
}

static void test_noncanonical_v4_and_v3_order_fail_closed(void)
{
    reset_backend();
    test_blob_v6_t malformed = {
        .schema = TEST_SCHEMA_V7,
        .next_seq = 3U,
        .total_written = 2U,
        .head = 2U,
        .count = 2U,
        .epoch = 1U,
        .content_revision = 3U,
    };
    malformed.entries[0] = make_entry(2U, "out-of-order-2", "rx", 2U);
    malformed.entries[1] = make_entry(1U, "out-of-order-1", "rx", 1U);
    assert(blob_write(&s_nvs, &malformed, sizeof(malformed)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_dm_store_stats().loaded);
    assert(s_nvs_write_count == 0U);

    reset_backend();
    test_blob_v3_t legacy = {
        .schema = TEST_SCHEMA_V3,
        .next_seq = 3U,
        .total_written = 2U,
        .head = 0U,
        .count = 2U,
        .epoch = 1U,
        .content_revision = 3U,
    };
    legacy.entries[0] = make_legacy_entry(1U, "bad-head-1", "rx", 1U);
    legacy.entries[1] = make_legacy_entry(2U, "bad-head-2", "rx", 2U);
    assert(blob_write(&s_nvs, &legacy, sizeof(legacy)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_dm_store_stats().loaded);
    assert(s_nvs_write_count == 0U);
}

static void test_offline_append_survives_higher_epoch_late_card(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "offline-local", -50, 50, 1U, 1U, 1U,
                               true, false, 0U) == ESP_OK);
    assert(blob_contains(&s_nvs, "offline-local"));

    seed_v3(&s_sd, 2U, 2U, "higher-epoch-card");
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_flush_if_due() == ESP_OK);
    assert(blob_contains(&s_sd, "higher-epoch-card"));
    assert(blob_contains(&s_sd, "offline-local"));
    assert(blob_contains(&s_nvs, "higher-epoch-card"));
    assert(blob_contains(&s_nvs, "offline-local"));
}

static void test_valid_legacy_nvs_dominates_foreign_clear_lineage(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_v3(&s_nvs, 1U, 2U, "legacy-local");
    seed_v3(&s_sd, UINT32_MAX, 2U, "foreign-clear");
    ((test_blob_v6_t *)s_sd.data)->clear_lineage =
        0x1122334455667788ULL;
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(((const test_blob_v6_t *)s_nvs.data)->clear_lineage == 0U);
    assert(((const test_blob_v6_t *)s_sd.data)->clear_lineage == 0U);
    assert(blob_contains(&s_nvs, "legacy-local"));
    assert(!blob_contains(&s_nvs, "foreign-clear"));
}

static void test_sd_lineage_seeds_only_without_nvs_or_local_mutation(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_v3(&s_sd, 3U, 2U, "sd-seed");
    ((test_blob_v6_t *)s_sd.data)->clear_lineage =
        0x8877665544332211ULL;
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(((const test_blob_v6_t *)s_nvs.data)->clear_lineage ==
           0x8877665544332211ULL);
    assert(blob_contains(&s_nvs, "sd-seed"));

    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "new-local-nvs", -50, 50, 1U, 1U, 1U,
                               true, false, 0U) == ESP_OK);
    seed_v3(&s_sd, UINT32_MAX, 2U, "must-not-replace-local");
    ((test_blob_v6_t *)s_sd.data)->clear_lineage =
        0xCAFEBABEDEADBEEFULL;
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_flush_if_due() == ESP_OK);
    assert(((const test_blob_v6_t *)s_sd.data)->clear_lineage == 0U);
    assert(blob_contains(&s_sd, "new-local-nvs"));
    assert(!blob_contains(&s_sd, "must-not-replace-local"));
}

static void test_conflicting_full_lineages_keep_truthful_counters(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_full_v3(&s_nvs, "nvs");
    seed_full_v3(&s_sd, "sd");
    assert(d1l_dm_store_init() == ESP_OK);
    const d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    assert(stats.count == D1L_DM_STORE_CAPACITY);
    assert(stats.next_seq == 2U * D1L_DM_STORE_CAPACITY + 1U);
    assert(stats.dropped_oldest == D1L_DM_STORE_CAPACITY);
    assert(stats.total_written == 2U * D1L_DM_STORE_CAPACITY);
    assert(stats.total_written == stats.dropped_oldest + stats.count);
}

static void test_rebooted_local_clear_lineage_dominates_foreign_cards(void)
{
    reset_backend();
    seed_v3(&s_nvs, 1U, 2U, "pre-clear");
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_clear() == ESP_OK);
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "post-clear-local", -50, 50, 1U, 1U, 1U,
                               true, false, 0U) == ESP_OK);
    const uint64_t local_lineage =
        ((const test_blob_v6_t *)s_nvs.data)->clear_lineage;
    assert(local_lineage != 0U);
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_stats().clear_lineage == local_lineage);

    seed_v3(&s_sd, d1l_dm_store_stats().epoch, 2U, "same-epoch-card");
    ((test_blob_v6_t *)s_sd.data)->clear_lineage =
        0xA5A5A5A55A5A5A5AULL;
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_flush_if_due() == ESP_OK);
    assert(((const test_blob_v6_t *)s_sd.data)->clear_lineage ==
           local_lineage);
    assert(!blob_contains(&s_sd, "same-epoch-card"));
    assert(blob_contains(&s_sd, "post-clear-local"));
    assert(blob_contains(&s_nvs, "post-clear-local"));

    seed_v3(&s_sd, UINT32_MAX, 2U, "higher-epoch-card");
    ((test_blob_v6_t *)s_sd.data)->clear_lineage =
        0x5A5A5A5AA5A5A5A5ULL;
    s_sd_enabled = false;
    s_backend_generation++;
    s_sd_enabled = true;
    s_backend_generation++;
    assert(d1l_dm_store_flush_if_due() == ESP_OK);
    assert(((const test_blob_v6_t *)s_sd.data)->clear_lineage ==
           local_lineage);
    assert(!blob_contains(&s_sd, "higher-epoch-card"));
    assert(blob_contains(&s_sd, "post-clear-local"));
}

static void test_late_full_sd_overlay_counters_are_truthful(void)
{
    reset_backend();
    seed_range_v3(&s_nvs, 4U, 1U, 4U, "base");
    seed_range_v3(&s_sd, 20U, 5U, 16U, "sd");
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "live-overlay", -50, 50, 1U, 1U, 1U,
                               true, false, 0U) == ESP_OK);
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_flush_if_due() == ESP_OK);
    const d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    assert(stats.total_written == 21U);
    assert(stats.dropped_oldest == 5U);
    assert(stats.count == D1L_DM_STORE_CAPACITY);
    assert(blob_contains(&s_sd, "live-overlay"));
}

static void test_higher_epoch_replay_keeps_evicted_mutation_counters(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    char text[32];
    for (uint32_t i = 1U; i <= 20U; ++i) {
        (void)snprintf(text, sizeof(text), "offline-%lu",
                       (unsigned long)i);
        assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                                   text, -50, 50, 1U, 1U, 1U,
                                   true, false, i) == ESP_OK);
    }
    seed_v3(&s_sd, 2U, 2U, NULL);
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_flush_if_due() == ESP_OK);
    const d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    assert(stats.total_written == 20U);
    assert(stats.dropped_oldest == 4U);
    assert(stats.count == D1L_DM_STORE_CAPACITY);
    assert(blob_contains(&s_sd, "offline-20"));
}

static void test_empty_same_epoch_late_card_does_not_double_count_live_rows(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    char text[32];
    for (uint32_t i = 1U; i <= 20U; ++i) {
        (void)snprintf(text, sizeof(text), "same-epoch-offline-%lu",
                       (unsigned long)i);
        assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                                   text, -50, 50, 1U, 1U, 1U,
                                   true, false, 0U) == ESP_OK);
    }
    d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    assert(stats.total_written == 20U);
    assert(stats.dropped_oldest == 4U);
    assert(stats.count == D1L_DM_STORE_CAPACITY);

    seed_v3(&s_sd, 1U, 1U, NULL);
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_flush_if_due() == ESP_OK);
    stats = d1l_dm_store_stats();
    assert(stats.total_written == 20U);
    assert(stats.dropped_oldest == 4U);
    assert(stats.count == D1L_DM_STORE_CAPACITY);
    assert(blob_contains(&s_sd, "same-epoch-offline-20"));
}

static void test_exhausted_sequence_fails_closed_and_clear_rotates_lineage(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_v3(&s_nvs, 1U, 2U, "nvs-max-seq");
    seed_v3(&s_sd, 1U, 2U, "sd-max-seq");
    ((test_blob_v6_t *)s_nvs.data)->next_seq = UINT32_MAX;
    ((test_blob_v6_t *)s_sd.data)->next_seq = UINT32_MAX;
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_stats().sd_primary_reconcile_pending);
    assert(d1l_dm_store_flush() == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U);
    assert(blob_contains(&s_sd, "sd-max-seq"));
    assert(blob_contains(&s_nvs, "nvs-max-seq"));

    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_v3(&s_nvs, 1U, 2U, "append-max-seq");
    seed_v3(&s_sd, 1U, 2U, "append-max-seq");
    ((test_blob_v6_t *)s_nvs.data)->next_seq = UINT32_MAX;
    ((test_blob_v6_t *)s_sd.data)->next_seq = UINT32_MAX;
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "must-not-wrap", -50, 50, 1U, 1U, 1U,
                               true, false, 0U) == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U && s_nvs_write_count == 0U);

    reset_backend();
    seed_v3(&s_nvs, UINT32_MAX, 2U, "max-epoch");
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_clear() == ESP_OK);
    const test_blob_v6_t *cleared = (const test_blob_v6_t *)s_nvs.data;
    assert(cleared->epoch == 1U);
    assert(cleared->clear_lineage != 0U);
    assert(cleared->count == 0U);
}

static void test_higher_epoch_replay_exhaustion_preserves_both_copies(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "offline-local", -50, 50, 1U, 1U, 1U,
                               true, false, 0U) == ESP_OK);
    seed_v3(&s_sd, 2U, 2U, "higher-max-seq");
    ((test_blob_v6_t *)s_sd.data)->next_seq = UINT32_MAX;
    const mock_blob_t sd_before = s_sd;
    const mock_blob_t nvs_before = s_nvs;
    s_sd_enabled = true;
    s_backend_generation = 1U;
    s_sd_write_count = 0U;
    s_nvs_write_count = 0U;

    assert(d1l_dm_store_flush() == ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U && s_nvs_write_count == 0U);
    assert(memcmp(&s_sd, &sd_before, sizeof(s_sd)) == 0);
    assert(memcmp(&s_nvs, &nvs_before, sizeof(s_nvs)) == 0);
    assert(d1l_dm_store_stats().sd_primary_reconcile_pending);
}

static void test_total_counter_exhaustion_rejects_append_without_writes(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_v3(&s_nvs, 1U, UINT32_MAX, "max-total");
    seed_v3(&s_sd, 1U, UINT32_MAX, "max-total");
    ((test_blob_v6_t *)s_nvs.data)->total_written = UINT32_MAX;
    ((test_blob_v6_t *)s_nvs.data)->dropped_oldest = UINT32_MAX - 1U;
    ((test_blob_v6_t *)s_sd.data)->total_written = UINT32_MAX;
    ((test_blob_v6_t *)s_sd.data)->dropped_oldest = UINT32_MAX - 1U;
    assert(d1l_dm_store_init() == ESP_OK);
    s_sd_write_count = 0U;
    s_nvs_write_count = 0U;
    assert(d1l_dm_store_append("0123456789abcdef", "Node", "rx",
                               "must-not-overflow-total", -50, 50,
                               1U, 1U, 1U, true, false, 0U) ==
           ESP_ERR_INVALID_STATE);
    assert(s_sd_write_count == 0U && s_nvs_write_count == 0U);
    assert(((const test_blob_v6_t *)s_sd.data)->total_written == UINT32_MAX);
    assert(((const test_blob_v6_t *)s_nvs.data)->total_written == UINT32_MAX);
}

static void test_v4_migration_preserves_rows_as_legacy_unverified(void)
{
    reset_backend();
    assert(sizeof(test_entry_v4_t) < sizeof(d1l_dm_entry_t));
    assert(sizeof(test_blob_v4_t) < sizeof(test_blob_v6_t));
    test_blob_v4_t legacy = {
        .schema = TEST_SCHEMA_V4,
        .next_seq = 8U,
        .total_written = 1U,
        .head = 1U,
        .count = 1U,
        .epoch = 3U,
        .content_revision = 9U,
        .clear_lineage = 0x1122334455667788ULL,
    };
    legacy.entries[0] = make_legacy_entry(7U, "v4-legacy-row", "rx", 44U);
    assert(blob_write(&s_nvs, &legacy, sizeof(legacy)) == ESP_OK);

    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.seq == 7U);
    assert(strcmp(row.text, "v4-legacy-row") == 0);
    assert(!row.identity_digest_valid);
    assert(row.ack_state == D1L_DM_ACK_STATE_LEGACY_UNVERIFIED);
    assert(row.ack_dispatch_count == 0U);
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(((const test_blob_v7_t *)s_nvs.data)->schema == TEST_SCHEMA_V7);
}

static void test_literal_v4_full_ring_migrates_without_data_loss(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    test_blob_v4_t legacy = {
        .schema = TEST_SCHEMA_V4,
        .next_seq = D1L_DM_STORE_CAPACITY + 1U,
        .total_written = D1L_DM_STORE_CAPACITY,
        .head = 0U,
        .count = D1L_DM_STORE_CAPACITY,
        .epoch = 7U,
        .content_revision = 19U,
        .clear_lineage = UINT64_C(0x1122334455667788),
    };
    for (uint32_t i = 0U; i < D1L_DM_STORE_CAPACITY; ++i) {
        char text[32];
        (void)snprintf(text, sizeof(text), "literal-v4-row-%02lu",
                       (unsigned long)(i + 1U));
        legacy.entries[i] = make_legacy_entry(
            i + 1U, text, i % 2U == 0U ? "rx" : "tx", 0x4100U + i);
        (void)snprintf(legacy.entries[i].contact_alias,
                       sizeof(legacy.entries[i].contact_alias),
                       "12345678901234567890123");
    }
    assert(blob_write(&s_nvs, &legacy, sizeof(legacy)) == ESP_OK);
    assert(blob_write(&s_sd, &legacy, sizeof(legacy)) == ESP_OK);

    assert(d1l_dm_store_init() == ESP_OK);
    const d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    assert(stats.loaded);
    assert(stats.count == D1L_DM_STORE_CAPACITY);
    assert(stats.epoch == legacy.epoch);
    assert(stats.content_revision == legacy.content_revision);
    assert(stats.clear_lineage == legacy.clear_lineage);
    d1l_dm_entry_t rows[D1L_DM_STORE_CAPACITY] = {0};
    assert(d1l_dm_store_copy_recent(rows, D1L_DM_STORE_CAPACITY) ==
           D1L_DM_STORE_CAPACITY);
    for (uint32_t i = 0U; i < D1L_DM_STORE_CAPACITY; ++i) {
        assert(rows[i].seq == i + 1U);
        assert(strcmp(rows[i].contact_alias,
                      "12345678901234567890123") == 0);
        assert(rows[i].delivery_revision ==
               (strcmp(rows[i].direction, "tx") == 0 ? 1U : 0U));
    }
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(((const test_blob_v7_t *)s_nvs.data)->schema == TEST_SCHEMA_V7);
    assert(((const test_blob_v7_t *)s_sd.data)->schema == TEST_SCHEMA_V7);
    assert(s_nvs.len == sizeof(test_blob_v7_t));
    assert(s_sd.len == sizeof(test_blob_v7_t));
}

static void test_schema_v6_narrow_preserves_all_metadata(void)
{
    reset_backend();
    assert(sizeof(test_entry_v6_narrow_t) < sizeof(d1l_dm_entry_t));
    assert(sizeof(test_blob_v6_narrow_t) < sizeof(test_blob_v6_wide_t));
    test_blob_v6_narrow_t legacy = {
        .schema = TEST_SCHEMA_V6,
        .next_seq = 3U,
        .total_written = 14U,
        .dropped_oldest = 12U,
        .head = 2U,
        .count = 2U,
        .epoch = 23U,
        .content_revision = 29U,
        .clear_lineage = UINT64_C(0x8877665544332211),
    };
    d1l_dm_entry_t rx = make_entry(1U, "schema6-narrow-rx", "rx", 0x6161U);
    (void)snprintf(rx.contact_alias, sizeof(rx.contact_alias),
                   "12345678901234567890123");
    set_ack_metadata(&rx, 0x61U, D1L_DM_ACK_STATE_SENT, 1U, 2U, ESP_OK);
    d1l_dm_entry_t tx = make_entry(2U, "schema6-narrow-tx", "tx", 0x6262U);
    tx.attempt = 2U;
    tx.delivery_state = D1L_DM_DELIVERY_FAILED_RADIO;
    tx.delivery_reason = D1L_DM_DELIVERY_REASON_RADIO_ERROR;
    tx.delivery_last_error = ESP_FAIL;
    tx.delivery_retry_count = 1U;
    tx.delivery_revision = 9U;
    legacy.entries[0] = make_v6_narrow_entry(&rx);
    legacy.entries[1] = make_v6_narrow_entry(&tx);
    assert(blob_write(&s_nvs, &legacy, sizeof(legacy)) == ESP_OK);

    assert(d1l_dm_store_init() == ESP_OK);
    const d1l_dm_store_stats_t stats = d1l_dm_store_stats();
    assert(stats.loaded && stats.count == 2U);
    assert(stats.epoch == legacy.epoch);
    assert(stats.content_revision == legacy.content_revision);
    assert(stats.clear_lineage == legacy.clear_lineage);
    d1l_dm_entry_t rows[2] = {0};
    assert(d1l_dm_store_copy_recent(rows, 2U) == 2U);
    assert(rows[0].seq == 1U);
    assert(strcmp(rows[0].contact_alias,
                  "12345678901234567890123") == 0);
    assert(rows[0].identity_digest_valid);
    assert(memcmp(rows[0].identity_digest, rx.identity_digest,
                  sizeof(rx.identity_digest)) == 0);
    assert(rows[0].ack_dispatch_count == 1U);
    assert(rows[0].ack_dispatch_kind == 2U);
    assert(rows[0].ack_state == D1L_DM_ACK_STATE_SENT);
    assert(rows[0].ack_last_error == ESP_OK);
    assert(rows[1].seq == 2U);
    assert(rows[1].delivery_session_id == tx.delivery_session_id);
    assert(rows[1].delivery_state == D1L_DM_DELIVERY_FAILED_RADIO);
    assert(rows[1].delivery_reason == D1L_DM_DELIVERY_REASON_RADIO_ERROR);
    assert(rows[1].delivery_last_error == ESP_FAIL);
    assert(rows[1].delivery_retry_count == 1U);
    assert(rows[1].delivery_revision == 9U);
    assert(d1l_dm_store_flush() == ESP_OK);
    const test_blob_v7_t *rewritten = (const test_blob_v7_t *)s_nvs.data;
    assert(s_nvs.len == sizeof(*rewritten));
    assert(rewritten->schema == TEST_SCHEMA_V7);
    assert(rewritten->epoch == legacy.epoch);
    assert(rewritten->content_revision == legacy.content_revision);
    assert(rewritten->clear_lineage == legacy.clear_lineage);
}

static void test_schema_v6_wide_preserves_utf8_and_rewrites_sd(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    test_blob_v6_wide_t legacy = make_v3(31U, 37U, "wide-placeholder");
    legacy.schema = TEST_SCHEMA_V6;
    legacy.clear_lineage = UINT64_C(0xaabbccddeeff0011);
    const char utf8_text[] = "schema6-wide-caf\xc3\xa9";
    memcpy(legacy.entries[0].text, utf8_text, sizeof(utf8_text));
    set_ack_metadata(&legacy.entries[0], 0x71U, D1L_DM_ACK_STATE_SENT,
                     1U, 3U, ESP_OK);
    assert(blob_write(&s_sd, &legacy, sizeof(legacy)) == ESP_OK);

    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(memcmp(row.text, utf8_text, sizeof(utf8_text)) == 0);
    assert(row.ack_dispatch_count == 1U);
    assert(row.ack_dispatch_kind == 3U);
    assert(row.ack_state == D1L_DM_ACK_STATE_SENT);
    assert(d1l_dm_store_stats().epoch == legacy.epoch);
    assert(d1l_dm_store_stats().content_revision == legacy.content_revision);
    assert(d1l_dm_store_stats().clear_lineage == legacy.clear_lineage);
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(s_sd.len == sizeof(test_blob_v7_t));
    assert(s_nvs.len == sizeof(test_blob_v7_t));
    assert(((const test_blob_v7_t *)s_sd.data)->schema == TEST_SCHEMA_V7);
    assert(((const test_blob_v7_t *)s_nvs.data)->schema == TEST_SCHEMA_V7);
}

static void assert_invalid_nvs_blob_is_not_mutated(void)
{
    const mock_blob_t before = s_nvs;
    const esp_err_t ret = d1l_dm_store_init();
    assert(ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_INVALID_SIZE);
    assert(!d1l_dm_store_stats().loaded);
    assert(s_nvs_write_count == 0U);
    assert(s_sd_write_count == 0U);
    assert(memcmp(&s_nvs, &before, sizeof(s_nvs)) == 0);
}

static void test_schema_v6_unknown_lengths_fail_closed_without_mutation(void)
{
    test_blob_v6_narrow_t narrow = {
        .schema = TEST_SCHEMA_V6,
        .next_seq = 1U,
        .head = 0U,
        .count = 0U,
        .epoch = 1U,
        .content_revision = 1U,
    };
    test_blob_v6_wide_t wide = make_v3(1U, 1U, NULL);
    wide.schema = TEST_SCHEMA_V6;

    reset_backend();
    assert(blob_write(&s_nvs, &narrow, sizeof(narrow)) == ESP_OK);
    s_nvs.len--;
    assert_invalid_nvs_blob_is_not_mutated();

    reset_backend();
    assert(blob_write(&s_nvs, &narrow, sizeof(narrow)) == ESP_OK);
    s_nvs.data[s_nvs.len++] = 0xa5U;
    assert_invalid_nvs_blob_is_not_mutated();

    reset_backend();
    assert(blob_write(&s_nvs, &wide, sizeof(wide)) == ESP_OK);
    s_nvs.len--;
    assert_invalid_nvs_blob_is_not_mutated();

    reset_backend();
    assert(blob_write(&s_nvs, &wide, sizeof(wide)) == ESP_OK);
    s_nvs.data[s_nvs.len++] = 0x5aU;
    assert_invalid_nvs_blob_is_not_mutated();
}

static void test_malformed_v5_ack_metadata_fails_closed(void)
{
    reset_backend();
    test_blob_v6_t malformed = make_v3(1U, 2U, "bad-pending-zero");
    set_ack_metadata(&malformed.entries[0], 0x11U,
                     D1L_DM_ACK_STATE_PENDING, 0U, 0U, ESP_OK);
    assert(blob_write(&s_nvs, &malformed, sizeof(malformed)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_dm_store_stats().loaded);
    assert(s_nvs_write_count == 0U);

    reset_backend();
    malformed = make_v3(1U, 2U, "bad-terminal-one");
    set_ack_metadata(&malformed.entries[0], 0x22U,
                     D1L_DM_ACK_STATE_TERMINAL, 1U, 1U,
                     ESP_ERR_TIMEOUT);
    assert(blob_write(&s_nvs, &malformed, sizeof(malformed)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_dm_store_stats().loaded);

    reset_backend();
    malformed = (test_blob_v6_t) {
        .schema = TEST_SCHEMA_V7,
        .next_seq = 3U,
        .total_written = 2U,
        .head = 2U,
        .count = 2U,
        .epoch = 1U,
        .content_revision = 3U,
    };
    malformed.entries[0] = make_entry(1U, "duplicate-a", "rx", 1U);
    malformed.entries[1] = make_entry(2U, "duplicate-b", "rx", 2U);
    set_ack_metadata(&malformed.entries[0], 0x44U,
                     D1L_DM_ACK_STATE_RETRYABLE, 0U, 0U, ESP_OK);
    set_ack_metadata(&malformed.entries[1], 0x44U,
                     D1L_DM_ACK_STATE_RETRYABLE, 0U, 0U, ESP_OK);
    assert(blob_write(&s_nvs, &malformed, sizeof(malformed)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_dm_store_stats().loaded);

}

static void test_malformed_v6_delivery_metadata_fails_closed(void)
{
    reset_backend();
    test_blob_v6_t malformed = make_v3(1U, 2U, NULL);
    malformed.next_seq = 2U;
    malformed.total_written = 1U;
    malformed.head = 1U;
    malformed.count = 1U;
    malformed.entries[0] = make_entry(
        1U, "bad-reason", "tx", 0x9090U);
    malformed.entries[0].delivery_reason =
        D1L_DM_DELIVERY_REASON_RADIO_ERROR;
    assert(blob_write(&s_nvs, &malformed, sizeof(malformed)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_dm_store_stats().loaded);
    assert(s_nvs_write_count == 0U);

    reset_backend();
    malformed = make_v3(1U, 2U, NULL);
    malformed.next_seq = 2U;
    malformed.total_written = 1U;
    malformed.head = 1U;
    malformed.count = 1U;
    malformed.entries[0] = make_entry(
        1U, "bad-ack", "tx", 0x9191U);
    malformed.entries[0].delivery_state = D1L_DM_DELIVERY_ACKNOWLEDGED;
    malformed.entries[0].delivery_reason =
        D1L_DM_DELIVERY_REASON_ACK_RECEIVED;
    malformed.entries[0].acked = false;
    assert(blob_write(&s_nvs, &malformed, sizeof(malformed)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_dm_store_stats().loaded);

    reset_backend();
    malformed = make_v3(1U, 2U, NULL);
    malformed.next_seq = 2U;
    malformed.total_written = 1U;
    malformed.head = 1U;
    malformed.count = 1U;
    malformed.entries[0] = make_entry(
        1U, "bad-delivered", "tx", 0x9292U);
    malformed.entries[0].delivered = true;
    assert(blob_write(&s_nvs, &malformed, sizeof(malformed)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_dm_store_stats().loaded);

    reset_backend();
    malformed = make_v3(1U, 3U, NULL);
    malformed.next_seq = 3U;
    malformed.total_written = 2U;
    malformed.head = 2U;
    malformed.count = 2U;
    malformed.entries[0] = make_entry(
        1U, "session-a", "tx", 0x9393U);
    malformed.entries[1] = make_entry(
        2U, "session-b", "tx", 0x9494U);
    malformed.entries[1].delivery_session_id =
        malformed.entries[0].delivery_session_id;
    assert(blob_write(&s_nvs, &malformed, sizeof(malformed)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_dm_store_stats().loaded);
}

static void test_append_failure_retains_one_identity_row_until_flush(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    fill_identity_digest(digest, 0x31U);
    s_nvs_write_error = ESP_FAIL;
    d1l_dm_store_append_outcome_t outcome = {0};
    assert(d1l_dm_store_append_rx_identity(
               "0123456789abcdef", "Node", "store-once", -50, 40,
               1U, 1U, 0U, 0x1234U, digest, &outcome) == ESP_FAIL);
    assert(outcome.inserted && !outcome.durable);
    assert(outcome.row_seq == 1U && outcome.error == ESP_FAIL);
    assert(d1l_dm_store_stats().count == 1U);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(strcmp(row.text, "store-once") == 0);
    assert(row.ack_dispatch_count == 0U);
    assert(d1l_dm_store_flush() == ESP_FAIL);
    assert(d1l_dm_store_stats().count == 1U);

    s_nvs_write_error = ESP_OK;
    s_now_us += (int64_t)D1L_DM_STORE_PERSIST_RETRY_INTERVAL_MS * 1000LL;
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(d1l_dm_store_stats().count == 1U);
    assert(((const test_blob_v6_t *)s_nvs.data)->count == 1U);
}

static void test_tx_append_quiesce_yield_flushes_same_row_without_duplicate(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_init() == ESP_OK);

    s_sd_write_error = ESP_ERR_NOT_FINISHED;
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_tx(
               "0123456789abcdef", "Node", "quiesce-retry", -50, 40,
               1U, 1U, 0U, 0x2233U, &append) == ESP_ERR_NOT_FINISHED);
    assert(append.inserted && !append.durable);
    assert(append.row_seq == 1U);
    assert(append.delivery_session_id != 0U);
    assert(append.error == ESP_ERR_NOT_FINISHED);
    assert(d1l_dm_store_stats().count == 1U);

    s_sd_write_error = ESP_OK;
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(d1l_dm_store_stats().count == 1U);
    assert(((const test_blob_v6_t *)s_sd.data)->count == 1U);
    assert(((const test_blob_v6_t *)s_nvs.data)->count == 1U);
    assert(strcmp(((const test_blob_v6_t *)s_sd.data)->entries[0].text,
                  "quiesce-retry") == 0);
}

static void test_partial_reservation_commit_never_rolls_back_or_sends_twice(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_init() == ESP_OK);
    uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    fill_identity_digest(digest, 0x42U);
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_rx_identity(
               "0123456789abcdef", "Node", "partial-reserve", -50, 40,
               1U, 1U, 0U, 0x3344U, digest, &append) == ESP_OK);

    s_sd_write_error = ESP_FAIL;
    d1l_dm_ack_reservation_t reservation = {0};
    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 1U, &reservation) == ESP_FAIL);
    assert(reservation.reserved && !reservation.durable);
    assert(reservation.dispatch_count == 1U);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_state == D1L_DM_ACK_STATE_PENDING);
    assert(row.ack_dispatch_count == 1U);
    assert(((const test_blob_v6_t *)s_nvs.data)->entries[0].ack_dispatch_count == 1U);
    assert(((const test_blob_v6_t *)s_sd.data)->entries[0].ack_dispatch_count == 0U);
    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 1U, &reservation) == ESP_ERR_INVALID_STATE);

    s_sd_write_error = ESP_OK;
    s_now_us += (int64_t)D1L_DM_STORE_PERSIST_RETRY_INTERVAL_MS * 1000LL;
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(((const test_blob_v6_t *)s_sd.data)->entries[0].ack_dispatch_count == 1U);
    assert(d1l_dm_store_complete_ack_dispatch(
               row.seq, digest, true, ESP_OK) == ESP_OK);
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_state == D1L_DM_ACK_STATE_SENT);
    assert(row.ack_dispatch_count == 1U);
}

static void test_deferred_ack_lifecycle_writes_only_on_explicit_flush(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_init() == ESP_OK);

    const uint32_t sd_writes_before = s_sd_write_count;
    const uint32_t nvs_writes_before = s_nvs_write_count;
    const size_t operations_before = s_operation_count;
    uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    fill_identity_digest(digest, 0xD1U);

    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_rx_identity_deferred(
               "0123456789abcdef", "Node", "deferred-ack", -50, 40,
               1U, 1U, 0U, 0xD1ACU, digest, &append) == ESP_OK);
    assert(append.inserted && !append.durable);
    assert(append.row_seq == 1U && append.error == ESP_OK);
    assert(s_sd_write_count == sd_writes_before);
    assert(s_nvs_write_count == nvs_writes_before);
    assert(s_operation_count == operations_before);

    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.seq == append.row_seq);
    assert(row.ack_state == D1L_DM_ACK_STATE_RETRYABLE);
    assert(row.ack_dispatch_count == 0U);
    assert(d1l_dm_store_stats().persistence_dirty);

    d1l_dm_ack_reservation_t reservation = {0};
    assert(d1l_dm_store_reserve_ack_dispatch_deferred(
               digest, 2U, &reservation) == ESP_OK);
    assert(reservation.reserved && !reservation.durable);
    assert(reservation.row_seq == append.row_seq);
    assert(reservation.dispatch_count == 1U);
    assert(reservation.error == ESP_OK);
    assert(s_sd_write_count == sd_writes_before);
    assert(s_nvs_write_count == nvs_writes_before);
    assert(s_operation_count == operations_before);

    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_state == D1L_DM_ACK_STATE_PENDING);
    assert(row.ack_dispatch_count == 1U);
    assert(row.ack_dispatch_kind == 2U);

    assert(d1l_dm_store_complete_ack_dispatch_deferred(
               reservation.row_seq, digest, true, ESP_OK) == ESP_OK);
    assert(s_sd_write_count == sd_writes_before);
    assert(s_nvs_write_count == nvs_writes_before);
    assert(s_operation_count == operations_before);
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_state == D1L_DM_ACK_STATE_SENT);
    assert(row.ack_dispatch_count == 1U);
    assert(row.ack_dispatch_kind == 2U);
    assert(row.ack_last_error == ESP_OK);

    assert(d1l_dm_store_flush() == ESP_OK);
    assert(s_sd_write_count == sd_writes_before + 1U);
    assert(s_nvs_write_count == nvs_writes_before + 1U);
    assert(!d1l_dm_store_stats().persistence_dirty);
    assert(s_sd.valid && s_nvs.valid);

    const test_blob_v6_t *sd = (const test_blob_v6_t *)s_sd.data;
    const test_blob_v6_t *nvs = (const test_blob_v6_t *)s_nvs.data;
    assert(sd->count == 1U && nvs->count == 1U);
    assert(sd->entries[0].ack_state == D1L_DM_ACK_STATE_SENT);
    assert(nvs->entries[0].ack_state == D1L_DM_ACK_STATE_SENT);
    assert(sd->entries[0].ack_dispatch_count == 1U);
    assert(nvs->entries[0].ack_dispatch_count == 1U);
    assert(sd->entries[0].ack_dispatch_kind == 2U);
    assert(nvs->entries[0].ack_dispatch_kind == 2U);
    assert(memcmp(sd->entries[0].identity_digest, digest, sizeof(digest)) == 0);
    assert(memcmp(nvs->entries[0].identity_digest, digest, sizeof(digest)) == 0);
}

static void test_deferred_ack_unloaded_fails_without_storage_io(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    fill_identity_digest(digest, 0xD2U);

    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_rx_identity_deferred(
               "0123456789abcdef", "Node", "unloaded", -50, 40,
               1U, 1U, 0U, 0xD2ACU, digest,
               &append) == ESP_ERR_INVALID_STATE);
    assert(!append.inserted && !append.durable);
    assert(append.error == ESP_ERR_INVALID_STATE);

    d1l_dm_ack_reservation_t reservation = {0};
    assert(d1l_dm_store_reserve_ack_dispatch_deferred(
               digest, 1U, &reservation) == ESP_ERR_INVALID_STATE);
    assert(!reservation.reserved && !reservation.durable);
    assert(reservation.error == ESP_ERR_INVALID_STATE);
    assert(d1l_dm_store_rebind_pending_ack_dispatch_deferred(
               1U, digest, 2U) == ESP_ERR_INVALID_STATE);
    assert(d1l_dm_store_complete_ack_dispatch_deferred(
               1U, digest, false, ESP_FAIL) == ESP_ERR_INVALID_STATE);
    d1l_dm_delivery_transition_outcome_t delivery = {0};
    assert(d1l_dm_store_transition_delivery_deferred(
               1U, D1L_DM_DELIVERY_AWAITING_ACK, 1U,
               D1L_DM_DELIVERY_ACKNOWLEDGED,
               D1L_DM_DELIVERY_REASON_ACK_RECEIVED, ESP_OK, &delivery) ==
           ESP_ERR_INVALID_STATE);
    assert(!delivery.changed && !delivery.durable);
    assert(delivery.error == ESP_ERR_INVALID_STATE);
    d1l_dm_entry_t row = {0};
    assert(!d1l_dm_store_find_rx_identity_loaded(digest, &row));

    assert(s_backend_state_count == 0U);
    assert(s_sd_read_count == 0U);
    assert(s_sd_write_count == 0U);
    assert(s_nvs_write_count == 0U);
    assert(s_operation_count == 0U);
}

static void test_reboot_ack_state_mutations_are_bounded(void)
{
    uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    fill_identity_digest(digest, 0xA5U);
    d1l_dm_entry_t row = {0};
    d1l_dm_ack_reservation_t reservation = {0};

    reset_backend();
    seed_ack_state(&s_nvs, 4U, D1L_DM_ACK_STATE_PENDING, 1U, ESP_OK);
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_state == D1L_DM_ACK_STATE_RETRYABLE);
    assert(row.ack_last_error == D1L_DM_ACK_INTERRUPTED_ERROR);
    assert(row.ack_dispatch_count == 1U);
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(d1l_dm_store_reserve_ack_dispatch(digest, 1U, &reservation) == ESP_OK);
    assert(reservation.dispatch_count == 2U);
    assert(d1l_dm_store_complete_ack_dispatch(
               reservation.row_seq, digest, true, ESP_OK) == ESP_OK);
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_state == D1L_DM_ACK_STATE_SENT);
    assert(row.ack_dispatch_count == 2U);
    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 1U, &reservation) == ESP_ERR_INVALID_STATE);

    reset_backend();
    seed_ack_state(&s_nvs, 4U, D1L_DM_ACK_STATE_PENDING, 2U, ESP_OK);
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_state == D1L_DM_ACK_STATE_TERMINAL);
    assert(row.ack_last_error == D1L_DM_ACK_INTERRUPTED_ERROR);
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 1U, &reservation) == ESP_ERR_INVALID_STATE);

    reset_backend();
    seed_ack_state(&s_nvs, 4U, D1L_DM_ACK_STATE_SENT, 1U, ESP_OK);
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_reserve_ack_dispatch(digest, 1U, &reservation) == ESP_OK);
    assert(reservation.dispatch_count == 2U);
    assert(d1l_dm_store_complete_ack_dispatch(
               reservation.row_seq, digest, true, ESP_OK) == ESP_OK);
    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 1U, &reservation) == ESP_ERR_INVALID_STATE);

    reset_backend();
    seed_ack_state(&s_nvs, 4U, D1L_DM_ACK_STATE_SENT, 2U, ESP_OK);
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 1U, &reservation) == ESP_ERR_INVALID_STATE);
}

static void test_txdone_persist_failure_blocks_until_flush_or_reboot(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    fill_identity_digest(digest, 0x5AU);
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_rx_identity(
               "0123456789abcdef", "Node", "txdone-dirty", -50, 40,
               1U, 1U, 0U, 0x5566U, digest, &append) == ESP_OK);
    d1l_dm_ack_reservation_t reservation = {0};
    assert(d1l_dm_store_reserve_ack_dispatch(digest, 1U, &reservation) == ESP_OK);
    s_nvs_write_error = ESP_FAIL;
    assert(d1l_dm_store_complete_ack_dispatch(
               reservation.row_seq, digest, true, ESP_OK) == ESP_FAIL);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_state == D1L_DM_ACK_STATE_SENT);
    assert(d1l_dm_store_stats().persistence_dirty);
    assert(((const test_blob_v6_t *)s_nvs.data)->entries[0].ack_state ==
           D1L_DM_ACK_STATE_PENDING);
    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 1U, &reservation) == ESP_ERR_INVALID_STATE);

    s_nvs_write_error = ESP_OK;
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_state == D1L_DM_ACK_STATE_RETRYABLE);
    assert(row.ack_dispatch_count == 1U);
    assert(row.ack_last_error == D1L_DM_ACK_INTERRUPTED_ERROR);
}

static void test_reconcile_ack_metadata_never_regresses(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_ack_state(&s_nvs, 5U, D1L_DM_ACK_STATE_SENT, 1U, ESP_OK);
    seed_ack_state(&s_sd, 6U, D1L_DM_ACK_STATE_PENDING, 1U, ESP_OK);
    assert(d1l_dm_store_init() == ESP_OK);
    uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    fill_identity_digest(digest, 0xA5U);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_dispatch_count == 1U);
    assert(row.ack_state == D1L_DM_ACK_STATE_SENT);

    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_ack_state(&s_nvs, 7U, D1L_DM_ACK_STATE_TERMINAL, 2U,
                   ESP_ERR_TIMEOUT);
    seed_ack_state(&s_sd, 8U, D1L_DM_ACK_STATE_SENT, 1U, ESP_OK);
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_dispatch_count == 2U);
    assert(row.ack_state == D1L_DM_ACK_STATE_TERMINAL);
    assert(row.ack_last_error == ESP_ERR_TIMEOUT);

    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    test_blob_v6_t lower_epoch = make_v3(1U, 9U, "epoch-nvs");
    lower_epoch.next_seq = 3U;
    lower_epoch.entries[0] = make_entry(2U, "same-digest", "rx", 0x99U);
    set_ack_metadata(&lower_epoch.entries[0], 0xD4U,
                     D1L_DM_ACK_STATE_SENT, 1U, 2U, ESP_OK);
    test_blob_v6_t higher_epoch = make_v3(2U, 10U, "epoch-sd");
    higher_epoch.entries[0] = make_entry(1U, "same-digest", "rx", 0x99U);
    set_ack_metadata(&higher_epoch.entries[0], 0xD4U,
                     D1L_DM_ACK_STATE_PENDING, 1U, 1U, ESP_OK);
    assert(blob_write(&s_nvs, &lower_epoch, sizeof(lower_epoch)) == ESP_OK);
    assert(blob_write(&s_sd, &higher_epoch, sizeof(higher_epoch)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_OK);
    fill_identity_digest(digest, 0xD4U);
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(d1l_dm_store_stats().count == 1U);
    assert(row.ack_dispatch_count == 1U);
    assert(row.ack_state == D1L_DM_ACK_STATE_SENT);
}

static void test_reconcile_same_digest_at_different_sequences_keeps_one_budget(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    test_blob_v6_t nvs = {
        .schema = TEST_SCHEMA_V7,
        .next_seq = 3U,
        .total_written = 2U,
        .head = 2U,
        .count = 2U,
        .epoch = 1U,
        .content_revision = 6U,
    };
    nvs.entries[0] = make_entry(1U, "identity-a", "rx", 0x11U);
    nvs.entries[1] = make_entry(2U, "identity-b", "rx", 0x22U);
    set_ack_metadata(&nvs.entries[0], 0x11U,
                     D1L_DM_ACK_STATE_RETRYABLE, 0U, 0U, ESP_OK);
    set_ack_metadata(&nvs.entries[1], 0xB2U,
                     D1L_DM_ACK_STATE_SENT, 1U, 2U, ESP_OK);

    test_blob_v6_t sd = {
        .schema = TEST_SCHEMA_V7,
        .next_seq = 2U,
        .total_written = 1U,
        .head = 1U,
        .count = 1U,
        .epoch = 1U,
        .content_revision = 5U,
    };
    sd.entries[0] = make_entry(1U, "identity-b", "rx", 0x22U);
    set_ack_metadata(&sd.entries[0], 0xB2U,
                     D1L_DM_ACK_STATE_PENDING, 2U, 3U, ESP_OK);
    assert(blob_write(&s_nvs, &nvs, sizeof(nvs)) == ESP_OK);
    assert(blob_write(&s_sd, &sd, sizeof(sd)) == ESP_OK);

    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_entry_t rows[D1L_DM_STORE_CAPACITY] = {0};
    const size_t copied = d1l_dm_store_copy_recent(
        rows, D1L_DM_STORE_CAPACITY);
    assert(copied == 2U);
    uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    fill_identity_digest(digest, 0xB2U);
    size_t digest_matches = 0U;
    for (size_t i = 0U; i < copied; ++i) {
        if (rows[i].identity_digest_valid &&
            memcmp(rows[i].identity_digest, digest, sizeof(digest)) == 0) {
            digest_matches++;
        }
    }
    assert(digest_matches == 1U);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_dispatch_count == D1L_DM_ACK_DISPATCH_MAX);
    assert(row.ack_state == D1L_DM_ACK_STATE_TERMINAL);
    d1l_dm_ack_reservation_t reservation = {0};
    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 1U, &reservation) == ESP_ERR_INVALID_STATE);
}

static void test_pending_kind_rebind_preserves_the_reserved_budget(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_init() == ESP_OK);
    uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    fill_identity_digest(digest, 0xC3U);
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_rx_identity(
               "0123456789abcdef", "Node", "rebind-route", -50, 40,
               1U, 1U, 0U, 0x7788U, digest, &append) == ESP_OK);
    d1l_dm_ack_reservation_t reservation = {0};
    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 2U, &reservation) == ESP_OK);
    assert(reservation.dispatch_count == 1U);

    s_sd_write_error = ESP_FAIL;
    assert(d1l_dm_store_rebind_pending_ack_dispatch(
               reservation.row_seq, digest, 1U) == ESP_FAIL);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_state == D1L_DM_ACK_STATE_PENDING);
    assert(row.ack_dispatch_count == 1U);
    assert(row.ack_dispatch_kind == 1U);
    assert(d1l_dm_store_stats().persistence_dirty);
    assert(((const test_blob_v6_t *)s_nvs.data)->entries[0].ack_dispatch_kind == 1U);
    assert(((const test_blob_v6_t *)s_sd.data)->entries[0].ack_dispatch_kind == 2U);

    s_sd_write_error = ESP_OK;
    s_now_us += (int64_t)D1L_DM_STORE_PERSIST_RETRY_INTERVAL_MS * 1000LL;
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_dispatch_count == 1U);
    assert(row.ack_dispatch_kind == 1U);
    assert(d1l_dm_store_complete_ack_dispatch(
               reservation.row_seq, digest, true, ESP_OK) == ESP_OK);
}

static void test_late_reconcile_resequence_keeps_pending_callback_budget(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    uint8_t digest[D1L_DM_IDENTITY_DIGEST_BYTES];
    fill_identity_digest(digest, 0xE5U);
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_rx_identity(
               "0123456789abcdef", "Node", "in-flight-resequence", -50, 40,
               1U, 1U, 0U, 0x8899U, digest, &append) == ESP_OK);
    d1l_dm_ack_reservation_t reservation = {0};
    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 1U, &reservation) == ESP_OK);
    const uint32_t queued_row_seq = reservation.row_seq;

    seed_v3(&s_sd, 2U, 20U, "card-seq-collision");
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_flush() == ESP_OK);

    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.seq != queued_row_seq);
    assert(row.ack_state == D1L_DM_ACK_STATE_PENDING);
    assert(row.ack_dispatch_count == 1U);
    assert(d1l_dm_store_complete_ack_dispatch(
               queued_row_seq, digest, true, ESP_OK) == ESP_OK);
    assert(d1l_dm_store_find_rx_identity(digest, &row));
    assert(row.ack_state == D1L_DM_ACK_STATE_SENT);
    assert(row.ack_dispatch_count == 1U);

    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 1U, &reservation) == ESP_OK);
    assert(reservation.dispatch_count == 2U);
    assert(d1l_dm_store_complete_ack_dispatch(
               reservation.row_seq, digest, true, ESP_OK) == ESP_OK);
    assert(d1l_dm_store_reserve_ack_dispatch(
               digest, 1U, &reservation) == ESP_ERR_INVALID_STATE);
}

static void test_v5_migration_never_invents_delivery_success(void)
{
    reset_backend();
    test_blob_v5_t legacy = {
        .schema = TEST_SCHEMA_V5,
        .next_seq = 3U,
        .total_written = 2U,
        .head = 2U,
        .count = 2U,
        .epoch = 1U,
        .content_revision = 3U,
    };
    legacy.entries[0] = make_v5_entry(
        1U, "legacy-unconfirmed", "tx", 0x1010U, false);
    legacy.entries[1] = make_v5_entry(
        2U, "legacy-acked", "tx", 0x2020U, true);
    assert(blob_write(&s_nvs, &legacy, sizeof(legacy)) == ESP_OK);

    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_entry_t rows[2] = {0};
    assert(d1l_dm_store_copy_recent(rows, 2U) == 2U);
    assert(rows[0].delivery_state ==
           D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT);
    assert(rows[0].delivery_reason == D1L_DM_DELIVERY_REASON_LEGACY_IMPORT);
    assert(rows[0].delivery_last_error ==
           D1L_DM_DELIVERY_INTERRUPTED_ERROR);
    assert(rows[0].delivery_session_id != 0U);
    assert(rows[0].delivery_revision == 1U);
    assert(!rows[0].delivered && !rows[0].acked);
    assert(rows[1].delivery_state == D1L_DM_DELIVERY_ACKNOWLEDGED);
    assert(rows[1].delivery_reason == D1L_DM_DELIVERY_REASON_LEGACY_IMPORT);
    assert(rows[1].delivery_last_error == ESP_OK);
    assert(rows[1].delivery_session_id != 0U);
    assert(rows[1].delivery_session_id != rows[0].delivery_session_id);
    assert(rows[1].delivery_revision == 1U);
    assert(rows[1].acked && rows[1].delivered);
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(((const test_blob_v7_t *)s_nvs.data)->schema == TEST_SCHEMA_V7);
}

static d1l_dm_store_append_outcome_t append_awaiting_ack(
    uint32_t ack_hash, const char *text)
{
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_tx(
               "0123456789abcdef", "Node", text, -50, 40,
               1U, 1U, 0U, ack_hash, &append) == ESP_OK);
    assert(append.inserted && append.durable);
    assert(d1l_dm_store_transition_delivery(
               append.delivery_session_id, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_WAITING_RADIO,
               D1L_DM_DELIVERY_REASON_RADIO_RESERVED, ESP_OK, NULL) ==
           ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               append.delivery_session_id,
               D1L_DM_DELIVERY_WAITING_RADIO, 2U,
               D1L_DM_DELIVERY_TX_ACTIVE,
               D1L_DM_DELIVERY_REASON_RADIO_STARTED, ESP_OK, NULL) ==
           ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               append.delivery_session_id, D1L_DM_DELIVERY_TX_ACTIVE, 3U,
               D1L_DM_DELIVERY_TX_DONE,
               D1L_DM_DELIVERY_REASON_RADIO_COMPLETED, ESP_OK, NULL) ==
           ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               append.delivery_session_id, D1L_DM_DELIVERY_TX_DONE, 4U,
               D1L_DM_DELIVERY_AWAITING_ACK,
               D1L_DM_DELIVERY_REASON_ACK_EXPECTED, ESP_OK, NULL) ==
           ESP_OK);
    return append;
}

static void append_inbound_rows(size_t count, const char *prefix)
{
    char text[48] = {0};
    for (size_t i = 0U; i < count; ++i) {
        (void)snprintf(text, sizeof(text), "%s-%lu", prefix,
                       (unsigned long)i);
        assert(d1l_dm_store_append(
                   "fedcba9876543210", "Peer", "rx", text, -60, 30,
                   1U, 1U, 0U, true, false, (uint32_t)i + 1U) == ESP_OK);
    }
}

static void test_full_ring_preserves_nonterminal_tx_until_exact_terminal(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    const d1l_dm_store_append_outcome_t first =
        append_awaiting_ack(0xB101U, "protected-awaiting-ack");
    append_inbound_rows(D1L_DM_STORE_CAPACITY - 1U, "fill-protected");

    const d1l_dm_store_stats_t before = d1l_dm_store_stats();
    assert(before.count == D1L_DM_STORE_CAPACITY);
    assert(before.dropped_oldest == 0U);
    const mock_blob_t durable_before = s_nvs;
    const uint32_t nvs_writes_before = s_nvs_write_count;
    const size_t operations_before = s_operation_count;

    const uint8_t identity[D1L_DM_IDENTITY_DIGEST_BYTES] = {0xB1U};
    d1l_dm_store_append_outcome_t rejected = {
        .inserted = true,
        .durable = true,
        .row_seq = UINT32_MAX,
        .delivery_session_id = UINT64_MAX,
        .error = ESP_OK,
    };
    assert(d1l_dm_store_append_rx_identity(
               "aaaaaaaaaaaaaaaa", "Blocked", "must-not-evict", -61, 20,
               1U, 1U, 0U, 0xB102U, identity, &rejected) ==
           ESP_ERR_NO_MEM);
    assert(!rejected.inserted && !rejected.durable);
    assert(rejected.row_seq == 0U && rejected.delivery_session_id == 0U);
    assert(rejected.error == ESP_ERR_NO_MEM);

    const d1l_dm_store_stats_t after = d1l_dm_store_stats();
    assert(after.next_seq == before.next_seq);
    assert(after.total_written == before.total_written);
    assert(after.dropped_oldest == before.dropped_oldest);
    assert(after.content_revision == before.content_revision);
    assert(after.persistence_revision == before.persistence_revision);
    assert(after.persistence_commit_count == before.persistence_commit_count);
    assert(after.persistence_fail_count == before.persistence_fail_count);
    assert(after.count == before.count);
    assert(after.persistence_dirty == before.persistence_dirty);
    assert(s_nvs_write_count == nvs_writes_before);
    assert(s_operation_count == operations_before);
    assert(memcmp(&s_nvs, &durable_before, sizeof(s_nvs)) == 0);

    d1l_dm_entry_t protected = {0};
    assert(d1l_dm_store_find_delivery_session(
        first.delivery_session_id, &protected));
    assert(protected.delivery_state == D1L_DM_DELIVERY_AWAITING_ACK);
    assert(protected.delivery_revision == 5U);

    d1l_dm_delivery_transition_outcome_t acknowledged = {0};
    assert(d1l_dm_store_transition_delivery(
               first.delivery_session_id,
               D1L_DM_DELIVERY_AWAITING_ACK, 5U,
               D1L_DM_DELIVERY_ACKNOWLEDGED,
               D1L_DM_DELIVERY_REASON_ACK_RECEIVED, ESP_OK,
               &acknowledged) == ESP_OK);
    assert(acknowledged.changed && acknowledged.durable);
    assert(acknowledged.delivery_revision == 6U);
    const test_blob_v6_t *durable = (const test_blob_v6_t *)s_nvs.data;
    assert(durable->entries[0].delivery_session_id ==
           first.delivery_session_id);
    assert(durable->entries[0].delivery_state ==
           D1L_DM_DELIVERY_ACKNOWLEDGED);
    assert(durable->entries[0].delivery_revision == 6U);

    d1l_dm_store_append_outcome_t later = {0};
    assert(d1l_dm_store_append_tx(
               "bbbbbbbbbbbbbbbb", "Later", "after-terminal", -50, 40,
               1U, 1U, 0U, 0xB103U, &later) == ESP_OK);
    assert(later.inserted && later.durable);
    d1l_dm_entry_t evicted = {0};
    assert(!d1l_dm_store_find_delivery_session(
        first.delivery_session_id, &evicted));
    d1l_dm_delivery_transition_outcome_t proceeding = {0};
    assert(d1l_dm_store_transition_delivery(
               later.delivery_session_id, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_WAITING_RADIO,
               D1L_DM_DELIVERY_REASON_RADIO_RESERVED, ESP_OK,
               &proceeding) == ESP_OK);
    assert(proceeding.changed && proceeding.durable);
    assert(proceeding.delivery_revision == 2U);
}

static void test_full_ring_terminal_head_still_evicts_normally(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_store_append_outcome_t terminal = {0};
    assert(d1l_dm_store_append_tx(
               "0123456789abcdef", "Node", "terminal-head", -50, 40,
               1U, 1U, 0U, 0xB201U, &terminal) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               terminal.delivery_session_id, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_FAILED_QUEUE,
               D1L_DM_DELIVERY_REASON_QUEUE_REJECTED, ESP_FAIL, NULL) ==
           ESP_OK);
    append_inbound_rows(D1L_DM_STORE_CAPACITY - 1U, "fill-terminal");

    const d1l_dm_store_stats_t before = d1l_dm_store_stats();
    assert(before.count == D1L_DM_STORE_CAPACITY);
    const uint8_t identity[D1L_DM_IDENTITY_DIGEST_BYTES] = {0xB2U};
    d1l_dm_store_append_outcome_t appended = {0};
    assert(d1l_dm_store_append_rx_identity(
               "cccccccccccccccc", "Replacement", "terminal-evicted",
               -62, 10, 1U, 1U, 0U, 0xB202U, identity, &appended) ==
           ESP_OK);
    assert(appended.inserted && appended.durable);
    const d1l_dm_store_stats_t after = d1l_dm_store_stats();
    assert(after.count == D1L_DM_STORE_CAPACITY);
    assert(after.next_seq == before.next_seq + 1U);
    assert(after.total_written == before.total_written + 1U);
    assert(after.dropped_oldest == before.dropped_oldest + 1U);
    assert(after.content_revision == before.content_revision + 1U);
    d1l_dm_entry_t evicted = {0};
    assert(!d1l_dm_store_find_delivery_session(
        terminal.delivery_session_id, &evicted));
    d1l_dm_entry_t newest = {0};
    assert(d1l_dm_store_copy_recent(&newest, 1U) == 1U);
    assert(strcmp(newest.text, "terminal-evicted") == 0);
}

static void test_ack_persistence_failure_recovers_same_revision_durably(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    const d1l_dm_store_append_outcome_t append =
        append_awaiting_ack(0xACACU, "ack-persistence-retry");

    s_nvs_write_error = ESP_FAIL;
    d1l_dm_delivery_transition_outcome_t failed = {0};
    assert(d1l_dm_store_transition_delivery(
               append.delivery_session_id,
               D1L_DM_DELIVERY_AWAITING_ACK, 5U,
               D1L_DM_DELIVERY_ACKNOWLEDGED,
               D1L_DM_DELIVERY_REASON_ACK_RECEIVED, ESP_OK, &failed) ==
           ESP_FAIL);
    assert(failed.changed && !failed.durable && !failed.persistence_retry);
    assert(failed.current_state == D1L_DM_DELIVERY_ACKNOWLEDGED);
    assert(failed.delivery_revision == 6U);

    /* Failed persistence is not public delivery truth. The owner can retain
     * the same awaiting-ACK state/revision and retry the exact transition. */
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.delivery_state == D1L_DM_DELIVERY_AWAITING_ACK);
    assert(row.delivery_revision == 5U);
    assert(!row.acked && !row.delivered);
    d1l_dm_entry_t session_truth = {0};
    assert(d1l_dm_store_find_delivery_session(
        append.delivery_session_id, &session_truth));
    assert(session_truth.delivery_state == D1L_DM_DELIVERY_AWAITING_ACK);
    assert(session_truth.delivery_revision == 5U);
    assert(!session_truth.acked && !session_truth.delivered);

    s_nvs_write_error = ESP_OK;
    s_now_us +=
        (int64_t)D1L_DM_STORE_PERSIST_RETRY_INTERVAL_MS * 1000LL;
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.delivery_state == D1L_DM_DELIVERY_ACKNOWLEDGED);
    assert(row.delivery_revision == 6U);
    assert(row.acked && row.delivered);
    memset(&session_truth, 0, sizeof(session_truth));
    assert(d1l_dm_store_find_delivery_session(
        append.delivery_session_id, &session_truth));
    assert(session_truth.delivery_state == D1L_DM_DELIVERY_ACKNOWLEDGED);
    assert(session_truth.delivery_revision == 6U);
    assert(session_truth.acked && session_truth.delivered);

    /* A background flush may make the ACK durable before the service owner
     * sees another copy of the packet. Replaying the same expected tuple is
     * an idempotent publication of revision 6, never a second transition. */
    d1l_dm_delivery_transition_outcome_t recovered = {0};
    assert(d1l_dm_store_transition_delivery(
               append.delivery_session_id,
               D1L_DM_DELIVERY_AWAITING_ACK, 5U,
               D1L_DM_DELIVERY_ACKNOWLEDGED,
               D1L_DM_DELIVERY_REASON_ACK_RECEIVED, ESP_OK, &recovered) ==
           ESP_OK);
    assert(!recovered.changed && recovered.persistence_retry);
    assert(recovered.durable);
    assert(recovered.previous_state == D1L_DM_DELIVERY_AWAITING_ACK);
    assert(recovered.current_state == D1L_DM_DELIVERY_ACKNOWLEDGED);
    assert(recovered.delivery_revision == 6U);

    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.delivery_state == D1L_DM_DELIVERY_ACKNOWLEDGED);
    assert(row.delivery_revision == 6U);
    assert(row.acked && row.delivered);
}

static void test_deferred_delivery_ack_waits_for_retained_flush(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    const d1l_dm_store_append_outcome_t append =
        append_awaiting_ack(0xAEAEU, "deferred-delivery-ack");
    const uint32_t sd_writes_before = s_sd_write_count;
    const uint32_t nvs_writes_before = s_nvs_write_count;
    const size_t operations_before = s_operation_count;

    d1l_dm_delivery_transition_outcome_t admitted = {0};
    assert(d1l_dm_store_transition_delivery_deferred(
               append.delivery_session_id,
               D1L_DM_DELIVERY_AWAITING_ACK, 5U,
               D1L_DM_DELIVERY_ACKNOWLEDGED,
               D1L_DM_DELIVERY_REASON_ACK_RECEIVED, ESP_OK, &admitted) ==
           ESP_OK);
    assert(admitted.changed && !admitted.durable);
    assert(!admitted.persistence_retry && admitted.error == ESP_OK);
    assert(admitted.current_state == D1L_DM_DELIVERY_ACKNOWLEDGED);
    assert(admitted.delivery_revision == 6U);
    assert(s_sd_write_count == sd_writes_before);
    assert(s_nvs_write_count == nvs_writes_before);
    assert(s_operation_count == operations_before);
    assert(d1l_dm_store_stats().persistence_dirty);

    /* Until the worker persists the admitted revision, all public store
     * queries retain the exact AWAITING_ACK owner truth. */
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_find_delivery_session(
        append.delivery_session_id, &row));
    assert(row.delivery_state == D1L_DM_DELIVERY_AWAITING_ACK);
    assert(row.delivery_revision == 5U);
    assert(!row.acked && !row.delivered);

    d1l_dm_delivery_transition_outcome_t replay = {0};
    assert(d1l_dm_store_transition_delivery_deferred(
               append.delivery_session_id,
               D1L_DM_DELIVERY_AWAITING_ACK, 5U,
               D1L_DM_DELIVERY_ACKNOWLEDGED,
               D1L_DM_DELIVERY_REASON_ACK_RECEIVED, ESP_OK, &replay) ==
           ESP_OK);
    assert(!replay.changed && replay.persistence_retry && !replay.durable);
    assert(replay.delivery_revision == 6U);
    assert(s_sd_write_count == sd_writes_before);
    assert(s_nvs_write_count == nvs_writes_before);
    assert(s_operation_count == operations_before);

    /* This is the same non-forced entry point used by the retained worker. */
    assert(d1l_dm_store_flush_if_due() == ESP_OK);
    assert(s_sd_write_count == sd_writes_before);
    assert(s_nvs_write_count == nvs_writes_before + 1U);
    assert(!d1l_dm_store_stats().persistence_dirty);
    assert(d1l_dm_store_find_delivery_session(
        append.delivery_session_id, &row));
    assert(row.delivery_state == D1L_DM_DELIVERY_ACKNOWLEDGED);
    assert(row.delivery_revision == 6U);
    assert(row.acked && row.delivered);
}

static void test_failed_ack_reboot_never_invents_delivery(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    const d1l_dm_store_append_outcome_t append =
        append_awaiting_ack(0xADADU, "ack-failure-reboot");

    s_nvs_write_error = ESP_FAIL;
    d1l_dm_delivery_transition_outcome_t failed = {0};
    assert(d1l_dm_store_transition_delivery(
               append.delivery_session_id,
               D1L_DM_DELIVERY_AWAITING_ACK, 5U,
               D1L_DM_DELIVERY_ACKNOWLEDGED,
               D1L_DM_DELIVERY_REASON_ACK_RECEIVED, ESP_OK, &failed) ==
           ESP_FAIL);
    assert(failed.changed && !failed.durable);

    /* Reboot reloads the last durable awaiting state and normalizes it as an
     * interruption. It must never resurrect the volatile ACK projection. */
    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.delivery_state ==
           D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT);
    assert(row.delivery_revision == 6U);
    assert(!row.acked && !row.delivered);
}

static void test_delivery_transition_cas_survives_reboot_truthfully(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_tx(
               "0123456789abcdef", "Node", "stateful", -50, 40,
               1U, 1U, 0U, 0xABCDU, &append) == ESP_OK);
    assert(append.inserted && append.durable);
    assert(append.row_seq == 1U);
    assert(append.delivery_session_id != 0U);

    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.seq == 1U);
    assert(row.delivery_session_id == append.delivery_session_id);
    assert(row.delivery_state == D1L_DM_DELIVERY_QUEUED);
    assert(row.delivery_reason == D1L_DM_DELIVERY_REASON_ENQUEUED);
    assert(row.delivery_revision == 1U);
    assert(!row.delivered && !row.acked);

    d1l_dm_delivery_transition_outcome_t outcome = {0};
    assert(d1l_dm_store_transition_delivery(
               row.delivery_session_id, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_WAITING_RADIO,
               D1L_DM_DELIVERY_REASON_RADIO_RESERVED, ESP_OK,
               &outcome) == ESP_OK);
    assert(outcome.changed && outcome.durable);
    assert(outcome.delivery_session_id == row.delivery_session_id);
    assert(outcome.row_seq == row.seq);
    assert(outcome.previous_state == D1L_DM_DELIVERY_QUEUED);
    assert(outcome.current_state == D1L_DM_DELIVERY_WAITING_RADIO);
    assert(outcome.delivery_revision == 2U);

    memset(&outcome, 0, sizeof(outcome));
    assert(d1l_dm_store_transition_delivery(
               row.delivery_session_id, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_TX_ACTIVE,
               D1L_DM_DELIVERY_REASON_RADIO_STARTED, ESP_OK,
               &outcome) == ESP_ERR_INVALID_STATE);
    assert(!outcome.changed && !outcome.durable);
    assert(outcome.row_seq == row.seq);
    assert(outcome.previous_state == D1L_DM_DELIVERY_WAITING_RADIO);
    assert(outcome.delivery_revision == 2U);

    assert(d1l_dm_store_transition_delivery(
               row.delivery_session_id, D1L_DM_DELIVERY_WAITING_RADIO, 2U,
               D1L_DM_DELIVERY_TX_ACTIVE,
               D1L_DM_DELIVERY_REASON_RADIO_STARTED, ESP_OK,
               NULL) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.delivery_state ==
           D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT);
    assert(row.delivery_reason == D1L_DM_DELIVERY_REASON_REBOOT_RECOVERY);
    assert(row.delivery_last_error == D1L_DM_DELIVERY_INTERRUPTED_ERROR);
    assert(row.delivery_revision == 4U);

    assert(d1l_dm_store_transition_delivery(
               row.delivery_session_id,
               D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT, 4U,
               D1L_DM_DELIVERY_RETRY_WAIT,
               D1L_DM_DELIVERY_REASON_RETRY_SCHEDULED,
               D1L_DM_DELIVERY_INTERRUPTED_ERROR, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               row.delivery_session_id, D1L_DM_DELIVERY_RETRY_WAIT, 5U,
               D1L_DM_DELIVERY_RETRY_TX,
               D1L_DM_DELIVERY_REASON_RETRY_STARTED, ESP_OK,
               NULL) == ESP_OK);
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.delivery_retry_count == 1U);
    assert(row.attempt == 1U);

    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.delivery_state ==
           D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT);
    assert(row.delivery_retry_count == 1U);
    assert(row.delivery_revision == 7U);

    assert(d1l_dm_store_mark_acked(0xABCDU, &row) == ESP_OK);
    assert(row.delivery_state == D1L_DM_DELIVERY_ACKNOWLEDGED);
    assert(row.delivery_reason == D1L_DM_DELIVERY_REASON_ACK_RECEIVED);
    assert(row.delivery_last_error == ESP_OK);
    assert(row.delivery_revision == 8U);
    assert(row.delivered && row.acked);
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.delivery_state == D1L_DM_DELIVERY_ACKNOWLEDGED);
    assert(row.delivery_revision == 8U);
}

static void test_v5_dual_backend_migration_session_id_is_deterministic(void)
{
    reset_backend();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    test_blob_v5_t nvs = {
        .schema = TEST_SCHEMA_V5,
        .next_seq = 2U,
        .total_written = 1U,
        .head = 1U,
        .count = 1U,
        .epoch = 1U,
        .content_revision = 3U,
    };
    nvs.entries[0] = make_v5_entry(
        1U, "same-migrated-session", "tx", 0x3030U, false);
    test_blob_v5_t sd = nvs;
    sd.next_seq = 8U;
    sd.entries[0].seq = 7U;
    sd.content_revision = 4U;
    assert(blob_write(&s_nvs, &nvs, sizeof(nvs)) == ESP_OK);
    assert(blob_write(&s_sd, &sd, sizeof(sd)) == ESP_OK);

    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_stats().count == 1U);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.delivery_session_id != 0U);
    assert(!row.delivered && !row.acked);
    assert(d1l_dm_store_flush() == ESP_OK);
    const test_blob_v6_t *nvs_v6 = (const test_blob_v6_t *)s_nvs.data;
    const test_blob_v6_t *sd_v6 = (const test_blob_v6_t *)s_sd.data;
    assert(nvs_v6->schema == TEST_SCHEMA_V7);
    assert(sd_v6->schema == TEST_SCHEMA_V7);
    assert(nvs_v6->entries[0].delivery_session_id == row.delivery_session_id);
    assert(sd_v6->entries[0].delivery_session_id == row.delivery_session_id);
}

static void test_v5_migration_session_collision_fails_closed(void)
{
    reset_backend();
    test_blob_v5_t collision = {
        .schema = TEST_SCHEMA_V5,
        .next_seq = 3U,
        .total_written = 2U,
        .head = 2U,
        .count = 2U,
        .epoch = 1U,
        .content_revision = 3U,
    };
    collision.entries[0] = make_v5_entry(
        1U, "migration-collision", "tx", 0x3131U, false);
    collision.entries[1] = collision.entries[0];
    collision.entries[1].seq = 2U;
    assert(blob_write(&s_nvs, &collision, sizeof(collision)) == ESP_OK);
    assert(d1l_dm_store_init() == ESP_ERR_INVALID_STATE);
    assert(!d1l_dm_store_stats().loaded);
    assert(s_nvs_write_count == 0U);
}

static void test_late_sd_resequence_transitions_by_stable_session_id(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_tx(
               "0123456789abcdef", "Node", "resequence-session", -50, 40,
               1U, 1U, 0U, 0x4040U, &append) == ESP_OK);
    const uint32_t original_row_seq = append.row_seq;
    const uint64_t session_id = append.delivery_session_id;
    assert(original_row_seq == 1U && session_id != 0U);

    seed_v3(&s_sd, 2U, 20U, "replacement-card-row");
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_dm_store_flush() == ESP_OK);

    d1l_dm_entry_t rows[D1L_DM_STORE_CAPACITY] = {0};
    const size_t count = d1l_dm_store_copy_recent(
        rows, D1L_DM_STORE_CAPACITY);
    const d1l_dm_entry_t *resequence = NULL;
    for (size_t i = 0U; i < count; ++i) {
        if (rows[i].delivery_session_id == session_id) {
            resequence = &rows[i];
            break;
        }
    }
    assert(resequence != NULL);
    assert(resequence->seq != original_row_seq);
    assert(resequence->delivery_state == D1L_DM_DELIVERY_QUEUED);
    assert(resequence->delivery_revision == 1U);

    d1l_dm_delivery_transition_outcome_t outcome = {0};
    assert(d1l_dm_store_transition_delivery(
               session_id, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_WAITING_RADIO,
               D1L_DM_DELIVERY_REASON_RADIO_RESERVED, ESP_OK,
               &outcome) == ESP_OK);
    assert(outcome.changed && outcome.durable);
    assert(outcome.delivery_session_id == session_id);
    assert(outcome.row_seq == resequence->seq);
    assert(outcome.row_seq != original_row_seq);
    assert(outcome.delivery_revision == 2U);
}

static void test_delivery_transition_rejects_aba_stale_revision(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_tx(
               "0123456789abcdef", "Node", "aba", -50, 40,
               1U, 1U, 0U, 0x5050U, &append) == ESP_OK);
    const uint64_t session_id = append.delivery_session_id;

    assert(d1l_dm_store_transition_delivery(
               session_id, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_FAILED_QUEUE,
               D1L_DM_DELIVERY_REASON_QUEUE_REJECTED, ESP_FAIL,
               NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session_id, D1L_DM_DELIVERY_FAILED_QUEUE, 2U,
               D1L_DM_DELIVERY_QUEUED,
               D1L_DM_DELIVERY_REASON_USER_RETRY, ESP_OK,
               NULL) == ESP_OK);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.attempt == 1U && row.delivery_retry_count == 1U);

    d1l_dm_delivery_transition_outcome_t stale = {0};
    assert(d1l_dm_store_transition_delivery(
               session_id, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_WAITING_RADIO,
               D1L_DM_DELIVERY_REASON_RADIO_RESERVED, ESP_OK,
               &stale) == ESP_ERR_INVALID_STATE);
    assert(!stale.changed && !stale.durable);
    assert(stale.delivery_session_id == session_id);
    assert(stale.row_seq == append.row_seq);
    assert(stale.previous_state == D1L_DM_DELIVERY_QUEUED);
    assert(stale.current_state == D1L_DM_DELIVERY_QUEUED);
    assert(stale.delivery_revision == 3U);

    assert(d1l_dm_store_transition_delivery(
               session_id, D1L_DM_DELIVERY_QUEUED, 3U,
               D1L_DM_DELIVERY_WAITING_RADIO,
               D1L_DM_DELIVERY_REASON_RADIO_RESERVED, ESP_OK,
               NULL) == ESP_OK);
}

static void test_retry_attempt_advances_once_and_fails_closed_at_overflow(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_tx(
               "0123456789abcdef", "Node", "retry-matrix", -50, 40,
               1U, 1U, 0U, 0x6060U, &append) == ESP_OK);
    const uint64_t session_id = append.delivery_session_id;
    assert(d1l_dm_store_transition_delivery(
               session_id, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_FAILED_QUEUE,
               D1L_DM_DELIVERY_REASON_QUEUE_REJECTED, ESP_FAIL,
               NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session_id, D1L_DM_DELIVERY_FAILED_QUEUE, 2U,
               D1L_DM_DELIVERY_RETRY_WAIT,
               D1L_DM_DELIVERY_REASON_RETRY_SCHEDULED, ESP_FAIL,
               NULL) == ESP_OK);
    d1l_dm_delivery_transition_outcome_t retry = {0};
    assert(d1l_dm_store_transition_delivery_retry(
               session_id, 3U, 0x6061U, &retry) == ESP_OK);
    assert(retry.changed && retry.durable);
    assert(retry.attempt == 1U && retry.retry_count == 1U);
    assert(retry.ack_hash == 0x6061U);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.attempt == 1U && row.delivery_retry_count == 1U);
    assert(row.ack_hash == 0x6061U);

    assert(d1l_dm_store_transition_delivery(
               session_id, D1L_DM_DELIVERY_RETRY_TX, 4U,
               D1L_DM_DELIVERY_FAILED_RADIO,
               D1L_DM_DELIVERY_REASON_RADIO_ERROR, ESP_FAIL,
               NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session_id, D1L_DM_DELIVERY_FAILED_RADIO, 5U,
               D1L_DM_DELIVERY_RETRY_WAIT,
               D1L_DM_DELIVERY_REASON_RETRY_SCHEDULED, ESP_FAIL,
               NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session_id, D1L_DM_DELIVERY_RETRY_WAIT, 6U,
               D1L_DM_DELIVERY_RETRY_TX,
               D1L_DM_DELIVERY_REASON_RETRY_STARTED, ESP_OK,
               NULL) == ESP_OK);
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.attempt == 2U && row.delivery_retry_count == 2U);

    d1l_dm_store_append_outcome_t overflow = {0};
    assert(d1l_dm_store_append_tx(
               "fedcba9876543210", "Node2", "retry-overflow", -50, 40,
               1U, 1U, UINT8_MAX, 0x6161U, &overflow) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               overflow.delivery_session_id, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_FAILED_QUEUE,
               D1L_DM_DELIVERY_REASON_QUEUE_REJECTED, ESP_FAIL,
               NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               overflow.delivery_session_id,
               D1L_DM_DELIVERY_FAILED_QUEUE, 2U,
               D1L_DM_DELIVERY_RETRY_WAIT,
               D1L_DM_DELIVERY_REASON_RETRY_SCHEDULED, ESP_FAIL,
               NULL) == ESP_OK);
    d1l_dm_delivery_transition_outcome_t rejected = {0};
    assert(d1l_dm_store_transition_delivery(
               overflow.delivery_session_id,
               D1L_DM_DELIVERY_RETRY_WAIT, 3U,
               D1L_DM_DELIVERY_RETRY_TX,
               D1L_DM_DELIVERY_REASON_RETRY_STARTED, ESP_OK,
               &rejected) == ESP_ERR_INVALID_STATE);
    assert(!rejected.changed);
    assert(rejected.delivery_revision == 3U);
    assert(d1l_dm_store_copy_recent(&row, 1U) == 1U);
    assert(row.attempt == UINT8_MAX && row.delivery_retry_count == 0U);
}

static void test_direct_timeout_flood_retry_then_final_timeout_is_one_session(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_tx(
               "0123456789abcdef", "Node", "one-flood-retry", -50, 40,
               1U, 2U, 0U, 0x7000U, &append) == ESP_OK);
    const uint64_t session = append.delivery_session_id;
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_WAITING_RADIO,
               D1L_DM_DELIVERY_REASON_RADIO_RESERVED, ESP_OK, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_WAITING_RADIO, 2U,
               D1L_DM_DELIVERY_TX_ACTIVE,
               D1L_DM_DELIVERY_REASON_RADIO_STARTED, ESP_OK, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_TX_ACTIVE, 3U,
               D1L_DM_DELIVERY_TX_DONE,
               D1L_DM_DELIVERY_REASON_RADIO_COMPLETED, ESP_OK, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_TX_DONE, 4U,
               D1L_DM_DELIVERY_AWAITING_ACK,
               D1L_DM_DELIVERY_REASON_ACK_EXPECTED, ESP_OK, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_AWAITING_ACK, 5U,
               D1L_DM_DELIVERY_RETRY_WAIT,
               D1L_DM_DELIVERY_REASON_RETRY_SCHEDULED,
               ESP_ERR_TIMEOUT, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery_retry(
               session, 6U, 0x7001U, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_RETRY_TX, 7U,
               D1L_DM_DELIVERY_WAITING_RADIO,
               D1L_DM_DELIVERY_REASON_RADIO_RESERVED, ESP_OK, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_WAITING_RADIO, 8U,
               D1L_DM_DELIVERY_TX_ACTIVE,
               D1L_DM_DELIVERY_REASON_RADIO_STARTED, ESP_OK, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_TX_ACTIVE, 9U,
               D1L_DM_DELIVERY_TX_DONE,
               D1L_DM_DELIVERY_REASON_RADIO_COMPLETED, ESP_OK, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_TX_DONE, 10U,
               D1L_DM_DELIVERY_AWAITING_ACK,
               D1L_DM_DELIVERY_REASON_ACK_EXPECTED, ESP_OK, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_AWAITING_ACK, 11U,
               D1L_DM_DELIVERY_FAILED_TIMEOUT,
               D1L_DM_DELIVERY_REASON_ACK_TIMEOUT,
               ESP_ERR_TIMEOUT, NULL) == ESP_OK);

    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_find_delivery_session(session, &row));
    assert(row.delivery_state == D1L_DM_DELIVERY_FAILED_TIMEOUT);
    assert(row.delivery_revision == 12U);
    assert(row.attempt == 1U && row.delivery_retry_count == 1U);
    assert(row.ack_hash == 0x7001U);
    assert(!row.acked && !row.delivered);
}

static void test_awaiting_ack_owner_survives_transition_persistence_failure(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_store_append_outcome_t append = {0};
    assert(d1l_dm_store_append_tx(
               "0123456789abcdef", "Node", "txdone-persist-failure",
               -50, 40, 1U, 1U, 0U, 0x7100U, &append) == ESP_OK);
    const uint64_t session = append.delivery_session_id;
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_QUEUED, 1U,
               D1L_DM_DELIVERY_WAITING_RADIO,
               D1L_DM_DELIVERY_REASON_RADIO_RESERVED, ESP_OK, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_WAITING_RADIO, 2U,
               D1L_DM_DELIVERY_TX_ACTIVE,
               D1L_DM_DELIVERY_REASON_RADIO_STARTED, ESP_OK, NULL) == ESP_OK);
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_TX_ACTIVE, 3U,
               D1L_DM_DELIVERY_TX_DONE,
               D1L_DM_DELIVERY_REASON_RADIO_COMPLETED, ESP_OK, NULL) == ESP_OK);

    s_nvs_write_error = ESP_FAIL;
    d1l_dm_delivery_transition_outcome_t awaiting = {0};
    assert(d1l_dm_store_transition_delivery(
               session, D1L_DM_DELIVERY_TX_DONE, 4U,
               D1L_DM_DELIVERY_AWAITING_ACK,
               D1L_DM_DELIVERY_REASON_ACK_EXPECTED, ESP_OK, &awaiting) ==
           ESP_FAIL);
    assert(awaiting.changed && !awaiting.durable);
    assert(awaiting.current_state == D1L_DM_DELIVERY_AWAITING_ACK);
    assert(awaiting.delivery_session_id == session);
    assert(awaiting.delivery_revision == 5U);
    d1l_dm_entry_t row = {0};
    assert(d1l_dm_store_find_delivery_session(session, &row));
    assert(row.delivery_state == D1L_DM_DELIVERY_AWAITING_ACK);
    assert(row.delivery_revision == 5U);
    assert(d1l_dm_store_stats().persistence_dirty);

    s_nvs_write_error = ESP_OK;
    s_now_us +=
        (int64_t)D1L_DM_STORE_PERSIST_RETRY_INTERVAL_MS * 1000LL;
    assert(d1l_dm_store_flush() == ESP_OK);
    assert(!d1l_dm_store_stats().persistence_dirty);
}

static void test_utf8_round_trip_and_invalid_input_have_no_side_effects(void)
{
    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    const char mixed[] = {
        'C', 'a', 'f', (char)0xc3, (char)0xa9, ' ',
        (char)0xe6, (char)0x9d, (char)0xb1,
        (char)0xe4, (char)0xba, (char)0xac, ' ',
        (char)0xf0, (char)0x9f, (char)0x98, (char)0x80,
        ' ', '"', 'x', '\\', 'y', '"', '\0'
    };
    assert(d1l_dm_store_append(
               "0123456789abcdef", "Node", "rx", mixed, -50, 50,
               1U, 1U, 0U, true, false, 0U) == ESP_OK);

    const d1l_dm_store_stats_t before = d1l_dm_store_stats();
    const uint32_t nvs_writes_before = s_nvs_write_count;
    const char invalid[] = {(char)0xed, (char)0xa0, (char)0x80, '\0'};
    assert(d1l_dm_store_append(
               "0123456789abcdef", "Node", "rx", invalid, -50, 50,
               1U, 1U, 0U, true, false, 0U) == ESP_ERR_INVALID_ARG);
    char oversized[D1L_MESSAGE_TEXT_LEN + 1U];
    memset(oversized, 'q', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';
    assert(d1l_dm_store_append(
               "0123456789abcdef", "Node", "rx", oversized, -50, 50,
               1U, 1U, 0U, true, false, 0U) == ESP_ERR_INVALID_SIZE);
    const d1l_dm_store_stats_t after = d1l_dm_store_stats();
    assert(after.count == before.count);
    assert(after.total_written == before.total_written);
    assert(s_nvs_write_count == nvs_writes_before);

    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_entry_t restored = {0};
    assert(d1l_dm_store_copy_recent(&restored, 1U) == 1U);
    assert(memcmp(restored.text, mixed, sizeof(mixed)) == 0);
}

static void assert_query_did_not_mutate_store(d1l_dm_store_stats_t before,
                                              d1l_dm_store_stats_t after)
{
    assert(after.next_seq == before.next_seq);
    assert(after.total_written == before.total_written);
    assert(after.dropped_oldest == before.dropped_oldest);
    assert(after.epoch == before.epoch);
    assert(after.content_revision == before.content_revision);
    assert(after.clear_lineage == before.clear_lineage);
    assert(after.persistence_commit_count == before.persistence_commit_count);
    assert(after.persistence_fail_count == before.persistence_fail_count);
    assert(after.persistence_stale_snapshot_count ==
           before.persistence_stale_snapshot_count);
    assert(after.sd_primary_commit_count == before.sd_primary_commit_count);
    assert(after.sd_primary_fail_count == before.sd_primary_fail_count);
    assert(after.sd_backend_generation == before.sd_backend_generation);
    assert(after.sd_primary_last_error == before.sd_primary_last_error);
    assert(after.nvs_fallback_commit_count ==
           before.nvs_fallback_commit_count);
    assert(after.nvs_fallback_fail_count == before.nvs_fallback_fail_count);
    assert(after.nvs_fallback_last_error ==
           before.nvs_fallback_last_error);
    assert(after.persistence_revision == before.persistence_revision);
    assert(after.count == before.count);
    assert(after.capacity == before.capacity);
    assert(after.loaded == before.loaded);
    assert(after.persistence_dirty == before.persistence_dirty);
    assert(after.sd_primary_required == before.sd_primary_required);
    assert(after.sd_primary_dirty == before.sd_primary_dirty);
    assert(after.sd_primary_reconcile_pending ==
           before.sd_primary_reconcile_pending);
    assert(after.nvs_fallback_dirty == before.nvs_fallback_dirty);
}

static void test_thread_query_is_exact_paged_and_read_only(void)
{
    static const char fingerprint_a[] = "0123456789abcdef";
    static const char fingerprint_b[] = "fedcba9876543210";
    static const char utf8_text[] = {
        'C', 'a', 'f', (char)0xc3, (char)0xa9, ' ',
        (char)0xe6, (char)0x9d, (char)0xb1,
        (char)0xe4, (char)0xba, (char)0xac, '\0'
    };
    static const char utf8_query[] = {
        'c', 'A', 'F', (char)0xc3, (char)0xa9, ' ',
        (char)0xe6, (char)0x9d, (char)0xb1,
        (char)0xe4, (char)0xba, (char)0xac, '\0'
    };
    static const char different_non_ascii[] = {
        'c', 'a', 'f', (char)0xc3, (char)0x89, '\0'
    };

    reset_backend();
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_append(
               fingerprint_a, "Alpha Node", "rx", "Older Match",
               -60, 40, 1U, 1U, 0U, true, false, 0x8101U) == ESP_OK);
    assert(d1l_dm_store_append(
               fingerprint_b, "Beta Node", "rx", "Cross MATCH",
               -61, 30, 1U, 1U, 0U, true, false, 0x8102U) == ESP_OK);
    assert(d1l_dm_store_append(
               fingerprint_a, "Alpha Node", "tx", "middle row",
               -62, 20, 1U, 1U, 0U, true, false, 0x8103U) == ESP_OK);
    assert(d1l_dm_store_append(
               fingerprint_a, "Alpha Node", "rx", utf8_text,
               -63, 10, 1U, 1U, 0U, true, false, 0x8104U) == ESP_OK);
    assert(d1l_dm_store_append(
               fingerprint_a, "Alpha Node", "rx", "Newest mAtCh",
               -64, 0, 1U, 1U, 0U, true, false, 0x8105U) == ESP_OK);
    assert(d1l_dm_store_append_volatile(
               fingerprint_a, "Alpha Node", "rx", "Volatile MATCH",
               -65, -10, 1U, 1U, 0U, true, false, 0x8106U) == ESP_OK);

    const d1l_dm_store_stats_t before = d1l_dm_store_stats();
    const uint32_t sd_writes_before = s_sd_write_count;
    const uint32_t nvs_writes_before = s_nvs_write_count;
    const size_t operations_before = s_operation_count;
    d1l_dm_entry_t rows[6] = {0};
    size_t total = 99U;

    /* Newest-page selection happens over matching rows, but each page is
     * returned oldest-to-newest for chat rendering. The other fingerprint's
     * matching text must never leak into this conversation. */
    assert(d1l_dm_store_query_thread_page(
               fingerprint_a, rows, 2U, 0U, "mAtCh", &total) == 2U);
    assert(total == 3U);
    assert(strcmp(rows[0].text, "Newest mAtCh") == 0);
    assert(strcmp(rows[1].text, "Volatile MATCH") == 0);

    memset(rows, 0, sizeof(rows));
    assert(d1l_dm_store_query_thread_page(
               fingerprint_a, rows, 2U, 1U, "MATCH", &total) == 2U);
    assert(total == 3U);
    assert(strcmp(rows[0].text, "Older Match") == 0);
    assert(strcmp(rows[1].text, "Newest mAtCh") == 0);
    assert(d1l_dm_store_query_thread_page(
               fingerprint_a, rows, 1U, 3U, "match", &total) == 0U);
    assert(total == 3U);

    /* NULL/empty preserve the pre-search API exactly, including the optional
     * volatile row. */
    assert(d1l_dm_store_query_thread_page(
               fingerprint_a, rows, 6U, 0U, NULL, &total) == 5U);
    assert(total == 5U);
    size_t copied_total = 0U;
    d1l_dm_entry_t copied[6] = {0};
    assert(d1l_dm_store_copy_thread_page(
               fingerprint_a, copied, 6U, 0U, &copied_total) == 5U);
    assert(copied_total == total);
    assert(memcmp(rows, copied, 5U * sizeof(rows[0])) == 0);
    assert(d1l_dm_store_query_thread_page(
               fingerprint_a, rows, 6U, 0U, "", &total) == 5U);
    assert(total == 5U);

    /* ASCII letters fold, while every UTF-8 byte remains exact. */
    assert(d1l_dm_store_query_thread_page(
               fingerprint_a, rows, 6U, 0U, utf8_query, &total) == 1U);
    assert(total == 1U && memcmp(rows[0].text, utf8_text,
                                sizeof(utf8_text)) == 0);
    assert(d1l_dm_store_query_thread_page(
               fingerprint_a, rows, 6U, 0U, different_non_ascii,
               &total) == 0U);
    assert(total == 0U);
    assert(d1l_dm_store_query_thread_page(
               fingerprint_a, rows, 6U, 0U, "ALPHA NODE", &total) == 5U);
    assert(total == 5U);
    assert(d1l_dm_store_query_thread_page(
               fingerprint_a, rows, 6U, 0U, "CDEF", &total) == 5U);
    assert(total == 5U);
    assert(d1l_dm_store_query_thread_page(
               fingerprint_a, rows, 6U, 0U, "TX", &total) == 1U);
    assert(total == 1U && strcmp(rows[0].direction, "tx") == 0);

    total = 99U;
    assert(d1l_dm_store_query_thread_page(
               fingerprint_a, NULL, 1U, 0U, "match", &total) == 0U);
    assert(total == 0U);
    assert_query_did_not_mutate_store(before, d1l_dm_store_stats());
    assert(s_sd_write_count == sd_writes_before);
    assert(s_nvs_write_count == nvs_writes_before);
    assert(s_operation_count == operations_before);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "deferred-ack") == 0) {
        test_deferred_ack_lifecycle_writes_only_on_explicit_flush();
        puts("native DM deferred ACK persistence: ok");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "deferred-unloaded") == 0) {
        test_deferred_ack_unloaded_fails_without_storage_io();
        puts("native DM deferred ACK unloaded path: ok");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "deferred-delivery-ack") == 0) {
        test_deferred_delivery_ack_waits_for_retained_flush();
        puts("native DM deferred delivery ACK persistence: ok");
        return 0;
    }
    assert(argc == 1);
    test_late_ready_merges_before_primary_write();
    test_replacement_generation_forces_read_before_write();
    test_nvs_failure_after_sd_success_stays_retryable();
    test_read_failure_never_writes_sd_and_nvs_can_advance();
    test_corruption_and_generation_change_fail_closed();
    test_generation_change_after_guarded_write_stays_pending();
    test_generation_flip_after_merge_rolls_back_before_nvs_write();
    test_init_generation_flip_restores_nvs_before_loaded_commit();
    test_v2_migrates_without_init_erase();
    test_v2_non_ascii_text_fails_closed_without_migration();
    test_v3_migrates_without_inventing_clear_lineage();
    test_volatile_preview_never_marks_persistence_dirty();
    test_append_path_limits_match_reboot_validator();
    test_corrupt_only_source_blocks_append_without_overwrite();
    test_noncanonical_v4_and_v3_order_fail_closed();
    test_offline_append_survives_higher_epoch_late_card();
    test_valid_legacy_nvs_dominates_foreign_clear_lineage();
    test_sd_lineage_seeds_only_without_nvs_or_local_mutation();
    test_conflicting_full_lineages_keep_truthful_counters();
    test_rebooted_local_clear_lineage_dominates_foreign_cards();
    test_late_full_sd_overlay_counters_are_truthful();
    test_higher_epoch_replay_keeps_evicted_mutation_counters();
    test_empty_same_epoch_late_card_does_not_double_count_live_rows();
    test_exhausted_sequence_fails_closed_and_clear_rotates_lineage();
    test_higher_epoch_replay_exhaustion_preserves_both_copies();
    test_total_counter_exhaustion_rejects_append_without_writes();
    test_v4_migration_preserves_rows_as_legacy_unverified();
    test_literal_v4_full_ring_migrates_without_data_loss();
    test_schema_v6_narrow_preserves_all_metadata();
    test_schema_v6_wide_preserves_utf8_and_rewrites_sd();
    test_schema_v6_unknown_lengths_fail_closed_without_mutation();
    test_malformed_v5_ack_metadata_fails_closed();
    test_malformed_v6_delivery_metadata_fails_closed();
    test_append_failure_retains_one_identity_row_until_flush();
    test_tx_append_quiesce_yield_flushes_same_row_without_duplicate();
    test_partial_reservation_commit_never_rolls_back_or_sends_twice();
    test_deferred_ack_lifecycle_writes_only_on_explicit_flush();
    test_reboot_ack_state_mutations_are_bounded();
    test_txdone_persist_failure_blocks_until_flush_or_reboot();
    test_reconcile_ack_metadata_never_regresses();
    test_reconcile_same_digest_at_different_sequences_keeps_one_budget();
    test_pending_kind_rebind_preserves_the_reserved_budget();
    test_late_reconcile_resequence_keeps_pending_callback_budget();
    test_v5_migration_never_invents_delivery_success();
    test_full_ring_preserves_nonterminal_tx_until_exact_terminal();
    test_full_ring_terminal_head_still_evicts_normally();
    test_ack_persistence_failure_recovers_same_revision_durably();
    test_deferred_delivery_ack_waits_for_retained_flush();
    test_failed_ack_reboot_never_invents_delivery();
    test_delivery_transition_cas_survives_reboot_truthfully();
    test_v5_dual_backend_migration_session_id_is_deterministic();
    test_v5_migration_session_collision_fails_closed();
    test_late_sd_resequence_transitions_by_stable_session_id();
    test_delivery_transition_rejects_aba_stale_revision();
    test_retry_attempt_advances_once_and_fails_closed_at_overflow();
    test_direct_timeout_flood_retry_then_final_timeout_is_one_session();
    test_awaiting_ack_owner_survives_transition_persistence_failure();
    test_utf8_round_trip_and_invalid_input_have_no_side_effects();
    test_thread_query_is_exact_paged_and_read_only();
    puts("native DM retained durability: ok");
    return 0;
}
