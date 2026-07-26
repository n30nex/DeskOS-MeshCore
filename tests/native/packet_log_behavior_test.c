#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/semphr.h"
#include "hal/rp2040_bridge.h"
#include "mesh/packet_log.h"
#include "storage/retained_blob_store.h"

#define MOCK_BLOB_MAX 65536U
#define MOCK_HISTORY_SEGMENT_BYTES \
    (D1L_PACKET_LOG_SD_SEGMENT_CAPACITY * D1L_RP2040_FILE_CHUNK_MAX)
#define HISTORY_MAGIC 0x314B5044UL
#define HISTORY_SCHEMA 1U

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
    d1l_packet_log_entry_t entries[D1L_PACKET_LOG_NVS_FALLBACK_CAPACITY];
} fallback_blob_t;

typedef struct {
    uint32_t schema;
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t head;
    uint32_t count;
    d1l_packet_log_entry_t entries[D1L_PACKET_LOG_RAM_CAPACITY];
} primary_blob_t;

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t entry_len;
    uint32_t checksum;
    d1l_packet_log_entry_t entry;
} history_record_t;

static mock_blob_t s_sd_primary;
static mock_blob_t s_nvs_fallback;
static uint8_t s_history_segments[D1L_PACKET_LOG_SD_SEGMENT_COUNT]
                                 [MOCK_HISTORY_SEGMENT_BYTES];
static size_t s_history_segment_len[D1L_PACKET_LOG_SD_SEGMENT_COUNT];
static bool s_sd_enabled;
static uint32_t s_backend_generation;
static bool s_fail_nvs_write;
static esp_err_t s_history_read_error;
static bool s_history_read_no_card;
static bool s_bump_generation_after_history_read;
static bool s_bump_generation_after_sd_read;
static bool s_bump_generation_before_reconcile_apply;
static uint32_t s_backend_state_calls_after_sd_read;
static bool s_preapply_generation_bump_fired;
static uint32_t s_fail_backend_state_on_call;
static bool s_inject_append_during_primary_write;
static uint32_t s_bump_generation_after_delete_count;
static bool s_attempt_append_during_delete;
static bool s_append_during_delete_result;
static uint32_t s_enable_sd_on_lock_take_countdown;
static uint32_t s_fail_nonblocking_take_count;
static bool s_enable_sd_during_nvs_erase;
static uint32_t s_backend_state_calls;
static uint32_t s_sd_primary_reads;
static uint32_t s_nvs_reads;
static uint32_t s_history_reads;
static uint32_t s_sd_primary_writes;
static uint32_t s_nvs_writes;
static uint32_t s_sd_primary_erases;
static uint32_t s_nvs_erases;
static uint32_t s_journal_mutations;
static uint32_t s_journal_truncates;
static uint32_t s_delete_count;
static uint32_t s_first_mutation_read_count;
static int64_t s_now_us;
static bool s_worker_should_yield;

bool d1l_route_store_persistence_should_yield(void)
{
    return s_worker_should_yield;
}

static void mock_reset(void)
{
    memset(&s_sd_primary, 0, sizeof(s_sd_primary));
    memset(&s_nvs_fallback, 0, sizeof(s_nvs_fallback));
    memset(s_history_segments, 0, sizeof(s_history_segments));
    memset(s_history_segment_len, 0, sizeof(s_history_segment_len));
    s_sd_enabled = false;
    s_backend_generation = 0U;
    s_fail_nvs_write = false;
    s_history_read_error = ESP_OK;
    s_history_read_no_card = false;
    s_bump_generation_after_history_read = false;
    s_bump_generation_after_sd_read = false;
    s_bump_generation_before_reconcile_apply = false;
    s_backend_state_calls_after_sd_read = 0U;
    s_preapply_generation_bump_fired = false;
    s_fail_backend_state_on_call = 0U;
    s_inject_append_during_primary_write = false;
    s_bump_generation_after_delete_count = 0U;
    s_attempt_append_during_delete = false;
    s_append_during_delete_result = true;
    s_enable_sd_on_lock_take_countdown = 0U;
    s_fail_nonblocking_take_count = 0U;
    s_enable_sd_during_nvs_erase = false;
    s_backend_state_calls = 0U;
    s_sd_primary_reads = 0U;
    s_nvs_reads = 0U;
    s_history_reads = 0U;
    s_sd_primary_writes = 0U;
    s_nvs_writes = 0U;
    s_sd_primary_erases = 0U;
    s_nvs_erases = 0U;
    s_journal_mutations = 0U;
    s_journal_truncates = 0U;
    s_delete_count = 0U;
    s_first_mutation_read_count = 0U;
    s_now_us = 1000000LL;
    s_worker_should_yield = false;
}

