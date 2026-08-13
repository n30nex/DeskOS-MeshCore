#include "app_model.h"

#include <stdio.h>
#include <string.h>

#include "app/dm_conversation_list.h"
#include "app/settings_model.h"
#include "comms/connectivity_manager.h"
#include "d1l_config.h"
#include "diagnostics/health_monitor.h"
#include "esp_attr.h"
#include "hal/display_preferences.h"
#include "mesh/admin_credential_store.h"
#include "mesh/channel_message_coordinator.h"
#include "mesh/meshcore_service.h"
#include "mesh/read_state.h"
#include "map/map_tile_provider.h"
#include "platform/time_service.h"
#include "platform/secure_random.h"
#include "storage/map_tile_store.h"
#include "storage/storage_status.h"

static d1l_app_model_t s_model = {
    .board_ready = false,
    .ui_ready = false,
    .board_error = ESP_ERR_INVALID_STATE,
    .boot_count = 0,
};

/* Snapshot publication is serialized by the UI task.  Keep the bounded
 * retained projection out of that task's stack. */
static d1l_dm_entry_t
    s_dm_conversation_source[D1L_DM_CONVERSATION_SOURCE_CAPACITY]
    EXT_RAM_BSS_ATTR;
static bool
    s_dm_conversation_source_unread[D1L_DM_CONVERSATION_SOURCE_CAPACITY]
    EXT_RAM_BSS_ATTR;
static d1l_dm_conversation_summary_t
    s_dm_conversation_summaries[D1L_APP_SNAPSHOT_DM_PREVIEW]
    EXT_RAM_BSS_ATTR;
static d1l_contact_entry_t
    s_dm_capable_contact_source[D1L_CONTACT_STORE_CAPACITY]
    EXT_RAM_BSS_ATTR;

static size_t count_dm_capable_contacts(void)
{
    memset(s_dm_capable_contact_source, 0,
           sizeof(s_dm_capable_contact_source));
    const size_t source_count = d1l_contact_store_copy_recent(
        s_dm_capable_contact_source, D1L_CONTACT_STORE_CAPACITY);
    size_t capable_count = 0U;
    for (size_t i = 0U; i < source_count; ++i) {
        if (d1l_contact_store_can_dm(&s_dm_capable_contact_source[i])) {
            capable_count++;
        }
    }
    return capable_count;
}

static bool retained_store_persistence_degraded(
    const d1l_storage_status_t *storage,
    d1l_retained_blob_store_id_t store_id)
{
    if (!storage || store_id >= D1L_RETAINED_BLOB_STORE_COUNT) {
        return true;
    }
    const d1l_retained_blob_store_sd_stats_t *stats =
        &storage->retained_sd_stats[store_id];
    return !d1l_retained_blob_store_nvs_ready() ||
        d1l_retained_blob_store_nvs_error() != ESP_OK ||
        d1l_retained_blob_store_nvs_migration_error() != ESP_OK ||
        stats->nvs_mirror_last_error != ESP_OK ||
        stats->sd_degraded_latched;
}

static void hex_prefix(char *dest, size_t dest_size, const uint8_t *src, size_t src_len)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!dest || dest_size == 0) {
        return;
    }
    size_t out = 0;
    for (size_t i = 0; src && i < src_len && out + 2U < dest_size; ++i) {
        dest[out++] = hex[(src[i] >> 4) & 0x0fU];
        dest[out++] = hex[src[i] & 0x0fU];
    }
    dest[out] = '\0';
}

static bool valid_radio_edit(const d1l_app_radio_profile_edit_t *profile)
{
    if (!profile) {
        return false;
    }
    return profile->frequency_hz >= 902000000UL &&
           profile->frequency_hz <= 928000000UL &&
           profile->bandwidth_tenths_khz >= 78U &&
           profile->bandwidth_tenths_khz <= 5000U &&
           profile->spreading_factor >= 5U &&
           profile->spreading_factor <= 12U &&
           profile->coding_rate >= 5U &&
           profile->coding_rate <= 8U &&
           profile->tx_power_dbm >= -9 &&
           profile->tx_power_dbm <= D1L_RADIO_TX_POWER_DBM;
}

static bool multi_channel_management_available(void)
{
    return d1l_release_feature_available(
        D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT);
}

static void radio_edit_from_settings(const d1l_settings_t *settings,
                                     d1l_app_radio_profile_edit_t *profile)
{
    if (!profile) {
        return;
    }
    d1l_settings_t snapshot = {0};
    const d1l_settings_t *src = settings;
    if (!src) {
        (void)d1l_settings_public_snapshot(&snapshot);
        src = &snapshot;
    }
    profile->frequency_hz = src->frequency_hz;
    profile->bandwidth_tenths_khz = src->bandwidth_tenths_khz;
    profile->spreading_factor = src->spreading_factor;
    profile->coding_rate = src->coding_rate;
    profile->tx_power_dbm = src->tx_power_dbm;
    profile->rx_boost = src->rx_boost;
}

static uint32_t age_seconds(uint32_t now_ms, uint32_t event_ms)
{
    if (event_ms == 0 || now_ms <= event_ms) {
        return 0;
    }
    return (now_ms - event_ms) / 1000U;
}

