#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D1L_UI_HOME_STATUS_TEXT_LEN 64U
#define D1L_UI_HOME_DEVICE_TEXT_LEN 32U

typedef struct {
    size_t public_unread_count;
    size_t dm_unread_count;
    size_t muted_dm_unread_count;
    size_t contact_count;
    size_t node_count;
    size_t packet_count;
    bool map_location_set;
    bool map_tile_cache_ready;
    const char *mesh_state;
    bool radio_ready;
    bool radio_applied;
    bool radio_apply_pending;
    bool wifi_connected;
    bool wifi_enabled;
    bool wifi_connecting;
    bool ble_build_enabled;
    bool ble_transport_supported;
    bool ble_companion_enabled;
    bool time_available;
    const char *time_label;
    bool storage_retained_backup_degraded;
    bool storage_retained_sd_degraded;
    bool storage_data_enabled;
    bool storage_sd_data_root_ready;
    bool storage_setup_required;
    bool storage_sd_needs_fat32;
    const char *storage_sd_state;
    const char *storage_setup_action;
} d1l_ui_home_view_input_t;

typedef struct {
    char messages_status[D1L_UI_HOME_STATUS_TEXT_LEN];
    char network_status[D1L_UI_HOME_STATUS_TEXT_LEN];
    char map_status[D1L_UI_HOME_STATUS_TEXT_LEN];
    char more_status[D1L_UI_HOME_STATUS_TEXT_LEN];
    char mesh_value[D1L_UI_HOME_DEVICE_TEXT_LEN];
    char time_value[D1L_UI_HOME_DEVICE_TEXT_LEN];
    char wifi_value[D1L_UI_HOME_DEVICE_TEXT_LEN];
    char ble_value[D1L_UI_HOME_DEVICE_TEXT_LEN];
    char sd_value[D1L_UI_HOME_DEVICE_TEXT_LEN];
    char sd_compact_value[D1L_UI_HOME_DEVICE_TEXT_LEN];
    char attention_value[D1L_UI_HOME_DEVICE_TEXT_LEN];
    uint32_t messages_status_color;
    uint32_t network_status_color;
    uint32_t map_status_color;
    uint32_t more_status_color;
    uint32_t mesh_value_color;
    uint32_t time_value_color;
    uint32_t wifi_value_color;
    uint32_t ble_value_color;
    uint32_t sd_value_color;
    uint32_t attention_value_color;
    bool mesh_needs_attention;
    bool storage_needs_attention;
    bool attention_required;
} d1l_ui_home_view_model_t;

const char *d1l_ui_home_sd_state(const d1l_ui_home_view_input_t *input);
void d1l_ui_home_view_apply_release_profile(
    d1l_ui_home_view_input_t *input);
void d1l_ui_home_view(const d1l_ui_home_view_input_t *input,
                      d1l_ui_home_view_model_t *out_view);