static esp_err_t mock_blob_read(const mock_blob_t *blob, void *dst,
                                size_t *len_inout)
{
    if (!blob || !dst || !len_inout) {
        return ESP_ERR_INVALID_ARG;
    }
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

static esp_err_t mock_blob_write(mock_blob_t *blob, const void *src, size_t len)
{
    if (!blob || !src || len == 0U || len > sizeof(blob->data)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(blob->data, src, len);
    blob->len = len;
    blob->valid = true;
    return ESP_OK;
}

static d1l_packet_log_entry_t packet_entry(uint32_t seq, const char *prefix)
{
    d1l_packet_log_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.seq = seq;
    entry.uptime_ms = seq * 100U;
    (void)snprintf(entry.direction, sizeof(entry.direction), "rx");
    (void)snprintf(entry.kind, sizeof(entry.kind), "mesh");
    (void)snprintf(entry.note, sizeof(entry.note), "%s-%lu", prefix,
                   (unsigned long)seq);
    return entry;
}

static void seed_primary(mock_blob_t *target, uint32_t first_seq,
                         uint32_t count, uint32_t total_written,
                         const char *prefix)
{
    primary_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.schema = 3U;
    blob.next_seq = total_written + 1U;
    blob.total_written = total_written;
    blob.dropped_oldest = total_written > D1L_PACKET_LOG_RAM_CAPACITY ?
                          total_written - D1L_PACKET_LOG_RAM_CAPACITY : 0U;
    blob.count = count;
    blob.head = count % D1L_PACKET_LOG_RAM_CAPACITY;
    for (uint32_t i = 0U; i < count; ++i) {
        blob.entries[i] = packet_entry(first_seq + i, prefix);
    }
    assert(mock_blob_write(target, &blob, sizeof(blob)) == ESP_OK);
}

static void seed_fallback(uint32_t first_seq, uint32_t count,
                          uint32_t total_written, const char *prefix)
{
    fallback_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.schema = 3U;
    blob.next_seq = total_written + 1U;
    blob.total_written = total_written;
    blob.dropped_oldest = total_written > D1L_PACKET_LOG_RAM_CAPACITY ?
                          total_written - D1L_PACKET_LOG_RAM_CAPACITY : 0U;
    blob.count = count;
    blob.head = count % D1L_PACKET_LOG_NVS_FALLBACK_CAPACITY;
    for (uint32_t i = 0U; i < count; ++i) {
        blob.entries[i] = packet_entry(first_seq + i, prefix);
    }
    assert(mock_blob_write(&s_nvs_fallback, &blob, sizeof(blob)) == ESP_OK);
}

static void seed_v2_compact_primary(uint32_t first_seq, uint32_t count,
                                    uint32_t total_written,
                                    const char *prefix)
{
    fallback_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.schema = 2U;
    blob.next_seq = total_written + 1U;
    blob.total_written = total_written;
    blob.dropped_oldest = total_written > D1L_PACKET_LOG_RAM_CAPACITY ?
                          total_written - D1L_PACKET_LOG_RAM_CAPACITY : 0U;
    blob.count = count;
    blob.head = count % D1L_PACKET_LOG_NVS_FALLBACK_CAPACITY;
    for (uint32_t i = 0U; i < count; ++i) {
        blob.entries[i] = packet_entry(first_seq + i, prefix);
    }
    assert(mock_blob_write(&s_sd_primary, &blob, sizeof(blob)) == ESP_OK);
}

static uint32_t history_checksum(const d1l_packet_log_entry_t *entry)
{
    const uint8_t *data = (const uint8_t *)entry;
    uint32_t hash = 2166136261UL;
    for (size_t i = 0U; i < sizeof(*entry); ++i) {
        hash ^= data[i];
        hash *= 16777619UL;
    }
    return hash;
}

static void seed_history(uint32_t first_seq, uint32_t count,
                         const char *prefix)
{
    for (uint32_t seq = first_seq; seq < first_seq + count; ++seq) {
        const uint32_t segment =
            ((seq - 1U) / D1L_PACKET_LOG_SD_SEGMENT_CAPACITY) %
            D1L_PACKET_LOG_SD_SEGMENT_COUNT;
        const uint32_t offset =
            ((seq - 1U) % D1L_PACKET_LOG_SD_SEGMENT_CAPACITY) *
            (uint32_t)sizeof(history_record_t);
        history_record_t record;
        memset(&record, 0, sizeof(record));
        record.magic = HISTORY_MAGIC;
        record.schema = HISTORY_SCHEMA;
        record.entry_len = sizeof(record.entry);
        record.entry = packet_entry(seq, prefix);
        record.checksum = history_checksum(&record.entry);
        memcpy(s_history_segments[segment] + offset, &record, sizeof(record));
        if (s_history_segment_len[segment] < offset + sizeof(record)) {
            s_history_segment_len[segment] = offset + sizeof(record);
        }
    }
}

static primary_blob_t current_primary(void)
{
    primary_blob_t blob;
    assert(s_sd_primary.valid && s_sd_primary.len == sizeof(blob));
    memcpy(&blob, s_sd_primary.data, sizeof(blob));
    return blob;
}

static fallback_blob_t current_fallback(void)
{
    fallback_blob_t blob;
    assert(s_nvs_fallback.valid && s_nvs_fallback.len == sizeof(blob));
    memcpy(&blob, s_nvs_fallback.data, sizeof(blob));
    return blob;
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
    if (ticks_to_wait == 0U && s_fail_nonblocking_take_count > 0U) {
        s_fail_nonblocking_take_count--;
        return 0;
    }
    if (s_enable_sd_on_lock_take_countdown > 0U) {
        s_enable_sd_on_lock_take_countdown--;
        if (s_enable_sd_on_lock_take_countdown == 0U) {
            s_sd_enabled = true;
            s_backend_generation++;
        }
    }
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    (void)handle;
    return pdTRUE;
}

bool d1l_retained_blob_store_uses_sd(d1l_retained_blob_store_id_t store_id)
{
    return store_id == D1L_RETAINED_BLOB_STORE_PACKET_LOG && s_sd_enabled;
}

bool d1l_retained_blob_store_backend_state(
    d1l_retained_blob_store_id_t store_id,
    d1l_retained_blob_store_backend_state_t *out_state)
{
    if (store_id != D1L_RETAINED_BLOB_STORE_PACKET_LOG || !out_state) {
        return false;
    }
    s_backend_state_calls++;
    if (s_bump_generation_before_reconcile_apply &&
        s_sd_primary_reads > 0U) {
        s_backend_state_calls_after_sd_read++;
        if (s_backend_state_calls_after_sd_read == 2U) {
            s_backend_generation++;
            s_bump_generation_before_reconcile_apply = false;
            s_preapply_generation_bump_fired = true;
        }
    }
    if (s_fail_backend_state_on_call > 0U) {
        s_fail_backend_state_on_call--;
        if (s_fail_backend_state_on_call == 0U) {
            return false;
        }
    }
    out_state->enabled = s_sd_enabled;
    out_state->generation = s_backend_generation;
    return true;
}

esp_err_t d1l_retained_blob_store_sd_media_lineage_ready(
    d1l_retained_blob_store_id_t store_id,
    uint32_t expected_backend_generation, bool *out_ready)
{
    assert(store_id == D1L_RETAINED_BLOB_STORE_PACKET_LOG);
    if (!out_ready) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sd_enabled || expected_backend_generation != s_backend_generation) {
        *out_ready = false;
        return ESP_ERR_INVALID_STATE;
    }
    *out_ready = true;
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_complete_packet_reset_lineage(
    uint32_t expected_backend_generation)
{
    return s_sd_enabled && expected_backend_generation == s_backend_generation ?
        ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t d1l_retained_blob_store_read_sd_primary(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    (void)key;
    if (store_id != D1L_RETAINED_BLOB_STORE_PACKET_LOG || !s_sd_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sd_primary_reads++;
    const esp_err_t ret = mock_blob_read(&s_sd_primary, dst, len_inout);
    if (s_bump_generation_after_sd_read) {
        s_bump_generation_after_sd_read = false;
        s_backend_generation++;
    }
    return ret;
}

esp_err_t d1l_retained_blob_store_read_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    (void)key;
    if (store_id != D1L_RETAINED_BLOB_STORE_PACKET_LOG) {
        return ESP_ERR_INVALID_ARG;
    }
    s_nvs_reads++;
    return mock_blob_read(&s_nvs_fallback, dst, len_inout);
}

static void note_sd_mutation(void)
{
    if (s_first_mutation_read_count == 0U) {
        s_first_mutation_read_count = s_sd_primary_reads;
    }
}

esp_err_t d1l_retained_blob_store_write_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len, uint32_t expected_generation)
{
    (void)key;
    if (store_id != D1L_RETAINED_BLOB_STORE_PACKET_LOG || !s_sd_enabled ||
        expected_generation != s_backend_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_inject_append_during_primary_write) {
        s_inject_append_during_primary_write = false;
        s_fail_backend_state_on_call = 2U;
        d1l_packet_log_entry_t concurrent;
        memset(&concurrent, 0, sizeof(concurrent));
        (void)snprintf(concurrent.direction, sizeof(concurrent.direction), "rx");
        (void)snprintf(concurrent.kind, sizeof(concurrent.kind), "mesh");
        (void)snprintf(concurrent.note, sizeof(concurrent.note),
                       "concurrent-force");
        assert(!d1l_packet_log_append(&concurrent));
    }
    note_sd_mutation();
    s_sd_primary_writes++;
    return mock_blob_write(&s_sd_primary, src, len);
}

esp_err_t d1l_retained_blob_store_write_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len)
{
    (void)key;
    if (store_id != D1L_RETAINED_BLOB_STORE_PACKET_LOG) {
        return ESP_ERR_INVALID_ARG;
    }
    s_nvs_writes++;
    if (s_fail_nvs_write) {
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    return mock_blob_write(&s_nvs_fallback, src, len);
}

esp_err_t d1l_retained_blob_store_erase_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id, const char *key,
    uint32_t expected_generation)
{
    (void)key;
    if (store_id != D1L_RETAINED_BLOB_STORE_PACKET_LOG || !s_sd_enabled ||
        expected_generation != s_backend_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sd_primary_erases++;
    memset(&s_sd_primary, 0, sizeof(s_sd_primary));
    note_sd_mutation();
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_erase_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key)
{
    (void)key;
    if (store_id != D1L_RETAINED_BLOB_STORE_PACKET_LOG) {
        return ESP_ERR_INVALID_ARG;
    }
    s_nvs_erases++;
    memset(&s_nvs_fallback, 0, sizeof(s_nvs_fallback));
    if (s_enable_sd_during_nvs_erase) {
        s_enable_sd_during_nvs_erase = false;
        s_sd_enabled = true;
        s_backend_generation++;
    }
    return ESP_OK;
}

esp_err_t d1l_rp2040_bridge_file_read(const char *path, uint32_t offset,
                                      uint8_t *out_data, size_t max_len,
                                      d1l_rp2040_file_result_t *out_result,
                                      uint32_t timeout_ms)
{
    unsigned long segment = 0UL;
    (void)timeout_ms;
    s_history_reads++;
    if (s_history_read_error != ESP_OK) {
        return s_history_read_error;
    }
    if (s_history_read_no_card) {
        if (!out_result) {
            return ESP_ERR_INVALID_ARG;
        }
        memset(out_result, 0, sizeof(*out_result));
        (void)snprintf(out_result->err, sizeof(out_result->err), "no_card");
        return ESP_ERR_NOT_FOUND;
    }
    if (!path || !out_data || !out_result ||
        sscanf(path, "stores/packet_log/segments/s%lu.bin", &segment) != 1 ||
        segment >= D1L_PACKET_LOG_SD_SEGMENT_COUNT ||
        s_history_segment_len[segment] == 0U) {
        if (out_result) {
            memset(out_result, 0, sizeof(*out_result));
            (void)snprintf(out_result->err, sizeof(out_result->err),
                           "not_found");
        }
        return ESP_ERR_NOT_FOUND;
    }
    if (offset > s_history_segment_len[segment]) {
        memset(out_result, 0, sizeof(*out_result));
        (void)snprintf(out_result->err, sizeof(out_result->err), "range");
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t available = s_history_segment_len[segment] - offset;
    const size_t copied = available < max_len ? available : max_len;
    memcpy(out_data, s_history_segments[segment] + offset, copied);
    memset(out_result, 0, sizeof(*out_result));
    out_result->offset = offset;
    out_result->length = copied;
    out_result->ok = true;
    out_result->eof = offset + copied >= s_history_segment_len[segment];
    if (s_bump_generation_after_history_read) {
        s_bump_generation_after_history_read = false;
        s_backend_generation++;
    }
    return ESP_OK;
}

esp_err_t d1l_rp2040_bridge_file_write(const char *path, uint32_t offset,
                                       const uint8_t *data, size_t len,
                                       bool truncate,
                                       d1l_rp2040_file_result_t *out_result,
                                       uint32_t timeout_ms)
{
    unsigned long segment = 0UL;
    (void)timeout_ms;
    if (!s_sd_enabled || !path || !data || !out_result ||
        sscanf(path, "stores/packet_log/segments/s%lu.bin", &segment) != 1 ||
        segment >= D1L_PACKET_LOG_SD_SEGMENT_COUNT ||
        offset + len > MOCK_HISTORY_SEGMENT_BYTES ||
        (!truncate && offset > s_history_segment_len[segment])) {
        return ESP_ERR_INVALID_STATE;
    }
    note_sd_mutation();
    s_journal_mutations++;
    if (truncate) {
        s_journal_truncates++;
        s_history_segment_len[segment] = 0U;
    }
    memcpy(s_history_segments[segment] + offset, data, len);
    if (s_history_segment_len[segment] < offset + len) {
        s_history_segment_len[segment] = offset + len;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->length = len;
    out_result->ok = true;
    return ESP_OK;
}

esp_err_t d1l_rp2040_bridge_file_append(const char *path, const uint8_t *data,
                                        size_t len,
                                        d1l_rp2040_file_result_t *out_result,
                                        uint32_t timeout_ms)
{
    unsigned long segment = 0UL;
    if (!path ||
        sscanf(path, "stores/packet_log/segments/s%lu.bin", &segment) != 1 ||
        segment >= D1L_PACKET_LOG_SD_SEGMENT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return d1l_rp2040_bridge_file_write(
        path, (uint32_t)s_history_segment_len[segment], data, len, false,
        out_result, timeout_ms);
}

esp_err_t d1l_rp2040_bridge_file_delete(const char *path,
                                        d1l_rp2040_file_result_t *out_result,
                                        uint32_t timeout_ms)
{
    unsigned long segment = 0UL;
    (void)timeout_ms;
    if (!path || !out_result ||
        sscanf(path, "stores/packet_log/segments/s%lu.bin", &segment) != 1 ||
        segment >= D1L_PACKET_LOG_SD_SEGMENT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_delete_count++;
    if (s_attempt_append_during_delete) {
        s_attempt_append_during_delete = false;
        d1l_packet_log_entry_t concurrent;
        memset(&concurrent, 0, sizeof(concurrent));
        (void)snprintf(concurrent.direction, sizeof(concurrent.direction), "rx");
        (void)snprintf(concurrent.kind, sizeof(concurrent.kind), "mesh");
        (void)snprintf(concurrent.note, sizeof(concurrent.note),
                       "concurrent-clear");
        s_append_during_delete_result = d1l_packet_log_append(&concurrent);
    }
    memset(s_history_segments[segment], 0,
           sizeof(s_history_segments[segment]));
    s_history_segment_len[segment] = 0U;
    if (s_bump_generation_after_delete_count > 0U &&
        s_delete_count == s_bump_generation_after_delete_count) {
        s_backend_generation++;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->ok = true;
    return ESP_OK;
}

static bool append_packet(const char *note)
{
    d1l_packet_log_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    (void)snprintf(entry.direction, sizeof(entry.direction), "rx");
    (void)snprintf(entry.kind, sizeof(entry.kind), "mesh");
    (void)snprintf(entry.note, sizeof(entry.note), "%s", note);
    return d1l_packet_log_append(&entry);
}

static esp_err_t append_packet_checked(const char *note,
                                       uint32_t *out_stored_seq)
{
    d1l_packet_log_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    (void)snprintf(entry.direction, sizeof(entry.direction), "rx");
    (void)snprintf(entry.kind, sizeof(entry.kind), "mesh");
    (void)snprintf(entry.note, sizeof(entry.note), "%s", note);
    return d1l_packet_log_append_raw_checked(
        &entry, NULL, 0U, out_stored_seq);
}

static void test_unloaded_deferred_append_fails_without_storage_work(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 17U;
    seed_primary(&s_sd_primary, 1U, 1U, 1U, "unloaded-sd");
    seed_fallback(1U, 1U, 1U, "unloaded-nvs");
    seed_history(1U, 1U, "unloaded-history");

    d1l_packet_log_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    (void)snprintf(entry.direction, sizeof(entry.direction), "tx");
    (void)snprintf(entry.kind, sizeof(entry.kind), "dm_ack");
    (void)snprintf(entry.note, sizeof(entry.note), "unloaded-deferred");

    assert(!d1l_packet_log_append_raw_deferred(&entry, NULL, 0U));
    assert(s_backend_state_calls == 0U);
    assert(s_sd_primary_reads == 0U);
    assert(s_nvs_reads == 0U);
    assert(s_history_reads == 0U);
    assert(s_sd_primary_writes == 0U);
    assert(s_nvs_writes == 0U);
    assert(s_sd_primary_erases == 0U);
    assert(s_nvs_erases == 0U);
    assert(s_journal_mutations == 0U);
    assert(s_journal_truncates == 0U);
    assert(s_delete_count == 0U);

    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(!stats.loaded);
    assert(stats.total_written == 0U);
    assert(stats.count == 0U);
    assert(!stats.persistence_dirty);
}

static void test_deferred_append_never_writes_on_caller(void)
{
    mock_reset();
    assert(d1l_packet_log_init() == ESP_OK);
    const uint32_t nvs_writes_before = s_nvs_writes;
    const uint32_t primary_writes_before = s_sd_primary_writes;

    d1l_packet_log_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    (void)snprintf(entry.direction, sizeof(entry.direction), "tx");
    (void)snprintf(entry.kind, sizeof(entry.kind), "advert");
    (void)snprintf(entry.note, sizeof(entry.note), "owner-deferred");
    assert(d1l_packet_log_append_raw_deferred(&entry, NULL, 0U));
    assert(s_nvs_writes == nvs_writes_before);
    assert(s_sd_primary_writes == primary_writes_before);
    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.total_written == 1U);
    assert(stats.persistence_dirty);
    assert(stats.nvs_fallback_dirty);

    assert(d1l_packet_log_flush_if_due() == ESP_OK);
    stats = d1l_packet_log_stats();
    assert(s_nvs_writes == nvs_writes_before + 1U);
    assert(!stats.persistence_dirty);
    assert(!stats.nvs_fallback_dirty);

    mock_reset();
    assert(d1l_packet_log_init() == ESP_OK);
    s_fail_nonblocking_take_count = 1U;
    assert(!d1l_packet_log_append_raw_deferred(&entry, NULL, 0U));
    stats = d1l_packet_log_stats();
    assert(stats.total_written == 0U);
    assert(!stats.persistence_dirty);
}

static void test_late_sd_is_read_and_merged_before_mutation(void)
{
    mock_reset();
    seed_fallback(1U, 8U, 8U, "nvs");
    assert(d1l_packet_log_init() == ESP_OK);
    assert(append_packet("live-off-card"));

    seed_primary(&s_sd_primary, 1U, 12U, 12U, "sd");
    seed_history(1U, 12U, "sd");
    s_sd_enabled = true;
    s_backend_generation = 2U;
    assert(d1l_packet_log_flush() == ESP_OK);

    assert(s_first_mutation_read_count > 0U);
    const primary_blob_t primary = current_primary();
    const fallback_blob_t fallback = current_fallback();
    assert(primary.total_written == 13U && primary.next_seq == 14U);
    assert(fallback.total_written == 13U && fallback.next_seq == 14U);
    assert(strcmp(primary.entries[primary.count - 1U].note,
                  "live-off-card") == 0);
    assert(primary.entries[primary.count - 1U].seq == 13U);
    assert(s_journal_mutations == 0U);

    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.sd_backend_generation == 2U);
    assert(stats.sd_reconcile_count == 1U);
    assert(!stats.sd_primary_reconcile_pending);
    assert(!stats.persistence_dirty);
}

static void test_checked_append_reports_resequenced_sd_overlay(void)
{
    mock_reset();
    seed_fallback(1U, 8U, 8U, "nvs");
    assert(d1l_packet_log_init() == ESP_OK);

    seed_primary(&s_sd_primary, 1U, 12U, 12U, "sd");
    seed_history(1U, 12U, "sd");
    s_sd_enabled = true;
    s_backend_generation = 2U;

    d1l_packet_log_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    (void)snprintf(entry.direction, sizeof(entry.direction), "tx");
    (void)snprintf(entry.kind, sizeof(entry.kind), "canary");
    (void)snprintf(entry.note, sizeof(entry.note), "checked-overlay");
    uint32_t stored_seq = 0U;
    assert(d1l_packet_log_append_raw_checked(
               &entry, NULL, 0U, &stored_seq) == ESP_OK);
    assert(stored_seq == 13U);

    d1l_packet_log_entry_t stored = {0};
    assert(d1l_packet_log_find_by_seq(stored_seq, &stored) == ESP_OK);
    assert(strcmp(stored.note, "checked-overlay") == 0);
    const primary_blob_t primary = current_primary();
    assert(primary.next_seq == 14U);
    assert(primary.entries[primary.count - 1U].seq == stored_seq);
    assert(strcmp(primary.entries[primary.count - 1U].note,
                  "checked-overlay") == 0);
}

static void test_occupied_next_slot_is_preserved_until_fresh_boundary(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 11U;
    seed_primary(&s_sd_primary, 1U, 2U, 2U, "base");
    seed_fallback(1U, 2U, 2U, "base");
    seed_history(1U, 2U, "base");
    seed_history(3U, 1U, "preserved");
    seed_history(65U, 1U, "boundary");
    seed_history(66U, 1U, "stale");

    assert(d1l_packet_log_init() == ESP_OK);
    assert(s_journal_mutations == 0U);
    uint32_t stored_seq = 0U;
    assert(append_packet_checked("canary", &stored_seq) == ESP_OK);
    assert(stored_seq == 3U);
    assert(s_journal_mutations == 0U);
    const history_record_t *segment0 =
        (const history_record_t *)s_history_segments[0];
    assert(strcmp(segment0[2].entry.note, "preserved-3") == 0);
    assert(d1l_packet_log_flush() == ESP_OK);
    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(!stats.persistence_dirty);
    assert(!stats.sd_primary_dirty);
    assert(!stats.nvs_fallback_dirty);
    assert(!stats.journal_dirty);
    assert(!stats.sd_primary_reconcile_pending);
    assert(stats.journal_commit_count == 0U);
    assert(stats.journal_fail_count == 0U);
    assert(stats.journal_last_error == ESP_OK);
    assert(stats.sd_primary_commit_count == 1U);
    assert(stats.nvs_fallback_commit_count == 1U);

    for (uint32_t seq = 4U; seq < 65U; ++seq) {
        char note[32];
        (void)snprintf(note, sizeof(note), "deferred-%lu",
                       (unsigned long)seq);
        assert(append_packet(note));
    }
    assert(s_journal_mutations == 0U);
    d1l_packet_log_entry_t boundary = packet_entry(65U, "boundary");
    assert(d1l_packet_log_append_raw_checked(
               &boundary, NULL, 0U, &stored_seq) == ESP_OK);
    assert(stored_seq == 65U);
    assert(s_journal_mutations == 1U);
    assert(s_journal_truncates == 1U);
    segment0 = (const history_record_t *)s_history_segments[0];
    assert(strcmp(segment0[2].entry.note, "preserved-3") == 0);
    const history_record_t *segment1 =
        (const history_record_t *)s_history_segments[1];
    assert(s_history_segment_len[1] == sizeof(history_record_t));
    assert(segment1[0].entry.seq == 65U);
    assert(strcmp(segment1[0].entry.note, "boundary-65") == 0);

    assert(append_packet_checked("after-boundary", &stored_seq) == ESP_OK);
    assert(stored_seq == 66U);
    assert(s_journal_mutations == 2U);
    assert(s_journal_truncates == 1U);
    assert(s_history_segment_len[1] == 2U * sizeof(history_record_t));
    segment1 = (const history_record_t *)s_history_segments[1];
    assert(segment1[1].entry.seq == 66U);
    assert(strcmp(segment1[1].entry.note, "after-boundary") == 0);

    assert(d1l_packet_log_flush() == ESP_OK);
    stats = d1l_packet_log_stats();
    assert(!stats.persistence_dirty);
    assert(!stats.sd_primary_dirty);
    assert(!stats.nvs_fallback_dirty);
    assert(!stats.journal_dirty);
    assert(!stats.sd_primary_reconcile_pending);
    assert(stats.journal_commit_count == 2U);
    assert(stats.journal_fail_count == 0U);
    assert(stats.journal_last_error == ESP_OK);
    assert(stats.sd_primary_commit_count >= 2U);
    assert(stats.nvs_fallback_commit_count >= 2U);
}

static void test_exact_empty_eof_continues_journal_normally(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 12U;
    seed_primary(&s_sd_primary, 1U, 2U, 2U, "eof");
    seed_fallback(1U, 2U, 2U, "eof");
    seed_history(1U, 2U, "eof");

    assert(d1l_packet_log_init() == ESP_OK);
    uint32_t stored_seq = 0U;
    assert(append_packet_checked("eof-next", &stored_seq) == ESP_OK);
    assert(stored_seq == 3U);
    assert(s_journal_mutations == 1U);
    const history_record_t *segment0 =
        (const history_record_t *)s_history_segments[0];
    assert(segment0[2].entry.seq == 3U);
    assert(strcmp(segment0[2].entry.note, "eof-next") == 0);
}

static void test_partial_next_slot_is_preserved_without_journal_mutation(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 13U;
    seed_primary(&s_sd_primary, 1U, 2U, 2U, "partial");
    seed_fallback(1U, 2U, 2U, "partial");
    seed_history(1U, 2U, "partial");
    const size_t partial_offset = 2U * sizeof(history_record_t);
    s_history_segments[0][partial_offset] = 0xA5U;
    s_history_segment_len[0] = partial_offset + 1U;

    assert(d1l_packet_log_init() == ESP_OK);
    uint32_t stored_seq = 0U;
    assert(append_packet_checked("partial-next", &stored_seq) == ESP_OK);
    assert(stored_seq == 3U);
    assert(s_journal_mutations == 0U);
    assert(s_history_segment_len[0] == partial_offset + 1U);
    assert(s_history_segments[0][partial_offset] == 0xA5U);
}

static void test_short_existing_segment_defers_to_fresh_boundary(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 16U;
    seed_primary(&s_sd_primary, 1U, 4U, 4U, "short");
    seed_fallback(1U, 4U, 4U, "short");
    seed_history(1U, 2U, "short");
    const size_t original_len = s_history_segment_len[0];
    uint8_t original[2U * sizeof(history_record_t)];
    memcpy(original, s_history_segments[0], original_len);

    assert(d1l_packet_log_init() == ESP_OK);
    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.sd_reconcile_count == 1U);
    assert(stats.sd_reconcile_fail_count == 0U);
    assert(stats.sd_reconcile_last_error == ESP_OK);
    assert(!stats.sd_primary_reconcile_pending);
    assert(!stats.persistence_dirty);
    assert(s_journal_mutations == 0U);
    assert(s_journal_truncates == 0U);
    assert(s_history_segment_len[0] == original_len);
    assert(memcmp(s_history_segments[0], original, original_len) == 0);

    uint32_t stored_seq = 0U;
    assert(append_packet_checked("short-next", &stored_seq) == ESP_OK);
    assert(stored_seq == 5U);
    assert(s_journal_mutations == 0U);
    assert(s_journal_truncates == 0U);
    assert(s_history_segment_len[0] == original_len);
    assert(memcmp(s_history_segments[0], original, original_len) == 0);
    stats = d1l_packet_log_stats();
    assert(!stats.persistence_dirty);
    assert(stats.sd_primary_commit_count == 1U);
    assert(stats.nvs_fallback_commit_count == 1U);
}

static void test_journal_probe_transport_and_generation_errors_never_mutate(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 14U;
    seed_primary(&s_sd_primary, 1U, 2U, 2U, "error");
    seed_fallback(1U, 2U, 2U, "error");
    seed_history(1U, 2U, "error");
    s_history_read_error = ESP_ERR_TIMEOUT;

    assert(d1l_packet_log_init() == ESP_ERR_TIMEOUT);
    assert(s_sd_primary_writes == 0U);
    assert(s_nvs_writes == 0U);
    assert(s_journal_mutations == 0U);

    s_history_read_error = ESP_OK;
    s_bump_generation_after_history_read = true;
    assert(d1l_packet_log_flush() == ESP_ERR_INVALID_STATE);
    assert(s_sd_primary_writes == 0U);
    assert(s_nvs_writes == 0U);
    assert(s_journal_mutations == 0U);

    assert(d1l_packet_log_flush() == ESP_OK);
    assert(!d1l_packet_log_stats().persistence_dirty);
}

static void test_journal_probe_no_card_is_not_treated_as_empty(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 15U;
    seed_primary(&s_sd_primary, 1U, 2U, 2U, "no-card");
    seed_fallback(1U, 2U, 2U, "no-card");
    seed_history(1U, 2U, "no-card");
    s_history_read_no_card = true;

    assert(d1l_packet_log_init() == ESP_ERR_NOT_FOUND);
    assert(s_sd_primary_writes == 0U);
    assert(s_nvs_writes == 0U);
    assert(s_journal_mutations == 0U);
    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.sd_primary_reconcile_pending);
    assert(stats.sd_reconcile_last_error == ESP_ERR_NOT_FOUND);

    s_history_read_no_card = false;
    assert(d1l_packet_log_flush() == ESP_OK);
    stats = d1l_packet_log_stats();
    assert(!stats.persistence_dirty);
    assert(!stats.sd_primary_dirty);
    assert(!stats.nvs_fallback_dirty);
    assert(!stats.journal_dirty);
    assert(!stats.sd_primary_reconcile_pending);
    assert(stats.sd_reconcile_last_error == ESP_OK);
    assert(stats.journal_fail_count == 0U);
    assert(stats.journal_last_error == ESP_OK);
    assert(s_sd_primary_writes == 0U);
    assert(s_nvs_writes == 0U);
    assert(s_journal_mutations == 0U);
}

static void test_v2_compact_sd_primary_is_promoted_and_rewritten(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 4U;
    seed_v2_compact_primary(1U, 8U, 8U, "upgrade");
    seed_fallback(5U, 8U, 12U, "upgrade");

    assert(d1l_packet_log_init() == ESP_OK);
    assert(s_sd_primary_reads > 0U);
    assert(s_first_mutation_read_count > 0U);
    assert(s_sd_primary_writes == 1U);
    assert(s_sd_primary.len == sizeof(primary_blob_t));

    const primary_blob_t primary = current_primary();
    assert(primary.schema == 3U);
    assert(primary.next_seq == 13U);
    assert(primary.total_written == 12U);
    assert(primary.count == 12U);
    for (uint32_t i = 0U; i < primary.count; ++i) {
        assert(primary.entries[i].seq == i + 1U);
        char expected[48];
        (void)snprintf(expected, sizeof(expected), "upgrade-%lu",
                       (unsigned long)(i + 1U));
        assert(strcmp(primary.entries[i].note, expected) == 0);
    }

    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.sd_backend_generation == 4U);
    assert(stats.sd_reconcile_count == 1U);
    assert(stats.sd_reconcile_fail_count == 0U);
    assert(stats.sd_reconcile_last_error == ESP_OK);
    assert(stats.sd_primary_commit_count == 1U);
    assert(!stats.sd_primary_reconcile_pending);
    assert(!stats.persistence_dirty);
}

static void test_empty_sd_primary_with_high_local_tail_uses_fresh_boundary(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 6U;
    seed_primary(&s_sd_primary, 1U, 0U, 0U, "empty");
    seed_fallback(421U, 8U, 428U, "local");

    assert(d1l_packet_log_init() == ESP_OK);
    assert(s_sd_primary_writes == 1U);
    assert(s_journal_mutations == 0U);
    const primary_blob_t primary = current_primary();
    assert(primary.next_seq == 429U);
    assert(primary.total_written == 428U);
    assert(primary.count == 8U);
    assert(strcmp(primary.entries[0].note, "local-421") == 0);
    assert(strcmp(primary.entries[7].note, "local-428") == 0);

    assert(d1l_packet_log_flush() == ESP_OK);
    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(!stats.persistence_dirty);
    assert(!stats.sd_primary_dirty);
    assert(!stats.nvs_fallback_dirty);
    assert(!stats.journal_dirty);
    assert(!stats.sd_primary_reconcile_pending);
    assert(stats.journal_fail_count == 0U);
    assert(stats.journal_last_error == ESP_OK);

    for (uint32_t seq = 429U; seq < 449U; ++seq) {
        char note[32];
        (void)snprintf(note, sizeof(note), "empty-tail-%lu",
                       (unsigned long)seq);
        assert(append_packet(note));
    }
    assert(s_journal_mutations == 0U);
    assert(d1l_packet_log_flush() == ESP_OK);
    stats = d1l_packet_log_stats();
    assert(!stats.persistence_dirty);
    assert(!stats.sd_primary_dirty);
    assert(!stats.nvs_fallback_dirty);
    assert(!stats.journal_dirty);
    assert(stats.journal_fail_count == 0U);

    uint32_t stored_seq = 0U;
    assert(append_packet_checked("empty-tail-boundary", &stored_seq) == ESP_OK);
    assert(stored_seq == 449U);
    assert(s_journal_mutations == 1U);
    assert(s_journal_truncates == 1U);
}

static void test_disjoint_v3_primary_and_local_tail_are_resequenced(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 7U;
    seed_primary(&s_sd_primary, 192U, 128U, 319U, "sd");
    seed_fallback(421U, 8U, 428U, "local");

    assert(d1l_packet_log_init() == ESP_OK);
    assert(s_first_mutation_read_count > 0U);
    assert(s_sd_primary_writes == 1U);

    const primary_blob_t primary = current_primary();
    assert(primary.schema == 3U);
    assert(primary.next_seq == 429U);
    assert(primary.total_written == 428U);
    assert(primary.count == D1L_PACKET_LOG_RAM_CAPACITY);
    assert(primary.entries[0].seq == 301U);
    assert(strcmp(primary.entries[0].note, "sd-200") == 0);
    assert(primary.entries[119].seq == 420U);
    assert(strcmp(primary.entries[119].note, "sd-319") == 0);
    assert(primary.entries[120].seq == 421U);
    assert(strcmp(primary.entries[120].note, "local-421") == 0);
    assert(primary.entries[127].seq == 428U);
    assert(strcmp(primary.entries[127].note, "local-428") == 0);

    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.sd_reconcile_count == 1U);
    assert(stats.sd_reconcile_fail_count == 0U);
    assert(!stats.sd_primary_reconcile_pending);
    assert(!stats.persistence_dirty);
    assert(!stats.journal_dirty);

    assert(d1l_packet_log_flush() == ESP_OK);
    assert(s_sd_primary_writes == 1U);
}

static void test_conflicting_same_seq_rows_are_both_preserved(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 8U;
    seed_primary(&s_sd_primary, 5U, 8U, 12U, "sd");
    seed_fallback(9U, 8U, 16U, "local");

    assert(d1l_packet_log_init() == ESP_OK);
    const primary_blob_t primary = current_primary();
    assert(primary.next_seq == 17U);
    assert(primary.total_written == 16U);
    assert(primary.count == 16U);
    assert(strcmp(primary.entries[4].note, "sd-9") == 0);
    assert(strcmp(primary.entries[5].note, "local-9") == 0);
    assert(strcmp(primary.entries[10].note, "sd-12") == 0);
    assert(strcmp(primary.entries[11].note, "local-12") == 0);
    assert(strcmp(primary.entries[12].note, "local-13") == 0);
    for (uint32_t i = 0U; i < primary.count; ++i) {
        assert(primary.entries[i].seq == i + 1U);
    }
    assert(!d1l_packet_log_stats().persistence_dirty);
}

static void test_disjoint_reconcile_generation_edge_never_writes_and_retries(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 9U;
    seed_primary(&s_sd_primary, 192U, 128U, 319U, "edge-sd");
    seed_fallback(421U, 8U, 428U, "edge-local");
    s_bump_generation_after_sd_read = true;

    assert(d1l_packet_log_init() == ESP_ERR_INVALID_STATE);
    assert(s_sd_primary_reads == 1U);
    assert(s_sd_primary_writes == 0U);
    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.sd_primary_reconcile_pending);
    assert(stats.sd_reconcile_last_error == ESP_ERR_INVALID_STATE);

    assert(d1l_packet_log_flush() == ESP_OK);
    assert(s_sd_primary_writes == 1U);
    stats = d1l_packet_log_stats();
    assert(!stats.sd_primary_reconcile_pending);
    assert(stats.sd_reconcile_last_error == ESP_OK);
    assert(!stats.persistence_dirty);
    const primary_blob_t primary = current_primary();
    assert(primary.next_seq == 429U);
    assert(strcmp(primary.entries[120].note, "edge-local-421") == 0);

    assert(d1l_packet_log_flush() == ESP_OK);
    assert(s_sd_primary_writes == 1U);
}