static void copy_cstr(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    size_t i = 0;
    while (src && src[i] && i + 1U < dest_size) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static void populate_home_messages(d1l_app_snapshot_t *snapshot)
{
    bool public_used[D1L_APP_SNAPSHOT_MESSAGE_PREVIEW] = {0};
    bool dm_used[D1L_APP_SNAPSHOT_DM_PREVIEW] = {0};
    snapshot->home_message_count = 0;

    while (snapshot->home_message_count < D1L_HOME_MESSAGE_PREVIEW) {
        bool found_public = false;
        size_t public_index = 0;
        uint32_t public_uptime = 0;
        for (size_t i = 0; i < snapshot->recent_message_count; ++i) {
            if (!public_used[i] && (!found_public ||
                                   snapshot->recent_messages[i].uptime_ms > public_uptime)) {
                found_public = true;
                public_index = i;
                public_uptime = snapshot->recent_messages[i].uptime_ms;
            }
        }

        bool found_dm = false;
        size_t dm_index = 0;
        uint32_t dm_uptime = 0;
        for (size_t i = 0; i < snapshot->recent_dm_count; ++i) {
            if (!dm_used[i] && (!found_dm || snapshot->recent_dms[i].uptime_ms > dm_uptime)) {
                found_dm = true;
                dm_index = i;
                dm_uptime = snapshot->recent_dms[i].uptime_ms;
            }
        }

        if (!found_public && !found_dm) {
            break;
        }

        d1l_home_message_preview_t *preview =
            &snapshot->home_messages[snapshot->home_message_count++];
        if (found_dm && (!found_public || dm_uptime >= public_uptime)) {
            const d1l_dm_entry_t *entry = &snapshot->recent_dms[dm_index];
            dm_used[dm_index] = true;
            preview->is_dm = true;
            preview->unread = snapshot->recent_dm_unread[dm_index];
            copy_cstr(preview->sender, sizeof(preview->sender),
                      entry->contact_alias[0] ? entry->contact_alias : "Direct");
            copy_cstr(preview->target_fingerprint, sizeof(preview->target_fingerprint),
                      entry->contact_fingerprint);
            copy_cstr(preview->direction, sizeof(preview->direction), entry->direction);
            copy_cstr(preview->status, sizeof(preview->status),
                      preview->unread ? "new DM" :
                      (entry->acked ? "acked" :
                       (entry->direction[0] == 't' ? "sent" : "direct")));
            copy_cstr(preview->text, sizeof(preview->text), entry->text);
            preview->age_sec = age_seconds(snapshot->uptime_ms, entry->uptime_ms);
            preview->rssi_dbm = entry->rssi_dbm;
            preview->snr_tenths = entry->snr_tenths;
            preview->path_hops = entry->path_hops;
        } else {
            const d1l_message_entry_t *entry = &snapshot->recent_messages[public_index];
            public_used[public_index] = true;
            preview->is_dm = false;
            preview->unread = entry->direction[0] == 'r' &&
                              entry->seq > snapshot->last_public_read_seq;
            copy_cstr(preview->sender, sizeof(preview->sender),
                      entry->author[0] ? entry->author : "Public");
            preview->target_fingerprint[0] = '\0';
            copy_cstr(preview->direction, sizeof(preview->direction), entry->direction);
            copy_cstr(preview->status, sizeof(preview->status),
                      preview->unread ? "new" :
                      (entry->direction[0] == 't' ? "sent" : "public"));
            copy_cstr(preview->text, sizeof(preview->text), entry->text);
            preview->age_sec = age_seconds(snapshot->uptime_ms, entry->uptime_ms);
            preview->rssi_dbm = entry->rssi_dbm;
            preview->snr_tenths = entry->snr_tenths;
            preview->path_hops = entry->path_hops;
        }
    }
}

static void populate_dm_conversation_summaries(d1l_app_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    memset(s_dm_conversation_source, 0, sizeof(s_dm_conversation_source));
    memset(s_dm_conversation_source_unread, 0,
           sizeof(s_dm_conversation_source_unread));
    memset(s_dm_conversation_summaries, 0,
           sizeof(s_dm_conversation_summaries));
    const size_t source_count = d1l_dm_store_copy_recent(
        s_dm_conversation_source, D1L_DM_CONVERSATION_SOURCE_CAPACITY);
    snapshot->dm_failure_latched =
        d1l_dm_conversation_list_has_retained_failure(
            s_dm_conversation_source, source_count);
    for (size_t i = 0U; i < source_count; ++i) {
        s_dm_conversation_source_unread[i] =
            d1l_read_state_dm_entry_is_unread(
                &s_dm_conversation_source[i]);
    }
    snapshot->recent_dm_count = d1l_dm_conversation_list_project(
        s_dm_conversation_source, s_dm_conversation_source_unread,
        source_count,
        s_dm_conversation_summaries, D1L_APP_SNAPSHOT_DM_PREVIEW,
        &snapshot->dm_conversation_count);
    for (size_t i = 0U; i < snapshot->recent_dm_count; ++i) {
        d1l_contact_entry_t contact = {0};
        s_dm_conversation_summaries[i].muted =
            d1l_contact_store_find_by_fingerprint(
                s_dm_conversation_summaries[i].latest.contact_fingerprint,
                &contact) && contact.muted;
        snapshot->recent_dms[i] = s_dm_conversation_summaries[i].latest;
        snapshot->recent_dm_unread_count[i] =
            s_dm_conversation_summaries[i].unread_count;
        snapshot->recent_dm_unread[i] =
            snapshot->recent_dm_unread_count[i] > 0U;
        snapshot->recent_dm_muted[i] = s_dm_conversation_summaries[i].muted;
    }
}

static void populate_home_repeaters(d1l_app_snapshot_t *snapshot)
{
    snapshot->home_repeater_count = 0;
    for (size_t i = 0;
         i < snapshot->recent_repeater_count && snapshot->home_repeater_count < D1L_HOME_REPEATER_PREVIEW;
         ++i) {
        const d1l_mesh_repeater_candidate_t *src = &snapshot->recent_repeaters[i];
        d1l_home_repeater_preview_t *dest =
            &snapshot->home_repeaters[snapshot->home_repeater_count++];
        copy_cstr(dest->label, sizeof(dest->label),
                  src->label[0] ? src->label : src->target);
        copy_cstr(dest->route, sizeof(dest->route),
                  src->route[0] ? src->route : "unknown");
        copy_cstr(dest->kind, sizeof(dest->kind),
                  src->kind[0] ? src->kind : "repeater");
        dest->age_sec = age_seconds(snapshot->uptime_ms, src->last_seen_ms);
        dest->rssi_dbm = src->rssi_dbm;
        dest->snr_tenths = src->snr_tenths;
        dest->path_hops = src->path_hops;
    }
}

d1l_app_model_t *d1l_app_model_get(void)
{
    return &s_model;
}

void d1l_app_model_snapshot(d1l_app_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    d1l_settings_t settings = {0};
    d1l_display_preferences_t display_preferences = {0};
    (void)d1l_settings_public_snapshot(&settings);
    d1l_display_preferences_get(&display_preferences);
    d1l_meshcore_service_status_t mesh = d1l_meshcore_service_status();
    d1l_message_store_stats_t messages = d1l_message_store_stats();
    d1l_dm_store_stats_t dms = d1l_dm_store_stats();
    d1l_read_state_stats_t read_state = d1l_read_state_stats();
    d1l_node_store_stats_t nodes = d1l_node_store_stats();
    d1l_contact_store_stats_t contacts = d1l_contact_store_stats();
    d1l_route_store_stats_t routes = d1l_route_store_stats();
    d1l_packet_log_stats_t packets = d1l_packet_log_stats();
    d1l_health_snapshot_t health = d1l_health_snapshot();
    d1l_connectivity_status_t connectivity = {0};
    d1l_connectivity_status(&connectivity);
    d1l_storage_status_t storage = {0};
    d1l_storage_status(&storage);
    d1l_time_service_status_t time_status = {0};
    d1l_time_service_status(&time_status);

    snapshot->board_ready = s_model.board_ready;
    snapshot->ui_ready = s_model.ui_ready;
    snapshot->release_profile_id = d1l_release_profile_id();
    snapshot->release_profile = d1l_release_profile_name();
    snapshot->sd_history_mode_id = d1l_release_sd_history_mode();
    snapshot->sd_history_mode = d1l_release_sd_history_mode_name();
    snapshot->release_capabilities = *d1l_release_capabilities();
    snapshot->identity_ready = settings.identity_ready || mesh.identity_ready;
    snapshot->settings_load_status = d1l_settings_load_status();
    snapshot->identity_state = d1l_settings_persisted_identity_state();
    snapshot->protocol_tx_ready = time_status.protocol_tx_ready;
    snapshot->protocol_tx_error = time_status.protocol_tx_error;
    snapshot->radio_ready = mesh.radio_ready;
    snapshot->radio_applied = mesh.radio_applied;
    snapshot->radio_apply_pending = mesh.radio_apply_pending;
    snapshot->companion_ready = mesh.companion_framing_ready;
    snapshot->radio_frequency_hz = settings.frequency_hz;
    snapshot->radio_bandwidth_tenths_khz = settings.bandwidth_tenths_khz;
    snapshot->radio_spreading_factor = settings.spreading_factor;
    snapshot->radio_coding_rate = settings.coding_rate;
    snapshot->radio_tx_power_dbm = settings.tx_power_dbm;
    snapshot->radio_rx_boost = settings.rx_boost;
    snapshot->radio_tcxo = d1l_settings_tcxo_name(settings.tcxo_mode);
    snapshot->radio_apply_error = esp_err_to_name(mesh.radio_apply_error);
    snapshot->wifi_enabled = connectivity.wifi_enabled_setting;
    snapshot->ble_companion_enabled = connectivity.ble_companion_enabled_setting;
    snapshot->observer_enabled = connectivity.observer_enabled_setting;
    snapshot->wifi_profile_saved = connectivity.wifi_profile_saved;
    snapshot->wifi_password_saved = connectivity.wifi_password_saved;
    snapshot->wifi_profile_count = (uint8_t)
        d1l_connectivity_wifi_profiles(
            snapshot->wifi_profiles,
            D1L_WIFI_PROFILE_CAPACITY,
            &snapshot->wifi_active_profile);
    snapshot->wifi_scan_supported =
        d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL) &&
        connectivity.wifi_scan_supported;
    snapshot->wifi_stack_active = connectivity.wifi_stack_active;
    snapshot->wifi_connected = connectivity.wifi_connected;
    snapshot->wifi_connecting = connectivity.wifi_connecting;
    snapshot->onboarding_complete = settings.onboarding_complete;
    snapshot->wifi_build_enabled = connectivity.wifi_build_enabled;
    snapshot->ble_build_enabled = connectivity.ble_build_enabled;
    snapshot->ble_transport_supported =
        d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE) &&
        D1L_BLE_COMPANION_TRANSPORT_SUPPORTED;
    snapshot->ble_peer_known = connectivity.ble_peer_known;
    snapshot->ble_protocol_running = connectivity.ble_protocol_running;
    snapshot->ble_protocol_ready = connectivity.ble_protocol_ready;
    snapshot->wifi_state = connectivity.wifi_state;
    snapshot->wifi_last_error = connectivity.wifi_last_error;
    snapshot->wifi_rssi_dbm = connectivity.wifi_rssi_dbm;
    snapshot->wifi_channel = connectivity.wifi_channel;
    snapshot->ble_pairing_passkey = connectivity.ble_pairing_passkey;
    snapshot->ble_protocol_command_count =
        connectivity.ble_protocol_command_count;
    snapshot->ble_protocol_response_count =
        connectivity.ble_protocol_response_count;
    snapshot->ble_protocol_unsupported_count =
        connectivity.ble_protocol_unsupported_count;
    snapshot->ble_protocol_malformed_count =
        connectivity.ble_protocol_malformed_count;
    snapshot->ble_state = connectivity.ble_state;
    snapshot->coexistence_policy = connectivity.coexistence_policy;
    snapshot->storage_direct_supported = storage.direct_supported;
    snapshot->storage_rp2040_bridge_required = storage.rp2040_bridge_required;
    snapshot->storage_rp2040_bridge_ready = storage.rp2040_bridge_ready;
    snapshot->storage_rp2040_sd_protocol_supported = storage.rp2040_sd_protocol_supported;
    snapshot->storage_sd_present = storage.sd_present;
    snapshot->storage_sd_mounted = storage.sd_mounted;
    snapshot->storage_sd_data_root_ready = storage.sd_data_root_ready;
    snapshot->storage_sd_needs_fat32 = storage.sd_needs_fat32;
    snapshot->storage_setup_required = storage.setup_required;
    snapshot->storage_setup_supported = storage.setup_supported;
    snapshot->storage_data_enabled = storage.data_enabled;
    snapshot->storage_response_truncated = storage.response_truncated;
    snapshot->storage_retained_sd_degraded = storage.retained_sd_degraded;
    snapshot->storage_retained_backup_degraded = storage.retained_backup_degraded;
    snapshot->message_store_loaded = messages.loaded;
    snapshot->dm_store_loaded = dms.loaded;
    snapshot->message_store_persistence_degraded =
        retained_store_persistence_degraded(
            &storage, D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES);
    snapshot->dm_store_persistence_degraded =
        retained_store_persistence_degraded(
            &storage, D1L_RETAINED_BLOB_STORE_DM_MESSAGES);
    snapshot->map_page_supported =
        d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP);
    snapshot->map_tile_cache_ready = d1l_map_tile_store_sd_ready(&storage);
    snapshot->map_tile_render_supported = D1L_MAP_TILE_RENDER_SUPPORTED;
    snapshot->map_tile_sideload_supported = false;
    d1l_map_tile_provider_t map_provider = {0};
    d1l_map_tile_provider_snapshot(&map_provider);
    snapshot->map_tile_provider_configured = map_provider.configured;
    snapshot->map_location_set = settings.map_location_set;
    snapshot->map_center_source = D1L_MAP_CENTER_SOURCE_UNKNOWN;
    if (settings.map_location_set &&
        settings.map_location_source ==
            D1L_MAP_LOCATION_SOURCE_AUTHENTICATED_COMPANION) {
        snapshot->map_center_source =
            D1L_MAP_CENTER_SOURCE_AUTHENTICATED_COMPANION;
    } else if (settings.map_location_set &&
               settings.map_location_source ==
                   D1L_MAP_LOCATION_SOURCE_MANUAL) {
        snapshot->map_center_source = D1L_MAP_CENTER_SOURCE_MANUAL;
    }
    snapshot->map_marker_age_reference_valid =
        time_status.clock.certificate_time_valid &&
        time_status.clock.wall_epoch_sec >= D1L_TIME_WALL_MIN_EPOCH &&
        time_status.clock.wall_epoch_sec <= (int64_t)UINT32_MAX;
    if (snapshot->map_marker_age_reference_valid) {
        snapshot->map_marker_reference_timestamp =
            (uint32_t)time_status.clock.wall_epoch_sec;
    }
    snapshot->map_tile_download_supported = snapshot->map_page_supported &&
                                            d1l_release_feature_available(
                                                D1L_RELEASE_FEATURE_WIFI_USER_CONTROL) &&
                                            connectivity.wifi_build_enabled &&
                                            connectivity.wifi_connected &&
                                            snapshot->map_tile_cache_ready &&
                                            snapshot->map_location_set &&
                                            snapshot->map_tile_render_supported;
    snapshot->map_lat_e7 = settings.map_lat_e7;
    snapshot->map_lon_e7 = settings.map_lon_e7;
    snapshot->map_tile_zoom = settings.map_tile_zoom;
    snapshot->storage_capacity_kb = storage.capacity_kb;
    snapshot->storage_free_kb = storage.free_kb;
    snapshot->storage_last_error = storage.last_error;
    snapshot->storage_sd_state = storage.sd_state;
    snapshot->storage_sd_interface = storage.sd_interface;
    snapshot->storage_sd_filesystem = storage.sd_filesystem;
    snapshot->storage_mount_point = storage.mount_point;
    snapshot->storage_data_root = storage.data_root;
    snapshot->storage_backend = storage.data_backend;
    snapshot->message_store_backend = storage.message_store_backend;
    snapshot->dm_store_backend = storage.dm_store_backend;
    snapshot->packet_log_backend = storage.packet_log_backend;
    snapshot->route_store_backend = storage.route_store_backend;
    snapshot->node_store_backend = storage.node_store_backend;
    snapshot->contact_store_backend = storage.contact_store_backend;
    snapshot->read_state_backend = storage.read_state_backend;
    snapshot->map_tile_backend = storage.map_tile_backend;
    snapshot->export_backend = storage.export_backend;
    snapshot->map_tile_cache_policy = D1L_MAP_TILE_CACHE_POLICY;
    snapshot->map_tile_cache_path_template = D1L_MAP_TILE_CACHE_PATH_TEMPLATE;
    snapshot->map_tile_download_state = !snapshot->map_tile_cache_ready ? "sd_cache_required" :
                                        !connectivity.wifi_build_enabled ? "wifi_unavailable" :
                                        !connectivity.wifi_connected ? "wifi_required" :
                                        !settings.map_location_set ? "location_required" :
                                        !snapshot->map_tile_render_supported ? "tile_render_pending" :
                                        "ready";
    snapshot->map_tile_download_requires = D1L_MAP_TILE_DOWNLOAD_REQUIRES;
    snapshot->map_tile_provider_policy = D1L_MAP_TILE_PROVIDER_POLICY;
    snapshot->map_tile_provider_attribution = D1L_MAP_TILE_PROVIDER_ATTRIBUTION;
    snapshot->storage_setup_action = storage.setup_action;
    snapshot->storage_note = storage.note;
    snapshot->time_available = time_status.display_time_valid;
    snapshot->time_approximate = time_status.display_time_approximate;
    snapshot->timezone_settings_ready =
        time_status.timezone_settings_ready;
    snapshot->high_contrast = settings.high_contrast;
    snapshot->night_mode = settings.night_mode;
    snapshot->display_brightness_percent =
        display_preferences.brightness_percent;
    snapshot->display_timeout_seconds =
        display_preferences.timeout_seconds;
    snapshot->notification_mode =
        (uint8_t)display_preferences.notification_mode;
    snapshot->timezone_offset_minutes =
        time_status.timezone_offset_minutes;
    copy_cstr(snapshot->time_label, sizeof(snapshot->time_label),
              time_status.display_time);
    copy_cstr(snapshot->timezone_label, sizeof(snapshot->timezone_label),
              time_status.timezone_label);
    snprintf(snapshot->node_name, sizeof(snapshot->node_name), "%s", settings.node_name);
    copy_cstr(snapshot->wifi_ssid, sizeof(snapshot->wifi_ssid),
              connectivity.wifi_ssid);
    copy_cstr(snapshot->wifi_ip, sizeof(snapshot->wifi_ip),
              connectivity.wifi_ip ? connectivity.wifi_ip : "");
    if (settings.identity_ready) {
        hex_prefix(snapshot->identity_fingerprint, sizeof(snapshot->identity_fingerprint),
                   settings.identity_public_key, 8U);
    }
    snapshot->mesh_state = d1l_meshcore_service_state_name(mesh.state);
    snapshot->reset_reason = health.reset_reason;
    snapshot->uptime_ms = health.uptime_ms;
    snapshot->heap_free = health.heap_free;
    snapshot->heap_min_free = health.heap_min_free;
    snapshot->heap_largest_free = health.heap_largest_free;
    snapshot->psram_free = health.psram_free;
    snapshot->psram_min_free = health.psram_min_free;
    snapshot->psram_largest_free = health.psram_largest_free;
    snapshot->current_task_stack_free_words = health.current_task_stack_free_words;
    snapshot->ui_task_stack_free_words = health.ui_task_stack_free_words;
    snapshot->lvgl_free_bytes = health.lvgl_free_bytes;
    snapshot->lvgl_largest_free_bytes = health.lvgl_largest_free_bytes;
    snapshot->lvgl_used_pct = health.lvgl_used_pct;
    snapshot->rx_packets = mesh.rx_packets;
    snapshot->rx_adverts = mesh.rx_adverts;
    snapshot->tx_packets = mesh.tx_packets;
    snapshot->rejected_commands = mesh.rejected_commands;
    snapshot->message_total_written = messages.total_written;
    /* The retained ring is shared by bounded group channels. Existing
     * Messages/Public surfaces must remain exact-Public and never infer their
     * total from private-channel occupancy. */
    snapshot->message_count = messages.public_count;
    snapshot->dm_total_written = dms.total_written;
    snapshot->dm_content_revision = dms.content_revision;
    snapshot->dm_count = dms.count;
    snapshot->dm_delivery_state = mesh.dm_delivery_state;
    const d1l_dm_delivery_state_t delivery_state =
        (d1l_dm_delivery_state_t)mesh.dm_delivery_state;
    snapshot->dm_delivery_active =
        mesh.dm_delivery_session_id != 0U &&
        d1l_dm_delivery_state_valid(delivery_state) &&
        !d1l_dm_delivery_state_terminal(delivery_state);
    snapshot->dm_retry_active = snapshot->dm_delivery_active &&
        (delivery_state == D1L_DM_DELIVERY_RETRY_WAIT ||
         delivery_state == D1L_DM_DELIVERY_RETRY_TX);
    snapshot->public_unread_count = read_state.public_unread_count;
    snapshot->dm_unread_count = read_state.dm_unread_count;
    snapshot->muted_dm_unread_count = read_state.muted_dm_unread_count;
    snapshot->last_public_read_seq = read_state.last_public_read_seq;
    snapshot->last_dm_read_seq = read_state.last_dm_read_seq;
    snapshot->read_state_mark_count = read_state.mark_read_count;
    snapshot->node_total_written = nodes.total_written;
    snapshot->node_count = nodes.count;
    snapshot->contact_total_written = contacts.total_written;
    snapshot->contact_count = contacts.count;
    snapshot->dm_capable_contact_count = count_dm_capable_contacts();
    snapshot->route_total_written = routes.total_written;
    snapshot->route_count = routes.count;
    snapshot->packet_total_written = packets.total_written;
    snapshot->packet_count = packets.count;
    d1l_channel_store_stats_t channel_stats = {0};
    (void)d1l_app_model_copy_channels(
        snapshot->channels, D1L_APP_SNAPSHOT_CHANNEL_PREVIEW,
        &snapshot->channel_count, &snapshot->active_channel_id,
        &channel_stats);
    snapshot->channel_store_revision = channel_stats.revision;
    snapshot->channel_store_load_status = channel_stats.load_status;
    snapshot->channel_store_loaded = channel_stats.loaded;
    snapshot->channel_message_reconcile_pending =
        d1l_channel_message_reconcile_pending();
    d1l_mesh_inspector_signal_summary(&snapshot->signal_summary);
    snapshot->recent_room_count =
        d1l_mesh_inspector_copy_room_servers(snapshot->recent_rooms, D1L_APP_SNAPSHOT_ROOM_PREVIEW);
    snapshot->recent_repeater_count =
        d1l_mesh_inspector_copy_repeater_candidates(snapshot->recent_repeaters,
                                                    D1L_APP_SNAPSHOT_REPEATER_PREVIEW);
    snapshot->path_hash_bytes = mesh.path_hash_bytes;
    snapshot->recent_contact_count =
        d1l_contact_store_copy_recent(snapshot->recent_contacts, D1L_APP_SNAPSHOT_CONTACT_PREVIEW);
    snapshot->recent_route_count =
        d1l_route_store_copy_recent(snapshot->recent_routes, D1L_APP_SNAPSHOT_ROUTE_PREVIEW);
    const d1l_node_query_t node_query = {
        .filter = D1L_NODE_FILTER_ALL,
        .sort = D1L_NODE_SORT_LAST_HEARD,
        .text = NULL,
        .keyed_only = false,
        .reachable_only = false,
    };
    snapshot->recent_node_count =
        d1l_app_model_query_nodes(&node_query, snapshot->recent_nodes,
                                  D1L_APP_SNAPSHOT_NODE_PREVIEW);
    snapshot->recent_message_count =
        d1l_message_store_copy_recent(snapshot->recent_messages, D1L_APP_SNAPSHOT_MESSAGE_PREVIEW);
    populate_dm_conversation_summaries(snapshot);
    snapshot->recent_packet_count =
        d1l_packet_log_copy_recent(snapshot->recent_packets, D1L_APP_SNAPSHOT_PACKET_PREVIEW);
    populate_home_messages(snapshot);
    populate_home_repeaters(snapshot);
}

