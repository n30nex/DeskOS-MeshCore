#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mesh/user_text.h"

#define D1L_MESSAGE_STORE_CAPACITY 16U
#define D1L_MESSAGE_AUTHOR_LEN 24U
#define D1L_MESSAGE_MAX_BYTES D1L_USER_TEXT_MAX_BYTES
/* Compatibility alias for existing fixed buffers/UI callers.  The protocol
 * limit is encoded UTF-8 bytes, not displayed characters. */
#define D1L_MESSAGE_MAX_CHARS D1L_MESSAGE_MAX_BYTES
#define D1L_MESSAGE_TEXT_LEN (D1L_MESSAGE_MAX_BYTES + 1U)
#define D1L_MESSAGE_STORE_PERSIST_RETRY_MS 5000U

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
    /* Explicit frozen-schema padding: channel_id starts at byte 192 on both
     * host and Xtensa ABIs instead of relying on uint64_t tail alignment. */
    uint8_t reserved[5];
    /* Stable application channel identity. The one-byte wire hash is not an
     * identity and must never be used to partition retained history. */
    uint64_t channel_id;
} d1l_message_entry_t;

typedef struct {
    uint32_t next_seq;
    uint32_t total_written;
    uint32_t dropped_oldest;
    uint32_t epoch;
    uint32_t content_revision;
    uint64_t clear_lineage;
    uint32_t persistence_commit_count;
    uint32_t persistence_fail_count;
    uint32_t persistence_stale_snapshot_count;
    uint32_t sd_primary_commit_count;
    uint32_t sd_primary_fail_count;
    uint32_t sd_backend_generation;
    esp_err_t sd_primary_last_error;
    uint32_t nvs_fallback_commit_count;
    uint32_t nvs_fallback_fail_count;
    esp_err_t nvs_fallback_last_error;
    uint64_t persistence_revision;
    size_t count;
    size_t public_count;
    size_t capacity;
    bool loaded;
    bool persistence_dirty;
    bool sd_primary_required;
    bool sd_primary_dirty;
    bool sd_primary_reconcile_pending;
    bool nvs_fallback_dirty;
} d1l_message_store_stats_t;

/* Captured under the same lock as retained rows. Channel reconciliation uses
 * the lineage and next sequence to distinguish ordinary ring eviction from a
 * durability rollback or a true sequence-generation reset. */
typedef struct {
    uint32_t epoch;
    uint32_t next_seq;
    uint64_t clear_lineage;
    uint64_t persistence_revision;
} d1l_message_retained_snapshot_t;

esp_err_t d1l_message_store_init(void);
esp_err_t d1l_message_store_clear(void);
esp_err_t d1l_message_store_clear_channel(uint64_t channel_id);
esp_err_t d1l_message_store_flush(void);
esp_err_t d1l_message_store_flush_if_due(void);
esp_err_t d1l_message_store_append_public(const char *direction, const char *author,
                                          const char *text, int rssi_dbm, int snr_tenths,
                                          uint8_t path_hash_bytes, uint8_t path_hops,
                                          bool delivered);
esp_err_t d1l_message_store_append_public_volatile(const char *direction, const char *author,
                                                   const char *text, int rssi_dbm, int snr_tenths,
                                                   uint8_t path_hash_bytes, uint8_t path_hops,
                                                   bool delivered);
/* A nonzero out_seq means the row was admitted to the visible retained ring.
 * It can therefore be used to advance the matching channel cursor even when
 * the returned status reports that one persistence mirror still needs retry.
 * Validation/init/capacity failures leave out_seq at zero. */
esp_err_t d1l_message_store_append_channel(
    uint64_t channel_id, const char *direction, const char *author,
    const char *text, int rssi_dbm, int snr_tenths,
    uint8_t path_hash_bytes, uint8_t path_hops, bool delivered,
    uint32_t *out_seq);
esp_err_t d1l_message_store_append_channel_volatile(
    uint64_t channel_id, const char *direction, const char *author,
    const char *text, int rssi_dbm, int snr_tenths,
    uint8_t path_hash_bytes, uint8_t path_hops, bool delivered);
d1l_message_store_stats_t d1l_message_store_stats(void);
/* Coherent secret-free snapshot of every durable channel row. max_entries
 * must fit the complete bounded ring so callers cannot reconcile from a
 * partial view. Volatile UI canaries are intentionally excluded. */
esp_err_t d1l_message_store_snapshot_retained(
    d1l_message_entry_t *out_entries, size_t max_entries, size_t *out_count,
    d1l_message_retained_snapshot_t *out_snapshot);
/* Compatibility Public APIs are deliberately exact-channel filtered. Private
 * rows can never enter the existing Public UI or read-state cursor. */
size_t d1l_message_store_copy_recent(d1l_message_entry_t *out_entries, size_t max_entries);
size_t d1l_message_store_query_page(d1l_message_entry_t *out_entries, size_t max_entries,
                                    size_t skip_newest, const char *query,
                                    size_t *out_total_matches);
/* Copies one Public page, its retained stats, and any matching volatile
 * preview identity under one store lock. This keeps row classification
 * coherent with the counters emitted by release evidence commands. */
size_t d1l_message_store_query_page_snapshot(
    d1l_message_entry_t *out_entries, size_t max_entries,
    size_t skip_newest, const char *query, size_t *out_total_matches,
    d1l_message_store_stats_t *out_stats, uint32_t *out_volatile_seq);
size_t d1l_message_store_query(d1l_message_entry_t *out_entries, size_t max_entries,
                               const char *query);
size_t d1l_message_store_copy_channel_recent(
    uint64_t channel_id, d1l_message_entry_t *out_entries,
    size_t max_entries);
size_t d1l_message_store_query_channel_page(
    uint64_t channel_id, d1l_message_entry_t *out_entries,
    size_t max_entries, size_t skip_newest, const char *query,
    size_t *out_total_matches);
size_t d1l_message_store_query_channel(
    uint64_t channel_id, d1l_message_entry_t *out_entries,
    size_t max_entries, const char *query);