static void test_disjoint_reconcile_generation_edge_before_apply_never_imports(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 14U;
    seed_primary(&s_sd_primary, 192U, 128U, 319U, "apply-edge-sd");
    seed_fallback(421U, 8U, 428U, "apply-edge-local");
    /* Arm relative to the primary read: the first later observation is the
     * post-read guard and the second is the final pre-apply guard. */
    s_bump_generation_before_reconcile_apply = true;

    assert(d1l_packet_log_init() == ESP_ERR_INVALID_STATE);
    assert(s_preapply_generation_bump_fired);
    assert(s_backend_state_calls_after_sd_read == 2U);
    assert(s_sd_primary_reads == 1U);
    assert(s_sd_primary_writes == 0U);
    assert(s_nvs_writes == 0U);
    d1l_packet_log_entry_t found = {0};
    assert(d1l_packet_log_find_by_seq(421U, &found) == ESP_OK);
    assert(strcmp(found.note, "apply-edge-local-421") == 0);
    assert(d1l_packet_log_find_by_seq(301U, &found) == ESP_ERR_NOT_FOUND);
    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.total_written == 428U);
    assert(stats.sd_primary_reconcile_pending);
    assert(stats.sd_reconcile_last_error == ESP_ERR_INVALID_STATE);

    assert(d1l_packet_log_flush() == ESP_OK);
    assert(s_sd_primary_writes == 1U);
    assert(s_nvs_writes == 1U);
    stats = d1l_packet_log_stats();
    assert(!stats.sd_primary_reconcile_pending);
    assert(!stats.persistence_dirty);
}

