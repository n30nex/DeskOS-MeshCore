#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/semphr.h"
#include "hal/rp2040_bridge.h"
#include "mesh/dm_store.h"
#include "mesh/message_store.h"
#include "mesh/packet_log.h"
#include "storage/retained_blob_store.h"

#define MOCK_BLOB_MAX 65536U
#define MOCK_HISTORY_SEGMENT_BYTES \
    (D1L_PACKET_LOG_SD_SEGMENT_CAPACITY * D1L_RP2040_FILE_CHUNK_MAX)

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
} persisted_header_t;

static mock_blob_t s_primary[D1L_RETAINED_BLOB_STORE_COUNT];
static mock_blob_t s_fallback[D1L_RETAINED_BLOB_STORE_COUNT];
static uint32_t s_write_count[D1L_RETAINED_BLOB_STORE_COUNT];
static uint8_t s_history_segments[D1L_PACKET_LOG_SD_SEGMENT_COUNT]
                                 [MOCK_HISTORY_SEGMENT_BYTES];
static size_t s_history_segment_len[D1L_PACKET_LOG_SD_SEGMENT_COUNT];
static size_t s_history_record_len;
static bool s_use_packet_sd;
static uint32_t s_fail_history_read_seq_min;
static int64_t s_now_us;

bool d1l_route_store_persistence_should_yield(void)
{
    return false;
}

static void mock_reset(void)
{
    memset(s_primary, 0, sizeof(s_primary));
    memset(s_fallback, 0, sizeof(s_fallback));
    memset(s_write_count, 0, sizeof(s_write_count));
    memset(s_history_segments, 0, sizeof(s_history_segments));
    memset(s_history_segment_len, 0, sizeof(s_history_segment_len));
    s_history_record_len = 0U;
    s_use_packet_sd = false;
    s_fail_history_read_seq_min = 0U;
    s_now_us = 1000000;
}

static esp_err_t mock_blob_read(const mock_blob_t *blob, void *dst, size_t *len_inout)
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