esp_err_t d1l_app_model_send_public_text(const char *text)
{
    return d1l_meshcore_service_send_public(text);
}

esp_err_t d1l_app_model_send_channel_text(uint64_t channel_id,
                                          const char *text)
{
    if (!multi_channel_management_available() &&
        channel_id != D1L_CHANNEL_PUBLIC_ID) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return d1l_meshcore_service_send_channel(channel_id, text);
}

esp_err_t d1l_app_model_send_active_channel_text(const char *text)
{
    if (!multi_channel_management_available()) {
        return d1l_meshcore_service_send_channel(
            D1L_CHANNEL_PUBLIC_ID, text);
    }
    return d1l_meshcore_service_send_active_channel(text);
}

esp_err_t d1l_app_model_copy_channels(d1l_channel_info_t *out_channels,
                                      size_t max_channels,
                                      size_t *out_count,
                                      uint64_t *out_active_channel_id,
                                      d1l_channel_store_stats_t *out_stats)
{
    const esp_err_t ret = d1l_channel_store_snapshot(
        out_channels, max_channels, out_count, out_active_channel_id,
        out_stats);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!multi_channel_management_available()) {
        size_t public_index = *out_count;
        for (size_t i = 0U; i < *out_count; ++i) {
            if (out_channels[i].channel_id == D1L_CHANNEL_PUBLIC_ID) {
                public_index = i;
                break;
            }
        }
        if (public_index == *out_count) {
            *out_count = 0U;
            *out_active_channel_id = 0U;
            return ESP_ERR_INVALID_STATE;
        }
        if (public_index != 0U) {
            out_channels[0] = out_channels[public_index];
        }
        *out_count = 1U;
        *out_active_channel_id = D1L_CHANNEL_PUBLIC_ID;
        out_stats->count = 1U;
    }
    const d1l_read_state_stats_t read_state = d1l_read_state_stats();
    for (size_t i = 0U; i < *out_count; ++i) {
        if (out_channels[i].channel_id == D1L_CHANNEL_PUBLIC_ID) {
            out_channels[i].unread_count = read_state.public_unread_count;
            break;
        }
    }
    return ESP_OK;
}