static void test_sd_success_nvs_failure_stays_retryable(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_packet_log_init() == ESP_OK);
    s_now_us += 6000000LL;
    const uint32_t primary_writes_before = s_sd_primary_writes;

    s_fail_nvs_write = true;
    assert(!append_packet("mirror-retry"));
    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(s_sd_primary_writes == primary_writes_before + 1U);
    assert(!stats.sd_primary_dirty);
    assert(stats.nvs_fallback_dirty);
    assert(stats.persistence_dirty);
    assert(stats.nvs_fallback_last_error == ESP_ERR_NVS_NOT_ENOUGH_SPACE);

    s_fail_nvs_write = false;
    const uint32_t primary_writes_after_failure = s_sd_primary_writes;
    assert(d1l_packet_log_flush_if_due() == ESP_OK);
    stats = d1l_packet_log_stats();
    assert(s_sd_primary_writes == primary_writes_after_failure);
    assert(!stats.nvs_fallback_dirty);
    assert(!stats.persistence_dirty);
    assert(current_fallback().total_written == 1U);
}

static void test_corrupt_or_changed_sd_never_mutates_before_reconcile(void)
{
    mock_reset();
    assert(d1l_packet_log_init() == ESP_OK);
    assert(append_packet("nvs-one"));

    memset(s_sd_primary.data, 0xa5, sizeof(primary_blob_t));
    s_sd_primary.len = sizeof(primary_blob_t);
    s_sd_primary.valid = true;
    s_sd_enabled = true;
    s_backend_generation = 2U;
    assert(!append_packet("nvs-two"));
    assert(s_sd_primary_writes == 0U);
    assert(s_journal_mutations == 0U);
    assert(s_delete_count == 0U);
    assert(current_fallback().total_written == 2U);
    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.sd_primary_reconcile_pending);
    assert(stats.sd_reconcile_last_error == ESP_ERR_INVALID_STATE);
    assert(!stats.nvs_fallback_dirty);
    assert(stats.persistence_dirty);

    /* A generation edge after a valid read is also fail-closed. */
    seed_primary(&s_sd_primary, 1U, 2U, 2U, "sd");
    seed_history(1U, 2U, "sd");
    s_bump_generation_after_sd_read = true;
    const uint32_t mutations_before = s_journal_mutations + s_sd_primary_writes;
    assert(d1l_packet_log_flush() == ESP_ERR_INVALID_STATE);
    assert(s_journal_mutations + s_sd_primary_writes == mutations_before);
    stats = d1l_packet_log_stats();
    assert(stats.sd_primary_reconcile_pending);
}

