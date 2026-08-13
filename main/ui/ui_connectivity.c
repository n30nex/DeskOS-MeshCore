#include "ui_connectivity.h"

#include <stdio.h>
#include <string.h>

static const char *safe_text(const char *value, const char *fallback)
{
    return value && value[0] ? value : fallback;
}

void d1l_ui_connectivity_wifi_view(const d1l_ui_wifi_view_input_t *input,
                                   d1l_ui_wifi_view_model_t *out_view)
{
    if (!input || !out_view) {
        return;
    }
    memset(out_view, 0, sizeof(*out_view));
    snprintf(out_view->state_line, sizeof(out_view->state_line),
             "Wi-Fi %s  |  %s", safe_text(input->state, "off"),
             input->build_enabled ? "available" : "unavailable");
    if (input->connected && input->ip && input->ip[0]) {
        if (input->rssi_dbm < 0) {
            snprintf(out_view->link_line, sizeof(out_view->link_line),
                     "IP %s  RSSI %d  ch %u", input->ip, input->rssi_dbm,
                     (unsigned)input->channel);
        } else {
            snprintf(out_view->link_line, sizeof(out_view->link_line),
                     "IP %s  RSSI --  ch %u", input->ip,
                     (unsigned)input->channel);
        }
    } else {
        snprintf(out_view->link_line, sizeof(out_view->link_line),
                 "Last %s", safe_text(input->last_error, "none"));
    }
    snprintf(out_view->profile_line, sizeof(out_view->profile_line),
             "Profile %u/%u  %s  password %s",
             input->profile_saved ?
                 (unsigned)input->active_profile + 1U : 0U,
             (unsigned)input->profile_count,
             input->profile_saved ? safe_text(input->ssid, "saved") :
                                    "not saved",
             input->password_saved ? "saved" : "open/empty");
    snprintf(out_view->ssid, sizeof(out_view->ssid), "%s",
             safe_text(input->ssid, ""));
    if (!input->build_enabled) {
        snprintf(out_view->scan_line, sizeof(out_view->scan_line),
                 "Wi-Fi is unavailable in this build");
    } else if (input->scan_loaded) {
        snprintf(out_view->scan_line, sizeof(out_view->scan_line),
                 "Scan %s  %u/%u networks  strongest %s",
                 safe_text(input->scan_reason, "unknown"),
                 (unsigned)input->scan_returned_count,
                 (unsigned)input->scan_total_count,
                 input->scan_returned_count > 0U ?
                     safe_text(input->strongest_ssid, "hidden") : "none");
    } else {
        snprintf(out_view->scan_line, sizeof(out_view->scan_line),
                 "Scan to list nearby 2.4 GHz networks");
    }
    out_view->controls_available = input->build_enabled;
    snprintf(out_view->toggle_label, sizeof(out_view->toggle_label), "%s",
             input->build_enabled && input->enabled ? "Disable" : "Enable");
    snprintf(out_view->password_placeholder,
             sizeof(out_view->password_placeholder), "%s",
             input->password_saved ? "Saved; enter to replace" : "Optional");
    out_view->state_color = input->build_enabled && input->enabled ?
        0x20D9EDU : 0xFBBF24U;
}

void d1l_ui_connectivity_ble_view(const d1l_ui_ble_view_input_t *input,
                                  d1l_ui_ble_view_model_t *out_view)
{
    if (!input || !out_view) {
        return;
    }
    memset(out_view, 0, sizeof(*out_view));
    snprintf(out_view->state_line, sizeof(out_view->state_line),
             "Bluetooth %s  |  %s", safe_text(input->state, "off"),
             input->build_enabled ? "available" : "unavailable");
    const bool runtime_available =
        input->build_enabled && input->transport_supported;
    out_view->controls_available = runtime_available;
    snprintf(out_view->purpose, sizeof(out_view->purpose), "%s",
             runtime_available ?
                 "Bluetooth companion connections are available." :
                 "Bluetooth companion connections are unavailable.");
    if (runtime_available && input->pairing_passkey >= 100000U &&
        input->pairing_passkey <= 999999U &&
        strcmp(safe_text(input->state, "off"), "pairing") == 0) {
        snprintf(out_view->runtime_note, sizeof(out_view->runtime_note),
                 "Enter PIN %06lu on the companion device.",
                 (unsigned long)input->pairing_passkey);
    } else if (runtime_available && input->protocol_ready) {
        snprintf(out_view->runtime_note, sizeof(out_view->runtime_note),
                 "Paired securely and ready to sync.");
    } else if (runtime_available && input->companion_enabled) {
        snprintf(out_view->runtime_note, sizeof(out_view->runtime_note),
                 "Ready to pair securely. Tap Pair to make this device visible.");
    } else {
        snprintf(out_view->runtime_note, sizeof(out_view->runtime_note), "%s",
                 runtime_available ?
                     "Enable Bluetooth, then tap Pair from this screen." :
                     "Bluetooth pairing is unavailable.");
    }
    snprintf(out_view->toggle_label, sizeof(out_view->toggle_label), "%s",
             runtime_available && input->companion_enabled ?
                 "Disable" : "Enable");
    snprintf(out_view->production_note, sizeof(out_view->production_note), "%s",
             runtime_available && input->protocol_running ?
                 "Using the MeshCore connection. Wi-Fi turns off while Bluetooth is active." :
                 "USB stays available for recovery and diagnostics.");
    out_view->pairing_available =
        runtime_available && input->companion_enabled;
    out_view->forget_available =
        runtime_available && input->peer_known;
    out_view->state_color = runtime_available && input->companion_enabled ?
        0x84FF2EU : 0xFBBF24U;
}
