#include "ui_home_view.h"

#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"

static bool text_equals(const char *value, const char *expected)
{
    return value && expected && strcmp(value, expected) == 0;
}

void d1l_ui_home_view_apply_release_profile(
    d1l_ui_home_view_input_t *input)
{
    if (!input) {
        return;
    }
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL)) {
        input->wifi_connected = false;
        input->wifi_enabled = false;
        input->wifi_connecting = false;
    }
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE)) {
        input->ble_build_enabled = false;
        input->ble_transport_supported = false;
        input->ble_companion_enabled = false;
    }
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP)) {
        input->map_location_set = false;
        input->map_tile_cache_ready = false;
    }
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY)) {
        input->storage_retained_sd_degraded = false;
        input->storage_sd_data_root_ready = false;
        input->storage_setup_required = false;
        input->storage_sd_needs_fat32 = false;
        input->storage_sd_state = "internal";
        input->storage_setup_action = "forced_nvs";
    }
}

static bool mesh_needs_attention(const d1l_ui_home_view_input_t *input)
{
    return input && text_equals(input->mesh_state, "radio_error");
}

static bool storage_needs_attention(const d1l_ui_home_view_input_t *input)
{
    if (!input) {
        return false;
    }
    return input->storage_retained_sd_degraded ||
           text_equals(input->storage_sd_state, "error") ||
           text_equals(input->storage_sd_state, "bridge_reported") ||
           text_equals(input->storage_setup_action,
                       "inspect_rp2040_sd_cmd0_firmware_path") ||
           text_equals(input->storage_setup_action,
                       "inspect_rp2040_sd_mount_error_firmware_path");
}

const char *d1l_ui_home_sd_state(const d1l_ui_home_view_input_t *input)
{
    if (!input) {
        return "unknown";
    }
    if (input->storage_retained_backup_degraded &&
        storage_needs_attention(input)) {
        return "storage issue";
    }
    if (input->storage_retained_backup_degraded) {
        return "backup issue";
    }
    if (storage_needs_attention(input)) {
        return "Needs attention";
    }
    if (text_equals(input->storage_setup_action, "wait_for_storage_reconnect")) {
        return "reconnecting";
    }
    if (input->storage_data_enabled || input->storage_sd_data_root_ready) {
        return "ready";
    }
    if (input->storage_setup_required) {
        return "setup";
    }
    if (text_equals(input->storage_setup_action, "bridge_unavailable") ||
        text_equals(input->storage_setup_action, "bridge_protocol_pending")) {
        return "offline";
    }
    if (text_equals(input->storage_setup_action, "insert_card")) {
        return "no card";
    }
    if (text_equals(input->storage_setup_action, "prepare_fat32_on_computer") ||
        text_equals(input->storage_setup_action,
                    "backup_reformat_fat32_on_computer")) {
        return "needs FAT32";
    }
    if (text_equals(input->storage_setup_action, "run_storage_mount") ||
        text_equals(input->storage_setup_action, "wait_for_storage_mount")) {
        return "mounting";
    }
    if (text_equals(input->storage_setup_action, "retry_storage_mount") ||
        text_equals(input->storage_setup_action, "use_nvs_fallback")) {
        return "setup";
    }
    if (text_equals(input->storage_setup_action, "forced_nvs")) {
        return "internal";
    }
    const char *state = input->storage_sd_state;
    if (!state || state[0] == '\0') {
        return "fallback";
    }
    if (strcmp(state, "mount_pending") == 0 || strcmp(state, "checking") == 0) {
        return "mounting";
    }
    if (strcmp(state, "mount_required") == 0) {
        return "mount";
    }
    if (strcmp(state, "no_card") == 0) {
        return "no card";
    }
    if (strcmp(state, "not_fat32_or_unmountable") == 0) {
        return "needs FAT32";
    }
    if (strcmp(state, "creating_deskos_files") == 0) {
        return "preparing";
    }
    if (strcmp(state, "deskos_manifest_invalid") == 0) {
        return "repair";
    }
    if (strcmp(state, "pending_bridge") == 0 ||
        strcmp(state, "protocol_pending") == 0 ||
        strcmp(state, "rp2040_unavailable") == 0 ||
        strcmp(state, "bridge_unavailable") == 0 ||
        strcmp(state, "unsupported") == 0) {
        return "offline";
    }
    if (strcmp(state, "fat32_ready") == 0) {
        return "FAT32 ready";
    }
    return "internal";
}

