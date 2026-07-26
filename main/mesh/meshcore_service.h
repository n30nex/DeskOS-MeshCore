#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mesh/meshcore_admin_runtime.h"
#include "mesh/meshcore_discovery.h"
#include "mesh/meshcore_trace.h"

typedef enum {
    D1L_MESHCORE_SERVICE_INITIALIZING = 0,
    D1L_MESHCORE_SERVICE_WAITING_FOR_RADIO,
    D1L_MESHCORE_SERVICE_READY,
    D1L_MESHCORE_SERVICE_TX_BUSY,
    D1L_MESHCORE_SERVICE_RADIO_ERROR,
} d1l_meshcore_service_state_t;

typedef struct {
    d1l_meshcore_service_state_t state;
    uint32_t rx_packets;
    uint32_t rx_adverts;
    uint32_t tx_packets;
    uint32_t channel_rx_unknown_hash;
    uint32_t channel_rx_hash_collision;
    uint32_t channel_rx_decrypt_failed;
    uint32_t channel_rx_reconcile_blocked;
    uint32_t rejected_commands;
    uint32_t ack_tx_queued;
    uint32_t ack_tx_done;
    uint32_t ack_tx_failed;
    uint32_t ack_tx_duplicate_rows_suppressed;
    uint32_t ack_tx_last_hash;
    esp_err_t ack_tx_last_error;
    uint32_t dm_route_direct_selected;
    uint32_t dm_route_flood_selected;
    uint32_t dm_route_missing_fallback;
    uint32_t dm_route_preboot_fallback;
    uint32_t dm_route_stale_fallback;
    uint32_t dm_route_malformed_fallback;
    uint32_t dm_route_expired_fallback;
    uint32_t dm_route_failed_fallback;
    uint32_t dm_route_direct_retry_fallback;
    uint32_t dm_route_last_path_age_ms;
    uint8_t dm_route_last_reason;
    uint64_t dm_delivery_session_id;
    uint32_t dm_delivery_revision;
    uint8_t dm_delivery_state;
    esp_err_t dm_delivery_last_error;
    uint32_t trace_tx_queued;
    uint32_t trace_rx_matched;
    uint32_t trace_rx_duplicates;
    uint32_t trace_pending_expired;
    uint32_t trace_rx_expired;
    uint32_t trace_rx_unmatched;
    uint32_t trace_rx_auth_mismatch;
    uint32_t trace_rx_path_mismatch;
    uint32_t trace_rx_malformed;
    uint32_t trace_rx_source_ignored;
    uint32_t trace_rx_in_flight_ignored;
    uint32_t trace_rx_unsupported;
    uint32_t runtime_command_queue_depth;
    uint32_t runtime_command_queue_high_water;
    uint32_t runtime_priority_queue_depth;
    uint32_t runtime_priority_queue_high_water;
    uint32_t runtime_event_queue_depth;
    uint32_t runtime_event_queue_high_water;
    uint32_t runtime_queue_drops;
    uint32_t runtime_callback_event_drops;
    uint32_t runtime_command_queue_saturation;
    uint32_t runtime_priority_queue_saturation;
    uint32_t runtime_fairness_forced_commands;
    uint32_t runtime_priority_burst_high_water;
    uint8_t runtime_priority_burst_bound;
    uint32_t runtime_owner_maintenance_runs;
    uint32_t runtime_terminal_recovery_dispatches;
    uint32_t runtime_task_heartbeat;
    uint32_t runtime_task_stack_free_words;
    uint64_t runtime_last_event_monotonic_us;
    uint8_t path_hash_bytes;
    bool identity_ready;
    bool radio_ready;
    bool radio_applied;
    bool radio_apply_pending;
    esp_err_t radio_apply_error;
    bool companion_framing_ready;
} d1l_meshcore_service_status_t;

typedef struct {
    bool pending;
    bool pending_expired;
    uint32_t pending_tag;
    uint32_t pending_age_ms;
    uint8_t pending_path_hash_bytes;
    uint8_t pending_path_hops;
    uint8_t pending_path_hashes[D1L_MESHCORE_TRACE_MAX_PATH_BYTES];
    bool last_attempt_valid;
    d1l_meshcore_trace_outcome_t last_attempt_outcome;
    uint32_t last_attempt_tag;
    uint32_t last_attempt_age_ms;
    bool cooldown_active;
    uint32_t cooldown_remaining_ms;
    bool last_result_valid;
    uint32_t last_tag;
    uint32_t last_age_ms;
    uint32_t last_round_trip_ms;
    char target_fingerprint[17];
    bool zero_hop_ping;
    uint8_t last_path_hash_bytes;
    uint8_t last_path_hops;
    uint8_t last_path_hashes[D1L_MESHCORE_TRACE_MAX_PATH_BYTES];
    int8_t last_path_snrs_quarter_db[D1L_MESHCORE_TRACE_MAX_HOPS];
    int last_rssi_dbm;
    int last_radio_snr_quarter_db;
    bool last_retention_attempted;
    bool last_route_summary_accepted;
    bool last_packet_preview_retained;
} d1l_meshcore_trace_snapshot_t;