static void test_init_does_not_erase_corrupt_sd(void)
{
    mock_reset();
    memset(s_sd_primary.data, 0x5a, sizeof(primary_blob_t));
    s_sd_primary.len = sizeof(primary_blob_t);
    s_sd_primary.valid = true;
    s_sd_enabled = true;
    s_backend_generation = 7U;
    assert(d1l_packet_log_init() == ESP_ERR_INVALID_STATE);
    assert(s_delete_count == 0U);
    assert(s_sd_primary_writes == 0U);
    assert(s_journal_mutations == 0U);
}

static void test_corrupt_only_fallback_reports_store_unavailable(void)
{
    mock_reset();
    memset(s_nvs_fallback.data, 0x5a, sizeof(fallback_blob_t));
    s_nvs_fallback.len = sizeof(fallback_blob_t);
    s_nvs_fallback.valid = true;
    assert(d1l_packet_log_init() == ESP_ERR_INVALID_STATE);
    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(!stats.loaded);
    assert(s_sd_primary_writes == 0U);
    assert(s_nvs_writes == 0U);
    assert(s_delete_count == 0U);
}

static void test_journal_ahead_primary_behind_replay_is_idempotent(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_primary(&s_sd_primary, 1U, 1U, 1U, "power");
    seed_fallback(1U, 2U, 2U, "power");
    seed_history(1U, 2U, "power");
    const size_t journal_len_before = s_history_segment_len[0];

    assert(d1l_packet_log_init() == ESP_OK);
    assert(s_journal_mutations == 0U);
    assert(s_history_segment_len[0] == journal_len_before);
    assert(current_primary().total_written == 2U);
    d1l_packet_log_entry_t found = {0};
    assert(d1l_packet_log_find_by_seq(2U, &found) == ESP_OK);
    assert(strcmp(found.note, "power-2") == 0);
    assert(!d1l_packet_log_stats().persistence_dirty);
}

