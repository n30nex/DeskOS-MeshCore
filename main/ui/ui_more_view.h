#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D1L_UI_MORE_TITLE_TEXT_LEN 48U
#define D1L_UI_MORE_STATUS_TEXT_LEN 64U
#define D1L_UI_MORE_MAX_ITEMS_PER_CATEGORY 3U
#define D1L_UI_MORE_VIEW_MODEL_MAX_BYTES 3200U

typedef enum {
    D1L_UI_SETTINGS_ACTION_NONE = 0,
    D1L_UI_SETTINGS_ACTION_STORAGE,
    D1L_UI_SETTINGS_ACTION_WIFI,
    D1L_UI_SETTINGS_ACTION_BLE,
    D1L_UI_SETTINGS_ACTION_RADIO,
    D1L_UI_SETTINGS_ACTION_MAP_TILES,
    D1L_UI_SETTINGS_ACTION_DISPLAY,
    D1L_UI_SETTINGS_ACTION_DIAGNOSTICS,
    D1L_UI_SETTINGS_ACTION_ADVANCED,
    D1L_UI_SETTINGS_ACTION_PACKETS,
    D1L_UI_SETTINGS_ACTION_TERMINAL,
    D1L_UI_SETTINGS_ACTION_OBSERVER,
    D1L_UI_SETTINGS_ACTION_UPDATE,
    D1L_UI_SETTINGS_ACTION_NOTIFICATIONS,
    D1L_UI_SETTINGS_ACTION_ADMIN,
    D1L_UI_SETTINGS_ACTION_COUNT,
} d1l_ui_settings_action_t;

typedef enum {
    D1L_UI_MORE_CATEGORY_NONE = -1,
    D1L_UI_MORE_CATEGORY_TOOLS = 0,
    D1L_UI_MORE_CATEGORY_CONNECTIONS,
    D1L_UI_MORE_CATEGORY_STORAGE_MAPS,
    D1L_UI_MORE_CATEGORY_DEVICE,
    D1L_UI_MORE_CATEGORY_SUPPORT,
    D1L_UI_MORE_CATEGORY_ADVANCED,
    D1L_UI_MORE_CATEGORY_COUNT,
} d1l_ui_more_category_t;

typedef struct {
    size_t packet_count;
    bool wifi_build_enabled;
    bool wifi_enabled;
    bool wifi_connected;
    bool wifi_connecting;
    bool ble_build_enabled;
    bool ble_transport_supported;
    bool ble_companion_enabled;
    bool radio_ready;
    bool radio_applied;
    bool radio_apply_pending;
    bool storage_sd_present;
    bool storage_sd_data_root_ready;
    bool storage_sd_needs_fat32;
    bool storage_setup_required;
    bool storage_data_enabled;
    bool storage_retained_sd_degraded;
    bool storage_retained_backup_degraded;
    bool map_location_set;
    bool map_tile_cache_ready;
    bool map_tile_render_supported;
    bool identity_ready;
    uint32_t terminal_event_count;
    uint32_t notification_unread_count;
    const char *observer_state;
    const char *update_state;
    const char *admin_state;
    bool time_available;
    bool time_approximate;
    bool timezone_settings_ready;
    const char *storage_sd_state;
    const char *storage_setup_action;
    const char *time_label;
    const char *timezone_label;
    const char *firmware_version;
} d1l_ui_more_view_input_t;

typedef struct {
    char title[D1L_UI_MORE_TITLE_TEXT_LEN];
    char status[D1L_UI_MORE_STATUS_TEXT_LEN];
    uint32_t accent;
    d1l_ui_settings_action_t action;
    bool warning;
    bool actionable;
} d1l_ui_more_item_view_t;

typedef struct {
    d1l_ui_more_category_t category;
    char title[D1L_UI_MORE_TITLE_TEXT_LEN];
    char summary[D1L_UI_MORE_STATUS_TEXT_LEN];
    uint32_t accent;
    bool warning;
    size_t item_count;
    d1l_ui_more_item_view_t items[D1L_UI_MORE_MAX_ITEMS_PER_CATEGORY];
} d1l_ui_more_category_view_t;

typedef struct {
    char title[D1L_UI_MORE_TITLE_TEXT_LEN];
    char subtitle[D1L_UI_MORE_STATUS_TEXT_LEN];
    size_t category_count;
    d1l_ui_more_category_view_t categories[D1L_UI_MORE_CATEGORY_COUNT];
} d1l_ui_more_view_model_t;

bool d1l_ui_more_view(const d1l_ui_more_view_input_t *input,
                      d1l_ui_more_view_model_t *out_view);
void d1l_ui_more_view_apply_release_profile(
    d1l_ui_more_view_input_t *input);
bool d1l_ui_more_view_model_is_valid(const d1l_ui_more_view_model_t *view_model);
