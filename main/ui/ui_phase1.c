#include "ui_phase1.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "rom/cache.h"

#include "app/app_model.h"
#include "app/release_profile.h"
#include "app/settings_model.h"
#include "bsp_lcd.h"
#include "d1l_config.h"
#include "diagnostics/health_monitor.h"
#include "diagnostics/event_log.h"
#include "hal/backlight.h"
#include "hal/display_preferences.h"
#include "hal/indicator_board.h"
#include "map/map_view_service.h"
#include "mesh/dm_store.h"
#include "mesh/user_text.h"
#include "ui_ble.h"
#include "ui_channel_sheets.h"
#include "ui_chrome.h"
#include "ui_compose_eligibility.h"
#include "ui_connectivity.h"
#include "ui_contact_sheets.h"
#include "ui_device_sheets.h"
#include "ui_dm_identity.h"
#include "ui_first_start.h"
#include "ui_home.h"
#include "ui_font_symbols_14.h"
#include "ui_keyboard.h"
#include "ui_map.h"
#include "ui_messages.h"
#include "ui_more_view.h"
#include "ui_modal.h"
#include "ui_navigation.h"
#include "ui_node_detail.h"
#include "ui_nodes.h"
#include "ui_packets.h"
#include "ui_radio_settings.h"
#include "ui_screen.h"
#include "ui_service_sheets.h"
#include "ui_settings.h"
#include "ui_storage_view.h"
#include "ui_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "d1l_ui";
#define D1L_PUBLIC_HISTORY_UI_INITIAL_ROWS 5U
#define D1L_PUBLIC_HISTORY_UI_LOAD_OLDER_STEP 5U
#define D1L_UI_ADMIN_ROOM_PREVIEW_COUNT 3U
#define D1L_UI_ADMIN_ROOM_TRANSCRIPT_BYTES 512U

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_color_t *s_buf1;
static lv_color_t *s_buf2;
static esp_timer_handle_t s_lv_tick_timer;
#if D1L_ENABLE_QUALIFICATION_HOOKS
static uint8_t *s_capture_shadow;
static uint8_t *s_capture_snapshot;
static SemaphoreHandle_t s_capture_lock;
static bool s_capture_shadow_ready;
static bool s_capture_active;
static uint32_t s_capture_frame_seq;
static uint32_t s_capture_flush_count;
static uint32_t s_capture_crc32;
#endif
static bool s_bsp_flush_ready_registered;
static TaskHandle_t s_ui_task_handle;
static TaskHandle_t s_touch_task_handle;
static SemaphoreHandle_t s_ui_start_done_sem;
static esp_err_t s_ui_start_result = ESP_ERR_INVALID_STATE;
static portMUX_TYPE s_touch_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_board_touch_state_t s_touch_state;
static bool s_touch_state_ready = false;
static portMUX_TYPE s_content_refresh_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_map_visibility_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_product_navigation_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_started = false;
static lv_obj_t *s_screen;
static lv_obj_t *s_top_bar;
static lv_obj_t *s_content;
static lv_obj_t *s_title_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_identity_label;
static lv_obj_t *s_lock_button;
static lv_obj_t *s_toast;
static lv_obj_t *s_sheet;
static lv_obj_t *s_dock;
static lv_obj_t *s_dock_buttons[5];
static lv_obj_t *s_compose_sheet;
static lv_obj_t *s_compose_title;
static lv_obj_t *s_compose_textarea;
static lv_obj_t *s_compose_counter;
static lv_obj_t *s_compose_keyboard;
static lv_obj_t *s_compose_send_button;
static lv_obj_t *s_public_history_sheet;
static lv_obj_t *s_public_search_sheet;
static lv_obj_t *s_public_search_title;
static lv_obj_t *s_public_search_textarea;
static lv_obj_t *s_public_search_keyboard;
static lv_obj_t *s_dm_search_sheet;
static lv_obj_t *s_dm_search_textarea;
static lv_obj_t *s_dm_search_keyboard;
static lv_obj_t *s_message_detail_sheet;
static lv_obj_t *s_storage_sheet;
static d1l_wifi_scan_result_t s_wifi_scan_result;
static bool s_wifi_scan_loaded;
static lv_obj_t *s_route_detail_sheet;
static lv_obj_t *s_route_trace_sheet;
static lv_obj_t *s_finder_sheet;
static lv_obj_t *s_packet_detail_sheet;
static lv_obj_t *s_packet_search_sheet;
static lv_obj_t *s_packet_search_textarea;
static lv_obj_t *s_packet_search_keyboard;
static lv_obj_t *s_mesh_roles_sheet;
static lv_obj_t *s_lock_overlay;
#if D1L_ENABLE_QUALIFICATION_HOOKS
static lv_obj_t *s_onboarding_sheet;
static lv_obj_t *s_onboarding_name_textarea;
static lv_obj_t *s_onboarding_keyboard;
#else
static d1l_ui_first_start_controller_t s_first_start_controller;
#endif
static bool s_lock_visible;
static bool s_onboarding_visible;
#if D1L_ENABLE_QUALIFICATION_HOOKS
static bool s_onboarding_probe_suppressed;
#endif
static uint32_t s_toast_until;
static d1l_app_snapshot_t s_snapshot EXT_RAM_BSS_ATTR;
static bool s_compose_dm;
#if D1L_ENABLE_QUALIFICATION_HOOKS
static bool s_compose_probe_send_suppressed;
#endif
static uint64_t s_compose_channel_id;
static char s_compose_channel_name[D1L_CHANNEL_NAME_LEN];
static d1l_ui_messages_mode_t s_messages_mode = D1L_UI_MESSAGES_MODE_ROOT;
static d1l_ui_home_controller_t s_home_controller;
static d1l_ui_messages_controller_t s_messages_controller EXT_RAM_BSS_ATTR;
static d1l_ui_nodes_controller_t s_nodes_controller EXT_RAM_BSS_ATTR;
static d1l_ui_packets_controller_t s_packets_controller EXT_RAM_BSS_ATTR;
static d1l_ui_settings_controller_t s_settings_controller EXT_RAM_BSS_ATTR;
static d1l_ui_storage_view_model_t s_storage_view EXT_RAM_BSS_ATTR;
static d1l_ui_wifi_controller_t s_wifi_controller EXT_RAM_BSS_ATTR;
static d1l_ui_map_sheets_controller_t s_map_sheets_controller EXT_RAM_BSS_ATTR;
static d1l_ui_ble_controller_t s_ble_controller EXT_RAM_BSS_ATTR;
static d1l_ui_device_sheets_controller_t s_device_sheets_controller EXT_RAM_BSS_ATTR;
static d1l_ui_service_sheets_controller_t s_service_sheets_controller EXT_RAM_BSS_ATTR;
static d1l_ui_radio_settings_controller_t s_radio_settings_controller EXT_RAM_BSS_ATTR;
static d1l_ui_contact_sheets_controller_t s_contact_sheets_controller EXT_RAM_BSS_ATTR;
static d1l_ui_channel_sheets_controller_t s_channel_sheets_controller EXT_RAM_BSS_ATTR;
static d1l_ui_node_detail_controller_t s_node_detail_controller EXT_RAM_BSS_ATTR;
static uint32_t s_settings_render_generation;
static bool s_terminal_clear_armed;
static bool s_nodes_clear_armed;
static bool s_update_install_armed;
static bool s_update_reboot_armed;
static d1l_meshcore_admin_mutation_t s_admin_mutation_armed =
    D1L_MESHCORE_ADMIN_MUTATION_NONE;
typedef enum {
    D1L_UI_ADMIN_CLI_ORIGIN_NONE = 0,
    D1L_UI_ADMIN_CLI_ORIGIN_GENERIC,
    D1L_UI_ADMIN_CLI_ORIGIN_ACL,
    D1L_UI_ADMIN_CLI_ORIGIN_ROOM_READ_ONLY_ON,
    D1L_UI_ADMIN_CLI_ORIGIN_ROOM_READ_ONLY_OFF,
} d1l_ui_admin_cli_origin_t;
static char s_admin_cli_armed[
    D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U] EXT_RAM_BSS_ATTR;
static d1l_ui_admin_cli_origin_t s_admin_cli_armed_origin =
    D1L_UI_ADMIN_CLI_ORIGIN_NONE;
static bool s_admin_cli_secure_input;
static bool s_admin_rendered_generation_valid;
static uint32_t s_admin_rendered_dm_revision;
static uint32_t s_terminal_clear_deadline;
static uint32_t s_nodes_clear_deadline;
static uint32_t s_update_install_deadline;
static uint32_t s_update_reboot_deadline;
static uint32_t s_admin_mutation_deadline;
static uint32_t s_admin_cli_deadline;
static uint32_t s_admin_rendered_generation;
static bool s_backlight_dimmed;
static bool s_backlight_pulse_active;
static uint32_t s_backlight_pulse_until;
static uint32_t s_last_notification_unread;
static d1l_packet_log_entry_t s_packet_query_rows[D1L_PACKET_LOG_CAPACITY] EXT_RAM_BSS_ATTR;
static d1l_packet_log_entry_t
    s_packet_row_payloads[D1L_PACKET_LOG_CAPACITY] EXT_RAM_BSS_ATTR;
static size_t s_packet_row_payload_count;
static uint32_t s_packet_row_generation;
static d1l_contact_entry_t s_compose_contact;
static esp_err_t s_compose_last_send_error;
static int32_t s_map_location_lat_e7;
static int32_t s_map_location_lon_e7;
static bool s_map_location_returns_to_options;
static d1l_route_entry_t s_route_detail_route;
static d1l_route_entry_t
    s_route_row_payloads[D1L_APP_SNAPSHOT_ROUTE_PREVIEW] EXT_RAM_BSS_ATTR;
static size_t s_route_row_payload_count;
static uint32_t s_route_row_generation;
static d1l_contact_entry_t s_route_trace_contact;
static d1l_message_entry_t s_message_detail_message;
static d1l_packet_log_entry_t s_packet_detail_packet;
static char s_admin_target_fingerprint[D1L_NODE_FINGERPRINT_LEN];
static d1l_dm_entry_t
    s_admin_room_preview[D1L_UI_ADMIN_ROOM_PREVIEW_COUNT] EXT_RAM_BSS_ATTR;
static char
    s_admin_room_transcript[D1L_UI_ADMIN_ROOM_TRANSCRIPT_BYTES]
        EXT_RAM_BSS_ATTR;
static d1l_node_view_t s_map_node_rows[D1L_NODE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
static d1l_route_entry_t s_route_trace_entries[D1L_ROUTE_STORE_CAPACITY]
    EXT_RAM_BSS_ATTR;
static d1l_meshcore_contact_telemetry_snapshot_t
    s_route_trace_telemetry EXT_RAM_BSS_ATTR;
static d1l_meshcore_trace_snapshot_t
    s_route_trace_snapshot EXT_RAM_BSS_ATTR;
static d1l_meshcore_discovery_snapshot_t
    s_finder_snapshot EXT_RAM_BSS_ATTR;
static uint32_t s_route_trace_telemetry_rendered_generation;
static uint32_t s_route_trace_last_render_second;
static uint32_t s_finder_rendered_generation;
static uint32_t s_finder_last_render_second;
static d1l_message_entry_t s_public_history_entries[D1L_MESSAGE_STORE_CAPACITY]
    EXT_RAM_BSS_ATTR;
static char s_public_search_text[D1L_MESSAGE_TEXT_LEN];
static uint64_t s_public_history_channel_id;
static char s_public_history_channel_name[D1L_CHANNEL_NAME_LEN];
static const uint32_t D1L_UI_TIMER_MIN_SLEEP_MS = 20U;
static const uint32_t D1L_UI_TIMER_MAX_SLEEP_MS = 50U;
static const uint32_t D1L_UI_MAP_VIEWPORT_REFRESH_MS = 500U;
static const uint32_t D1L_UI_ADMIN_MUTATION_CONFIRM_WINDOW_MS = 5000U;
static size_t s_public_history_limit = D1L_PUBLIC_HISTORY_UI_INITIAL_ROWS;
#if D1L_ENABLE_QUALIFICATION_HOOKS
/* A full retained Packet view can legitimately take longer than 1.5 s to rebuild
 * before a nested page opens. Keep the serial proof bounded without turning a
 * populated device into a false timeout. */
static const uint32_t D1L_UI_SCROLL_PROBE_TIMEOUT_MS = 5000U;
static const uint32_t D1L_UI_COMPOSE_PROBE_TIMEOUT_MS = 1500U;
typedef struct {
    uint32_t next_id;
    uint32_t pending_id;
    uint32_t inflight_id;
    uint32_t completed_id;
    uint32_t abandoned_id;
    uint32_t signaled_id;
    bool admission_reserved;
} d1l_ui_probe_gate_t;

static SemaphoreHandle_t s_scroll_probe_done_sem;
static portMUX_TYPE s_scroll_probe_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_ui_probe_gate_t s_scroll_probe_gate;
static char s_scroll_probe_surface[24];
static d1l_ui_scroll_probe_result_t s_scroll_probe_result;
static SemaphoreHandle_t s_compose_probe_done_sem;
static portMUX_TYPE s_compose_probe_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_ui_probe_gate_t s_compose_probe_gate;
static char s_compose_probe_target[16];
static d1l_ui_compose_probe_result_t s_compose_probe_result;
#endif

#if CONFIG_FREERTOS_UNICORE
#define D1L_UI_TASK_CORE 0
#else
#define D1L_UI_TASK_CORE 1
#endif

#define D1L_TOUCH_TASK_STACK_BYTES 4096U
#define D1L_UI_TASK_STACK_BYTES 12288U
#define D1L_UI_ROW_TOKEN_INDEX_BITS 8U
#define D1L_UI_ROW_TOKEN_INDEX_MASK 0xFFU
#define D1L_UI_ROW_TOKEN_GENERATION_MASK 0x00FFFFFFU

_Static_assert(D1L_PACKET_LOG_CAPACITY <= D1L_UI_ROW_TOKEN_INDEX_MASK,
               "packet row token index is too small");
_Static_assert(D1L_APP_SNAPSHOT_ROUTE_PREVIEW <= D1L_UI_ROW_TOKEN_INDEX_MASK,
               "route row token index is too small");

static void render_active_tab(void);
static esp_err_t initialize_ui_runtime(void);
static void fail_ui_start_on_ui_task(esp_err_t result);
static void touch_poll_task(void *arg);
static bool show_contact_detail_sheet(void);
static bool show_contact_options_sheet(void);
static void open_public_history_event_cb(lv_event_t *event);
static void open_public_search_event_cb(lv_event_t *event);
static void open_home_dm_preview_event_cb(lv_event_t *event);
static void open_contact_detail_event_cb(lv_event_t *event);
static void open_map_node_detail(const char *fingerprint);
static void open_route_trace_event_cb(lv_event_t *event);
static void render_route_trace_sheet(void);
static void render_finder_sheet(void);
static void open_route_detail_event_cb(lv_event_t *event);
static void open_packet_detail_event_cb(lv_event_t *event);
static void open_packet_search_event_cb(lv_event_t *event);
static void open_mesh_roles_event_cb(lv_event_t *event);
static void render_mesh_roles_sheet(void);
static void packet_pause_event_cb(lv_event_t *event);
static void packet_load_older_event_cb(lv_event_t *event);
static void packet_load_newer_event_cb(lv_event_t *event);
static void public_history_load_older_event_cb(lv_event_t *event);
static void packet_detail_mode_event_cb(lv_event_t *event);
static void open_radio_settings_event_cb(lv_event_t *event);
static void open_storage_sheet_event_cb(lv_event_t *event);
static void open_wifi_sheet_event_cb(lv_event_t *event);
static void open_ble_sheet_event_cb(lv_event_t *event);
static void open_display_sheet_event_cb(lv_event_t *event);
static void open_diagnostics_sheet_event_cb(lv_event_t *event);
static void open_terminal_sheet_event_cb(lv_event_t *event);
static void open_observer_sheet_event_cb(lv_event_t *event);
static void open_update_sheet_event_cb(lv_event_t *event);
static void open_notifications_sheet_event_cb(lv_event_t *event);
static void open_admin_sheet_event_cb(lv_event_t *event);
static void open_sheet_event_cb(lv_event_t *event);
static bool confirmation_active(bool armed, uint32_t deadline);

typedef d1l_ui_screen_t d1l_ui_tab_t;

#define D1L_UI_TAB_HOME D1L_UI_SCREEN_HOME
#define D1L_UI_TAB_MESSAGES D1L_UI_SCREEN_MESSAGES
#define D1L_UI_TAB_NODES D1L_UI_SCREEN_NODES
#define D1L_UI_TAB_MAP D1L_UI_SCREEN_MAP
#define D1L_UI_TAB_PACKETS D1L_UI_SCREEN_PACKETS
#define D1L_UI_TAB_SETTINGS D1L_UI_SCREEN_SETTINGS

typedef struct {
    d1l_ui_tab_t tab;
    const char *label;
    const char *icon;
} d1l_ui_dock_item_t;

static const d1l_ui_dock_item_t k_dock_items[] = {
    {D1L_UI_TAB_HOME, "Home", LV_SYMBOL_HOME},
    {D1L_UI_TAB_MESSAGES, "Channels", LV_SYMBOL_ENVELOPE},
    {D1L_UI_TAB_NODES, "Contacts", LV_SYMBOL_LIST},
    {D1L_UI_TAB_MAP, "Map", LV_SYMBOL_IMAGE},
    {D1L_UI_TAB_SETTINGS, "Settings", LV_SYMBOL_SETTINGS},
};

static const d1l_ui_dock_item_t k_core_dock_items[] = {
    {D1L_UI_TAB_HOME, "Home", LV_SYMBOL_HOME},
    {D1L_UI_TAB_MESSAGES, "Channels", LV_SYMBOL_ENVELOPE},
    {D1L_UI_TAB_NODES, "Contacts", LV_SYMBOL_LIST},
    {D1L_UI_TAB_MAP, "Map", LV_SYMBOL_IMAGE},
    {D1L_UI_TAB_SETTINGS, "Settings", LV_SYMBOL_SETTINGS},
};

_Static_assert(sizeof(s_dock_buttons) / sizeof(s_dock_buttons[0]) ==
                   sizeof(k_dock_items) / sizeof(k_dock_items[0]),
               "dock button and item counts must match");
_Static_assert(sizeof(k_core_dock_items) / sizeof(k_core_dock_items[0]) ==
                   sizeof(k_dock_items) / sizeof(k_dock_items[0]),
               "Core and development docks must have matching item counts");

static const d1l_ui_dock_item_t *active_dock_items(size_t *out_count)
{
    const d1l_ui_dock_item_t *items =
        d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP) ?
            k_dock_items : k_core_dock_items;
    if (out_count) {
        *out_count = sizeof(k_dock_items) / sizeof(k_dock_items[0]);
    }
    return items;
}

typedef struct {
    uint32_t rx_packets;
    uint32_t rx_adverts;
    uint32_t tx_packets;
    uint32_t message_total_written;
    uint32_t dm_total_written;
    uint32_t dm_content_revision;
    uint32_t node_total_written;
    uint32_t contact_total_written;
    uint32_t route_total_written;
    uint32_t packet_total_written;
    uint32_t public_unread_count;
    uint32_t dm_unread_count;
    uint32_t muted_dm_unread_count;
    uint32_t last_public_read_seq;
    uint32_t last_dm_read_seq;
    uint32_t read_state_mark_count;
    uint32_t channel_store_revision;
    uint64_t active_channel_id;
    size_t message_count;
    size_t dm_count;
    size_t node_count;
    size_t contact_count;
    size_t dm_capable_contact_count;
    size_t route_count;
    size_t packet_count;
    bool storage_sd_present;
    bool storage_sd_mounted;
    bool storage_sd_data_root_ready;
    bool storage_sd_needs_fat32;
    bool storage_setup_required;
    bool storage_data_enabled;
    bool storage_retained_sd_degraded;
    bool storage_retained_backup_degraded;
    bool message_store_loaded;
    bool dm_store_loaded;
    bool message_store_persistence_degraded;
    bool dm_store_persistence_degraded;
    bool dm_retry_active;
    bool dm_failure_latched;
    bool map_tile_cache_ready;
    bool map_location_set;
    bool map_tile_render_supported;
    bool identity_ready;
    bool radio_ready;
    bool radio_applied;
    bool radio_apply_pending;
    bool wifi_build_enabled;
    bool wifi_enabled;
    bool wifi_connected;
    bool wifi_connecting;
    bool ble_build_enabled;
    bool ble_transport_supported;
    bool ble_companion_enabled;
    const char *storage_sd_state;
    const char *storage_setup_action;
    const char *message_store_backend;
    const char *dm_store_backend;
    const char *node_store_backend;
    const char *contact_store_backend;
    const char *read_state_backend;
} d1l_ui_content_generation_t;

typedef struct {
    bool location_set;
    d1l_map_center_source_t center_source;
    int32_t lat_e7;
    int32_t lon_e7;
    uint8_t zoom;
} d1l_ui_map_render_input_t;

typedef enum {
    D1L_MESH_ROLES_PAGE_ROOT = 0,
    D1L_MESH_ROLES_PAGE_ROOM_SERVERS,
    D1L_MESH_ROLES_PAGE_REPEATER_CANDIDATES,
} d1l_mesh_roles_page_t;

typedef enum {
    D1L_STORAGE_PAGE_ROOT = 0,
    D1L_STORAGE_PAGE_CARD_STATUS,
    D1L_STORAGE_PAGE_DATA_LOCATIONS,
} d1l_storage_page_t;

static bool s_content_refresh_pending = false;
static bool s_map_interactive_touch_authorized;
static bool s_product_navigation_wake_pending;
static d1l_ui_content_generation_t s_rendered_content_generation;
static bool s_rendered_content_generation_valid = false;
static d1l_ui_map_render_input_t s_rendered_map_input;
static bool s_rendered_map_input_valid = false;
static bool s_packet_detail_advanced;
static bool s_message_detail_advanced;
static d1l_mesh_roles_page_t s_mesh_roles_page = D1L_MESH_ROLES_PAGE_ROOT;
static d1l_storage_page_t s_storage_page = D1L_STORAGE_PAGE_ROOT;

static d1l_ui_content_generation_t content_generation_from_snapshot(
    const d1l_app_snapshot_t *snapshot)
{
    return (d1l_ui_content_generation_t) {
        .rx_packets = snapshot->rx_packets,
        .rx_adverts = snapshot->rx_adverts,
        .tx_packets = snapshot->tx_packets,
        .message_total_written = snapshot->message_total_written,
        .dm_total_written = snapshot->dm_total_written,
        .dm_content_revision = snapshot->dm_content_revision,
        .node_total_written = snapshot->node_total_written,
        .contact_total_written = snapshot->contact_total_written,
        .route_total_written = snapshot->route_total_written,
        .packet_total_written = snapshot->packet_total_written,
        .public_unread_count = snapshot->public_unread_count,
        .dm_unread_count = snapshot->dm_unread_count,
        .muted_dm_unread_count = snapshot->muted_dm_unread_count,
        .last_public_read_seq = snapshot->last_public_read_seq,
        .last_dm_read_seq = snapshot->last_dm_read_seq,
        .read_state_mark_count = snapshot->read_state_mark_count,
        .channel_store_revision = snapshot->channel_store_revision,
        .active_channel_id = snapshot->active_channel_id,
        .message_count = snapshot->message_count,
        .dm_count = snapshot->dm_count,
        .node_count = snapshot->node_count,
        .contact_count = snapshot->contact_count,
        .dm_capable_contact_count = snapshot->dm_capable_contact_count,
        .route_count = snapshot->route_count,
        .packet_count = snapshot->packet_count,
        .storage_sd_present = snapshot->storage_sd_present,
        .storage_sd_mounted = snapshot->storage_sd_mounted,
        .storage_sd_data_root_ready = snapshot->storage_sd_data_root_ready,
        .storage_sd_needs_fat32 = snapshot->storage_sd_needs_fat32,
        .storage_setup_required = snapshot->storage_setup_required,
        .storage_data_enabled = snapshot->storage_data_enabled,
        .storage_retained_sd_degraded = snapshot->storage_retained_sd_degraded,
        .storage_retained_backup_degraded = snapshot->storage_retained_backup_degraded,
        .message_store_loaded = snapshot->message_store_loaded,
        .dm_store_loaded = snapshot->dm_store_loaded,
        .message_store_persistence_degraded =
            snapshot->message_store_persistence_degraded,
        .dm_store_persistence_degraded =
            snapshot->dm_store_persistence_degraded,
        .dm_retry_active = snapshot->dm_retry_active,
        .dm_failure_latched = snapshot->dm_failure_latched,
        .map_tile_cache_ready = snapshot->map_tile_cache_ready,
        .map_location_set = snapshot->map_location_set,
        .map_tile_render_supported = snapshot->map_tile_render_supported,
        .identity_ready = snapshot->identity_ready,
        .radio_ready = snapshot->radio_ready,
        .radio_applied = snapshot->radio_applied,
        .radio_apply_pending = snapshot->radio_apply_pending,
        .wifi_build_enabled = snapshot->wifi_build_enabled,
        .wifi_enabled = snapshot->wifi_enabled,
        .wifi_connected = snapshot->wifi_connected,
        .wifi_connecting = snapshot->wifi_connecting,
        .ble_build_enabled = snapshot->ble_build_enabled,
        .ble_transport_supported = snapshot->ble_transport_supported,
        .ble_companion_enabled = snapshot->ble_companion_enabled,
        .storage_sd_state = snapshot->storage_sd_state,
        .storage_setup_action = snapshot->storage_setup_action,
        .message_store_backend = snapshot->message_store_backend,
        .dm_store_backend = snapshot->dm_store_backend,
        .node_store_backend = snapshot->node_store_backend,
        .contact_store_backend = snapshot->contact_store_backend,
        .read_state_backend = snapshot->read_state_backend,
    };
}

static bool content_generation_text_equal(const char *left, const char *right)
{
    if (left == right) {
        return true;
    }
    return left && right && strcmp(left, right) == 0;
}

static bool content_generation_equal(const d1l_ui_content_generation_t *left,
                                     const d1l_ui_content_generation_t *right)
{
    return left->rx_packets == right->rx_packets &&
        left->rx_adverts == right->rx_adverts &&
        left->tx_packets == right->tx_packets &&
        left->message_total_written == right->message_total_written &&
        left->dm_total_written == right->dm_total_written &&
        left->dm_content_revision == right->dm_content_revision &&
        left->node_total_written == right->node_total_written &&
        left->contact_total_written == right->contact_total_written &&
        left->route_total_written == right->route_total_written &&
        left->packet_total_written == right->packet_total_written &&
        left->public_unread_count == right->public_unread_count &&
        left->dm_unread_count == right->dm_unread_count &&
        left->muted_dm_unread_count == right->muted_dm_unread_count &&
        left->last_public_read_seq == right->last_public_read_seq &&
        left->last_dm_read_seq == right->last_dm_read_seq &&
        left->read_state_mark_count == right->read_state_mark_count &&
        left->channel_store_revision == right->channel_store_revision &&
        left->active_channel_id == right->active_channel_id &&
        left->message_count == right->message_count &&
        left->dm_count == right->dm_count &&
        left->node_count == right->node_count &&
        left->contact_count == right->contact_count &&
        left->dm_capable_contact_count == right->dm_capable_contact_count &&
        left->route_count == right->route_count &&
        left->packet_count == right->packet_count &&
        left->storage_sd_present == right->storage_sd_present &&
        left->storage_sd_mounted == right->storage_sd_mounted &&
        left->storage_sd_data_root_ready == right->storage_sd_data_root_ready &&
        left->storage_sd_needs_fat32 == right->storage_sd_needs_fat32 &&
        left->storage_setup_required == right->storage_setup_required &&
        left->storage_data_enabled == right->storage_data_enabled &&
        left->storage_retained_sd_degraded == right->storage_retained_sd_degraded &&
        left->storage_retained_backup_degraded == right->storage_retained_backup_degraded &&
        left->message_store_loaded == right->message_store_loaded &&
        left->dm_store_loaded == right->dm_store_loaded &&
        left->message_store_persistence_degraded ==
            right->message_store_persistence_degraded &&
        left->dm_store_persistence_degraded ==
            right->dm_store_persistence_degraded &&
        left->dm_retry_active == right->dm_retry_active &&
        left->dm_failure_latched == right->dm_failure_latched &&
        left->map_tile_cache_ready == right->map_tile_cache_ready &&
        left->map_location_set == right->map_location_set &&
        left->map_tile_render_supported == right->map_tile_render_supported &&
        left->identity_ready == right->identity_ready &&
        left->radio_ready == right->radio_ready &&
        left->radio_applied == right->radio_applied &&
        left->radio_apply_pending == right->radio_apply_pending &&
        left->wifi_build_enabled == right->wifi_build_enabled &&
        left->wifi_enabled == right->wifi_enabled &&
        left->wifi_connected == right->wifi_connected &&
        left->wifi_connecting == right->wifi_connecting &&
        left->ble_build_enabled == right->ble_build_enabled &&
        left->ble_transport_supported == right->ble_transport_supported &&
        left->ble_companion_enabled == right->ble_companion_enabled &&
        content_generation_text_equal(left->storage_sd_state,
                                      right->storage_sd_state) &&
        content_generation_text_equal(left->storage_setup_action,
                                      right->storage_setup_action) &&
        content_generation_text_equal(left->message_store_backend,
                                      right->message_store_backend) &&
        content_generation_text_equal(left->dm_store_backend,
                                      right->dm_store_backend) &&
        content_generation_text_equal(left->node_store_backend,
                                      right->node_store_backend) &&
        content_generation_text_equal(left->contact_store_backend,
                                      right->contact_store_backend) &&
        content_generation_text_equal(left->read_state_backend,
                                      right->read_state_backend);
}

static void remember_rendered_content_generation(const d1l_app_snapshot_t *snapshot)
{
    s_rendered_content_generation = content_generation_from_snapshot(snapshot);
    s_rendered_content_generation_valid = true;
}

static bool content_generation_changed_from_rendered(const d1l_app_snapshot_t *snapshot)
{
    d1l_ui_content_generation_t current = content_generation_from_snapshot(snapshot);
    if (!s_rendered_content_generation_valid) {
        s_rendered_content_generation = current;
        s_rendered_content_generation_valid = true;
        return false;
    }
    return !content_generation_equal(&current, &s_rendered_content_generation);
}

static d1l_ui_map_render_input_t map_render_input_from_snapshot(
    const d1l_app_snapshot_t *snapshot)
{
    return (d1l_ui_map_render_input_t) {
        .location_set = snapshot->map_location_set,
        .center_source = snapshot->map_center_source,
        .lat_e7 = snapshot->map_lat_e7,
        .lon_e7 = snapshot->map_lon_e7,
        .zoom = snapshot->map_tile_zoom,
    };
}

static void remember_rendered_map_input(const d1l_app_snapshot_t *snapshot)
{
    s_rendered_map_input = map_render_input_from_snapshot(snapshot);
    s_rendered_map_input_valid = true;
}

static bool map_render_input_changed_from_rendered(const d1l_app_snapshot_t *snapshot)
{
    const d1l_ui_map_render_input_t current = map_render_input_from_snapshot(snapshot);
    return !s_rendered_map_input_valid ||
           current.location_set != s_rendered_map_input.location_set ||
           current.center_source != s_rendered_map_input.center_source ||
           current.lat_e7 != s_rendered_map_input.lat_e7 ||
           current.lon_e7 != s_rendered_map_input.lon_e7 ||
           current.zoom != s_rendered_map_input.zoom;
}

static void request_tab_switch(d1l_ui_tab_t tab)
{
    if (!d1l_ui_screen_available(tab)) {
        return;
    }
    d1l_ui_navigation_request(tab);
}

static bool begin_pending_tab_switch(d1l_ui_tab_t *out_tab)
{
    return d1l_ui_navigation_begin_pending(out_tab);
}

static void finish_pending_tab_switch(d1l_ui_tab_t rendered_tab)
{
    d1l_ui_navigation_finish(rendered_tab);
}

static void request_content_refresh(void)
{
    portENTER_CRITICAL(&s_content_refresh_lock);
    s_content_refresh_pending = true;
    portEXIT_CRITICAL(&s_content_refresh_lock);
}

static bool begin_pending_content_refresh(void)
{
    bool pending;

    portENTER_CRITICAL(&s_content_refresh_lock);
    pending = s_content_refresh_pending;
    s_content_refresh_pending = false;
    portEXIT_CRITICAL(&s_content_refresh_lock);
    return pending;
}

static bool content_refresh_pending(void)
{
    bool pending;

    portENTER_CRITICAL(&s_content_refresh_lock);
    pending = s_content_refresh_pending;
    portEXIT_CRITICAL(&s_content_refresh_lock);
    return pending;
}

static void set_map_interactive_touch_authorized(bool authorized)
{
    portENTER_CRITICAL(&s_map_visibility_lock);
    s_map_interactive_touch_authorized = authorized;
    portEXIT_CRITICAL(&s_map_visibility_lock);
}

static bool map_interactive_touch_authorized(void)
{
    bool authorized;
    portENTER_CRITICAL(&s_map_visibility_lock);
    authorized = s_map_interactive_touch_authorized;
    portEXIT_CRITICAL(&s_map_visibility_lock);
    return authorized;
}

static void request_product_navigation_wake(void)
{
    portENTER_CRITICAL(&s_product_navigation_lock);
    s_product_navigation_wake_pending = true;
    portEXIT_CRITICAL(&s_product_navigation_lock);
}

static bool begin_product_navigation_wake(void)
{
    bool pending;
    portENTER_CRITICAL(&s_product_navigation_lock);
    pending = s_product_navigation_wake_pending;
    s_product_navigation_wake_pending = false;
    portEXIT_CRITICAL(&s_product_navigation_lock);
    return pending;
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
/* Probe-gate helpers are called only while the matching probe lock is held. */
static bool probe_gate_busy(const d1l_ui_probe_gate_t *gate)
{
    return gate && (gate->admission_reserved || gate->pending_id != 0U ||
                    gate->inflight_id != 0U);
}

static bool probe_gate_reserve_admission(d1l_ui_probe_gate_t *gate)
{
    if (!gate || probe_gate_busy(gate)) {
        return false;
    }
    gate->admission_reserved = true;
    return true;
}

static uint32_t probe_gate_commit_admission(d1l_ui_probe_gate_t *gate)
{
    if (!gate || !gate->admission_reserved || gate->pending_id != 0U ||
        gate->inflight_id != 0U) {
        return 0U;
    }
    gate->next_id++;
    if (gate->next_id == 0U) {
        gate->next_id++;
    }
    gate->pending_id = gate->next_id;
    gate->completed_id = 0U;
    gate->abandoned_id = 0U;
    gate->signaled_id = 0U;
    gate->admission_reserved = false;
    return gate->pending_id;
}

static void probe_gate_abort_admission(d1l_ui_probe_gate_t *gate)
{
    if (gate) {
        gate->admission_reserved = false;
    }
}

static uint32_t probe_gate_begin(d1l_ui_probe_gate_t *gate)
{
    if (!gate || gate->pending_id == 0U || gate->inflight_id != 0U) {
        return 0U;
    }
    const uint32_t request_id = gate->pending_id;
    gate->pending_id = 0U;
    gate->inflight_id = request_id;
    return request_id;
}

static void probe_gate_timeout(d1l_ui_probe_gate_t *gate, uint32_t request_id)
{
    if (!gate || request_id == 0U) {
        return;
    }
    if (gate->pending_id == request_id) {
        gate->pending_id = 0U;
    } else if (gate->inflight_id == request_id) {
        if (gate->signaled_id == request_id) {
            gate->inflight_id = 0U;
            gate->completed_id = 0U;
            gate->abandoned_id = 0U;
            gate->signaled_id = 0U;
        } else {
            gate->abandoned_id = request_id;
        }
    }
}

static bool probe_gate_publish(d1l_ui_probe_gate_t *gate, uint32_t request_id)
{
    if (!gate || request_id == 0U || gate->inflight_id != request_id) {
        return false;
    }
    gate->completed_id = request_id;
    return true;
}

static void probe_gate_finish_signal(d1l_ui_probe_gate_t *gate, uint32_t request_id)
{
    if (gate && gate->inflight_id == request_id) {
        gate->signaled_id = request_id;
        if (gate->abandoned_id == request_id) {
            gate->inflight_id = 0U;
            gate->completed_id = 0U;
            gate->abandoned_id = 0U;
            gate->signaled_id = 0U;
        }
    }
}

static bool probe_gate_acknowledge(d1l_ui_probe_gate_t *gate, uint32_t request_id)
{
    if (!gate || gate->inflight_id != request_id || gate->completed_id != request_id) {
        return false;
    }
    gate->inflight_id = 0U;
    gate->completed_id = 0U;
    gate->abandoned_id = 0U;
    gate->signaled_id = 0U;
    return true;
}

static bool scroll_probe_pending(void)
{
    bool pending;

    portENTER_CRITICAL(&s_scroll_probe_lock);
    pending = probe_gate_busy(&s_scroll_probe_gate);
    portEXIT_CRITICAL(&s_scroll_probe_lock);
    return pending;
}

static bool compose_probe_pending(void)
{
    bool pending;

    portENTER_CRITICAL(&s_compose_probe_lock);
    pending = probe_gate_busy(&s_compose_probe_gate);
    portEXIT_CRITICAL(&s_compose_probe_lock);
    return pending;
}
#endif

static bool render_dm_thread_sheet(void);
static void handle_messages_action(
    const d1l_ui_messages_action_event_t *event, void *context);
static void render_public_history_sheet(void);
static void show_public_history_sheet(void);
static void render_packet_detail_sheet(void);
static bool render_radio_settings_sheet(void);
static void render_storage_sheet(void);
static bool render_wifi_sheet(void);
static bool render_ble_sheet(void);
static bool render_display_sheet(void);
static bool render_diagnostics_sheet(void);
static void service_sheets_action_handler(
    d1l_ui_service_action_t action, void *context);
static bool render_map_location_sheet(void);
static bool render_map_options_sheet(d1l_ui_map_options_page_t page);
static void hide_node_detail_sheet(void);
static void request_tab_event_cb(lv_event_t *event);
static void open_map_location_sheet_event_cb(lv_event_t *event);
static void open_map_location_from_options_event_cb(lv_event_t *event);
static void open_map_options_sheet_event_cb(lv_event_t *event);
static void open_map_cache_status_event_cb(lv_event_t *event);
static void back_to_map_options_event_cb(lv_event_t *event);
static void close_map_location_sheet_event_cb(lv_event_t *event);
static void map_location_textarea_event_cb(lv_event_t *event);
static void map_location_save_event_cb(lv_event_t *event);
static void map_location_clear_event_cb(lv_event_t *event);
static void close_map_options_sheet_event_cb(lv_event_t *event);
static void message_detail_mode_event_cb(lv_event_t *event);
static void map_location_keyboard_event_cb(lv_event_t *event);
static void unlock_event_cb(lv_event_t *event);
static void process_pending_content_refresh(void);
#if D1L_ENABLE_QUALIFICATION_HOOKS
static void process_pending_scroll_probe(void);
#endif
static const char *dm_row_state(const d1l_dm_entry_t *entry, bool unread);
#if D1L_ENABLE_QUALIFICATION_HOOKS
static void process_pending_compose_probe(void);
static bool object_is_visible(lv_obj_t *obj);
#endif

static const d1l_ui_map_callbacks_t callbacks = {
    .open_options = open_map_options_sheet_event_cb,
    .close_options = close_map_options_sheet_event_cb,
    .open_location = open_map_location_from_options_event_cb,
    .open_cache_status = open_map_cache_status_event_cb,
    .back_to_options = back_to_map_options_event_cb,
    .close_location = close_map_location_sheet_event_cb,
    .save_location = map_location_save_event_cb,
    .clear_location = map_location_clear_event_cb,
    .location_textarea = map_location_textarea_event_cb,
    .location_keyboard = map_location_keyboard_event_cb,
    .open_node_detail = open_map_node_detail,
};

static const char *tab_name(d1l_ui_tab_t tab)
{
    return d1l_ui_screen_name(tab);
}

static bool tab_from_name(const char *name, d1l_ui_tab_t *out_tab)
{
    return d1l_ui_screen_from_name(name, out_tab);
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static bool scroll_surface_from_name(const char *name, char *out_surface,
                                     size_t out_surface_len, d1l_ui_tab_t *out_tab)
{
    return d1l_ui_scroll_surface_from_name(name, out_surface, out_surface_len, out_tab);
}

static bool compose_probe_target_from_name(const char *name, char *out_target,
                                           size_t out_target_len)
{
    return d1l_ui_keyboard_normalize_probe_target(name, out_target, out_target_len);
}

static bool compose_probe_target_available(const char *target)
{
    if (!target) {
        return false;
    }
    if (strcmp(target, "public") == 0 ||
        strcmp(target, "public_long") == 0 ||
        strcmp(target, "public_search") == 0) {
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_PUBLIC_MESSAGES);
    }
    if (strcmp(target, "dm") == 0 ||
        strcmp(target, "dm_long") == 0 ||
        strcmp(target, "dm_search") == 0) {
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_DIRECT_MESSAGES);
    }
    if (strcmp(target, "packet_search") == 0) {
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_PACKETS);
    }
    if (strcmp(target, "contact_edit") == 0) {
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_BASIC_CONTACTS);
    }
    if (strcmp(target, "map_location") == 0) {
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP) &&
               d1l_release_feature_available(D1L_RELEASE_FEATURE_LOCATION);
    }
    if (strcmp(target, "wifi_ssid") == 0 ||
        strcmp(target, "wifi_password") == 0) {
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL);
    }
    return strcmp(target, "onboarding") == 0;
}
#endif

static void request_full_screen_repaint(void)
{
    if (s_screen) {
        lv_obj_invalidate(s_screen);
    }
}

static void force_ui_layout_repaint(void)
{
    if (!s_screen) {
        return;
    }
    lv_obj_update_layout(s_screen);
    request_full_screen_repaint();
    lv_refr_now(NULL);
}

static void configure_sheet_scroll(lv_obj_t *sheet)
{
    d1l_ui_modal_configure_scroll(sheet);
}

static void lv_tick_task(void *arg)
{
    (void)arg;
    lv_tick_inc(5);
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static uint32_t crc32_bytes(const uint8_t *data, size_t len)
{
    return crc32_update(0U, data, len);
}

static esp_err_t init_capture_buffers(void)
{
    if (!s_capture_lock) {
        s_capture_lock = xSemaphoreCreateMutex();
        if (!s_capture_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_capture_shadow) {
        s_capture_shadow = heap_caps_malloc(D1L_UI_CAPTURE_TOTAL_BYTES,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_capture_shadow) {
            s_capture_shadow = heap_caps_malloc(D1L_UI_CAPTURE_TOTAL_BYTES, MALLOC_CAP_8BIT);
        }
        if (!s_capture_shadow) {
            return ESP_ERR_NO_MEM;
        }
        memset(s_capture_shadow, 0, D1L_UI_CAPTURE_TOTAL_BYTES);
    }
    if (!s_capture_snapshot) {
        s_capture_snapshot = heap_caps_malloc(D1L_UI_CAPTURE_TOTAL_BYTES,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_capture_snapshot) {
            s_capture_snapshot = heap_caps_malloc(D1L_UI_CAPTURE_TOTAL_BYTES, MALLOC_CAP_8BIT);
        }
        if (!s_capture_snapshot) {
            return ESP_ERR_NO_MEM;
        }
        memset(s_capture_snapshot, 0, D1L_UI_CAPTURE_TOTAL_BYTES);
    }
    return ESP_OK;
}

static void copy_rect_to_capture_shadow(int x1,
                                        int y1,
                                        int x2,
                                        int y2,
                                        const uint8_t *src,
                                        int src_origin_x,
                                        int src_origin_y,
                                        int src_stride_pixels)
{
    if (!s_capture_shadow || !src || !s_capture_lock || src_stride_pixels <= 0) {
        return;
    }
    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > (int)D1L_UI_CAPTURE_WIDTH) {
        x2 = (int)D1L_UI_CAPTURE_WIDTH;
    }
    if (y2 > (int)D1L_UI_CAPTURE_HEIGHT) {
        y2 = (int)D1L_UI_CAPTURE_HEIGHT;
    }
    if (x1 >= x2 || y1 >= y2) {
        return;
    }
    if (x1 < src_origin_x || y1 < src_origin_y) {
        return;
    }

    const size_t copy_bytes = (size_t)(x2 - x1) * D1L_UI_CAPTURE_BYTES_PER_PIXEL;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    for (int y = y1; y < y2; ++y) {
        const size_t src_offset =
            ((size_t)(y - src_origin_y) * (size_t)src_stride_pixels + (size_t)(x1 - src_origin_x)) *
            D1L_UI_CAPTURE_BYTES_PER_PIXEL;
        const size_t dest_offset =
            ((size_t)y * D1L_UI_CAPTURE_WIDTH + (size_t)x1) *
            D1L_UI_CAPTURE_BYTES_PER_PIXEL;
        memcpy(s_capture_shadow + dest_offset, src + src_offset, copy_bytes);
    }
    s_capture_shadow_ready = true;
    s_capture_flush_count++;
    s_capture_frame_seq++;
    xSemaphoreGive(s_capture_lock);
}
#endif

static bool lcd_flush_ready_cb(void *data)
{
    lv_disp_drv_t *drv = data ? (lv_disp_drv_t *)data : &s_disp_drv;
    lv_disp_flush_ready(drv);
    return false;
}

#if CONFIG_LCD_LVGL_DIRECT_MODE
static bool lcd_flush_is_last_cb(void)
{
    return lv_disp_flush_is_last(&s_disp_drv);
}

static void lcd_direct_mode_copy_cb(void)
{
    lv_disp_t *disp_refr = _lv_refr_get_disp_refreshing();
    if (!disp_refr || !disp_refr->driver || !disp_refr->driver->draw_buf) {
        return;
    }

    uint8_t *buf_act = disp_refr->driver->draw_buf->buf_act;
    uint8_t *buf1 = disp_refr->driver->draw_buf->buf1;
    uint8_t *buf2 = disp_refr->driver->draw_buf->buf2;
    const int h_res = disp_refr->driver->hor_res;

    if (!buf_act || !buf1 || !buf2 || h_res <= 0) {
        return;
    }
    uint8_t *fb_from = buf_act;
    uint8_t *fb_to = (fb_from == buf1) ? buf2 : buf1;
    const uint32_t bytes_per_line = (uint32_t)h_res * sizeof(lv_color_t);

    for (int32_t i = 0; i < disp_refr->inv_p; ++i) {
        if (disp_refr->inv_area_joined[i] != 0) {
            continue;
        }
        const lv_coord_t y_start = disp_refr->inv_areas[i].y1;
        const lv_coord_t y_end = disp_refr->inv_areas[i].y2 + 1;
        uint8_t *from = fb_from + ((uint32_t)y_start * (uint32_t)h_res) *
                                  sizeof(lv_color_t);
        uint8_t *to = fb_to + ((uint32_t)y_start * (uint32_t)h_res) *
                              sizeof(lv_color_t);
        for (lv_coord_t y = y_start; y < y_end; ++y) {
            memcpy(to, from, bytes_per_line);
            from += bytes_per_line;
            to += bytes_per_line;
        }

#if D1L_ENABLE_QUALIFICATION_HOOKS
        copy_rect_to_capture_shadow(0, y_start, D1L_UI_CAPTURE_WIDTH, y_end,
                                    fb_to, 0, 0, h_res);
#endif
        uint8_t *flush_ptr = fb_to + (uint32_t)y_start * bytes_per_line;
        const uint32_t bytes_to_flush = (uint32_t)(y_end - y_start) * bytes_per_line;
        Cache_WriteBack_Addr((uint32_t)flush_ptr, bytes_to_flush);
    }
}
#endif

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_err_t ret = bsp_lcd_flush(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
#if !CONFIG_LCD_LVGL_DIRECT_MODE && D1L_ENABLE_QUALIFICATION_HOOKS
    copy_rect_to_capture_shadow(area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                                (const uint8_t *)color_map, area->x1, area->y1,
                                area->x2 - area->x1 + 1);
#endif
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "flush failed: %s", esp_err_to_name(ret));
    }
    if (ret != ESP_OK || !s_bsp_flush_ready_registered) {
        lv_disp_flush_ready(drv);
    }
}

static lv_coord_t clamp_touch_coord(int32_t value, lv_coord_t max_value)
{
    if (value < 0) {
        return 0;
    }
    if (value > max_value) {
        return max_value;
    }
    return (lv_coord_t)value;
}

static bool touch_sample_has_valid_point(const d1l_board_touch_state_t *sample)
{
    return sample &&
           sample->coordinate_valid &&
           sample->x < CONFIG_LCD_EVB_SCREEN_WIDTH &&
           sample->y < CONFIG_LCD_EVB_SCREEN_HEIGHT;
}

static void publish_touch_state(const d1l_board_touch_state_t *sample)
{
    portENTER_CRITICAL(&s_touch_lock);
    s_touch_state = *sample;
    s_touch_state_ready = true;
    portEXIT_CRITICAL(&s_touch_lock);
}

static bool read_cached_touch_state(d1l_board_touch_state_t *out_sample)
{
    bool ready;

    portENTER_CRITICAL(&s_touch_lock);
    ready = s_touch_state_ready;
    if (ready) {
        *out_sample = s_touch_state;
    }
    portEXIT_CRITICAL(&s_touch_lock);

    return ready;
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static lv_coord_t last_x = 0;
    static lv_coord_t last_y = 0;
    static d1l_board_touch_state_t sample;

    data->state = LV_INDEV_STATE_REL;
    data->point.x = last_x;
    data->point.y = last_y;

    memset(&sample, 0, sizeof(sample));
    if (!read_cached_touch_state(&sample)) {
        return;
    }
    if (sample.pressed && touch_sample_has_valid_point(&sample)) {
        data->state = LV_INDEV_STATE_PR;
        last_x = clamp_touch_coord((int32_t)CONFIG_LCD_EVB_SCREEN_WIDTH - 1 - (int32_t)sample.x,
                                   CONFIG_LCD_EVB_SCREEN_WIDTH - 1);
        last_y = clamp_touch_coord((int32_t)CONFIG_LCD_EVB_SCREEN_HEIGHT - 1 - (int32_t)sample.y,
                                   CONFIG_LCD_EVB_SCREEN_HEIGHT - 1);
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

static void label_set_fmt(lv_obj_t *label, const char *fmt, ...)
{
    if (!label || !fmt) {
        return;
    }
    char text[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    lv_label_set_text(label, text);
}

static void label_set_dot_width(lv_obj_t *label, lv_coord_t width)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
}

static void obj_align_if(lv_obj_t *obj, lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs)
{
    if (!obj) {
        return;
    }
    lv_obj_align(obj, align, x_ofs, y_ofs);
}

static void obj_set_pos_if(lv_obj_t *obj, lv_coord_t x, lv_coord_t y)
{
    if (!obj) {
        return;
    }
    lv_obj_set_pos(obj, x, y);
}

static void obj_set_style_text_font_if(lv_obj_t *obj, const lv_font_t *font)
{
    if (!obj || !font) {
        return;
    }
    lv_obj_set_style_text_font(obj, font, 0);
}

static lv_obj_t *create_screen_object(const char *name)
{
    lv_obj_t *obj = lv_obj_create(NULL);
    if (!obj) {
        ESP_LOGE(TAG, "%s allocation failed", name ? name : "screen");
    }
    return obj;
}

static lv_obj_t *create_object(lv_obj_t *parent, const char *name)
{
    if (!parent) {
        ESP_LOGE(TAG, "%s parent missing", name ? name : "object");
        return NULL;
    }
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) {
        ESP_LOGE(TAG, "%s allocation failed", name ? name : "object");
    }
    return obj;
}

static lv_obj_t *create_label_object(lv_obj_t *parent, const char *name)
{
    if (!parent) {
        ESP_LOGE(TAG, "%s parent missing", name ? name : "label");
        return NULL;
    }
    lv_obj_t *label = lv_label_create(parent);
    if (!label) {
        ESP_LOGE(TAG, "%s allocation failed", name ? name : "label");
    }
    return label;
}

static lv_obj_t *create_textarea(lv_obj_t *parent, const char *name)
{
    if (!parent) {
        ESP_LOGE(TAG, "%s parent missing", name ? name : "textarea");
        return NULL;
    }
    lv_obj_t *textarea = lv_textarea_create(parent);
    if (!textarea) {
        ESP_LOGE(TAG, "%s allocation failed", name ? name : "textarea");
    }
    return textarea;
}

static lv_obj_t *create_keyboard(lv_obj_t *parent, const char *name)
{
    if (!parent) {
        ESP_LOGE(TAG, "%s parent missing", name ? name : "keyboard");
        return NULL;
    }
    lv_obj_t *keyboard = lv_keyboard_create(parent);
    if (!keyboard) {
        ESP_LOGE(TAG, "%s allocation failed", name ? name : "keyboard");
    }
    return keyboard;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, uint32_t color)
{
    if (!parent || !text) {
        return NULL;
    }
    lv_obj_t *label = create_label_object(parent, "label");
    if (!label) {
        return NULL;
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, &d1l_ui_font_symbols_14, 0);
    return label;
}

static lv_obj_t *create_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    if (!parent) {
        return NULL;
    }
    lv_obj_t *panel = create_object(parent, "panel");
    if (!panel) {
        return NULL;
    }
    lv_obj_set_size(panel, w, h);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x263241), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t *create_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                               lv_event_cb_t cb, void *user_data)
{
    if (!parent || !text) {
        return NULL;
    }
    lv_obj_t *button = lv_btn_create(parent);
    if (!button) {
        ESP_LOGE(TAG, "button allocation failed");
        return NULL;
    }
    lv_obj_set_size(button, w, h);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1E2A36), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x263545), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = create_label_object(button, "button label");
    if (label) {
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(0xF4F7FB), 0);
        lv_obj_center(label);
    } else {
        ESP_LOGE(TAG, "button label allocation failed");
    }
    if (cb) {
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
    }
    return button;
}

static lv_obj_t *create_dock_button(lv_obj_t *parent,
                                    const d1l_ui_dock_item_t *item,
                                    int x,
                                    lv_event_cb_t cb)
{
    if (!parent || !item || !item->label || !item->icon) {
        return NULL;
    }
    lv_obj_t *button = lv_btn_create(parent);
    if (!button) {
        ESP_LOGE(TAG, "dock button allocation failed");
        return NULL;
    }
    lv_obj_set_size(button, 88, 44);
    lv_obj_set_pos(button, x, 4);
    lv_obj_set_style_radius(button, 7, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x101B25), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1D303E), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x263545), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 1, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(button, lv_color_hex(0x5EEAD4), LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);

    /* The symbol is never the only cue: every compact tab keeps visible text. */
    lv_obj_t *icon = create_label(button, item->icon, 0x8EA0AE);
    if (icon) {
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 3);
    }
    lv_obj_t *label = create_label(button, item->label, 0xF4F7FB);
    if (label) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, 82);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -3);
    }
    if (cb) {
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, (void *)&item->tab);
    }
    return button;
}

static void update_dock_selected_state(void)
{
    const d1l_ui_tab_t active = d1l_ui_navigation_active();
    size_t item_count = 0U;
    const d1l_ui_dock_item_t *items = active_dock_items(&item_count);
    for (size_t i = 0; i < item_count; ++i) {
        if (!s_dock_buttons[i]) {
            continue;
        }
        if (items[i].tab == active) {
            lv_obj_add_state(s_dock_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_dock_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void style_contact_option_button(lv_obj_t *button, uint32_t accent,
                                        const char *detail)
{
    if (!button) {
        return;
    }
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label) {
        lv_obj_set_style_text_color(label, lv_color_hex(accent), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    }
    lv_obj_t *detail_label = create_label(button, detail ? detail : "", 0x8EA0AE);
    if (detail_label) {
        lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(detail_label, 220);
        lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(detail_label, LV_ALIGN_RIGHT_MID, -12, 0);
    }
}

static void show_toast(const char *action, esp_err_t ret)
{
    if (!s_toast) {
        return;
    }
    if (ret == ESP_OK) {
        label_set_fmt(s_toast, "%s queued", action);
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x12362F), 0);
        lv_obj_set_style_border_color(s_toast, lv_color_hex(0x5EEAD4), 0);
    } else {
        label_set_fmt(s_toast, "%s: %s", action, esp_err_to_name(ret));
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x3A1720), 0);
        lv_obj_set_style_border_color(s_toast, lv_color_hex(0xF87171), 0);
    }
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    s_toast_until = lv_tick_get() + 3000U;
}

static void show_toast_text(const char *text, bool ok)
{
    if (!s_toast) {
        return;
    }
    lv_label_set_text(s_toast, text);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(ok ? 0x12362F : 0x3A1720), 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(ok ? 0x5EEAD4 : 0xF87171), 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    s_toast_until = lv_tick_get() + 3000U;
}

static void set_dock_hidden(bool hidden)
{
    if (!s_dock) {
        return;
    }
    if (lv_obj_has_flag(s_dock, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    }
    request_full_screen_repaint();
}

static void restore_dock_for_active_tab(void)
{
    if (d1l_ui_modal_has_active()) {
        set_dock_hidden(true);
        return;
    }
    const d1l_ui_chrome_layout_t layout =
        d1l_ui_chrome_layout_for_screen(d1l_ui_navigation_active());
    set_dock_hidden(!layout.dock_visible);
}

static void show_modal(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    if (d1l_ui_navigation_active() == D1L_UI_TAB_MAP) {
        d1l_ui_map_viewport_prepare_cover();
    }
    d1l_ui_map_viewport_release();
    set_dock_hidden(true);
    d1l_ui_modal_show(obj);
}

static void layout_content_for_active_tab(void)
{
    if (s_content) {
        const d1l_ui_chrome_layout_t layout =
            d1l_ui_chrome_layout_for_screen(d1l_ui_navigation_active());
        lv_obj_set_pos(s_content, 0, layout.content_y);
        lv_obj_set_size(s_content, 480, layout.content_height);
        d1l_ui_screen_configure_content_root(s_content, layout.content_scrollable);
    }
    update_dock_selected_state();
    restore_dock_for_active_tab();
}

static void hide_sheet(void)
{
    d1l_ui_modal_hide(s_sheet);
    restore_dock_for_active_tab();
}

static void hide_compose_sheet(void)
{
    d1l_ui_modal_hide(s_compose_sheet);
#if D1L_ENABLE_QUALIFICATION_HOOKS
    s_onboarding_probe_suppressed = false;
    s_compose_probe_send_suppressed = false;
#endif
    restore_dock_for_active_tab();
    s_compose_dm = false;
    s_compose_channel_id = 0U;
    s_compose_channel_name[0] = '\0';
    s_compose_last_send_error = ESP_OK;
    memset(&s_compose_contact, 0, sizeof(s_compose_contact));
    request_full_screen_repaint();
}

static void hide_public_history_sheet(void)
{
    d1l_ui_modal_hide(s_public_history_sheet);
    s_public_history_limit = D1L_PUBLIC_HISTORY_UI_INITIAL_ROWS;
    s_public_history_channel_id = 0U;
    s_public_history_channel_name[0] = '\0';
    restore_dock_for_active_tab();
}

static void hide_public_search_sheet(void)
{
    d1l_ui_modal_hide(s_public_search_sheet);
    restore_dock_for_active_tab();
}

static void hide_dm_search_sheet(void)
{
    d1l_ui_modal_hide(s_dm_search_sheet);
    restore_dock_for_active_tab();
}

static void hide_message_detail_sheet(void)
{
    d1l_ui_modal_hide(s_message_detail_sheet);
    s_message_detail_advanced = false;
    memset(&s_message_detail_message, 0, sizeof(s_message_detail_message));
    restore_dock_for_active_tab();
}

static void hide_dm_thread_sheet(void)
{
    d1l_ui_modal_hide(s_dm_search_sheet);
    d1l_ui_messages_hide_thread(&s_messages_controller);
    restore_dock_for_active_tab();
}

static void hide_radio_settings_sheet(void)
{
    d1l_ui_radio_settings_deactivate(&s_radio_settings_controller);
    d1l_ui_modal_hide(
        d1l_ui_radio_settings_sheet(&s_radio_settings_controller));
    restore_dock_for_active_tab();
}

static void hide_storage_sheet(void)
{
    d1l_ui_modal_hide(s_storage_sheet);
    s_storage_page = D1L_STORAGE_PAGE_ROOT;
    restore_dock_for_active_tab();
}

static void hide_wifi_sheet(void)
{
    d1l_ui_wifi_deactivate(&s_wifi_controller);
    d1l_ui_modal_hide(d1l_ui_wifi_sheet(&s_wifi_controller));
    restore_dock_for_active_tab();
}

static void hide_ble_sheet(void)
{
    d1l_ui_ble_deactivate(&s_ble_controller);
    d1l_ui_modal_hide(d1l_ui_ble_sheet(&s_ble_controller));
    restore_dock_for_active_tab();
}

static void hide_display_sheet(void)
{
    d1l_ui_device_sheets_hide_display(&s_device_sheets_controller);
    restore_dock_for_active_tab();
}

static void hide_diagnostics_sheet(void)
{
    d1l_ui_device_sheets_hide_diagnostics(&s_device_sheets_controller);
    restore_dock_for_active_tab();
}

static void clear_admin_cli_confirmation(void)
{
    d1l_meshcore_admin_secure_zero(
        s_admin_cli_armed, sizeof(s_admin_cli_armed));
    s_admin_cli_armed_origin = D1L_UI_ADMIN_CLI_ORIGIN_NONE;
    s_admin_cli_deadline = 0U;
}

static bool admin_cli_allowed_for_current_session(const char *command)
{
    d1l_meshcore_admin_snapshot_t status = {0};
    d1l_meshcore_service_admin_snapshot(&status);
    return status.state == D1L_MESHCORE_ADMIN_AUTHENTICATED &&
           d1l_meshcore_admin_cli_command_allowed(
               command, status.role, status.permissions);
}

static void arm_admin_cli_command(
    const char *command, d1l_ui_admin_cli_origin_t origin)
{
    clear_admin_cli_confirmation();
    if (!command || origin == D1L_UI_ADMIN_CLI_ORIGIN_NONE) {
        return;
    }
    snprintf(s_admin_cli_armed, sizeof(s_admin_cli_armed), "%s", command);
    s_admin_cli_armed_origin = origin;
    s_admin_cli_deadline =
        lv_tick_get() + D1L_UI_ADMIN_MUTATION_CONFIRM_WINDOW_MS;
}

static void hide_service_sheets(void)
{
    d1l_ui_service_sheets_hide_all(&s_service_sheets_controller);
    s_terminal_clear_armed = false;
    s_update_install_armed = false;
    s_update_reboot_armed = false;
    s_admin_mutation_armed = D1L_MESHCORE_ADMIN_MUTATION_NONE;
    clear_admin_cli_confirmation();
    s_admin_cli_secure_input = false;
    s_admin_rendered_generation_valid = false;
    restore_dock_for_active_tab();
}

static void hide_map_location_sheet(void)
{
    d1l_ui_map_sheets_hide_location(&s_map_sheets_controller);
    restore_dock_for_active_tab();
}

static void hide_map_options_sheet(void)
{
    d1l_ui_map_sheets_hide_options(&s_map_sheets_controller);
    restore_dock_for_active_tab();
}

static void hide_contact_edit_sheet(void)
{
    d1l_ui_contact_sheets_hide_edit(&s_contact_sheets_controller);
    restore_dock_for_active_tab();
}

static void hide_contact_options_sheet(void)
{
    d1l_ui_contact_sheets_hide_options(&s_contact_sheets_controller);
    restore_dock_for_active_tab();
}

static void hide_contact_forget_sheet(void)
{
    d1l_ui_contact_sheets_hide_forget(&s_contact_sheets_controller);
    restore_dock_for_active_tab();
}

static void hide_contact_detail_sheet(void)
{
    d1l_ui_contact_sheets_hide_all(&s_contact_sheets_controller, true);
    d1l_ui_modal_hide(s_route_trace_sheet);
    hide_node_detail_sheet();
    memset(&s_route_trace_contact, 0, sizeof(s_route_trace_contact));
    restore_dock_for_active_tab();
}

static void hide_contact_export_sheet(void)
{
    d1l_ui_contact_sheets_hide_export(&s_contact_sheets_controller);
    restore_dock_for_active_tab();
}

static void hide_channel_management_sheets(void)
{
    d1l_ui_channel_sheets_hide_all(&s_channel_sheets_controller, true);
    restore_dock_for_active_tab();
}

static void hide_node_detail_sheet(void)
{
    d1l_ui_node_detail_deactivate(&s_node_detail_controller);
    restore_dock_for_active_tab();
}

static void hide_route_detail_sheet(void)
{
    d1l_ui_modal_hide(s_route_detail_sheet);
    memset(&s_route_detail_route, 0, sizeof(s_route_detail_route));
    restore_dock_for_active_tab();
}

static void hide_route_trace_sheet(void)
{
    d1l_ui_modal_hide(s_route_trace_sheet);
    memset(&s_route_trace_contact, 0, sizeof(s_route_trace_contact));
    restore_dock_for_active_tab();
}

static void hide_finder_sheet(void)
{
    d1l_ui_modal_hide(s_finder_sheet);
    restore_dock_for_active_tab();
}

static void hide_packet_detail_sheet(void)
{
    d1l_ui_modal_hide(s_packet_detail_sheet);
    memset(&s_packet_detail_packet, 0, sizeof(s_packet_detail_packet));
    restore_dock_for_active_tab();
}

static void hide_packet_search_sheet(void)
{
    d1l_ui_modal_hide(s_packet_search_sheet);
    restore_dock_for_active_tab();
}

static void hide_mesh_roles_sheet(void)
{
    d1l_ui_modal_hide(s_mesh_roles_sheet);
    s_mesh_roles_page = D1L_MESH_ROLES_PAGE_ROOT;
    restore_dock_for_active_tab();
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void hide_onboarding_sheet(void)
{
    d1l_ui_modal_hide(s_onboarding_sheet);
    s_onboarding_visible = false;
    restore_dock_for_active_tab();
    request_full_screen_repaint();
}

static void update_onboarding_visibility(const d1l_app_snapshot_t *snapshot)
{
    if (!snapshot || !s_onboarding_sheet) {
        return;
    }
    if (snapshot->onboarding_complete) {
#if D1L_ENABLE_QUALIFICATION_HOOKS
        s_onboarding_probe_suppressed = false;
#endif
        hide_onboarding_sheet();
        return;
    }
#if D1L_ENABLE_QUALIFICATION_HOOKS
    if (s_onboarding_probe_suppressed) {
        hide_onboarding_sheet();
        return;
    }
#endif
    s_onboarding_visible = true;
    show_modal(s_onboarding_sheet);
    request_full_screen_repaint();
}
#endif

static void update_startup_overlay(const d1l_app_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
#if D1L_ENABLE_QUALIFICATION_HOOKS
    update_onboarding_visibility(snapshot);
#else
    d1l_ui_first_start_update(&s_first_start_controller, snapshot);
    s_onboarding_visible =
        d1l_ui_first_start_visible(&s_first_start_controller);
#endif
}

static void set_object_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void home_view_input_from_snapshot(
    const d1l_app_snapshot_t *snapshot,
    d1l_ui_home_view_input_t *out_input)
{
    if (!snapshot || !out_input) {
        return;
    }
    *out_input = (d1l_ui_home_view_input_t) {
        .public_unread_count = snapshot->public_unread_count,
        .dm_unread_count = snapshot->dm_unread_count,
        .muted_dm_unread_count = snapshot->muted_dm_unread_count,
        .contact_count = snapshot->contact_count,
        .node_count = snapshot->node_count,
        .packet_count = snapshot->packet_count,
        .map_location_set = snapshot->map_location_set,
        .map_tile_cache_ready = snapshot->map_tile_cache_ready,
        .mesh_state = snapshot->mesh_state,
        .radio_ready = snapshot->radio_ready,
        .radio_applied = snapshot->radio_applied,
        .radio_apply_pending = snapshot->radio_apply_pending,
        .wifi_connected = snapshot->wifi_connected,
        .wifi_enabled = snapshot->wifi_enabled,
        .wifi_connecting = snapshot->wifi_connecting,
        .ble_build_enabled = snapshot->ble_build_enabled,
        .ble_transport_supported = snapshot->ble_transport_supported,
        .ble_companion_enabled = snapshot->ble_companion_enabled,
        .time_available = snapshot->time_available,
        .time_label = snapshot->time_label,
        .storage_retained_backup_degraded =
            snapshot->storage_retained_backup_degraded,
        .storage_retained_sd_degraded = snapshot->storage_retained_sd_degraded,
        .storage_data_enabled = snapshot->storage_data_enabled,
        .storage_sd_data_root_ready = snapshot->storage_sd_data_root_ready,
        .storage_setup_required = snapshot->storage_setup_required,
        .storage_sd_needs_fat32 = snapshot->storage_sd_needs_fat32,
        .storage_sd_state = snapshot->storage_sd_state,
        .storage_setup_action = snapshot->storage_setup_action,
        .node_store_backend = snapshot->node_store_backend,
    };
    d1l_ui_home_view_apply_release_profile(out_input);
}

static void more_view_input_from_snapshot(
    const d1l_app_snapshot_t *snapshot,
    d1l_ui_more_view_input_t *out_input)
{
    if (!snapshot || !out_input) {
        return;
    }
    d1l_observer_status_t observer = {0};
    d1l_observer_status(&observer);
    d1l_update_status_t update = {0};
    d1l_update_status(&update);
    d1l_meshcore_admin_snapshot_t admin = {0};
    d1l_meshcore_service_admin_snapshot(&admin);
    const d1l_event_log_status_t terminal = d1l_event_log_status();
    const uint64_t unread_total =
        (uint64_t)snapshot->public_unread_count +
        (uint64_t)snapshot->dm_unread_count;
    const char *admin_state =
        admin.state == D1L_MESHCORE_ADMIN_AUTHENTICATED ? "authenticated" :
        admin.state == D1L_MESHCORE_ADMIN_LOGIN_PENDING ? "login pending" :
        admin.state == D1L_MESHCORE_ADMIN_STATUS_PENDING ? "refreshing" :
        admin.state == D1L_MESHCORE_ADMIN_MUTATION_PENDING ? "applying" :
        admin.state == D1L_MESHCORE_ADMIN_CLI_PENDING ? "command pending" :
        admin.state == D1L_MESHCORE_ADMIN_QUERY_PENDING ? "query pending" :
        admin.state == D1L_MESHCORE_ADMIN_TIMED_OUT ? "timed out" :
        admin.state == D1L_MESHCORE_ADMIN_REJECTED_CREDENTIALS ?
            "credentials rejected" :
        admin.state == D1L_MESHCORE_ADMIN_DISCONNECTED ? "disconnected" :
        admin.state == D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL ?
            "unsupported protocol" :
        admin.state == D1L_MESHCORE_ADMIN_RADIO_BUSY ? "radio busy" :
        admin.state == D1L_MESHCORE_ADMIN_VOLATILE_REPLAY_REJECTED ?
            "recent login replay rejected" :
        admin.state == D1L_MESHCORE_ADMIN_DURABLE_REPLAY_REJECTED ?
            "saved login replay rejected" :
        admin.state == D1L_MESHCORE_ADMIN_LOCAL_STORAGE_FAILED ?
            "local storage failed" : "idle";
    *out_input = (d1l_ui_more_view_input_t) {
        .packet_count = snapshot->packet_count,
        .wifi_build_enabled = snapshot->wifi_build_enabled,
        .wifi_enabled = snapshot->wifi_enabled,
        .wifi_connected = snapshot->wifi_connected,
        .wifi_connecting = snapshot->wifi_connecting,
        .ble_build_enabled = snapshot->ble_build_enabled,
        .ble_transport_supported = snapshot->ble_transport_supported,
        .ble_companion_enabled = snapshot->ble_companion_enabled,
        .radio_ready = snapshot->radio_ready,
        .radio_applied = snapshot->radio_applied,
        .radio_apply_pending = snapshot->radio_apply_pending,
        .storage_sd_present = snapshot->storage_sd_present,
        .storage_sd_data_root_ready = snapshot->storage_sd_data_root_ready,
        .storage_sd_needs_fat32 = snapshot->storage_sd_needs_fat32,
        .storage_setup_required = snapshot->storage_setup_required,
        .storage_data_enabled = snapshot->storage_data_enabled,
        .storage_retained_sd_degraded = snapshot->storage_retained_sd_degraded,
        .storage_retained_backup_degraded =
            snapshot->storage_retained_backup_degraded,
        .map_location_set = snapshot->map_location_set,
        .map_tile_cache_ready = snapshot->map_tile_cache_ready,
        .map_tile_render_supported = snapshot->map_tile_render_supported,
        .identity_ready = snapshot->identity_ready,
        .terminal_event_count = (uint32_t)terminal.count,
        .notification_unread_count =
            unread_total > UINT32_MAX ? UINT32_MAX : (uint32_t)unread_total,
        .observer_state = d1l_observer_state_name(observer.state),
        .update_state = d1l_update_state_name(update.state),
        .admin_state = admin_state,
        .time_available = snapshot->time_available,
        .time_approximate = snapshot->time_approximate,
        .timezone_settings_ready = snapshot->timezone_settings_ready,
        .storage_sd_state = snapshot->storage_sd_state,
        .storage_setup_action = snapshot->storage_setup_action,
        .time_label = snapshot->time_label,
        .timezone_label = snapshot->timezone_label,
        .firmware_version = D1L_FIRMWARE_VERSION,
    };
    d1l_ui_more_view_apply_release_profile(out_input);
}

static void storage_view_input_from_snapshot(
    const d1l_app_snapshot_t *snapshot,
    d1l_ui_storage_view_input_t *out_input)
{
    if (!snapshot || !out_input) {
        return;
    }
    *out_input = (d1l_ui_storage_view_input_t) {
        .rp2040_bridge_required = snapshot->storage_rp2040_bridge_required,
        .rp2040_bridge_ready = snapshot->storage_rp2040_bridge_ready,
        .rp2040_sd_protocol_supported =
            snapshot->storage_rp2040_sd_protocol_supported,
        .sd_present = snapshot->storage_sd_present,
        .sd_mounted = snapshot->storage_sd_mounted,
        .sd_data_root_ready = snapshot->storage_sd_data_root_ready,
        .sd_needs_fat32 = snapshot->storage_sd_needs_fat32,
        .data_enabled = snapshot->storage_data_enabled,
        .retained_sd_degraded = snapshot->storage_retained_sd_degraded,
        .retained_backup_degraded = snapshot->storage_retained_backup_degraded,
        .capacity_kb = snapshot->storage_capacity_kb,
        .free_kb = snapshot->storage_free_kb,
        .sd_state = snapshot->storage_sd_state,
        .sd_filesystem = snapshot->storage_sd_filesystem,
        .setup_action = snapshot->storage_setup_action,
        .message_store_backend = snapshot->message_store_backend,
        .dm_store_backend = snapshot->dm_store_backend,
        .packet_log_backend = snapshot->packet_log_backend,
        .route_store_backend = snapshot->route_store_backend,
        .node_store_backend = snapshot->node_store_backend,
        .contact_store_backend = snapshot->contact_store_backend,
        .read_state_backend = snapshot->read_state_backend,
        .map_tile_backend = snapshot->map_tile_backend,
        .export_backend = snapshot->export_backend,
    };
}

static void update_chrome(const d1l_app_snapshot_t *snapshot)
{
    if (!snapshot || !s_title_label || !s_status_label || !s_identity_label) {
        return;
    }
    const d1l_ui_chrome_layout_t layout =
        d1l_ui_chrome_layout_for_screen(d1l_ui_navigation_active());
    lv_label_set_text(s_title_label, layout.title);
    if (s_top_bar) {
        lv_obj_set_size(s_top_bar, 480, layout.content_y);
        lv_obj_set_style_pad_all(s_top_bar, layout.header_detail_visible ? 8 : 1, 0);
    }
    set_object_hidden(s_title_label, layout.content_y == 0U);
    if (layout.header_detail_visible) {
        lv_obj_align(s_title_label, LV_ALIGN_LEFT_MID, 2, 0);
    } else {
        lv_obj_align(s_title_label, LV_ALIGN_TOP_LEFT, 10, 0);
    }
    set_object_hidden(s_status_label, !layout.header_detail_visible);
    set_object_hidden(s_identity_label, !layout.header_detail_visible);
    set_object_hidden(s_lock_button, !layout.header_detail_visible);
    if (!layout.header_detail_visible) {
        return;
    }
    label_set_fmt(s_status_label, "%s  Mesh %s",
                  snapshot->time_label[0] ? snapshot->time_label : "--:--",
                  snapshot->mesh_state ? snapshot->mesh_state : "starting");
    d1l_ui_home_view_input_t home_input = {0};
    home_view_input_from_snapshot(snapshot, &home_input);
    const bool connectivity_controls_available =
        d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL) ||
        d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE);
    if (!connectivity_controls_available) {
        if (d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY)) {
            label_set_fmt(s_identity_label, "NVS retained  SD %s",
                          d1l_ui_home_sd_state(&home_input));
        } else {
            label_set_fmt(s_identity_label, "NVS %s  USB recovery",
                          snapshot->storage_data_enabled ?
                              "retained" : "ready");
        }
    } else {
        label_set_fmt(s_identity_label, "Wi-Fi %s  BLE %s  SD %s",
                      snapshot->wifi_state ? snapshot->wifi_state : "off",
                      snapshot->ble_state ? snapshot->ble_state : "off",
                      d1l_ui_home_sd_state(&home_input));
    }
}

static void format_snr_tenths(char *dest, size_t dest_size, int snr_tenths)
{
    const int snr_abs = snr_tenths < 0 ? -snr_tenths : snr_tenths;
    snprintf(dest, dest_size, "%s%d.%d", snr_tenths < 0 ? "-" : "",
             snr_abs / 10, snr_abs % 10);
}

static void map_location_from_snapshot(const d1l_app_snapshot_t *snapshot)
{
    if (snapshot && snapshot->map_location_set) {
        s_map_location_lat_e7 = snapshot->map_lat_e7;
        s_map_location_lon_e7 = snapshot->map_lon_e7;
        return;
    }
    s_map_location_lat_e7 = 0;
    s_map_location_lon_e7 = 0;
}

static bool parse_coord_text_e7(const char *text, int32_t min_value, int32_t max_value,
                                int32_t *out_value)
{
    if (!text || !out_value) {
        return false;
    }
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    bool negative = false;
    if (*text == '-' || *text == '+') {
        negative = (*text == '-');
        ++text;
    }

    int64_t whole = 0;
    int64_t fraction = 0;
    uint8_t fraction_digits = 0;
    bool saw_digit = false;
    while (*text >= '0' && *text <= '9') {
        saw_digit = true;
        whole = (whole * 10) + (*text - '0');
        if (whole > 180) {
            return false;
        }
        ++text;
    }
    if (*text == '.') {
        ++text;
        while (*text >= '0' && *text <= '9') {
            saw_digit = true;
            if (fraction_digits < 7U) {
                fraction = (fraction * 10) + (*text - '0');
                fraction_digits++;
            }
            ++text;
        }
    }
    while (fraction_digits < 7U) {
        fraction *= 10;
        fraction_digits++;
    }
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    if (!saw_digit || *text != '\0') {
        return false;
    }

    int64_t scaled = (whole * 10000000LL) + fraction;
    if (negative) {
        scaled = -scaled;
    }
    if (scaled < min_value || scaled > max_value) {
        return false;
    }
    *out_value = (int32_t)scaled;
    return true;
}

static d1l_ui_dm_identity_eligibility_t dm_identity_for_contact(
    const d1l_contact_entry_t *entry,
    d1l_contact_entry_t *out_current)
{
    if (out_current) {
        memset(out_current, 0, sizeof(*out_current));
    }
    d1l_contact_entry_t current = {0};
    const bool found = entry && entry->public_key_hex[0] != '\0' &&
        d1l_app_model_find_contact_by_public_key(
            entry->public_key_hex, &current) == ESP_OK;
    const d1l_ui_dm_identity_input_t input = {
        .source = D1L_UI_DM_IDENTITY_SOURCE_CONTACT,
        .fingerprint = entry ? entry->fingerprint : NULL,
        .public_key_hex = entry ? entry->public_key_hex : NULL,
        .resolved_contact = found ? &current : NULL,
        .contact_found_by_full_key = found,
    };
    const d1l_ui_dm_identity_eligibility_t eligibility =
        d1l_ui_dm_identity_eligibility(&input);
    if (eligibility.can_open_compose && out_current) {
        *out_current = current;
    }
    return eligibility;
}

static d1l_ui_dm_identity_eligibility_t dm_identity_for_node(
    const d1l_node_view_t *view,
    d1l_contact_entry_t *out_current)
{
    if (out_current) {
        memset(out_current, 0, sizeof(*out_current));
    }
    d1l_contact_entry_t current = {0};
    const bool found = view && view->node.public_key_hex[0] != '\0' &&
        d1l_app_model_find_contact_by_public_key(
            view->node.public_key_hex, &current) == ESP_OK;
    const d1l_ui_dm_identity_input_t input = {
        .source = D1L_UI_DM_IDENTITY_SOURCE_HEARD_NODE,
        .fingerprint = view ? view->node.fingerprint : NULL,
        .public_key_hex = view ? view->node.public_key_hex : NULL,
        .resolved_contact = found ? &current : NULL,
        .contact_found_by_full_key = found,
    };
    const d1l_ui_dm_identity_eligibility_t eligibility =
        d1l_ui_dm_identity_eligibility(&input);
    if (eligibility.can_open_compose && out_current) {
        *out_current = current;
    }
    return eligibility;
}

static void show_dm_identity_reason(
    d1l_ui_dm_identity_eligibility_t eligibility)
{
    char message[96];
    snprintf(message, sizeof(message), "DM blocked [%s]: %.48s",
             d1l_ui_dm_identity_reason_code(eligibility.reason),
             d1l_ui_dm_identity_reason_text(eligibility.reason));
    show_toast_text(message, false);
}

static bool node_view_can_dm(const d1l_node_view_t *view)
{
    return dm_identity_for_node(view, NULL).can_open_compose;
}

static bool contact_can_dm(const d1l_contact_entry_t *entry)
{
    return dm_identity_for_contact(entry, NULL).can_open_compose;
}

static bool contact_can_export(const d1l_contact_entry_t *entry)
{
    return entry && d1l_contact_store_has_export_key(entry) &&
           d1l_contact_store_meshcore_type_id(entry->type) != 0U;
}

static bool radio_edit_from_snapshot(const d1l_app_snapshot_t *snapshot)
{
    d1l_app_radio_profile_edit_t edit = {0};
    if (!snapshot) {
        d1l_app_model_current_radio_profile(&edit);
        return d1l_ui_radio_settings_set_edit(
            &s_radio_settings_controller, &edit);
    }
    edit.frequency_hz = snapshot->radio_frequency_hz;
    edit.bandwidth_tenths_khz = snapshot->radio_bandwidth_tenths_khz;
    edit.spreading_factor = snapshot->radio_spreading_factor;
    edit.coding_rate = snapshot->radio_coding_rate;
    edit.tx_power_dbm = snapshot->radio_tx_power_dbm;
    edit.rx_boost = snapshot->radio_rx_boost;
    return d1l_ui_radio_settings_set_edit(
        &s_radio_settings_controller, &edit);
}

static void handle_home_action(d1l_ui_home_action_t action, void *context)
{
    (void)context;
    if (!d1l_ui_home_action_available(action)) {
        return;
    }
    switch (action) {
    case D1L_UI_HOME_ACTION_MESSAGES:
        s_messages_mode = D1L_UI_MESSAGES_MODE_ROOT;
        request_tab_switch(D1L_UI_TAB_MESSAGES);
        break;
    case D1L_UI_HOME_ACTION_NETWORK:
        request_tab_switch(D1L_UI_TAB_NODES);
        break;
    case D1L_UI_HOME_ACTION_MAP:
        set_map_interactive_touch_authorized(true);
        request_tab_switch(D1L_UI_TAB_MAP);
        break;
    case D1L_UI_HOME_ACTION_MORE:
        request_tab_switch(D1L_UI_TAB_SETTINGS);
        break;
    case D1L_UI_HOME_ACTION_PACKETS:
        request_tab_switch(D1L_UI_TAB_PACKETS);
        break;
    case D1L_UI_HOME_ACTION_RADIO:
        open_radio_settings_event_cb(NULL);
        break;
    case D1L_UI_HOME_ACTION_WIFI:
        open_wifi_sheet_event_cb(NULL);
        break;
    case D1L_UI_HOME_ACTION_BLE:
        open_ble_sheet_event_cb(NULL);
        break;
    case D1L_UI_HOME_ACTION_STORAGE:
        open_storage_sheet_event_cb(NULL);
        break;
    case D1L_UI_HOME_ACTION_ATTENTION:
        open_diagnostics_sheet_event_cb(NULL);
        break;
    case D1L_UI_HOME_ACTION_NONE:
    default:
        break;
    }
}

static void render_home_screen(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)
{
    d1l_ui_home_view_input_t input = {0};
    home_view_input_from_snapshot(snapshot, &input);
    d1l_ui_home_view(&input, &s_home_controller.rendered);
    d1l_ui_home_render(&s_home_controller, content, &s_home_controller.rendered,
                       handle_home_action, NULL);
}

static void handle_settings_action(d1l_ui_settings_action_t action, void *context)
{
    (void)context;
    if (!d1l_ui_settings_action_available(action)) {
        return;
    }
    switch (action) {
    case D1L_UI_SETTINGS_ACTION_PACKETS:
        request_tab_switch(D1L_UI_TAB_PACKETS);
        break;
    case D1L_UI_SETTINGS_ACTION_STORAGE:
        open_storage_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_WIFI:
        open_wifi_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_BLE:
        open_ble_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_RADIO:
        open_radio_settings_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_MAP_TILES:
        open_map_options_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_DISPLAY:
        open_display_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_DIAGNOSTICS:
        open_diagnostics_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_TERMINAL:
        open_terminal_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_OBSERVER:
        open_observer_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_UPDATE:
        open_update_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_NOTIFICATIONS:
        open_notifications_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_ADMIN:
        s_admin_target_fingerprint[0] = '\0';
        open_admin_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_ADVANCED:
        open_sheet_event_cb(NULL);
        break;
    case D1L_UI_SETTINGS_ACTION_NONE:
    case D1L_UI_SETTINGS_ACTION_COUNT:
    default:
        break;
    }
}

static void render_settings(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)
{
    d1l_ui_more_view_input_t input = {0};
    more_view_input_from_snapshot(snapshot, &input);
    if (!d1l_ui_more_view(&input, &s_settings_controller.rendered)) {
        d1l_ui_settings_deactivate(&s_settings_controller);
        return;
    }
    ++s_settings_render_generation;
    if (s_settings_render_generation == 0U) {
        s_settings_render_generation = 1U;
    }
    (void)d1l_ui_settings_render(&s_settings_controller, content,
                                 &s_settings_controller.rendered,
                                 s_settings_render_generation,
                                 handle_settings_action, NULL);
}

static size_t refresh_packet_terminal_rows(void)
{
    d1l_ui_packets_query_request_t query;
    for (size_t attempt = 0U; attempt < 2U; ++attempt) {
        if (!d1l_ui_packets_query_request(&s_packets_controller, &query)) {
            break;
        }
        size_t total_matches = 0U;
        bool sd_used = false;
        const size_t row_count = d1l_packet_log_query_page(
            s_packet_query_rows, query.row_limit, query.skip_newest, query.direction,
            query.kind, query.search_text, &total_matches, &sd_used);
        if (!d1l_ui_packets_accept_query(&s_packets_controller, s_packet_query_rows, row_count,
                                         total_matches, sd_used)) {
            break;
        }
    }
    return s_packets_controller.row_count;
}

static uint32_t next_row_generation(uint32_t current)
{
    current = (current + 1U) & D1L_UI_ROW_TOKEN_GENERATION_MASK;
    return current == 0U ? 1U : current;
}

static void *row_token(uint32_t generation, size_t index)
{
    const uintptr_t token =
        ((uintptr_t)generation << D1L_UI_ROW_TOKEN_INDEX_BITS) |
        ((uintptr_t)index + 1U);
    return (void *)token;
}

static bool row_token_index(const void *user_data,
                            uint32_t expected_generation,
                            size_t payload_count,
                            size_t *out_index)
{
    const uintptr_t token = (uintptr_t)user_data;
    const uint32_t generation =
        (uint32_t)((token >> D1L_UI_ROW_TOKEN_INDEX_BITS) &
                   D1L_UI_ROW_TOKEN_GENERATION_MASK);
    const size_t encoded_index =
        (size_t)(token & D1L_UI_ROW_TOKEN_INDEX_MASK);
    if (!out_index || generation == 0U ||
        generation != expected_generation || encoded_index == 0U ||
        encoded_index > payload_count) {
        return false;
    }
    *out_index = encoded_index - 1U;
    return true;
}

static void render_packet_row(lv_obj_t *parent,
                              int y,
                              const d1l_packet_log_entry_t *entry,
                              size_t payload_index)
{
    if (!entry) {
        return;
    }
    const uint32_t accent_color = d1l_ui_packets_entry_color(entry);
    char snr[16];
    format_snr_tenths(snr, sizeof(snr), entry->snr_tenths);

    lv_obj_t *row = create_panel(parent, 18, y, 424, 46);
    if (!row) {
        return;
    }
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, open_packet_detail_event_cb, LV_EVENT_CLICKED,
                        row_token(s_packet_row_generation, payload_index));
    lv_obj_set_style_pad_all(row, 7, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(accent_color), 0);

    lv_obj_t *accent = create_object(row, "packet row accent");
    if (accent) {
        lv_obj_set_size(accent, 4, 30);
        lv_obj_align(accent, LV_ALIGN_LEFT_MID, -2, 0);
        lv_obj_set_style_radius(accent, 2, 0);
        lv_obj_set_style_bg_color(accent, lv_color_hex(accent_color), 0);
        lv_obj_set_style_border_width(accent, 0, 0);
    }

    lv_obj_t *status = create_label(row, d1l_ui_packets_entry_status(entry), accent_color);
    label_set_dot_width(status, 38);
    obj_align_if(status, LV_ALIGN_TOP_LEFT, 8, 0);
    lv_obj_t *kind = create_label(row, entry->kind[0] ? entry->kind : "packet", 0xF4F7FB);
    label_set_dot_width(kind, 128);
    obj_align_if(kind, LV_ALIGN_TOP_LEFT, 54, 0);
    lv_obj_t *meta = create_label(row, "", 0x8EA0AE);
    label_set_fmt(meta, "#%lu  rssi %d  snr %s",
                  (unsigned long)entry->seq, entry->rssi_dbm, snr);
    label_set_dot_width(meta, 190);
    obj_align_if(meta, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *note = create_label(row, entry->note[0] ? entry->note : "-", 0xE5EDF5);
    label_set_dot_width(note, 380);
    obj_align_if(note, LV_ALIGN_BOTTOM_LEFT, 8, 0);
}

static void render_route_row(lv_obj_t *parent,
                             int y,
                             const d1l_route_entry_t *entry,
                             size_t payload_index)
{
    lv_obj_t *row = create_panel(parent, 18, y, 424, 48);
    if (!row) {
        return;
    }
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, open_route_detail_event_cb, LV_EVENT_CLICKED,
                        row_token(s_route_row_generation, payload_index));
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_t *label = create_label(row, entry->label[0] ? entry->label : entry->target, 0xA7F3D0);
    label_set_dot_width(label, 176);
    obj_align_if(label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *meta = create_label(row, "", 0x8EA0AE);
    label_set_fmt(meta, "#%lu %s %s", (unsigned long)entry->seq, entry->direction, entry->route);
    label_set_dot_width(meta, 190);
    obj_align_if(meta, LV_ALIGN_TOP_RIGHT, 0, 0);
    const int snr_abs = entry->last_snr_tenths < 0 ? -entry->last_snr_tenths : entry->last_snr_tenths;
    lv_obj_t *signal = create_label(row, "", 0xE5EDF5);
    label_set_fmt(signal, "%s  rssi %d  snr %s%d.%d  hops %u",
                  entry->kind, entry->last_rssi_dbm,
                  entry->last_snr_tenths < 0 ? "-" : "", snr_abs / 10, snr_abs % 10,
                  entry->path_hops);
    label_set_dot_width(signal, 392);
    obj_align_if(signal, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static bool snapshot_find_channel(const d1l_app_snapshot_t *snapshot,
                                  uint64_t channel_id,
                                  d1l_channel_info_t *out_channel)
{
    if (!snapshot || channel_id == 0U) {
        return false;
    }
    size_t count = snapshot->channel_count;
    if (count > D1L_APP_SNAPSHOT_CHANNEL_PREVIEW) {
        count = D1L_APP_SNAPSHOT_CHANNEL_PREVIEW;
    }
    for (size_t i = 0U; i < count; ++i) {
        if (snapshot->channels[i].channel_id != channel_id) {
            continue;
        }
        if (out_channel) {
            *out_channel = snapshot->channels[i];
        }
        return true;
    }
    return false;
}

static uint32_t snapshot_channel_read_seq(
    const d1l_app_snapshot_t *snapshot,
    const d1l_channel_info_t *channel)
{
    if (!snapshot || !channel) {
        return 0U;
    }
    return channel->channel_id == D1L_CHANNEL_PUBLIC_ID ?
        snapshot->last_public_read_seq : channel->read_through_seq;
}

static d1l_ui_compose_eligibility_t compose_eligibility_for_text(
    const d1l_user_text_info_t *text_info)
{
    d1l_channel_info_t channel = {0};
    const bool channel_found = s_compose_dm || snapshot_find_channel(
        &s_snapshot, s_compose_channel_id, &channel);
    bool qualification_send_suppressed = false;
#if D1L_ENABLE_QUALIFICATION_HOOKS
    qualification_send_suppressed = s_compose_probe_send_suppressed;
#endif
    const bool channel_sendable = !qualification_send_suppressed &&
        (s_compose_dm || (channel_found && channel.enabled));
    bool contact_found = !s_compose_dm;
    bool contact_sendable = !s_compose_dm;
    if (s_compose_dm && s_compose_contact.fingerprint[0] != '\0') {
        d1l_contact_entry_t current = {0};
        contact_found = d1l_app_model_find_contact(
            s_compose_contact.fingerprint, &current) == ESP_OK;
        contact_sendable = !qualification_send_suppressed && contact_found &&
            d1l_contact_store_can_dm(&current);
        if (contact_sendable) {
            s_compose_contact = current;
        }
    }
    const d1l_ui_compose_eligibility_input_t input = {
        .is_dm = s_compose_dm,
        .text_result = text_info ? text_info->result :
            D1L_USER_TEXT_INVALID_UTF8,
        .board_ready = s_snapshot.board_ready,
        .mesh_state = s_snapshot.mesh_state,
        .radio_ready = s_snapshot.radio_ready,
        .path_hash_bytes = s_snapshot.path_hash_bytes,
        .protocol_tx_ready = s_snapshot.protocol_tx_ready,
        .settings_load_status = s_snapshot.settings_load_status,
        .identity_state = s_snapshot.identity_state,
        .channel_found = channel_found,
        .channel_sendable = channel_sendable,
        .contact_found = contact_found,
        .contact_sendable = contact_sendable,
        .dm_delivery_active = s_snapshot.dm_delivery_active,
        .previous_send_error = s_compose_last_send_error,
    };
    return d1l_ui_compose_eligibility(&input);
}

static void update_compose_counter(void)
{
    if (!s_compose_counter) {
        return;
    }
    const char *text = s_compose_textarea ? lv_textarea_get_text(s_compose_textarea) : "";
    const size_t used = text ? strlen(text) : 0U;
    const d1l_user_text_info_t info = d1l_user_text_validate(text);
    const d1l_ui_compose_eligibility_t eligibility =
        compose_eligibility_for_text(&info);
    if (info.result == D1L_USER_TEXT_OK &&
        eligibility.reason == D1L_UI_COMPOSE_READY) {
        label_set_fmt(s_compose_counter, "%u chars | %u/%u B",
                      (unsigned)info.character_count, (unsigned)info.byte_count,
                      (unsigned)D1L_MESSAGE_MAX_BYTES);
    } else if (info.result == D1L_USER_TEXT_OK) {
        label_set_fmt(s_compose_counter, "%s | %u/%u B",
                      eligibility.status,
                      (unsigned)info.byte_count,
                      (unsigned)D1L_MESSAGE_MAX_BYTES);
    } else if (info.result == D1L_USER_TEXT_EMPTY) {
        label_set_fmt(s_compose_counter, "0 chars | 0/%u B",
                      (unsigned)D1L_MESSAGE_MAX_BYTES);
    } else if (info.result == D1L_USER_TEXT_TOO_LONG) {
        label_set_fmt(s_compose_counter, "Too long | %u/%u B",
                      (unsigned)used, (unsigned)D1L_MESSAGE_MAX_BYTES);
    } else {
        label_set_fmt(s_compose_counter, "Invalid text | %u/%u B",
                      (unsigned)used, (unsigned)D1L_MESSAGE_MAX_BYTES);
    }
    const bool text_error = info.result != D1L_USER_TEXT_OK &&
        info.result != D1L_USER_TEXT_EMPTY;
    lv_obj_set_style_text_color(s_compose_counter,
                                lv_color_hex(text_error ? 0xF87171 :
                                             eligibility.retry_available ?
                                                 0xFBBF24 : 0x8EA0AE),
                                0);
    if (s_compose_send_button) {
        if (eligibility.send_enabled) {
            lv_obj_clear_state(s_compose_send_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_compose_send_button, LV_STATE_DISABLED);
        }
    }
}

static void layout_compose_sheet_controls(void)
{
    if (s_compose_sheet) {
        lv_obj_set_size(s_compose_sheet, 480, 424);
        lv_obj_set_pos(s_compose_sheet, 0, 56);
        lv_obj_set_style_pad_all(s_compose_sheet, 0, 0);
        lv_obj_clear_flag(s_compose_sheet, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_compose_sheet, LV_DIR_NONE);
        lv_obj_set_scrollbar_mode(s_compose_sheet, LV_SCROLLBAR_MODE_OFF);
        lv_obj_scroll_to_y(s_compose_sheet, 0, LV_ANIM_OFF);
    }
    if (s_compose_textarea) {
        lv_obj_set_size(s_compose_textarea, 448, 78);
        lv_obj_set_pos(s_compose_textarea, 16, 58);
    }
    if (s_compose_counter) {
        lv_obj_set_width(s_compose_counter, 248);
        lv_obj_set_pos(s_compose_counter, 216, 140);
        lv_obj_set_style_text_align(s_compose_counter, LV_TEXT_ALIGN_RIGHT, 0);
    }
    if (s_compose_keyboard) {
        lv_obj_set_size(s_compose_keyboard, 448, 258);
        lv_obj_set_align(s_compose_keyboard, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(s_compose_keyboard, 16, 158);
    }
}

static bool show_channel_compose_sheet(uint64_t channel_id,
                                       const char *title,
                                       const char *placeholder)
{
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT) &&
        channel_id != D1L_CHANNEL_PUBLIC_ID) {
        return false;
    }
    d1l_app_model_snapshot(&s_snapshot);
    d1l_channel_info_t channel = {0};
    if (!snapshot_find_channel(&s_snapshot, channel_id, &channel)) {
        show_toast("Channel", ESP_ERR_NOT_FOUND);
        return false;
    }
    if (!channel.enabled) {
        show_toast("Channel", ESP_ERR_INVALID_STATE);
        return false;
    }
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_message_detail_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_channel_management_sheets();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_finder_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
#if D1L_ENABLE_QUALIFICATION_HOOKS
    s_compose_probe_send_suppressed = false;
#endif
    s_compose_dm = false;
    s_compose_channel_id = channel.channel_id;
    snprintf(s_compose_channel_name, sizeof(s_compose_channel_name), "%s",
             channel.name);
    s_compose_last_send_error = ESP_OK;
    memset(&s_compose_contact, 0, sizeof(s_compose_contact));
    if (s_compose_title) {
        char default_title[64];
        snprintf(default_title, sizeof(default_title), "Compose %.32s",
                 channel.name);
        lv_label_set_text(s_compose_title,
                          title && title[0] ? title : default_title);
    }
    if (s_compose_sheet) {
        show_modal(s_compose_sheet);
    }
    if (s_compose_textarea && s_compose_keyboard) {
        lv_textarea_set_text(s_compose_textarea, "");
        lv_textarea_set_placeholder_text(s_compose_textarea,
                                         placeholder && placeholder[0] ?
                                             placeholder :
                                         channel.channel_id ==
                                                 D1L_CHANNEL_PUBLIC_ID ?
                                             "Public message" :
                                             "Channel message");
        lv_keyboard_set_textarea(s_compose_keyboard, s_compose_textarea);
        layout_compose_sheet_controls();
        update_compose_counter();
    }
    request_full_screen_repaint();
    return true;
}

static void open_compose_event_cb(lv_event_t *event)
{
    (void)event;
    const uint64_t channel_id =
        s_messages_controller.rendered.active_channel_id;
    (void)show_channel_compose_sheet(
        channel_id, NULL,
        channel_id == D1L_CHANNEL_PUBLIC_ID ?
            "Public message" : "Channel message");
}

static void mark_public_read_event_cb(lv_event_t *event)
{
    (void)event;
    const uint64_t channel_id =
        s_messages_controller.rendered.active_channel_id;
    esp_err_t ret = d1l_app_model_mark_channel_read(channel_id);
    show_toast("Channel read", ret);
    if (ret == ESP_OK) {
        request_content_refresh();
    }
}

static void present_dm_compose_sheet(const d1l_contact_entry_t *selected,
                                     bool probe_only)
{
    if (!selected) {
        return;
    }
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_message_detail_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_contact_detail_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
#if D1L_ENABLE_QUALIFICATION_HOOKS
    s_compose_probe_send_suppressed = probe_only;
#else
    (void)probe_only;
#endif
    s_compose_dm = true;
    s_compose_channel_id = 0U;
    s_compose_channel_name[0] = '\0';
    s_compose_last_send_error = ESP_OK;
    s_compose_contact = *selected;
    if (s_compose_title) {
        char title[48];
        snprintf(title, sizeof(title), "DM %.32s",
                 selected->alias[0] ? selected->alias : selected->fingerprint);
        lv_label_set_text(s_compose_title, title);
    }
    if (s_compose_sheet) {
        show_modal(s_compose_sheet);
    }
    if (s_compose_textarea && s_compose_keyboard) {
        lv_textarea_set_text(s_compose_textarea, "");
        lv_textarea_set_placeholder_text(s_compose_textarea, "Direct message");
        lv_keyboard_set_textarea(s_compose_keyboard, s_compose_textarea);
        layout_compose_sheet_controls();
        update_compose_counter();
    }
    request_full_screen_repaint();
}

static void open_dm_compose_for_contact(const d1l_contact_entry_t *entry)
{
    d1l_contact_entry_t selected = {0};
    if (entry) {
        selected = *entry;
    }
    const d1l_ui_dm_identity_eligibility_t eligibility =
        dm_identity_for_contact(entry, &selected);
    if (!eligibility.can_open_compose) {
        show_dm_identity_reason(eligibility);
        return;
    }
    present_dm_compose_sheet(&selected, false);
}

static void open_node_dm_for(const d1l_node_view_t *view)
{
    d1l_contact_entry_t contact = {0};
    const d1l_ui_dm_identity_eligibility_t eligibility =
        dm_identity_for_node(view, &contact);
    if (!eligibility.can_open_compose) {
        show_dm_identity_reason(eligibility);
        return;
    }
    open_dm_compose_for_contact(&contact);
}

static const d1l_contact_entry_t *selected_contact(void)
{
    return d1l_ui_contact_sheets_contact(&s_contact_sheets_controller);
}

static bool set_selected_contact(const d1l_contact_entry_t *contact)
{
    if (!contact) {
        return false;
    }
    const d1l_ui_dm_identity_eligibility_t eligibility =
        dm_identity_for_contact(contact, NULL);
    return d1l_ui_contact_sheets_set_contact(
        &s_contact_sheets_controller,
        contact,
        eligibility.reason,
        contact_can_export(contact),
        d1l_contact_store_meshcore_type_id(contact->type));
}

static void contact_sheets_action_handler(
    const d1l_ui_contact_action_event_t *event,
    void *context);

static bool show_contact_detail_sheet(void)
{
    request_content_refresh();
    if (!d1l_ui_contact_sheets_render_detail(
            &s_contact_sheets_controller,
            contact_sheets_action_handler,
            NULL)) {
        hide_contact_detail_sheet();
        return false;
    }
    show_modal(d1l_ui_contact_sheets_detail(&s_contact_sheets_controller));
    return true;
}

static bool show_contact_options_sheet(void)
{
    request_content_refresh();
    if (!d1l_ui_contact_sheets_render_options(
            &s_contact_sheets_controller,
            contact_sheets_action_handler,
            NULL)) {
        hide_contact_options_sheet();
        return false;
    }
    show_modal(d1l_ui_contact_sheets_options(&s_contact_sheets_controller));
    return true;
}

static bool show_contact_forget_sheet(void)
{
    if (!d1l_ui_contact_sheets_render_forget(
            &s_contact_sheets_controller,
            contact_sheets_action_handler,
            NULL)) {
        hide_contact_forget_sheet();
        return false;
    }
    show_modal(d1l_ui_contact_sheets_forget(&s_contact_sheets_controller));
    return true;
}

static bool show_contact_export_sheet(void)
{
    if (!d1l_ui_contact_sheets_render_export(
            &s_contact_sheets_controller,
            contact_sheets_action_handler,
            NULL)) {
        hide_contact_export_sheet();
        return false;
    }
    show_modal(d1l_ui_contact_sheets_export(&s_contact_sheets_controller));
    return true;
}

static bool show_contact_edit_sheet(void)
{
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_compose_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    d1l_ui_contact_sheets_hide_all(&s_contact_sheets_controller, false);
    if (!d1l_ui_contact_sheets_render_edit(
            &s_contact_sheets_controller,
            contact_sheets_action_handler,
            NULL)) {
        hide_contact_edit_sheet();
        return false;
    }
    show_modal(d1l_ui_contact_sheets_edit(&s_contact_sheets_controller));
    return true;
}

static void update_contact_detail_flags(bool favorite, bool muted)
{
    const d1l_contact_entry_t *contact = selected_contact();
    d1l_contact_entry_t updated = {0};
    esp_err_t ret = contact ?
        d1l_app_model_set_contact_flags(
            contact->fingerprint, favorite, muted, &updated) :
        ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK && set_selected_contact(&updated)) {
        request_content_refresh();
        show_contact_options_sheet();
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_STATE;
    }
    show_toast("Contact", ret);
}

static void contact_sheets_action_handler(
    const d1l_ui_contact_action_event_t *event,
    void *context)
{
    (void)context;
    if (!event || !d1l_ui_contact_action_available(event->action)) {
        return;
    }
    const d1l_contact_entry_t *contact = event->contact;
    switch (event->action) {
    case D1L_UI_CONTACT_ACTION_CLOSE_DETAIL:
        hide_contact_detail_sheet();
        return;
    case D1L_UI_CONTACT_ACTION_MESSAGE:
        if (contact) {
            open_dm_compose_for_contact(contact);
        } else {
            show_toast("DM", ESP_ERR_INVALID_STATE);
        }
        return;
    case D1L_UI_CONTACT_ACTION_OPEN_OPTIONS:
        if (!show_contact_options_sheet()) {
            show_toast("Contact", ESP_ERR_NO_MEM);
        }
        return;
    case D1L_UI_CONTACT_ACTION_CLOSE_OPTIONS:
        hide_contact_options_sheet();
        show_contact_detail_sheet();
        return;
    case D1L_UI_CONTACT_ACTION_ROUTE_TRACE:
        open_route_trace_event_cb(NULL);
        return;
    case D1L_UI_CONTACT_ACTION_RENAME:
        if (!show_contact_edit_sheet()) {
            show_toast("Contact", ESP_ERR_NO_MEM);
        }
        return;
    case D1L_UI_CONTACT_ACTION_TOGGLE_FAVORITE:
        if (contact) {
            update_contact_detail_flags(!contact->favorite, contact->muted);
        } else {
            show_toast("Contact", ESP_ERR_INVALID_STATE);
        }
        return;
    case D1L_UI_CONTACT_ACTION_TOGGLE_MUTE:
        if (contact) {
            update_contact_detail_flags(contact->favorite, !contact->muted);
        } else {
            show_toast("Contact", ESP_ERR_INVALID_STATE);
        }
        return;
    case D1L_UI_CONTACT_ACTION_EXPORT: {
        char uri[D1L_CONTACT_EXPORT_URI_LEN] = {0};
        esp_err_t ret = contact ?
            d1l_app_model_export_contact_uri(
                contact->fingerprint, uri, sizeof(uri)) :
            ESP_ERR_INVALID_STATE;
        if (ret != ESP_OK ||
            !d1l_ui_contact_sheets_set_export_uri(
                &s_contact_sheets_controller, uri)) {
            show_toast("Export", ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret);
            return;
        }
        hide_sheet();
        hide_public_history_sheet();
        hide_public_search_sheet();
        hide_dm_thread_sheet();
        hide_radio_settings_sheet();
        hide_compose_sheet();
        hide_route_detail_sheet();
        hide_route_trace_sheet();
        hide_packet_detail_sheet();
        hide_packet_search_sheet();
        hide_mesh_roles_sheet();
        if (!show_contact_export_sheet()) {
            show_toast("Export", ESP_ERR_NO_MEM);
        }
        return;
    }
    case D1L_UI_CONTACT_ACTION_OPEN_FORGET:
        if (!show_contact_forget_sheet()) {
            show_toast("Contact", ESP_ERR_NO_MEM);
        }
        return;
    case D1L_UI_CONTACT_ACTION_CANCEL_FORGET:
        hide_contact_forget_sheet();
        show_contact_options_sheet();
        return;
    case D1L_UI_CONTACT_ACTION_CONFIRM_FORGET: {
        d1l_contact_entry_t removed = {0};
        esp_err_t ret = contact ?
            d1l_app_model_delete_contact(contact->fingerprint, &removed) :
            ESP_ERR_INVALID_STATE;
        if (ret == ESP_OK) {
            hide_contact_detail_sheet();
            request_content_refresh();
        }
        show_toast("Forget contact", ret);
        return;
    }
    case D1L_UI_CONTACT_ACTION_CLOSE_EXPORT:
        hide_contact_export_sheet();
        show_contact_options_sheet();
        return;
    case D1L_UI_CONTACT_ACTION_SAVE_EDIT: {
        d1l_contact_entry_t updated = {0};
        esp_err_t ret = contact && event->text ?
            d1l_app_model_rename_contact(
                contact->fingerprint, event->text, &updated) :
            ESP_ERR_INVALID_STATE;
        if (ret == ESP_OK && set_selected_contact(&updated)) {
            hide_contact_edit_sheet();
            show_contact_options_sheet();
        } else if (ret == ESP_OK) {
            ret = ESP_ERR_INVALID_STATE;
        }
        show_toast("Rename", ret);
        return;
    }
    case D1L_UI_CONTACT_ACTION_CANCEL_EDIT:
        hide_contact_edit_sheet();
        show_contact_options_sheet();
        return;
    case D1L_UI_CONTACT_ACTION_NONE:
    default:
        return;
    }
}

static bool ui_route_trace_is_direct(const d1l_route_entry_t *entry)
{
    return entry && (entry->path_hops == 0 ||
                     strcmp(entry->route, "direct") == 0);
}

static size_t ui_route_trace_best_index(const d1l_route_entry_t *entries, size_t count)
{
    size_t best = 0;
    bool best_set = false;
    for (size_t i = 0; i < count; ++i) {
        const bool candidate_direct = ui_route_trace_is_direct(&entries[i]);
        const bool best_direct = best_set && ui_route_trace_is_direct(&entries[best]);
        if (!best_set ||
            (candidate_direct && !best_direct) ||
            (candidate_direct == best_direct && entries[i].confidence > entries[best].confidence) ||
            (candidate_direct == best_direct && entries[i].confidence == entries[best].confidence &&
             entries[i].seq > entries[best].seq)) {
            best = i;
            best_set = true;
        }
    }
    return best;
}

static void close_route_trace_event_cb(lv_event_t *event)
{
    (void)event;
    hide_route_trace_sheet();
    show_contact_options_sheet();
}

static void route_trace_request_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE)) {
        return;
    }
    if (s_route_trace_contact.fingerprint[0] == '\0') {
        show_toast("TRACE", ESP_ERR_INVALID_STATE);
        return;
    }
    esp_err_t ret = d1l_app_model_send_trace_contact(
        s_route_trace_contact.fingerprint);
    if (ret == ESP_OK) {
        show_toast_text("TRACE queued", true);
    } else {
        show_toast("TRACE", ret);
    }
}

static void route_ping_request_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE) ||
        strcmp(s_route_trace_contact.type, "repeater") != 0 ||
        s_route_trace_contact.fingerprint[0] == '\0') {
        show_toast("Ping", ESP_ERR_INVALID_STATE);
        return;
    }
    const esp_err_t ret = d1l_app_model_ping_repeater(
        s_route_trace_contact.fingerprint);
    if (ret == ESP_OK) {
        show_toast_text("Zero-hop Ping queued", true);
        render_route_trace_sheet();
    } else {
        show_toast("Ping", ret);
    }
}

static void route_probe_request_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE) ||
        s_route_trace_contact.fingerprint[0] == '\0') {
        show_toast("Probe", ESP_ERR_INVALID_STATE);
        return;
    }
    char token[D1L_MESSAGE_TEXT_LEN] = {0};
    const esp_err_t ret =
        d1l_app_model_request_path_discovery_probe(
            s_route_trace_contact.fingerprint, token, sizeof(token));
    memset(token, 0, sizeof(token));
    if (ret == ESP_OK) {
        show_toast_text("Reachability + telemetry probe sent", true);
        render_route_trace_sheet();
    } else {
        show_toast("Probe", ret);
    }
}

static void route_reset_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_route_trace_contact.fingerprint[0] == '\0') {
        show_toast("Route reset", ESP_ERR_INVALID_STATE);
        return;
    }
    esp_err_t ret = d1l_app_model_reset_contact_route(
        s_route_trace_contact.fingerprint);
    if (ret == ESP_OK) {
        d1l_contact_entry_t refreshed = {0};
        ret = d1l_app_model_find_contact(
            s_route_trace_contact.fingerprint, &refreshed);
        if (ret == ESP_OK) {
            s_route_trace_contact = refreshed;
        }
    }
    show_toast("Route reset", ret);
    if (ret == ESP_OK) {
        render_route_trace_sheet();
    }
}

static void render_route_trace_sheet(void)
{
    if (!s_route_trace_sheet) {
        return;
    }
    const lv_coord_t previous_scroll_y =
        lv_obj_get_scroll_y(s_route_trace_sheet);
    lv_obj_clean(s_route_trace_sheet);

    const d1l_contact_entry_t *contact = &s_route_trace_contact;
    const char *alias = contact->alias[0] ? contact->alias : contact->fingerprint;
    const size_t route_count =
        d1l_app_model_copy_route_trace(contact->fingerprint, s_route_trace_entries,
                                       D1L_ROUTE_STORE_CAPACITY);
    const size_t best = route_count ? ui_route_trace_best_index(s_route_trace_entries, route_count) : 0;
    d1l_app_model_contact_telemetry_snapshot(
        contact->fingerprint, &s_route_trace_telemetry);
    d1l_app_model_trace_snapshot(&s_route_trace_snapshot);
    s_route_trace_telemetry_rendered_generation =
        s_route_trace_telemetry.generation;
    s_route_trace_last_render_second = lv_tick_get() / 1000U;

    create_button(s_route_trace_sheet, "Back", 12, 6, 72, 44,
                  close_route_trace_event_cb, NULL);
    lv_obj_t *title = create_label(s_route_trace_sheet, alias, 0xF4F7FB);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 150);
    lv_obj_set_pos(title, 94, 10);

    if (strcmp(contact->type, "repeater") == 0) {
        create_button(s_route_trace_sheet, "Probe", 250, 6, 52, 44,
                      route_probe_request_event_cb, NULL);
        create_button(s_route_trace_sheet, "Ping", 306, 6, 48, 44,
                      route_ping_request_event_cb, NULL);
        create_button(s_route_trace_sheet, "Trace", 358, 6, 50, 44,
                      route_trace_request_event_cb, NULL);
        create_button(s_route_trace_sheet, "Reset", 412, 6, 52, 44,
                      route_reset_event_cb, NULL);
    } else {
        create_button(s_route_trace_sheet, "Probe", 250, 6, 68, 44,
                      route_probe_request_event_cb, NULL);
        create_button(s_route_trace_sheet, "Trace", 324, 6, 66, 44,
                      route_trace_request_event_cb, NULL);
        create_button(s_route_trace_sheet, "Reset", 396, 6, 68, 44,
                      route_reset_event_cb, NULL);
    }

    lv_obj_t *meta = create_label(s_route_trace_sheet, "", 0x8EA0AE);
    label_set_fmt(meta, "Trace %.16s  rows %u/%u",
                  contact->fingerprint, (unsigned)route_count,
                  (unsigned)D1L_ROUTE_STORE_CAPACITY);
    lv_obj_set_pos(meta, 16, 60);

    lv_obj_t *contact_path = create_label(s_route_trace_sheet, "", 0x8EA0AE);
    label_set_fmt(contact_path, "%s  contact path %s hops %u",
                  contact->public_key_hex[0] ? "key retained" : "no key",
                  contact->out_path_valid ? "known" : "unknown",
                  contact->out_path_valid ? contact->path_hops : 0);
    lv_label_set_long_mode(contact_path, LV_LABEL_LONG_DOT);
    lv_obj_set_width(contact_path, 448);
    lv_obj_set_pos(contact_path, 16, 88);

    lv_obj_t *best_label = create_label(s_route_trace_sheet, "", route_count ? 0x5EEAD4 : 0xFBBF24);
    if (route_count) {
        label_set_fmt(best_label, "best %s  seq %lu  confidence %u  hops %u",
                      s_route_trace_entries[best].route,
                      (unsigned long)s_route_trace_entries[best].seq,
                      s_route_trace_entries[best].confidence,
                      s_route_trace_entries[best].path_hops);
    } else {
        label_set_fmt(best_label, "No retained route evidence yet");
    }
    lv_label_set_long_mode(best_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(best_label, 448);
    lv_obj_set_pos(best_label, 16, 116);

    lv_obj_t *telemetry_state =
        create_label(s_route_trace_sheet, "", 0x93C5FD);
    if (s_route_trace_telemetry.pending) {
        label_set_fmt(
            telemetry_state,
            "Reachability/telemetry pending  tag %08lX  timeout %lus",
            (unsigned long)s_route_trace_telemetry.pending_tag,
            (unsigned long)(
                (s_route_trace_telemetry.pending_remaining_ms + 999U) /
                1000U));
    } else {
        label_set_fmt(
            telemetry_state, "Telemetry %s  history %u/%u",
            d1l_meshcore_contact_telemetry_state_name(
                s_route_trace_telemetry.state),
            (unsigned)s_route_trace_telemetry.history_count,
            (unsigned)D1L_MESHCORE_CONTACT_TELEMETRY_HISTORY_CAPACITY);
    }
    lv_label_set_long_mode(telemetry_state, LV_LABEL_LONG_DOT);
    lv_obj_set_width(telemetry_state, 448);
    lv_obj_set_pos(telemetry_state, 16, 142);

    int content_y = 168;
    const bool trace_target_matches =
        s_route_trace_snapshot.target_fingerprint[0] != '\0' &&
        strcmp(s_route_trace_snapshot.target_fingerprint,
               contact->fingerprint) == 0;
    if (trace_target_matches &&
        (s_route_trace_snapshot.pending ||
         s_route_trace_snapshot.last_attempt_valid)) {
        lv_obj_t *trace_panel =
            create_object(s_route_trace_sheet, "trace result");
        if (trace_panel) {
            lv_obj_set_width(trace_panel, 448);
            lv_obj_set_pos(trace_panel, 16, content_y);
            lv_obj_set_style_bg_color(
                trace_panel, lv_color_hex(0x10282A), 0);
            lv_obj_set_style_border_color(
                trace_panel, lv_color_hex(0x2DD4BF), 0);
            lv_obj_set_style_border_width(trace_panel, 1, 0);
            lv_obj_set_style_radius(trace_panel, 8, 0);
            lv_obj_set_style_pad_all(trace_panel, 8, 0);
            lv_obj_clear_flag(trace_panel, LV_OBJ_FLAG_SCROLLABLE);

            const char *operation =
                s_route_trace_snapshot.zero_hop_ping ?
                    "Zero-hop Ping" : "TRACE";
            char result_line[160] = {0};
            char snr_line[192] = {0};
            int panel_height = 64;
            if (s_route_trace_snapshot.pending) {
                snprintf(
                    result_line, sizeof(result_line),
                    "%s pending  %lus elapsed",
                    operation,
                    (unsigned long)(
                        s_route_trace_snapshot.pending_age_ms / 1000U));
                snprintf(
                    snr_line, sizeof(snr_line),
                    "Waiting for a correlated direct response");
            } else if (
                s_route_trace_snapshot.last_attempt_outcome ==
                D1L_MESHCORE_TRACE_OUTCOME_NO_RESPONSE) {
                snprintf(result_line, sizeof(result_line),
                         "%s timed out", operation);
                snprintf(
                    snr_line, sizeof(snr_line),
                    "No zero-hop/direct response; inspect route or range");
            } else if (
                s_route_trace_snapshot.last_result_valid &&
                s_route_trace_snapshot.last_tag ==
                    s_route_trace_snapshot.last_attempt_tag) {
                const int back_snr =
                    s_route_trace_snapshot.last_radio_snr_quarter_db;
                const int back_abs = back_snr < 0 ? -back_snr : back_snr;
                snprintf(
                    result_line, sizeof(result_line),
                    "%s reply  RTT %lums  RSSI %d  back SNR %s%d.%02d",
                    operation,
                    (unsigned long)
                        s_route_trace_snapshot.last_round_trip_ms,
                    s_route_trace_snapshot.last_rssi_dbm,
                    back_snr < 0 ? "-" : "", back_abs / 4,
                    (back_abs % 4) * 25);
                size_t used = 0U;
                used += (size_t)snprintf(
                    snr_line, sizeof(snr_line), "Hop SNR:");
                for (size_t i = 0U;
                     i < s_route_trace_snapshot.last_path_hops &&
                     used + 16U < sizeof(snr_line); ++i) {
                    const int hop =
                        s_route_trace_snapshot
                            .last_path_snrs_quarter_db[i];
                    const int hop_abs = hop < 0 ? -hop : hop;
                    const int written = snprintf(
                        &snr_line[used], sizeof(snr_line) - used,
                        " %u=%s%d.%02d", (unsigned)(i + 1U),
                        hop < 0 ? "-" : "", hop_abs / 4,
                        (hop_abs % 4) * 25);
                    if (written <= 0 ||
                        (size_t)written >= sizeof(snr_line) - used) {
                        break;
                    }
                    used += (size_t)written;
                }
                panel_height = 82;
            } else {
                snprintf(result_line, sizeof(result_line),
                         "%s result unavailable", operation);
                snprintf(snr_line, sizeof(snr_line),
                         "No matched response retained");
            }

            lv_obj_t *result =
                create_label(trace_panel, result_line, 0x5EEAD4);
            if (result) {
                lv_label_set_long_mode(result, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(result, 416);
                lv_obj_set_pos(result, 8, 4);
            }
            lv_obj_t *snrs =
                create_label(trace_panel, snr_line, 0xE5EDF5);
            if (snrs) {
                lv_label_set_long_mode(snrs, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(snrs, 416);
                lv_obj_set_pos(snrs, 8, 30);
            }
            lv_obj_set_height(trace_panel, panel_height);
            content_y += panel_height + 10;
        }
    }
    if (s_route_trace_telemetry.history_count == 0U) {
        lv_obj_t *empty = create_label(
            s_route_trace_sheet,
            "Tap Probe to discover a route, verify reachability, and request base telemetry.",
            0x8EA0AE);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, 448);
        lv_obj_set_pos(empty, 16, content_y);
        content_y += 56;
    } else {
        for (size_t i = 0U;
             i < s_route_trace_telemetry.history_count; ++i) {
            const d1l_meshcore_contact_telemetry_entry_t *entry =
                &s_route_trace_telemetry.history[i];
            lv_obj_t *panel =
                create_object(s_route_trace_sheet, "telemetry history row");
            if (!panel) {
                continue;
            }
            lv_obj_set_width(panel, 448);
            lv_obj_set_pos(panel, 16, content_y);
            lv_obj_set_style_bg_color(
                panel, lv_color_hex(i == 0U ? 0x10282A : 0x111923), 0);
            lv_obj_set_style_border_color(
                panel, lv_color_hex(0x334155), 0);
            lv_obj_set_style_border_width(panel, 1, 0);
            lv_obj_set_style_radius(panel, 8, 0);
            lv_obj_set_style_pad_all(panel, 8, 0);
            lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *record_meta = create_label(panel, "", 0x5EEAD4);
            label_set_fmt(
                record_meta,
                "#%lu  %lus ago  RSSI %d  SNR %s%d.%d  hops %u",
                (unsigned long)entry->sequence,
                (unsigned long)(entry->age_ms / 1000U),
                entry->last_rssi_dbm,
                entry->last_snr_tenths < 0 ? "-" : "",
                (entry->last_snr_tenths < 0 ?
                     -entry->last_snr_tenths :
                     entry->last_snr_tenths) /
                    10,
                (entry->last_snr_tenths < 0 ?
                     -entry->last_snr_tenths :
                     entry->last_snr_tenths) %
                    10,
                entry->path_hops);
            lv_label_set_long_mode(record_meta, LV_LABEL_LONG_DOT);
            lv_obj_set_width(record_meta, 416);
            lv_obj_set_pos(record_meta, 8, 4);

            lv_obj_t *record_text =
                create_label(panel, entry->result.text, 0xE5EDF5);
            lv_label_set_long_mode(record_text, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(record_text, 416);
            lv_obj_set_pos(record_text, 8, 28);
            lv_obj_update_layout(record_text);
            int panel_height = lv_obj_get_height(record_text) + 44;
            if (panel_height < 76) {
                panel_height = 76;
            }
            if (panel_height > 300) {
                panel_height = 300;
                lv_label_set_long_mode(record_text, LV_LABEL_LONG_DOT);
                lv_obj_set_height(record_text, 256);
            }
            lv_obj_set_height(panel, panel_height);
            content_y += panel_height + 10;
        }
    }

    lv_obj_t *route_heading =
        create_label(s_route_trace_sheet, "Route evidence", 0xFBBF24);
    lv_obj_set_pos(route_heading, 16, content_y);
    content_y += 26;

    lv_obj_t *list = create_object(s_route_trace_sheet, "route trace list");
    if (!list) {
        return;
    }
    lv_obj_set_size(list, 448, 200);
    lv_obj_set_pos(list, 16, content_y);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0B111B), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(0x253447), 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_radius(list, 8, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    int y = 0;
    for (size_t i = 0; i < route_count; ++i) {
        const d1l_route_entry_t *entry = &s_route_trace_entries[i];
        lv_obj_t *row = create_object(list, "route trace row");
        if (!row) {
            y += 58;
            continue;
        }
        lv_obj_set_size(row, 416, 50);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_color(row, lv_color_hex(ui_route_trace_is_direct(entry) ? 0x132A32 : 0x182230), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x334155), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *line1 = create_label(row, "", 0xF4F7FB);
        label_set_fmt(line1, "#%lu %s %s %s",
                      (unsigned long)entry->seq, entry->kind, entry->route, entry->direction);
        lv_label_set_long_mode(line1, LV_LABEL_LONG_DOT);
        lv_obj_set_width(line1, 386);
        lv_obj_set_pos(line1, 10, 6);

        const int snr_abs = entry->last_snr_tenths < 0 ? -entry->last_snr_tenths : entry->last_snr_tenths;
        lv_obj_t *line2 = create_label(row, "", 0x8EA0AE);
        label_set_fmt(line2, "rssi %d snr %s%d.%d hops %u conf %u seen %lu",
                      entry->last_rssi_dbm,
                      entry->last_snr_tenths < 0 ? "-" : "", snr_abs / 10, snr_abs % 10,
                      entry->path_hops, entry->confidence, (unsigned long)entry->seen_count);
        lv_label_set_long_mode(line2, LV_LABEL_LONG_DOT);
        lv_obj_set_width(line2, 386);
        lv_obj_set_pos(line2, 10, 28);
        y += 58;
    }
    content_y += 210;

    lv_obj_t *note = create_label(s_route_trace_sheet,
                                  "Probe and TRACE are contact-only RF; Reset forgets only this contact; no Public RF",
                                  0x8EA0AE);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, 448);
    lv_obj_set_pos(note, 16, content_y);
    lv_obj_set_height(note, 50);
    lv_obj_update_layout(s_route_trace_sheet);
    lv_obj_scroll_to_y(
        s_route_trace_sheet, previous_scroll_y, LV_ANIM_OFF);
}

static void open_route_trace_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE)) {
        return;
    }
    const d1l_contact_entry_t *contact = selected_contact();
    if (!contact) {
        show_toast("Trace", ESP_ERR_INVALID_STATE);
        return;
    }
    s_route_trace_contact = *contact;
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_compose_sheet();
    d1l_ui_contact_sheets_hide_options(&s_contact_sheets_controller);
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    render_route_trace_sheet();
    if (s_route_trace_sheet) {
        show_modal(s_route_trace_sheet);
    }
}

static void show_contact_detail_for(const d1l_contact_entry_t *entry)
{
    if (!set_selected_contact(entry)) {
        show_toast("Contact", ESP_ERR_INVALID_STATE);
        return;
    }
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_compose_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    hide_contact_export_sheet();
    hide_contact_edit_sheet();
    d1l_ui_contact_sheets_hide_options(&s_contact_sheets_controller);
    d1l_ui_contact_sheets_hide_forget(&s_contact_sheets_controller);
    hide_node_detail_sheet();
    if (!show_contact_detail_sheet()) {
        show_toast("Contact", ESP_ERR_NO_MEM);
    }
}

static void open_contact_detail_event_cb(lv_event_t *event)
{
    const void *user_data = event ? lv_event_get_user_data(event) : NULL;
    show_contact_detail_for((const d1l_contact_entry_t *)user_data);
}

static void handle_node_detail_action(
    const d1l_ui_node_detail_action_event_t *event,
    void *context)
{
    (void)context;
    if (!event) {
        return;
    }
    switch (event->action) {
    case D1L_UI_NODE_DETAIL_ACTION_CLOSE:
        hide_node_detail_sheet();
        if (event->return_to_map) {
            /* The detail modal may outlive a settings/time refresh. Rebind the
             * retained viewport to the current trusted center before any
             * reacquire; stale saved coordinates must fail closed. */
            d1l_app_model_snapshot(&s_snapshot);
            d1l_ui_map_viewport_update_truth_context(&s_snapshot);
        }
        if (event->return_to_map &&
            d1l_ui_navigation_active() == D1L_UI_TAB_MAP &&
            !d1l_ui_modal_has_active() && !s_lock_visible &&
            !s_onboarding_visible && map_interactive_touch_authorized() &&
            !d1l_ui_map_viewport_reacquire()) {
            request_content_refresh();
        }
        break;
    case D1L_UI_NODE_DETAIL_ACTION_OPEN_DM:
        open_node_dm_for(event->node);
        break;
    case D1L_UI_NODE_DETAIL_ACTION_EXPLAIN_DM:
        show_dm_identity_reason(dm_identity_for_node(event->node, NULL));
        break;
    case D1L_UI_NODE_DETAIL_ACTION_OPEN_ADMIN:
        if (event->node && event->node->node.fingerprint[0] != '\0') {
            clear_admin_cli_confirmation();
            s_admin_cli_secure_input = false;
            s_admin_rendered_generation_valid = false;
            d1l_meshcore_admin_snapshot_t admin_status = {0};
            d1l_meshcore_service_admin_snapshot(&admin_status);
            if (admin_status.fingerprint[0] != '\0' &&
                strcmp(admin_status.fingerprint,
                       event->node->node.fingerprint) != 0 &&
                admin_status.state != D1L_MESHCORE_ADMIN_IDLE &&
                admin_status.state != D1L_MESHCORE_ADMIN_TIMED_OUT) {
                const esp_err_t logout_result =
                    d1l_meshcore_service_admin_logout();
                if (logout_result != ESP_OK) {
                    show_toast("Admin switch", logout_result);
                    break;
                }
            }
            snprintf(s_admin_target_fingerprint,
                     sizeof(s_admin_target_fingerprint), "%s",
                     event->node->node.fingerprint);
            hide_node_detail_sheet();
            open_admin_sheet_event_cb(NULL);
        }
        break;
    case D1L_UI_NODE_DETAIL_ACTION_NONE:
    default:
        break;
    }
}

static void show_node_detail_view(const d1l_node_view_t *view, bool return_to_map)
{
    if (!view || view->node.fingerprint[0] == '\0') {
        show_toast("Node", ESP_ERR_INVALID_STATE);
        return;
    }
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_channel_management_sheets();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    const d1l_ui_dm_identity_eligibility_t eligibility =
        dm_identity_for_node(view, NULL);
    d1l_ui_node_detail_view_model_t view_model;
    if (!d1l_ui_node_detail_build_view_model(
            view, eligibility, return_to_map, &view_model)) {
        show_toast("Node", ESP_ERR_INVALID_STATE);
        return;
    }
    if (!d1l_ui_node_detail_render(
            &s_node_detail_controller, &view_model,
            handle_node_detail_action, NULL)) {
        show_toast("Node", ESP_ERR_NO_MEM);
        return;
    }
    show_modal(d1l_ui_node_detail_sheet(&s_node_detail_controller));
}

static void open_map_node_detail(const char *fingerprint)
{
    if (!fingerprint || fingerprint[0] == '\0') {
        show_toast("Node", ESP_ERR_INVALID_ARG);
        return;
    }
    const d1l_node_query_t query = {
        .filter = D1L_NODE_FILTER_ALL,
        .sort = D1L_NODE_SORT_LAST_HEARD,
        .text = fingerprint,
    };
    const size_t count = d1l_app_model_query_nodes(
        &query, s_map_node_rows, D1L_NODE_STORE_CAPACITY);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(s_map_node_rows[i].node.fingerprint, fingerprint) == 0) {
            show_node_detail_view(&s_map_node_rows[i], true);
            return;
        }
    }
    show_toast("Node", ESP_ERR_NOT_FOUND);
}

static void close_route_detail_event_cb(lv_event_t *event)
{
    (void)event;
    hide_route_detail_sheet();
}

static void render_route_detail_sheet(void)
{
    if (!s_route_detail_sheet) {
        return;
    }
    lv_obj_clean(s_route_detail_sheet);

    const d1l_route_entry_t *entry = &s_route_detail_route;
    lv_obj_t *title = create_label(s_route_detail_sheet,
                                   entry->label[0] ? entry->label : entry->target,
                                   0xF4F7FB);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 260);
    lv_obj_set_pos(title, 8, 4);

    create_button(s_route_detail_sheet, "Close", 316, 0, 76, 40, close_route_detail_event_cb, NULL);

    lv_obj_t *target = create_label(s_route_detail_sheet, "", 0xE5EDF5);
    label_set_fmt(target, "target %.16s", entry->target[0] ? entry->target : "-");
    lv_label_set_long_mode(target, LV_LABEL_LONG_DOT);
    lv_obj_set_width(target, 392);
    lv_obj_set_pos(target, 8, 48);

    lv_obj_t *route = create_label(s_route_detail_sheet, "", 0x8EA0AE);
    label_set_fmt(route, "#%lu  %s  %s  %s",
                  (unsigned long)entry->seq,
                  entry->kind[0] ? entry->kind : "packet",
                  entry->route[0] ? entry->route : "unknown",
                  entry->direction[0] ? entry->direction : "-");
    lv_label_set_long_mode(route, LV_LABEL_LONG_DOT);
    lv_obj_set_width(route, 392);
    lv_obj_set_pos(route, 8, 82);

    const int snr_abs = entry->last_snr_tenths < 0 ? -entry->last_snr_tenths : entry->last_snr_tenths;
    lv_obj_t *signal = create_label(s_route_detail_sheet, "", 0x8EA0AE);
    label_set_fmt(signal, "rssi %d  snr %s%d.%d  payload %u",
                  entry->last_rssi_dbm,
                  entry->last_snr_tenths < 0 ? "-" : "", snr_abs / 10, snr_abs % 10,
                  entry->payload_len);
    lv_label_set_long_mode(signal, LV_LABEL_LONG_DOT);
    lv_obj_set_width(signal, 392);
    lv_obj_set_pos(signal, 8, 116);

    lv_obj_t *path = create_label(s_route_detail_sheet, "", 0x8EA0AE);
    label_set_fmt(path, "hash %u byte  hops %u  confidence %u",
                  entry->path_hash_bytes, entry->path_hops, entry->confidence);
    lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
    lv_obj_set_width(path, 392);
    lv_obj_set_pos(path, 8, 150);

    lv_obj_t *seen = create_label(s_route_detail_sheet, "", 0x8EA0AE);
    label_set_fmt(seen, "seen %lu  first %lums  last %lums",
                  (unsigned long)entry->seen_count,
                  (unsigned long)entry->first_seen_ms,
                  (unsigned long)entry->last_seen_ms);
    lv_label_set_long_mode(seen, LV_LABEL_LONG_DOT);
    lv_obj_set_width(seen, 392);
    lv_obj_set_pos(seen, 8, 184);
}

static void open_route_detail_event_cb(lv_event_t *event)
{
    size_t payload_index = 0U;
    if (!event ||
        !row_token_index(lv_event_get_user_data(event),
                         s_route_row_generation,
                         s_route_row_payload_count,
                         &payload_index) ||
        s_route_row_payloads[payload_index].seq == 0U) {
        show_toast("Route", ESP_ERR_INVALID_STATE);
        return;
    }
    s_route_detail_route = s_route_row_payloads[payload_index];
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_message_detail_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    render_route_detail_sheet();
    if (s_route_detail_sheet) {
        show_modal(s_route_detail_sheet);
    }
}

static void close_message_detail_event_cb(lv_event_t *event)
{
    (void)event;
    hide_message_detail_sheet();
}

static const char *message_delivery_label(const d1l_message_entry_t *entry)
{
    if (!entry) {
        return "unknown";
    }
    if (entry->direction[0] == 't') {
        return "sent over RF";
    }
    d1l_channel_info_t channel = {0};
    return snapshot_find_channel(&s_snapshot, entry->channel_id, &channel) &&
            entry->seq > snapshot_channel_read_seq(&s_snapshot, &channel) ?
        "new" : "received";
}

static void reply_message_detail_event_cb(lv_event_t *event)
{
    (void)event;
    const d1l_message_entry_t entry = s_message_detail_message;
    if (entry.seq == 0 || entry.author[0] == '\0') {
        show_toast("Reply", ESP_ERR_INVALID_STATE);
        return;
    }
    char title[48];
    char placeholder[64];
    d1l_app_model_snapshot(&s_snapshot);
    d1l_channel_info_t channel = {0};
    if (!snapshot_find_channel(&s_snapshot, entry.channel_id, &channel) ||
        !channel.enabled) {
        show_toast("Reply", ESP_ERR_NOT_FOUND);
        return;
    }
    snprintf(title, sizeof(title), "Reply %.32s", entry.author);
    snprintf(placeholder, sizeof(placeholder), "Reply to %.48s", entry.author);
    (void)show_channel_compose_sheet(
        entry.channel_id, title, placeholder);
}

static d1l_ui_dm_identity_eligibility_t public_sender_dm_eligibility(void)
{
    const d1l_ui_dm_identity_input_t input = {
        .source = D1L_UI_DM_IDENTITY_SOURCE_PUBLIC_SENDER_LABEL,
        .fingerprint = NULL,
        .public_key_hex = NULL,
        .resolved_contact = NULL,
        .contact_found_by_full_key = false,
    };
    return d1l_ui_dm_identity_eligibility(&input);
}

static void explain_public_sender_dm_event_cb(lv_event_t *event)
{
    (void)event;
    /* This action is explanation-only. A Public display name is never used
     * to select a Contact, open compose, or dispatch RF. */
    show_dm_identity_reason(public_sender_dm_eligibility());
}

static lv_obj_t *create_nested_page_body(lv_obj_t *page, const char *name)
{
    lv_obj_t *body = create_object(page, name);
    if (!body) {
        return NULL;
    }
    lv_obj_set_size(body, 448, 292);
    lv_obj_set_pos(body, 16, 60);
    lv_obj_set_style_radius(body, 8, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_color(body, lv_color_hex(0x263241), 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return body;
}

static lv_obj_t *create_nested_page_label(lv_obj_t *parent, const char *text,
                                           uint32_t color, bool wrap)
{
    lv_obj_t *label = create_label(parent, text, color);
    if (!label) {
        return NULL;
    }
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    return label;
}

static void render_message_detail_sheet(void)
{
    if (!s_message_detail_sheet) {
        return;
    }
    lv_obj_clean(s_message_detail_sheet);

    const d1l_message_entry_t *entry = &s_message_detail_message;
    d1l_channel_info_t detail_channel = {0};
    const bool channel_found = snapshot_find_channel(
        &s_snapshot, entry->channel_id, &detail_channel);
    create_button(s_message_detail_sheet, "Back", 12, 6, 72, 44,
                  close_message_detail_event_cb, NULL);
    lv_obj_t *title = create_label(s_message_detail_sheet, "Message Detail", 0xF4F7FB);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 364);
    lv_obj_set_pos(title, 100, 10);

    lv_obj_t *body = create_nested_page_body(s_message_detail_sheet, "message detail body");
    if (!body) {
        return;
    }

    create_nested_page_label(body, "Sender", 0x5EEAD4, false);
    lv_obj_t *sender = create_nested_page_label(body, "", 0xE5EDF5, false);
    if (sender) {
        label_set_fmt(sender, "%s  %s",
                      entry->author[0] ? entry->author : "unknown",
                      message_delivery_label(entry));
    }

    create_nested_page_label(body, "Channel", 0x5EEAD4, false);
    create_nested_page_label(
        body, channel_found && detail_channel.name[0] ?
            detail_channel.name : "Channel unavailable",
        channel_found ? 0xE5EDF5 : 0xFBBF24, false);

    const d1l_ui_dm_identity_eligibility_t sender_dm =
        public_sender_dm_eligibility();
    char dm_reason[160];
    snprintf(dm_reason, sizeof(dm_reason), "DM unavailable [%s]: %s",
             d1l_ui_dm_identity_reason_code(sender_dm.reason),
             d1l_ui_dm_identity_reason_text(sender_dm.reason));
    create_nested_page_label(body, dm_reason, 0xFBBF24, true);

    create_nested_page_label(body, "Message", 0x5EEAD4, false);
    create_nested_page_label(body, entry->text[0] ? entry->text : "-", 0xF4F7FB, true);

    lv_obj_t *technical = create_button(body,
                                        s_message_detail_advanced ?
                                        "Hide technical details" : "Technical details",
                                        0, 0, 424, 48, message_detail_mode_event_cb, NULL);
    if (technical) {
        lv_obj_set_width(technical, LV_PCT(100));
    }

    if (s_message_detail_advanced) {
        lv_obj_t *signal = create_nested_page_label(body, "", 0x8EA0AE, true);
        if (signal) {
            if (entry->direction[0] == 't') {
                lv_label_set_text(signal,
                                  "Signal  not measured for retained channel TxDone");
            } else {
                const int snr_abs = entry->snr_tenths < 0 ?
                    -entry->snr_tenths : entry->snr_tenths;
                label_set_fmt(signal, "Signal  rssi %d  snr %s%d.%d",
                              entry->rssi_dbm,
                              entry->snr_tenths < 0 ? "-" : "",
                              snr_abs / 10, snr_abs % 10);
            }
        }
        lv_obj_t *path = create_nested_page_label(body, "", 0x8EA0AE, true);
        if (path) {
            if (entry->direction[0] == 't') {
                lv_label_set_text(path,
                                  "Path hops  not measured for retained channel TxDone");
            } else {
                label_set_fmt(path, "Path  %u hop%s",
                              entry->path_hops,
                              entry->path_hops == 1 ? "" : "s");
            }
        }
        lv_obj_t *seq = create_nested_page_label(body, "", 0x8EA0AE, true);
        if (seq) {
            label_set_fmt(seq, "Sequence  %lu  uptime %lums  direction %s",
                          (unsigned long)entry->seq,
                          (unsigned long)entry->uptime_ms,
                          entry->direction[0] ? entry->direction : "?");
        }
        lv_obj_t *hash = create_nested_page_label(body, "", 0x8EA0AE, true);
        if (hash) {
            label_set_fmt(hash, "Path hash  %u byte%s  retained after %s",
                          entry->path_hash_bytes,
                          entry->path_hash_bytes == 1 ? "" : "s",
                          entry->direction[0] == 't' ? "TxDone" : "receive");
        }
    }

    if (entry->direction[0] != 't') {
        create_button(s_message_detail_sheet, "Reply", 16, 360, 216, 52,
                      reply_message_detail_event_cb, NULL);
        create_button(s_message_detail_sheet, "DM sender", 248, 360, 216, 52,
                      explain_public_sender_dm_event_cb, NULL);
    }
}

static void show_message_detail_for(const d1l_message_entry_t *entry)
{
    if (!entry || entry->seq == 0) {
        show_toast("Message", ESP_ERR_INVALID_STATE);
        return;
    }
    s_message_detail_message = *entry;
    s_message_detail_advanced = false;
    d1l_app_model_snapshot(&s_snapshot);
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    hide_node_detail_sheet();
    render_message_detail_sheet();
    if (s_message_detail_sheet) {
        show_modal(s_message_detail_sheet);
    }
}

static void message_detail_mode_event_cb(lv_event_t *event)
{
    (void)event;
    s_message_detail_advanced = !s_message_detail_advanced;
    render_message_detail_sheet();
}

static void close_packet_detail_event_cb(lv_event_t *event)
{
    (void)event;
    hide_packet_detail_sheet();
}

static void packet_detail_mode_event_cb(lv_event_t *event)
{
    (void)event;
    s_packet_detail_advanced = !s_packet_detail_advanced;
    render_packet_detail_sheet();
}

static void render_packet_detail_sheet(void)
{
    if (!s_packet_detail_sheet) {
        return;
    }
    lv_obj_clean(s_packet_detail_sheet);

    const d1l_packet_log_entry_t *entry = &s_packet_detail_packet;
    const uint32_t accent_color = d1l_ui_packets_entry_color(entry);
    char snr[16];
    format_snr_tenths(snr, sizeof(snr), entry->snr_tenths);

    lv_obj_t *title = create_label(s_packet_detail_sheet, "Packet Detail", 0xF4F7FB);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 190);
    lv_obj_set_pos(title, 8, 4);

    create_button(s_packet_detail_sheet,
                  s_packet_detail_advanced ? "Normal" : "Advanced",
                  218, 0, 92, 40, packet_detail_mode_event_cb, NULL);
    create_button(s_packet_detail_sheet, "Close", 320, 0, 76, 40, close_packet_detail_event_cb, NULL);

    lv_obj_t *kind_title = create_label(s_packet_detail_sheet, "Kind", 0x8EA0AE);
    lv_obj_set_pos(kind_title, 8, 54);
    lv_obj_t *kind = create_label(s_packet_detail_sheet, "", accent_color);
    label_set_fmt(kind, "%s  %s  #%lu",
                  entry->kind[0] ? entry->kind : "packet",
                  d1l_ui_packets_entry_status(entry),
                  (unsigned long)entry->seq);
    label_set_dot_width(kind, 392);
    lv_obj_set_pos(kind, 8, 76);

    lv_obj_t *signal_title = create_label(s_packet_detail_sheet, "Signal", 0x8EA0AE);
    lv_obj_set_pos(signal_title, 8, 108);
    lv_obj_t *signal = create_label(s_packet_detail_sheet, "", 0xE5EDF5);
    label_set_fmt(signal, "direction %s  rssi %d  snr %s",
                  entry->direction[0] ? entry->direction : "-",
                  entry->rssi_dbm, snr);
    label_set_dot_width(signal, 392);
    lv_obj_set_pos(signal, 8, 130);

    lv_obj_t *payload_title = create_label(s_packet_detail_sheet, "Payload", 0x8EA0AE);
    lv_obj_set_pos(payload_title, 8, 162);
    lv_obj_t *payload = create_label(s_packet_detail_sheet, "", 0xE5EDF5);
    label_set_fmt(payload, "%u bytes  raw %u%s",
                  entry->payload_len,
                  entry->raw_len,
                  entry->raw_truncated ? " truncated" : "");
    label_set_dot_width(payload, 392);
    lv_obj_set_pos(payload, 8, 184);

    lv_obj_t *note = create_label(s_packet_detail_sheet,
                                  entry->note[0] ? entry->note : "No packet note",
                                  0xF4F7FB);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, 392);
    lv_obj_set_pos(note, 8, 216);

    if (!s_packet_detail_advanced) {
        lv_obj_t *hint = create_label(s_packet_detail_sheet, "Advanced shows path and raw hex", 0x8EA0AE);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
        lv_obj_set_width(hint, 392);
        lv_obj_set_pos(hint, 8, 252);
        return;
    }

    lv_obj_t *path_title = create_label(s_packet_detail_sheet, "Path", 0x8EA0AE);
    lv_obj_set_pos(path_title, 8, 252);
    lv_obj_t *path = create_label(s_packet_detail_sheet, "", 0xE5EDF5);
    label_set_fmt(path, "hash %u byte  hops %u  uptime %lums",
                  entry->path_hash_bytes, entry->path_hops, (unsigned long)entry->uptime_ms);
    lv_obj_set_width(path, 392);
    lv_obj_set_pos(path, 8, 274);

    lv_obj_t *raw_title = create_label(s_packet_detail_sheet, "Raw Hex", 0x8EA0AE);
    lv_obj_set_pos(raw_title, 8, 306);

    lv_obj_t *raw = create_label(s_packet_detail_sheet, entry->raw_hex[0] ? entry->raw_hex : "-", 0x93C5FD);
    lv_label_set_long_mode(raw, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(raw, 392);
    lv_obj_set_pos(raw, 8, 328);
}

static void open_packet_detail_event_cb(lv_event_t *event)
{
    size_t payload_index = 0U;
    if (!event ||
        !row_token_index(lv_event_get_user_data(event),
                         s_packet_row_generation,
                         s_packet_row_payload_count,
                         &payload_index) ||
        s_packet_row_payloads[payload_index].seq == 0U) {
        show_toast("Packet", ESP_ERR_INVALID_STATE);
        return;
    }
    s_packet_detail_packet = s_packet_row_payloads[payload_index];
    s_packet_detail_advanced = false;
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_message_detail_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    render_packet_detail_sheet();
    if (s_packet_detail_sheet) {
        show_modal(s_packet_detail_sheet);
    }
}

static void packet_filter_event_cb(lv_event_t *event)
{
    d1l_ui_packets_select_filter(
        &s_packets_controller,
        (d1l_ui_packet_filter_t)(uintptr_t)lv_event_get_user_data(event));
    request_content_refresh();
}

static void packet_pause_event_cb(lv_event_t *event)
{
    (void)event;
    if (!s_packets_controller.paused) {
        refresh_packet_terminal_rows();
    }
    d1l_ui_packets_toggle_pause(&s_packets_controller);
    request_content_refresh();
}

static void packet_load_older_event_cb(lv_event_t *event)
{
    (void)event;
    d1l_ui_packets_load_older(&s_packets_controller);
    request_content_refresh();
}

static void packet_load_newer_event_cb(lv_event_t *event)
{
    (void)event;
    d1l_ui_packets_load_newer(&s_packets_controller);
    request_content_refresh();
}

static void close_packet_search_event_cb(lv_event_t *event)
{
    (void)event;
    hide_packet_search_sheet();
}

static void clear_packet_search_event_cb(lv_event_t *event)
{
    (void)event;
    d1l_ui_packets_clear_search(&s_packets_controller);
    if (s_packet_search_textarea) {
        lv_textarea_set_text(s_packet_search_textarea, "");
    }
    hide_packet_search_sheet();
    request_content_refresh();
}

static void apply_packet_search_event_cb(lv_event_t *event)
{
    (void)event;
    const char *text = "";
    if (s_packet_search_textarea) {
        const char *textarea_text = lv_textarea_get_text(s_packet_search_textarea);
        text = textarea_text ? textarea_text : "";
    }
    d1l_ui_packets_set_search(&s_packets_controller, text);
    hide_packet_search_sheet();
    request_content_refresh();
}

static void packet_search_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        apply_packet_search_event_cb(event);
    } else if (code == LV_EVENT_CANCEL) {
        hide_packet_search_sheet();
    }
}

static void open_packet_search_event_cb(lv_event_t *event)
{
    (void)event;
    if (!s_packet_search_sheet) {
        return;
    }
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_message_detail_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_mesh_roles_sheet();
    if (s_packet_search_textarea && s_packet_search_keyboard) {
        lv_textarea_set_text(s_packet_search_textarea, s_packets_controller.search_text);
        lv_keyboard_set_textarea(s_packet_search_keyboard, s_packet_search_textarea);
    }
    show_modal(s_packet_search_sheet);
}

static void close_mesh_roles_event_cb(lv_event_t *event)
{
    (void)event;
    hide_mesh_roles_sheet();
}

static void mesh_roles_subpage_back_event_cb(lv_event_t *event)
{
    (void)event;
    s_mesh_roles_page = D1L_MESH_ROLES_PAGE_ROOT;
    render_mesh_roles_sheet();
    show_modal(s_mesh_roles_sheet);
}

static void open_mesh_room_servers_event_cb(lv_event_t *event)
{
    (void)event;
    s_mesh_roles_page = D1L_MESH_ROLES_PAGE_ROOM_SERVERS;
    render_mesh_roles_sheet();
    show_modal(s_mesh_roles_sheet);
}

static void open_mesh_repeater_candidates_event_cb(lv_event_t *event)
{
    (void)event;
    s_mesh_roles_page = D1L_MESH_ROLES_PAGE_REPEATER_CANDIDATES;
    render_mesh_roles_sheet();
    show_modal(s_mesh_roles_sheet);
}

static void render_mesh_roles_header(const char *title, lv_event_cb_t back_cb)
{
    create_button(s_mesh_roles_sheet, "Back", 12, 8, 76, 44, back_cb, NULL);
    lv_obj_t *heading = create_label(s_mesh_roles_sheet, title, 0xF4F7FB);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    lv_obj_set_width(heading, 360);
    lv_obj_set_pos(heading, 104, 13);
}

static void render_room_server_role_row(lv_obj_t *parent, int y, const d1l_mesh_room_server_t *entry)
{
    char snr[16];
    lv_obj_t *row = create_panel(parent, 0, y, 424, 56);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_t *name = create_label(row, entry->name[0] ? entry->name : entry->fingerprint, 0xA7F3D0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 406);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
    format_snr_tenths(snr, sizeof(snr), entry->snr_tenths);
    lv_obj_t *meta = create_label(row, "", 0xE5EDF5);
    label_set_fmt(meta, "%.12s  %d dBm / %s dB",
                  entry->fingerprint, entry->rssi_dbm, snr);
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(meta, 406);
    lv_obj_align(meta, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static const char *friendly_repeater_route(const d1l_mesh_repeater_candidate_t *entry)
{
    const char *route = entry && entry->route[0] ? entry->route : "";
    if (strcmp(route, "heard_path") == 0) {
        return "heard path";
    }
    if (strcmp(route, "flood") == 0) {
        return "flood path";
    }
    if (strcmp(route, "direct") == 0) {
        return "direct path";
    }
    if (!route[0] || strcmp(route, "unknown") == 0 || strchr(route, '_')) {
        return "route evidence";
    }
    return route;
}

static void render_repeater_role_row(lv_obj_t *parent, int y,
                                     const d1l_mesh_repeater_candidate_t *entry)
{
    char snr[16];
    lv_obj_t *row = create_panel(parent, 0, y, 424, 56);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_t *label = create_label(row, entry->label[0] ? entry->label : entry->target, 0xFBBF24);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, 406);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    format_snr_tenths(snr, sizeof(snr), entry->snr_tenths);
    lv_obj_t *meta = create_label(row, "", 0xE5EDF5);
    label_set_fmt(meta, "%d dBm / %s dB  %u hop%s via %.16s",
                  entry->rssi_dbm, snr, entry->path_hops,
                  entry->path_hops == 1U ? "" : "s",
                  friendly_repeater_route(entry));
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(meta, 406);
    lv_obj_align(meta, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static lv_obj_t *create_mesh_roles_list(const char *title, uint32_t accent,
                                        size_t loaded_count, uint32_t total_count)
{
    const size_t visible_count = loaded_count < 4U ? loaded_count : 4U;
    if (total_count < loaded_count) {
        total_count = (uint32_t)loaded_count;
    }

    lv_obj_t *panel = create_panel(s_mesh_roles_sheet, 16, 64, 448, 352);
    if (!panel) {
        return NULL;
    }
    lv_obj_set_style_pad_all(panel, 0, 0);

    lv_obj_t *section = create_label(panel, title, accent);
    lv_obj_set_pos(section, 12, 10);
    lv_obj_t *showing = create_label(panel, "", 0x8EA0AE);
    label_set_fmt(showing, "showing %u/%u",
                  (unsigned)visible_count, (unsigned)loaded_count);
    lv_label_set_long_mode(showing, LV_LABEL_LONG_DOT);
    lv_obj_set_width(showing, 160);
    lv_obj_set_style_text_align(showing, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(showing, 276, 10);

    lv_obj_t *list = create_object(panel, "mesh roles bounded list");
    if (!list) {
        return NULL;
    }
    lv_obj_set_size(list, 424, 256);
    lv_obj_set_pos(list, 12, 44);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *footer = create_label(panel, "", 0x8EA0AE);
    if (total_count > loaded_count) {
        label_set_fmt(footer, "Newest %u of %lu loaded - scroll",
                      (unsigned)loaded_count, (unsigned long)total_count);
    } else if (loaded_count > visible_count) {
        label_set_fmt(footer, "Scroll for %u more",
                      (unsigned)(loaded_count - visible_count));
        lv_obj_set_style_text_color(footer, lv_color_hex(accent), 0);
    } else {
        lv_label_set_text(footer, "All shown");
    }
    lv_label_set_long_mode(footer, LV_LABEL_LONG_DOT);
    lv_obj_set_width(footer, 424);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(footer, 12, 312);
    return list;
}

static void render_mesh_roles_root(void)
{
    char room_count[32];
    char repeater_count[32];
    uint32_t room_total = s_snapshot.signal_summary.room_server_count;
    uint32_t repeater_total = s_snapshot.signal_summary.repeater_candidate_count;
    if (room_total < s_snapshot.recent_room_count) {
        room_total = (uint32_t)s_snapshot.recent_room_count;
    }
    if (repeater_total < s_snapshot.recent_repeater_count) {
        repeater_total = (uint32_t)s_snapshot.recent_repeater_count;
    }
    render_mesh_roles_header("Mesh Roles", close_mesh_roles_event_cb);

    lv_obj_t *intro = create_label(s_mesh_roles_sheet,
                                   "Browse one role group at a time.", 0x8EA0AE);
    lv_label_set_long_mode(intro, LV_LABEL_LONG_DOT);
    lv_obj_set_width(intro, 360);
    lv_obj_set_pos(intro, 104, 36);

    snprintf(room_count, sizeof(room_count), "%lu found  >",
             (unsigned long)room_total);
    lv_obj_t *rooms = create_button(s_mesh_roles_sheet, "Rooms",
                                    16, 76, 448, 84,
                                    open_mesh_room_servers_event_cb, NULL);
    style_contact_option_button(rooms, 0xA7F3D0, room_count);

    snprintf(repeater_count, sizeof(repeater_count), "%lu found  >",
             (unsigned long)repeater_total);
    lv_obj_t *repeaters = create_button(s_mesh_roles_sheet, "Repeaters",
                                        16, 176, 448, 84,
                                        open_mesh_repeater_candidates_event_cb, NULL);
    style_contact_option_button(repeaters, 0xFBBF24, repeater_count);

    lv_obj_t *note = create_panel(s_mesh_roles_sheet, 16, 288, 448, 76);
    lv_obj_set_style_pad_all(note, 12, 0);
    lv_obj_t *note_title = create_label(note, "Large meshes stay bounded", 0x93C5FD);
    lv_obj_align(note_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *note_text = create_label(
        note, "Read-only lists. Each role scrolls separately.", 0x8EA0AE);
    lv_label_set_long_mode(note_text, LV_LABEL_LONG_DOT);
    lv_obj_set_width(note_text, 420);
    lv_obj_align(note_text, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void render_mesh_roles_room_servers(void)
{
    uint32_t total = s_snapshot.signal_summary.room_server_count;
    if (total < s_snapshot.recent_room_count) {
        total = (uint32_t)s_snapshot.recent_room_count;
    }
    render_mesh_roles_header("Rooms", mesh_roles_subpage_back_event_cb);
    lv_obj_t *meta = create_label(s_mesh_roles_sheet, "", 0x8EA0AE);
    label_set_fmt(meta, "%lu room server%s",
                  (unsigned long)total, total == 1U ? "" : "s");
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(meta, 360);
    lv_obj_set_pos(meta, 104, 36);

    lv_obj_t *list = create_mesh_roles_list("Room servers", 0xA7F3D0,
                                            s_snapshot.recent_room_count, total);
    if (!list) {
        return;
    }
    int y = 0;
    for (size_t i = 0; i < s_snapshot.recent_room_count; ++i) {
        render_room_server_role_row(list, y, &s_snapshot.recent_rooms[i]);
        y += 64;
    }
    if (s_snapshot.recent_room_count == 0) {
        lv_obj_t *empty = create_label(list, "No room servers heard yet.", 0x8EA0AE);
        lv_obj_set_pos(empty, 8, 12);
    }
}

static void render_mesh_roles_repeater_candidates(void)
{
    uint32_t total = s_snapshot.signal_summary.repeater_candidate_count;
    if (total < s_snapshot.recent_repeater_count) {
        total = (uint32_t)s_snapshot.recent_repeater_count;
    }
    render_mesh_roles_header("Repeaters", mesh_roles_subpage_back_event_cb);
    lv_obj_t *meta = create_label(s_mesh_roles_sheet, "", 0x8EA0AE);
    label_set_fmt(meta, "%lu repeater candidate%s",
                  (unsigned long)total, total == 1U ? "" : "s");
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(meta, 360);
    lv_obj_set_pos(meta, 104, 36);

    lv_obj_t *list = create_mesh_roles_list("Repeater candidates", 0xFBBF24,
                                            s_snapshot.recent_repeater_count, total);
    if (!list) {
        return;
    }
    int y = 0;
    for (size_t i = 0; i < s_snapshot.recent_repeater_count; ++i) {
        render_repeater_role_row(list, y, &s_snapshot.recent_repeaters[i]);
        y += 64;
    }
    if (s_snapshot.recent_repeater_count == 0) {
        lv_obj_t *empty = create_label(list, "No path-hop candidates yet.", 0x8EA0AE);
        lv_obj_set_pos(empty, 8, 12);
    }
}

static void render_mesh_roles_sheet(void)
{
    if (!s_mesh_roles_sheet) {
        return;
    }
    d1l_app_model_snapshot(&s_snapshot);
    update_chrome(&s_snapshot);
    lv_obj_clean(s_mesh_roles_sheet);

    switch (s_mesh_roles_page) {
    case D1L_MESH_ROLES_PAGE_ROOM_SERVERS:
        render_mesh_roles_room_servers();
        break;
    case D1L_MESH_ROLES_PAGE_REPEATER_CANDIDATES:
        render_mesh_roles_repeater_candidates();
        break;
    case D1L_MESH_ROLES_PAGE_ROOT:
    default:
        s_mesh_roles_page = D1L_MESH_ROLES_PAGE_ROOT;
        render_mesh_roles_root();
        break;
    }
}

static void open_mesh_roles_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN)) {
        return;
    }
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    s_mesh_roles_page = D1L_MESH_ROLES_PAGE_ROOT;
    render_mesh_roles_sheet();
    if (s_mesh_roles_sheet) {
        show_modal(s_mesh_roles_sheet);
    }
}

static void close_compose_event_cb(lv_event_t *event)
{
    (void)event;
    hide_compose_sheet();
}

static void clear_compose_event_cb(lv_event_t *event)
{
    (void)event;
    if (d1l_ui_compose_error_clears_on_text_change(
            s_compose_last_send_error)) {
        s_compose_last_send_error = ESP_OK;
    }
    if (s_compose_textarea) {
        lv_textarea_set_text(s_compose_textarea, "");
    }
    update_compose_counter();
}

static void send_compose_text(void)
{
    if (!s_compose_textarea) {
        return;
    }
#if D1L_ENABLE_QUALIFICATION_HOOKS
    if (s_compose_probe_send_suppressed) {
        update_compose_counter();
        return;
    }
#endif
    const char *text = lv_textarea_get_text(s_compose_textarea);
    const d1l_user_text_info_t info = d1l_user_text_validate(text);
    d1l_app_model_snapshot(&s_snapshot);
    const d1l_ui_compose_eligibility_t eligibility =
        compose_eligibility_for_text(&info);
    if (!eligibility.send_enabled) {
        show_toast_text(eligibility.status, false);
        update_compose_counter();
        return;
    }
    esp_err_t ret = s_compose_dm ?
                    d1l_app_model_send_dm_text(s_compose_contact.fingerprint, text) :
                    d1l_app_model_send_channel_text(
                        s_compose_channel_id, text);
    if (ret == ESP_OK) {
        s_compose_last_send_error = ESP_OK;
        show_toast(s_compose_dm ? "DM" : "Channel message", ret);
        lv_textarea_set_text(s_compose_textarea, "");
        update_compose_counter();
        hide_compose_sheet();
        request_content_refresh();
    } else {
        s_compose_last_send_error = ret;
        if (s_compose_dm) {
            /* A failed DM command can already have retained a truthful failed
             * row before RF. Refresh the conversation without claiming that
             * nothing was sent; the draft remains for explicit user retry. */
            request_content_refresh();
        }
        d1l_app_model_snapshot(&s_snapshot);
        const d1l_ui_compose_eligibility_t retry =
            compose_eligibility_for_text(&info);
        show_toast_text(retry.status, false);
        update_compose_counter();
    }
}

static void send_compose_event_cb(lv_event_t *event)
{
    (void)event;
    send_compose_text();
}

static void compose_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        send_compose_text();
    } else if (code == LV_EVENT_CANCEL) {
        hide_compose_sheet();
    }
}

static void compose_textarea_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        if (d1l_ui_compose_error_clears_on_text_change(
                s_compose_last_send_error)) {
            s_compose_last_send_error = ESP_OK;
        }
        update_compose_counter();
    }
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void complete_onboarding_from_ui(const char *fallback_name)
{
    const char *name = fallback_name;
    if (s_onboarding_name_textarea) {
        const char *entered = lv_textarea_get_text(s_onboarding_name_textarea);
        if (entered && entered[0] != '\0') {
            name = entered;
        }
    }
    if (!name || name[0] == '\0') {
        name = D1L_NODE_NAME_FACTORY_DEFAULT;
    }
    esp_err_t ret = d1l_app_model_complete_onboarding(name);
    if (ret == ESP_OK) {
        hide_onboarding_sheet();
        request_content_refresh();
    }
    show_toast("Onboarding", ret);
}

static void onboarding_start_event_cb(lv_event_t *event)
{
    (void)event;
    complete_onboarding_from_ui(D1L_NODE_NAME_FACTORY_DEFAULT);
}

static void onboarding_defaults_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_onboarding_name_textarea) {
        lv_textarea_set_text(s_onboarding_name_textarea,
                             D1L_NODE_NAME_FACTORY_DEFAULT);
    }
    complete_onboarding_from_ui(D1L_NODE_NAME_FACTORY_DEFAULT);
}

static void onboarding_keyboard_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) {
        complete_onboarding_from_ui(D1L_NODE_NAME_FACTORY_DEFAULT);
    }
}
#endif

static const char *public_row_state(const d1l_message_entry_t *entry)
{
    d1l_channel_info_t channel = {0};
    const uint32_t read_seq = entry && snapshot_find_channel(
        &s_snapshot, entry->channel_id, &channel) ?
        snapshot_channel_read_seq(&s_snapshot, &channel) : 0U;
    if (entry && entry->direction[0] == 'r' && entry->seq > read_seq) {
        return "new";
    }
    return entry && entry->direction[0] == 't' ?
        "sent over RF" : "received";
}

static void close_public_history_event_cb(lv_event_t *event)
{
    (void)event;
    hide_public_search_sheet();
    hide_public_history_sheet();
}

static void close_public_search_event_cb(lv_event_t *event)
{
    (void)event;
    hide_public_search_sheet();
    show_public_history_sheet();
}

static void public_history_load_older_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_public_history_limit < D1L_PUBLIC_HISTORY_UI_INITIAL_ROWS) {
        s_public_history_limit = D1L_PUBLIC_HISTORY_UI_INITIAL_ROWS;
    }
    if (s_public_history_limit < D1L_MESSAGE_STORE_CAPACITY) {
        s_public_history_limit += D1L_PUBLIC_HISTORY_UI_LOAD_OLDER_STEP;
        if (s_public_history_limit > D1L_MESSAGE_STORE_CAPACITY) {
            s_public_history_limit = D1L_MESSAGE_STORE_CAPACITY;
        }
    }
    show_public_history_sheet();
}

static void render_public_history_sheet(void)
{
    if (!s_public_history_sheet) {
        return;
    }
    d1l_app_model_snapshot(&s_snapshot);
    update_chrome(&s_snapshot);
    lv_obj_clean(s_public_history_sheet);

    d1l_channel_info_t history_channel = {0};
    const bool channel_found = snapshot_find_channel(
        &s_snapshot, s_public_history_channel_id, &history_channel);
    char history_title[64];
    snprintf(history_title, sizeof(history_title), "%.32s History",
             channel_found && history_channel.name[0] ?
                 history_channel.name : "Channel");
    lv_obj_t *title = create_label(
        s_public_history_sheet, history_title, 0xF4F7FB);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 178);
    lv_obj_set_pos(title, 8, 4);

    create_button(s_public_history_sheet, "Search", 196, 0, 76, 40,
                  open_public_search_event_cb, NULL);
    create_button(s_public_history_sheet, "Clear", 280, 0, 64, 40,
                  open_public_search_event_cb, (void *)"clear");
    create_button(s_public_history_sheet, "Close", 352, 0, 72, 40,
                  close_public_history_event_cb, NULL);

    if (s_public_history_limit < D1L_PUBLIC_HISTORY_UI_INITIAL_ROWS) {
        s_public_history_limit = D1L_PUBLIC_HISTORY_UI_INITIAL_ROWS;
    }
    if (s_public_history_limit > D1L_MESSAGE_STORE_CAPACITY) {
        s_public_history_limit = D1L_MESSAGE_STORE_CAPACITY;
    }
    size_t total_matches = 0;
    const size_t history_count = channel_found ?
        d1l_app_model_query_channel_messages_page(
            s_public_history_channel_id, s_public_history_entries,
            s_public_history_limit, 0, s_public_search_text,
            &total_matches) : 0U;
    lv_obj_t *meta = create_label(s_public_history_sheet, "", 0x8EA0AE);
    if (s_public_search_text[0]) {
        label_set_fmt(meta, "showing %u/%u  find %.18s",
                      (unsigned)history_count, (unsigned)total_matches,
                      s_public_search_text);
    } else {
        label_set_fmt(meta, "showing %u/%u retained",
                      (unsigned)history_count, (unsigned)total_matches);
    }
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(meta, 392);
    lv_obj_set_pos(meta, 8, 42);

    lv_obj_t *list = create_object(s_public_history_sheet, "public history list");
    if (!list) {
        return;
    }
    lv_obj_set_size(list, 424, 190);
    lv_obj_set_pos(list, 0, 70);
    lv_obj_set_style_radius(list, 6, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    int y = 0;
    const uint32_t read_seq = channel_found ?
        snapshot_channel_read_seq(&s_snapshot, &history_channel) : 0U;
    for (size_t i = 0; i < history_count; ++i) {
        const d1l_message_entry_t *entry = &s_public_history_entries[i];
        const bool unread = entry->direction[0] == 'r' &&
            entry->seq > read_seq;
        lv_obj_t *row = create_panel(list, 0, y, 404, 52);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_t *author = create_label(row, entry->author,
                                        unread ? 0xFBBF24 :
                                        (entry->direction[0] == 't' ? 0x93C5FD : 0x5EEAD4));
        lv_label_set_long_mode(author, LV_LABEL_LONG_DOT);
        lv_obj_set_width(author, 156);
        lv_obj_align(author, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *state = create_label(row, "", 0x8EA0AE);
        label_set_fmt(state, "#%lu %s", (unsigned long)entry->seq, public_row_state(entry));
        lv_obj_align(state, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_t *text = create_label(row, entry->text, 0xE5EDF5);
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
        lv_obj_set_width(text, 372);
        lv_obj_align(text, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        y += 58;
    }
    if (history_count == 0) {
        lv_obj_t *empty = create_label(list,
                                       !channel_found ?
                                       "Channel unavailable" :
                                       s_public_search_text[0] ?
                                       "No channel rows match" :
                                       "No retained channel rows",
                                       0x8EA0AE);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
    }
    if (history_count < total_matches && s_public_history_limit < D1L_MESSAGE_STORE_CAPACITY) {
        create_button(s_public_history_sheet, "Load Older", 8, 272, 126, 36,
                      public_history_load_older_event_cb, NULL);
    }
}

static void show_public_history_sheet(void)
{
    render_public_history_sheet();
    if (s_public_history_sheet) {
        show_modal(s_public_history_sheet);
    }
}

static void apply_public_search_event_cb(lv_event_t *event)
{
    (void)event;
    s_public_search_text[0] = '\0';
    if (s_public_search_textarea) {
        const char *text = lv_textarea_get_text(s_public_search_textarea);
        if (text) {
            snprintf(s_public_search_text, sizeof(s_public_search_text), "%s", text);
        }
    }
    s_public_history_limit = D1L_PUBLIC_HISTORY_UI_INITIAL_ROWS;
    hide_public_search_sheet();
    show_public_history_sheet();
}

static void clear_public_search_event_cb(lv_event_t *event)
{
    (void)event;
    s_public_search_text[0] = '\0';
    if (s_public_search_textarea) {
        lv_textarea_set_text(s_public_search_textarea, "");
    }
    s_public_history_limit = D1L_PUBLIC_HISTORY_UI_INITIAL_ROWS;
    hide_public_search_sheet();
    show_public_history_sheet();
}

static void public_search_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        apply_public_search_event_cb(event);
    } else if (code == LV_EVENT_CANCEL) {
        close_public_search_event_cb(event);
    }
}

static void open_public_search_event_cb(lv_event_t *event)
{
    const void *user_data = event ? lv_event_get_user_data(event) : NULL;
    if (user_data && strcmp((const char *)user_data, "clear") == 0) {
        clear_public_search_event_cb(event);
        return;
    }
    if (!s_public_search_sheet) {
        return;
    }
    if (s_public_search_title) {
        char title[64];
        if (s_public_history_channel_id == D1L_CHANNEL_PUBLIC_ID) {
            snprintf(title, sizeof(title), "%s", "Public Search");
        } else {
            snprintf(title, sizeof(title), "Search %.32s",
                     s_public_history_channel_name[0] ?
                         s_public_history_channel_name : "channel");
        }
        lv_label_set_text(s_public_search_title, title);
    }
    if (s_public_search_textarea && s_public_search_keyboard) {
        lv_textarea_set_text(s_public_search_textarea, s_public_search_text);
        lv_keyboard_set_textarea(s_public_search_keyboard, s_public_search_textarea);
    }
    show_modal(s_public_search_sheet);
}

static void open_public_history_event_cb(lv_event_t *event)
{
    (void)event;
    s_public_history_limit = D1L_PUBLIC_HISTORY_UI_INITIAL_ROWS;
    s_public_search_text[0] = '\0';
    hide_sheet();
    hide_compose_sheet();
    hide_public_search_sheet();
    hide_message_detail_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    s_public_history_channel_id =
        s_messages_controller.rendered.active_channel_id;
    snprintf(s_public_history_channel_name,
             sizeof(s_public_history_channel_name), "%s",
             s_messages_controller.rendered.active_channel_name);
    show_public_history_sheet();
}

static size_t load_dm_thread_rows(
    const char *fingerprint,
    d1l_dm_entry_t *out_entries,
    bool *out_unread,
    size_t max_entries,
    size_t skip_newest,
    const char *query,
    size_t *out_total_matches,
    void *context)
{
    (void)context;
    return d1l_app_model_query_dm_thread_page(
        fingerprint, out_entries, out_unread, max_entries, skip_newest,
        query, out_total_matches);
}

static bool messages_backend_available(const char *backend)
{
    return backend && strcmp(backend, "unavailable") != 0;
}

static d1l_ui_messages_store_state_t messages_store_state_from_snapshot(
    const d1l_app_snapshot_t *snapshot,
    bool direct_messages)
{
    if (!snapshot) {
        return D1L_UI_MESSAGES_STORE_UNAVAILABLE;
    }
    return d1l_ui_messages_store_state(
        direct_messages ? snapshot->dm_store_loaded :
            snapshot->message_store_loaded,
        messages_backend_available(
            direct_messages ? snapshot->dm_store_backend :
                snapshot->message_store_backend),
        direct_messages ? snapshot->dm_store_persistence_degraded :
            snapshot->message_store_persistence_degraded);
}

static bool render_dm_thread_sheet(void)
{
    if (!d1l_ui_messages_thread_active(&s_messages_controller)) {
        return false;
    }
    d1l_app_model_snapshot(&s_snapshot);
    update_chrome(&s_snapshot);
    s_messages_controller.rendered.dm_store_state =
        messages_store_state_from_snapshot(&s_snapshot, true);
    const char *fingerprint = d1l_ui_messages_thread_fingerprint(
        &s_messages_controller);
    d1l_contact_entry_t contact = {0};
    const bool reply_available = fingerprint &&
        d1l_app_model_find_contact(fingerprint, &contact) == ESP_OK &&
        dm_identity_for_contact(&contact, NULL).can_open_compose;
    return d1l_ui_messages_render_thread(
        &s_messages_controller,
        load_dm_thread_rows,
        NULL,
        reply_available,
        handle_messages_action,
        NULL);
}

static void show_dm_thread_after_search(void)
{
    hide_dm_search_sheet();
    if (render_dm_thread_sheet()) {
        show_modal(d1l_ui_messages_thread_sheet(&s_messages_controller));
        return;
    }
    hide_dm_thread_sheet();
    show_toast("DM", ESP_ERR_NO_MEM);
}

static void close_dm_search_event_cb(lv_event_t *event)
{
    (void)event;
    show_dm_thread_after_search();
}

static void apply_dm_search_event_cb(lv_event_t *event)
{
    (void)event;
    const char *query = s_dm_search_textarea ?
        lv_textarea_get_text(s_dm_search_textarea) : "";
    if (!d1l_ui_messages_set_thread_search(
            &s_messages_controller, query ? query : "")) {
        show_toast("DM search", ESP_ERR_INVALID_ARG);
        return;
    }
    show_dm_thread_after_search();
}

static void clear_dm_search_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_ui_messages_set_thread_search(&s_messages_controller, NULL)) {
        show_toast("DM search", ESP_ERR_INVALID_STATE);
        return;
    }
    if (s_dm_search_textarea) {
        lv_textarea_set_text(s_dm_search_textarea, "");
    }
    show_dm_thread_after_search();
}

static void dm_search_keyboard_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        apply_dm_search_event_cb(event);
    } else if (code == LV_EVENT_CANCEL) {
        close_dm_search_event_cb(event);
    }
}

static void open_dm_search_event_cb(lv_event_t *event)
{
    (void)event;
    const char *query = d1l_ui_messages_thread_search(&s_messages_controller);
    if (!query || !s_dm_search_sheet) {
        show_toast("DM search", ESP_ERR_INVALID_STATE);
        return;
    }
    if (s_dm_search_textarea && s_dm_search_keyboard) {
        lv_textarea_set_text(s_dm_search_textarea, query);
        lv_keyboard_set_textarea(s_dm_search_keyboard, s_dm_search_textarea);
    }
    show_modal(s_dm_search_sheet);
}

static void show_dm_thread_for(const char *fingerprint, const char *alias)
{
    if (!d1l_ui_messages_select_thread(
            &s_messages_controller, fingerprint, alias)) {
        show_toast("DM", ESP_ERR_INVALID_STATE);
        return;
    }
    if (d1l_app_model_mark_dm_thread_read(fingerprint) == ESP_OK) {
        request_content_refresh();
    }
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_radio_settings_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    if (!render_dm_thread_sheet()) {
        hide_dm_thread_sheet();
        show_toast("DM", ESP_ERR_NO_MEM);
        return;
    }
    show_modal(d1l_ui_messages_thread_sheet(&s_messages_controller));
}

static void open_home_dm_preview_event_cb(lv_event_t *event)
{
    const d1l_home_message_preview_t *entry =
        (const d1l_home_message_preview_t *)lv_event_get_user_data(event);
    if (!entry || !entry->is_dm) {
        show_toast("DM", ESP_ERR_INVALID_STATE);
        return;
    }
    show_dm_thread_for(entry->target_fingerprint, entry->sender);
}

static void set_messages_mode(d1l_ui_messages_mode_t mode)
{
    if (mode != D1L_UI_MESSAGES_MODE_ROOT &&
        mode != D1L_UI_MESSAGES_MODE_PUBLIC &&
        mode != D1L_UI_MESSAGES_MODE_DIRECT) {
        mode = D1L_UI_MESSAGES_MODE_ROOT;
    }
    s_messages_mode = mode;
    if (d1l_ui_navigation_active() == D1L_UI_TAB_MESSAGES) {
        request_content_refresh();
        return;
    }
    request_tab_switch(D1L_UI_TAB_MESSAGES);
}

static void messages_view_model_from_snapshot(const d1l_app_snapshot_t *snapshot,
                                              d1l_ui_messages_view_model_t *view_model)
{
    if (!snapshot || !view_model) {
        return;
    }
    memset(view_model, 0, sizeof(*view_model));
    view_model->mode = s_messages_mode;
    view_model->dm_total = snapshot->dm_conversation_count;
    view_model->dm_unread = snapshot->dm_unread_count;
    view_model->muted_dm_unread = snapshot->muted_dm_unread_count;
    view_model->dm_capable_contact_count =
        snapshot->dm_capable_contact_count;
    view_model->public_store_state =
        messages_store_state_from_snapshot(snapshot, false);
    view_model->dm_store_state =
        messages_store_state_from_snapshot(snapshot, true);

    const bool channel_management_available =
        d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT);
    view_model->channel_store_loaded =
        channel_management_available && snapshot->channel_store_loaded;
    if (channel_management_available) {
        view_model->channel_count = snapshot->channel_count;
        if (view_model->channel_count > D1L_CHANNEL_STORE_CAPACITY) {
            view_model->channel_count = D1L_CHANNEL_STORE_CAPACITY;
        }
        memcpy(view_model->channels, snapshot->channels,
               view_model->channel_count * sizeof(view_model->channels[0]));
    }
    d1l_channel_info_t active_channel = {0};
    bool active_channel_found = false;
    view_model->active_channel_id = d1l_ui_messages_projected_channel_id(
        snapshot->active_channel_id);
    active_channel_found = snapshot_find_channel(
        snapshot, view_model->active_channel_id, &active_channel);
    if (active_channel_found) {
        snprintf(view_model->active_channel_name,
                 sizeof(view_model->active_channel_name), "%s",
                 active_channel.name);
        view_model->active_channel_enabled = active_channel.enabled;
        view_model->public_unread = active_channel.unread_count;
        view_model->last_channel_read_seq = snapshot_channel_read_seq(
            snapshot, &active_channel);
        size_t total_matches = 0U;
        view_model->public_row_count =
            d1l_app_model_query_channel_messages_page(
                active_channel.channel_id, view_model->public_rows,
                D1L_UI_MESSAGES_PUBLIC_PREVIEW_ROWS, 0U, NULL,
                &total_matches);
        if (view_model->public_row_count >
            D1L_UI_MESSAGES_PUBLIC_PREVIEW_ROWS) {
            view_model->public_row_count =
                D1L_UI_MESSAGES_PUBLIC_PREVIEW_ROWS;
        }
        view_model->public_total = total_matches;
    }

    view_model->dm_row_count = snapshot->recent_dm_count;
    if (view_model->dm_row_count > D1L_UI_MESSAGES_DM_PREVIEW_ROWS) {
        view_model->dm_row_count = D1L_UI_MESSAGES_DM_PREVIEW_ROWS;
    }
    memcpy(view_model->dm_rows, snapshot->recent_dms,
           view_model->dm_row_count * sizeof(view_model->dm_rows[0]));
    memcpy(view_model->dm_row_unread, snapshot->recent_dm_unread,
           view_model->dm_row_count * sizeof(view_model->dm_row_unread[0]));
    memcpy(view_model->dm_row_unread_count,
           snapshot->recent_dm_unread_count,
           view_model->dm_row_count *
               sizeof(view_model->dm_row_unread_count[0]));
    memcpy(view_model->dm_row_muted, snapshot->recent_dm_muted,
           view_model->dm_row_count * sizeof(view_model->dm_row_muted[0]));
    view_model->dm_retry_active = snapshot->dm_retry_active;
    view_model->dm_failure_latched = snapshot->dm_failure_latched;
}

static bool show_channel_selector_sheet(void)
{
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT)) {
        return false;
    }
    d1l_app_model_snapshot(&s_snapshot);
    messages_view_model_from_snapshot(
        &s_snapshot, &s_messages_controller.rendered);
    if (!d1l_ui_messages_render_channel_selector(
            &s_messages_controller, handle_messages_action, NULL)) {
        d1l_ui_messages_hide_channel_selector(&s_messages_controller);
        return false;
    }
    show_modal(d1l_ui_messages_channel_selector_sheet(
        &s_messages_controller));
    return true;
}

static void channel_management_action_handler(
    const d1l_ui_channel_action_event_t *event,
    void *context);

static bool show_channel_create_sheet(void)
{
    d1l_ui_messages_hide_channel_selector(&s_messages_controller);
    d1l_ui_channel_sheets_hide_all(&s_channel_sheets_controller, false);
    if (!d1l_ui_channel_sheets_render_create(
            &s_channel_sheets_controller,
            channel_management_action_handler, NULL)) {
        d1l_ui_channel_sheets_hide_create(&s_channel_sheets_controller);
        return false;
    }
    show_modal(d1l_ui_channel_sheets_create_sheet(
        &s_channel_sheets_controller));
    return true;
}

static bool show_channel_import_sheet(void)
{
    d1l_ui_messages_hide_channel_selector(&s_messages_controller);
    d1l_ui_channel_sheets_hide_all(&s_channel_sheets_controller, false);
    if (!d1l_ui_channel_sheets_render_import(
            &s_channel_sheets_controller,
            channel_management_action_handler, NULL)) {
        d1l_ui_channel_sheets_hide_import(&s_channel_sheets_controller);
        return false;
    }
    show_modal(d1l_ui_channel_sheets_import_sheet(
        &s_channel_sheets_controller));
    return true;
}

static bool show_channel_options_sheet(void)
{
    d1l_ui_messages_hide_channel_selector(&s_messages_controller);
    d1l_ui_channel_sheets_hide_all(&s_channel_sheets_controller, false);
    if (!d1l_ui_channel_sheets_render_options(
            &s_channel_sheets_controller,
            channel_management_action_handler, NULL)) {
        d1l_ui_channel_sheets_hide_options(&s_channel_sheets_controller);
        return false;
    }
    show_modal(d1l_ui_channel_sheets_options(
        &s_channel_sheets_controller));
    return true;
}

static bool show_channel_edit_sheet(void)
{
    d1l_ui_channel_sheets_hide_all(&s_channel_sheets_controller, false);
    if (!d1l_ui_channel_sheets_render_edit(
            &s_channel_sheets_controller,
            channel_management_action_handler, NULL)) {
        d1l_ui_channel_sheets_hide_edit(&s_channel_sheets_controller);
        return false;
    }
    show_modal(d1l_ui_channel_sheets_edit(
        &s_channel_sheets_controller));
    return true;
}

static bool show_channel_remove_sheet(void)
{
    d1l_ui_channel_sheets_hide_all(&s_channel_sheets_controller, false);
    if (!d1l_ui_channel_sheets_render_remove(
            &s_channel_sheets_controller,
            channel_management_action_handler, NULL)) {
        d1l_ui_channel_sheets_hide_remove(&s_channel_sheets_controller);
        return false;
    }
    show_modal(d1l_ui_channel_sheets_remove(
        &s_channel_sheets_controller));
    return true;
}

static void show_channel_mutation_result(
    const char *action,
    esp_err_t ret,
    d1l_channel_mutation_result_t result)
{
    if (ret == ESP_OK) {
        show_toast_text(action, true);
        return;
    }
    switch (result) {
    case D1L_CHANNEL_MUTATION_NAME_COLLISION:
        show_toast_text("Channel name already exists", false);
        return;
    case D1L_CHANNEL_MUTATION_SECRET_COLLISION:
        show_toast_text("Channel key already exists", false);
        return;
    case D1L_CHANNEL_MUTATION_FULL:
        show_toast_text("All channel slots are in use", false);
        return;
    case D1L_CHANNEL_MUTATION_PROTECTED:
        show_toast_text("Public channel is protected", false);
        return;
    case D1L_CHANNEL_MUTATION_NONE:
    case D1L_CHANNEL_MUTATION_CREATED:
    case D1L_CHANNEL_MUTATION_UPDATED:
    case D1L_CHANNEL_MUTATION_REMOVED:
    case D1L_CHANNEL_MUTATION_EXISTS:
    default:
        show_toast(action, ret);
        return;
    }
}

static bool set_managed_channel(const d1l_channel_info_t *channel)
{
    return channel && channel->channel_id != 0U &&
        d1l_ui_channel_sheets_set_channel(
            &s_channel_sheets_controller, channel);
}

static bool show_channel_export_sheet(void)
{
    const d1l_channel_info_t *channel =
        d1l_ui_channel_sheets_channel(&s_channel_sheets_controller);
    char uri[D1L_CHANNEL_SHARE_URI_LEN] = {0};
    esp_err_t ret = channel ?
        d1l_app_model_export_channel_share_uri(
            channel->channel_id, uri, sizeof(uri)) :
        ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK && !d1l_ui_channel_sheets_set_export_uri(
            &s_channel_sheets_controller, uri)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    d1l_app_model_clear_channel_share_uri(uri, sizeof(uri));
    if (ret != ESP_OK) {
        d1l_ui_channel_sheets_clear_export_uri(
            &s_channel_sheets_controller);
        show_toast("Channel export", ret);
        return false;
    }
    d1l_ui_channel_sheets_hide_options(&s_channel_sheets_controller);
    if (!d1l_ui_channel_sheets_render_export(
            &s_channel_sheets_controller,
            channel_management_action_handler, NULL)) {
        d1l_ui_channel_sheets_hide_export(&s_channel_sheets_controller);
        show_toast("Channel export", ESP_ERR_NO_MEM);
        return false;
    }
    show_modal(d1l_ui_channel_sheets_export(
        &s_channel_sheets_controller));
    return true;
}

static void return_to_channel_selector(void)
{
    d1l_ui_channel_sheets_hide_all(&s_channel_sheets_controller, true);
    request_content_refresh();
    if (!show_channel_selector_sheet()) {
        show_toast("Channels", ESP_ERR_NO_MEM);
    }
}

static size_t bounded_channel_uri_length(const char *text)
{
    if (!text) {
        return 0U;
    }
    const char *end = memchr(text, '\0', D1L_CHANNEL_SHARE_URI_LEN);
    return end ? (size_t)(end - text) : 0U;
}

static void channel_management_action_handler(
    const d1l_ui_channel_action_event_t *event,
    void *context)
{
    (void)context;
    if (!event ||
        !d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT)) {
        return;
    }
    const d1l_channel_info_t *channel = event->channel;
    d1l_channel_mutation_result_t result = D1L_CHANNEL_MUTATION_NONE;
    d1l_channel_info_t updated = {0};
    esp_err_t ret = ESP_OK;

    switch (event->action) {
    case D1L_UI_CHANNEL_ACTION_CANCEL_CREATE:
    case D1L_UI_CHANNEL_ACTION_CANCEL_IMPORT:
        return_to_channel_selector();
        return;
    case D1L_UI_CHANNEL_ACTION_SUBMIT_CREATE:
        ret = d1l_app_model_create_channel(
            event->text, false, &result, &updated);
        show_channel_mutation_result("Channel created", ret, result);
        if (ret == ESP_OK && set_managed_channel(&updated)) {
            d1l_ui_channel_sheets_hide_create(
                &s_channel_sheets_controller);
            if (!d1l_release_feature_available(
                    D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI) ||
                !show_channel_export_sheet()) {
                show_channel_options_sheet();
            }
        }
        return;
    case D1L_UI_CHANNEL_ACTION_SUBMIT_IMPORT: {
        const size_t uri_len = bounded_channel_uri_length(event->text);
        ret = uri_len > 0U ? d1l_app_model_import_channel_uri(
            event->text, uri_len, &result, &updated) :
            ESP_ERR_INVALID_ARG;
        d1l_ui_channel_sheets_hide_import(&s_channel_sheets_controller);
        show_channel_mutation_result("Channel imported", ret, result);
        if (ret == ESP_OK && set_managed_channel(&updated)) {
            show_channel_options_sheet();
        } else if (ret != ESP_OK && !show_channel_import_sheet()) {
            return_to_channel_selector();
        }
        return;
    }
    case D1L_UI_CHANNEL_ACTION_CLOSE_OPTIONS:
        return_to_channel_selector();
        return;
    case D1L_UI_CHANNEL_ACTION_OPEN_EDIT:
        if (!show_channel_edit_sheet()) {
            show_toast("Channel edit", ESP_ERR_NO_MEM);
        }
        return;
    case D1L_UI_CHANNEL_ACTION_CANCEL_EDIT:
        if (!show_channel_options_sheet()) {
            return_to_channel_selector();
        }
        return;
    case D1L_UI_CHANNEL_ACTION_SUBMIT_EDIT:
        ret = channel ? d1l_app_model_update_channel(
            channel->channel_id, event->text, channel->enabled,
            channel->is_default, &result, &updated) :
            ESP_ERR_INVALID_STATE;
        show_channel_mutation_result("Channel updated", ret, result);
        if (ret == ESP_OK && set_managed_channel(&updated)) {
            show_channel_options_sheet();
        }
        return;
    case D1L_UI_CHANNEL_ACTION_TOGGLE_ENABLED:
        ret = channel && channel->channel_id != D1L_CHANNEL_PUBLIC_ID ?
            d1l_app_model_update_channel(
                channel->channel_id, channel->name, !channel->enabled,
                false, &result, &updated) :
            ESP_ERR_NOT_SUPPORTED;
        show_channel_mutation_result(
            channel && channel->enabled ? "Channel disabled" :
                "Channel enabled",
            ret, result);
        if (ret == ESP_OK && set_managed_channel(&updated)) {
            show_channel_options_sheet();
        }
        return;
    case D1L_UI_CHANNEL_ACTION_MAKE_DEFAULT:
        ret = channel && channel->enabled ? d1l_app_model_update_channel(
            channel->channel_id, channel->name, true, true,
            &result, &updated) : ESP_ERR_INVALID_STATE;
        show_channel_mutation_result("Default channel updated", ret, result);
        if (ret == ESP_OK && set_managed_channel(&updated)) {
            show_channel_options_sheet();
        }
        return;
    case D1L_UI_CHANNEL_ACTION_OPEN_EXPORT:
        if (d1l_release_feature_available(
                D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI)) {
            show_channel_export_sheet();
        }
        return;
    case D1L_UI_CHANNEL_ACTION_CLOSE_EXPORT:
        d1l_ui_channel_sheets_hide_export(&s_channel_sheets_controller);
        if (!show_channel_options_sheet()) {
            return_to_channel_selector();
        }
        return;
    case D1L_UI_CHANNEL_ACTION_OPEN_REMOVE:
        if (!show_channel_remove_sheet()) {
            show_toast("Channel remove", ESP_ERR_NO_MEM);
        }
        return;
    case D1L_UI_CHANNEL_ACTION_CANCEL_REMOVE:
        if (!show_channel_options_sheet()) {
            return_to_channel_selector();
        }
        return;
    case D1L_UI_CHANNEL_ACTION_CONFIRM_REMOVE:
        ret = channel ? d1l_app_model_remove_channel(
            channel->channel_id, true, &result, &updated) :
            ESP_ERR_INVALID_STATE;
        show_channel_mutation_result("Channel removed", ret, result);
        if (ret == ESP_OK) {
            return_to_channel_selector();
        }
        return;
    case D1L_UI_CHANNEL_ACTION_NONE:
    default:
        return;
    }
}

static void handle_messages_action(const d1l_ui_messages_action_event_t *event,
                                   void *context)
{
    (void)context;
    if (!event || !d1l_ui_messages_action_available(event->action)) {
        return;
    }
    switch (event->action) {
    case D1L_UI_MESSAGES_ACTION_MARK_PUBLIC_READ:
        mark_public_read_event_cb(NULL);
        break;
    case D1L_UI_MESSAGES_ACTION_COMPOSE_PUBLIC:
        open_compose_event_cb(NULL);
        break;
    case D1L_UI_MESSAGES_ACTION_OPEN_HISTORY:
        open_public_history_event_cb(NULL);
        break;
    case D1L_UI_MESSAGES_ACTION_SHOW_ROOT:
        set_messages_mode(D1L_UI_MESSAGES_MODE_ROOT);
        break;
    case D1L_UI_MESSAGES_ACTION_SHOW_PUBLIC:
        set_messages_mode(D1L_UI_MESSAGES_MODE_PUBLIC);
        break;
    case D1L_UI_MESSAGES_ACTION_SHOW_DIRECT:
        set_messages_mode(D1L_UI_MESSAGES_MODE_DIRECT);
        break;
    case D1L_UI_MESSAGES_ACTION_OPEN_PUBLIC_MESSAGE:
        show_message_detail_for(event->public_message);
        break;
    case D1L_UI_MESSAGES_ACTION_OPEN_DM_THREAD:
        if (!event->dm_message || event->dm_message->contact_fingerprint[0] == '\0') {
            show_toast("DM", ESP_ERR_INVALID_STATE);
            break;
        }
        show_dm_thread_for(event->dm_message->contact_fingerprint,
                           event->dm_message->contact_alias[0] ?
                           event->dm_message->contact_alias :
                           event->dm_message->contact_fingerprint);
        break;
    case D1L_UI_MESSAGES_ACTION_CLOSE_DM_THREAD:
        hide_dm_thread_sheet();
        request_content_refresh();
        break;
    case D1L_UI_MESSAGES_ACTION_LOAD_OLDER_DM_THREAD:
        if (d1l_ui_messages_expand_thread(&s_messages_controller)) {
            if (render_dm_thread_sheet()) {
                show_modal(d1l_ui_messages_thread_sheet(&s_messages_controller));
            } else {
                hide_dm_thread_sheet();
                request_content_refresh();
                show_toast("DM", ESP_ERR_NO_MEM);
            }
        }
        break;
    case D1L_UI_MESSAGES_ACTION_OPEN_DM_SEARCH:
        open_dm_search_event_cb(NULL);
        break;
    case D1L_UI_MESSAGES_ACTION_REPLY_DM_THREAD: {
        const char *fingerprint = d1l_ui_messages_thread_fingerprint(
            &s_messages_controller);
        d1l_contact_entry_t contact = {0};
        esp_err_t ret = fingerprint ?
            d1l_app_model_find_contact(fingerprint, &contact) :
            ESP_ERR_INVALID_STATE;
        if (ret == ESP_OK) {
            open_dm_compose_for_contact(&contact);
            request_content_refresh();
        } else {
            show_toast("DM", ret);
        }
        break;
    }
    case D1L_UI_MESSAGES_ACTION_TOGGLE_DM_DETAILS:
        if (d1l_ui_messages_toggle_thread_details(
                &s_messages_controller, event->dm_message)) {
            if (render_dm_thread_sheet()) {
                show_modal(d1l_ui_messages_thread_sheet(&s_messages_controller));
            } else {
                hide_dm_thread_sheet();
                request_content_refresh();
                show_toast("DM", ESP_ERR_NO_MEM);
            }
        }
        break;
    case D1L_UI_MESSAGES_ACTION_OPEN_CHANNEL_SELECTOR:
        if (!show_channel_selector_sheet()) {
            show_toast("Channels", ESP_ERR_NO_MEM);
            request_content_refresh();
        }
        break;
    case D1L_UI_MESSAGES_ACTION_CLOSE_CHANNEL_SELECTOR:
        d1l_ui_messages_hide_channel_selector(&s_messages_controller);
        request_content_refresh();
        break;
    case D1L_UI_MESSAGES_ACTION_CREATE_CHANNEL:
        if (!show_channel_create_sheet()) {
            show_toast("New channel", ESP_ERR_NO_MEM);
            return_to_channel_selector();
        }
        break;
    case D1L_UI_MESSAGES_ACTION_IMPORT_CHANNEL:
        if (!show_channel_import_sheet()) {
            show_toast("Import channel", ESP_ERR_NO_MEM);
            return_to_channel_selector();
        }
        break;
    case D1L_UI_MESSAGES_ACTION_SELECT_CHANNEL:
        if (!event->channel || !event->channel->enabled ||
            event->channel->channel_id == 0U) {
            show_toast("Channels", ESP_ERR_INVALID_STATE);
            break;
        }
        {
            d1l_channel_info_t selected = {0};
            const esp_err_t ret = d1l_app_model_select_channel(
                event->channel->channel_id, &selected);
            show_toast("Channels", ret);
            if (ret == ESP_OK) {
                s_public_search_text[0] = '\0';
                d1l_ui_messages_hide_channel_selector(
                    &s_messages_controller);
                request_content_refresh();
            }
        }
        break;
    case D1L_UI_MESSAGES_ACTION_MANAGE_CHANNEL:
        if (!set_managed_channel(event->channel)) {
            show_toast("Manage channel", ESP_ERR_INVALID_STATE);
            break;
        }
        if (!show_channel_options_sheet()) {
            show_toast("Manage channel", ESP_ERR_NO_MEM);
            return_to_channel_selector();
        }
        break;
    case D1L_UI_MESSAGES_ACTION_NONE:
    default:
        break;
    }
}

static void render_messages(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)
{
    messages_view_model_from_snapshot(snapshot, &s_messages_controller.rendered);
    d1l_ui_messages_render(&s_messages_controller, content,
                           &s_messages_controller.rendered,
                           handle_messages_action, NULL);
}

static void nodes_view_model_from_snapshot(const d1l_app_snapshot_t *snapshot,
                                           d1l_ui_nodes_view_model_t *view_model)
{
    if (!snapshot || !view_model) {
        return;
    }
    memset(view_model, 0, sizeof(*view_model));
    view_model->contact_count = snapshot->contact_count;

    view_model->contact_row_count = snapshot->recent_contact_count;
    if (view_model->contact_row_count > D1L_APP_SNAPSHOT_CONTACT_PREVIEW) {
        view_model->contact_row_count = D1L_APP_SNAPSHOT_CONTACT_PREVIEW;
    }
    memcpy(view_model->contact_rows, snapshot->recent_contacts,
           view_model->contact_row_count * sizeof(view_model->contact_rows[0]));
    for (size_t i = 0; i < view_model->contact_row_count; ++i) {
        view_model->contact_can_dm[i] = contact_can_dm(&view_model->contact_rows[i]);
    }

    const d1l_node_query_t node_query = {
        .filter = D1L_NODE_FILTER_ALL,
        .sort = D1L_NODE_SORT_LAST_HEARD,
        .text = NULL,
        .keyed_only = false,
        .reachable_only = false,
    };
    view_model->node_row_count = d1l_app_model_query_nodes(
        &node_query, view_model->node_rows, D1L_NODE_STORE_CAPACITY);
    if (view_model->node_row_count > D1L_NODE_STORE_CAPACITY) {
        view_model->node_row_count = D1L_NODE_STORE_CAPACITY;
    }
    const char *node_roles[D1L_NODE_STORE_CAPACITY] = {0};
    for (size_t i = 0; i < view_model->node_row_count; ++i) {
        node_roles[i] = view_model->node_rows[i].role;
    }
    d1l_ui_nodes_summarize_roles(
        node_roles, view_model->node_row_count, D1L_NODE_STORE_CAPACITY,
        &view_model->role_counts);
    for (size_t i = 0; i < view_model->node_row_count; ++i) {
        view_model->node_can_dm[i] = node_view_can_dm(&view_model->node_rows[i]);
    }
}

static const char *finder_role_name(uint8_t node_type)
{
    switch (node_type) {
    case D1L_MESHCORE_DISCOVERY_NODE_TYPE_CHAT:
        return "Chat";
    case D1L_MESHCORE_DISCOVERY_NODE_TYPE_REPEATER:
        return "Repeater";
    case D1L_MESHCORE_DISCOVERY_NODE_TYPE_ROOM:
        return "Room";
    case D1L_MESHCORE_DISCOVERY_NODE_TYPE_SENSOR:
        return "Sensor";
    default:
        return "Node";
    }
}

static void close_finder_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    hide_finder_sheet();
}

static void finder_scan_event_cb(lv_event_t *event)
{
    (void)event;
    const esp_err_t ret = d1l_app_model_discover_nearby();
    if (ret == ESP_OK) {
        show_toast_text("Nearby scan sent", true);
    } else if (ret != ESP_ERR_NOT_FINISHED) {
        show_toast("Nearby scan", ret);
    }
    render_finder_sheet();
}

static void render_finder_sheet(void)
{
    if (!s_finder_sheet) {
        return;
    }
    const lv_coord_t previous_scroll_y = lv_obj_get_scroll_y(s_finder_sheet);
    lv_obj_clean(s_finder_sheet);
    d1l_app_model_discovery_snapshot(&s_finder_snapshot);
    s_finder_rendered_generation = s_finder_snapshot.generation;
    s_finder_last_render_second = lv_tick_get() / 1000U;

    create_button(s_finder_sheet, "Back", 12, 6, 72, 44,
                  close_finder_sheet_event_cb, NULL);
    lv_obj_t *title =
        create_label(s_finder_sheet, "Find Nearby", 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, 250);
        lv_obj_set_pos(title, 94, 10);
    }
    create_button(s_finder_sheet, "Scan", 380, 6, 84, 44,
                  finder_scan_event_cb, NULL);

    char state[96] = {0};
    if (s_finder_snapshot.active) {
        snprintf(state, sizeof(state),
                 "Listening %lus  found %u  responses %lu",
                 (unsigned long)(
                     (s_finder_snapshot.remaining_ms + 999U) / 1000U),
                 (unsigned)s_finder_snapshot.result_count,
                 (unsigned long)s_finder_snapshot.total_responses);
    } else {
        snprintf(state, sizeof(state),
                 "Scan idle  found %u  responses %lu",
                 (unsigned)s_finder_snapshot.result_count,
                 (unsigned long)s_finder_snapshot.total_responses);
    }
    lv_obj_t *status = create_label(s_finder_sheet, state,
                                    s_finder_snapshot.active ?
                                        0x5EEAD4 : 0x8EA0AE);
    if (status) {
        lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
        lv_obj_set_width(status, 448);
        lv_obj_set_pos(status, 16, 62);
    }
    lv_obj_t *boundary = create_label(
        s_finder_sheet,
        "Zero-hop RF only. Discovery keys are unverified until a signed advert is received.",
        0xFBBF24);
    if (boundary) {
        lv_label_set_long_mode(boundary, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(boundary, 448);
        lv_obj_set_pos(boundary, 16, 88);
    }

    int y = 132;
    for (size_t i = 0U; i < s_finder_snapshot.result_count; ++i) {
        const d1l_meshcore_discovery_result_t *entry =
            &s_finder_snapshot.results[i];
        d1l_contact_entry_t contact = {0};
        const bool known =
            d1l_app_model_find_contact_by_public_key(
                entry->public_key_hex, &contact) == ESP_OK;
        const char *name = known && contact.alias[0] ?
            contact.alias : entry->fingerprint;

        lv_obj_t *panel =
            create_object(s_finder_sheet, "finder result row");
        if (!panel) {
            continue;
        }
        lv_obj_set_size(panel, 448, 74);
        lv_obj_set_pos(panel, 16, y);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x111923), 0);
        lv_obj_set_style_border_color(
            panel, lv_color_hex(known ? 0x2DD4BF : 0x334155), 0);
        lv_obj_set_style_border_width(panel, 1, 0);
        lv_obj_set_style_radius(panel, 8, 0);
        lv_obj_set_style_pad_all(panel, 8, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

        char heading[96] = {0};
        snprintf(heading, sizeof(heading), "%s  %s%s",
                 name, finder_role_name(entry->node_type),
                 known ? "  contact" : "");
        lv_obj_t *heading_label =
            create_label(panel, heading, known ? 0x5EEAD4 : 0xF4F7FB);
        if (heading_label) {
            lv_label_set_long_mode(heading_label, LV_LABEL_LONG_DOT);
            lv_obj_set_width(heading_label, 416);
            lv_obj_set_pos(heading_label, 8, 2);
        }

        const int local_abs = entry->local_snr_quarter_db < 0 ?
            -entry->local_snr_quarter_db : entry->local_snr_quarter_db;
        const int remote_abs = entry->remote_snr_quarter_db < 0 ?
            -entry->remote_snr_quarter_db : entry->remote_snr_quarter_db;
        char metrics[160] = {0};
        snprintf(
            metrics, sizeof(metrics),
            "RSSI %d  there %s%d.%02d  back %s%d.%02d  %lus ago",
            entry->last_rssi_dbm,
            entry->remote_snr_quarter_db < 0 ? "-" : "",
            remote_abs / 4, (remote_abs % 4) * 25,
            entry->local_snr_quarter_db < 0 ? "-" : "",
            local_abs / 4, (local_abs % 4) * 25,
            (unsigned long)(entry->age_ms / 1000U));
        lv_obj_t *metrics_label =
            create_label(panel, metrics, 0x8EA0AE);
        if (metrics_label) {
            lv_label_set_long_mode(metrics_label, LV_LABEL_LONG_DOT);
            lv_obj_set_width(metrics_label, 416);
            lv_obj_set_pos(metrics_label, 8, 28);
        }
        lv_obj_t *key = create_label(
            panel, entry->public_key_hex, 0x64748B);
        if (key) {
            lv_label_set_long_mode(key, LV_LABEL_LONG_DOT);
            lv_obj_set_width(key, 416);
            lv_obj_set_pos(key, 8, 50);
        }
        y += 84;
    }
    if (s_finder_snapshot.result_count == 0U) {
        lv_obj_t *empty = create_label(
            s_finder_sheet,
            s_finder_snapshot.active ?
                "Waiting for compatible chats, repeaters, rooms, and sensors..." :
                "Tap Scan to actively find compatible zero-hop nodes.",
            0x8EA0AE);
        if (empty) {
            lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(empty, 448);
            lv_obj_set_pos(empty, 16, y + 12);
        }
    }
    lv_obj_update_layout(s_finder_sheet);
    lv_obj_scroll_to_y(s_finder_sheet, previous_scroll_y, LV_ANIM_OFF);
}

static void handle_nodes_action(const d1l_ui_nodes_action_event_t *event,
                                void *context)
{
    (void)context;
    if (!event) {
        return;
    }
    switch (event->action) {
    case D1L_UI_NODES_ACTION_OPEN_CONTACT:
        show_contact_detail_for(event->contact);
        break;
    case D1L_UI_NODES_ACTION_OPEN_CONTACT_DM:
        open_dm_compose_for_contact(event->contact);
        break;
    case D1L_UI_NODES_ACTION_OPEN_NODE:
        show_node_detail_view(event->node, false);
        break;
    case D1L_UI_NODES_ACTION_OPEN_NODE_DM:
        open_node_dm_for(event->node);
        break;
    case D1L_UI_NODES_ACTION_FIND_NEARBY: {
        const esp_err_t ret = d1l_app_model_discover_nearby();
        if (ret == ESP_OK) {
            show_toast_text("Nearby scan sent", true);
        } else if (ret != ESP_ERR_NOT_FINISHED) {
            show_toast("Nearby scan", ret);
        }
        render_finder_sheet();
        show_modal(s_finder_sheet);
        break;
    }
    case D1L_UI_NODES_ACTION_CLEAR_HEARD:
        if (!confirmation_active(
                s_nodes_clear_armed, s_nodes_clear_deadline)) {
            s_nodes_clear_armed = true;
            s_nodes_clear_deadline = lv_tick_get() + 5000U;
            show_toast_text("Tap Clear again to erase heard nodes", true);
        } else {
            s_nodes_clear_armed = false;
            const esp_err_t ret = d1l_app_model_clear_nodes(true);
            show_toast("Clear heard nodes", ret);
            if (ret == ESP_OK) {
                request_content_refresh();
            }
        }
        break;
    case D1L_UI_NODES_ACTION_NONE:
    default:
        break;
    }
}

static void render_nodes(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)
{
    nodes_view_model_from_snapshot(snapshot, &s_nodes_controller.rendered);
    d1l_ui_nodes_render(&s_nodes_controller, content, &s_nodes_controller.rendered,
                        handle_nodes_action, NULL);
}

static void close_map_location_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    hide_map_location_sheet();
    if (s_map_location_returns_to_options) {
        s_map_location_returns_to_options = false;
        d1l_app_model_snapshot(&s_snapshot);
        if (render_map_options_sheet(D1L_UI_MAP_OPTIONS_ROOT)) {
            show_modal(d1l_ui_map_options_sheet(&s_map_sheets_controller));
        }
    } else {
        request_content_refresh();
    }
}

static void map_location_textarea_event_cb(lv_event_t *event)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_LOCATION)) {
        return;
    }
    lv_obj_t *keyboard =
        d1l_ui_map_location_keyboard(&s_map_sheets_controller);
    lv_obj_t *latitude =
        d1l_ui_map_latitude_textarea(&s_map_sheets_controller);
    lv_obj_t *longitude =
        d1l_ui_map_longitude_textarea(&s_map_sheets_controller);
    if (d1l_ui_keyboard_focus_textarea_from_event(keyboard, event, latitude,
                                                   longitude)) {
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(keyboard);
    }
}

static void map_location_save_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_LOCATION)) {
        return;
    }
    lv_obj_t *latitude =
        d1l_ui_map_latitude_textarea(&s_map_sheets_controller);
    lv_obj_t *longitude =
        d1l_ui_map_longitude_textarea(&s_map_sheets_controller);
    const char *lat_text = latitude ? lv_textarea_get_text(latitude) : "";
    const char *lon_text = longitude ? lv_textarea_get_text(longitude) : "";
    int32_t lat_e7 = 0;
    int32_t lon_e7 = 0;
    if (!parse_coord_text_e7(lat_text, D1L_MAP_LOCATION_LAT_E7_MIN,
                             D1L_MAP_LOCATION_LAT_E7_MAX, &lat_e7)) {
        show_toast_text("Latitude must be -90 to 90", false);
        return;
    }
    if (!parse_coord_text_e7(lon_text, D1L_MAP_LOCATION_LON_E7_MIN,
                             D1L_MAP_LOCATION_LON_E7_MAX, &lon_e7)) {
        show_toast_text("Longitude must be -180 to 180", false);
        return;
    }
    s_map_location_lat_e7 = lat_e7;
    s_map_location_lon_e7 = lon_e7;
    esp_err_t ret = d1l_app_model_set_map_location(s_map_location_lat_e7,
                                                   s_map_location_lon_e7);
    if (ret == ESP_OK) {
        s_map_location_returns_to_options = false;
        hide_map_location_sheet();
        hide_map_options_sheet();
        set_map_interactive_touch_authorized(true);
        request_tab_switch(D1L_UI_TAB_MAP);
        show_toast_text("Device PIN saved", true);
    } else {
        show_toast_text("Location rejected", false);
    }
}

static void map_location_clear_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_LOCATION)) {
        return;
    }
    esp_err_t ret = d1l_app_model_clear_map_location();
    if (ret == ESP_OK) {
        s_map_location_returns_to_options = false;
        map_location_from_snapshot(NULL);
        hide_map_location_sheet();
        hide_map_options_sheet();
        request_content_refresh();
        show_toast_text("Device PIN cleared", true);
    } else {
        show_toast_text("Clear location failed", false);
    }
}

static bool render_map_location_sheet(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP) ||
        !d1l_release_feature_available(D1L_RELEASE_FEATURE_LOCATION)) {
        return false;
    }
    return d1l_ui_map_sheets_render_location(
        &s_map_sheets_controller, &s_snapshot, s_map_location_lat_e7,
        s_map_location_lon_e7, &callbacks);
}

static void open_map_location_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP) ||
        !d1l_release_feature_available(D1L_RELEASE_FEATURE_LOCATION)) {
        return;
    }
    s_map_location_returns_to_options = false;
    d1l_app_model_snapshot(&s_snapshot);
    map_location_from_snapshot(&s_snapshot);
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_message_detail_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_storage_sheet();
    hide_wifi_sheet();
    hide_ble_sheet();
    hide_display_sheet();
    hide_diagnostics_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    if (render_map_location_sheet()) {
        show_modal(d1l_ui_map_location_sheet(&s_map_sheets_controller));
    } else {
        show_toast_text("Map location unavailable", false);
    }
}

static void open_map_location_from_options_event_cb(lv_event_t *event)
{
    open_map_location_sheet_event_cb(event);
    s_map_location_returns_to_options = true;
}

static bool render_map_options_sheet(d1l_ui_map_options_page_t page)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP)) {
        return false;
    }
    return d1l_ui_map_sheets_render_options(
        &s_map_sheets_controller, &s_snapshot, page, &callbacks);
}

static void close_map_options_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP)) {
        return;
    }
    hide_map_options_sheet();
    set_map_interactive_touch_authorized(true);
    request_tab_switch(D1L_UI_TAB_MAP);
}

static void open_map_cache_status_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP)) {
        return;
    }
    d1l_app_model_snapshot(&s_snapshot);
    if (!render_map_options_sheet(D1L_UI_MAP_OPTIONS_CACHE_STATUS)) {
        show_toast_text("Map options unavailable", false);
    }
}

static void back_to_map_options_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP)) {
        return;
    }
    d1l_app_model_snapshot(&s_snapshot);
    if (!render_map_options_sheet(D1L_UI_MAP_OPTIONS_ROOT)) {
        show_toast_text("Map options unavailable", false);
    }
}

static void open_map_options_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP)) {
        return;
    }
    d1l_app_model_snapshot(&s_snapshot);
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_message_detail_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_storage_sheet();
    hide_wifi_sheet();
    hide_ble_sheet();
    hide_display_sheet();
    hide_diagnostics_sheet();
    hide_map_location_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    if (render_map_options_sheet(D1L_UI_MAP_OPTIONS_ROOT)) {
        show_modal(d1l_ui_map_options_sheet(&s_map_sheets_controller));
    } else {
        show_toast_text("Map options unavailable", false);
    }
}

static void render_map(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)
{
    if (s_lock_visible || s_onboarding_visible || d1l_ui_modal_has_active() ||
        !map_interactive_touch_authorized()) {
        d1l_ui_map_viewport_suppress_next_acquire();
    }
    d1l_ui_map_render(content, snapshot, &callbacks);
}

static lv_obj_t *render_packet_filter_button(lv_obj_t *parent, const char *label, int x,
                                             d1l_ui_packet_filter_t mode)
{
    lv_obj_t *button = create_button(parent, label, x, 112, 58, 34, packet_filter_event_cb,
                                     (void *)(uintptr_t)mode);
    if (button && s_packets_controller.filter == mode) {
        lv_obj_set_style_bg_color(button, lv_color_hex(0x12362F), 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x5EEAD4), 0);
        lv_obj_set_style_border_width(button, 1, 0);
    }
    return button;
}

static void render_packets(lv_obj_t *content, const d1l_app_snapshot_t *snapshot)
{
    s_packet_row_generation = next_row_generation(s_packet_row_generation);
    s_packet_row_payload_count = 0U;
    s_route_row_generation = next_row_generation(s_route_row_generation);
    s_route_row_payload_count = 0U;

    char snr[16];
    format_snr_tenths(snr, sizeof(snr), snapshot->signal_summary.latest_snr_tenths);
    lv_obj_t *title = create_label(content, "Packets", 0xF4F7FB);
    obj_set_style_text_font_if(title, &lv_font_montserrat_24);
    obj_set_pos_if(title, 18, 16);

    lv_obj_t *terminal = create_panel(content, 18, 52, 424, 50);
    if (terminal) {
        lv_obj_set_style_pad_all(terminal, 8, 0);
        lv_obj_set_style_bg_color(terminal, lv_color_hex(0x071018), 0);
        lv_obj_set_style_border_color(terminal, lv_color_hex(0x334155), 0);
        lv_obj_t *line1 = create_label(terminal, "", 0x5EEAD4);
        label_set_fmt(line1, "live %s  rssi %d  snr %s  avg %d",
                      s_packets_controller.paused ? "paused" : "tail",
                      snapshot->signal_summary.latest_rssi_dbm,
                      snr,
                      snapshot->signal_summary.avg_rssi_dbm);
        label_set_dot_width(line1, 392);
        obj_align_if(line1, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *line2 = create_label(terminal, "", 0x8EA0AE);
        label_set_fmt(line2, "rooms %lu  repeaters %lu  samples %lu",
                      (unsigned long)snapshot->signal_summary.room_server_count,
                      (unsigned long)snapshot->signal_summary.repeater_candidate_count,
                      (unsigned long)snapshot->signal_summary.sample_count);
        label_set_dot_width(line2, 392);
        obj_align_if(line2, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    render_packet_filter_button(content, "All", 18, D1L_UI_PACKET_FILTER_ALL);
    render_packet_filter_button(content, "RX", 82, D1L_UI_PACKET_FILTER_RX);
    render_packet_filter_button(content, "TX", 146, D1L_UI_PACKET_FILTER_TX);
    render_packet_filter_button(content, "Text", 210, D1L_UI_PACKET_FILTER_TEXT);
    create_button(content, "Search", 278, 112, 74, 34, open_packet_search_event_cb, NULL);
    create_button(content, s_packets_controller.paused ? "Resume" : "Pause", 362, 112, 80, 34,
                  packet_pause_event_cb, NULL);
    if (s_packets_controller.search_text[0]) {
        lv_obj_t *search = create_label(content, "", 0xFBBF24);
        label_set_fmt(search, "find %.18s", s_packets_controller.search_text);
        lv_label_set_long_mode(search, LV_LABEL_LONG_DOT);
        lv_obj_set_width(search, 408);
        lv_obj_set_pos(search, 26, 152);
    }

    lv_obj_t *feed_title = create_label(content, "Packet Feed", 0x8EA0AE);
    obj_set_pos_if(feed_title, 26, s_packets_controller.search_text[0] ? 176 : 156);

    size_t packet_rows = refresh_packet_terminal_rows();
    int y = s_packets_controller.search_text[0] ? 204 : 184;
    lv_obj_t *feed_count = create_label(content, "", 0x8EA0AE);
    const size_t page_first = packet_rows > 0 ? s_packets_controller.skip_newest + 1U : 0;
    const size_t page_last = s_packets_controller.skip_newest + packet_rows;
    label_set_fmt(feed_count, "page %u-%u/%u%s",
                  (unsigned)page_first, (unsigned)page_last,
                  (unsigned)s_packets_controller.total_matches,
                  s_packets_controller.sd_history_page ? " SD" : "");
    lv_label_set_long_mode(feed_count, LV_LABEL_LONG_DOT);
    lv_obj_set_width(feed_count, 210);
    lv_obj_set_pos(feed_count, 218, s_packets_controller.search_text[0] ? 176 : 156);
    if (packet_rows > D1L_PACKET_LOG_CAPACITY) {
        packet_rows = D1L_PACKET_LOG_CAPACITY;
    }
    s_packet_row_payload_count = packet_rows;
    for (size_t i = 0; i < packet_rows; ++i) {
        s_packet_row_payloads[i] = s_packets_controller.rows[i];
        render_packet_row(content, y, &s_packet_row_payloads[i], i);
        y += 52;
    }
    if (packet_rows == 0 && snapshot->recent_packet_count > 0) {
        lv_obj_t *empty = create_label(content, "No packets match filter", 0x8EA0AE);
        lv_obj_set_pos(empty, 26, y + 8);
        y += 34;
    } else if (packet_rows == 0) {
        lv_obj_t *empty = create_label(content, "Packet log is empty", 0x8EA0AE);
        lv_obj_set_pos(empty, 26, y + 8);
        y += 34;
    }

    if (d1l_ui_packets_can_load_older(&s_packets_controller)) {
        create_button(content, "Load Older", 18, y + 4, 128, 40,
                      packet_load_older_event_cb, NULL);
        if (d1l_ui_packets_can_load_newer(&s_packets_controller)) {
            create_button(content, "Newer", 154, y + 4, 92, 40,
                          packet_load_newer_event_cb, NULL);
        }
        y += 54;
    } else if (d1l_ui_packets_can_load_newer(&s_packets_controller)) {
        create_button(content, "Newer", 18, y + 4, 92, 40,
                      packet_load_newer_event_cb, NULL);
        y += 54;
    }

    if (d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN)) {
        y += 10;
        lv_obj_t *roles = create_button(content, "Mesh Roles", 18, y, 130, 36,
                                        open_mesh_roles_event_cb, NULL);
        if (roles) {
            lv_obj_set_style_bg_color(roles, lv_color_hex(0x12362F), 0);
        }
        y += 48;
    }
    size_t route_rows = snapshot->recent_route_count;
    if (route_rows > D1L_APP_SNAPSHOT_ROUTE_PREVIEW) {
        route_rows = D1L_APP_SNAPSHOT_ROUTE_PREVIEW;
    }
    s_route_row_payload_count = route_rows;
    if (route_rows > 0U) {
        lv_obj_t *routes = create_label(content, "Routes", 0x8EA0AE);
        lv_obj_set_pos(routes, 26, y);
        y += 24;
        for (size_t i = 0; i < route_rows; ++i) {
            s_route_row_payloads[i] = snapshot->recent_routes[i];
            render_route_row(content, y, &s_route_row_payloads[i], i);
            y += 54;
        }
    }
}

static void radio_settings_action_handler(
    d1l_ui_radio_settings_action_t action,
    const d1l_app_radio_profile_edit_t *edit,
    void *context)
{
    (void)context;
    if (action >= D1L_UI_RADIO_SETTINGS_ACTION_FREQ_DOWN &&
        action <= D1L_UI_RADIO_SETTINGS_ACTION_RX_BOOST) {
        if (!render_radio_settings_sheet()) {
            hide_radio_settings_sheet();
        }
        return;
    }
    switch (action) {
    case D1L_UI_RADIO_SETTINGS_ACTION_DEFAULTS: {
        d1l_app_radio_profile_edit_t defaults = {0};
        d1l_app_model_default_radio_profile(&defaults);
        if (!d1l_ui_radio_settings_set_edit(
                &s_radio_settings_controller, &defaults) ||
            !render_radio_settings_sheet()) {
            hide_radio_settings_sheet();
        }
        return;
    }
    case D1L_UI_RADIO_SETTINGS_ACTION_SAVE: {
        esp_err_t ret = edit ?
            d1l_app_model_save_radio_profile(edit) : ESP_ERR_INVALID_ARG;
        if (ret != ESP_OK) {
            show_toast_text("Radio profile rejected", false);
            return;
        }
        d1l_app_model_snapshot(&s_snapshot);
        request_content_refresh();
        if (render_radio_settings_sheet()) {
            show_modal(
                d1l_ui_radio_settings_sheet(&s_radio_settings_controller));
        } else {
            hide_radio_settings_sheet();
        }
        show_toast_text("Radio saved; RF apply pending", true);
        return;
    }
    case D1L_UI_RADIO_SETTINGS_ACTION_CLOSE:
        hide_radio_settings_sheet();
        return;
    case D1L_UI_RADIO_SETTINGS_ACTION_NONE:
    default:
        return;
    }
}

static bool render_radio_settings_sheet(void)
{
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_RADIO_SETTINGS)) {
        return false;
    }
    return d1l_ui_radio_settings_render(
        &s_radio_settings_controller,
        s_snapshot.radio_applied,
        s_snapshot.radio_apply_pending,
        radio_settings_action_handler,
        NULL);
}

static void open_radio_settings_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_RADIO_SETTINGS)) {
        return;
    }
    d1l_app_model_snapshot(&s_snapshot);
    if (!radio_edit_from_snapshot(&s_snapshot)) {
        hide_radio_settings_sheet();
        return;
    }
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_dm_thread_sheet();
    hide_storage_sheet();
    hide_wifi_sheet();
    hide_ble_sheet();
    hide_display_sheet();
    hide_diagnostics_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    if (render_radio_settings_sheet()) {
        show_modal(d1l_ui_radio_settings_sheet(
            &s_radio_settings_controller));
    } else {
        hide_radio_settings_sheet();
    }
}

static void close_storage_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    hide_storage_sheet();
}

static void storage_subpage_back_event_cb(lv_event_t *event)
{
    (void)event;
    s_storage_page = D1L_STORAGE_PAGE_ROOT;
    render_storage_sheet();
    show_modal(s_storage_sheet);
}

static void open_storage_card_status_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY)) {
        return;
    }
    s_storage_page = D1L_STORAGE_PAGE_CARD_STATUS;
    render_storage_sheet();
    show_modal(s_storage_sheet);
}

static void open_storage_data_locations_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY)) {
        return;
    }
    s_storage_page = D1L_STORAGE_PAGE_DATA_LOCATIONS;
    render_storage_sheet();
    show_modal(s_storage_sheet);
}

static void render_storage_header(const char *title, lv_event_cb_t back_cb)
{
    create_button(s_storage_sheet, "Back", 12, 8, 76, 44, back_cb, NULL);
    lv_obj_t *heading = create_label(s_storage_sheet, title, 0xF4F7FB);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    lv_obj_set_width(heading, 360);
    lv_obj_set_pos(heading, 104, 13);
}

static void render_storage_safety_copy(void)
{
    lv_obj_t *panel = create_panel(s_storage_sheet, 16, 368, 448, 44);
    if (!panel) {
        return;
    }
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_t *label = create_label(
        panel, "FAT32 only - This device never formats cards.", 0xFBBF24);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

static void render_storage_detail_row(lv_obj_t *parent, int y,
                                      const char *name, const char *value,
                                      uint32_t accent)
{
    lv_obj_t *name_label = create_label(parent, name, accent);
    lv_obj_set_width(name_label, 150);
    lv_obj_set_pos(name_label, 12, y);
    lv_obj_t *value_label = create_label(parent, value, 0xE5EDF5);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(value_label, 254);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(value_label, 170, y);
}

static void render_storage_root(void)
{
    const d1l_ui_storage_hero_view_t *hero = &s_storage_view.hero;
    render_storage_header("Storage", close_storage_sheet_event_cb);
    lv_obj_t *subtitle = create_label(s_storage_sheet,
        d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY) ?
            "Card and saved-data overview" : "Retained internal storage",
        0x8EA0AE);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(subtitle, 360);
    lv_obj_set_pos(subtitle, 104, 40);

    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY)) {
        lv_obj_t *internal = create_panel(s_storage_sheet, 16, 76, 448, 120);
        if (internal) {
            lv_obj_set_style_pad_all(internal, 12, 0);
            lv_obj_t *eyebrow = create_label(
                internal, "Current storage", 0x8EA0AE);
            lv_obj_set_pos(eyebrow, 0, 0);
            lv_obj_t *state = create_label(
                internal, "Internal NVS", 0x5EEAD4);
            lv_obj_set_style_text_font(state, &lv_font_montserrat_24, 0);
            lv_obj_set_pos(state, 0, 26);
            lv_obj_t *detail = create_label(
                internal,
                "Settings, contacts, Public and direct messages, routes, and "
                "read markers use the retained Core store.",
                0xE5EDF5);
            lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
            lv_obj_set_size(detail, 424, 58);
            lv_obj_set_pos(detail, 0, 58);
        }
        lv_obj_t *recovery = create_panel(s_storage_sheet, 16, 212, 448, 92);
        if (recovery) {
            lv_obj_set_style_pad_all(recovery, 12, 0);
            lv_obj_t *title = create_label(
                recovery, "Recovery", 0x93C5FD);
            lv_obj_set_pos(title, 0, 0);
            lv_obj_t *copy = create_label(
                recovery,
                "Use the checksum-verified USB install and recovery guide. "
                "Normal upgrades preserve retained state.",
                0xE5EDF5);
            lv_label_set_long_mode(copy, LV_LABEL_LONG_WRAP);
            lv_obj_set_size(copy, 424, 56);
            lv_obj_set_pos(copy, 0, 26);
        }
        return;
    }

    lv_obj_t *hero_panel = create_panel(s_storage_sheet, 16, 64, 448, 92);
    if (hero_panel) {
        lv_obj_set_style_pad_all(hero_panel, 0, 0);
        lv_obj_t *eyebrow = create_label(hero_panel, "Current storage", 0x8EA0AE);
        lv_obj_set_pos(eyebrow, 12, 8);
        lv_obj_t *state = create_label(hero_panel, hero->state, hero->accent);
        lv_obj_set_style_text_font(state, &lv_font_montserrat_24, 0);
        lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
        lv_obj_set_width(state, 424);
        lv_obj_set_pos(state, 12, 28);
        lv_obj_t *detail = create_label(hero_panel, hero->detail, 0x8EA0AE);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
        lv_obj_set_width(detail, 424);
        lv_obj_set_pos(detail, 12, 64);
    }

    lv_obj_t *card = create_button(s_storage_sheet, "Card status",
                                    16, 168, 448, 68,
                                    open_storage_card_status_event_cb, NULL);
    style_contact_option_button(card, hero->accent,
                                s_storage_view.card_summary);

    lv_obj_t *data = create_button(s_storage_sheet, "Data locations",
                                    16, 248, 448, 68,
                                    open_storage_data_locations_event_cb, NULL);
    style_contact_option_button(data, 0x93C5FD, s_storage_view.data_summary);

    lv_obj_t *guidance = create_panel(s_storage_sheet, 16, 324, 448, 40);
    if (guidance) {
        lv_obj_set_style_pad_all(guidance, 0, 0);
        lv_obj_t *copy = create_label(guidance, hero->guidance, hero->accent);
        lv_label_set_long_mode(copy, LV_LABEL_LONG_DOT);
        lv_obj_set_width(copy, 424);
        lv_obj_set_pos(copy, 12, 10);
    }
}

static void render_storage_card_status(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY)) {
        return;
    }
    const d1l_ui_storage_card_view_t *card = &s_storage_view.card;

    render_storage_header("Card status", storage_subpage_back_event_cb);
    lv_obj_t *subtitle = create_label(s_storage_sheet,
                                      "Read-only card details", 0x8EA0AE);
    lv_obj_set_pos(subtitle, 104, 40);

    lv_obj_t *panel = create_panel(s_storage_sheet, 16, 68, 448, 288);
    if (!panel) {
        return;
    }
    lv_obj_set_style_pad_all(panel, 0, 0);
    render_storage_detail_row(panel, 18, "State", card->state,
                              card->state_accent);
    render_storage_detail_row(panel, 70, "Filesystem",
                              card->filesystem, 0x93C5FD);
    render_storage_detail_row(panel, 122, "Capacity", card->capacity, 0xC4B5FD);
    render_storage_detail_row(panel, 174, "Free space", card->free_space, 0xC4B5FD);
    render_storage_detail_row(panel, 226, "Readiness", card->readiness,
                              card->readiness_accent);
}

static void render_storage_location_row(lv_obj_t *parent, int y,
                                        const char *name, const char *location_text,
                                        uint32_t accent)
{
    lv_obj_t *row = create_panel(parent, 0, y, 424, 48);
    if (!row) {
        return;
    }
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_t *name_label = create_label(row, name, 0xF4F7FB);
    lv_obj_set_width(name_label, 180);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *location = create_label(row, location_text, accent);
    lv_label_set_long_mode(location, LV_LABEL_LONG_DOT);
    lv_obj_set_width(location, 220);
    lv_obj_set_style_text_align(location, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(location, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void render_storage_data_locations(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY)) {
        return;
    }
    render_storage_header("Data locations", storage_subpage_back_event_cb);
    lv_obj_t *subtitle = create_label(s_storage_sheet,
                                      "Where saved data is kept", 0x8EA0AE);
    lv_obj_set_pos(subtitle, 104, 40);

    lv_obj_t *list = create_object(s_storage_sheet, "storage locations list");
    if (!list) {
        return;
    }
    lv_obj_set_size(list, 448, 288);
    lv_obj_set_pos(list, 16, 68);
    lv_obj_set_style_radius(list, 8, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0B121A), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(0x263241), 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    size_t visible_index = 0U;
    for (size_t index = 0U; index < s_storage_view.location_count; ++index) {
        const d1l_ui_storage_location_view_t *location =
            &s_storage_view.locations[index];
        if (!d1l_ui_storage_location_available(location->location)) {
            continue;
        }
        render_storage_location_row(list, (int)(visible_index * 56U), location->name,
                                    location->value, location->accent);
        visible_index++;
    }
}

static void render_storage_sheet(void)
{
    if (!s_storage_sheet ||
        !d1l_release_feature_available(D1L_RELEASE_FEATURE_RETAINED_NVS)) {
        return;
    }
    d1l_app_model_snapshot(&s_snapshot);
    d1l_ui_storage_view_input_t input = {0};
    storage_view_input_from_snapshot(&s_snapshot, &input);
    if (!d1l_ui_storage_view(&input, &s_storage_view)) {
        return;
    }
    lv_obj_clean(s_storage_sheet);

    switch (s_storage_page) {
    case D1L_STORAGE_PAGE_CARD_STATUS:
        render_storage_card_status();
        break;
    case D1L_STORAGE_PAGE_DATA_LOCATIONS:
        render_storage_data_locations();
        break;
    case D1L_STORAGE_PAGE_ROOT:
    default:
        s_storage_page = D1L_STORAGE_PAGE_ROOT;
        render_storage_root();
        break;
    }
    if (d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY)) {
        render_storage_safety_copy();
    }
}

static void open_storage_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_RETAINED_NVS)) {
        return;
    }
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_wifi_sheet();
    hide_ble_sheet();
    hide_display_sheet();
    hide_diagnostics_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    s_storage_page = D1L_STORAGE_PAGE_ROOT;
    render_storage_sheet();
    if (s_storage_sheet) {
        show_modal(s_storage_sheet);
    }
}

static void wifi_refresh_sheet(void)
{
    d1l_app_model_snapshot(&s_snapshot);
    if (!render_wifi_sheet()) {
        hide_wifi_sheet();
    }
}

static void wifi_action_handler(d1l_ui_wifi_action_t action,
                                const char *ssid,
                                const char *password,
                                void *context)
{
    (void)context;
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL)) {
        return;
    }
    esp_err_t ret = ESP_OK;
    switch (action) {
    case D1L_UI_WIFI_ACTION_CLOSE:
        hide_wifi_sheet();
        return;
    case D1L_UI_WIFI_ACTION_SAVE:
        ret = d1l_app_model_save_wifi_profile(
            ssid ? ssid : "",
            password && password[0] != '\0' ? password : NULL);
        show_toast("Wi-Fi profile", ret);
        if (ret != ESP_OK) {
            return;
        }
        wifi_refresh_sheet();
        request_content_refresh();
        return;
    case D1L_UI_WIFI_ACTION_CLEAR:
        ret = s_snapshot.wifi_profile_count > 0U ?
            d1l_app_model_delete_wifi_profile(
                s_snapshot.wifi_active_profile) :
            ESP_ERR_NOT_FOUND;
        show_toast("Wi-Fi delete", ret);
        if (ret != ESP_OK) {
            return;
        }
        wifi_refresh_sheet();
        request_content_refresh();
        return;
    case D1L_UI_WIFI_ACTION_SCAN:
        ret = d1l_app_model_wifi_scan(&s_wifi_scan_result);
        s_wifi_scan_loaded = true;
        show_toast("Wi-Fi scan", ret);
        wifi_refresh_sheet();
        request_content_refresh();
        return;
    case D1L_UI_WIFI_ACTION_CONNECT:
        ret = d1l_app_model_wifi_connect();
        show_toast("Wi-Fi connect", ret);
        wifi_refresh_sheet();
        request_content_refresh();
        return;
    case D1L_UI_WIFI_ACTION_NEXT_PROFILE:
        ret = s_snapshot.wifi_profile_count > 1U ?
            d1l_app_model_select_wifi_profile(
                (uint8_t)(
                    (s_snapshot.wifi_active_profile + 1U) %
                    s_snapshot.wifi_profile_count)) :
            ESP_ERR_NOT_FOUND;
        show_toast("Wi-Fi profile", ret);
        if (ret != ESP_OK) {
            return;
        }
        wifi_refresh_sheet();
        request_content_refresh();
        return;
    case D1L_UI_WIFI_ACTION_TOGGLE:
        ret = d1l_app_model_set_wifi_enabled(!s_snapshot.wifi_enabled);
        show_toast("Wi-Fi", ret);
        if (ret != ESP_OK) {
            return;
        }
        wifi_refresh_sheet();
        request_content_refresh();
        return;
    case D1L_UI_WIFI_ACTION_NONE:
    default:
        return;
    }
}

static void map_location_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        map_location_save_event_cb(event);
    } else if (code == LV_EVENT_CANCEL) {
        lv_obj_t *keyboard =
            d1l_ui_map_location_keyboard(&s_map_sheets_controller);
        d1l_ui_keyboard_clear_textarea(keyboard);
        if (keyboard) {
            lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ble_action_handler(d1l_ui_ble_action_t action, void *context)
{
    (void)context;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE)) {
        return;
    }
    switch (action) {
    case D1L_UI_BLE_ACTION_CLOSE:
        hide_ble_sheet();
        return;
    case D1L_UI_BLE_ACTION_TOGGLE: {
        esp_err_t ret = d1l_app_model_set_ble_enabled(
            !s_snapshot.ble_companion_enabled);
        show_toast("BLE", ret);
        if (ret != ESP_OK) {
            return;
        }
        d1l_app_model_snapshot(&s_snapshot);
        if (!render_ble_sheet()) {
            hide_ble_sheet();
        }
        request_content_refresh();
        return;
    }
    case D1L_UI_BLE_ACTION_PAIR: {
        const esp_err_t ret = d1l_app_model_ble_begin_pairing();
        show_toast("BLE Pair", ret);
        d1l_app_model_snapshot(&s_snapshot);
        if (!render_ble_sheet()) {
            hide_ble_sheet();
        }
        request_content_refresh();
        return;
    }
    case D1L_UI_BLE_ACTION_FORGET: {
        const esp_err_t ret = d1l_app_model_ble_forget_peer();
        show_toast("BLE Forget", ret);
        d1l_app_model_snapshot(&s_snapshot);
        if (!render_ble_sheet()) {
            hide_ble_sheet();
        }
        request_content_refresh();
        return;
    }
    case D1L_UI_BLE_ACTION_NONE:
    default:
        return;
    }
}

static bool render_wifi_sheet(void)
{
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL)) {
        return false;
    }
    if (!d1l_ui_wifi_sheet(&s_wifi_controller)) {
        return false;
    }
    const char *strongest_ssid = s_wifi_scan_result.returned_count > 0U ?
        s_wifi_scan_result.aps[0].ssid : NULL;
    const d1l_ui_wifi_view_input_t view_input = {
        .build_enabled = s_snapshot.wifi_build_enabled,
        .enabled = s_snapshot.wifi_enabled,
        .connected = s_snapshot.wifi_connected,
        .profile_saved = s_snapshot.wifi_profile_saved,
        .password_saved = s_snapshot.wifi_password_saved,
        .profile_count = s_snapshot.wifi_profile_count,
        .active_profile = s_snapshot.wifi_active_profile,
        .scan_loaded = s_wifi_scan_loaded,
        .state = s_snapshot.wifi_state,
        .ip = s_snapshot.wifi_ip,
        .last_error = s_snapshot.wifi_last_error,
        .ssid = s_snapshot.wifi_ssid,
        .scan_reason = s_wifi_scan_result.reason,
        .strongest_ssid = strongest_ssid,
        .rssi_dbm = s_snapshot.wifi_rssi_dbm,
        .channel = s_snapshot.wifi_channel,
        .scan_returned_count = s_wifi_scan_result.returned_count,
        .scan_total_count = s_wifi_scan_result.total_count,
    };
    d1l_ui_wifi_view_model_t view = {0};
    d1l_ui_connectivity_wifi_view(&view_input, &view);
    if (!d1l_ui_wifi_render(&s_wifi_controller, &view,
                            wifi_action_handler, NULL)) {
        ESP_LOGE(TAG, "Wi-Fi sheet render rejected invalid state");
        return false;
    }
    return true;
}
static bool render_ble_sheet(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE)) {
        return false;
    }
    if (!d1l_ui_ble_sheet(&s_ble_controller)) {
        return false;
    }
    const d1l_ui_ble_view_input_t view_input = {
        .build_enabled = s_snapshot.ble_build_enabled,
        .transport_supported = s_snapshot.ble_transport_supported,
        .companion_enabled = s_snapshot.ble_companion_enabled,
        .peer_known = s_snapshot.ble_peer_known,
        .protocol_running = s_snapshot.ble_protocol_running,
        .protocol_ready = s_snapshot.ble_protocol_ready,
        .pairing_passkey = s_snapshot.ble_pairing_passkey,
        .state = s_snapshot.ble_state,
    };
    d1l_ui_ble_view_model_t view = {0};
    d1l_ui_connectivity_ble_view(&view_input, &view);
    if (!d1l_ui_ble_render(&s_ble_controller, &view,
                           ble_action_handler, NULL)) {
        ESP_LOGE(TAG, "BLE sheet render rejected invalid state");
        return false;
    }
    return true;
}

static void device_sheets_action_handler(
    d1l_ui_device_sheets_action_t action,
    void *context)
{
    (void)context;
    d1l_settings_t settings = {0};
    d1l_display_preferences_t preferences = {0};
    switch (action) {
    case D1L_UI_DEVICE_SHEETS_ACTION_CLOSE_DISPLAY:
        hide_display_sheet();
        return;
    case D1L_UI_DEVICE_SHEETS_ACTION_BRIGHTNESS: {
        d1l_display_preferences_get(&preferences);
        static const uint8_t levels[] = {25U, 50U, 70U, 85U, 100U};
        uint8_t next = levels[0];
        for (size_t i = 0U; i < sizeof(levels) / sizeof(levels[0]); ++i) {
            if (preferences.brightness_percent < levels[i]) {
                next = levels[i];
                break;
            }
            next = levels[(i + 1U) %
                          (sizeof(levels) / sizeof(levels[0]))];
        }
        esp_err_t ret = d1l_display_preferences_set_brightness(next);
        if (ret == ESP_OK) {
            (void)d1l_settings_public_snapshot(&settings);
            ret = d1l_backlight_set_percent(
                settings.night_mode ? 25 : next);
        }
        show_toast("Brightness", ret);
        d1l_app_model_snapshot(&s_snapshot);
        (void)render_display_sheet();
        return;
    }
    case D1L_UI_DEVICE_SHEETS_ACTION_NIGHT_MODE: {
        (void)d1l_settings_public_snapshot(&settings);
        settings.night_mode = !settings.night_mode;
        esp_err_t ret = d1l_settings_update_fields(
            &settings, D1L_SETTINGS_UPDATE_NIGHT_MODE);
        if (ret == ESP_OK) {
            d1l_display_preferences_get(&preferences);
            ret = d1l_backlight_set_percent(
                settings.night_mode ? 25 : preferences.brightness_percent);
        }
        show_toast("Night mode", ret);
        d1l_app_model_snapshot(&s_snapshot);
        (void)render_display_sheet();
        request_full_screen_repaint();
        return;
    }
    case D1L_UI_DEVICE_SHEETS_ACTION_HIGH_CONTRAST: {
        (void)d1l_settings_public_snapshot(&settings);
        settings.high_contrast = !settings.high_contrast;
        const esp_err_t ret = d1l_settings_update_fields(
            &settings, D1L_SETTINGS_UPDATE_HIGH_CONTRAST);
        show_toast("High contrast", ret);
        d1l_app_model_snapshot(&s_snapshot);
        if (ret == ESP_OK && s_screen) {
            lv_obj_set_style_bg_color(
                s_screen,
                lv_color_hex(settings.high_contrast ? 0x000000 : 0x071018),
                0);
        }
        (void)render_display_sheet();
        request_full_screen_repaint();
        return;
    }
    case D1L_UI_DEVICE_SHEETS_ACTION_TIMEOUT: {
        d1l_display_preferences_get(&preferences);
        const uint16_t next =
            preferences.timeout_seconds == 0U ? 30U :
            preferences.timeout_seconds == 30U ? 60U :
            preferences.timeout_seconds == 60U ? 120U :
            preferences.timeout_seconds == 120U ? 300U : 0U;
        const esp_err_t ret = d1l_display_preferences_set_timeout(next);
        show_toast("Display timeout", ret);
        d1l_app_model_snapshot(&s_snapshot);
        (void)render_display_sheet();
        return;
    }
    case D1L_UI_DEVICE_SHEETS_ACTION_CLOSE_DIAGNOSTICS:
        hide_diagnostics_sheet();
        return;
    case D1L_UI_DEVICE_SHEETS_ACTION_OPEN_TERMINAL:
        hide_diagnostics_sheet();
        open_terminal_sheet_event_cb(NULL);
        return;
    case D1L_UI_DEVICE_SHEETS_ACTION_NONE:
    default:
        return;
    }
}

static bool render_display_sheet(void)
{
    return d1l_ui_device_sheets_render_display(
        &s_device_sheets_controller, &s_snapshot,
        device_sheets_action_handler, NULL);
}

static bool render_diagnostics_sheet(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_DIAGNOSTICS)) {
        return false;
    }
    d1l_app_model_snapshot(&s_snapshot);
    return d1l_ui_device_sheets_render_diagnostics(
        &s_device_sheets_controller, &s_snapshot,
        device_sheets_action_handler, NULL);
}

static bool confirmation_active(bool armed, uint32_t deadline)
{
    return armed && (int32_t)(deadline - lv_tick_get()) > 0;
}

static bool render_terminal_service_sheet(void)
{
    d1l_ui_terminal_sheet_input_t input = {
        .status = d1l_event_log_status(),
        .clear_armed = confirmation_active(
            s_terminal_clear_armed, s_terminal_clear_deadline),
    };
    if (!input.clear_armed) {
        s_terminal_clear_armed = false;
    }
    input.entry_count = d1l_event_log_copy_recent(
        input.entries, D1L_UI_TERMINAL_PREVIEW_COUNT);
    return d1l_ui_service_sheets_render_terminal(
        &s_service_sheets_controller, &input,
        service_sheets_action_handler, NULL);
}

static bool render_observer_service_sheet(void)
{
    d1l_observer_status_t status = {0};
    d1l_observer_status(&status);
    return d1l_ui_service_sheets_render_observer(
        &s_service_sheets_controller, &status,
        service_sheets_action_handler, NULL);
}

static bool render_update_service_sheet(void)
{
    d1l_update_status_t status = {0};
    d1l_update_status(&status);
    const bool install_armed = confirmation_active(
        s_update_install_armed, s_update_install_deadline);
    const bool reboot_armed = confirmation_active(
        s_update_reboot_armed, s_update_reboot_deadline);
    if (!install_armed) {
        s_update_install_armed = false;
    }
    if (!reboot_armed) {
        s_update_reboot_armed = false;
    }
    return d1l_ui_service_sheets_render_update(
        &s_service_sheets_controller, &status,
        install_armed, reboot_armed,
        service_sheets_action_handler, NULL);
}

static bool render_notifications_service_sheet(void)
{
    d1l_display_preferences_t preferences = {0};
    d1l_display_preferences_get(&preferences);
    d1l_ui_notifications_sheet_input_t input = {
        .public_unread = s_snapshot.public_unread_count,
        .dm_unread = s_snapshot.dm_unread_count,
        .muted_unread = s_snapshot.muted_dm_unread_count,
        .mode = preferences.notification_mode,
    };
    return d1l_ui_service_sheets_render_notifications(
        &s_service_sheets_controller, &input,
        service_sheets_action_handler, NULL);
}

static void build_admin_room_transcript(
    const d1l_meshcore_admin_snapshot_t *status)
{
    s_admin_room_transcript[0] = '\0';
    memset(s_admin_room_preview, 0, sizeof(s_admin_room_preview));
    if (!status ||
        status->state != D1L_MESHCORE_ADMIN_AUTHENTICATED ||
        status->role != D1L_MESHCORE_ADMIN_ROLE_ROOM ||
        status->fingerprint[0] == '\0') {
        return;
    }

    const size_t count = d1l_dm_store_copy_thread(
        status->fingerprint, s_admin_room_preview,
        D1L_UI_ADMIN_ROOM_PREVIEW_COUNT);
    if (count == 0U) {
        snprintf(
            s_admin_room_transcript, sizeof(s_admin_room_transcript),
            "No room posts received yet.");
        return;
    }

    size_t used = 0U;
    for (size_t i = 0U; i < count; ++i) {
        const d1l_dm_entry_t *entry = &s_admin_room_preview[i];
        const bool outbound = strcmp(entry->direction, "tx") == 0;
        const char *delivery = outbound ?
            (entry->acked ? " [delivered]" : " [pending]") : "";
        const int written = snprintf(
            &s_admin_room_transcript[used],
            sizeof(s_admin_room_transcript) - used,
            "%s%s%s%s",
            i == 0U ? "" : "\n",
            outbound ? "You: " : "",
            entry->text,
            delivery);
        if (written < 0) {
            s_admin_room_transcript[0] = '\0';
            return;
        }
        if ((size_t)written >=
            sizeof(s_admin_room_transcript) - used) {
            used = sizeof(s_admin_room_transcript) - 1U;
            break;
        }
        used += (size_t)written;
    }
    s_admin_room_transcript[used] = '\0';
}

static bool render_admin_service_sheet(void)
{
    d1l_meshcore_admin_snapshot_t status = {0};
    d1l_meshcore_service_admin_snapshot(&status);
    if (status.state != D1L_MESHCORE_ADMIN_AUTHENTICATED ||
        !confirmation_active(
            s_admin_mutation_armed != D1L_MESHCORE_ADMIN_MUTATION_NONE,
            s_admin_mutation_deadline)) {
        s_admin_mutation_armed = D1L_MESHCORE_ADMIN_MUTATION_NONE;
    }
    if (status.state != D1L_MESHCORE_ADMIN_AUTHENTICATED ||
        !confirmation_active(
            s_admin_cli_armed[0] != '\0', s_admin_cli_deadline)) {
        clear_admin_cli_confirmation();
    }
    build_admin_room_transcript(&status);
    const d1l_dm_store_stats_t dm_stats = d1l_dm_store_stats();
    const bool complete = d1l_ui_service_sheets_render_admin(
        &s_service_sheets_controller, &status,
        s_admin_target_fingerprint,
        s_admin_mutation_armed,
        s_admin_cli_armed[0] != '\0' &&
            s_admin_cli_armed_origin ==
                D1L_UI_ADMIN_CLI_ORIGIN_GENERIC,
        s_admin_cli_armed[0] != '\0' &&
            s_admin_cli_armed_origin ==
                D1L_UI_ADMIN_CLI_ORIGIN_ACL,
        s_admin_cli_armed[0] != '\0' &&
            s_admin_cli_armed_origin ==
                D1L_UI_ADMIN_CLI_ORIGIN_ROOM_READ_ONLY_ON,
        s_admin_cli_armed[0] != '\0' &&
            s_admin_cli_armed_origin ==
                D1L_UI_ADMIN_CLI_ORIGIN_ROOM_READ_ONLY_OFF,
        s_admin_cli_secure_input,
        s_admin_room_transcript,
        service_sheets_action_handler, NULL);
    if (complete) {
        s_admin_rendered_generation = status.generation;
        s_admin_rendered_dm_revision = dm_stats.content_revision;
        s_admin_rendered_generation_valid = true;
    }
    return complete;
}

static void refresh_admin_service_sheet_if_changed(void)
{
    d1l_meshcore_admin_snapshot_t status = {0};
    d1l_meshcore_service_admin_snapshot(&status);
    const bool mutation_expired =
        s_admin_mutation_armed != D1L_MESHCORE_ADMIN_MUTATION_NONE &&
        !confirmation_active(true, s_admin_mutation_deadline);
    const bool cli_expired =
        s_admin_cli_armed[0] != '\0' &&
        !confirmation_active(true, s_admin_cli_deadline);
    const d1l_dm_store_stats_t dm_stats = d1l_dm_store_stats();
    const bool room_content_changed =
        status.state == D1L_MESHCORE_ADMIN_AUTHENTICATED &&
        status.role == D1L_MESHCORE_ADMIN_ROLE_ROOM &&
        dm_stats.content_revision != s_admin_rendered_dm_revision;
    const bool edit_in_progress =
        d1l_ui_service_sheets_admin_edit_has_text(
            &s_service_sheets_controller);
    if (!s_admin_rendered_generation_valid ||
        status.generation != s_admin_rendered_generation ||
        mutation_expired || cli_expired ||
        (room_content_changed && !edit_in_progress)) {
        (void)render_admin_service_sheet();
    }
}

static void service_sheets_action_handler(
    d1l_ui_service_action_t action,
    void *context)
{
    (void)context;
    switch (action) {
    case D1L_UI_SERVICE_ACTION_CLOSE_TERMINAL:
    case D1L_UI_SERVICE_ACTION_CLOSE_OBSERVER:
    case D1L_UI_SERVICE_ACTION_CLOSE_UPDATE:
    case D1L_UI_SERVICE_ACTION_CLOSE_NOTIFICATIONS:
        hide_service_sheets();
        return;
    case D1L_UI_SERVICE_ACTION_CLOSE_ADMIN:
        s_admin_mutation_armed = D1L_MESHCORE_ADMIN_MUTATION_NONE;
        clear_admin_cli_confirmation();
        s_admin_cli_secure_input = false;
        hide_service_sheets();
        return;
    case D1L_UI_SERVICE_ACTION_TERMINAL_LEVEL: {
        const d1l_event_log_status_t status = d1l_event_log_status();
        const d1l_event_log_level_t next =
            (d1l_event_log_level_t)(
                ((unsigned)status.runtime_level + 1U) %
                ((unsigned)D1L_EVENT_LOG_LEVEL_DEBUG + 1U));
        show_toast("Terminal level",
                   d1l_event_log_set_runtime_level(next));
        (void)render_terminal_service_sheet();
        return;
    }
    case D1L_UI_SERVICE_ACTION_TERMINAL_CLEAR:
        if (!confirmation_active(
                s_terminal_clear_armed, s_terminal_clear_deadline)) {
            s_terminal_clear_armed = true;
            s_terminal_clear_deadline = lv_tick_get() + 5000U;
            show_toast_text("Tap Confirm Clear within 5 seconds", true);
        } else {
            s_terminal_clear_armed = false;
            show_toast("Clear logs", d1l_event_log_clear());
        }
        (void)render_terminal_service_sheet();
        return;
    case D1L_UI_SERVICE_ACTION_OBSERVER_TOGGLE: {
        d1l_observer_status_t status = {0};
        d1l_observer_status(&status);
        show_toast(status.enabled ? "Observer off" : "Observer on",
                   d1l_observer_set_enabled(!status.enabled));
        (void)render_observer_service_sheet();
        request_content_refresh();
        return;
    }
    case D1L_UI_SERVICE_ACTION_UPDATE_INSTALL:
        if (!confirmation_active(
                s_update_install_armed, s_update_install_deadline)) {
            s_update_install_armed = true;
            s_update_install_deadline = lv_tick_get() + 5000U;
            show_toast_text("Tap Confirm Install within 5 seconds", true);
        } else {
            s_update_install_armed = false;
            show_toast("Signed update",
                       d1l_update_request_install());
        }
        (void)render_update_service_sheet();
        return;
    case D1L_UI_SERVICE_ACTION_UPDATE_CANCEL:
        show_toast("Update cancel", d1l_update_cancel());
        (void)render_update_service_sheet();
        return;
    case D1L_UI_SERVICE_ACTION_UPDATE_REBOOT:
        if (!confirmation_active(
                s_update_reboot_armed, s_update_reboot_deadline)) {
            s_update_reboot_armed = true;
            s_update_reboot_deadline = lv_tick_get() + 5000U;
            show_toast_text("Tap Confirm Reboot within 5 seconds", true);
            (void)render_update_service_sheet();
            return;
        }
        s_update_reboot_armed = false;
        show_toast("Update reboot",
                   d1l_update_reboot_to_installed());
        (void)render_update_service_sheet();
        return;
    case D1L_UI_SERVICE_ACTION_NOTIFICATIONS_MODE: {
        d1l_display_preferences_t preferences = {0};
        d1l_display_preferences_get(&preferences);
        const d1l_notification_mode_t next =
            (d1l_notification_mode_t)(
                ((unsigned)preferences.notification_mode + 1U) %
                ((unsigned)D1L_NOTIFICATION_MODE_QUIET_HOURS + 1U));
        show_toast("Notification mode",
                   d1l_display_preferences_set_notification_mode(next));
        d1l_app_model_snapshot(&s_snapshot);
        (void)render_notifications_service_sheet();
        request_content_refresh();
        return;
    }
    case D1L_UI_SERVICE_ACTION_OPEN_MESSAGES:
        hide_service_sheets();
        s_messages_mode = D1L_UI_MESSAGES_MODE_ROOT;
        request_tab_switch(D1L_UI_TAB_MESSAGES);
        return;
    case D1L_UI_SERVICE_ACTION_ADMIN_LOGIN: {
        char password[D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U] = {0};
        clear_admin_cli_confirmation();
        s_admin_cli_secure_input = false;
        if (s_admin_target_fingerprint[0] == '\0') {
            show_toast_text("Select a repeater or room first", true);
            return;
        }
        if (!d1l_ui_service_sheets_take_admin_password(
                &s_service_sheets_controller, password, sizeof(password))) {
            show_toast("Admin login", ESP_ERR_INVALID_STATE);
            return;
        }
        const esp_err_t result = d1l_meshcore_service_admin_login(
            s_admin_target_fingerprint, password);
        d1l_meshcore_admin_secure_zero(password, sizeof(password));
        show_toast("Admin login", result);
        (void)render_admin_service_sheet();
        return;
    }
    case D1L_UI_SERVICE_ACTION_ADMIN_REFRESH:
        s_admin_mutation_armed = D1L_MESHCORE_ADMIN_MUTATION_NONE;
        clear_admin_cli_confirmation();
        show_toast("Admin status",
                   d1l_meshcore_service_admin_request_status());
        (void)render_admin_service_sheet();
        return;
    case D1L_UI_SERVICE_ACTION_ADMIN_TELEMETRY:
    case D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS:
    case D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS_NEXT:
    case D1L_UI_SERVICE_ACTION_ADMIN_ACCESS_LIST: {
        s_admin_mutation_armed = D1L_MESHCORE_ADMIN_MUTATION_NONE;
        clear_admin_cli_confirmation();
        d1l_meshcore_admin_query_t query =
            D1L_MESHCORE_ADMIN_QUERY_TELEMETRY;
        uint16_t offset = 0U;
        const char *label = "Server telemetry";
        if (action == D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS ||
            action == D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS_NEXT) {
            query = D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS;
            label = "Repeater neighbours";
        } else if (action ==
                   D1L_UI_SERVICE_ACTION_ADMIN_ACCESS_LIST) {
            query = D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST;
            label = "Server access list";
        }
        if (action == D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS_NEXT) {
            d1l_meshcore_admin_snapshot_t status = {0};
            d1l_meshcore_service_admin_snapshot(&status);
            const uint32_t next_offset =
                (uint32_t)status.query_result.offset +
                (uint32_t)status.query_result.count;
            if (!status.query_result.valid ||
                status.query_result.kind !=
                    D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS ||
                status.query_result.count == 0U ||
                next_offset >= status.query_result.total ||
                next_offset > UINT16_MAX) {
                show_toast_text("No next neighbour page", true);
                (void)render_admin_service_sheet();
                return;
            }
            offset = (uint16_t)next_offset;
        }
        show_toast(label,
                   d1l_meshcore_service_admin_request_query(query, offset));
        (void)render_admin_service_sheet();
        return;
    }
    case D1L_UI_SERVICE_ACTION_ADMIN_CLEAR_STATS:
    case D1L_UI_SERVICE_ACTION_ADMIN_ADVERTISE_ZERO_HOP: {
        clear_admin_cli_confirmation();
        const d1l_meshcore_admin_mutation_t mutation =
            action == D1L_UI_SERVICE_ACTION_ADMIN_CLEAR_STATS ?
                D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS :
                D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP;
        if (s_admin_mutation_armed != mutation ||
            !confirmation_active(true, s_admin_mutation_deadline)) {
            s_admin_mutation_armed = mutation;
            s_admin_mutation_deadline =
                lv_tick_get() + D1L_UI_ADMIN_MUTATION_CONFIRM_WINDOW_MS;
            show_toast_text(
                mutation == D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS ?
                    "Tap Confirm Clear Stats within 5 seconds" :
                    "Tap Confirm Zero-Hop within 5 seconds",
                true);
        } else {
            s_admin_mutation_armed = D1L_MESHCORE_ADMIN_MUTATION_NONE;
            show_toast(
                mutation == D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS ?
                    "Clear remote stats" : "Send zero-hop advert",
                d1l_meshcore_service_admin_request_mutation(
                    mutation, true));
        }
        (void)render_admin_service_sheet();
        return;
    }
    case D1L_UI_SERVICE_ACTION_ADMIN_ROOM_SEND: {
        char text[D1L_USER_TEXT_MAX_BYTES + 1U] = {0};
        if (!d1l_ui_service_sheets_take_admin_room_post(
                &s_service_sheets_controller, text, sizeof(text))) {
            show_toast_text("Enter one valid room message", true);
            (void)render_admin_service_sheet();
            return;
        }
        const esp_err_t result =
            d1l_meshcore_service_admin_send_room_post(text);
        d1l_meshcore_admin_secure_zero(text, sizeof(text));
        show_toast("Room post", result);
        (void)render_admin_service_sheet();
        return;
    }
    case D1L_UI_SERVICE_ACTION_ADMIN_ACL_APPLY: {
        char command[D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U] = {0};
        if (s_admin_cli_armed_origin == D1L_UI_ADMIN_CLI_ORIGIN_ACL &&
            s_admin_cli_armed[0] != '\0' &&
            confirmation_active(true, s_admin_cli_deadline)) {
            snprintf(command, sizeof(command), "%s", s_admin_cli_armed);
            clear_admin_cli_confirmation();
            const esp_err_t result =
                d1l_meshcore_service_admin_request_cli(command, true);
            d1l_meshcore_admin_secure_zero(command, sizeof(command));
            show_toast("ACL change", result);
            (void)render_admin_service_sheet();
            return;
        }
        char edit[D1L_MESHCORE_ADMIN_ACL_EDIT_MAX_BYTES + 1U] = {0};
        char public_key_hex[
            (D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U) + 1U] = {0};
        clear_admin_cli_confirmation();
        const bool edit_valid =
            d1l_ui_service_sheets_take_admin_acl(
                &s_service_sheets_controller, edit, sizeof(edit)) &&
            strlen(edit) == D1L_MESHCORE_ADMIN_ACL_EDIT_MAX_BYTES &&
            edit[D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U] == ' ' &&
            edit[(D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U) + 1U] >= '0' &&
            edit[(D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U) + 1U] <= '3';
        if (edit_valid) {
            memcpy(
                public_key_hex, edit,
                D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U);
            const uint8_t permissions = (uint8_t)(
                edit[(D1L_MESHCORE_ADMIN_PUBLIC_KEY_BYTES * 2U) + 1U] -
                '0');
            if (!d1l_meshcore_admin_format_acl_command(
                    public_key_hex, permissions,
                    command, sizeof(command)) ||
                !admin_cli_allowed_for_current_session(command)) {
                command[0] = '\0';
            }
        }
        d1l_meshcore_admin_secure_zero(edit, sizeof(edit));
        d1l_meshcore_admin_secure_zero(
            public_key_hex, sizeof(public_key_hex));
        if (command[0] == '\0') {
            d1l_meshcore_admin_secure_zero(command, sizeof(command));
            show_toast_text(
                "Use a full 64-hex public key and permission 0, 1, 2 or 3",
                true);
            (void)render_admin_service_sheet();
            return;
        }
        arm_admin_cli_command(command, D1L_UI_ADMIN_CLI_ORIGIN_ACL);
        d1l_meshcore_admin_secure_zero(command, sizeof(command));
        show_toast_text("Tap Confirm ACL within 5 seconds", true);
        (void)render_admin_service_sheet();
        return;
    }
    case D1L_UI_SERVICE_ACTION_ADMIN_ROOM_READ_ONLY_ON:
    case D1L_UI_SERVICE_ACTION_ADMIN_ROOM_READ_ONLY_OFF: {
        const bool enable =
            action == D1L_UI_SERVICE_ACTION_ADMIN_ROOM_READ_ONLY_ON;
        const d1l_ui_admin_cli_origin_t origin = enable ?
            D1L_UI_ADMIN_CLI_ORIGIN_ROOM_READ_ONLY_ON :
            D1L_UI_ADMIN_CLI_ORIGIN_ROOM_READ_ONLY_OFF;
        const char *command = enable ?
            "set allow.read.only on" : "set allow.read.only off";
        if (s_admin_cli_armed_origin == origin &&
            s_admin_cli_armed[0] != '\0' &&
            confirmation_active(true, s_admin_cli_deadline)) {
            char confirmed[
                D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U] = {0};
            snprintf(confirmed, sizeof(confirmed), "%s", s_admin_cli_armed);
            clear_admin_cli_confirmation();
            const esp_err_t result =
                d1l_meshcore_service_admin_request_cli(confirmed, true);
            d1l_meshcore_admin_secure_zero(confirmed, sizeof(confirmed));
            show_toast("Room guest access", result);
        } else if (!admin_cli_allowed_for_current_session(command)) {
            clear_admin_cli_confirmation();
            show_toast_text(
                "Room admin permission and compatible firmware required",
                true);
        } else {
            arm_admin_cli_command(command, origin);
            show_toast_text(
                enable ?
                    "Tap Confirm Guest On within 5 seconds" :
                    "Tap Confirm Guest Off within 5 seconds",
                true);
        }
        (void)render_admin_service_sheet();
        return;
    }
    case D1L_UI_SERVICE_ACTION_ADMIN_CLI_SECURE_TOGGLE:
        clear_admin_cli_confirmation();
        s_admin_cli_secure_input = !s_admin_cli_secure_input;
        show_toast_text(
            s_admin_cli_secure_input ?
                "Secure masked command input enabled" :
                "Visible command input enabled",
            false);
        (void)render_admin_service_sheet();
        return;
    case D1L_UI_SERVICE_ACTION_ADMIN_CLI_SEND: {
        char command[D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U] = {0};
        if (s_admin_cli_armed_origin ==
                D1L_UI_ADMIN_CLI_ORIGIN_GENERIC &&
            s_admin_cli_armed[0] != '\0' &&
            confirmation_active(true, s_admin_cli_deadline)) {
            snprintf(command, sizeof(command), "%s", s_admin_cli_armed);
            clear_admin_cli_confirmation();
            const esp_err_t result =
                d1l_meshcore_service_admin_request_cli(command, true);
            d1l_meshcore_admin_secure_zero(command, sizeof(command));
            show_toast("Server command", result);
            (void)render_admin_service_sheet();
            return;
        }
        clear_admin_cli_confirmation();
        if (!d1l_ui_service_sheets_take_admin_cli(
                &s_service_sheets_controller, command, sizeof(command)) ||
            !d1l_meshcore_admin_cli_command_valid(command) ||
            !admin_cli_allowed_for_current_session(command)) {
            d1l_meshcore_admin_secure_zero(command, sizeof(command));
            show_toast_text(
                "Command is unsupported for this authenticated server",
                true);
            (void)render_admin_service_sheet();
            return;
        }
        const bool sensitive =
            d1l_meshcore_admin_cli_command_sensitive(command);
        if (sensitive && !s_admin_cli_secure_input) {
            d1l_meshcore_admin_secure_zero(command, sizeof(command));
            show_toast_text(
                "Enable Secure Input before entering passwords or keys",
                true);
            (void)render_admin_service_sheet();
            return;
        }
        if (d1l_meshcore_admin_cli_command_read_only(command)) {
            const esp_err_t result =
                d1l_meshcore_service_admin_request_cli(command, false);
            d1l_meshcore_admin_secure_zero(command, sizeof(command));
            show_toast("Server command", result);
            (void)render_admin_service_sheet();
            return;
        }
        arm_admin_cli_command(
            command, D1L_UI_ADMIN_CLI_ORIGIN_GENERIC);
        d1l_meshcore_admin_secure_zero(command, sizeof(command));
        show_toast_text(
            "Tap Confirm Command within 5 seconds", true);
        (void)render_admin_service_sheet();
        return;
    }
    case D1L_UI_SERVICE_ACTION_ADMIN_LOGOUT:
        s_admin_mutation_armed = D1L_MESHCORE_ADMIN_MUTATION_NONE;
        clear_admin_cli_confirmation();
        s_admin_cli_secure_input = false;
        show_toast("Admin logout",
                   d1l_meshcore_service_admin_logout());
        (void)render_admin_service_sheet();
        return;
    case D1L_UI_SERVICE_ACTION_NONE:
    default:
        return;
    }
}

static void open_terminal_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_ui_settings_action_available(
            D1L_UI_SETTINGS_ACTION_TERMINAL)) {
        return;
    }
    hide_sheet();
    d1l_app_model_snapshot(&s_snapshot);
    if (render_terminal_service_sheet()) {
        show_modal(d1l_ui_service_sheets_terminal(
            &s_service_sheets_controller));
    }
}

static void open_observer_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_ui_settings_action_available(
            D1L_UI_SETTINGS_ACTION_OBSERVER)) {
        return;
    }
    hide_sheet();
    if (render_observer_service_sheet()) {
        show_modal(d1l_ui_service_sheets_observer(
            &s_service_sheets_controller));
    }
}

static void open_update_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_ui_settings_action_available(
            D1L_UI_SETTINGS_ACTION_UPDATE)) {
        return;
    }
    hide_sheet();
    if (render_update_service_sheet()) {
        show_modal(d1l_ui_service_sheets_update(
            &s_service_sheets_controller));
    }
}

static void open_notifications_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_ui_settings_action_available(
            D1L_UI_SETTINGS_ACTION_NOTIFICATIONS)) {
        return;
    }
    hide_sheet();
    d1l_app_model_snapshot(&s_snapshot);
    if (render_notifications_service_sheet()) {
        show_modal(d1l_ui_service_sheets_notifications(
            &s_service_sheets_controller));
    }
}

static void open_admin_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_ui_settings_action_available(
            D1L_UI_SETTINGS_ACTION_ADMIN)) {
        return;
    }
    hide_sheet();
    if (render_admin_service_sheet()) {
        show_modal(d1l_ui_service_sheets_admin(
            &s_service_sheets_controller));
    }
}

static void open_wifi_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL)) {
        return;
    }
    d1l_app_model_snapshot(&s_snapshot);
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_storage_sheet();
    hide_ble_sheet();
    hide_display_sheet();
    hide_diagnostics_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    const bool wifi_rendered = render_wifi_sheet();
    lv_obj_t *wifi_sheet = d1l_ui_wifi_sheet(&s_wifi_controller);
    if (wifi_rendered && wifi_sheet) {
        show_modal(wifi_sheet);
    } else {
        hide_wifi_sheet();
    }
}

static void open_ble_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE)) {
        return;
    }
    d1l_app_model_snapshot(&s_snapshot);
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_storage_sheet();
    hide_wifi_sheet();
    hide_display_sheet();
    hide_diagnostics_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    if (render_ble_sheet()) {
        show_modal(d1l_ui_ble_sheet(&s_ble_controller));
    } else {
        hide_ble_sheet();
    }
}

static void open_display_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    d1l_app_model_snapshot(&s_snapshot);
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_storage_sheet();
    hide_wifi_sheet();
    hide_ble_sheet();
    hide_diagnostics_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    if (render_display_sheet()) {
        show_modal(d1l_ui_device_sheets_display(&s_device_sheets_controller));
    } else {
        hide_display_sheet();
    }
}

static void open_diagnostics_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_DIAGNOSTICS)) {
        return;
    }
    d1l_app_model_snapshot(&s_snapshot);
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_storage_sheet();
    hide_wifi_sheet();
    hide_ble_sheet();
    hide_display_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    if (render_diagnostics_sheet()) {
        show_modal(
            d1l_ui_device_sheets_diagnostics(&s_device_sheets_controller));
    } else {
        hide_diagnostics_sheet();
    }
}

static void advert_zero_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_ui_settings_action_available(
            D1L_UI_SETTINGS_ACTION_ADVANCED)) {
        return;
    }
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    show_toast("Zero advert", d1l_app_model_request_advert(false));
}

static void advert_flood_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_ui_settings_action_available(
            D1L_UI_SETTINGS_ACTION_ADVANCED)) {
        return;
    }
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    show_toast("Flood advert", d1l_app_model_request_advert(true));
}

static void open_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    if (!d1l_ui_settings_action_available(
            D1L_UI_SETTINGS_ACTION_ADVANCED)) {
        return;
    }
    if (s_sheet) {
        hide_public_history_sheet();
        hide_public_search_sheet();
        hide_dm_thread_sheet();
        hide_radio_settings_sheet();
        hide_storage_sheet();
        hide_wifi_sheet();
        hide_ble_sheet();
        hide_display_sheet();
        hide_diagnostics_sheet();
        hide_map_location_sheet();
        hide_map_options_sheet();
        hide_contact_detail_sheet();
        hide_contact_export_sheet();
        hide_route_detail_sheet();
        hide_route_trace_sheet();
        hide_packet_detail_sheet();
        hide_packet_search_sheet();
        hide_mesh_roles_sheet();
        show_modal(s_sheet);
    }
}

static void close_sheet_event_cb(lv_event_t *event)
{
    (void)event;
    hide_sheet();
}

static void render_active_tab(void)
{
    static const d1l_ui_screen_renderer_t renderers[] = {
        {D1L_UI_TAB_HOME, render_home_screen},
        {D1L_UI_TAB_MESSAGES, render_messages},
        {D1L_UI_TAB_NODES, render_nodes},
        {D1L_UI_TAB_MAP, render_map},
        {D1L_UI_TAB_PACKETS, render_packets},
        {D1L_UI_TAB_SETTINGS, render_settings},
    };

    if (!s_content) {
        return;
    }
    /* Release before d1l_ui_screen_render cleans the old Map viewport. */
    d1l_ui_map_viewport_release();
    d1l_app_model_snapshot(&s_snapshot);
    update_chrome(&s_snapshot);
    layout_content_for_active_tab();
    if (d1l_ui_navigation_active() != D1L_UI_TAB_HOME) {
        d1l_ui_home_deactivate(&s_home_controller);
    }
    /* A Home preview can own the exact DM thread overlay. Ambient Home
     * refreshes must not deactivate that controller; explicit tab switches
     * hide the thread before reaching this renderer. */
    if (d1l_ui_navigation_active() != D1L_UI_TAB_MESSAGES &&
        !d1l_ui_messages_thread_active(&s_messages_controller)) {
        d1l_ui_messages_deactivate(&s_messages_controller);
    }
    if (d1l_ui_navigation_active() != D1L_UI_TAB_NODES) {
        d1l_ui_nodes_deactivate(&s_nodes_controller);
    }
    if (d1l_ui_navigation_active() != D1L_UI_TAB_SETTINGS) {
        d1l_ui_settings_deactivate(&s_settings_controller);
    }
    (void)d1l_ui_screen_render(d1l_ui_navigation_active(), &s_snapshot, s_content,
                               renderers, sizeof(renderers) / sizeof(renderers[0]));
    update_startup_overlay(&s_snapshot);
    remember_rendered_content_generation(&s_snapshot);
    remember_rendered_map_input(&s_snapshot);
    request_full_screen_repaint();
}

esp_err_t d1l_ui_phase1_request_tab(const char *name)
{
    d1l_ui_tab_t tab = D1L_UI_TAB_HOME;
    if (!tab_from_name(name, &tab)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!d1l_ui_screen_available(tab)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_started || !s_content) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * `ui tab` is a supported product navigation command. Opening Map through
     * it must behave like the corresponding Home/dock action so the configured
     * current view can render from SD and, when available, refresh over Wi-Fi.
     * Qualification automation uses its separate suppressed scroll path and
     * never calls this product Map navigation.
     */
    request_product_navigation_wake();
    set_map_interactive_touch_authorized(tab == D1L_UI_TAB_MAP);
    if (tab == D1L_UI_TAB_MESSAGES) {
        s_messages_mode = D1L_UI_MESSAGES_MODE_ROOT;
    }
    request_tab_switch(tab);
    return ESP_OK;
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
esp_err_t d1l_ui_phase1_request_map_acceptance(void)
{
    if (!d1l_ui_screen_available(D1L_UI_TAB_MAP)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /*
     * The inactivity lock stays foreground while this bounded acceptance view
     * renders behind it. Never bypass onboarding or an active user modal.
     */
    if (!s_started || !s_content || s_onboarding_visible ||
        d1l_ui_modal_has_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * This narrow machine-acceptance path opens only the configured current
     * Map view. It accepts no URL or coordinates and does not transmit RF.
     * Read-only scroll-probe automation remains network-suppressed.
     */
    d1l_ui_map_viewport_prepare_acceptance();
    d1l_map_view_service_force_reload_next_acquire();
    set_map_interactive_touch_authorized(true);
    request_tab_switch(D1L_UI_TAB_MAP);
    return ESP_OK;
}

esp_err_t d1l_ui_phase1_scroll_probe(const char *surface,
                                      d1l_ui_scroll_probe_result_t *result)
{
    char canonical[24] = {0};
    d1l_ui_tab_t tab = D1L_UI_TAB_HOME;
    uint32_t request_id = 0U;
    if (!result || !scroll_surface_from_name(surface, canonical, sizeof(canonical), &tab)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!d1l_ui_scroll_surface_available(canonical, tab)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_started || !s_content || !s_scroll_probe_done_sem) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_scroll_probe_lock);
    if (!probe_gate_reserve_admission(&s_scroll_probe_gate)) {
        portEXIT_CRITICAL(&s_scroll_probe_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_scroll_probe_lock);

    /* The idle gate is reserved before stale-token drain, so a late producer
     * cannot publish between drain and admission. */
    while (xSemaphoreTake(s_scroll_probe_done_sem, 0) == pdTRUE) {
    }

    portENTER_CRITICAL(&s_scroll_probe_lock);
    request_id = probe_gate_commit_admission(&s_scroll_probe_gate);
    if (request_id == 0U) {
        probe_gate_abort_admission(&s_scroll_probe_gate);
        portEXIT_CRITICAL(&s_scroll_probe_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_scroll_probe_result, 0, sizeof(s_scroll_probe_result));
    snprintf(s_scroll_probe_surface, sizeof(s_scroll_probe_surface), "%s", canonical);
    portEXIT_CRITICAL(&s_scroll_probe_lock);

    if (xSemaphoreTake(s_scroll_probe_done_sem,
                       pdMS_TO_TICKS(D1L_UI_SCROLL_PROBE_TIMEOUT_MS)) != pdTRUE) {
        portENTER_CRITICAL(&s_scroll_probe_lock);
        probe_gate_timeout(&s_scroll_probe_gate, request_id);
        portEXIT_CRITICAL(&s_scroll_probe_lock);
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_scroll_probe_lock);
    if (s_scroll_probe_gate.completed_id != request_id ||
        s_scroll_probe_gate.inflight_id != request_id) {
        probe_gate_timeout(&s_scroll_probe_gate, request_id);
        portEXIT_CRITICAL(&s_scroll_probe_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *result = s_scroll_probe_result;
    if (!probe_gate_acknowledge(&s_scroll_probe_gate, request_id)) {
        portEXIT_CRITICAL(&s_scroll_probe_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_scroll_probe_lock);
    return ESP_OK;
}

esp_err_t d1l_ui_phase1_compose_probe(const char *target,
                                       d1l_ui_compose_probe_result_t *result)
{
    char canonical[16] = {0};
    uint32_t request_id = 0U;
    if (!result || !compose_probe_target_from_name(target, canonical, sizeof(canonical))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!compose_probe_target_available(canonical)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_started || !s_content || !s_compose_probe_done_sem) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_compose_probe_lock);
    if (!probe_gate_reserve_admission(&s_compose_probe_gate)) {
        portEXIT_CRITICAL(&s_compose_probe_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_compose_probe_lock);

    while (xSemaphoreTake(s_compose_probe_done_sem, 0) == pdTRUE) {
    }

    portENTER_CRITICAL(&s_compose_probe_lock);
    request_id = probe_gate_commit_admission(&s_compose_probe_gate);
    if (request_id == 0U) {
        probe_gate_abort_admission(&s_compose_probe_gate);
        portEXIT_CRITICAL(&s_compose_probe_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_compose_probe_result, 0, sizeof(s_compose_probe_result));
    snprintf(s_compose_probe_target, sizeof(s_compose_probe_target), "%s", canonical);
    portEXIT_CRITICAL(&s_compose_probe_lock);

    if (xSemaphoreTake(s_compose_probe_done_sem,
                       pdMS_TO_TICKS(D1L_UI_COMPOSE_PROBE_TIMEOUT_MS)) != pdTRUE) {
        portENTER_CRITICAL(&s_compose_probe_lock);
        probe_gate_timeout(&s_compose_probe_gate, request_id);
        portEXIT_CRITICAL(&s_compose_probe_lock);
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_compose_probe_lock);
    if (s_compose_probe_gate.completed_id != request_id ||
        s_compose_probe_gate.inflight_id != request_id) {
        probe_gate_timeout(&s_compose_probe_gate, request_id);
        portEXIT_CRITICAL(&s_compose_probe_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *result = s_compose_probe_result;
    if (!probe_gate_acknowledge(&s_compose_probe_gate, request_id)) {
        portEXIT_CRITICAL(&s_compose_probe_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_compose_probe_lock);
    return ESP_OK;
}
#endif

const char *d1l_ui_phase1_active_tab_name(void)
{
    return tab_name(d1l_ui_navigation_active());
}

bool d1l_ui_phase1_started(void)
{
    return s_started && s_ui_task_handle != NULL &&
           s_touch_task_handle != NULL;
}

const char *d1l_ui_phase1_pending_tab_name(void)
{
    return tab_name(d1l_ui_navigation_pending());
}

bool d1l_ui_phase1_tab_switch_pending(void)
{
    return d1l_ui_navigation_switch_pending();
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void fill_capture_status(d1l_ui_capture_status_t *status)
{
    if (!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    status->available = s_capture_shadow != NULL && s_capture_snapshot != NULL && s_capture_lock != NULL;
    status->active = s_capture_active;
    status->shadow_ready = s_capture_shadow_ready;
    status->started = s_started;
    status->onboarding_visible = s_onboarding_visible;
    status->lock_visible = s_lock_visible;
    status->tab_pending = d1l_ui_phase1_tab_switch_pending();
    status->content_pending = content_refresh_pending();
    status->render_pending = status->tab_pending || status->content_pending ||
        scroll_probe_pending() || compose_probe_pending();
    status->width = D1L_UI_CAPTURE_WIDTH;
    status->height = D1L_UI_CAPTURE_HEIGHT;
    status->bytes_per_pixel = D1L_UI_CAPTURE_BYTES_PER_PIXEL;
    status->total_bytes = D1L_UI_CAPTURE_TOTAL_BYTES;
    status->max_chunk_bytes = D1L_UI_CAPTURE_MAX_CHUNK_BYTES;
    status->frame_seq = s_capture_frame_seq;
    status->flush_count = s_capture_flush_count;
    status->capture_crc32 = s_capture_crc32;
    snprintf(status->active_tab, sizeof(status->active_tab), "%s", d1l_ui_phase1_active_tab_name());
    snprintf(status->pending_tab, sizeof(status->pending_tab), "%s", d1l_ui_phase1_pending_tab_name());
}

esp_err_t d1l_ui_capture_status(d1l_ui_capture_status_t *out_status)
{
    if (!out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_capture_lock) {
        fill_capture_status(out_status);
        return ESP_OK;
    }
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    fill_capture_status(out_status);
    xSemaphoreGive(s_capture_lock);
    return ESP_OK;
}

esp_err_t d1l_ui_capture_begin(d1l_ui_capture_status_t *out_status)
{
    if (!s_capture_lock || !s_capture_shadow || !s_capture_snapshot) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (!s_capture_shadow_ready || s_capture_active) {
        xSemaphoreGive(s_capture_lock);
        return ESP_ERR_INVALID_STATE;
    }
    d1l_ui_map_viewport_set_suppressed(true);
    memcpy(s_capture_snapshot, s_capture_shadow, D1L_UI_CAPTURE_TOTAL_BYTES);
    s_capture_crc32 = crc32_bytes(s_capture_snapshot, D1L_UI_CAPTURE_TOTAL_BYTES);
    s_capture_active = true;
    fill_capture_status(out_status);
    xSemaphoreGive(s_capture_lock);
    return ESP_OK;
}

esp_err_t d1l_ui_capture_chunk(size_t offset,
                               size_t requested_len,
                               uint8_t *out_data,
                               size_t out_capacity,
                               size_t *out_len,
                               uint32_t *out_crc32,
                               d1l_ui_capture_status_t *out_status)
{
    if (!out_data || !out_len || out_capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    if (out_crc32) {
        *out_crc32 = 0U;
    }
    if (!s_capture_lock || !s_capture_snapshot) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (!s_capture_active) {
        xSemaphoreGive(s_capture_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (offset >= D1L_UI_CAPTURE_TOTAL_BYTES || requested_len == 0U) {
        xSemaphoreGive(s_capture_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t len = requested_len;
    const size_t remaining = D1L_UI_CAPTURE_TOTAL_BYTES - offset;
    if (len > remaining) {
        len = remaining;
    }
    if (len > D1L_UI_CAPTURE_MAX_CHUNK_BYTES) {
        len = D1L_UI_CAPTURE_MAX_CHUNK_BYTES;
    }
    if (len > out_capacity) {
        len = out_capacity;
    }
    memcpy(out_data, s_capture_snapshot + offset, len);
    if (out_crc32) {
        *out_crc32 = crc32_bytes(out_data, len);
    }
    *out_len = len;
    fill_capture_status(out_status);
    xSemaphoreGive(s_capture_lock);
    return ESP_OK;
}

esp_err_t d1l_ui_capture_end(d1l_ui_capture_status_t *out_status)
{
    if (!s_capture_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (!s_capture_active) {
        xSemaphoreGive(s_capture_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_capture_active = false;
    fill_capture_status(out_status);
    xSemaphoreGive(s_capture_lock);
    d1l_ui_map_viewport_set_suppressed(false);
    if (d1l_ui_navigation_active() == D1L_UI_TAB_MAP &&
        !d1l_ui_modal_has_active() && !s_lock_visible && !s_onboarding_visible &&
        map_interactive_touch_authorized()) {
        request_content_refresh();
    }
    return ESP_OK;
}
#endif

static void request_tab_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    const d1l_ui_tab_t *tab = (const d1l_ui_tab_t *)lv_event_get_user_data(event);
    if (tab && d1l_ui_screen_available(*tab)) {
        set_map_interactive_touch_authorized(*tab == D1L_UI_TAB_MAP);
        if (*tab == D1L_UI_TAB_MESSAGES) {
            s_messages_mode = D1L_UI_MESSAGES_MODE_ROOT;
        }
        request_tab_switch(*tab);
    }
}

static void dock_event_cb(lv_event_t *event)
{
    request_tab_event_cb(event);
}

static void process_pending_tab_switch(void)
{
    d1l_ui_tab_t rendered_tab;
    if (!begin_pending_tab_switch(&rendered_tab)) {
        return;
    }
    if (begin_product_navigation_wake()) {
        lv_disp_trig_activity(NULL);
        unlock_event_cb(NULL);
    }
    d1l_ui_map_viewport_release();
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_compose_sheet();
    hide_message_detail_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_storage_sheet();
    hide_wifi_sheet();
    hide_ble_sheet();
    hide_display_sheet();
    hide_diagnostics_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_channel_management_sheets();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    render_active_tab();
    force_ui_layout_repaint();
    finish_pending_tab_switch(rendered_tab);
}

static void process_pending_content_refresh(void)
{
    if (!begin_pending_content_refresh()) {
        return;
    }
    if (d1l_ui_phase1_tab_switch_pending()) {
        request_content_refresh();
        return;
    }
    /* Snapshot the visible Messages hierarchy before rebuilding the active
     * tab. Incoming store generations must refresh the related retained view
     * without changing the selected mode or replacing the user's modal. */
    const d1l_ui_messages_mode_t messages_mode = s_messages_mode;
    const bool messages_active =
        d1l_ui_navigation_active() == D1L_UI_TAB_MESSAGES;
    const bool refresh_public_history = messages_active &&
        (d1l_ui_modal_visible(s_public_history_sheet) ||
         d1l_ui_modal_visible(s_public_search_sheet));
    const bool refresh_dm_thread =
        d1l_ui_messages_thread_active(&s_messages_controller);
    const bool refresh_channel_selector = messages_active &&
        d1l_ui_messages_channel_selector_active(&s_messages_controller);
    const bool public_search_visible =
        d1l_ui_modal_visible(s_public_search_sheet);
    const bool dm_search_visible = d1l_ui_modal_visible(s_dm_search_sheet);
    render_active_tab();
    /* render_active_tab() consumes the captured mode but never owns it. This
     * assignment is an explicit fail-safe against future renderer changes. */
    s_messages_mode = messages_mode;
    if (refresh_public_history) {
        /* The search textarea and keyboard live in a separate modal. Rebuild
         * only the retained result sheet so focus and the typed query survive. */
        render_public_history_sheet();
        if (!public_search_visible) {
            show_modal(s_public_history_sheet);
        }
    }
    if (refresh_dm_thread) {
        if (render_dm_thread_sheet()) {
            show_modal(dm_search_visible ? s_dm_search_sheet :
                       d1l_ui_messages_thread_sheet(&s_messages_controller));
        } else {
            hide_dm_thread_sheet();
            show_toast("DM", ESP_ERR_NO_MEM);
        }
    }
    if (refresh_channel_selector) {
        if (d1l_ui_messages_render_channel_selector(
                &s_messages_controller, handle_messages_action, NULL)) {
            show_modal(d1l_ui_messages_channel_selector_sheet(
                &s_messages_controller));
        } else {
            d1l_ui_messages_hide_channel_selector(&s_messages_controller);
            /* Selector rendering owns the shared Messages action generation.
             * Rebuild the exposed root controls after any partial allocation
             * failure so no stale or inert bindings remain visible. */
            request_content_refresh();
            show_toast("Channels", ESP_ERR_NO_MEM);
        }
    }
    force_ui_layout_repaint();
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static lv_obj_t *find_scrollable_descendant(lv_obj_t *root, lv_obj_t **fallback)
{
    if (!root) {
        return NULL;
    }
    lv_obj_update_layout(root);
    if (lv_obj_has_flag(root, LV_OBJ_FLAG_SCROLLABLE)) {
        if (!*fallback) {
            *fallback = root;
        }
        if (lv_obj_get_scroll_bottom(root) > 0 || lv_obj_get_scroll_top(root) > 0) {
            return root;
        }
    }

    const uint32_t child_count = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *child = lv_obj_get_child(root, (int32_t)i);
        lv_obj_t *target = find_scrollable_descendant(child, fallback);
        if (target) {
            return target;
        }
    }
    return NULL;
}

static lv_obj_t *scroll_probe_find_target(lv_obj_t *root)
{
    lv_obj_t *fallback = NULL;
    lv_obj_t *target = find_scrollable_descendant(root, &fallback);
    return target ? target : fallback;
}

static void scroll_probe_contact_entry(d1l_contact_entry_t *entry)
{
    if (!entry) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    d1l_app_model_snapshot(&s_snapshot);
    if (s_snapshot.recent_contact_count > 0 &&
        s_snapshot.recent_contacts[0].fingerprint[0] != '\0') {
        *entry = s_snapshot.recent_contacts[0];
        return;
    }
    snprintf(entry->fingerprint, sizeof(entry->fingerprint), "%s", "C0DEC0DEC0DEC0DE");
    snprintf(entry->public_key_hex, sizeof(entry->public_key_hex), "%s",
             "C0DEC0DEC0DEC0DEC0DEC0DEC0DEC0DEC0DEC0DEC0DEC0DEC0DEC0DEC0DEC0DE");
    snprintf(entry->alias, sizeof(entry->alias), "%s", "Contact Probe");
    snprintf(entry->heard_name, sizeof(entry->heard_name), "%s", "Contact Probe");
    snprintf(entry->type, sizeof(entry->type), "%s", "chat");
    entry->last_rssi_dbm = -42;
    entry->last_snr_tenths = 30;
    entry->out_path_valid = true;
}

static lv_obj_t *scroll_probe_open_contact_surface(const char *surface)
{
    d1l_contact_entry_t contact = {0};
    scroll_probe_contact_entry(&contact);
    hide_contact_detail_sheet();
    if (!set_selected_contact(&contact)) {
        return NULL;
    }

    if (strcmp(surface, "contact_detail") == 0) {
        show_contact_detail_sheet();
        return d1l_ui_contact_sheets_detail(&s_contact_sheets_controller);
    }
    if (strcmp(surface, "contact_options") == 0) {
        show_contact_options_sheet();
        return d1l_ui_contact_sheets_options(&s_contact_sheets_controller);
    }
    if (strcmp(surface, "contact_forget") == 0) {
        show_contact_forget_sheet();
        return d1l_ui_contact_sheets_forget(&s_contact_sheets_controller);
    }
    if (strcmp(surface, "contact_route") == 0) {
        s_route_trace_contact = contact;
        render_route_trace_sheet();
        if (s_route_trace_sheet) {
            show_modal(s_route_trace_sheet);
        }
        return s_route_trace_sheet;
    }
    return NULL;
}

static lv_obj_t *scroll_probe_open_mesh_surface(const char *surface)
{
    open_mesh_roles_event_cb(NULL);
    if (strcmp(surface, "mesh_rooms") == 0) {
        open_mesh_room_servers_event_cb(NULL);
    } else if (strcmp(surface, "mesh_repeaters") == 0) {
        open_mesh_repeater_candidates_event_cb(NULL);
    } else if (strcmp(surface, "mesh_roles") != 0) {
        return NULL;
    }
    return s_mesh_roles_sheet;
}

static lv_obj_t *scroll_probe_open_storage_surface(const char *surface)
{
    open_storage_sheet_event_cb(NULL);
    if (strcmp(surface, "storage_card") == 0) {
        open_storage_card_status_event_cb(NULL);
    } else if (strcmp(surface, "storage_data") == 0) {
        open_storage_data_locations_event_cb(NULL);
        return scroll_probe_find_target(s_storage_sheet);
    } else if (strcmp(surface, "storage") != 0) {
        return NULL;
    }
    return s_storage_sheet;
}

static lv_obj_t *scroll_probe_open_map_surface(const char *surface)
{
    if (strcmp(surface, "map") == 0) {
        render_active_tab();
        return s_content;
    }
    if (strcmp(surface, "map_location") == 0) {
        open_map_location_sheet_event_cb(NULL);
        return d1l_ui_map_location_sheet(&s_map_sheets_controller);
    }
    open_map_options_sheet_event_cb(NULL);
    if (strcmp(surface, "map_cache") == 0) {
        open_map_cache_status_event_cb(NULL);
    } else if (strcmp(surface, "map_options") != 0) {
        return NULL;
    }
    return d1l_ui_map_options_sheet(&s_map_sheets_controller);
}

static void measure_scroll_probe_target(lv_obj_t *target,
                                        d1l_ui_scroll_probe_result_t *result)
{
    if (!target || !result) {
        return;
    }
    lv_obj_update_layout(target);
    result->target_found = true;
    result->scrollable = lv_obj_has_flag(target, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_scroll_to_y(target, 0, LV_ANIM_OFF);
    lv_obj_update_layout(target);
    result->before_y = (int32_t)lv_obj_get_scroll_y(target);
    result->scroll_top_before = (int32_t)lv_obj_get_scroll_top(target);
    result->scroll_bottom_before = (int32_t)lv_obj_get_scroll_bottom(target);

    lv_obj_scroll_to_y(target, LV_COORD_MAX, LV_ANIM_OFF);
    lv_obj_update_layout(target);
    result->after_y = (int32_t)lv_obj_get_scroll_y(target);
    result->scroll_top_after = (int32_t)lv_obj_get_scroll_top(target);
    result->scroll_bottom_after = (int32_t)lv_obj_get_scroll_bottom(target);
    result->movement_required =
        result->scroll_top_before > 0 ||
        result->scroll_bottom_before > 0 ||
        result->scroll_top_after > 0 ||
        result->scroll_bottom_after > 0;
    result->moved = result->before_y != result->after_y ||
        result->scroll_top_before != result->scroll_top_after ||
        result->scroll_bottom_before != result->scroll_bottom_after;
    result->ok = result->surface_supported && result->target_found &&
        result->scrollable &&
        (!result->movement_required || result->moved);
}

static lv_obj_t *scroll_probe_open_surface(const char *surface)
{
    if (strcmp(surface, "public_messages") == 0) {
        s_messages_mode = D1L_UI_MESSAGES_MODE_PUBLIC;
        render_active_tab();
        s_public_history_channel_id = D1L_CHANNEL_PUBLIC_ID;
        snprintf(s_public_history_channel_name,
                 sizeof(s_public_history_channel_name), "%s", "Public");
        show_public_history_sheet();
        return scroll_probe_find_target(s_public_history_sheet);
    }
    if (strcmp(surface, "dm_thread") == 0) {
        s_messages_mode = D1L_UI_MESSAGES_MODE_DIRECT;
        render_active_tab();
        d1l_app_model_snapshot(&s_snapshot);
        if (s_snapshot.recent_dm_count == 0 ||
            s_snapshot.recent_dms[0].contact_fingerprint[0] == '\0') {
            show_dm_thread_for("0000000000000000", "Scroll Probe DM");
            return scroll_probe_find_target(
                d1l_ui_messages_thread_sheet(&s_messages_controller));
        }
        show_dm_thread_for(s_snapshot.recent_dms[0].contact_fingerprint,
                           s_snapshot.recent_dms[0].contact_alias[0] ?
                           s_snapshot.recent_dms[0].contact_alias :
                           s_snapshot.recent_dms[0].contact_fingerprint);
        return scroll_probe_find_target(
            d1l_ui_messages_thread_sheet(&s_messages_controller));
    }
    if (strcmp(surface, "storage") == 0 ||
        strcmp(surface, "storage_card") == 0 ||
        strcmp(surface, "storage_data") == 0) {
        return scroll_probe_open_storage_surface(surface);
    }
    if (strcmp(surface, "map") == 0 ||
        strcmp(surface, "map_options") == 0 ||
        strcmp(surface, "map_location") == 0 ||
        strcmp(surface, "map_cache") == 0) {
        return scroll_probe_open_map_surface(surface);
    }
    if (strcmp(surface, "wifi") == 0) {
        open_wifi_sheet_event_cb(NULL);
        return scroll_probe_find_target(d1l_ui_wifi_sheet(&s_wifi_controller));
    }
    if (strncmp(surface, "contact_", strlen("contact_")) == 0) {
        return scroll_probe_open_contact_surface(surface);
    }
    if (strcmp(surface, "mesh_roles") == 0 ||
        strcmp(surface, "mesh_rooms") == 0 ||
        strcmp(surface, "mesh_repeaters") == 0) {
        return scroll_probe_open_mesh_surface(surface);
    }
    return scroll_probe_find_target(s_content);
}

static void run_scroll_probe_on_ui_task(const char *surface,
                                        d1l_ui_scroll_probe_result_t *result)
{
    d1l_ui_tab_t tab = D1L_UI_TAB_HOME;
    char canonical[24] = {0};
    if (!scroll_surface_from_name(surface, canonical, sizeof(canonical), &tab) ||
        !d1l_ui_scroll_surface_available(canonical, tab)) {
        return;
    }
    set_map_interactive_touch_authorized(false);
    d1l_ui_map_viewport_set_suppressed(true);

    result->surface_supported = true;
    snprintf(result->surface, sizeof(result->surface), "%s", canonical);
    snprintf(result->tab, sizeof(result->tab), "%s", tab_name(tab));
    request_tab_switch(tab);
    process_pending_tab_switch();

    lv_obj_t *target = scroll_probe_open_surface(canonical);
    const bool static_page =
        strncmp(canonical, "contact_", strlen("contact_")) == 0 ||
        strcmp(canonical, "map") == 0 ||
        strcmp(canonical, "map_options") == 0 ||
        strcmp(canonical, "map_location") == 0 ||
        strcmp(canonical, "map_cache") == 0 ||
        strcmp(canonical, "storage") == 0 ||
        strcmp(canonical, "storage_card") == 0 ||
        strcmp(canonical, "mesh_roles") == 0 ||
        strcmp(canonical, "mesh_rooms") == 0 ||
        strcmp(canonical, "mesh_repeaters") == 0;
    force_ui_layout_repaint();
    if (static_page) {
        result->target_found = target != NULL;
        result->scrollable = target && lv_obj_has_flag(target, LV_OBJ_FLAG_SCROLLABLE);
        result->ok = result->surface_supported && result->target_found;
        d1l_ui_map_viewport_set_suppressed(false);
        return;
    }
    measure_scroll_probe_target(target, result);
    if (strcmp(canonical, "storage_data") == 0 && target) {
        lv_obj_scroll_to_y(target, 0, LV_ANIM_OFF);
        lv_obj_update_layout(target);
        force_ui_layout_repaint();
    }
    d1l_ui_map_viewport_set_suppressed(false);
}

static uint32_t begin_pending_scroll_probe(char *surface, size_t surface_len)
{
    uint32_t request_id;

    portENTER_CRITICAL(&s_scroll_probe_lock);
    request_id = probe_gate_begin(&s_scroll_probe_gate);
    if (request_id != 0U && surface && surface_len > 0) {
        snprintf(surface, surface_len, "%s", s_scroll_probe_surface);
    }
    portEXIT_CRITICAL(&s_scroll_probe_lock);
    return request_id;
}

static void finish_pending_scroll_probe(const d1l_ui_scroll_probe_result_t *result,
                                        uint32_t request_id)
{
    bool signal_completion;
    portENTER_CRITICAL(&s_scroll_probe_lock);
    signal_completion = probe_gate_publish(&s_scroll_probe_gate, request_id);
    if (signal_completion) {
        s_scroll_probe_result = *result;
    }
    portEXIT_CRITICAL(&s_scroll_probe_lock);

    /* Publish before releasing the in-flight owner. A timed-out request can
     * leave a semaphore token, but no later request can be admitted until the
     * token has been published and can be drained safely. */
    if (signal_completion && s_scroll_probe_done_sem) {
        (void)xSemaphoreGive(s_scroll_probe_done_sem);
    }
    portENTER_CRITICAL(&s_scroll_probe_lock);
    probe_gate_finish_signal(&s_scroll_probe_gate, request_id);
    portEXIT_CRITICAL(&s_scroll_probe_lock);
}

static void process_pending_scroll_probe(void)
{
    char surface[24] = {0};
    const uint32_t request_id = begin_pending_scroll_probe(surface, sizeof(surface));
    if (request_id == 0U) {
        return;
    }

    d1l_ui_scroll_probe_result_t result = {0};
    run_scroll_probe_on_ui_task(surface, &result);
    finish_pending_scroll_probe(&result, request_id);
}

static bool object_is_visible(lv_obj_t *obj)
{
    return d1l_ui_modal_visible(obj);
}

static bool object_is_hidden_or_missing(lv_obj_t *obj)
{
    return !obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static const char *compose_probe_sample_text(const char *target)
{
    if (target && strstr(target, "long")) {
        return "Long hardware compose validation text checks the 480 screen keyboard without RF.";
    }
    return "test from DeskOS D1L";
}

static d1l_ui_tab_t compose_probe_tab_for_target(const char *target)
{
    if (!target) {
        return D1L_UI_TAB_HOME;
    }
    if (d1l_ui_keyboard_probe_target_is_compose(target) ||
        strcmp(target, "public_search") == 0 ||
        strcmp(target, "dm_search") == 0) {
        return D1L_UI_TAB_MESSAGES;
    }
    if (strcmp(target, "packet_search") == 0) {
        return D1L_UI_TAB_PACKETS;
    }
    if (strcmp(target, "contact_edit") == 0) {
        return D1L_UI_TAB_NODES;
    }
    if (strcmp(target, "map_location") == 0) {
        return D1L_UI_TAB_MAP;
    }
    if (strcmp(target, "wifi_ssid") == 0 ||
        strcmp(target, "wifi_password") == 0) {
        return D1L_UI_TAB_SETTINGS;
    }
    return D1L_UI_TAB_HOME;
}

static void hide_keyboard_probe_sheets(bool include_onboarding)
{
    hide_sheet();
    hide_public_history_sheet();
    hide_public_search_sheet();
    hide_message_detail_sheet();
    hide_dm_thread_sheet();
    hide_radio_settings_sheet();
    hide_storage_sheet();
    hide_wifi_sheet();
    hide_ble_sheet();
    hide_display_sheet();
    hide_diagnostics_sheet();
    hide_map_location_sheet();
    hide_map_options_sheet();
    hide_contact_detail_sheet();
    hide_contact_export_sheet();
    hide_channel_management_sheets();
    hide_route_detail_sheet();
    hide_route_trace_sheet();
    hide_packet_detail_sheet();
    hide_packet_search_sheet();
    hide_mesh_roles_sheet();
    hide_compose_sheet();
    if (include_onboarding) {
        hide_onboarding_sheet();
    }
}

static void fill_compose_probe_objects(const char *target, lv_obj_t **sheet,
                                       lv_obj_t **textarea, lv_obj_t **keyboard)
{
    if (!sheet || !textarea || !keyboard) {
        return;
    }
    *sheet = NULL;
    *textarea = NULL;
    *keyboard = NULL;
    if (d1l_ui_keyboard_probe_target_is_compose(target)) {
        *sheet = s_compose_sheet;
        *textarea = s_compose_textarea;
        *keyboard = s_compose_keyboard;
    } else if (strcmp(target, "public_search") == 0) {
        *sheet = s_public_search_sheet;
        *textarea = s_public_search_textarea;
        *keyboard = s_public_search_keyboard;
    } else if (strcmp(target, "dm_search") == 0) {
        *sheet = s_dm_search_sheet;
        *textarea = s_dm_search_textarea;
        *keyboard = s_dm_search_keyboard;
    } else if (strcmp(target, "packet_search") == 0) {
        *sheet = s_packet_search_sheet;
        *textarea = s_packet_search_textarea;
        *keyboard = s_packet_search_keyboard;
    } else if (strcmp(target, "contact_edit") == 0) {
        *sheet = d1l_ui_contact_sheets_edit(&s_contact_sheets_controller);
        *textarea = d1l_ui_contact_sheets_edit_textarea(
            &s_contact_sheets_controller);
        *keyboard = d1l_ui_contact_sheets_edit_keyboard(
            &s_contact_sheets_controller);
    } else if (d1l_ui_keyboard_probe_target_is_onboarding(target)) {
        *sheet = s_onboarding_sheet;
        *textarea = s_onboarding_name_textarea;
        *keyboard = s_onboarding_keyboard;
    } else if (strcmp(target, "map_location") == 0) {
        *sheet = d1l_ui_map_location_sheet(&s_map_sheets_controller);
        *textarea = d1l_ui_map_latitude_textarea(&s_map_sheets_controller);
        *keyboard = d1l_ui_map_location_keyboard(&s_map_sheets_controller);
    } else if (strcmp(target, "wifi_ssid") == 0) {
        *sheet = d1l_ui_wifi_sheet(&s_wifi_controller);
        *textarea = d1l_ui_wifi_ssid_textarea(&s_wifi_controller);
        *keyboard = d1l_ui_wifi_keyboard(&s_wifi_controller);
    } else if (strcmp(target, "wifi_password") == 0) {
        *sheet = d1l_ui_wifi_sheet(&s_wifi_controller);
        *textarea = d1l_ui_wifi_password_textarea(&s_wifi_controller);
        *keyboard = d1l_ui_wifi_keyboard(&s_wifi_controller);
    }
}

static void open_compose_probe_on_ui_task(const char *target)
{
    hide_keyboard_probe_sheets(true);
    s_onboarding_probe_suppressed = true;
    request_tab_switch(D1L_UI_TAB_MESSAGES);
    process_pending_tab_switch();
    s_onboarding_probe_suppressed = true;
    s_messages_mode = d1l_ui_keyboard_probe_target_is_dm(target) ?
        D1L_UI_MESSAGES_MODE_DIRECT : D1L_UI_MESSAGES_MODE_PUBLIC;
    render_active_tab();
    hide_onboarding_sheet();

    if (d1l_ui_keyboard_probe_target_is_dm(target)) {
        d1l_contact_entry_t contact = {0};
        snprintf(contact.fingerprint, sizeof(contact.fingerprint), "%s", "0000000000000000");
        snprintf(contact.public_key_hex, sizeof(contact.public_key_hex), "%s", "00");
        snprintf(contact.alias, sizeof(contact.alias), "%s", "Probe DM");
        present_dm_compose_sheet(&contact, true);
    } else {
        (void)show_channel_compose_sheet(
            D1L_CHANNEL_PUBLIC_ID, "Compose Public", "Public message");
        s_compose_probe_send_suppressed = true;
    }

    if (s_compose_textarea) {
        lv_textarea_set_text(s_compose_textarea, compose_probe_sample_text(target));
        update_compose_counter();
    }
    if (s_compose_keyboard) {
        lv_keyboard_set_mode(s_compose_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(s_compose_keyboard, s_compose_textarea);
        layout_compose_sheet_controls();
    }
    lv_obj_update_layout(s_screen);
    layout_compose_sheet_controls();
    lv_obj_update_layout(s_screen);
    request_full_screen_repaint();
    force_ui_layout_repaint();
}

static void open_keyboard_probe_on_ui_task(const char *target)
{
    if (d1l_ui_keyboard_probe_target_is_compose(target)) {
        open_compose_probe_on_ui_task(target);
        return;
    }

    hide_keyboard_probe_sheets(true);
    s_onboarding_probe_suppressed = !d1l_ui_keyboard_probe_target_is_onboarding(target);
    request_tab_switch(compose_probe_tab_for_target(target));
    process_pending_tab_switch();
    s_onboarding_probe_suppressed = !d1l_ui_keyboard_probe_target_is_onboarding(target);
    if (!d1l_ui_keyboard_probe_target_is_onboarding(target)) {
        hide_onboarding_sheet();
    }

    if (strcmp(target, "public_search") == 0) {
        s_messages_mode = D1L_UI_MESSAGES_MODE_PUBLIC;
        render_active_tab();
        hide_onboarding_sheet();
        s_public_history_channel_id = D1L_CHANNEL_PUBLIC_ID;
        snprintf(s_public_history_channel_name,
                 sizeof(s_public_history_channel_name), "%s", "Public");
        open_public_search_event_cb(NULL);
        if (s_public_search_textarea) {
            lv_textarea_set_text(s_public_search_textarea, "probe");
        }
        if (s_public_search_keyboard) {
            lv_keyboard_set_textarea(s_public_search_keyboard, s_public_search_textarea);
        }
    } else if (strcmp(target, "dm_search") == 0) {
        s_messages_mode = D1L_UI_MESSAGES_MODE_DIRECT;
        render_active_tab();
        hide_onboarding_sheet();
        if (d1l_ui_messages_select_thread(
                &s_messages_controller, "0000000000000000", "Probe DM") &&
            render_dm_thread_sheet()) {
            open_dm_search_event_cb(NULL);
        }
        if (s_dm_search_textarea) {
            lv_textarea_set_text(s_dm_search_textarea, "probe");
        }
        if (s_dm_search_keyboard) {
            lv_keyboard_set_textarea(s_dm_search_keyboard, s_dm_search_textarea);
        }
    } else if (strcmp(target, "packet_search") == 0) {
        open_packet_search_event_cb(NULL);
        if (s_packet_search_textarea) {
            lv_textarea_set_text(s_packet_search_textarea, "probe");
        }
        if (s_packet_search_keyboard) {
            lv_keyboard_set_textarea(s_packet_search_keyboard, s_packet_search_textarea);
        }
    } else if (strcmp(target, "contact_edit") == 0) {
        d1l_contact_entry_t contact = {0};
        snprintf(contact.fingerprint, sizeof(contact.fingerprint),
                 "%s", "0000000000000000");
        snprintf(contact.public_key_hex, sizeof(contact.public_key_hex),
                 "%s", "00");
        snprintf(contact.alias, sizeof(contact.alias),
                 "%s", "Probe Contact");
        snprintf(contact.type, sizeof(contact.type), "%s", "chat");
        if (set_selected_contact(&contact)) {
            show_contact_edit_sheet();
        }
    } else if (d1l_ui_keyboard_probe_target_is_onboarding(target)) {
        s_onboarding_probe_suppressed = false;
        if (s_onboarding_sheet) {
            s_onboarding_visible = true;
            show_modal(s_onboarding_sheet);
        }
        if (s_onboarding_name_textarea) {
            lv_textarea_set_text(s_onboarding_name_textarea, "DeskOS Probe");
        }
        if (s_onboarding_keyboard) {
            lv_keyboard_set_textarea(s_onboarding_keyboard, s_onboarding_name_textarea);
        }
    } else if (strcmp(target, "map_location") == 0) {
        open_map_location_sheet_event_cb(NULL);
        lv_obj_t *latitude =
            d1l_ui_map_latitude_textarea(&s_map_sheets_controller);
        lv_obj_t *keyboard =
            d1l_ui_map_location_keyboard(&s_map_sheets_controller);
        if (latitude) {
            lv_textarea_set_text(latitude, "43.6532");
        }
        if (keyboard) {
            lv_keyboard_set_textarea(keyboard, latitude);
            lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(keyboard);
        }
    } else if (strcmp(target, "wifi_ssid") == 0 ||
               strcmp(target, "wifi_password") == 0) {
        open_wifi_sheet_event_cb(NULL);
        lv_obj_t *wifi_ssid = d1l_ui_wifi_ssid_textarea(&s_wifi_controller);
        lv_obj_t *wifi_password =
            d1l_ui_wifi_password_textarea(&s_wifi_controller);
        lv_obj_t *wifi_keyboard = d1l_ui_wifi_keyboard(&s_wifi_controller);
        if (strcmp(target, "wifi_password") == 0) {
            if (wifi_password) {
                lv_textarea_set_text(wifi_password, "probe-pass");
            }
            if (wifi_keyboard) {
                lv_keyboard_set_textarea(wifi_keyboard, wifi_password);
            }
        } else {
            if (wifi_ssid) {
                lv_textarea_set_text(wifi_ssid, "ProbeNet");
            }
            if (wifi_keyboard) {
                lv_keyboard_set_textarea(wifi_keyboard, wifi_ssid);
            }
        }
    }

    lv_obj_update_layout(s_screen);
    request_full_screen_repaint();
    force_ui_layout_repaint();
}

static void fill_compose_probe_geometry(d1l_ui_compose_probe_result_t *result,
                                        lv_obj_t *sheet, lv_obj_t *textarea,
                                        lv_obj_t *keyboard)
{
    if (!result) {
        return;
    }
    if (sheet) {
        result->sheet_x = (int32_t)lv_obj_get_x(sheet);
        result->sheet_y = (int32_t)lv_obj_get_y(sheet);
        result->sheet_w = (int32_t)lv_obj_get_width(sheet);
        result->sheet_h = (int32_t)lv_obj_get_height(sheet);
    }
    if (textarea) {
        result->textarea_x = (int32_t)lv_obj_get_x(textarea);
        result->textarea_y = (int32_t)lv_obj_get_y(textarea);
        result->textarea_w = (int32_t)lv_obj_get_width(textarea);
        result->textarea_h = (int32_t)lv_obj_get_height(textarea);
    }
    if (keyboard) {
        result->keyboard_x = (int32_t)lv_obj_get_x(keyboard);
        result->keyboard_y = (int32_t)lv_obj_get_y(keyboard);
        result->keyboard_w = (int32_t)lv_obj_get_width(keyboard);
        result->keyboard_h = (int32_t)lv_obj_get_height(keyboard);
    }
}

static void run_compose_probe_on_ui_task(const char *target,
                                         d1l_ui_compose_probe_result_t *result)
{
    char canonical[16] = {0};
    if (!compose_probe_target_from_name(target, canonical, sizeof(canonical)) ||
        !compose_probe_target_available(canonical)) {
        return;
    }
    set_map_interactive_touch_authorized(false);
    d1l_ui_map_viewport_set_suppressed(true);
    result->target_supported = true;
    snprintf(result->target, sizeof(result->target), "%s", canonical);

    open_keyboard_probe_on_ui_task(canonical);
    snprintf(result->active_tab, sizeof(result->active_tab), "%s", tab_name(d1l_ui_navigation_active()));

    lv_obj_t *sheet = NULL;
    lv_obj_t *textarea = NULL;
    lv_obj_t *keyboard = NULL;
    fill_compose_probe_objects(canonical, &sheet, &textarea, &keyboard);
    result->sheet_visible = object_is_visible(sheet);
    result->textarea_visible = object_is_visible(textarea);
    result->keyboard_visible = object_is_visible(keyboard);
    result->onboarding_visible = s_onboarding_visible;
    result->dock_hidden = object_is_hidden_or_missing(s_dock);
    result->dm_mode = s_compose_dm ||
        (strcmp(canonical, "dm_search") == 0 &&
         s_messages_mode == D1L_UI_MESSAGES_MODE_DIRECT);
    result->tx_suppressed = s_compose_probe_send_suppressed;
    if (d1l_ui_keyboard_probe_target_is_compose(canonical)) {
        const char *text = s_compose_textarea ?
            lv_textarea_get_text(s_compose_textarea) : "";
        const d1l_user_text_info_t info = d1l_user_text_validate(text);
        result->send_enabled =
            compose_eligibility_for_text(&info).send_enabled;
    }
    fill_compose_probe_geometry(result, sheet, textarea, keyboard);

    const bool sheet_inside_screen =
        result->sheet_x >= 0 &&
        result->sheet_y >= 0 &&
        result->sheet_x + result->sheet_w <= (int32_t)D1L_UI_CAPTURE_WIDTH &&
        result->sheet_y + result->sheet_h <= (int32_t)D1L_UI_CAPTURE_HEIGHT;
    const bool keyboard_inside_sheet =
        result->keyboard_x >= 0 &&
        result->keyboard_y >= 0 &&
        result->keyboard_x + result->keyboard_w <= result->sheet_w &&
        result->keyboard_y + result->keyboard_h <= result->sheet_h;
    const bool keyboard_inside_screen =
        result->sheet_x + result->keyboard_x >= 0 &&
        result->sheet_y + result->keyboard_y >= 0 &&
        result->sheet_x + result->keyboard_x + result->keyboard_w <= (int32_t)D1L_UI_CAPTURE_WIDTH &&
        result->sheet_y + result->keyboard_y + result->keyboard_h <= (int32_t)D1L_UI_CAPTURE_HEIGHT;
    const bool textarea_inside_sheet =
        result->textarea_x >= 0 &&
        result->textarea_y >= 0 &&
        result->textarea_x + result->textarea_w <= result->sheet_w &&
        result->textarea_y + result->textarea_h <= result->keyboard_y;
    const bool onboarding_state_ok =
        result->onboarding_visible == d1l_ui_keyboard_probe_target_is_onboarding(canonical);
    const bool dock_state_ok =
        !d1l_ui_keyboard_probe_requires_hidden_dock(canonical) || result->dock_hidden;
    const bool dm_state_ok =
        (!d1l_ui_keyboard_probe_target_is_compose(canonical) &&
         strcmp(canonical, "dm_search") != 0) ||
        result->dm_mode == d1l_ui_keyboard_probe_target_is_dm(canonical);
    const bool compose_send_suppressed =
        !d1l_ui_keyboard_probe_target_is_compose(canonical) ||
        (result->tx_suppressed && !result->send_enabled);

    result->ok = result->target_supported &&
        result->sheet_visible &&
        result->textarea_visible &&
        result->keyboard_visible &&
        onboarding_state_ok &&
        dock_state_ok &&
        dm_state_ok &&
        compose_send_suppressed &&
        result->keyboard_w >= d1l_ui_keyboard_probe_min_width(canonical) &&
        result->keyboard_h >= d1l_ui_keyboard_probe_min_height(canonical) &&
        sheet_inside_screen &&
        keyboard_inside_sheet &&
        keyboard_inside_screen &&
        textarea_inside_sheet;
    d1l_ui_map_viewport_set_suppressed(false);
}

static uint32_t begin_pending_compose_probe(char *target, size_t target_len)
{
    uint32_t request_id;

    portENTER_CRITICAL(&s_compose_probe_lock);
    request_id = probe_gate_begin(&s_compose_probe_gate);
    if (request_id != 0U && target && target_len > 0) {
        snprintf(target, target_len, "%s", s_compose_probe_target);
    }
    portEXIT_CRITICAL(&s_compose_probe_lock);
    return request_id;
}

static void finish_pending_compose_probe(const d1l_ui_compose_probe_result_t *result,
                                         uint32_t request_id)
{
    bool signal_completion;
    portENTER_CRITICAL(&s_compose_probe_lock);
    signal_completion = probe_gate_publish(&s_compose_probe_gate, request_id);
    if (signal_completion) {
        s_compose_probe_result = *result;
    }
    portEXIT_CRITICAL(&s_compose_probe_lock);
    if (signal_completion && s_compose_probe_done_sem) {
        (void)xSemaphoreGive(s_compose_probe_done_sem);
    }
    portENTER_CRITICAL(&s_compose_probe_lock);
    probe_gate_finish_signal(&s_compose_probe_gate, request_id);
    portEXIT_CRITICAL(&s_compose_probe_lock);
}

static void process_pending_compose_probe(void)
{
    char target[16] = {0};
    const uint32_t request_id = begin_pending_compose_probe(target, sizeof(target));
    if (request_id == 0U) {
        return;
    }

    d1l_ui_compose_probe_result_t result = {0};
    run_compose_probe_on_ui_task(target, &result);
    finish_pending_compose_probe(&result, request_id);
}
#endif

static void lock_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_lock_overlay) {
        if (d1l_ui_navigation_active() == D1L_UI_TAB_MAP) {
            d1l_ui_map_viewport_prepare_cover();
        }
        d1l_ui_map_viewport_release();
        s_lock_visible = true;
        lv_obj_clear_flag(s_lock_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static uint8_t desired_backlight_percent(void)
{
    d1l_settings_t settings = {0};
    d1l_display_preferences_t preferences = {0};
    (void)d1l_settings_public_snapshot(&settings);
    d1l_display_preferences_get(&preferences);
    return settings.night_mode ? 25U : preferences.brightness_percent;
}

static void unlock_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_lock_overlay) {
        (void)d1l_backlight_set_percent(desired_backlight_percent());
        s_backlight_dimmed = false;
        s_backlight_pulse_active = false;
        s_lock_visible = false;
        lv_obj_add_flag(s_lock_overlay, LV_OBJ_FLAG_HIDDEN);
        if (d1l_ui_navigation_active() == D1L_UI_TAB_MAP &&
            !d1l_ui_phase1_tab_switch_pending() &&
            !d1l_ui_modal_has_active() && !s_onboarding_visible) {
            request_content_refresh();
        }
    }
}

static bool notification_quiet_now(void)
{
    const char *time = s_snapshot.time_label;
    if (!s_snapshot.time_available || !time) {
        return true;
    }
    while (*time && (*time < '0' || *time > '9')) {
        time++;
    }
    if (time[0] < '0' || time[0] > '9' ||
        time[1] < '0' || time[1] > '9') {
        return true;
    }
    const unsigned hour =
        (unsigned)(time[0] - '0') * 10U + (unsigned)(time[1] - '0');
    return hour >= 22U || hour < 7U;
}

static void display_runtime_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    d1l_display_preferences_t preferences = {0};
    d1l_display_preferences_get(&preferences);
    const uint32_t now = lv_tick_get();
    const uint64_t unread_wide =
        (uint64_t)s_snapshot.public_unread_count +
        (uint64_t)s_snapshot.dm_unread_count;
    const uint32_t unread = unread_wide > UINT32_MAX ?
        UINT32_MAX : (uint32_t)unread_wide;

    if (unread > s_last_notification_unread &&
        preferences.notification_mode != D1L_NOTIFICATION_MODE_OFF &&
        (preferences.notification_mode !=
             D1L_NOTIFICATION_MODE_QUIET_HOURS ||
         !notification_quiet_now()) &&
        !s_backlight_dimmed && !s_lock_visible) {
        if (d1l_backlight_set_percent(100) == ESP_OK) {
            s_backlight_pulse_active = true;
            s_backlight_pulse_until = now + 450U;
        }
    }
    s_last_notification_unread = unread;

    if (s_backlight_pulse_active &&
        (int32_t)(now - s_backlight_pulse_until) >= 0) {
        (void)d1l_backlight_set_percent(desired_backlight_percent());
        s_backlight_pulse_active = false;
    }

    const uint32_t inactive_ms = lv_disp_get_inactive_time(NULL);
    if (s_backlight_dimmed) {
        if (inactive_ms < 500U) {
            (void)d1l_backlight_set_percent(desired_backlight_percent());
            s_backlight_dimmed = false;
        }
        return;
    }
    if (preferences.timeout_seconds > 0U &&
        inactive_ms >= (uint32_t)preferences.timeout_seconds * 1000U &&
        !s_lock_visible && !s_onboarding_visible) {
        lock_event_cb(NULL);
        (void)d1l_backlight_set_percent(0);
        s_backlight_dimmed = true;
        s_backlight_pulse_active = false;
    }
}

static void map_viewport_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const bool map_uncovered =
        d1l_ui_navigation_active() == D1L_UI_TAB_MAP &&
        !d1l_ui_modal_has_active() && !s_lock_visible && !s_onboarding_visible &&
        map_interactive_touch_authorized();
    if (map_uncovered) {
        d1l_ui_map_viewport_update_truth_context(&s_snapshot);
        (void)d1l_ui_map_viewport_refresh();
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    d1l_app_model_snapshot(&s_snapshot);
    update_chrome(&s_snapshot);
    update_startup_overlay(&s_snapshot);
#if !D1L_ENABLE_QUALIFICATION_HOOKS
    lv_timer_set_period(
        timer, s_onboarding_visible ? 250U : 2000U);
#endif
    if (d1l_ui_modal_visible(s_compose_sheet)) {
        update_compose_counter();
    }
    if (d1l_ui_modal_visible(d1l_ui_service_sheets_update(
            &s_service_sheets_controller))) {
        (void)render_update_service_sheet();
    } else if (d1l_ui_modal_visible(d1l_ui_service_sheets_observer(
                   &s_service_sheets_controller))) {
        (void)render_observer_service_sheet();
    } else if (d1l_ui_modal_visible(d1l_ui_service_sheets_admin(
                   &s_service_sheets_controller))) {
        refresh_admin_service_sheet_if_changed();
    } else if (d1l_ui_modal_visible(d1l_ui_service_sheets_notifications(
                   &s_service_sheets_controller))) {
        (void)render_notifications_service_sheet();
    } else if (d1l_ui_modal_visible(s_finder_sheet)) {
        d1l_app_model_discovery_snapshot(&s_finder_snapshot);
        const uint32_t render_second = lv_tick_get() / 1000U;
        if (s_finder_snapshot.generation != s_finder_rendered_generation ||
            render_second != s_finder_last_render_second) {
            render_finder_sheet();
        }
    } else if (d1l_ui_modal_visible(s_route_trace_sheet) &&
               s_route_trace_contact.fingerprint[0] != '\0') {
        d1l_app_model_contact_telemetry_snapshot(
            s_route_trace_contact.fingerprint,
            &s_route_trace_telemetry);
        const uint32_t render_second = lv_tick_get() / 1000U;
        if (s_route_trace_telemetry.generation !=
                s_route_trace_telemetry_rendered_generation ||
            render_second != s_route_trace_last_render_second) {
            render_route_trace_sheet();
        }
    }
    const bool map_active = d1l_ui_navigation_active() == D1L_UI_TAB_MAP;
    const bool map_uncovered = map_active &&
        !d1l_ui_modal_has_active() && !s_lock_visible && !s_onboarding_visible &&
        map_interactive_touch_authorized();
    if (map_uncovered) {
        d1l_ui_map_viewport_update_truth_context(&s_snapshot);
        (void)d1l_ui_map_viewport_refresh();
    }
    if (map_uncovered && map_render_input_changed_from_rendered(&s_snapshot)) {
        request_content_refresh();
    } else if (content_generation_changed_from_rendered(&s_snapshot)) {
        if (map_active) {
            /* Ambient mesh/message counters do not change the Map surface.
             * A full content render tears down its visible lease and can
             * cancel a bounded tile batch or an in-progress drag, so simply
             * acknowledge these counters here.  Explicit Map changes such as
             * saving/clearing a location still call request_content_refresh(). */
            remember_rendered_content_generation(&s_snapshot);
        } else {
            request_content_refresh();
        }
    }
    if (s_toast && s_toast_until != 0 && lv_tick_get() > s_toast_until) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = 0;
    }
}

static void create_top_bar(lv_obj_t *screen)
{
    lv_obj_t *bar = create_object(screen, "top bar");
    if (!bar) {
        return;
    }
    s_top_bar = bar;
    lv_obj_set_size(bar, 480, 56);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 8, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_title_label = create_label(bar, "DeskOS", 0xF4F7FB);
    lv_obj_align(s_title_label, LV_ALIGN_LEFT_MID, 2, 0);

    s_status_label = create_label(bar, "starting", 0x5EEAD4);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_status_label, 190);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_RIGHT, -48, 1);

    s_identity_label = create_label(bar, "ID --------", 0x8EA0AE);
    lv_label_set_long_mode(s_identity_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_identity_label, 190);
    lv_obj_align(s_identity_label, LV_ALIGN_BOTTOM_RIGHT, -48, -1);

    s_lock_button = create_button(bar, "Lock", 404, 6, 64, 40, lock_event_cb, NULL);
}

static void create_dock(lv_obj_t *screen)
{
    s_dock = create_object(screen, "dock");
    if (!s_dock) {
        return;
    }
    lv_obj_set_size(s_dock, 480, D1L_UI_DOCK_HEIGHT);
    lv_obj_set_pos(s_dock, 0, D1L_UI_DOCK_Y);
    lv_obj_set_style_bg_color(s_dock, lv_color_hex(0x09131D), 0);
    lv_obj_set_style_border_width(s_dock, 0, 0);
    lv_obj_set_style_pad_all(s_dock, 0, 0);
    lv_obj_clear_flag(s_dock, LV_OBJ_FLAG_SCROLLABLE);

    size_t item_count = 0U;
    const d1l_ui_dock_item_t *items = active_dock_items(&item_count);
    for (size_t i = 0; i < item_count; ++i) {
        s_dock_buttons[i] = create_dock_button(
            s_dock, &items[i], 4 + (int)i * 96, dock_event_cb);
    }
    update_dock_selected_state();
}

static void create_toast(lv_obj_t *screen)
{
    s_toast = create_label_object(screen, "toast");
    if (!s_toast) {
        return;
    }
    lv_obj_set_size(s_toast, 420, 42);
    lv_obj_set_pos(s_toast, 30, 366);
    lv_obj_set_style_radius(s_toast, 8, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x12362F), 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0x5EEAD4), 0);
    lv_obj_set_style_pad_all(s_toast, 11, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_hex(0xF4F7FB), 0);
    lv_label_set_text(s_toast, "");
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

static void create_sheet(lv_obj_t *screen)
{
    s_sheet = create_object(screen, "advert sheet");
    if (!s_sheet) {
        return;
    }
    lv_obj_set_size(s_sheet, 440, 160);
    lv_obj_set_pos(s_sheet, 20, 250);
    lv_obj_set_style_radius(s_sheet, 8, 0);
    lv_obj_set_style_bg_color(s_sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(s_sheet, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_sheet, 1, 0);
    lv_obj_set_style_pad_all(s_sheet, 14, 0);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);

    create_label(s_sheet, "Advert", 0xF4F7FB);
    create_button(s_sheet, "Zero Hop", 16, 52, 118, 52, advert_zero_event_cb, NULL);
    create_button(s_sheet, "Flood", 156, 52, 118, 52, advert_flood_event_cb, NULL);
    create_button(s_sheet, "Close", 296, 52, 96, 52, close_sheet_event_cb, NULL);
    d1l_ui_modal_hide(s_sheet);
}

static void create_compose_sheet(lv_obj_t *screen)
{
    s_compose_sheet = create_object(screen, "compose sheet");
    if (!s_compose_sheet) {
        return;
    }
    lv_obj_set_style_radius(s_compose_sheet, 0, 0);
    lv_obj_set_style_bg_color(s_compose_sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(s_compose_sheet, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_compose_sheet, 1, 0);

    s_compose_title = create_label(s_compose_sheet, "Public", 0xF4F7FB);
    lv_obj_set_style_text_font(s_compose_title, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(s_compose_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_compose_title, 210);
    lv_obj_set_pos(s_compose_title, 16, 8);

    s_compose_send_button = create_button(s_compose_sheet, "Send", 252, 8, 62, 44,
                                          send_compose_event_cb, NULL);
    if (s_compose_send_button) {
        lv_obj_set_style_opa(s_compose_send_button, LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_add_state(s_compose_send_button, LV_STATE_DISABLED);
    }
    create_button(s_compose_sheet, "Clear", 322, 8, 62, 44, clear_compose_event_cb, NULL);
    create_button(s_compose_sheet, "Close", 392, 8, 72, 44, close_compose_event_cb, NULL);

    s_compose_textarea = create_textarea(s_compose_sheet, "compose textarea");
    if (!s_compose_textarea) {
        d1l_ui_modal_hide(s_compose_sheet);
        return;
    }
    lv_textarea_set_placeholder_text(s_compose_textarea, "Public message");
    lv_textarea_set_max_length(s_compose_textarea, D1L_MESSAGE_MAX_CHARS);
    lv_textarea_set_one_line(s_compose_textarea, false);
    lv_textarea_set_text(s_compose_textarea, "");
    lv_obj_set_style_radius(s_compose_textarea, 8, 0);
    lv_obj_set_style_bg_color(s_compose_textarea, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_color(s_compose_textarea, lv_color_hex(0x263241), 0);
    lv_obj_set_style_text_color(s_compose_textarea, lv_color_hex(0xF4F7FB), 0);
    lv_obj_set_style_text_color(s_compose_textarea, lv_color_hex(0x8EA0AE), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(
        s_compose_textarea, &d1l_ui_font_symbols_14, 0);
    lv_obj_add_event_cb(s_compose_textarea, compose_textarea_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_compose_counter = create_label(s_compose_sheet, "0/138", 0x8EA0AE);
    lv_label_set_long_mode(s_compose_counter, LV_LABEL_LONG_DOT);

    s_compose_keyboard = create_keyboard(s_compose_sheet, "compose keyboard");
    if (!s_compose_keyboard) {
        d1l_ui_modal_hide(s_compose_sheet);
        return;
    }
    d1l_ui_keyboard_configure_compose(s_compose_keyboard);
    lv_keyboard_set_textarea(s_compose_keyboard, s_compose_textarea);
    layout_compose_sheet_controls();
    lv_obj_add_event_cb(s_compose_keyboard, compose_keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_compose_keyboard, compose_keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    d1l_ui_modal_hide(s_compose_sheet);
}

static void create_public_history_sheet(lv_obj_t *screen)
{
    s_public_history_sheet = create_object(screen, "public history sheet");
    if (!s_public_history_sheet) {
        return;
    }
    lv_obj_set_size(s_public_history_sheet, 448, 328);
    lv_obj_set_pos(s_public_history_sheet, 16, 82);
    lv_obj_set_style_radius(s_public_history_sheet, 8, 0);
    lv_obj_set_style_bg_color(s_public_history_sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(s_public_history_sheet, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_public_history_sheet, 1, 0);
    lv_obj_set_style_pad_all(s_public_history_sheet, 12, 0);
    configure_sheet_scroll(s_public_history_sheet);
    d1l_ui_modal_hide(s_public_history_sheet);
}

static void create_message_detail_sheet(lv_obj_t *screen)
{
    s_message_detail_sheet = create_object(screen, "message detail sheet");
    if (!s_message_detail_sheet) {
        return;
    }
    lv_obj_set_size(s_message_detail_sheet, 480, 424);
    lv_obj_set_pos(s_message_detail_sheet, 0, 56);
    lv_obj_set_style_radius(s_message_detail_sheet, 0, 0);
    lv_obj_set_style_bg_color(s_message_detail_sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_width(s_message_detail_sheet, 0, 0);
    lv_obj_set_style_pad_all(s_message_detail_sheet, 0, 0);
    lv_obj_clear_flag(s_message_detail_sheet, LV_OBJ_FLAG_SCROLLABLE);
    d1l_ui_modal_hide(s_message_detail_sheet);
}

static void create_public_search_sheet(lv_obj_t *screen)
{
    s_public_search_sheet = create_object(screen, "public search sheet");
    if (!s_public_search_sheet) {
        return;
    }
    lv_obj_set_size(s_public_search_sheet, 448, 320);
    lv_obj_set_pos(s_public_search_sheet, 16, 82);
    lv_obj_set_style_radius(s_public_search_sheet, 8, 0);
    lv_obj_set_style_bg_color(s_public_search_sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(s_public_search_sheet, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_public_search_sheet, 1, 0);
    lv_obj_set_style_pad_all(s_public_search_sheet, 12, 0);
    lv_obj_clear_flag(s_public_search_sheet, LV_OBJ_FLAG_SCROLLABLE);

    s_public_search_title = create_label(
        s_public_search_sheet, "Channel Search", 0xF4F7FB);
    lv_obj_set_style_text_font(
        s_public_search_title, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(s_public_search_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_public_search_title, 190);
    lv_obj_set_pos(s_public_search_title, 8, 4);

    create_button(s_public_search_sheet, "Apply", 212, 0, 66, 40,
                  apply_public_search_event_cb, NULL);
    create_button(s_public_search_sheet, "Clear", 286, 0, 64, 40,
                  clear_public_search_event_cb, NULL);
    create_button(s_public_search_sheet, "Close", 358, 0, 66, 40,
                  close_public_search_event_cb, NULL);

    s_public_search_textarea = create_textarea(s_public_search_sheet, "public search textarea");
    if (!s_public_search_textarea) {
        d1l_ui_modal_hide(s_public_search_sheet);
        return;
    }
    lv_obj_set_size(s_public_search_textarea, 424, 48);
    lv_obj_set_pos(s_public_search_textarea, 0, 54);
    lv_textarea_set_one_line(s_public_search_textarea, true);
    lv_textarea_set_max_length(s_public_search_textarea, D1L_MESSAGE_TEXT_LEN - 1U);
    lv_textarea_set_placeholder_text(s_public_search_textarea, "Search author or message");
    lv_obj_set_style_radius(s_public_search_textarea, 8, 0);
    lv_obj_set_style_bg_color(s_public_search_textarea, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_color(s_public_search_textarea, lv_color_hex(0x263241), 0);
    lv_obj_set_style_text_color(s_public_search_textarea, lv_color_hex(0xF4F7FB), 0);
    lv_obj_set_style_text_color(s_public_search_textarea, lv_color_hex(0x8EA0AE),
                                LV_PART_TEXTAREA_PLACEHOLDER);

    s_public_search_keyboard = create_keyboard(s_public_search_sheet, "public search keyboard");
    if (!s_public_search_keyboard) {
        d1l_ui_modal_hide(s_public_search_sheet);
        return;
    }
    d1l_ui_keyboard_configure_input(s_public_search_keyboard, s_public_search_textarea,
                                    0, 114, 424, 194);
    lv_obj_add_event_cb(s_public_search_keyboard, public_search_keyboard_event_cb,
                        LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_public_search_keyboard, public_search_keyboard_event_cb,
                        LV_EVENT_CANCEL, NULL);

    d1l_ui_modal_hide(s_public_search_sheet);
}

static void create_dm_search_sheet(lv_obj_t *screen)
{
    s_dm_search_sheet = create_object(screen, "dm search sheet");
    if (!s_dm_search_sheet) {
        return;
    }
    lv_obj_set_size(s_dm_search_sheet, 448, 320);
    lv_obj_set_pos(s_dm_search_sheet, 16, 82);
    lv_obj_set_style_radius(s_dm_search_sheet, 8, 0);
    lv_obj_set_style_bg_color(s_dm_search_sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(s_dm_search_sheet, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_dm_search_sheet, 1, 0);
    lv_obj_set_style_pad_all(s_dm_search_sheet, 12, 0);
    lv_obj_clear_flag(s_dm_search_sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = create_label(s_dm_search_sheet, "DM Search", 0xF4F7FB);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(title, 8, 4);

    create_button(s_dm_search_sheet, "Apply", 212, 0, 66, 44,
                  apply_dm_search_event_cb, NULL);
    create_button(s_dm_search_sheet, "Clear", 286, 0, 64, 44,
                  clear_dm_search_event_cb, NULL);
    create_button(s_dm_search_sheet, "Close", 358, 0, 66, 44,
                  close_dm_search_event_cb, NULL);

    s_dm_search_textarea = create_textarea(
        s_dm_search_sheet, "dm search textarea");
    if (!s_dm_search_textarea) {
        d1l_ui_modal_hide(s_dm_search_sheet);
        return;
    }
    lv_obj_set_size(s_dm_search_textarea, 424, 48);
    lv_obj_set_pos(s_dm_search_textarea, 0, 58);
    lv_textarea_set_one_line(s_dm_search_textarea, true);
    lv_textarea_set_max_length(
        s_dm_search_textarea, D1L_MESSAGE_TEXT_LEN - 1U);
    lv_textarea_set_placeholder_text(
        s_dm_search_textarea, "Search this conversation");
    lv_obj_set_style_radius(s_dm_search_textarea, 8, 0);
    lv_obj_set_style_bg_color(
        s_dm_search_textarea, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_color(
        s_dm_search_textarea, lv_color_hex(0x263241), 0);
    lv_obj_set_style_text_color(
        s_dm_search_textarea, lv_color_hex(0xF4F7FB), 0);
    lv_obj_set_style_text_color(
        s_dm_search_textarea, lv_color_hex(0x8EA0AE),
        LV_PART_TEXTAREA_PLACEHOLDER);

    s_dm_search_keyboard = create_keyboard(
        s_dm_search_sheet, "dm search keyboard");
    if (!s_dm_search_keyboard) {
        d1l_ui_modal_hide(s_dm_search_sheet);
        return;
    }
    d1l_ui_keyboard_configure_input(
        s_dm_search_keyboard, s_dm_search_textarea, 0, 118, 424, 190);
    lv_obj_add_event_cb(s_dm_search_keyboard, dm_search_keyboard_event_cb,
                        LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_dm_search_keyboard, dm_search_keyboard_event_cb,
                        LV_EVENT_CANCEL, NULL);

    d1l_ui_modal_hide(s_dm_search_sheet);
}

static void create_storage_sheet(lv_obj_t *screen)
{
    s_storage_sheet = create_object(screen, "storage sheet");
    if (!s_storage_sheet) {
        return;
    }
    lv_obj_set_size(s_storage_sheet, 480, 424);
    lv_obj_set_pos(s_storage_sheet, 0, 56);
    lv_obj_set_style_radius(s_storage_sheet, 0, 0);
    lv_obj_set_style_bg_color(s_storage_sheet, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_width(s_storage_sheet, 0, 0);
    lv_obj_set_style_pad_all(s_storage_sheet, 0, 0);
    lv_obj_clear_flag(s_storage_sheet, LV_OBJ_FLAG_SCROLLABLE);
    d1l_ui_modal_hide(s_storage_sheet);
}

static void create_route_detail_sheet(lv_obj_t *screen)
{
    s_route_detail_sheet = create_object(screen, "route detail sheet");
    if (!s_route_detail_sheet) {
        return;
    }
    lv_obj_set_size(s_route_detail_sheet, 448, 250);
    lv_obj_set_pos(s_route_detail_sheet, 16, 132);
    lv_obj_set_style_radius(s_route_detail_sheet, 8, 0);
    lv_obj_set_style_bg_color(s_route_detail_sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(s_route_detail_sheet, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_route_detail_sheet, 1, 0);
    lv_obj_set_style_pad_all(s_route_detail_sheet, 12, 0);
    lv_obj_clear_flag(s_route_detail_sheet, LV_OBJ_FLAG_SCROLLABLE);
    d1l_ui_modal_hide(s_route_detail_sheet);
}

static void create_route_trace_sheet(lv_obj_t *screen)
{
    s_route_trace_sheet = create_object(screen, "route trace sheet");
    if (!s_route_trace_sheet) {
        return;
    }
    lv_obj_set_size(s_route_trace_sheet, 480, 424);
    lv_obj_set_pos(s_route_trace_sheet, 0, 56);
    lv_obj_set_style_radius(s_route_trace_sheet, 0, 0);
    lv_obj_set_style_bg_color(s_route_trace_sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_width(s_route_trace_sheet, 0, 0);
    lv_obj_set_style_pad_all(s_route_trace_sheet, 0, 0);
    lv_obj_set_scroll_dir(s_route_trace_sheet, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(
        s_route_trace_sheet, LV_SCROLLBAR_MODE_AUTO);
    d1l_ui_modal_hide(s_route_trace_sheet);
}

static void create_finder_sheet(lv_obj_t *screen)
{
    s_finder_sheet = create_object(screen, "finder sheet");
    if (!s_finder_sheet) {
        return;
    }
    lv_obj_set_size(s_finder_sheet, 480, 424);
    lv_obj_set_pos(s_finder_sheet, 0, 56);
    lv_obj_set_style_radius(s_finder_sheet, 0, 0);
    lv_obj_set_style_bg_color(
        s_finder_sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_width(s_finder_sheet, 0, 0);
    lv_obj_set_style_pad_all(s_finder_sheet, 0, 0);
    lv_obj_set_scroll_dir(s_finder_sheet, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(
        s_finder_sheet, LV_SCROLLBAR_MODE_AUTO);
    d1l_ui_modal_hide(s_finder_sheet);
}

static void create_packet_detail_sheet(lv_obj_t *screen)
{
    s_packet_detail_sheet = create_object(screen, "packet detail sheet");
    if (!s_packet_detail_sheet) {
        return;
    }
    lv_obj_set_size(s_packet_detail_sheet, 448, 326);
    lv_obj_set_pos(s_packet_detail_sheet, 16, 76);
    lv_obj_set_style_radius(s_packet_detail_sheet, 8, 0);
    lv_obj_set_style_bg_color(s_packet_detail_sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(s_packet_detail_sheet, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_packet_detail_sheet, 1, 0);
    lv_obj_set_style_pad_all(s_packet_detail_sheet, 12, 0);
    lv_obj_set_scrollbar_mode(s_packet_detail_sheet, LV_SCROLLBAR_MODE_AUTO);
    d1l_ui_modal_hide(s_packet_detail_sheet);
}

static void create_packet_search_sheet(lv_obj_t *screen)
{
    s_packet_search_sheet = create_object(screen, "packet search sheet");
    if (!s_packet_search_sheet) {
        return;
    }
    lv_obj_set_size(s_packet_search_sheet, 448, 320);
    lv_obj_set_pos(s_packet_search_sheet, 16, 82);
    lv_obj_set_style_radius(s_packet_search_sheet, 8, 0);
    lv_obj_set_style_bg_color(s_packet_search_sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(s_packet_search_sheet, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_packet_search_sheet, 1, 0);
    lv_obj_set_style_pad_all(s_packet_search_sheet, 12, 0);
    lv_obj_clear_flag(s_packet_search_sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = create_label(s_packet_search_sheet, "Packet Search", 0xF4F7FB);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(title, 8, 4);

    create_button(s_packet_search_sheet, "Apply", 212, 0, 66, 40, apply_packet_search_event_cb, NULL);
    create_button(s_packet_search_sheet, "Clear", 286, 0, 64, 40, clear_packet_search_event_cb, NULL);
    create_button(s_packet_search_sheet, "Close", 358, 0, 66, 40, close_packet_search_event_cb, NULL);

    s_packet_search_textarea = create_textarea(s_packet_search_sheet, "packet search textarea");
    if (!s_packet_search_textarea) {
        d1l_ui_modal_hide(s_packet_search_sheet);
        return;
    }
    lv_obj_set_size(s_packet_search_textarea, 424, 48);
    lv_obj_set_pos(s_packet_search_textarea, 0, 54);
    lv_textarea_set_one_line(s_packet_search_textarea, true);
    lv_textarea_set_max_length(s_packet_search_textarea, D1L_PACKET_LOG_QUERY_TEXT_LEN - 1U);
    lv_textarea_set_placeholder_text(s_packet_search_textarea, "Search kind, note, raw hex");
    lv_obj_set_style_radius(s_packet_search_textarea, 8, 0);
    lv_obj_set_style_bg_color(s_packet_search_textarea, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_color(s_packet_search_textarea, lv_color_hex(0x263241), 0);
    lv_obj_set_style_text_color(s_packet_search_textarea, lv_color_hex(0xF4F7FB), 0);
    lv_obj_set_style_text_color(s_packet_search_textarea, lv_color_hex(0x8EA0AE), LV_PART_TEXTAREA_PLACEHOLDER);

    s_packet_search_keyboard = create_keyboard(s_packet_search_sheet, "packet search keyboard");
    if (!s_packet_search_keyboard) {
        d1l_ui_modal_hide(s_packet_search_sheet);
        return;
    }
    d1l_ui_keyboard_configure_input(s_packet_search_keyboard, s_packet_search_textarea,
                                    0, 114, 424, 194);
    lv_obj_add_event_cb(s_packet_search_keyboard, packet_search_keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_packet_search_keyboard, packet_search_keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    d1l_ui_modal_hide(s_packet_search_sheet);
}

static void create_mesh_roles_sheet(lv_obj_t *screen)
{
    s_mesh_roles_sheet = create_object(screen, "mesh roles sheet");
    if (!s_mesh_roles_sheet) {
        return;
    }
    lv_obj_set_size(s_mesh_roles_sheet, 480, 424);
    lv_obj_set_pos(s_mesh_roles_sheet, 0, 56);
    lv_obj_set_style_radius(s_mesh_roles_sheet, 0, 0);
    lv_obj_set_style_bg_color(s_mesh_roles_sheet, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_width(s_mesh_roles_sheet, 0, 0);
    lv_obj_set_style_pad_all(s_mesh_roles_sheet, 0, 0);
    lv_obj_clear_flag(s_mesh_roles_sheet, LV_OBJ_FLAG_SCROLLABLE);
    d1l_ui_modal_hide(s_mesh_roles_sheet);
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void create_onboarding_sheet(lv_obj_t *screen)
{
    s_onboarding_sheet = create_object(screen, "onboarding sheet");
    if (!s_onboarding_sheet) {
        return;
    }
    lv_obj_set_size(s_onboarding_sheet, 480, 480);
    lv_obj_set_pos(s_onboarding_sheet, 0, 0);
    lv_obj_set_style_radius(s_onboarding_sheet, 0, 0);
    lv_obj_set_style_bg_color(s_onboarding_sheet, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_width(s_onboarding_sheet, 0, 0);
    lv_obj_set_style_pad_all(s_onboarding_sheet, 12, 0);
    lv_obj_clear_flag(s_onboarding_sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = create_label(s_onboarding_sheet, "MeshCore DeskOS D1L", 0xF4F7FB);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(title, 8, 4);

    lv_obj_t *subtitle = create_label(s_onboarding_sheet, "First boot setup", 0x5EEAD4);
    lv_obj_set_pos(subtitle, 8, 38);

    lv_obj_t *name_label = create_label(s_onboarding_sheet, "Node name", 0x8EA0AE);
    lv_obj_set_pos(name_label, 8, 68);

    s_onboarding_name_textarea = create_textarea(s_onboarding_sheet, "onboarding name textarea");
    if (!s_onboarding_name_textarea) {
        d1l_ui_modal_hide(s_onboarding_sheet);
        return;
    }
    lv_obj_set_size(s_onboarding_name_textarea, 424, 48);
    lv_obj_set_pos(s_onboarding_name_textarea, 8, 92);
    lv_textarea_set_one_line(s_onboarding_name_textarea, true);
    lv_textarea_set_max_length(s_onboarding_name_textarea, D1L_NODE_NAME_LEN - 1U);
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    lv_textarea_set_text(s_onboarding_name_textarea, settings.node_name);
    lv_obj_set_style_radius(s_onboarding_name_textarea, 8, 0);
    lv_obj_set_style_bg_color(s_onboarding_name_textarea, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(s_onboarding_name_textarea, lv_color_hex(0x263241), 0);
    lv_obj_set_style_text_color(s_onboarding_name_textarea, lv_color_hex(0xF4F7FB), 0);

    lv_obj_t *preset = create_label(s_onboarding_sheet,
                                    "Canada/USA preset confirmed  910.525 BW62.5 SF7 CR5",
                                    0xE5EDF5);
    lv_label_set_long_mode(preset, LV_LABEL_LONG_DOT);
    lv_obj_set_width(preset, 424);
    lv_obj_set_pos(preset, 8, 152);

    const bool optional_transports_available =
        d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL) ||
        d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE) ||
        d1l_release_feature_available(
            D1L_RELEASE_FEATURE_OBSERVER_MQTT);
    const char *role_text = optional_transports_available ?
        "Role Desk Companion  Wi-Fi off  BLE off  Observer off" :
        "Role Desk Companion  Core radio profile";
    lv_obj_t *role = create_label(s_onboarding_sheet, role_text, 0x8EA0AE);
    lv_label_set_long_mode(role, LV_LABEL_LONG_DOT);
    lv_obj_set_width(role, 424);
    lv_obj_set_pos(role, 8, 180);

    create_button(s_onboarding_sheet, "Start", 8, 210, 116, 40, onboarding_start_event_cb, NULL);
    create_button(s_onboarding_sheet, "Use Defaults", 136, 210, 146, 40,
                  onboarding_defaults_event_cb, NULL);

    s_onboarding_keyboard = create_keyboard(s_onboarding_sheet, "onboarding keyboard");
    if (!s_onboarding_keyboard) {
        d1l_ui_modal_hide(s_onboarding_sheet);
        return;
    }
    d1l_ui_keyboard_configure_input(s_onboarding_keyboard, s_onboarding_name_textarea,
                                    8, 258, 424, 160);
    lv_obj_add_event_cb(s_onboarding_keyboard, onboarding_keyboard_event_cb, LV_EVENT_READY, NULL);

    if (settings.onboarding_complete) {
        s_onboarding_visible = false;
        d1l_ui_modal_hide(s_onboarding_sheet);
    } else {
        s_onboarding_visible = true;
    }
}
#endif

static void create_lock_overlay(lv_obj_t *screen)
{
    s_lock_overlay = create_object(screen, "lock overlay");
    if (!s_lock_overlay) {
        return;
    }
    lv_obj_set_size(s_lock_overlay, 480, 480);
    lv_obj_set_pos(s_lock_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_lock_overlay, lv_color_hex(0x02060A), 0);
    lv_obj_set_style_border_width(s_lock_overlay, 0, 0);
    lv_obj_clear_flag(s_lock_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_lock_overlay, unlock_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = create_label(s_lock_overlay, "MeshCore DeskOS", 0xF4F7FB);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -24);
    lv_obj_t *hint = create_label(s_lock_overlay, "Tap to unlock", 0x5EEAD4);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 24);
    s_lock_visible = false;
    lv_obj_add_flag(s_lock_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void ui_task(void *arg)
{
    (void)arg;
    s_ui_start_result = initialize_ui_runtime();
    if (s_ui_start_result != ESP_OK) {
        fail_ui_start_on_ui_task(s_ui_start_result);
        return;
    }

    /* The fully featured LVGL tree leaves too little internal RAM for this
     * UI-owned stack. The touch task remains LVGL-free and is started only
     * after the complete display/input/tree runtime is ready. */
    if (xTaskCreatePinnedToCoreWithCaps(
            touch_poll_task, "d1l_touch", D1L_TOUCH_TASK_STACK_BYTES,
            NULL, 4, &s_touch_task_handle, D1L_UI_TASK_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_touch_task_handle = NULL;
        fail_ui_start_on_ui_task(ESP_ERR_NO_MEM);
        return;
    }

    d1l_health_monitor_register_ui_task(xTaskGetCurrentTaskHandle());
    d1l_app_model_get()->ui_ready = true;
    s_started = true;
    s_ui_start_result = ESP_OK;
    if (s_ui_start_done_sem) {
        (void)xSemaphoreGive(s_ui_start_done_sem);
    }

    while (true) {
        uint32_t wait_ms = lv_timer_handler();
        d1l_health_monitor_sample_lvgl();
        process_pending_tab_switch();
        process_pending_content_refresh();
#if D1L_ENABLE_QUALIFICATION_HOOKS
        process_pending_scroll_probe();
        process_pending_compose_probe();
#endif
        if (d1l_ui_phase1_tab_switch_pending()) {
            wait_ms = D1L_UI_TIMER_MIN_SLEEP_MS;
        }
        if (wait_ms < D1L_UI_TIMER_MIN_SLEEP_MS) {
            wait_ms = D1L_UI_TIMER_MIN_SLEEP_MS;
        } else if (wait_ms > D1L_UI_TIMER_MAX_SLEEP_MS) {
            wait_ms = D1L_UI_TIMER_MAX_SLEEP_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

static void touch_poll_task(void *arg)
{
    (void)arg;
    while (true) {
        d1l_board_touch_state_t sample = {0};
        if (d1l_board_touch_read(&sample) != ESP_OK) {
            sample.pressed = false;
            sample.coordinate_valid = false;
        }
        publish_touch_state(&sample);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t d1l_ui_phase1_show_home(void)
{
    set_map_interactive_touch_authorized(false);
    d1l_ui_map_viewport_release();
    d1l_ui_modal_reset();
    s_screen = create_screen_object("root screen");
    if (!s_screen) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x071018), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    create_top_bar(s_screen);

    s_content = create_object(s_screen, "content root");
    if (!s_content) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_content, 480, 424);
    lv_obj_set_pos(s_content, 0, 56);
    d1l_ui_screen_configure_content_root(s_content, true);

    create_dock(s_screen);
    if (d1l_ui_settings_action_available(
            D1L_UI_SETTINGS_ACTION_ADVANCED)) {
        create_sheet(s_screen);
    }
    create_compose_sheet(s_screen);
    create_public_history_sheet(s_screen);
    create_public_search_sheet(s_screen);
    create_dm_search_sheet(s_screen);
    create_message_detail_sheet(s_screen);
    if (!d1l_ui_messages_create(&s_messages_controller, s_screen)) {
        return ESP_ERR_NO_MEM;
    }
    if (!d1l_ui_radio_settings_create(
            &s_radio_settings_controller, s_screen)) {
        return ESP_ERR_NO_MEM;
    }
    create_storage_sheet(s_screen);
    if (d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL)) {
        if (!d1l_ui_wifi_create(&s_wifi_controller, s_screen)) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE)) {
        if (!d1l_ui_ble_create(&s_ble_controller, s_screen)) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!d1l_ui_device_sheets_create(&s_device_sheets_controller, s_screen)) {
        return ESP_ERR_NO_MEM;
    }
    if (!d1l_ui_service_sheets_create(
            &s_service_sheets_controller, s_screen)) {
        return ESP_ERR_NO_MEM;
    }
    if (d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP)) {
        if (!d1l_ui_map_sheets_create(&s_map_sheets_controller, s_screen)) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!d1l_ui_contact_sheets_create(
            &s_contact_sheets_controller, s_screen)) {
        return ESP_ERR_NO_MEM;
    }
    if (d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT)) {
        if (!d1l_ui_channel_sheets_create(
                &s_channel_sheets_controller, s_screen)) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!d1l_ui_node_detail_create(&s_node_detail_controller, s_screen)) {
        return ESP_ERR_NO_MEM;
    }
    create_route_detail_sheet(s_screen);
    if (d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE)) {
        create_route_trace_sheet(s_screen);
        create_finder_sheet(s_screen);
    }
    create_packet_detail_sheet(s_screen);
    create_packet_search_sheet(s_screen);
    if (d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN)) {
        create_mesh_roles_sheet(s_screen);
    }
    create_toast(s_screen);
    create_lock_overlay(s_screen);
#if D1L_ENABLE_QUALIFICATION_HOOKS
    create_onboarding_sheet(s_screen);
#else
    if (!d1l_ui_first_start_create(
            &s_first_start_controller, s_screen)) {
        return ESP_ERR_NO_MEM;
    }
    s_onboarding_visible = true;
#endif

    render_active_tab();
    const uint64_t initial_unread =
        (uint64_t)s_snapshot.public_unread_count +
        (uint64_t)s_snapshot.dm_unread_count;
    s_last_notification_unread = initial_unread > UINT32_MAX ?
        UINT32_MAX : (uint32_t)initial_unread;
    if (d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP)) {
        lv_timer_create(map_viewport_timer_cb,
                        D1L_UI_MAP_VIEWPORT_REFRESH_MS, NULL);
    }
    lv_timer_create(refresh_timer_cb,
#if D1L_ENABLE_QUALIFICATION_HOOKS
                    2000,
#else
                    250,
#endif
                    NULL);
    lv_timer_create(display_runtime_timer_cb, 100, NULL);
    lv_scr_load(s_screen);
    return ESP_OK;
}

static esp_err_t initialize_ui_runtime(void)
{
    lv_init();
    d1l_ui_packets_init(&s_packets_controller);
#if D1L_ENABLE_QUALIFICATION_HOOKS
    ESP_RETURN_ON_ERROR(init_capture_buffers(), TAG,
                        "capture buffer init failed");
#endif

    const size_t buffer_pixels =
        D1L_UI_DISPLAY_WIDTH * D1L_UI_DISPLAY_HEIGHT;
#if CONFIG_LCD_LVGL_DIRECT_MODE
    void *fb1 = NULL;
    void *fb2 = NULL;
    bsp_lcd_get_frame_buffer(&fb1, &fb2);
    s_buf1 = (lv_color_t *)fb1;
    s_buf2 = (lv_color_t *)fb2;
#else
    s_buf1 = heap_caps_malloc(
        buffer_pixels * sizeof(lv_color_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_buf2 = heap_caps_malloc(
        buffer_pixels * sizeof(lv_color_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!s_buf1 || !s_buf2) {
        return ESP_ERR_NO_MEM;
    }

    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, buffer_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = D1L_UI_DISPLAY_WIDTH;
    s_disp_drv.ver_res = D1L_UI_DISPLAY_HEIGHT;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
#if CONFIG_LCD_LVGL_DIRECT_MODE
    s_disp_drv.direct_mode = 1;
    bsp_lcd_flush_is_last_register(lcd_flush_is_last_cb);
    bsp_lcd_direct_mode_register(lcd_direct_mode_copy_cb);
#endif
    s_bsp_flush_ready_registered =
        (bsp_lcd_set_cb(lcd_flush_ready_cb, &s_disp_drv) == ESP_OK);
    if (!s_bsp_flush_ready_registered) {
        ESP_LOGW(
            TAG,
            "BSP LCD flush callback registration failed; falling back to immediate ready");
    }
    lv_disp_drv_register(&s_disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    ESP_RETURN_ON_ERROR(d1l_ui_phase1_show_home(), TAG,
                        "home screen failed");

    const esp_timer_create_args_t tick_args = {
        .callback = &lv_tick_task,
        .name = "lvgl_tick",
    };
    ESP_RETURN_ON_ERROR(
        esp_timer_create(&tick_args, &s_lv_tick_timer),
        TAG, "LVGL tick timer creation failed");
    esp_err_t ret = esp_timer_start_periodic(s_lv_tick_timer, 5000);
    if (ret != ESP_OK) {
        (void)esp_timer_delete(s_lv_tick_timer);
        s_lv_tick_timer = NULL;
        return ret;
    }

    d1l_health_monitor_set_lvgl_ready(true);
    return ESP_OK;
}

static void fail_ui_start_on_ui_task(esp_err_t result)
{
    d1l_health_monitor_register_ui_task(NULL);
    if (s_lv_tick_timer) {
        (void)esp_timer_stop(s_lv_tick_timer);
        (void)esp_timer_delete(s_lv_tick_timer);
        s_lv_tick_timer = NULL;
    }
    d1l_health_monitor_set_lvgl_ready(false);
    d1l_app_model_get()->ui_ready = false;
    s_touch_task_handle = NULL;
    s_ui_task_handle = NULL;
    s_started = false;
    s_ui_start_result = result;
    if (s_ui_start_done_sem) {
        (void)xSemaphoreGive(s_ui_start_done_sem);
    }
    vTaskDelete(NULL);
}

esp_err_t d1l_ui_phase1_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
#if D1L_ENABLE_QUALIFICATION_HOOKS
    if (!s_scroll_probe_done_sem) {
        s_scroll_probe_done_sem = xSemaphoreCreateBinary();
        if (!s_scroll_probe_done_sem) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_compose_probe_done_sem) {
        s_compose_probe_done_sem = xSemaphoreCreateBinary();
        if (!s_compose_probe_done_sem) {
            return ESP_ERR_NO_MEM;
        }
    }
#endif
    if (!s_ui_start_done_sem) {
        s_ui_start_done_sem = xSemaphoreCreateBinary();
        if (!s_ui_start_done_sem) {
            return ESP_ERR_NO_MEM;
        }
    }
    while (xSemaphoreTake(s_ui_start_done_sem, 0) == pdTRUE) {
    }
    s_ui_start_result = ESP_ERR_INVALID_STATE;

    if (xTaskCreatePinnedToCoreWithCaps(
            ui_task, "d1l_ui", D1L_UI_TASK_STACK_BYTES, NULL, 5,
            &s_ui_task_handle, D1L_UI_TASK_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_ui_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    (void)xSemaphoreTake(s_ui_start_done_sem, portMAX_DELAY);
    const esp_err_t start_result = s_ui_start_result;
    return start_result;
}
