#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "app/settings_model.h"
#include "app/release_profile.h"
#include "comms/connectivity_manager.h"
#include "map/map_marker_truth.h"
#include "mesh/channel_store.h"
#include "mesh/contact_store.h"
#include "mesh/dm_store.h"
#include "mesh/message_store.h"
#include "mesh/mesh_inspector.h"
#include "mesh/meshcore_service.h"
#include "mesh/node_store.h"
#include "mesh/packet_log.h"
#include "mesh/route_store.h"

#define D1L_APP_SNAPSHOT_PACKET_PREVIEW 4U
#define D1L_APP_SNAPSHOT_CHANNEL_PREVIEW D1L_CHANNEL_STORE_CAPACITY
#define D1L_APP_SNAPSHOT_MESSAGE_PREVIEW 5U
#define D1L_APP_SNAPSHOT_DM_PREVIEW 5U
#define D1L_APP_SNAPSHOT_NODE_PREVIEW 4U
#define D1L_APP_SNAPSHOT_CONTACT_PREVIEW 2U
#define D1L_APP_SNAPSHOT_ROUTE_PREVIEW 2U
#define D1L_APP_SNAPSHOT_ROOM_PREVIEW D1L_ROOM_SERVER_PREVIEW_CAPACITY
#define D1L_APP_SNAPSHOT_REPEATER_PREVIEW D1L_REPEATER_PREVIEW_CAPACITY
#define D1L_HOME_MESSAGE_PREVIEW 5U
#define D1L_HOME_REPEATER_PREVIEW 3U
#define D1L_MAP_TILE_RENDER_SUPPORTED true
#define D1L_BLE_COMPANION_TRANSPORT_SUPPORTED true

typedef struct {
    bool is_dm;
    bool unread;
    char sender[D1L_MESSAGE_AUTHOR_LEN];
    char target_fingerprint[D1L_NODE_FINGERPRINT_LEN];
    char text[D1L_MESSAGE_TEXT_LEN];
    char direction[4];
    char status[16];
    uint32_t age_sec;
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hops;
} d1l_home_message_preview_t;

typedef struct {
    char label[D1L_ROUTE_LABEL_LEN];
    char route[D1L_ROUTE_NAME_LEN];
    char kind[D1L_ROUTE_KIND_LEN];
    uint32_t age_sec;
    int rssi_dbm;
    int snr_tenths;
    uint8_t path_hops;
} d1l_home_repeater_preview_t;

typedef struct {
    bool board_ready;
    bool ui_ready;
    esp_err_t board_error;
    uint32_t boot_count;
} d1l_app_model_t;

typedef struct {
    uint32_t frequency_hz;
    uint16_t bandwidth_tenths_khz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    bool rx_boost;
} d1l_app_radio_profile_edit_t;