static bool blob_contains(const mock_blob_t *blob, const char *needle)
{
    if (!blob || !blob->valid || !needle || needle[0] == '\0') {
        return false;
    }
    const size_t needle_len = strlen(needle);
    if (needle_len > blob->len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= blob->len; ++i) {
        if (memcmp(blob->data + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool history_contains(const char *needle)
{
    if (!needle || needle[0] == '\0') {
        return false;
    }
    const size_t needle_len = strlen(needle);
    for (size_t segment = 0; segment < D1L_PACKET_LOG_SD_SEGMENT_COUNT;
         ++segment) {
        for (size_t i = 0;
             i + needle_len <= s_history_segment_len[segment]; ++i) {
            if (memcmp(s_history_segments[segment] + i, needle,
                       needle_len) == 0) {
                return true;
            }
        }
    }
    return false;
}

static persisted_header_t fallback_header(d1l_retained_blob_store_id_t store_id)
{
    persisted_header_t header = {0};
    assert(store_id < D1L_RETAINED_BLOB_STORE_COUNT);
    assert(s_fallback[store_id].valid);
    assert(s_fallback[store_id].len >= sizeof(header));
    memcpy(&header, s_fallback[store_id].data, sizeof(header));
    return header;
}

static void assert_header_lineage(persisted_header_t header, uint32_t next_seq,
                                  uint32_t total_written, uint32_t dropped_oldest)
{
    assert(header.next_seq == next_seq);
    assert(header.total_written == total_written);
    assert(header.dropped_oldest == dropped_oldest);
}

int64_t esp_timer_get_time(void)
{
    s_now_us += 1000;
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

bool d1l_retained_blob_store_uses_sd(d1l_retained_blob_store_id_t store_id)
{
    return s_use_packet_sd && store_id == D1L_RETAINED_BLOB_STORE_PACKET_LOG;
}

bool d1l_retained_blob_store_backend_state(
    d1l_retained_blob_store_id_t store_id,
    d1l_retained_blob_store_backend_state_t *out_state)
{
    if (store_id >= D1L_RETAINED_BLOB_STORE_COUNT || !out_state) {
        return false;
    }
    out_state->enabled = d1l_retained_blob_store_uses_sd(store_id);
    out_state->generation = out_state->enabled ? 1U : 0U;
    return true;
}

esp_err_t d1l_retained_blob_store_sd_media_lineage_ready(
    d1l_retained_blob_store_id_t store_id,
    uint32_t expected_backend_generation, bool *out_ready)
{
    if (!out_ready || store_id != D1L_RETAINED_BLOB_STORE_PACKET_LOG ||
        !s_use_packet_sd || expected_backend_generation != 1U) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_ready = true;
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_complete_packet_reset_lineage(
    uint32_t expected_backend_generation)
{
    return s_use_packet_sd && expected_backend_generation == 1U ?
        ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t d1l_retained_blob_store_read_sd_primary(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    (void)key;
    if (!d1l_retained_blob_store_uses_sd(store_id)) {
        return ESP_ERR_INVALID_STATE;
    }
    return mock_blob_read(&s_primary[store_id], dst, len_inout);
}

esp_err_t d1l_retained_blob_store_read_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key,
    void *dst, size_t *len_inout)
{
    (void)key;
    return mock_blob_read(&s_fallback[store_id], dst, len_inout);
}

esp_err_t d1l_retained_blob_store_write_sd_primary(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len)
{
    (void)key;
    if (!d1l_retained_blob_store_uses_sd(store_id)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = mock_blob_write(&s_primary[store_id], src, len);
    if (ret == ESP_OK) {
        s_write_count[store_id]++;
    }
    return ret;
}

esp_err_t d1l_retained_blob_store_write_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len, uint32_t expected_generation)
{
    if (expected_generation != 1U) {
        return ESP_ERR_INVALID_STATE;
    }
    return d1l_retained_blob_store_write_sd_primary(store_id, key, src, len);
}

esp_err_t d1l_retained_blob_store_write_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key,
    const void *src, size_t len)
{
    (void)key;
    const esp_err_t ret = mock_blob_write(&s_fallback[store_id], src, len);
    if (ret == ESP_OK) {
        s_write_count[store_id]++;
    }
    return ret;
}

esp_err_t d1l_retained_blob_store_erase_sd_primary(
    d1l_retained_blob_store_id_t store_id, const char *key)
{
    (void)key;
    if (!d1l_retained_blob_store_uses_sd(store_id)) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_primary[store_id], 0, sizeof(s_primary[store_id]));
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_erase_sd_primary_guarded(
    d1l_retained_blob_store_id_t store_id, const char *key,
    uint32_t expected_generation)
{
    if (expected_generation != 1U) {
        return ESP_ERR_INVALID_STATE;
    }
    return d1l_retained_blob_store_erase_sd_primary(store_id, key);
}

esp_err_t d1l_retained_blob_store_erase_nvs_fallback(
    d1l_retained_blob_store_id_t store_id, const char *key)
{
    (void)key;
    memset(&s_fallback[store_id], 0, sizeof(s_fallback[store_id]));
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_read(d1l_retained_blob_store_id_t store_id,
                                       const char *key, void *dst, size_t *len_inout)
{
    (void)key;
    const mock_blob_t *blob = d1l_retained_blob_store_uses_sd(store_id) ?
        &s_primary[store_id] : &s_fallback[store_id];
    return mock_blob_read(blob, dst, len_inout);
}

esp_err_t d1l_retained_blob_store_read_fallback(d1l_retained_blob_store_id_t store_id,
                                                const char *key, void *dst,
                                                size_t *len_inout)
{
    (void)key;
    return mock_blob_read(&s_fallback[store_id], dst, len_inout);
}

esp_err_t d1l_retained_blob_store_write(d1l_retained_blob_store_id_t store_id,
                                        const char *key, const void *src, size_t len)
{
    (void)key;
    const esp_err_t ret = mock_blob_write(&s_fallback[store_id], src, len);
    if (ret == ESP_OK) {
        s_write_count[store_id]++;
    }
    return ret;
}

esp_err_t d1l_retained_blob_store_write_split(d1l_retained_blob_store_id_t store_id,
                                              const char *key,
                                              const void *primary_src,
                                              size_t primary_len,
                                              const void *fallback_src,
                                              size_t fallback_len)
{
    (void)key;
    if (primary_src && primary_len > 0U) {
        const esp_err_t ret = mock_blob_write(&s_primary[store_id], primary_src,
                                              primary_len);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (fallback_src && fallback_len > 0U) {
        const esp_err_t ret = mock_blob_write(&s_fallback[store_id], fallback_src,
                                              fallback_len);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    s_write_count[store_id]++;
    return ESP_OK;
}

esp_err_t d1l_retained_blob_store_erase(d1l_retained_blob_store_id_t store_id,
                                        const char *key)
{
    (void)key;
    memset(&s_primary[store_id], 0, sizeof(s_primary[store_id]));
    memset(&s_fallback[store_id], 0, sizeof(s_fallback[store_id]));
    return ESP_OK;
}

esp_err_t d1l_rp2040_bridge_file_read(const char *path, uint32_t offset,
                                      uint8_t *out_data, size_t max_len,
                                      d1l_rp2040_file_result_t *out_result,
                                      uint32_t timeout_ms)
{
    unsigned long segment = 0;
    (void)timeout_ms;
    if (!path || !out_data || !out_result ||
        sscanf(path, "stores/packet_log/segments/s%lu.bin", &segment) != 1 ||
        segment >= D1L_PACKET_LOG_SD_SEGMENT_COUNT ||
        offset >= s_history_segment_len[segment]) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t seq = s_history_record_len == 0U ? 0U :
        (uint32_t)segment * D1L_PACKET_LOG_SD_SEGMENT_CAPACITY +
        offset / (uint32_t)s_history_record_len + 1U;
    if (s_fail_history_read_seq_min > 0U &&
        seq >= s_fail_history_read_seq_min) {
        return ESP_FAIL;
    }
    const size_t available = s_history_segment_len[segment] - offset;
    const size_t copied = available < max_len ? available : max_len;
    memcpy(out_data, s_history_segments[segment] + offset, copied);
    memset(out_result, 0, sizeof(*out_result));
    out_result->length = copied;
    out_result->ok = true;
    return ESP_OK;
}

esp_err_t d1l_rp2040_bridge_file_write(const char *path, uint32_t offset,
                                       const uint8_t *data, size_t len,
                                       bool truncate,
                                       d1l_rp2040_file_result_t *out_result,
                                       uint32_t timeout_ms)
{
    unsigned long segment = 0;
    (void)timeout_ms;
    if (!path || !data || !out_result ||
        sscanf(path, "stores/packet_log/segments/s%lu.bin", &segment) != 1 ||
        segment >= D1L_PACKET_LOG_SD_SEGMENT_COUNT ||
        offset + len > MOCK_HISTORY_SEGMENT_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (truncate) {
        s_history_segment_len[segment] = 0;
    }
    if (s_history_record_len == 0U) {
        s_history_record_len = len;
    }
    assert(s_history_record_len == len);
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
    unsigned long segment = 0;
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
    unsigned long segment = 0;
    (void)timeout_ms;
    if (!path || !out_result ||
        sscanf(path, "stores/packet_log/segments/s%lu.bin", &segment) != 1 ||
        segment >= D1L_PACKET_LOG_SD_SEGMENT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(*out_result));
    if (s_history_segment_len[segment] == 0U) {
        (void)snprintf(out_result->err, sizeof(out_result->err), "not_found");
        return ESP_ERR_NOT_FOUND;
    }
    memset(s_history_segments[segment], 0,
           sizeof(s_history_segments[segment]));
    s_history_segment_len[segment] = 0;
    out_result->ok = true;
    return ESP_OK;
}

static void assert_message_stats_equal(d1l_message_store_stats_t left,
                                       d1l_message_store_stats_t right)
{
    assert(left.next_seq == right.next_seq);
    assert(left.total_written == right.total_written);
    assert(left.dropped_oldest == right.dropped_oldest);
    assert(left.epoch == right.epoch);
    assert(left.content_revision == right.content_revision);
    assert(left.persistence_commit_count == right.persistence_commit_count);
    assert(left.persistence_fail_count == right.persistence_fail_count);
    assert(left.persistence_stale_snapshot_count == right.persistence_stale_snapshot_count);
    assert(left.sd_primary_commit_count == right.sd_primary_commit_count);
    assert(left.sd_primary_fail_count == right.sd_primary_fail_count);
    assert(left.sd_backend_generation == right.sd_backend_generation);
    assert(left.sd_primary_last_error == right.sd_primary_last_error);
    assert(left.nvs_fallback_commit_count == right.nvs_fallback_commit_count);
    assert(left.nvs_fallback_fail_count == right.nvs_fallback_fail_count);
    assert(left.nvs_fallback_last_error == right.nvs_fallback_last_error);
    assert(left.persistence_revision == right.persistence_revision);
    assert(left.count == right.count);
    assert(left.capacity == right.capacity);
    assert(left.persistence_dirty == right.persistence_dirty);
    assert(left.sd_primary_required == right.sd_primary_required);
    assert(left.sd_primary_dirty == right.sd_primary_dirty);
    assert(left.sd_primary_reconcile_pending == right.sd_primary_reconcile_pending);
    assert(left.nvs_fallback_dirty == right.nvs_fallback_dirty);
}

static void test_message_volatile_preview_does_not_consume_history(void)
{
    mock_reset();
    assert(d1l_message_store_init() == ESP_OK);
    char text[40];
    for (uint32_t i = 1; i <= D1L_MESSAGE_STORE_CAPACITY; ++i) {
        (void)snprintf(text, sizeof(text), "public-durable-%lu", (unsigned long)i);
        assert(d1l_message_store_append_public("rx", "Node", text, -60, 40,
                                               1U, 1U, true) == ESP_OK);
    }

    const d1l_message_store_stats_t before = d1l_message_store_stats();
    assert(before.next_seq == D1L_MESSAGE_STORE_CAPACITY + 1U);
    assert(before.total_written == D1L_MESSAGE_STORE_CAPACITY);
    assert(before.dropped_oldest == 0U);
    const uint32_t writes_before =
        s_write_count[D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES];
    const persisted_header_t header_before =
        fallback_header(D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES);

    assert(d1l_message_store_append_public_volatile(
               "rx", "UI Canary", "ui-data-canary public-test", -55, 60,
               1U, 1U, true) == ESP_OK);
    assert_message_stats_equal(d1l_message_store_stats(), before);
    assert(s_write_count[D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES] == writes_before);
    const persisted_header_t header_after_preview =
        fallback_header(D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES);
    assert(memcmp(&header_before, &header_after_preview,
                  sizeof(header_before)) == 0);

    d1l_message_entry_t visible[D1L_MESSAGE_STORE_CAPACITY + 1U] = {0};
    assert(d1l_message_store_copy_recent(visible,
                                         D1L_MESSAGE_STORE_CAPACITY + 1U) ==
           D1L_MESSAGE_STORE_CAPACITY + 1U);
    assert(visible[0].seq == 1U);
    assert(visible[D1L_MESSAGE_STORE_CAPACITY].seq == before.next_seq);
    assert(strstr(visible[D1L_MESSAGE_STORE_CAPACITY].text,
                  "ui-data-canary") != NULL);
    size_t matches = 0;
    assert(d1l_message_store_query_page(visible, 1U, 0U, "public-test",
                                        &matches) == 1U);
    assert(matches == 1U);
    assert(d1l_message_store_query_page(visible, 1U, 1U, NULL,
                                        &matches) == 1U);
    assert(matches == D1L_MESSAGE_STORE_CAPACITY + 1U);
    assert(visible[0].seq == D1L_MESSAGE_STORE_CAPACITY);
    d1l_message_store_stats_t snapshot_stats = {0};
    uint32_t volatile_seq = 0U;
    memset(visible, 0, sizeof(visible));
    assert(d1l_message_store_query_page_snapshot(
               visible, D1L_MESSAGE_STORE_CAPACITY + 1U, 0U, NULL,
               &matches, &snapshot_stats, &volatile_seq) ==
           D1L_MESSAGE_STORE_CAPACITY + 1U);
    assert_message_stats_equal(snapshot_stats, before);
    assert(volatile_seq == before.next_seq);
    assert(visible[D1L_MESSAGE_STORE_CAPACITY].seq == volatile_seq);
    memset(visible, 0, sizeof(visible));
    assert(d1l_message_store_query_page_snapshot(
               visible, 1U, 1U, NULL, &matches, &snapshot_stats,
               &volatile_seq) == 1U);
    assert(matches == D1L_MESSAGE_STORE_CAPACITY + 1U);
    assert(visible[0].seq == D1L_MESSAGE_STORE_CAPACITY);
    assert(visible[0].seq != volatile_seq);
    assert(d1l_message_store_append_public_volatile(
               "rx", "UI Canary", "ui-data-canary replacement", -54, 61,
               1U, 1U, true) == ESP_OK);
    memset(visible, 0, sizeof(visible));
    assert(d1l_message_store_query_page_snapshot(
               visible, 1U, 0U, "replacement", &matches, &snapshot_stats,
               &volatile_seq) == 1U);
    assert(matches == 1U);
    assert_message_stats_equal(snapshot_stats, before);
    assert(volatile_seq == before.next_seq);
    assert(visible[0].seq == volatile_seq);
    assert(strcmp(visible[0].text, "ui-data-canary replacement") == 0);

    assert(d1l_message_store_append_public("rx", "Node", "public-real-next",
                                           -50, 70, 1U, 1U, true) == ESP_OK);
    const d1l_message_store_stats_t after = d1l_message_store_stats();
    assert(after.next_seq == before.next_seq + 1U);
    assert(after.total_written == before.total_written + 1U);
    assert(after.dropped_oldest == before.dropped_oldest + 1U);
    assert(d1l_message_store_copy_recent(visible, D1L_MESSAGE_STORE_CAPACITY) ==
           D1L_MESSAGE_STORE_CAPACITY);
    assert(visible[0].seq == 2U);
    assert(visible[D1L_MESSAGE_STORE_CAPACITY - 1U].seq == before.next_seq);
    assert(strcmp(visible[D1L_MESSAGE_STORE_CAPACITY - 1U].text,
                  "public-real-next") == 0);
    assert(!blob_contains(&s_fallback[D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES],
                          "ui-data-canary"));
    assert_header_lineage(
        fallback_header(D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES),
        after.next_seq, after.total_written, after.dropped_oldest);
}

static void assert_dm_stats_equal(d1l_dm_store_stats_t left,
                                  d1l_dm_store_stats_t right)
{
    assert(left.next_seq == right.next_seq);
    assert(left.total_written == right.total_written);
    assert(left.dropped_oldest == right.dropped_oldest);
    assert(left.epoch == right.epoch);
    assert(left.content_revision == right.content_revision);
    assert(left.persistence_commit_count == right.persistence_commit_count);
    assert(left.persistence_fail_count == right.persistence_fail_count);
    assert(left.persistence_stale_snapshot_count == right.persistence_stale_snapshot_count);
    assert(left.sd_primary_commit_count == right.sd_primary_commit_count);
    assert(left.sd_primary_fail_count == right.sd_primary_fail_count);
    assert(left.sd_backend_generation == right.sd_backend_generation);
    assert(left.sd_primary_last_error == right.sd_primary_last_error);
    assert(left.nvs_fallback_commit_count == right.nvs_fallback_commit_count);
    assert(left.nvs_fallback_fail_count == right.nvs_fallback_fail_count);
    assert(left.nvs_fallback_last_error == right.nvs_fallback_last_error);
    assert(left.persistence_revision == right.persistence_revision);
    assert(left.count == right.count);
    assert(left.capacity == right.capacity);
    assert(left.persistence_dirty == right.persistence_dirty);
    assert(left.sd_primary_required == right.sd_primary_required);
    assert(left.sd_primary_dirty == right.sd_primary_dirty);
    assert(left.sd_primary_reconcile_pending == right.sd_primary_reconcile_pending);
    assert(left.nvs_fallback_dirty == right.nvs_fallback_dirty);
}

static void test_dm_volatile_preview_does_not_consume_history(void)
{
    static const char fingerprint[] = "0123456789abcdef";
    mock_reset();
    assert(d1l_dm_store_init() == ESP_OK);
    char text[40];
    for (uint32_t i = 1; i <= D1L_DM_STORE_CAPACITY; ++i) {
        (void)snprintf(text, sizeof(text), "dm-durable-%lu", (unsigned long)i);
        assert(d1l_dm_store_append(fingerprint, "Node", "rx", text, -60, 40,
                                   1U, 1U, 1U, true, false, i) == ESP_OK);
    }

    const d1l_dm_store_stats_t before = d1l_dm_store_stats();
    const uint32_t writes_before =
        s_write_count[D1L_RETAINED_BLOB_STORE_DM_MESSAGES];
    const persisted_header_t header_before =
        fallback_header(D1L_RETAINED_BLOB_STORE_DM_MESSAGES);
    assert(d1l_dm_store_append_volatile(
               fingerprint, "UI Canary", "rx", "ui-data-canary dm-test",
               -55, 60, 1U, 1U, 1U, true, false, 0x1234U) == ESP_OK);
    assert_dm_stats_equal(d1l_dm_store_stats(), before);
    assert(s_write_count[D1L_RETAINED_BLOB_STORE_DM_MESSAGES] == writes_before);
    const persisted_header_t header_after_preview =
        fallback_header(D1L_RETAINED_BLOB_STORE_DM_MESSAGES);
    assert(memcmp(&header_before, &header_after_preview, sizeof(header_before)) == 0);

    d1l_dm_entry_t visible[D1L_DM_STORE_CAPACITY + 1U] = {0};
    size_t matches = 0;
    assert(d1l_dm_store_copy_recent_page(visible,
                                         D1L_DM_STORE_CAPACITY + 1U,
                                         0U, &matches) ==
           D1L_DM_STORE_CAPACITY + 1U);
    assert(matches == D1L_DM_STORE_CAPACITY + 1U);
    assert(visible[0].seq == 1U);
    assert(visible[D1L_DM_STORE_CAPACITY].seq == before.next_seq);
    assert(d1l_dm_store_copy_thread_page(fingerprint, visible, 1U, 0U,
                                         &matches) == 1U);
    assert(matches == D1L_DM_STORE_CAPACITY + 1U);
    assert(strstr(visible[0].text, "ui-data-canary") != NULL);
    assert(d1l_dm_store_copy_recent_page(visible, 1U, 1U, &matches) == 1U);
    assert(matches == D1L_DM_STORE_CAPACITY + 1U);
    assert(visible[0].seq == D1L_DM_STORE_CAPACITY);
    assert(d1l_dm_store_copy_thread_page(fingerprint, visible, 1U, 1U,
                                         &matches) == 1U);
    assert(visible[0].seq == D1L_DM_STORE_CAPACITY);
    d1l_dm_store_stats_t snapshot_stats = {0};
    uint32_t volatile_seq = 0U;
    memset(visible, 0, sizeof(visible));
    assert(d1l_dm_store_copy_recent_page_snapshot(
               visible, D1L_DM_STORE_CAPACITY + 1U, 0U, &matches,
               &snapshot_stats, &volatile_seq) ==
           D1L_DM_STORE_CAPACITY + 1U);
    assert_dm_stats_equal(snapshot_stats, before);
    assert(volatile_seq == before.next_seq);
    assert(visible[D1L_DM_STORE_CAPACITY].seq == volatile_seq);
    memset(visible, 0, sizeof(visible));
    assert(d1l_dm_store_copy_recent_page_snapshot(
               visible, 1U, 1U, &matches, &snapshot_stats,
               &volatile_seq) == 1U);
    assert(matches == D1L_DM_STORE_CAPACITY + 1U);
    assert(visible[0].seq == D1L_DM_STORE_CAPACITY);
    assert(visible[0].seq != volatile_seq);
    memset(visible, 0, sizeof(visible));
    volatile_seq = 0U;
    assert(d1l_dm_store_copy_thread_page_snapshot(
               fingerprint, visible, D1L_DM_STORE_CAPACITY + 1U, 0U,
               &matches, &snapshot_stats, &volatile_seq) ==
           D1L_DM_STORE_CAPACITY + 1U);
    assert_dm_stats_equal(snapshot_stats, before);
    assert(volatile_seq == before.next_seq);
    assert(visible[D1L_DM_STORE_CAPACITY].seq == volatile_seq);
    assert(d1l_dm_store_append_volatile(
               fingerprint, "UI Canary", "rx",
               "ui-data-canary dm-replacement", -54, 61, 1U, 1U, 1U,
               true, false, 0x4321U) == ESP_OK);
    memset(visible, 0, sizeof(visible));
    assert(d1l_dm_store_copy_thread_page_snapshot(
               fingerprint, visible, 1U, 0U, &matches, &snapshot_stats,
               &volatile_seq) == 1U);
    assert(matches == D1L_DM_STORE_CAPACITY + 1U);
    assert_dm_stats_equal(snapshot_stats, before);
    assert(volatile_seq == before.next_seq);
    assert(visible[0].seq == volatile_seq);
    assert(strcmp(visible[0].text,
                  "ui-data-canary dm-replacement") == 0);

    memset(visible, 0, sizeof(visible));
    assert(d1l_dm_store_query_thread_page(
               fingerprint, visible, 3U, 0U, "DM-DURABLE", &matches) == 3U);
    assert(matches == D1L_DM_STORE_CAPACITY);
    assert(visible[0].seq == D1L_DM_STORE_CAPACITY - 2U);
    assert(visible[1].seq == D1L_DM_STORE_CAPACITY - 1U);
    assert(visible[2].seq == D1L_DM_STORE_CAPACITY);
    memset(visible, 0, sizeof(visible));
    assert(d1l_dm_store_query_thread_page(
               fingerprint, visible, 1U, 0U, "UI-DATA-CANARY",
               &matches) == 1U);
    assert(matches == 1U);
    assert(visible[0].seq == before.next_seq);
    assert(strcmp(visible[0].text,
                  "ui-data-canary dm-replacement") == 0);
    assert_dm_stats_equal(d1l_dm_store_stats(), before);
    assert(s_write_count[D1L_RETAINED_BLOB_STORE_DM_MESSAGES] == writes_before);

    assert(d1l_dm_store_append(fingerprint, "Node", "rx", "dm-real-next",
                               -50, 70, 1U, 1U, 1U, true, false,
                               0x9876U) == ESP_OK);
    const d1l_dm_store_stats_t after = d1l_dm_store_stats();
    assert(after.next_seq == before.next_seq + 1U);
    assert(after.total_written == before.total_written + 1U);
    assert(after.dropped_oldest == before.dropped_oldest + 1U);
    assert(d1l_dm_store_copy_recent(visible, D1L_DM_STORE_CAPACITY) ==
           D1L_DM_STORE_CAPACITY);
    assert(visible[0].seq == 2U);
    assert(visible[D1L_DM_STORE_CAPACITY - 1U].seq == before.next_seq);
    assert(strcmp(visible[D1L_DM_STORE_CAPACITY - 1U].text,
                  "dm-real-next") == 0);
    assert(!blob_contains(&s_fallback[D1L_RETAINED_BLOB_STORE_DM_MESSAGES],
                          "ui-data-canary"));
    assert_header_lineage(fallback_header(D1L_RETAINED_BLOB_STORE_DM_MESSAGES),
                          after.next_seq, after.total_written,
                          after.dropped_oldest);
}

static void assert_packet_stats_equal(d1l_packet_log_stats_t left,
                                      d1l_packet_log_stats_t right)
{
    assert(left.next_seq == right.next_seq);
    assert(left.total_written == right.total_written);
    assert(left.dropped_oldest == right.dropped_oldest);
    assert(left.sd_history_records == right.sd_history_records);
    assert(left.sd_history_failed_writes == right.sd_history_failed_writes);
    assert(left.sd_backend_generation == right.sd_backend_generation);
    assert(left.persistence_commit_count == right.persistence_commit_count);
    assert(left.persistence_fail_count == right.persistence_fail_count);
    assert(left.sd_reconcile_count == right.sd_reconcile_count);
    assert(left.sd_reconcile_fail_count == right.sd_reconcile_fail_count);
    assert(left.sd_reconcile_last_error == right.sd_reconcile_last_error);
    assert(left.sd_primary_commit_count == right.sd_primary_commit_count);
    assert(left.sd_primary_fail_count == right.sd_primary_fail_count);
    assert(left.sd_primary_last_error == right.sd_primary_last_error);
    assert(left.nvs_fallback_commit_count == right.nvs_fallback_commit_count);
    assert(left.nvs_fallback_fail_count == right.nvs_fallback_fail_count);
    assert(left.nvs_fallback_last_error == right.nvs_fallback_last_error);
    assert(left.journal_commit_count == right.journal_commit_count);
    assert(left.journal_fail_count == right.journal_fail_count);
    assert(left.journal_last_error == right.journal_last_error);
    assert(left.persistence_revision == right.persistence_revision);
    assert(left.count == right.count);
    assert(left.capacity == right.capacity);
    assert(left.sd_capacity == right.sd_capacity);
    assert(left.persistence_dirty == right.persistence_dirty);
    assert(left.sd_primary_dirty == right.sd_primary_dirty);
    assert(left.sd_primary_reconcile_pending == right.sd_primary_reconcile_pending);
    assert(left.nvs_fallback_dirty == right.nvs_fallback_dirty);
    assert(left.journal_dirty == right.journal_dirty);
    assert(left.sd_history_enabled == right.sd_history_enabled);
}

static bool append_packet(const char *note, bool volatile_preview)
{
    d1l_packet_log_entry_t entry = {
        .direction = "rx",
        .kind = "mesh",
        .note = "",
    };
    (void)snprintf(entry.note, sizeof(entry.note), "%s", note);
    return volatile_preview ?
        d1l_packet_log_append_raw_volatile(&entry, (const uint8_t *)note,
                                           strlen(note)) :
        d1l_packet_log_append_raw(&entry, (const uint8_t *)note, strlen(note));
}

static void test_packet_volatile_preview_does_not_consume_history(void)
{
    mock_reset();
    assert(d1l_packet_log_init() == ESP_OK);
    d1l_packet_log_entry_t stale_preview = {
        .direction = "rx",
        .kind = "ui_canary",
        .note = "ui-canary stale-preview",
    };
    assert(d1l_packet_log_append_raw_volatile(&stale_preview, NULL, 0U));
    d1l_packet_log_entry_t one[1] = {0};
    assert(d1l_packet_log_copy_recent(one, 1U) == 1U);
    assert(d1l_packet_log_init() == ESP_OK);
    assert(d1l_packet_log_copy_recent(one, 1U) == 0U);
    assert(d1l_packet_log_append_raw_volatile(&stale_preview, NULL, 0U));
    assert(d1l_packet_log_clear() == ESP_OK);
    assert(d1l_packet_log_copy_recent(one, 1U) == 0U);

    char note[40];
    for (uint32_t i = 1; i <= D1L_PACKET_LOG_CAPACITY; ++i) {
        (void)snprintf(note, sizeof(note), "packet-durable-%lu", (unsigned long)i);
        assert(append_packet(note, false));
    }

    const d1l_packet_log_stats_t before = d1l_packet_log_stats();
    const uint32_t writes_before =
        s_write_count[D1L_RETAINED_BLOB_STORE_PACKET_LOG];
    const persisted_header_t header_before =
        fallback_header(D1L_RETAINED_BLOB_STORE_PACKET_LOG);

    d1l_packet_log_entry_t canary = {
        .direction = "rx",
        .kind = "ui_canary",
        .note = "ui-canary packet-test",
    };
    assert(d1l_packet_log_append_raw_volatile(
        &canary, (const uint8_t *)"ui-data-canary packet-test",
        strlen("ui-data-canary packet-test")));
    assert_packet_stats_equal(d1l_packet_log_stats(), before);
    assert(s_write_count[D1L_RETAINED_BLOB_STORE_PACKET_LOG] == writes_before);
    const persisted_header_t header_after_preview =
        fallback_header(D1L_RETAINED_BLOB_STORE_PACKET_LOG);
    assert(memcmp(&header_before, &header_after_preview, sizeof(header_before)) == 0);

    d1l_packet_log_entry_t visible[D1L_PACKET_LOG_CAPACITY + 1U] = {0};
    assert(d1l_packet_log_copy_recent(visible, D1L_PACKET_LOG_CAPACITY + 1U) ==
           D1L_PACKET_LOG_CAPACITY + 1U);
    assert(visible[0].seq == 1U);
    assert(visible[D1L_PACKET_LOG_CAPACITY].seq == before.next_seq);
    size_t matches = 0;
    bool sd_used = true;
    assert(d1l_packet_log_query_page(visible, 1U, 0U, "any", "ui_canary",
                                     "packet-test", &matches, &sd_used) == 1U);
    assert(matches == 1U);
    assert(!sd_used);
    assert(d1l_packet_log_query_page(visible, 1U, 1U, "any", "any",
                                     "any", &matches, &sd_used) == 1U);
    assert(matches == D1L_PACKET_LOG_CAPACITY + 1U);
    assert(visible[0].seq == D1L_PACKET_LOG_CAPACITY);
    d1l_packet_log_entry_t found = {0};
    assert(d1l_packet_log_find_by_seq(before.next_seq, &found) == ESP_OK);
    assert(strcmp(found.kind, "ui_canary") == 0);

    /* Even an explicit flush after the preview must serialize only durable
     * rows and their original lineage. */
    assert(d1l_packet_log_flush() == ESP_OK);
    assert_packet_stats_equal(d1l_packet_log_stats(), before);
    assert(!blob_contains(&s_fallback[D1L_RETAINED_BLOB_STORE_PACKET_LOG],
                          "ui-canary"));
    assert_header_lineage(fallback_header(D1L_RETAINED_BLOB_STORE_PACKET_LOG),
                          before.next_seq, before.total_written,
                          before.dropped_oldest);

    assert(append_packet("packet-real-next", false));
    const d1l_packet_log_stats_t after = d1l_packet_log_stats();
    assert(after.next_seq == before.next_seq + 1U);
    assert(after.total_written == before.total_written + 1U);
    assert(after.dropped_oldest == before.dropped_oldest + 1U);
    assert(d1l_packet_log_copy_recent(visible, D1L_PACKET_LOG_CAPACITY) ==
           D1L_PACKET_LOG_CAPACITY);
    assert(visible[0].seq == 2U);
    assert(visible[D1L_PACKET_LOG_CAPACITY - 1U].seq == before.next_seq);
    assert(strcmp(visible[D1L_PACKET_LOG_CAPACITY - 1U].note,
                  "packet-real-next") == 0);
    assert(d1l_packet_log_find_by_seq(before.next_seq, &found) == ESP_OK);
    assert(strcmp(found.note, "packet-real-next") == 0);
    assert(!blob_contains(&s_fallback[D1L_RETAINED_BLOB_STORE_PACKET_LOG],
                          "ui-canary"));
    assert_header_lineage(fallback_header(D1L_RETAINED_BLOB_STORE_PACKET_LOG),
                          after.next_seq, after.total_written,
                          after.dropped_oldest);
}

static void test_packet_sd_query_orders_volatile_preview_after_history(void)
{
    mock_reset();
    s_use_packet_sd = true;
    assert(d1l_packet_log_init() == ESP_OK);
    char note[40];
    for (uint32_t i = 1; i <= D1L_PACKET_LOG_CAPACITY + 1U; ++i) {
        (void)snprintf(note, sizeof(note), "sd-packet-%lu", (unsigned long)i);
        assert(append_packet(note, false));
    }
    const d1l_packet_log_stats_t before = d1l_packet_log_stats();
    assert(before.count == D1L_PACKET_LOG_CAPACITY);
    assert(before.sd_history_records == D1L_PACKET_LOG_CAPACITY + 1U);
    assert(before.dropped_oldest == 1U);
    const uint32_t writes_before =
        s_write_count[D1L_RETAINED_BLOB_STORE_PACKET_LOG];

    d1l_packet_log_entry_t canary = {
        .direction = "rx",
        .kind = "ui_canary",
        .note = "ui-canary sd-order",
    };
    assert(d1l_packet_log_append_raw_volatile(&canary, NULL, 0U));
    assert_packet_stats_equal(d1l_packet_log_stats(), before);
    assert(s_write_count[D1L_RETAINED_BLOB_STORE_PACKET_LOG] == writes_before);
    assert(!history_contains("ui-canary"));

    d1l_packet_log_entry_t page[3] = {0};
    size_t matches = 0;
    bool sd_used = false;
    assert(d1l_packet_log_query_page(page, 3U, 0U, "any", "any", "any",
                                     &matches, &sd_used) == 3U);
    assert(sd_used);
    assert(matches == D1L_PACKET_LOG_CAPACITY + 2U);
    assert(page[0].seq == D1L_PACKET_LOG_CAPACITY);
    assert(page[1].seq == D1L_PACKET_LOG_CAPACITY + 1U);
    assert(page[2].seq == before.next_seq);
    assert(strcmp(page[2].kind, "ui_canary") == 0);

    memset(page, 0, sizeof(page));
    assert(d1l_packet_log_query_page(page, 3U, 1U, "any", "any", "any",
                                     &matches, &sd_used) == 3U);
    assert(page[0].seq == D1L_PACKET_LOG_CAPACITY - 1U);
    assert(page[1].seq == D1L_PACKET_LOG_CAPACITY);
    assert(page[2].seq == D1L_PACKET_LOG_CAPACITY + 1U);

    memset(page, 0, sizeof(page));
    assert(d1l_packet_log_query_page(page, 1U, 0U, "any", "ui_canary",
                                     "sd-order", &matches, &sd_used) == 1U);
    assert(matches == 1U);
    assert(sd_used);
    assert(page[0].seq == before.next_seq);

    assert(d1l_packet_log_flush() == ESP_OK);
    assert_packet_stats_equal(d1l_packet_log_stats(), before);
    assert(!blob_contains(&s_primary[D1L_RETAINED_BLOB_STORE_PACKET_LOG],
                          "ui-canary"));
    assert(!blob_contains(&s_fallback[D1L_RETAINED_BLOB_STORE_PACKET_LOG],
                          "ui-canary"));
    assert(!history_contains("ui-canary"));

    assert(append_packet("sd-packet-real-next", false));
    const d1l_packet_log_stats_t after = d1l_packet_log_stats();
    assert(after.next_seq == before.next_seq + 1U);
    assert(after.total_written == before.total_written + 1U);
    assert(after.dropped_oldest == before.dropped_oldest + 1U);
    memset(page, 0, sizeof(page));
    assert(d1l_packet_log_query_page(page, 3U, 0U, "any", "any", "any",
                                     &matches, &sd_used) == 3U);
    assert(page[2].seq == before.next_seq);
    assert(strcmp(page[2].note, "sd-packet-real-next") == 0);
}

static void test_packet_sd_query_uses_recent_ram_when_newest_segment_reads_fail(void)
{
    mock_reset();
    s_use_packet_sd = true;
    assert(d1l_packet_log_init() == ESP_OK);
    char note[40];
    for (uint32_t i = 1U; i <= D1L_PACKET_LOG_CAPACITY + 2U; ++i) {
        (void)snprintf(note, sizeof(note), "overlay-packet-%lu",
                       (unsigned long)i);
        assert(append_packet(note, false));
    }
    s_fail_history_read_seq_min = D1L_PACKET_LOG_CAPACITY + 1U;
    const uint32_t newest_seq = D1L_PACKET_LOG_CAPACITY + 2U;
    char target[40];
    (void)snprintf(target, sizeof(target), "overlay-packet-%lu",
                   (unsigned long)newest_seq);

    d1l_packet_log_entry_t direct = {0};
    assert(d1l_packet_log_find_by_seq(newest_seq, &direct) == ESP_OK);
    assert(strcmp(direct.note, target) == 0);

    d1l_packet_log_entry_t page[3] = {0};
    size_t matches = 0U;
    bool sd_used = false;
    assert(d1l_packet_log_query_page(
               page, 3U, 0U, "any", "any", target,
               &matches, &sd_used) == 1U);
    assert(sd_used);
    assert(matches == 1U);
    assert(page[0].seq == newest_seq);
    assert(strcmp(page[0].note, target) == 0);

    memset(page, 0, sizeof(page));
    assert(d1l_packet_log_query_page(page, 3U, 0U, "any", "any", "any",
                                     &matches, &sd_used) == 3U);
    assert(sd_used);
    assert(matches == D1L_PACKET_LOG_CAPACITY + 2U);
    assert(page[0].seq == D1L_PACKET_LOG_CAPACITY);
    assert(page[1].seq == D1L_PACKET_LOG_CAPACITY + 1U);
    assert(page[2].seq == D1L_PACKET_LOG_CAPACITY + 2U);

    memset(page, 0, sizeof(page));
    assert(d1l_packet_log_query_page(page, 3U, 1U, "any", "any", "any",
                                     &matches, &sd_used) == 3U);
    assert(sd_used);
    assert(matches == D1L_PACKET_LOG_CAPACITY + 2U);
    assert(page[0].seq == D1L_PACKET_LOG_CAPACITY - 1U);
    assert(page[1].seq == D1L_PACKET_LOG_CAPACITY);
    assert(page[2].seq == D1L_PACKET_LOG_CAPACITY + 1U);

    memset(page, 0, sizeof(page));
    assert(d1l_packet_log_query_page(page, 3U, 1U, "any", "any",
                                     "overlay-packet", &matches,
                                     &sd_used) == 3U);
    assert(sd_used);
    assert(matches == D1L_PACKET_LOG_CAPACITY + 2U);
    assert(page[0].seq == D1L_PACKET_LOG_CAPACITY - 1U);
    assert(page[1].seq == D1L_PACKET_LOG_CAPACITY);
    assert(page[2].seq == D1L_PACKET_LOG_CAPACITY + 1U);
}

static void test_sentinel_shaped_durable_rows_survive_persistence(void)
{
    static const char fingerprint[] = "0123456789abcdef";

    mock_reset();
    assert(d1l_message_store_init() == ESP_OK);
    assert(d1l_message_store_append_public(
               "rx", "UI Canary", "ui-data-canary legitimate-public",
               -60, 40, 1U, 1U, true) == ESP_OK);
    assert(blob_contains(
        &s_fallback[D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES],
        "ui-data-canary legitimate-public"));
    assert(fallback_header(D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES).count == 1U);
    assert(d1l_message_store_init() == ESP_OK);
    d1l_message_entry_t public_row = {0};
    assert(d1l_message_store_copy_recent(&public_row, 1U) == 1U);
    assert(strcmp(public_row.author, "UI Canary") == 0);
    assert(strcmp(public_row.text, "ui-data-canary legitimate-public") == 0);

    mock_reset();
    assert(d1l_dm_store_init() == ESP_OK);
    assert(d1l_dm_store_append(
               fingerprint, "UI Canary", "rx",
               "ui-data-canary legitimate-dm", -60, 40, 1U, 1U, 1U,
               true, false, 0x1234U) == ESP_OK);
    assert(blob_contains(&s_fallback[D1L_RETAINED_BLOB_STORE_DM_MESSAGES],
                         "ui-data-canary legitimate-dm"));
    assert(fallback_header(D1L_RETAINED_BLOB_STORE_DM_MESSAGES).count == 1U);
    assert(d1l_dm_store_init() == ESP_OK);
    d1l_dm_entry_t dm_row = {0};
    assert(d1l_dm_store_copy_recent(&dm_row, 1U) == 1U);
    assert(strcmp(dm_row.contact_alias, "UI Canary") == 0);
    assert(strcmp(dm_row.text, "ui-data-canary legitimate-dm") == 0);

    mock_reset();
    assert(d1l_packet_log_init() == ESP_OK);
    d1l_packet_log_entry_t packet = {
        .direction = "rx",
        .kind = "ui_canary",
        .note = "ui-canary legitimate-packet",
    };
    assert(d1l_packet_log_append_raw(
        &packet, (const uint8_t *)"ui-data-canary legitimate-packet",
        strlen("ui-data-canary legitimate-packet")));
    assert(blob_contains(&s_fallback[D1L_RETAINED_BLOB_STORE_PACKET_LOG],
                         "ui-canary legitimate-packet"));
    assert(fallback_header(D1L_RETAINED_BLOB_STORE_PACKET_LOG).count == 1U);
    assert(d1l_packet_log_init() == ESP_OK);
    d1l_packet_log_entry_t packet_row = {0};
    assert(d1l_packet_log_copy_recent(&packet_row, 1U) == 1U);
    assert(strcmp(packet_row.kind, "ui_canary") == 0);
    assert(strcmp(packet_row.note, "ui-canary legitimate-packet") == 0);
}

int main(void)
{
    test_message_volatile_preview_does_not_consume_history();
    test_dm_volatile_preview_does_not_consume_history();
    test_packet_volatile_preview_does_not_consume_history();
    test_packet_sd_query_orders_volatile_preview_after_history();
    test_packet_sd_query_uses_recent_ram_when_newest_segment_reads_fail();
    test_sentinel_shaped_durable_rows_survive_persistence();
    puts("native volatile retained-store isolation: ok");
    return 0;
}