typedef struct {
    uint8_t node_type;
    char fingerprint[17];
    char public_key_hex[65];
    int last_rssi_dbm;
    int local_snr_quarter_db;
    int remote_snr_quarter_db;
    uint32_t age_ms;
} d1l_meshcore_discovery_result_t;

typedef struct {
    uint32_t generation;
    bool active;
    uint32_t tag;
    uint32_t remaining_ms;
    uint32_t total_responses;
    size_t result_count;
    d1l_meshcore_discovery_result_t
        results[D1L_MESHCORE_DISCOVERY_MAX_RESULTS];
} d1l_meshcore_discovery_snapshot_t;

typedef d1l_meshcore_admin_runtime_snapshot_t d1l_meshcore_admin_snapshot_t;

#define D1L_MESHCORE_CONTACT_TELEMETRY_HISTORY_CAPACITY 4U

typedef enum {
    D1L_MESHCORE_CONTACT_TELEMETRY_IDLE = 0,
    D1L_MESHCORE_CONTACT_TELEMETRY_PENDING,
    D1L_MESHCORE_CONTACT_TELEMETRY_RECEIVED,
    D1L_MESHCORE_CONTACT_TELEMETRY_EXPIRED,
    D1L_MESHCORE_CONTACT_TELEMETRY_MALFORMED,
    D1L_MESHCORE_CONTACT_TELEMETRY_REQUEST_FAILED,
} d1l_meshcore_contact_telemetry_state_t;

typedef struct {
    uint32_t sequence;
    uint32_t tag;
    uint32_t age_ms;
    int last_rssi_dbm;
    int last_snr_tenths;
    uint8_t path_hops;
    d1l_meshcore_admin_query_result_t result;
} d1l_meshcore_contact_telemetry_entry_t;

typedef struct {
    uint32_t generation;
    uint32_t total_received;
    d1l_meshcore_contact_telemetry_state_t state;
    bool pending;
    uint32_t pending_tag;
    uint32_t pending_remaining_ms;
    size_t history_count;
    d1l_meshcore_contact_telemetry_entry_t
        history[D1L_MESHCORE_CONTACT_TELEMETRY_HISTORY_CAPACITY];
} d1l_meshcore_contact_telemetry_snapshot_t;

void d1l_meshcore_service_init(void);
esp_err_t d1l_meshcore_service_start_rx_async(void);
esp_err_t d1l_meshcore_service_ensure_identity(void);
d1l_meshcore_service_status_t d1l_meshcore_service_status(void);
void d1l_meshcore_service_trace_snapshot(d1l_meshcore_trace_snapshot_t *out_snapshot);
void d1l_meshcore_service_discovery_snapshot(
    d1l_meshcore_discovery_snapshot_t *out_snapshot);
void d1l_meshcore_service_admin_snapshot(
    d1l_meshcore_admin_snapshot_t *out_snapshot);
void d1l_meshcore_service_contact_telemetry_snapshot(
    const char *fingerprint,
    d1l_meshcore_contact_telemetry_snapshot_t *out_snapshot);
const char *d1l_meshcore_contact_telemetry_state_name(
    d1l_meshcore_contact_telemetry_state_t state);
/* Synchronous, bounded owner-task commands. ESP_ERR_TIMEOUT reports request
 * slot or command-queue saturation; no Admin/session side effect is admitted
 * after the caller deadline. The login credential is copied only into the
 * exact guarded request slot and is never retained in the command queue. */
esp_err_t d1l_meshcore_service_admin_login(const char *fingerprint,
                                           const char *password);
esp_err_t d1l_meshcore_service_admin_request_status(void);
esp_err_t d1l_meshcore_service_admin_request_query(
    d1l_meshcore_admin_query_t query, uint16_t offset);
esp_err_t d1l_meshcore_service_admin_request_mutation(
    d1l_meshcore_admin_mutation_t mutation, bool local_confirmed);
esp_err_t d1l_meshcore_service_admin_request_cli(
    const char *command, bool local_confirmed);
esp_err_t d1l_meshcore_service_admin_send_room_post(const char *text);
esp_err_t d1l_meshcore_service_admin_logout(void);
esp_err_t d1l_meshcore_service_request_advert(bool flood);
esp_err_t d1l_meshcore_service_send_channel(uint64_t channel_id,
                                            const char *text);
esp_err_t d1l_meshcore_service_send_active_channel(const char *text);
esp_err_t d1l_meshcore_service_send_public(const char *text);
esp_err_t d1l_meshcore_service_send_dm(const char *fingerprint, const char *text);
esp_err_t d1l_meshcore_service_request_path_discovery_probe(
    const char *fingerprint,
    char *out_token,
    size_t out_token_size);
esp_err_t d1l_meshcore_service_reset_contact_route(
    const char *fingerprint);
esp_err_t d1l_meshcore_service_discover_nearby(void);
void d1l_meshcore_service_clear_discovery_results(void);
/* ESP_ERR_NOT_FINISHED means one TRACE is pending; ESP_ERR_NOT_ALLOWED means
 * the bounded post-outcome cooldown is active. */
esp_err_t d1l_meshcore_service_send_trace_contact(const char *fingerprint);
esp_err_t d1l_meshcore_service_ping_repeater(const char *fingerprint);
const char *d1l_meshcore_service_state_name(d1l_meshcore_service_state_t state);