typedef struct {
    bool board_ready;
    bool ui_ready;
    d1l_release_profile_id_t release_profile_id;
    const char *release_profile;
    d1l_sd_history_mode_t sd_history_mode_id;
    const char *sd_history_mode;
    d1l_release_capabilities_t release_capabilities;
    bool identity_ready;
    bool radio_ready;
    bool radio_applied;
    bool radio_apply_pending;
    bool companion_ready;
    bool wifi_enabled;
    bool ble_companion_enabled;
    bool observer_enabled;
    bool wifi_profile_saved;
    bool wifi_password_saved;
    uint8_t wifi_profile_count;
    uint8_t wifi_active_profile;
    d1l_wifi_profile_info_t
        wifi_profiles[D1L_WIFI_PROFILE_CAPACITY];
    bool wifi_scan_supported;
    bool wifi_stack_active;
    bool wifi_connected;
    bool wifi_connecting;
    bool onboarding_complete;
    bool wifi_build_enabled;
    bool ble_build_enabled;
    bool ble_transport_supported;
    bool ble_peer_known;
    bool ble_protocol_running;
    bool ble_protocol_ready;
    bool storage_direct_supported;
    bool storage_rp2040_bridge_required;
    bool storage_rp2040_bridge_ready;
    bool storage_rp2040_sd_protocol_supported;
    bool storage_sd_present;
    bool storage_sd_mounted;
    bool storage_sd_data_root_ready;
    bool storage_sd_needs_fat32;
    bool storage_setup_required;
    bool storage_setup_supported;
    bool storage_data_enabled;
    bool storage_response_truncated;
    bool storage_retained_sd_degraded;
    bool storage_retained_backup_degraded;
    bool message_store_loaded;
    bool dm_store_loaded;
    bool message_store_persistence_degraded;
    bool dm_store_persistence_degraded;
    bool time_available;
    bool time_approximate;
    bool timezone_settings_ready;
    bool high_contrast;
    bool night_mode;
    uint8_t display_brightness_percent;
    uint16_t display_timeout_seconds;
    uint8_t notification_mode;
    bool protocol_tx_ready;
    esp_err_t protocol_tx_error;
    esp_err_t settings_load_status;
    d1l_identity_state_t identity_state;
    bool map_page_supported;
    bool map_tile_cache_ready;
    bool map_tile_download_supported;
    bool map_tile_render_supported;
    bool map_tile_sideload_supported;
    bool map_tile_provider_configured;
    bool map_location_set;
    d1l_map_center_source_t map_center_source;
    bool map_marker_age_reference_valid;
    uint32_t map_marker_reference_timestamp;
    int32_t map_lat_e7;
    int32_t map_lon_e7;
    uint8_t map_tile_zoom;
    uint32_t storage_capacity_kb;
    uint32_t storage_free_kb;
    const char *wifi_state;
    const char *ble_state;
    const char *coexistence_policy;
    const char *storage_sd_state;
    const char *storage_sd_interface;
    const char *storage_sd_filesystem;
    const char *storage_mount_point;
    const char *storage_data_root;
    const char *storage_backend;
    const char *message_store_backend;
    const char *dm_store_backend;
    const char *packet_log_backend;
    const char *route_store_backend;
    const char *node_store_backend;
    const char *contact_store_backend;
    const char *read_state_backend;
    const char *map_tile_backend;
    const char *export_backend;
    const char *map_tile_cache_policy;
    const char *map_tile_cache_path_template;
    const char *map_tile_download_state;
    const char *map_tile_download_requires;
    const char *map_tile_provider_policy;
    const char *map_tile_provider_attribution;
    const char *storage_setup_action;
    const char *storage_note;
    esp_err_t storage_last_error;
    char time_label[8];
    char timezone_label[D1L_TIMEZONE_LABEL_LEN];
    int16_t timezone_offset_minutes;
    char node_name[32];
    char wifi_ssid[D1L_WIFI_SSID_LEN];
    char wifi_ip[16];
    char identity_fingerprint[17];
    const char *reset_reason;
    const char *mesh_state;
    const char *wifi_last_error;
    int8_t wifi_rssi_dbm;
    uint8_t wifi_channel;
    uint32_t ble_pairing_passkey;
    uint32_t ble_protocol_command_count;
    uint32_t ble_protocol_response_count;
    uint32_t ble_protocol_unsupported_count;
    uint32_t ble_protocol_malformed_count;
    uint32_t radio_frequency_hz;
    uint16_t radio_bandwidth_tenths_khz;
    uint8_t radio_spreading_factor;
    uint8_t radio_coding_rate;
    int8_t radio_tx_power_dbm;
    bool radio_rx_boost;
    const char *radio_tcxo;
    const char *radio_apply_error;
    uint32_t uptime_ms;
    uint32_t heap_free;
    uint32_t heap_min_free;
    uint32_t heap_largest_free;
    uint32_t psram_free;
    uint32_t psram_min_free;
    uint32_t psram_largest_free;
    uint32_t current_task_stack_free_words;
    uint32_t ui_task_stack_free_words;
    uint32_t lvgl_free_bytes;
    uint32_t lvgl_largest_free_bytes;
    uint8_t lvgl_used_pct;
    uint32_t rx_packets;
    uint32_t rx_adverts;
    uint32_t tx_packets;
    uint32_t rejected_commands;
    uint32_t message_total_written;
    size_t message_count;
    uint32_t dm_total_written;
    uint32_t dm_content_revision;
    size_t dm_count;
    size_t dm_conversation_count;
    uint8_t dm_delivery_state;
    bool dm_delivery_active;
    bool dm_retry_active;
    bool dm_failure_latched;
    uint32_t public_unread_count;
    uint32_t dm_unread_count;
    uint32_t muted_dm_unread_count;
    uint32_t last_public_read_seq;
    uint32_t last_dm_read_seq;
    uint32_t read_state_mark_count;
    uint32_t node_total_written;
    size_t node_count;
    uint32_t contact_total_written;
    size_t contact_count;
    size_t dm_capable_contact_count;
    uint32_t route_total_written;
    size_t route_count;
    uint32_t packet_total_written;
    size_t packet_count;
    uint32_t channel_store_revision;
    esp_err_t channel_store_load_status;
    bool channel_store_loaded;
    bool channel_message_reconcile_pending;
    uint64_t active_channel_id;
    d1l_channel_info_t channels[D1L_APP_SNAPSHOT_CHANNEL_PREVIEW];
    size_t channel_count;
    d1l_mesh_signal_summary_t signal_summary;
    d1l_mesh_room_server_t recent_rooms[D1L_APP_SNAPSHOT_ROOM_PREVIEW];
    size_t recent_room_count;
    d1l_mesh_repeater_candidate_t recent_repeaters[D1L_APP_SNAPSHOT_REPEATER_PREVIEW];
    size_t recent_repeater_count;
    uint8_t path_hash_bytes;
    d1l_contact_entry_t recent_contacts[D1L_APP_SNAPSHOT_CONTACT_PREVIEW];
    size_t recent_contact_count;
    d1l_route_entry_t recent_routes[D1L_APP_SNAPSHOT_ROUTE_PREVIEW];
    size_t recent_route_count;
    d1l_node_view_t recent_nodes[D1L_APP_SNAPSHOT_NODE_PREVIEW];
    size_t recent_node_count;
    d1l_message_entry_t recent_messages[D1L_APP_SNAPSHOT_MESSAGE_PREVIEW];
    size_t recent_message_count;
    d1l_dm_entry_t recent_dms[D1L_APP_SNAPSHOT_DM_PREVIEW];
    bool recent_dm_unread[D1L_APP_SNAPSHOT_DM_PREVIEW];
    uint32_t recent_dm_unread_count[D1L_APP_SNAPSHOT_DM_PREVIEW];
    bool recent_dm_muted[D1L_APP_SNAPSHOT_DM_PREVIEW];
    size_t recent_dm_count;
    d1l_packet_log_entry_t recent_packets[D1L_APP_SNAPSHOT_PACKET_PREVIEW];
    size_t recent_packet_count;
    d1l_home_message_preview_t home_messages[D1L_HOME_MESSAGE_PREVIEW];
    size_t home_message_count;
    d1l_home_repeater_preview_t home_repeaters[D1L_HOME_REPEATER_PREVIEW];
    size_t home_repeater_count;
} d1l_app_snapshot_t;

