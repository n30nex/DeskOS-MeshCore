#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_home_view.h"

static void test_default_view_is_bounded_and_truthful(void)
{
    d1l_ui_home_view_input_t input = {0};
    d1l_ui_home_view_model_t view;
    d1l_ui_home_view(&input, &view);

    assert(strcmp(view.messages_status, "All caught up") == 0);
    assert(strcmp(view.network_status, "0 contacts | 0 nearby") == 0);
    assert(strcmp(view.map_status, "Set a location") == 0);
    assert(strcmp(view.more_status, "0 packets captured") == 0);
    assert(strcmp(view.mesh_value, "Starting") == 0);
    assert(strcmp(view.time_value, "Syncing") == 0);
    assert(strcmp(view.wifi_value, "Off") == 0);
    assert(strcmp(view.ble_value, "Unavailable") == 0);
    assert(strcmp(view.sd_value, "fallback") == 0);
    assert(strcmp(view.sd_compact_value, "Internal") == 0);
    assert(strcmp(view.attention_value, "OK") == 0);
    assert(view.messages_status_color == 0x5EEAD4U);
    assert(view.map_status_color == 0xFBBF24U);
    assert(!view.storage_needs_attention);
}

static void test_ready_view_owns_all_rendered_strings(void)
{
    d1l_ui_home_view_input_t input = {
        .public_unread_count = 2U,
        .dm_unread_count = 3U,
        .contact_count = 4U,
        .node_count = 5U,
        .packet_count = 1U,
        .map_location_set = true,
        .map_tile_cache_ready = true,
        .mesh_state = "ready",
        .radio_ready = true,
        .radio_applied = true,
        .wifi_connected = true,
        .wifi_enabled = true,
        .ble_build_enabled = true,
        .ble_transport_supported = true,
        .ble_companion_enabled = true,
        .time_available = true,
        .time_label = "12:34",
        .storage_data_enabled = true,
        .storage_sd_state = "fat32_ready",
    };
    d1l_ui_home_view_model_t view;
    d1l_ui_home_view(&input, &view);

    assert(strcmp(view.messages_status, "5 unread") == 0);
    assert(strcmp(view.network_status, "4 contacts | 5 nearby") == 0);
    assert(strcmp(view.map_status, "Ready to open") == 0);
    assert(strcmp(view.more_status, "1 packet captured") == 0);
    assert(strcmp(view.mesh_value, "Ready") == 0);
    assert(strcmp(view.time_value, "12:34") == 0);
    assert(strcmp(view.wifi_value, "Connected") == 0);
    assert(strcmp(view.ble_value, "On") == 0);
    assert(strcmp(view.sd_value, "ready") == 0);
    assert(strcmp(view.sd_compact_value, "Ready") == 0);
    assert(strcmp(view.attention_value, "OK") == 0);
    assert(view.sd_value_color == 0x5EEAD4U);
}

static void test_storage_and_map_fail_closed_states(void)
{
    d1l_ui_home_view_input_t input = {
        .map_location_set = true,
        .storage_setup_action = "insert_card",
    };
    d1l_ui_home_view_model_t view;
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.sd_value, "no card") == 0);
    assert(strcmp(view.sd_compact_value, "No card") == 0);
    assert(strcmp(view.map_status, "Insert SD") == 0);
    assert(strcmp(view.attention_value, "Notice") == 0);

    input.storage_setup_action = "inspect_rp2040_sd_mount_error_firmware_path";
    input.storage_retained_backup_degraded = true;
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.sd_value, "storage issue") == 0);
    assert(strcmp(view.sd_compact_value, "Check") == 0);
    assert(strcmp(view.map_status, "Check SD") == 0);
    assert(view.storage_needs_attention);
    assert(view.attention_required);
    assert(strcmp(view.attention_value, "Check") == 0);
    assert(view.sd_value_color == 0xF87171U);

    input.storage_setup_action = NULL;
    input.storage_retained_backup_degraded = false;
    input.storage_sd_needs_fat32 = true;
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.map_status, "Needs FAT32") == 0);
}

static void test_connecting_unavailable_and_mesh_error_states_are_explicit(void)
{
    d1l_ui_home_view_input_t input = {
        .mesh_state = "radio_error",
        .wifi_enabled = true,
        .wifi_connecting = true,
        .ble_build_enabled = true,
        .ble_transport_supported = false,
        .ble_companion_enabled = true,
        .radio_apply_pending = true,
        .storage_setup_required = true,
        .storage_sd_needs_fat32 = true,
    };
    d1l_ui_home_view_model_t view;
    d1l_ui_home_view(&input, &view);

    assert(strcmp(view.mesh_value, "Error") == 0);
    assert(view.mesh_value_color == 0xF87171U);
    assert(strcmp(view.wifi_value, "Connecting") == 0);
    assert(view.wifi_value_color == 0xFBBF24U);
    assert(strcmp(view.ble_value, "Unavailable") == 0);
    assert(view.ble_value_color == 0x8EA0AEU);
    assert(strcmp(view.sd_compact_value, "Setup") == 0);
    assert(strcmp(view.attention_value, "Check") == 0);
    assert(view.attention_required);

    input.mesh_state = "starting";
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.mesh_value, "Applying") == 0);
    assert(strcmp(view.attention_value, "Notice") == 0);
    assert(!view.attention_required);
}