esp_err_t d1l_app_model_select_channel(uint64_t channel_id,
                                       d1l_channel_info_t *out_channel)
{
    if (channel_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!multi_channel_management_available() &&
        channel_id != D1L_CHANNEL_PUBLIC_ID) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (out_channel) {
        memset(out_channel, 0, sizeof(*out_channel));
    }

    d1l_channel_mutation_result_t result = D1L_CHANNEL_MUTATION_NONE;
    d1l_channel_info_t selected = {0};
    const esp_err_t ret = d1l_channel_store_select(
        channel_id, &result, &selected);
    if (ret != ESP_OK) {
        return ret;
    }
    if (result != D1L_CHANNEL_MUTATION_UPDATED &&
        result != D1L_CHANNEL_MUTATION_EXISTS) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_channel) {
        *out_channel = selected;
    }
    return ESP_OK;
}

static esp_err_t prepare_channel_mutation_outputs(
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_channel)
{
    if (out_channel) {
        memset(out_channel, 0, sizeof(*out_channel));
    }
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = D1L_CHANNEL_MUTATION_NONE;
    return ESP_OK;
}

static void clear_sensitive_bytes(void *data, size_t data_size)
{
    volatile uint8_t *cursor = (volatile uint8_t *)data;
    while (cursor && data_size > 0U) {
        *cursor++ = 0U;
        data_size--;
    }
}