d1l_app_model_t *d1l_app_model_get(void);
void d1l_app_model_snapshot(d1l_app_snapshot_t *snapshot);
esp_err_t d1l_app_model_send_public_text(const char *text);
esp_err_t d1l_app_model_send_channel_text(uint64_t channel_id,
                                          const char *text);
esp_err_t d1l_app_model_send_active_channel_text(const char *text);
esp_err_t d1l_app_model_copy_channels(d1l_channel_info_t *out_channels,
                                      size_t max_channels,
                                      size_t *out_count,
                                      uint64_t *out_active_channel_id,
                                      d1l_channel_store_stats_t *out_stats);
esp_err_t d1l_app_model_select_channel(uint64_t channel_id,
                                       d1l_channel_info_t *out_channel);
/* Channel-management reads return only d1l_channel_info_t, which is redacted.
 * Secret-bearing inputs are passed synchronously to the canonical retained
 * store; this boundary never copies them into normal app state or logs. The
 * caller remains responsible for clearing its input buffer after the call. */
esp_err_t d1l_app_model_add_channel(
    const char *name, const uint8_t *secret, uint8_t secret_len,
    bool enabled, bool make_default,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_channel);
/* Creates an enabled shareable channel with a fresh 128-bit device-generated
 * secret. Secret bytes are securely cleared after the retained-store call. */
esp_err_t d1l_app_model_create_channel(
    const char *name, bool make_default,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_channel);
esp_err_t d1l_app_model_import_channel_uri(
    const char *uri, size_t uri_len,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_channel);
esp_err_t d1l_app_model_update_channel(
    uint64_t channel_id, const char *name, bool enabled, bool make_default,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_channel);
/* confirmed must be true. This makes the destructive confirmation decision an
 * explicit input at the application boundary instead of a UI-only convention. */
esp_err_t d1l_app_model_remove_channel(
    uint64_t channel_id, bool confirmed,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_channel);
/* This explicit export is the only app-model operation that returns channel
 * secret material. The caller must never log the URI and must call the clear
 * helper as soon as the one-shot share/QR view closes. The destination is
 * securely cleared before use and again on error. */
esp_err_t d1l_app_model_export_channel_share_uri(
    uint64_t channel_id, char *dest, size_t dest_size);
void d1l_app_model_clear_channel_share_uri(char *dest, size_t dest_size);
size_t d1l_app_model_query_public_messages_page(d1l_message_entry_t *out_entries,
                                                size_t max_entries,
                                                size_t skip_newest,
                                                const char *query,
                                                size_t *out_total_matches);
size_t d1l_app_model_query_public_messages(d1l_message_entry_t *out_entries,
                                           size_t max_entries, const char *query);
size_t d1l_app_model_query_channel_messages_page(
    uint64_t channel_id, d1l_message_entry_t *out_entries,
    size_t max_entries, size_t skip_newest, const char *query,
    size_t *out_total_matches);