static void test_sequential_journal_write_accepts_exact_empty_eof(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_packet_log_init() == ESP_OK);
    assert(append_packet("sequential-one"));
    assert(append_packet("sequential-two"));
    assert(s_journal_mutations == 2U);
    assert(s_history_segment_len[0] == 2U * sizeof(history_record_t));
    const history_record_t *records =
        (const history_record_t *)s_history_segments[0];
    assert(records[0].entry.seq == 1U);
    assert(strcmp(records[0].entry.note, "sequential-one") == 0);
    assert(records[1].entry.seq == 2U);
    assert(strcmp(records[1].entry.note, "sequential-two") == 0);
}

static void test_journal_cooperative_yield_keeps_dirty_without_failure(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_packet_log_init() == ESP_OK);
    const d1l_packet_log_stats_t before = d1l_packet_log_stats();

    s_worker_should_yield = true;
    d1l_packet_log_entry_t entry = packet_entry(0U, "yield");
    uint32_t stored_seq = 0U;
    assert(d1l_packet_log_append_raw_checked(
               &entry, NULL, 0U, &stored_seq) == ESP_ERR_NOT_FINISHED);
    const d1l_packet_log_stats_t yielded = d1l_packet_log_stats();
    assert(yielded.total_written == before.total_written + 1U);
    assert(yielded.journal_dirty);
    assert(yielded.journal_fail_count == before.journal_fail_count);
    assert(yielded.sd_history_failed_writes ==
           before.sd_history_failed_writes);
    assert(yielded.persistence_fail_count == before.persistence_fail_count);
    assert(s_journal_mutations == 0U);

    s_worker_should_yield = false;
    assert(d1l_packet_log_flush() == ESP_OK);
    const d1l_packet_log_stats_t recovered = d1l_packet_log_stats();
    assert(!recovered.persistence_dirty);
    assert(!recovered.journal_dirty);
    assert(recovered.journal_fail_count == before.journal_fail_count);
    assert(recovered.persistence_fail_count == before.persistence_fail_count);
    assert(s_journal_mutations == 1U);
}