esp_err_t d1l_app_model_add_channel(
    const char *name, const uint8_t *secret, uint8_t secret_len,
    bool enabled, bool make_default,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_channel)
{
    if (!multi_channel_management_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t prepared = prepare_channel_mutation_outputs(
        out_result, out_channel);
    if (prepared != ESP_OK) {
        return prepared;
    }
    return d1l_channel_store_add(name, secret, secret_len, enabled,
                                 make_default, out_result, out_channel);
}

esp_err_t d1l_app_model_create_channel(
    const char *name, bool make_default,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_channel)
{
    if (!multi_channel_management_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t prepared = prepare_channel_mutation_outputs(
        out_result, out_channel);
    if (prepared != ESP_OK) {
        return prepared;
    }
    uint8_t secret[D1L_CHANNEL_SECRET_128_LEN];
    const esp_err_t random_ret =
        d1l_secure_random_fill(secret, sizeof(secret));
    if (random_ret != ESP_OK) {
        clear_sensitive_bytes(secret, sizeof(secret));
        return random_ret;
    }
    const esp_err_t ret = d1l_channel_store_add(
        name, secret, (uint8_t)sizeof(secret), true, make_default,
        out_result, out_channel);
    clear_sensitive_bytes(secret, sizeof(secret));
    return ret;
}

esp_err_t d1l_app_model_import_channel_uri(
    const char *uri, size_t uri_len,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_channel)
{
    if (!multi_channel_management_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t prepared = prepare_channel_mutation_outputs(
        out_result, out_channel);
    if (prepared != ESP_OK) {
        return prepared;
    }
    return d1l_channel_store_import_uri(uri, uri_len, out_result,
                                        out_channel);
}

esp_err_t d1l_app_model_update_channel(
    uint64_t channel_id, const char *name, bool enabled, bool make_default,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_channel)
{
    if (!multi_channel_management_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t prepared = prepare_channel_mutation_outputs(
        out_result, out_channel);
    if (prepared != ESP_OK) {
        return prepared;
    }
    return d1l_channel_store_update(channel_id, name, enabled, make_default,
                                    out_result, out_channel);
}

esp_err_t d1l_app_model_remove_channel(
    uint64_t channel_id, bool confirmed,
    d1l_channel_mutation_result_t *out_result,
    d1l_channel_info_t *out_channel)
{
    if (!multi_channel_management_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t prepared = prepare_channel_mutation_outputs(
        out_result, out_channel);
    if (prepared != ESP_OK) {
        return prepared;
    }
    if (!confirmed) {
        return ESP_ERR_INVALID_STATE;
    }
    return d1l_channel_store_remove(channel_id, out_result, out_channel);
}

esp_err_t d1l_app_model_export_channel_share_uri(
    uint64_t channel_id, char *dest, size_t dest_size)
{
    if (!multi_channel_management_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!dest || dest_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_app_model_clear_channel_share_uri(dest, dest_size);
    const esp_err_t ret = d1l_channel_store_export_share_uri(
        channel_id, dest, dest_size);
    if (ret != ESP_OK) {
        d1l_app_model_clear_channel_share_uri(dest, dest_size);
    }
    return ret;
}

void d1l_app_model_clear_channel_share_uri(char *dest, size_t dest_size)
{
    clear_sensitive_bytes(dest, dest_size);
}

size_t d1l_app_model_query_public_messages_page(d1l_message_entry_t *out_entries,
                                                size_t max_entries,
                                                size_t skip_newest,
                                                const char *query,
                                                size_t *out_total_matches)
{
    return d1l_message_store_query_page(out_entries, max_entries, skip_newest,
                                        query, out_total_matches);
}

size_t d1l_app_model_query_public_messages(d1l_message_entry_t *out_entries,
                                           size_t max_entries, const char *query)
{
    return d1l_app_model_query_public_messages_page(out_entries, max_entries, 0,
                                                    query, NULL);
}

size_t d1l_app_model_query_channel_messages_page(
    uint64_t channel_id, d1l_message_entry_t *out_entries,
    size_t max_entries, size_t skip_newest, const char *query,
    size_t *out_total_matches)
{
    if (!multi_channel_management_available() &&
        channel_id != D1L_CHANNEL_PUBLIC_ID) {
        if (out_total_matches) {
            *out_total_matches = 0U;
        }
        return 0U;
    }
    return d1l_message_store_query_channel_page(
        channel_id, out_entries, max_entries, skip_newest, query,
        out_total_matches);
}

esp_err_t d1l_app_model_send_dm_text(const char *fingerprint, const char *text)
{
    return d1l_meshcore_service_send_dm(fingerprint, text);
}

esp_err_t d1l_app_model_request_path_discovery_probe(const char *fingerprint,
                                                     char *out_token,
                                                     size_t out_token_size)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return d1l_meshcore_service_request_path_discovery_probe(
        fingerprint, out_token, out_token_size);
}

void d1l_app_model_contact_telemetry_snapshot(
    const char *fingerprint,
    d1l_meshcore_contact_telemetry_snapshot_t *out_snapshot)
{
    d1l_meshcore_service_contact_telemetry_snapshot(
        fingerprint, out_snapshot);
}

esp_err_t d1l_app_model_reset_contact_route(const char *fingerprint)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return d1l_meshcore_service_reset_contact_route(fingerprint);
}

esp_err_t d1l_app_model_send_trace_contact(const char *fingerprint)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return d1l_meshcore_service_send_trace_contact(fingerprint);
}

void d1l_app_model_trace_snapshot(
    d1l_meshcore_trace_snapshot_t *out_snapshot)
{
    d1l_meshcore_service_trace_snapshot(out_snapshot);
}

esp_err_t d1l_app_model_ping_repeater(const char *fingerprint)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return d1l_meshcore_service_ping_repeater(fingerprint);
}

esp_err_t d1l_app_model_discover_nearby(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return d1l_meshcore_service_discover_nearby();
}

void d1l_app_model_discovery_snapshot(
    d1l_meshcore_discovery_snapshot_t *out_snapshot)
{
    d1l_meshcore_service_discovery_snapshot(out_snapshot);
}

esp_err_t d1l_app_model_clear_nodes(bool confirmed)
{
    if (!confirmed) {
        return ESP_ERR_NOT_ALLOWED;
    }
    const esp_err_t ret = d1l_node_store_clear();
    if (ret == ESP_OK) {
        d1l_meshcore_service_clear_discovery_results();
    }
    return ret;
}

size_t d1l_app_model_query_dm_thread_page(const char *fingerprint,
                                          d1l_dm_entry_t *out_entries,
                                          bool *out_unread,
                                          size_t max_entries,
                                          size_t skip_newest,
                                          const char *query,
                                          size_t *out_total_matches)
{
    const size_t copied = d1l_dm_store_query_thread_page(
        fingerprint, out_entries, max_entries, skip_newest, query,
        out_total_matches);
    if (out_unread) {
        for (size_t i = 0; i < copied; ++i) {
            out_unread[i] = d1l_read_state_dm_entry_is_unread(&out_entries[i]);
        }
    }
    return copied;
}

size_t d1l_app_model_copy_dm_thread_page(const char *fingerprint,
                                         d1l_dm_entry_t *out_entries,
                                         bool *out_unread, size_t max_entries,
                                         size_t skip_newest,
                                         size_t *out_total_matches)
{
    return d1l_app_model_query_dm_thread_page(
        fingerprint, out_entries, out_unread, max_entries, skip_newest, NULL,
        out_total_matches);
}

size_t d1l_app_model_copy_dm_thread(const char *fingerprint, d1l_dm_entry_t *out_entries,
                                    bool *out_unread, size_t max_entries)
{
    return d1l_app_model_copy_dm_thread_page(fingerprint, out_entries, out_unread,
                                             max_entries, 0, NULL);
}

esp_err_t d1l_app_model_find_contact(const char *fingerprint, d1l_contact_entry_t *out_contact)
{
    if (!fingerprint || fingerprint[0] == '\0' || !out_contact) {
        return ESP_ERR_INVALID_ARG;
    }
    return d1l_contact_store_find_by_fingerprint(fingerprint, out_contact) ?
           ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t d1l_app_model_find_contact_by_public_key(
    const char *public_key_hex, d1l_contact_entry_t *out_contact)
{
    if (!public_key_hex || public_key_hex[0] == '\0' || !out_contact) {
        return ESP_ERR_INVALID_ARG;
    }
    return d1l_contact_store_find_by_public_key(public_key_hex, out_contact) ?
           ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t d1l_app_model_set_contact_flags(const char *fingerprint, bool favorite, bool muted,
                                          d1l_contact_entry_t *out_contact)
{
    return d1l_contact_store_set_flags(fingerprint, favorite, muted, out_contact);
}

esp_err_t d1l_app_model_rename_contact(const char *fingerprint, const char *alias,
                                       d1l_contact_entry_t *out_contact)
{
    return d1l_contact_store_rename(fingerprint, alias, out_contact);
}

esp_err_t d1l_app_model_delete_contact(const char *fingerprint, d1l_contact_entry_t *out_contact)
{
    const esp_err_t forget_ret =
        d1l_admin_credential_store_forget(fingerprint);
    if (forget_ret != ESP_OK && forget_ret != ESP_ERR_NOT_FOUND) {
        return forget_ret;
    }
    return d1l_contact_store_delete(fingerprint, out_contact);
}

esp_err_t d1l_app_model_export_contact_uri(const char *fingerprint, char *dest, size_t dest_size)
{
    if (!fingerprint || fingerprint[0] == '\0' || !dest || dest_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_contact_entry_t contact = {0};
    if (!d1l_contact_store_find_by_fingerprint(fingerprint, &contact)) {
        return ESP_ERR_NOT_FOUND;
    }
    return d1l_contact_store_export_uri(&contact, dest, dest_size);
}

size_t d1l_app_model_copy_route_trace(const char *fingerprint, d1l_route_entry_t *out_entries,
                                      size_t max_entries)
{
    return d1l_route_store_copy_for_target(fingerprint, out_entries, max_entries);
}

size_t d1l_app_model_query_nodes(const d1l_node_query_t *query, d1l_node_view_t *out_entries,
                                 size_t max_entries)
{
    return d1l_node_store_query(query, out_entries, max_entries);
}

esp_err_t d1l_app_model_mark_public_read(void)
{
    const esp_err_t reconcile_ret = d1l_channel_message_reconcile();
    if (reconcile_ret != ESP_OK) {
        return reconcile_ret;
    }
    const esp_err_t channel_ret =
        d1l_channel_store_mark_all_read(D1L_CHANNEL_PUBLIC_ID);
    return channel_ret == ESP_OK ? d1l_read_state_mark_public_read() :
        channel_ret;
}

esp_err_t d1l_app_model_mark_channel_read(uint64_t channel_id)
{
    if (channel_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!multi_channel_management_available() &&
        channel_id != D1L_CHANNEL_PUBLIC_ID) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (channel_id == D1L_CHANNEL_PUBLIC_ID) {
        return d1l_app_model_mark_public_read();
    }
    const esp_err_t reconcile_ret = d1l_channel_message_reconcile();
    return reconcile_ret == ESP_OK ?
        d1l_channel_store_mark_all_read(channel_id) : reconcile_ret;
}

esp_err_t d1l_app_model_mark_dm_thread_read(const char *fingerprint)
{
    return d1l_read_state_mark_dm_thread_read(fingerprint);
}

esp_err_t d1l_app_model_request_advert(bool flood)
{
    return d1l_meshcore_service_request_advert(flood);
}

static esp_err_t validate_map_location(int32_t lat_e7, int32_t lon_e7)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_LOCATION)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (lat_e7 < D1L_MAP_LOCATION_LAT_E7_MIN ||
        lat_e7 > D1L_MAP_LOCATION_LAT_E7_MAX ||
        lon_e7 < D1L_MAP_LOCATION_LON_E7_MIN ||
        lon_e7 > D1L_MAP_LOCATION_LON_E7_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t d1l_app_model_set_map_location(int32_t lat_e7, int32_t lon_e7)
{
    const esp_err_t valid = validate_map_location(lat_e7, lon_e7);
    if (valid != ESP_OK) {
        return valid;
    }
    d1l_settings_t settings = {0};
    settings.map_location_set = true;
    settings.map_lat_e7 = lat_e7;
    settings.map_lon_e7 = lon_e7;
    settings.map_location_source = D1L_MAP_LOCATION_SOURCE_MANUAL;
    return d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_MAP_LOCATION);
}

esp_err_t d1l_app_model_set_companion_map_location(
    int32_t lat_e7, int32_t lon_e7)
{
    const esp_err_t valid = validate_map_location(lat_e7, lon_e7);
    if (valid != ESP_OK) {
        return valid;
    }
    d1l_settings_t settings = {0};
    settings.map_location_set = true;
    settings.map_lat_e7 = lat_e7;
    settings.map_lon_e7 = lon_e7;
    settings.map_location_source =
        D1L_MAP_LOCATION_SOURCE_AUTHENTICATED_COMPANION;
    return d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_MAP_LOCATION);
}

esp_err_t d1l_app_model_clear_map_location(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_LOCATION)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    d1l_settings_t settings = {0};
    settings.map_location_set = false;
    settings.map_lat_e7 = 0;
    settings.map_lon_e7 = 0;
    settings.map_location_source = D1L_MAP_LOCATION_SOURCE_UNKNOWN;
    return d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_MAP_LOCATION);
}

esp_err_t d1l_app_model_set_timezone_offset_minutes(
    int16_t offset_minutes)
{
    if (!d1l_time_display_offset_valid(offset_minutes)) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_settings_t settings = {
        .timezone_schema_version =
            D1L_TIMEZONE_SETTING_SCHEMA_VERSION,
        .timezone_offset_minutes = offset_minutes,
    };
    return d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_TIMEZONE);
}