static void test_unknown_mesh_and_storage_reconnect_fail_closed(void)
{
    d1l_ui_home_view_input_t input = {
        .mesh_state = "not_ready",
        .radio_ready = true,
        .radio_applied = true,
        .storage_setup_action = "wait_for_storage_reconnect",
        .storage_data_enabled = true,
    };
    d1l_ui_home_view_model_t view;
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.mesh_value, "Starting") == 0);
    assert(view.mesh_value_color == 0xFBBF24U);
    assert(strcmp(view.sd_value, "reconnecting") == 0);
    assert(strcmp(view.sd_compact_value, "Reconnect") == 0);
    assert(view.sd_value_color == 0xFBBF24U);
    assert(strcmp(view.attention_value, "Notice") == 0);

    input.mesh_state = "unready";
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.mesh_value, "Starting") == 0);

    input.mesh_state = "listening";
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.mesh_value, "Starting") == 0);

    input.mesh_state = "waiting_for_radio";
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.mesh_value, "Offline") == 0);
    assert(view.mesh_value_color == 0x8EA0AEU);
}

static void test_tx_busy_requires_ready_radio_and_stays_healthy(void)
{
    d1l_ui_home_view_input_t input = {
        .mesh_state = "tx_busy",
        .radio_ready = true,
        .radio_applied = true,
    };
    d1l_ui_home_view_model_t view;
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.mesh_value, "Busy") == 0);
    assert(view.mesh_value_color == 0x5EEAD4U);
    assert(!view.mesh_needs_attention);
    assert(!view.attention_required);
    assert(strcmp(view.attention_value, "OK") == 0);

    input.radio_ready = false;
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.mesh_value, "Starting") == 0);
    assert(view.mesh_value_color == 0xFBBF24U);

    input.radio_ready = true;
    input.radio_applied = false;
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.mesh_value, "Starting") == 0);

    input.radio_ready = false;
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.mesh_value, "Starting") == 0);
}

static void test_unread_count_saturates_instead_of_wrapping(void)
{
    d1l_ui_home_view_input_t input = {
        .public_unread_count = SIZE_MAX,
        .dm_unread_count = 1U,
    };
    d1l_ui_home_view_model_t view;
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.messages_status, "All caught up") != 0);
    assert(strstr(view.messages_status, "unread") != NULL);
}

static void test_muted_unread_stays_separate_from_attention_count(void)
{
    d1l_ui_home_view_input_t input = {
        .public_unread_count = 2U,
        .dm_unread_count = 1U,
        .muted_dm_unread_count = 4U,
    };
    d1l_ui_home_view_model_t view;
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.messages_status, "3 unread + 4 excluded") == 0);
    assert(view.messages_status_color == 0xFBBF24U);

    input.public_unread_count = 0U;
    input.dm_unread_count = 0U;
    d1l_ui_home_view(&input, &view);
    assert(strcmp(view.messages_status, "4 excluded") == 0);
    assert(view.messages_status_color == 0x8EA0AEU);
}

static void test_release_profile_strips_hidden_sd_failures(void)
{
    d1l_ui_home_view_input_t input = {
        .map_location_set = true,
        .map_tile_cache_ready = true,
        .wifi_connected = true,
        .wifi_enabled = true,
        .wifi_connecting = true,
        .ble_build_enabled = true,
        .ble_transport_supported = true,
        .ble_companion_enabled = true,
        .storage_retained_sd_degraded = true,
        .storage_sd_data_root_ready = true,
        .storage_setup_required = true,
        .storage_sd_needs_fat32 = true,
        .storage_sd_state = "error",
        .storage_setup_action =
            "inspect_rp2040_sd_mount_error_firmware_path",
    };
    d1l_ui_home_view_apply_release_profile(&input);
    d1l_ui_home_view_model_t view;
    d1l_ui_home_view(&input, &view);
#if EXPECT_CORE
    assert(!input.wifi_connected);
    assert(!input.wifi_connecting);
    assert(!input.ble_build_enabled);
    assert(!input.map_location_set);
    assert(!input.storage_retained_sd_degraded);
    assert(!input.storage_sd_data_root_ready);
    assert(strcmp(view.sd_value, "internal") == 0);
    assert(strcmp(view.sd_compact_value, "Internal") == 0);
    assert(view.sd_value_color == 0x8EA0AEU);
    assert(!view.storage_needs_attention);
    assert(strcmp(view.attention_value, "OK") == 0);
#else
    assert(input.wifi_connected);
    assert(input.ble_build_enabled);
    assert(input.map_location_set);
    assert(input.storage_retained_sd_degraded);
    assert(strcmp(view.sd_compact_value, "Check") == 0);
    assert(view.sd_value_color == 0xF87171U);
    assert(view.storage_needs_attention);
#endif

    input.storage_retained_backup_degraded = true;
    d1l_ui_home_view(&input, &view);
    assert(view.sd_value_color == 0xFBBF24U ||
           view.sd_value_color == 0xF87171U);
}

int main(void)
{
    test_default_view_is_bounded_and_truthful();
    test_ready_view_owns_all_rendered_strings();
    test_storage_and_map_fail_closed_states();
    test_unread_count_saturates_instead_of_wrapping();
    test_muted_unread_stays_separate_from_attention_count();
    test_release_profile_strips_hidden_sd_failures();
    test_connecting_unavailable_and_mesh_error_states_are_explicit();
    test_unknown_mesh_and_storage_reconnect_fail_closed();
    test_tx_busy_requires_ready_radio_and_stays_healthy();
    puts("native UI home view model: ok");
    return 0;
}
