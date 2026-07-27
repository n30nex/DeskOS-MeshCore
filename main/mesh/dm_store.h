#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "mesh/contact_store.h"
#include "mesh/dm_delivery_state.h"
#include "mesh/message_store.h"

#define D1L_DM_STORE_CAPACITY 16U
#define D1L_DM_DIRECTION_LEN 4U
#define D1L_DM_CONTACT_ALIAS_LEN 32U
#define D1L_DM_STORE_PERSIST_RETRY_INTERVAL_MS 5000U
#define D1L_DM_IDENTITY_DIGEST_BYTES 32U
#define D1L_DM_ACK_DISPATCH_MAX 2U
#define D1L_DM_ACK_DISPATCH_KIND_MAX 3U
#define D1L_DM_ACK_INTERRUPTED_ERROR ESP_ERR_INVALID_STATE
#define D1L_DM_DELIVERY_INTERRUPTED_ERROR ESP_ERR_INVALID_STATE

typedef enum {
    D1L_DM_ACK_STATE_LEGACY_UNVERIFIED = 0,
    D1L_DM_ACK_STATE_PENDING,
    D1L_DM_ACK_STATE_SENT,
    D1L_DM_ACK_STATE_RETRYABLE,
    D1L_DM_ACK_STATE_TERMINAL,
} d1l_dm_ack_state_t;

typedef struct {
    uint32_t seq;
    uint64_t delivery_session_id;
    uint32_t uptime_ms;
    char contact_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char contact_alias[D1L_DM_CONTACT_ALIAS_LEN];
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
} d1l_dm_entry_t;

typedef struct {
    bool inserted;
    bool durable;
    uint32_t row_seq;
    uint64_t delivery_session_id;
    esp_err_t error;
} d1l_dm_store_append_outcome_t;

typedef struct {
    bool reserved;
    bool durable;
    uint32_t row_seq;
    uint8_t dispatch_count;
    esp_err_t error;
} d1l_dm_ack_reservation_t;

typedef struct {
    bool changed;
    bool durable;
    bool persistence_retry;
    uint64_t delivery_session_id;
    uint32_t row_seq;
    d1l_dm_delivery_state_t previous_state;
    d1l_dm_delivery_state_t current_state;
    uint8_t retry_count;
    uint8_t attempt;
    uint32_t ack_hash;
    uint32_t delivery_revision;
    esp_err_t error;
} d1l_dm_delivery_transition_outcome_t;

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
    size_t capacity;
    bool loaded;
    bool persistence_dirty;
    bool sd_primary_required;
    bool sd_primary_dirty;
    bool sd_primary_reconcile_pending;
    bool nvs_fallback_dirty;
} d1l_dm_store_stats_t;

esp_err_t d1l_dm_store_init(void);
esp_err_t d1l_dm_store_clear(void);
esp_err_t d1l_dm_store_flush(void);
esp_err_t d1l_dm_store_flush_if_due(void);
esp_err_t d1l_dm_store_append(const char *contact_fingerprint, const char *contact_alias,
                              const char *direction, const char *text, int rssi_dbm,
                              int snr_tenths, uint8_t path_hash_bytes, uint8_t path_hops,
                              uint8_t attempt, bool delivered, bool acked,
                              uint32_t ack_hash);
esp_err_t d1l_dm_store_append_tx(
    const char *contact_fingerprint, const char *contact_alias,
    const char *text, int rssi_dbm, int snr_tenths,
    uint8_t path_hash_bytes, uint8_t path_hops, uint8_t attempt,
    uint32_t ack_hash, d1l_dm_store_append_outcome_t *outcome);
esp_err_t d1l_dm_store_append_rx_identity(
    const char *contact_fingerprint, const char *contact_alias,
    const char *text, int rssi_dbm, int snr_tenths,
    uint8_t path_hash_bytes, uint8_t path_hops, uint8_t attempt,
    uint32_t ack_hash,
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    d1l_dm_store_append_outcome_t *outcome);
/* Retains an inbound row in the bounded ring and marks both backends dirty
 * without performing storage I/O in the MeshCore radio owner. */
esp_err_t d1l_dm_store_append_rx_identity_deferred(
    const char *contact_fingerprint, const char *contact_alias,
    const char *text, int rssi_dbm, int snr_tenths,
    uint8_t path_hash_bytes, uint8_t path_hops, uint8_t attempt,
    uint32_t ack_hash,
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    d1l_dm_store_append_outcome_t *outcome);
esp_err_t d1l_dm_store_append_volatile(const char *contact_fingerprint, const char *contact_alias,
                                       const char *direction, const char *text, int rssi_dbm,
                                       int snr_tenths, uint8_t path_hash_bytes, uint8_t path_hops,
                                       uint8_t attempt, bool delivered, bool acked,
                                       uint32_t ack_hash);
esp_err_t d1l_dm_store_mark_acked(uint32_t ack_hash, d1l_dm_entry_t *out_entry);
bool d1l_dm_store_find_rx_identity(
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    d1l_dm_entry_t *out_entry);
/* Read only from an already-loaded ring. This never attempts backend
 * initialization and is safe on the radio owner's RX-to-ACK path. */
