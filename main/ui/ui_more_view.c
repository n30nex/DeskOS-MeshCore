#include "ui_more_view.h"

#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"

enum {
    COLOR_TEXT = 0xF4F7FB,
    COLOR_MUTED = 0xA6B0B7,
    COLOR_GREEN = 0x20D9ED,
    COLOR_AMBER = 0xFBBF24,
    COLOR_RED = 0xF87171,
    COLOR_BLUE = 0x4D7FFF,
    COLOR_DISPLAY = 0x84FF2E,
    COLOR_VIOLET = 0x7D93FF,
    COLOR_WARNING_TEXT = 0xFCA5A5,
};

_Static_assert(sizeof(d1l_ui_more_view_model_t) <= D1L_UI_MORE_VIEW_MODEL_MAX_BYTES,
               "More view model exceeded its persistent-owner size budget");

static bool text_equals(const char *value, const char *expected)
{
    return value && expected && strcmp(value, expected) == 0;
}

void d1l_ui_more_view_apply_release_profile(
    d1l_ui_more_view_input_t *input)
{
    if (!input) {
        return;
    }
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL)) {
        input->wifi_build_enabled = false;
        input->wifi_enabled = false;
        input->wifi_connected = false;
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
        input->map_tile_render_supported = false;
    }
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_SD_HISTORY)) {
        input->storage_sd_present = false;
        input->storage_sd_data_root_ready = false;
        input->storage_sd_needs_fat32 = false;
        input->storage_setup_required = false;
        input->storage_retained_sd_degraded = false;
        input->storage_sd_state = "internal";
        input->storage_setup_action = "forced_nvs";
    }
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (!destination || capacity == 0U) {
        return;
    }
    snprintf(destination, capacity, "%s", source ? source : "");
}

static bool bounded_text_is_terminated(const char *text, size_t capacity)
{
    return text && capacity > 0U && memchr(text, '\0', capacity) != NULL;
}

static bool sd_needs_attention(const d1l_ui_more_view_input_t *input)
{
    return input && (input->storage_retained_sd_degraded ||
           text_equals(input->storage_sd_state, "error") ||
           text_equals(input->storage_sd_state, "bridge_reported") ||
           text_equals(input->storage_setup_action,
                       "inspect_rp2040_sd_cmd0_firmware_path") ||
           text_equals(input->storage_setup_action,
                       "inspect_rp2040_sd_mount_error_firmware_path"));
}

static const char *map_storage_state(const d1l_ui_more_view_input_t *input)
{
    if (sd_needs_attention(input)) {
        return "Check SD";
    }
    if (text_equals(input->storage_setup_action, "insert_card")) {
        return "Insert SD";
    }
    if (input->storage_sd_needs_fat32) {
        return "Needs FAT32";
    }
    return "SD starting";
}

static void set_item(d1l_ui_more_item_view_t *item,
                     const char *title,
                     const char *status,
                     uint32_t accent,
                     d1l_ui_settings_action_t action,
                     bool warning)
{
    if (!item) {
        return;
    }
    copy_text(item->title, sizeof(item->title), title);
    copy_text(item->status, sizeof(item->status), status);
    item->accent = accent;
    item->action = action;
    item->warning = warning;
    item->actionable = action > D1L_UI_SETTINGS_ACTION_NONE &&
        action < D1L_UI_SETTINGS_ACTION_COUNT;
}

static d1l_ui_more_category_view_t *set_category(
    d1l_ui_more_view_model_t *view,
    d1l_ui_more_category_t category,
    const char *title,
    const char *summary,
    uint32_t accent,
    bool warning,
    size_t item_count)
{
    if (!view || category < D1L_UI_MORE_CATEGORY_TOOLS ||
        category >= D1L_UI_MORE_CATEGORY_COUNT || item_count == 0U ||
        item_count > D1L_UI_MORE_MAX_ITEMS_PER_CATEGORY) {
        return NULL;
    }
    d1l_ui_more_category_view_t *out = &view->categories[category];
    out->category = category;
    copy_text(out->title, sizeof(out->title), title);
    copy_text(out->summary, sizeof(out->summary), summary);
    out->accent = accent;
    out->warning = warning;
    out->item_count = item_count;
    return out;
}