esp_err_t d1l_app_model_set_wifi_enabled(bool enabled)
{
    return d1l_connectivity_set_wifi_enabled(enabled);
}

esp_err_t d1l_app_model_wifi_scan(d1l_wifi_scan_result_t *out_result)
{
    return d1l_connectivity_wifi_scan(out_result);
}

esp_err_t d1l_app_model_wifi_connect(void)
{
    return d1l_connectivity_wifi_connect();
}

esp_err_t d1l_app_model_wifi_disconnect(void)
{
    return d1l_connectivity_wifi_disconnect();
}

esp_err_t d1l_app_model_save_wifi_profile(const char *ssid, const char *password)
{
    return d1l_connectivity_save_wifi_profile(ssid, password);
}

esp_err_t d1l_app_model_select_wifi_profile(uint8_t profile_index)
{
    return d1l_connectivity_select_wifi_profile(profile_index);
}

esp_err_t d1l_app_model_delete_wifi_profile(uint8_t profile_index)
{
    return d1l_connectivity_delete_wifi_profile(profile_index);
}

esp_err_t d1l_app_model_clear_wifi_profile(void)
{
    return d1l_connectivity_clear_wifi_profile();
}

esp_err_t d1l_app_model_set_ble_enabled(bool enabled)
{
    return d1l_connectivity_set_ble_enabled(enabled);
}

