#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_settings.h"

#ifndef EXPECT_BLE
#error "EXPECT_BLE must bind the release-profile case"
#endif

_Static_assert(sizeof(d1l_ui_more_view_model_t) <= D1L_UI_MORE_VIEW_MODEL_MAX_BYTES,
               "More view model size budget regressed");
_Static_assert(sizeof(d1l_ui_settings_controller_t) <=
                   D1L_UI_SETTINGS_CONTROLLER_MAX_BYTES,
               "More controller size budget regressed");

static const d1l_ui_more_item_view_t *item(
    const d1l_ui_more_view_model_t *view,
    d1l_ui_more_category_t category,
    size_t index)
{
    assert(view != NULL);
    assert(category >= D1L_UI_MORE_CATEGORY_TOOLS);
    assert(category < D1L_UI_MORE_CATEGORY_COUNT);
    assert(index < view->categories[category].item_count);
    return &view->categories[category].items[index];
}

static void test_default_view_is_owned_bounded_and_truthful(void)
{
    d1l_ui_more_view_input_t input = {.firmware_version = "1.2.3"};
    d1l_ui_more_view_model_t view;
    assert(d1l_ui_more_view(&input, &view));
    assert(d1l_ui_more_view_model_is_valid(&view));
    assert(strcmp(view.title, "Tools") == 0);
    assert(strcmp(view.subtitle, "Settings and utilities") == 0);
    assert(view.category_count == D1L_UI_MORE_CATEGORY_COUNT);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_TOOLS, 0)->status,
                  "0 saved") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_CONNECTIONS, 0)->status,
                  "Unavailable") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_CONNECTIONS, 1)->status,
                  "Unavailable") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_CONNECTIONS, 2)->status,
                  "Not configured") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_STORAGE_MAPS, 0)->status,
                  "Internal storage") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_STORAGE_MAPS, 1)->status,
                  "Set location") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_DEVICE, 0)->status,
                  "Time setting unavailable") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_DEVICE, 1)->status,
                  "0 unread") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_DEVICE, 2)->status,
                  "Not set") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_ADVANCED, 0)->status,
                  "Needs setup") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_SUPPORT, 0)->status,
                  "Version 1.2.3") == 0);
    assert(!item(&view, D1L_UI_MORE_CATEGORY_SUPPORT, 0)->actionable);
}

static void test_connectivity_and_ready_storage_states(void)
{
    d1l_ui_more_view_input_t input = {
        .packet_count = 19U,
        .wifi_build_enabled = true,
        .wifi_enabled = true,
        .wifi_connected = true,
        .ble_build_enabled = true,
        .ble_transport_supported = true,
        .ble_companion_enabled = true,
        .observer_state = "connected",
        .radio_ready = true,
        .storage_sd_present = true,
        .storage_sd_data_root_ready = true,
        .storage_data_enabled = true,
        .map_location_set = true,
        .map_tile_cache_ready = true,
        .map_tile_render_supported = true,
        .identity_ready = true,
        .notification_unread_count = 4U,
        .time_available = true,
        .time_approximate = true,
        .timezone_settings_ready = true,
        .time_label = "~08:42",
        .timezone_label = "UTC-04:00",
        .firmware_version = "9.8.7",
    };
    d1l_ui_more_view_model_t view;
    assert(d1l_ui_more_view(&input, &view));
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_TOOLS, 0)->status,
                  "19 saved") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_CONNECTIONS, 0)->status,
                  "Connected") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_CONNECTIONS, 1)->status,
                  "On") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_CONNECTIONS, 2)->status,
                  "connected") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_STORAGE_MAPS, 0)->status,
                  "Ready") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_STORAGE_MAPS, 1)->status,
                  "Ready") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_DEVICE, 0)->status,
                  "UTC-04:00 / ~08:42") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_DEVICE, 1)->status,
                  "4 unread") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_DEVICE, 2)->status,
                  "Ready") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_ADVANCED, 0)->status,
                  "Ready") == 0);

    input.wifi_connected = false;
    input.wifi_connecting = true;
    input.radio_ready = false;
    input.radio_apply_pending = true;
    assert(d1l_ui_more_view(&input, &view));
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_CONNECTIONS, 0)->status,
                  "Connecting") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_ADVANCED, 0)->status,
                  "Applying") == 0);
    assert(strcmp(item(&view, D1L_UI_MORE_CATEGORY_STORAGE_MAPS, 1)->status,
                  "Needs Wi-Fi") == 0);
}

static void test_storage_faults_override_stale_ready_flags(void)
{
    d1l_ui_more_view_input_t input = {
        .storage_sd_present = true,
        .storage_sd_data_root_ready = true,
        .storage_data_enabled = true,
        .storage_retained_backup_degraded = true,
        .storage_sd_state = "bridge_reported",
        .map_location_set = true,
        .firmware_version = "test",
    };
    d1l_ui_more_view_model_t view;
    assert(d1l_ui_more_view(&input, &view));
    const d1l_ui_more_category_view_t *storage =
        &view.categories[D1L_UI_MORE_CATEGORY_STORAGE_MAPS];
    assert(strcmp(storage->summary, "Storage needs attention") == 0);
    assert(storage->warning);
    assert(strcmp(storage->items[0].status, "Needs attention") == 0);
    assert(storage->items[0].warning);
    assert(strcmp(storage->items[1].status, "Check SD") == 0);

    input.storage_retained_backup_degraded = false;
    input.storage_sd_state = NULL;
    input.storage_setup_action = "wait_for_storage_reconnect";
    assert(d1l_ui_more_view(&input, &view));
    assert(strcmp(storage->summary, "SD reconnecting") == 0);
    assert(strcmp(storage->items[0].status, "Reconnecting") == 0);
}

