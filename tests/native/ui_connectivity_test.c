#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_connectivity.h"

static void test_wifi_truth_models_offline_connected_and_scan_states(void)
{
    d1l_ui_wifi_view_model_t view;
    d1l_ui_wifi_view_input_t input = {
        .last_error = "not_started",
    };
    d1l_ui_connectivity_wifi_view(&input, &view);
    assert(strcmp(view.state_line, "Wi-Fi off  |  unavailable") == 0);
    assert(strcmp(view.link_line, "Last not_started") == 0);
    assert(strcmp(view.profile_line,
                  "Profile 0/0  not saved  password open/empty") == 0);
    assert(strcmp(view.scan_line,
                  "Wi-Fi is unavailable in this build") == 0);
    assert(strcmp(view.toggle_label, "Enable") == 0);
    assert(!view.controls_available);

    input.enabled = true;
    d1l_ui_connectivity_wifi_view(&input, &view);
    assert(!view.controls_available);
    assert(strcmp(view.toggle_label, "Enable") == 0);
    assert(view.state_color == 0xFBBF24U);

    input = (d1l_ui_wifi_view_input_t) {
        .build_enabled = true,
        .enabled = true,
        .connected = true,
        .profile_saved = true,
        .password_saved = true,
        .profile_count = 1U,
        .active_profile = 0U,
        .scan_loaded = true,
        .state = "ready",
        .ip = "192.0.2.5",
        .ssid = "Mesh Lab",
        .scan_reason = "ok",
        .strongest_ssid = "Mesh Lab",
        .rssi_dbm = -42,
        .channel = 6U,
        .scan_returned_count = 3U,
        .scan_total_count = 4U,
    };
    d1l_ui_connectivity_wifi_view(&input, &view);
    assert(view.controls_available);
    assert(strcmp(view.link_line, "IP 192.0.2.5  RSSI -42  ch 6") == 0);
    assert(strcmp(view.profile_line,
                  "Profile 1/1  Mesh Lab  password saved") == 0);
    assert(strcmp(view.scan_line,
                  "Scan ok  3/4 networks  strongest Mesh Lab") == 0);
    assert(strcmp(view.toggle_label, "Disable") == 0);
    assert(strcmp(view.password_placeholder, "Saved; enter to replace") == 0);

    input.rssi_dbm = 0;
    d1l_ui_connectivity_wifi_view(&input, &view);
    assert(strcmp(view.link_line, "IP 192.0.2.5  RSSI --  ch 6") == 0);
}

static void test_ble_never_implies_unmeasured_transport(void)
{
    d1l_ui_ble_view_model_t view;
    d1l_ui_ble_view_input_t input = {.state = "off"};
    d1l_ui_connectivity_ble_view(&input, &view);
    assert(!view.controls_available);
    assert(strstr(view.purpose, "unavailable") != NULL);
    assert(strstr(view.runtime_note, "Bluetooth pairing") != NULL);

    input = (d1l_ui_ble_view_input_t) {
        .build_enabled = false,
        .transport_supported = true,
        .companion_enabled = true,
        .state = "configured",
    };
    d1l_ui_connectivity_ble_view(&input, &view);
    assert(!view.controls_available);
    assert(strcmp(view.toggle_label, "Enable") == 0);
    assert(view.state_color == 0xFBBF24U);
    assert(strstr(view.purpose, "unavailable") != NULL);

    input = (d1l_ui_ble_view_input_t) {
        .build_enabled = true,
        .transport_supported = true,
        .companion_enabled = true,
        .state = "ready",
    };
    d1l_ui_connectivity_ble_view(&input, &view);
    assert(view.controls_available);
    assert(strcmp(view.toggle_label, "Disable") == 0);
    assert(strstr(view.purpose, "available") != NULL);

    input.state = "pairing";
    input.pairing_passkey = 123456U;
    d1l_ui_connectivity_ble_view(&input, &view);
    assert(view.pairing_active);
    assert(view.pairing_passkey == 123456U);
    assert(!view.pairing_available);
    assert(strstr(view.purpose, "Pairing request detected") != NULL);
    assert(strcmp(view.runtime_note,
                  "Enter this PIN in the MeshCore app.") == 0);
    assert(strcmp(view.production_note,
                  "Bluetooth mode is active; Wi-Fi is off.") == 0);
}

int main(void)
{
    test_wifi_truth_models_offline_connected_and_scan_states();
    test_ble_never_implies_unmeasured_transport();
    puts("native UI connectivity view model: ok");
    return 0;
}