static void test_gapped_compact_tail_fails_closed(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 3U;
    seed_primary(&s_sd_primary, 1U, 2U, 3U, "gap");
    primary_blob_t gapped = current_primary();
    gapped.entries[1] = packet_entry(3U, "gap");
    assert(mock_blob_write(&s_sd_primary, &gapped, sizeof(gapped)) == ESP_OK);

    assert(d1l_packet_log_init() == ESP_ERR_INVALID_STATE);
    assert(s_sd_primary_writes == 0U);
    assert(s_journal_mutations == 0U);
    assert(s_delete_count == 0U);
    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.sd_primary_reconcile_pending);
    assert(stats.sd_reconcile_last_error == ESP_ERR_INVALID_STATE);
}

static void test_clear_rejects_concurrent_append_and_stops_on_card_edge(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    seed_primary(&s_sd_primary, 1U, 1U, 1U, "clear");
    seed_fallback(1U, 1U, 1U, "clear");
    seed_history(1U, 1U, "clear");
    assert(d1l_packet_log_init() == ESP_OK);

    s_attempt_append_during_delete = true;
    s_bump_generation_after_delete_count = 1U;
    assert(d1l_packet_log_clear() == ESP_ERR_INVALID_STATE);
    assert(!s_append_during_delete_result);
    assert(s_delete_count == 1U);
    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.total_written == 1U);
    assert(stats.count == 1U);
    assert(!stats.clear_in_progress);
    assert(stats.sd_primary_reconcile_pending);

    d1l_packet_log_entry_t preview;
    memset(&preview, 0, sizeof(preview));
    (void)snprintf(preview.direction, sizeof(preview.direction), "rx");
    (void)snprintf(preview.kind, sizeof(preview.kind), "preview");
    assert(d1l_packet_log_append_raw_volatile(&preview, NULL, 0U));
}