static void test_invalid_and_long_inputs_fail_closed_or_truncate(void)
{
    const char long_version[] =
        "release-candidate-with-an-intentionally-very-long-provenance-suffix-0123456789";
    d1l_ui_more_view_input_t input = {.firmware_version = long_version};
    d1l_ui_more_view_model_t view;
    assert(d1l_ui_more_view(&input, &view));
    const d1l_ui_more_item_view_t *about =
        item(&view, D1L_UI_MORE_CATEGORY_SUPPORT, 0);
    assert(about->status[sizeof(about->status) - 1U] == '\0');
    assert(strlen(about->status) < sizeof(about->status));

    view.category_count = D1L_UI_MORE_CATEGORY_COUNT - 1U;
    assert(!d1l_ui_more_view_model_is_valid(&view));
    assert(!d1l_ui_more_view(NULL, &view));
    assert(!d1l_ui_more_view(&input, NULL));
}

static void test_unterminated_fixed_buffers_fail_closed(void)
{
    d1l_ui_more_view_input_t input = {.firmware_version = "test"};
    d1l_ui_more_view_model_t view;

    assert(d1l_ui_more_view(&input, &view));
    memset(view.title, 'X', sizeof(view.title));
    assert(!d1l_ui_more_view_model_is_valid(&view));

    assert(d1l_ui_more_view(&input, &view));
    memset(view.categories[D1L_UI_MORE_CATEGORY_CONNECTIONS].summary, 'X',
           sizeof(view.categories[D1L_UI_MORE_CATEGORY_CONNECTIONS].summary));
    assert(!d1l_ui_more_view_model_is_valid(&view));

    assert(d1l_ui_more_view(&input, &view));
    memset(view.categories[D1L_UI_MORE_CATEGORY_STORAGE_MAPS].items[0].status, 'X',
           sizeof(view.categories[D1L_UI_MORE_CATEGORY_STORAGE_MAPS].items[0].status));
    assert(!d1l_ui_more_view_model_is_valid(&view));

    assert(d1l_ui_more_view(&input, &view));
    memset(view.categories[D1L_UI_MORE_CATEGORY_SUPPORT].items[2].title, 'X',
           sizeof(view.categories[D1L_UI_MORE_CATEGORY_SUPPORT].items[2].title));
    assert(!d1l_ui_more_view_model_is_valid(&view));
}

static void test_release_profile_strips_hidden_sd_failures(void)
{
    d1l_ui_more_view_input_t input = {
        .wifi_build_enabled = true,
        .wifi_enabled = true,
        .wifi_connected = true,
        .wifi_connecting = true,
        .ble_build_enabled = true,
        .ble_transport_supported = true,
        .ble_companion_enabled = true,
        .storage_sd_present = true,
        .storage_sd_data_root_ready = true,
        .storage_sd_needs_fat32 = true,
        .storage_setup_required = true,
        .storage_retained_sd_degraded = true,
        .map_location_set = true,
        .map_tile_cache_ready = true,
        .map_tile_render_supported = true,
        .storage_sd_state = "error",
        .storage_setup_action =
            "inspect_rp2040_sd_mount_error_firmware_path",
        .firmware_version = "test",
    };
    d1l_ui_more_view_apply_release_profile(&input);
    d1l_ui_more_view_model_t view;
    assert(d1l_ui_more_view(&input, &view));
    const d1l_ui_more_item_view_t *storage =
        item(&view, D1L_UI_MORE_CATEGORY_STORAGE_MAPS, 0U);
    assert(input.wifi_build_enabled);
    assert(input.ble_build_enabled == (EXPECT_BLE != 0));
    assert(input.ble_transport_supported == (EXPECT_BLE != 0));
    assert(input.ble_companion_enabled == (EXPECT_BLE != 0));
    assert(input.map_location_set);
    assert(input.storage_sd_present);
    assert(input.storage_retained_sd_degraded);
    assert(strcmp(storage->status, "Needs attention") == 0);
    assert(storage->accent == 0xF87171U);
    assert(storage->warning);

    input.storage_retained_backup_degraded = true;
    assert(d1l_ui_more_view(&input, &view));
    storage = item(&view, D1L_UI_MORE_CATEGORY_STORAGE_MAPS, 0U);
    assert(strcmp(storage->status, "Needs attention") == 0);
    assert(storage->accent == 0xF87171U);
    assert(storage->warning);
}

int main(void)
{
    test_default_view_is_owned_bounded_and_truthful();
    test_connectivity_and_ready_storage_states();
    test_storage_faults_override_stale_ready_flags();
    test_invalid_and_long_inputs_fail_closed_or_truncate();
    test_unterminated_fixed_buffers_fail_closed();
    test_release_profile_strips_hidden_sd_failures();
    puts("native UI More view model: ok");
    return 0;
}