esp_err_t d1l_app_model_ble_begin_pairing(void)
{
    return d1l_connectivity_ble_begin_pairing();
}

esp_err_t d1l_app_model_ble_forget_peer(void)
{
    return d1l_connectivity_ble_forget_peer();
}

void d1l_app_model_current_radio_profile(d1l_app_radio_profile_edit_t *profile)
{
    radio_edit_from_settings(NULL, profile);
}

void d1l_app_model_default_radio_profile(d1l_app_radio_profile_edit_t *profile)
{
    if (!profile) {
        return;
    }
    const d1l_radio_profile_t *defaults = d1l_radio_profile_uscan_default();
    profile->frequency_hz = defaults->frequency_hz;
    profile->bandwidth_tenths_khz = (uint16_t)((defaults->bandwidth_khz * 10.0f) + 0.5f);
    profile->spreading_factor = defaults->spreading_factor;
    profile->coding_rate = defaults->coding_rate;
    profile->tx_power_dbm = defaults->tx_power_dbm;
    profile->rx_boost = defaults->rx_boost;
}

esp_err_t d1l_app_model_save_radio_profile(const d1l_app_radio_profile_edit_t *profile)
{
    if (!valid_radio_edit(profile)) {
        return ESP_ERR_INVALID_ARG;
    }

    d1l_settings_t settings = {0};
    settings.frequency_hz = profile->frequency_hz;
    settings.bandwidth_tenths_khz = profile->bandwidth_tenths_khz;
    settings.spreading_factor = profile->spreading_factor;
    settings.coding_rate = profile->coding_rate;
    settings.tx_power_dbm = profile->tx_power_dbm;
    settings.rx_boost = profile->rx_boost;
    settings.tcxo_mode = D1L_TCXO_NONE;
    return d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_RADIO_PROFILE);
}

