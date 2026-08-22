#pragma once

#include <stdbool.h>
#include <stdint.h>

#define D1L_UI_CONNECTIVITY_TEXT_LEN 96U
#define D1L_UI_CONNECTIVITY_SSID_LEN 33U
#define D1L_UI_CONNECTIVITY_LABEL_LEN 32U

typedef struct {
    bool build_enabled;
    bool enabled;
    bool connected;
    bool profile_saved;
    bool password_saved;
    uint8_t profile_count;
    uint8_t active_profile;
    bool scan_loaded;
    const char *state;
    const char *ip;
    const char *last_error;
    const char *ssid;
    const char *scan_reason;
    const char *strongest_ssid;
    int8_t rssi_dbm;
    uint8_t channel;
    uint16_t scan_returned_count;
    uint16_t scan_total_count;
} d1l_ui_wifi_view_input_t;

typedef struct {
    char state_line[D1L_UI_CONNECTIVITY_TEXT_LEN];
    char link_line[D1L_UI_CONNECTIVITY_TEXT_LEN];
    char profile_line[D1L_UI_CONNECTIVITY_TEXT_LEN];
    char scan_line[D1L_UI_CONNECTIVITY_TEXT_LEN];
    char ssid[D1L_UI_CONNECTIVITY_SSID_LEN];
    char toggle_label[D1L_UI_CONNECTIVITY_LABEL_LEN];
    char password_placeholder[D1L_UI_CONNECTIVITY_LABEL_LEN];
    uint32_t state_color;
    bool controls_available;
} d1l_ui_wifi_view_model_t;

typedef struct {
    bool build_enabled;
    bool transport_supported;
    bool companion_enabled;
    bool peer_known;
    bool protocol_running;
    bool protocol_ready;
    uint32_t pairing_passkey;
    const char *state;
} d1l_ui_ble_view_input_t;

typedef struct {
    char state_line[D1L_UI_CONNECTIVITY_TEXT_LEN];
    char purpose[D1L_UI_CONNECTIVITY_TEXT_LEN];
    char runtime_note[D1L_UI_CONNECTIVITY_TEXT_LEN];
    char toggle_label[D1L_UI_CONNECTIVITY_LABEL_LEN];
    char production_note[D1L_UI_CONNECTIVITY_TEXT_LEN];
    uint32_t state_color;
    uint32_t pairing_passkey;
    bool controls_available;
    bool pairing_active;
    bool pairing_available;
    bool forget_available;
} d1l_ui_ble_view_model_t;

void d1l_ui_connectivity_wifi_view(const d1l_ui_wifi_view_input_t *input,
                                   d1l_ui_wifi_view_model_t *out_view);
void d1l_ui_connectivity_ble_view(const d1l_ui_ble_view_input_t *input,
                                  d1l_ui_ble_view_model_t *out_view);