bool d1l_ui_more_view_model_is_valid(const d1l_ui_more_view_model_t *view_model)
{
    if (!view_model ||
        !bounded_text_is_terminated(view_model->title, sizeof(view_model->title)) ||
        !bounded_text_is_terminated(view_model->subtitle, sizeof(view_model->subtitle)) ||
        view_model->title[0] == '\0' || view_model->subtitle[0] == '\0' ||
        view_model->category_count != D1L_UI_MORE_CATEGORY_COUNT) {
        return false;
    }
    for (size_t category_index = 0U;
         category_index < view_model->category_count; ++category_index) {
        const d1l_ui_more_category_view_t *category =
            &view_model->categories[category_index];
        if (category->category != (d1l_ui_more_category_t)category_index ||
            !bounded_text_is_terminated(category->title, sizeof(category->title)) ||
            !bounded_text_is_terminated(category->summary, sizeof(category->summary)) ||
            category->title[0] == '\0' || category->summary[0] == '\0' ||
            category->item_count == 0U ||
            category->item_count > D1L_UI_MORE_MAX_ITEMS_PER_CATEGORY) {
            return false;
        }
        for (size_t item_index = 0U;
             item_index < D1L_UI_MORE_MAX_ITEMS_PER_CATEGORY; ++item_index) {
            const d1l_ui_more_item_view_t *item = &category->items[item_index];
            if (!bounded_text_is_terminated(item->title, sizeof(item->title)) ||
                !bounded_text_is_terminated(item->status, sizeof(item->status))) {
                return false;
            }
            if (item_index >= category->item_count) {
                continue;
            }
            const bool action_valid = item->action > D1L_UI_SETTINGS_ACTION_NONE &&
                item->action < D1L_UI_SETTINGS_ACTION_COUNT;
            if (item->title[0] == '\0' || item->status[0] == '\0' ||
                item->action < D1L_UI_SETTINGS_ACTION_NONE ||
                item->action >= D1L_UI_SETTINGS_ACTION_COUNT ||
                item->actionable != action_valid) {
                return false;
            }
        }
    }
    return true;
}