static const char *map_storage_state(const d1l_ui_home_view_input_t *input)
{
    if (text_equals(input->storage_setup_action, "insert_card")) {
        return "Insert SD";
    }
    if (storage_needs_attention(input)) {
        return "Check SD";
    }
    if (input->storage_sd_needs_fat32) {
        return "Needs FAT32";
    }
    if (strcmp(d1l_ui_home_sd_state(input), "no card") == 0) {
        return "Insert SD";
    }
    return "SD starting";
}

void d1l_ui_home_view(const d1l_ui_home_view_input_t *input,
                      d1l_ui_home_view_model_t *out_view)
{
    if (!input || !out_view) {
        return;
    }
    memset(out_view, 0, sizeof(*out_view));

    const size_t unread_count = input->public_unread_count >
        SIZE_MAX - input->dm_unread_count ? SIZE_MAX :
        input->public_unread_count + input->dm_unread_count;
    if (unread_count > 0U && input->muted_dm_unread_count > 0U) {
        snprintf(out_view->messages_status, sizeof(out_view->messages_status),
                 "%llu unread + %llu excluded",
                 (unsigned long long)unread_count,
                 (unsigned long long)input->muted_dm_unread_count);
    } else if (unread_count > 0U) {
        snprintf(out_view->messages_status, sizeof(out_view->messages_status),
                 "%llu unread", (unsigned long long)unread_count);
    } else if (input->muted_dm_unread_count > 0U) {
        snprintf(out_view->messages_status, sizeof(out_view->messages_status),
                 "%llu excluded",
                 (unsigned long long)input->muted_dm_unread_count);
    } else {
        snprintf(out_view->messages_status, sizeof(out_view->messages_status),
                 "All caught up");
    }
    out_view->messages_status_color = unread_count ? 0xFBBF24U :
        (input->muted_dm_unread_count ? 0x8EA0AEU : 0x5EEAD4U);

    snprintf(out_view->network_status, sizeof(out_view->network_status),
             "%llu contacts | %llu nearby",
             (unsigned long long)input->contact_count,
             (unsigned long long)input->node_count);
    out_view->network_status_color = input->contact_count ? 0x5EEAD4U : 0x8EA0AEU;

    const char *map_status = "Ready to open";
    out_view->map_status_color = 0x5EEAD4U;
    if (!input->map_location_set) {
        map_status = "Set a location";
        out_view->map_status_color = 0xFBBF24U;
    } else if (!input->map_tile_cache_ready) {
        map_status = map_storage_state(input);
        out_view->map_status_color = 0xFBBF24U;
    } else if (!input->wifi_connected) {
        map_status = "Needs Wi-Fi";
        out_view->map_status_color = 0xFBBF24U;
    }
    snprintf(out_view->map_status, sizeof(out_view->map_status), "%s", map_status);

    snprintf(out_view->more_status, sizeof(out_view->more_status),
             "%llu packet%s captured",
             (unsigned long long)input->packet_count,
             input->packet_count == 1U ? "" : "s");
    out_view->more_status_color = input->packet_count ? 0xC4B5FDU : 0x8EA0AEU;

    out_view->mesh_needs_attention = mesh_needs_attention(input);
    const char *mesh_value = "Starting";
    out_view->mesh_value_color = 0xFBBF24U;
    if (out_view->mesh_needs_attention) {
        mesh_value = "Error";
        out_view->mesh_value_color = 0xF87171U;
    } else if (input->radio_apply_pending) {
        mesh_value = "Applying";
    } else if (input->radio_ready && input->radio_applied &&
               text_equals(input->mesh_state, "tx_busy")) {
        mesh_value = "Busy";
        out_view->mesh_value_color = 0x5EEAD4U;
    } else if (input->radio_ready && input->radio_applied &&
               text_equals(input->mesh_state, "ready")) {
        mesh_value = "Ready";
        out_view->mesh_value_color = 0x5EEAD4U;
    } else if (text_equals(input->mesh_state, "waiting_for_radio") ||
               text_equals(input->mesh_state, "offline") ||
               text_equals(input->mesh_state, "unavailable")) {
        mesh_value = "Offline";
        out_view->mesh_value_color = 0x8EA0AEU;
    }
    snprintf(out_view->mesh_value, sizeof(out_view->mesh_value), "%s",
             mesh_value);

    snprintf(out_view->time_value, sizeof(out_view->time_value), "%s",
             input->time_available && input->time_label && input->time_label[0] ?
                 input->time_label : "Syncing");
    snprintf(out_view->wifi_value, sizeof(out_view->wifi_value), "%s",
             input->wifi_connected ? "Connected" :
             (input->wifi_connecting ? "Connecting" :
              (input->wifi_enabled ? "On" : "Off")));
    snprintf(out_view->ble_value, sizeof(out_view->ble_value), "%s",
             !input->ble_build_enabled || !input->ble_transport_supported ?
                 "Unavailable" :
             (input->ble_companion_enabled ? "On" : "Off"));
    snprintf(out_view->sd_value, sizeof(out_view->sd_value), "%s",
             d1l_ui_home_sd_state(input));

    out_view->storage_needs_attention = storage_needs_attention(input);
    out_view->time_value_color = input->time_available ? 0x5EEAD4U : 0x8EA0AEU;
    out_view->wifi_value_color = input->wifi_connected ? 0x5EEAD4U :
        (input->wifi_connecting ? 0xFBBF24U :
         (input->wifi_enabled ? 0xA7F3D0U : 0x8EA0AEU));
    const bool ble_available = input->ble_build_enabled &&
        input->ble_transport_supported;
    out_view->ble_value_color = ble_available && input->ble_companion_enabled ?
        0xA7F3D0U : 0x8EA0AEU;
    const bool sd_notice = input->storage_setup_required ||
        text_equals(out_view->sd_value, "no card") ||
        text_equals(out_view->sd_value, "needs FAT32") ||
        text_equals(out_view->sd_value, "mounting") ||
        text_equals(out_view->sd_value, "reconnecting");
    out_view->sd_value_color = out_view->storage_needs_attention ? 0xF87171U :
        (input->storage_retained_backup_degraded ? 0xFBBF24U :
         (sd_notice ? 0xFBBF24U :
          ((input->storage_data_enabled || input->storage_sd_data_root_ready) ?
               0x5EEAD4U : 0x8EA0AEU)));

    const char *sd_compact = out_view->sd_value;
    if (out_view->storage_needs_attention) {
        sd_compact = "Check";
    } else if (text_equals(out_view->sd_value, "needs FAT32")) {
        sd_compact = "FAT32";
    } else if (text_equals(out_view->sd_value, "storage issue") ||
               text_equals(out_view->sd_value, "backup issue")) {
        sd_compact = "Degraded";
    } else if (text_equals(out_view->sd_value, "ready") ||
               text_equals(out_view->sd_value, "FAT32 ready")) {
        sd_compact = "Ready";
    } else if (text_equals(out_view->sd_value, "no card")) {
        sd_compact = "No card";
    } else if (text_equals(out_view->sd_value, "reconnecting")) {
        sd_compact = "Reconnect";
    } else if (text_equals(out_view->sd_value, "mount") ||
               text_equals(out_view->sd_value, "mounting") ||
               text_equals(out_view->sd_value, "preparing")) {
        sd_compact = "Starting";
    } else if (text_equals(out_view->sd_value, "setup")) {
        sd_compact = "Setup";
    } else if (text_equals(out_view->sd_value, "fallback") ||
               text_equals(out_view->sd_value, "internal")) {
        sd_compact = "Internal";
    } else if (text_equals(out_view->sd_value, "offline")) {
        sd_compact = "Offline";
    } else if (text_equals(out_view->sd_value, "repair")) {
        sd_compact = "Repair";
    }
    snprintf(out_view->sd_compact_value,
             sizeof(out_view->sd_compact_value), "%s", sd_compact);

    out_view->attention_required = out_view->mesh_needs_attention ||
        out_view->storage_needs_attention;
    const bool attention_notice = input->storage_retained_backup_degraded ||
        input->radio_apply_pending || input->wifi_connecting ||
        sd_notice;
    snprintf(out_view->attention_value, sizeof(out_view->attention_value), "%s",
             out_view->attention_required ? "Check" :
             (attention_notice ? "Notice" : "OK"));
    out_view->attention_value_color = out_view->attention_required ? 0xF87171U :
        (attention_notice ? 0xFBBF24U : 0x5EEAD4U);
}