esp_err_t d1l_app_model_complete_onboarding(const char *node_name)
{
    if (!d1l_settings_node_name_valid(node_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    d1l_settings_t before = {0};
    esp_err_t ret = d1l_settings_public_snapshot(&before);
    if (ret != ESP_OK) {
        return ret;
    }
    const bool first_completion = !before.onboarding_complete;
    if (!first_completion) {
        return d1l_settings_complete_onboarding(
            node_name, before.wifi_enabled,
            before.ble_companion_enabled, before.observer_enabled);
    }
    ret = d1l_channel_store_seed_onboarding_defaults();
    if (ret != ESP_OK) {
        return ret;
    }
    d1l_meshcore_service_init();
    ret = d1l_meshcore_service_ensure_identity();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = d1l_settings_complete_onboarding(
        node_name, before.wifi_enabled,
        before.ble_companion_enabled, before.observer_enabled);
    if (ret != ESP_OK) {
        return ret;
    }
    /*
     * START_RX enters the owner queue before the synchronous advert request,
     * so the first signed flood advert cannot run ahead of RX readiness.
     */
    ret = d1l_meshcore_service_start_rx_async();
    if (ret != ESP_OK) {
        return ret;
    }
    return d1l_meshcore_service_request_boot_advert(true);
}

esp_err_t d1l_app_model_reset_onboarding(void)
{
    return d1l_settings_reset_onboarding();
}