bool d1l_ui_more_view(const d1l_ui_more_view_input_t *input,
                      d1l_ui_more_view_model_t *out_view)
{
    if (!input || !out_view) {
        return false;
    }
    memset(out_view, 0, sizeof(*out_view));
    copy_text(out_view->title, sizeof(out_view->title), "Tools");
    copy_text(out_view->subtitle, sizeof(out_view->subtitle), "Settings and utilities");
    out_view->category_count = D1L_UI_MORE_CATEGORY_COUNT;

    char packet_status[D1L_UI_MORE_STATUS_TEXT_LEN];
    char terminal_status[D1L_UI_MORE_STATUS_TEXT_LEN];
    char notification_status[D1L_UI_MORE_STATUS_TEXT_LEN];
    char about_status[D1L_UI_MORE_STATUS_TEXT_LEN];
    char display_status[D1L_UI_MORE_STATUS_TEXT_LEN];
    snprintf(packet_status, sizeof(packet_status), "%llu saved",
             (unsigned long long)input->packet_count);
    snprintf(terminal_status, sizeof(terminal_status), "%lu events",
             (unsigned long)input->terminal_event_count);
    snprintf(notification_status, sizeof(notification_status), "%lu unread",
             (unsigned long)input->notification_unread_count);
    snprintf(about_status, sizeof(about_status), "Version %s",
             input->firmware_version && input->firmware_version[0] != '\0'
                 ? input->firmware_version : "unknown");
    if (!input->timezone_settings_ready) {
        snprintf(display_status, sizeof(display_status),
                 "Time setting unavailable");
    } else {
        snprintf(display_status, sizeof(display_status), "%s / %s",
                 input->timezone_label && input->timezone_label[0] != '\0' ?
                     input->timezone_label : "UTC",
                 input->time_available && input->time_label &&
                         input->time_label[0] != '\0' ?
                     input->time_label : "syncing");
    }

    const char *wifi_status = !input->wifi_build_enabled ? "Unavailable" :
        (input->wifi_connected ? "Connected" :
         (input->wifi_connecting ? "Connecting" :
          (input->wifi_enabled ? "On" : "Off")));
    const char *ble_status = !input->ble_build_enabled ||
        !input->ble_transport_supported ? "Unavailable" :
        (input->ble_companion_enabled ? "On" : "Off");
    const char *radio_status = input->radio_apply_pending ? "Applying" :
        ((input->radio_ready || input->radio_applied) ? "Ready" : "Needs setup");
    const bool sd_attention = sd_needs_attention(input);
    const bool storage_attention = input->storage_retained_backup_degraded ||
        sd_attention;
    const bool storage_reconnecting = text_equals(
        input->storage_setup_action, "wait_for_storage_reconnect");
    const bool storage_ready = input->storage_data_enabled ||
        input->storage_sd_data_root_ready;
    const char *storage_status = storage_reconnecting ? "Reconnecting" :
        (storage_attention ? "Needs attention" :
         (storage_ready ? "Ready" :
          (input->storage_setup_required ? "Needs setup" :
           (input->storage_sd_present ? "Detected" : "Internal storage"))));
    const char *map_status = !input->map_location_set ? "Set location" :
        (!input->map_tile_cache_ready ? map_storage_state(input) :
         (!input->wifi_connected ? "Needs Wi-Fi" :
          (input->map_tile_render_supported ? "Ready" : "Loading")));
    const char *storage_summary = input->storage_retained_backup_degraded &&
        sd_attention ? "Storage needs attention" :
        (input->storage_retained_backup_degraded ? "Backup needs attention" :
         (sd_attention ? "SD needs attention" :
          (storage_reconnecting ? "SD reconnecting" : "SD card and map")));

    d1l_ui_more_category_view_t *category = set_category(
        out_view, D1L_UI_MORE_CATEGORY_TOOLS, "Tools",
        "Packets, diagnostics, and terminal", COLOR_BLUE, false, 3U);
    set_item(&category->items[0], "Packets", packet_status, COLOR_BLUE,
             D1L_UI_SETTINGS_ACTION_PACKETS, false);
    set_item(&category->items[1], "Diagnostics", "Health & reports", COLOR_VIOLET,
             D1L_UI_SETTINGS_ACTION_DIAGNOSTICS, false);
    set_item(&category->items[2], "Terminal", terminal_status, COLOR_TEXT,
             D1L_UI_SETTINGS_ACTION_TERMINAL, false);

    category = set_category(out_view, D1L_UI_MORE_CATEGORY_CONNECTIONS,
                            "Connections", "Wi-Fi, Bluetooth, and observer",
                            COLOR_GREEN, false, 3U);
    set_item(&category->items[0], "Wi-Fi", wifi_status,
             input->wifi_connected ? COLOR_GREEN : COLOR_TEXT,
             D1L_UI_SETTINGS_ACTION_WIFI, false);
    set_item(&category->items[1], "Bluetooth", ble_status,
             input->ble_companion_enabled ? COLOR_GREEN : COLOR_TEXT,
             D1L_UI_SETTINGS_ACTION_BLE, false);
    set_item(&category->items[2], "Observer",
             input->observer_state && input->observer_state[0] ?
                 input->observer_state : "Not configured",
             text_equals(input->observer_state, "connected") ?
                 COLOR_GREEN : COLOR_TEXT,
             D1L_UI_SETTINGS_ACTION_OBSERVER, false);

    category = set_category(out_view, D1L_UI_MORE_CATEGORY_STORAGE_MAPS,
                            "Storage & maps", storage_summary,
                            storage_attention ? COLOR_WARNING_TEXT : COLOR_AMBER,
                            storage_attention, 3U);
    set_item(&category->items[0], "SD Card", storage_status,
             storage_attention ? COLOR_RED :
             (storage_reconnecting ? COLOR_AMBER :
              (storage_ready ? COLOR_GREEN : COLOR_TEXT)),
             D1L_UI_SETTINGS_ACTION_STORAGE, storage_attention);
    set_item(&category->items[1], "Map options", map_status,
             input->map_tile_cache_ready ? COLOR_GREEN : COLOR_TEXT,
             D1L_UI_SETTINGS_ACTION_MAP_TILES, false);
    set_item(&category->items[2], "Signed update",
             input->update_state && input->update_state[0] ?
                 input->update_state : "Idle",
             text_equals(input->update_state, "reboot_required") ?
                 COLOR_GREEN : COLOR_TEXT,
             D1L_UI_SETTINGS_ACTION_UPDATE, false);

    category = set_category(out_view, D1L_UI_MORE_CATEGORY_DEVICE,
                            "Device", "Display, notifications, identity",
                            COLOR_BLUE, false, 3U);
    set_item(&category->items[0], "Display", display_status, COLOR_DISPLAY,
             D1L_UI_SETTINGS_ACTION_DISPLAY, false);
    set_item(&category->items[1], "Notifications", notification_status,
             input->notification_unread_count ? COLOR_AMBER : COLOR_GREEN,
             D1L_UI_SETTINGS_ACTION_NOTIFICATIONS, false);
    set_item(&category->items[2], "Identity",
             input->identity_ready ? "Ready" : "Not set", COLOR_TEXT,
             D1L_UI_SETTINGS_ACTION_NONE, false);

    category = set_category(out_view, D1L_UI_MORE_CATEGORY_SUPPORT,
                            "Support", "About this device", COLOR_VIOLET,
                            false, 1U);
    set_item(&category->items[0], "About", about_status, COLOR_TEXT,
             D1L_UI_SETTINGS_ACTION_NONE, false);

    category = set_category(out_view, D1L_UI_MORE_CATEGORY_ADVANCED,
                            "Advanced", "Radio and authenticated server tools",
                            COLOR_WARNING_TEXT, false, 3U);
    set_item(&category->items[0], "Radio", radio_status,
             input->radio_ready ? COLOR_GREEN : COLOR_TEXT,
             D1L_UI_SETTINGS_ACTION_RADIO, false);
    set_item(&category->items[1], "Server admin",
             input->admin_state && input->admin_state[0] ?
                 input->admin_state : "Idle",
             text_equals(input->admin_state, "authenticated") ?
                 COLOR_GREEN : COLOR_TEXT,
             D1L_UI_SETTINGS_ACTION_ADMIN, false);
    set_item(&category->items[2], "Share this node", "Let nearby devices find you",
             COLOR_WARNING_TEXT, D1L_UI_SETTINGS_ACTION_ADVANCED, false);

    return d1l_ui_more_view_model_is_valid(out_view);
}