esp_err_t d1l_app_model_send_dm_text(const char *fingerprint, const char *text);
esp_err_t d1l_app_model_request_path_discovery_probe(const char *fingerprint,
                                                     char *out_token,
                                                     size_t out_token_size);
void d1l_app_model_contact_telemetry_snapshot(
    const char *fingerprint,
    d1l_meshcore_contact_telemetry_snapshot_t *out_snapshot);
esp_err_t d1l_app_model_reset_contact_route(const char *fingerprint);
esp_err_t d1l_app_model_send_trace_contact(const char *fingerprint);
void d1l_app_model_trace_snapshot(
    d1l_meshcore_trace_snapshot_t *out_snapshot);
esp_err_t d1l_app_model_ping_repeater(const char *fingerprint);
esp_err_t d1l_app_model_discover_nearby(void);
void d1l_app_model_discovery_snapshot(
    d1l_meshcore_discovery_snapshot_t *out_snapshot);
esp_err_t d1l_app_model_clear_nodes(bool confirmed);
size_t d1l_app_model_query_dm_thread_page(const char *fingerprint,
                                          d1l_dm_entry_t *out_entries,
                                          bool *out_unread,
                                          size_t max_entries,
                                          size_t skip_newest,
                                          const char *query,
                                          size_t *out_total_matches);
size_t d1l_app_model_copy_dm_thread_page(const char *fingerprint,
                                         d1l_dm_entry_t *out_entries,
                                         bool *out_unread, size_t max_entries,
                                         size_t skip_newest,
                                         size_t *out_total_matches);
size_t d1l_app_model_copy_dm_thread(const char *fingerprint, d1l_dm_entry_t *out_entries,
                                    bool *out_unread, size_t max_entries);
esp_err_t d1l_app_model_find_contact(const char *fingerprint, d1l_contact_entry_t *out_contact);
esp_err_t d1l_app_model_find_contact_by_public_key(
    const char *public_key_hex, d1l_contact_entry_t *out_contact);
esp_err_t d1l_app_model_set_contact_flags(const char *fingerprint, bool favorite, bool muted,
                                          d1l_contact_entry_t *out_contact);
esp_err_t d1l_app_model_rename_contact(const char *fingerprint, const char *alias,
                                       d1l_contact_entry_t *out_contact);
esp_err_t d1l_app_model_delete_contact(const char *fingerprint, d1l_contact_entry_t *out_contact);
esp_err_t d1l_app_model_export_contact_uri(const char *fingerprint, char *dest, size_t dest_size);
size_t d1l_app_model_copy_route_trace(const char *fingerprint, d1l_route_entry_t *out_entries,
                                      size_t max_entries);
size_t d1l_app_model_query_nodes(const d1l_node_query_t *query, d1l_node_view_t *out_entries,
                                 size_t max_entries);
esp_err_t d1l_app_model_mark_public_read(void);
esp_err_t d1l_app_model_mark_channel_read(uint64_t channel_id);
esp_err_t d1l_app_model_mark_dm_thread_read(const char *fingerprint);
esp_err_t d1l_app_model_request_advert(bool flood);
esp_err_t d1l_app_model_set_map_location(int32_t lat_e7, int32_t lon_e7);
esp_err_t d1l_app_model_set_companion_map_location(
    int32_t lat_e7, int32_t lon_e7);
esp_err_t d1l_app_model_clear_map_location(void);
esp_err_t d1l_app_model_set_timezone_offset_minutes(
    int16_t offset_minutes);
esp_err_t d1l_app_model_set_wifi_enabled(bool enabled);
esp_err_t d1l_app_model_wifi_scan(d1l_wifi_scan_result_t *out_result);
esp_err_t d1l_app_model_wifi_connect(void);
esp_err_t d1l_app_model_wifi_disconnect(void);
esp_err_t d1l_app_model_save_wifi_profile(const char *ssid, const char *password);
esp_err_t d1l_app_model_select_wifi_profile(uint8_t profile_index);
esp_err_t d1l_app_model_delete_wifi_profile(uint8_t profile_index);
esp_err_t d1l_app_model_clear_wifi_profile(void);
esp_err_t d1l_app_model_set_ble_enabled(bool enabled);
esp_err_t d1l_app_model_ble_begin_pairing(void);
esp_err_t d1l_app_model_ble_forget_peer(void);
void d1l_app_model_current_radio_profile(d1l_app_radio_profile_edit_t *profile);
void d1l_app_model_default_radio_profile(d1l_app_radio_profile_edit_t *profile);
esp_err_t d1l_app_model_save_radio_profile(const d1l_app_radio_profile_edit_t *profile);
esp_err_t d1l_app_model_complete_onboarding(const char *node_name);
esp_err_t d1l_app_model_reset_onboarding(void);