static void test_clear_resamples_card_inserted_while_waiting_for_ownership(void)
{
    mock_reset();
    seed_fallback(1U, 1U, 1U, "local");
    seed_primary(&s_sd_primary, 1U, 1U, 1U, "local");
    seed_history(1U, 1U, "local");
    assert(d1l_packet_log_init() == ESP_OK);

    /* The card becomes ready on the first lock acquisition inside clear. */
    s_enable_sd_on_lock_take_countdown = 1U;
    assert(d1l_packet_log_clear() == ESP_OK);
    assert(s_delete_count == D1L_PACKET_LOG_SD_SEGMENT_COUNT);
    assert(!s_sd_primary.valid);
    assert(!s_nvs_fallback.valid);
    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.total_written == 0U);
    assert(stats.count == 0U);
    assert(!stats.persistence_dirty);
}

static void test_clear_rejects_card_inserted_during_nvs_commit(void)
{
    mock_reset();
    seed_fallback(1U, 1U, 1U, "local");
    seed_primary(&s_sd_primary, 1U, 1U, 1U, "replacement");
    seed_history(1U, 1U, "replacement");
    assert(d1l_packet_log_init() == ESP_OK);

    s_enable_sd_during_nvs_erase = true;
    assert(d1l_packet_log_clear() == ESP_ERR_INVALID_STATE);
    assert(s_sd_primary.valid);
    assert(s_delete_count == 0U);
    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.total_written == 1U);
    assert(stats.count == 1U);
    assert(stats.sd_primary_reconcile_pending);
    assert(stats.persistence_dirty);
}

static void test_force_flush_rejects_snapshot_staled_by_concurrent_append(void)
{
    mock_reset();
    s_sd_enabled = true;
    s_backend_generation = 1U;
    assert(d1l_packet_log_init() == ESP_OK);
    assert(append_packet("force-one"));
    assert(append_packet("force-two"));
    assert(d1l_packet_log_stats().sd_primary_dirty);

    s_inject_append_during_primary_write = true;
    assert(d1l_packet_log_flush() == ESP_ERR_INVALID_STATE);
    const d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    assert(stats.total_written == 3U);
    assert(stats.persistence_dirty);
    assert(stats.sd_primary_dirty);
    assert(stats.nvs_fallback_dirty);
    assert(stats.journal_dirty);
    d1l_packet_log_entry_t found = {0};
    assert(d1l_packet_log_find_by_seq(3U, &found) == ESP_OK);
    assert(strcmp(found.note, "concurrent-force") == 0);

    /* Once SD is genuinely unavailable, a stable current NVS fallback is a
     * valid controlled-reboot boundary; dormant SD/journal bits must not
     * wedge reboot until the removable card returns. */
    s_sd_enabled = false;
    s_backend_generation++;
    assert(d1l_packet_log_flush() == ESP_OK);
    const d1l_packet_log_stats_t fallback_stats = d1l_packet_log_stats();
    assert(!fallback_stats.persistence_dirty);
    assert(!fallback_stats.nvs_fallback_dirty);
}

int main(void)
{
    test_unloaded_deferred_append_fails_without_storage_work();
    test_deferred_append_never_writes_on_caller();
    test_late_sd_is_read_and_merged_before_mutation();
    test_checked_append_reports_resequenced_sd_overlay();
    test_occupied_next_slot_is_preserved_until_fresh_boundary();
    test_exact_empty_eof_continues_journal_normally();
    test_partial_next_slot_is_preserved_without_journal_mutation();
    test_short_existing_segment_defers_to_fresh_boundary();
    test_journal_probe_transport_and_generation_errors_never_mutate();
    test_journal_probe_no_card_is_not_treated_as_empty();
    test_v2_compact_sd_primary_is_promoted_and_rewritten();
    test_empty_sd_primary_with_high_local_tail_uses_fresh_boundary();
    test_disjoint_v3_primary_and_local_tail_are_resequenced();
    test_conflicting_same_seq_rows_are_both_preserved();
    test_disjoint_reconcile_generation_edge_never_writes_and_retries();
    test_disjoint_reconcile_generation_edge_before_apply_never_imports();
    test_sd_success_nvs_failure_stays_retryable();
    test_corrupt_or_changed_sd_never_mutates_before_reconcile();
    test_init_does_not_erase_corrupt_sd();
    test_corrupt_only_fallback_reports_store_unavailable();
    test_journal_ahead_primary_behind_replay_is_idempotent();
    test_sequential_journal_write_accepts_exact_empty_eof();
    test_journal_cooperative_yield_keeps_dirty_without_failure();
    test_gapped_compact_tail_fails_closed();
    test_clear_rejects_concurrent_append_and_stops_on_card_edge();
    test_clear_resamples_card_inserted_while_waiting_for_ownership();
    test_clear_rejects_card_inserted_during_nvs_commit();
    test_force_flush_rejects_snapshot_staled_by_concurrent_append();
    puts("native packet retained durability: ok");
    return 0;
}
