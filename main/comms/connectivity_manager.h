#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "app/settings_model.h"

#define D1L_WIFI_SCAN_MAX 8U

typedef struct {
    char ssid[D1L_WIFI_SSID_LEN];
    int8_t rssi_dbm;
    uint8_t channel;
    const char *auth;
} d1l_wifi_scan_ap_t;

typedef struct {
    bool scan_started;
    bool scan_supported;
    bool truncated;
    uint16_t total_count;
    uint16_t returned_count;
    const char *reason;
    esp_err_t last_error;
    d1l_wifi_scan_ap_t aps[D1L_WIFI_SCAN_MAX];
} d1l_wifi_scan_result_t;

typedef struct {
    bool usb_console_ready;
    bool wifi_enabled_setting;
    bool ble_companion_enabled_setting;
    bool observer_enabled_setting;
    bool wifi_build_enabled;
    bool ble_build_enabled;
    bool wifi_scan_supported;
    bool wifi_stack_active;
    bool wifi_connected;
    bool wifi_connecting;
    bool wifi_retry_scheduled;
    bool wifi_user_cancelled;
    bool wifi_safe_mode;
    bool wifi_boot_guard_ready;
    bool wifi_boot_guard_recovered;
    bool wifi_crash_loop_detected;
    bool ble_stack_active;
    bool ble_peer_known;
    bool ble_protocol_running;
    bool ble_protocol_ready;
    bool wifi_profile_saved;
    bool wifi_password_saved;
    const char *wifi_state;
    const char *ble_state;
    char wifi_ssid[D1L_WIFI_SSID_LEN];
    const char *wifi_ip;
    int8_t wifi_rssi_dbm;
    uint8_t wifi_channel;
    uint8_t wifi_retry_attempt;
    uint8_t wifi_consecutive_crash_boots;
    uint16_t wifi_last_disconnect_reason;
    uint32_t wifi_retry_delay_ms;
    uint32_t ble_pairing_passkey;
    uint32_t ble_protocol_command_count;
    uint32_t ble_protocol_response_count;
    uint32_t ble_protocol_unsupported_count;
    uint32_t ble_protocol_malformed_count;
    uint32_t wifi_retry_task_stack_high_water_bytes;
    const char *wifi_last_error;
    const char *wifi_last_failure_class;
    const char *wifi_last_active_subsystem;
    const char *wifi_boot_guard_error;
    const char *coexistence_policy;
} d1l_connectivity_status_t;

esp_err_t d1l_connectivity_init(void);
esp_err_t d1l_connectivity_prepare_reboot(void);
void d1l_connectivity_status(d1l_connectivity_status_t *out_status);
esp_err_t d1l_connectivity_wifi_scan(d1l_wifi_scan_result_t *out_result);
esp_err_t d1l_connectivity_wifi_connect(void);
esp_err_t d1l_connectivity_wifi_disconnect(void);
esp_err_t d1l_connectivity_set_wifi_enabled(bool enabled);
esp_err_t d1l_connectivity_set_ble_enabled(bool enabled);
esp_err_t d1l_connectivity_ble_begin_pairing(void);
esp_err_t d1l_connectivity_ble_forget_peer(void);
esp_err_t d1l_connectivity_save_wifi_profile(const char *ssid, const char *password);
esp_err_t d1l_connectivity_clear_wifi_profile(void);