bool d1l_dm_store_find_rx_identity_loaded(
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    d1l_dm_entry_t *out_entry);
bool d1l_dm_store_find_delivery_session(
    uint64_t delivery_session_id, d1l_dm_entry_t *out_entry);
esp_err_t d1l_dm_store_reserve_ack_dispatch(
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    uint8_t dispatch_kind, d1l_dm_ack_reservation_t *reservation);
esp_err_t d1l_dm_store_reserve_ack_dispatch_deferred(
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    uint8_t dispatch_kind, d1l_dm_ack_reservation_t *reservation);
esp_err_t d1l_dm_store_rebind_pending_ack_dispatch(
    uint32_t row_seq,
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    uint8_t dispatch_kind);
esp_err_t d1l_dm_store_rebind_pending_ack_dispatch_deferred(
    uint32_t row_seq,
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    uint8_t dispatch_kind);
esp_err_t d1l_dm_store_complete_ack_dispatch(
    uint32_t row_seq,
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    bool sent, esp_err_t error);
esp_err_t d1l_dm_store_complete_ack_dispatch_deferred(
    uint32_t row_seq,
    const uint8_t identity_digest[D1L_DM_IDENTITY_DIGEST_BYTES],
    bool sent, esp_err_t error);
esp_err_t d1l_dm_store_transition_delivery(
    uint64_t delivery_session_id,
    d1l_dm_delivery_state_t expected_state,
    uint32_t expected_delivery_revision,
    d1l_dm_delivery_state_t next_state,
    d1l_dm_delivery_reason_t reason, esp_err_t error,
    d1l_dm_delivery_transition_outcome_t *outcome);
/* Admits only the exact AWAITING_ACK -> ACKNOWLEDGED CAS in loaded RAM.
 * The retained worker owns persistence; public delivery truth remains masked
 * until that exact next revision is durable. */
esp_err_t d1l_dm_store_transition_delivery_deferred(
    uint64_t delivery_session_id,
    d1l_dm_delivery_state_t expected_state,
    uint32_t expected_delivery_revision,
    d1l_dm_delivery_state_t next_state,
    d1l_dm_delivery_reason_t reason, esp_err_t error,
    d1l_dm_delivery_transition_outcome_t *outcome);
/* Atomically advances RETRY_WAIT -> RETRY_TX and binds the ACK identity of
 * the rebuilt attempt to the same retained delivery session. */
esp_err_t d1l_dm_store_transition_delivery_retry(
    uint64_t delivery_session_id, uint32_t expected_delivery_revision,
    uint32_t retry_ack_hash,
    d1l_dm_delivery_transition_outcome_t *outcome);
const char *d1l_dm_ack_state_name(d1l_dm_ack_state_t state);
const char *d1l_dm_ack_dispatch_kind_name(uint8_t dispatch_kind);
d1l_dm_store_stats_t d1l_dm_store_stats(void);
size_t d1l_dm_store_copy_recent_page(d1l_dm_entry_t *out_entries, size_t max_entries,
                                     size_t skip_newest, size_t *out_total_matches);
/* Copies one DM page, its retained stats, and any matching volatile preview
 * identity under one store lock. */
size_t d1l_dm_store_copy_recent_page_snapshot(
    d1l_dm_entry_t *out_entries, size_t max_entries, size_t skip_newest,
    size_t *out_total_matches, d1l_dm_store_stats_t *out_stats,
    uint32_t *out_volatile_seq);
size_t d1l_dm_store_copy_recent(d1l_dm_entry_t *out_entries, size_t max_entries);
/* Searches one exact-fingerprint conversation across the bounded durable ring
 * and its optional volatile tail.  Paging skips newest matching rows, while
 * each returned page remains oldest-to-newest for conversation rendering.
 * ASCII letters compare case-insensitively; every non-ASCII byte must match
 * exactly.  A NULL or empty query is equivalent to copy_thread_page(). */
size_t d1l_dm_store_query_thread_page(const char *contact_fingerprint,
                                      d1l_dm_entry_t *out_entries,
                                      size_t max_entries, size_t skip_newest,
                                      const char *query,
                                      size_t *out_total_matches);
size_t d1l_dm_store_copy_thread_page_snapshot(
    const char *contact_fingerprint, d1l_dm_entry_t *out_entries,
    size_t max_entries, size_t skip_newest, size_t *out_total_matches,
    d1l_dm_store_stats_t *out_stats, uint32_t *out_volatile_seq);
size_t d1l_dm_store_copy_thread_page(const char *contact_fingerprint,
                                     d1l_dm_entry_t *out_entries,
                                     size_t max_entries, size_t skip_newest,
                                     size_t *out_total_matches);
size_t d1l_dm_store_copy_thread(const char *contact_fingerprint, d1l_dm_entry_t *out_entries,
                                size_t max_entries);
