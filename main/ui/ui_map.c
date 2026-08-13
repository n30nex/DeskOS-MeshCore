#include "ui_map.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "map/map_marker_truth.h"
#include "map/map_point_projection.h"
#include "map/map_prefetch_service.h"
#include "map/map_tile_provider.h"
#include "map/map_view_service.h"
#include "map/map_viewport_truth_transition.h"
#include "mesh/node_store.h"
#include "ui_chrome.h"
#include "ui_keyboard.h"
#include "ui_modal.h"

#define MAP_COLOR_BG 0x17191AU
#define MAP_COLOR_PANEL 0x20262BU
#define MAP_COLOR_PANEL_PRESSED 0x182533U
#define MAP_COLOR_BORDER 0x33404AU
#define MAP_COLOR_TEXT 0xF4F7FBU
#define MAP_COLOR_MUTED 0xA6B0B7U
#define MAP_COLOR_DETAIL 0xD5E0E9U
#define MAP_COLOR_GOOD 0x20D9EDU
#define MAP_COLOR_INFO 0x4D7FFFU
#define MAP_COLOR_WARN 0xFBBF24U
#define MAP_VIEWPORT_WIDTH 478U
#define MAP_VIEWPORT_HEIGHT (D1L_UI_DOCKED_CONTENT_HEIGHT - 2U)
#define MAP_VIEWPORT_PANEL_HEIGHT (MAP_VIEWPORT_HEIGHT + 2U)
#define MAP_DRAG_THRESHOLD_PIXELS 24
#define MAP_DRAG_MAX_X_PIXELS ((int32_t)MAP_VIEWPORT_WIDTH / 3)
#define MAP_DRAG_MAX_Y_PIXELS ((int32_t)MAP_VIEWPORT_HEIGHT / 3)
#define MAP_MARKER_QUERY_LIMIT D1L_NODE_SD_HISTORY_CAPACITY
#define MAP_MARKER_LABEL_LIMIT 8U
#define MAP_MARKER_DOT_DIAMETER 14
#define MAP_MARKER_HIT_RADIUS 22
#define MAP_MARKER_LABEL_WIDTH 112
#define MAP_MARKER_LABEL_HEIGHT 38
#define MAP_MARKER_LABEL_GAP 3
#define MAP_MARKER_NAME_MAX_CHARS 14U

_Static_assert(sizeof(d1l_ui_map_sheets_controller_t) <=
                   D1L_UI_MAP_SHEETS_CONTROLLER_MAX_BYTES,
               "Map sheets controller exceeded its persistent-owner size budget");
_Static_assert(MAP_VIEWPORT_PANEL_HEIGHT == D1L_UI_DOCKED_CONTENT_HEIGHT,
               "Map viewport panel must fill the docked content root");

typedef struct {
    int16_t x1;
    int16_t y1;
    int16_t x2;
    int16_t y2;
} d1l_ui_map_marker_rect_t;

typedef struct {
    int16_t screen_x;
    int16_t screen_y;
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
} d1l_ui_map_marker_hit_t;

static uint32_t s_viewport_generation;
static uint32_t s_viewport_revision;
static uint32_t s_viewport_suppression_depth;
static bool s_viewport_suppress_next_acquire;
#if D1L_ENABLE_QUALIFICATION_HOOKS
static bool s_viewport_acceptance_reset_pending;
#endif
static bool s_viewport_allocation_failed;
static portMUX_TYPE s_viewport_lease_lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t *s_viewport_pixels;
static lv_img_dsc_t s_viewport_image;
static lv_obj_t *s_viewport_obj;
static lv_obj_t *s_viewport_image_obj;
static lv_obj_t *s_viewport_marker_layer;
static lv_obj_t *s_viewport_status_label;
static lv_obj_t *s_viewport_detail_label;
static lv_obj_t *s_viewport_readiness_label;
static lv_obj_t *s_viewport_attribution_label;
static lv_obj_t *s_viewport_drag_hint_label;
static lv_obj_t *s_viewport_progress_bar;
static lv_obj_t *s_viewport_zoom_label;
static lv_obj_t *s_viewport_zoom_in_button;
static lv_obj_t *s_viewport_zoom_out_button;
static lv_obj_t *s_viewport_options_button;
static lv_obj_t *s_viewport_center_button;
static lv_obj_t *s_viewport_center_source_label;
static lv_obj_t *s_viewport_pin_truth_label;
static d1l_ui_map_open_node_detail_cb_t s_viewport_open_node_detail;
static d1l_node_marker_t
    s_viewport_marker_candidates[MAP_MARKER_QUERY_LIMIT] EXT_RAM_BSS_ATTR;
static d1l_ui_map_marker_hit_t
    s_viewport_marker_hits[MAP_MARKER_QUERY_LIMIT] EXT_RAM_BSS_ATTR;
static d1l_ui_map_marker_rect_t
    s_viewport_marker_bounds[MAP_MARKER_LABEL_LIMIT];
static size_t s_viewport_marker_count;
static size_t s_viewport_marker_label_count;
static uint32_t s_viewport_marker_store_generation;
static uint32_t s_viewport_marker_frame_generation;
static uint32_t s_viewport_marker_context_generation;
static uint32_t s_viewport_marker_rendered_context_generation;
static uint32_t s_viewport_marker_reference_timestamp;
static bool s_viewport_marker_age_reference_valid;
static d1l_map_center_source_t s_viewport_center_source;
static bool s_viewport_center_trusted_for_acquire;
static bool s_viewport_position_initialized;
static int32_t s_viewport_saved_lat_e7;
static int32_t s_viewport_saved_lon_e7;
static int32_t s_viewport_lat_e7;
static int32_t s_viewport_lon_e7;
static uint8_t s_viewport_zoom = D1L_MAP_VIEW_DEFAULT_ZOOM;
static bool s_viewport_drag_active;
static int32_t s_viewport_drag_x;
static int32_t s_viewport_drag_y;

static lv_obj_t *map_label(lv_obj_t *parent, const char *text, uint32_t color)
{
    if (!parent || !text) {
        return NULL;
    }
    lv_obj_t *label = lv_label_create(parent);
    if (!label) {
        return NULL;
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void map_label_dot(lv_obj_t *label, int width)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
}

static void map_label_wrap(lv_obj_t *label, int width)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width);
}

static lv_obj_t *map_panel(lv_obj_t *parent, int x, int y, int width, int height)
{
    if (!parent) {
        return NULL;
    }
    lv_obj_t *panel = lv_obj_create(parent);
    if (!panel) {
        return NULL;
    }
    lv_obj_set_size(panel, width, height);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(MAP_COLOR_PANEL), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(MAP_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t *map_button(lv_obj_t *parent, const char *text,
                            int x, int y, int width, int height,
                            lv_event_cb_t callback)
{
    if (!parent || !text) {
        return NULL;
    }
    lv_obj_t *button = lv_btn_create(parent);
    if (!button) {
        return NULL;
    }
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 9, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x252D33), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2E3A43), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = map_label(button, text, MAP_COLOR_TEXT);
    if (label) {
        lv_obj_center(label);
    }
    if (callback) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    }
    return button;
}

static void map_style_primary_button(lv_obj_t *button)
{
    if (!button) {
        return;
    }
    lv_obj_set_style_bg_color(button, lv_color_hex(0x23321A), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x174A40), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(MAP_COLOR_GOOD), 0);
    lv_obj_set_style_border_width(button, 1, 0);
}

static void map_style_overlay_button(lv_obj_t *button)
{
    if (!button) {
        return;
    }
    lv_obj_set_style_bg_opa(button, LV_OPA_90, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(MAP_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(button, 1, 0);
}

static void map_render_header(lv_obj_t *parent, const char *title,
                              const char *subtitle, const char *back_label,
                              lv_event_cb_t back_callback)
{
    if (!parent) {
        return;
    }
    int title_x = 16;
    if (back_label && back_callback) {
        const int back_width = strcmp(back_label, "Back to Map") == 0 ? 120 : 80;
        map_button(parent, back_label, 16, 8, back_width, 44, back_callback);
        title_x = 16 + back_width + 16;
    }
    lv_obj_t *title_label = map_label(parent, title, MAP_COLOR_TEXT);
    if (title_label) {
        lv_obj_set_style_text_font(title_label, &lv_font_montserrat_24, 0);
        map_label_dot(title_label, 480 - title_x - 16);
        lv_obj_set_pos(title_label, title_x, 6);
    }
    lv_obj_t *subtitle_label = map_label(parent, subtitle ? subtitle : "",
                                         MAP_COLOR_MUTED);
    if (subtitle_label) {
        map_label_dot(subtitle_label, 480 - title_x - 16);
        lv_obj_set_pos(subtitle_label, title_x, 34);
    }
}

static const char *map_location_status(const d1l_app_snapshot_t *snapshot)
{
    if (!snapshot || !snapshot->map_location_set) {
        return "Not set";
    }
    return d1l_map_center_source_label(snapshot->map_center_source);
}

static bool map_center_available(const d1l_app_snapshot_t *snapshot)
{
    return snapshot && snapshot->map_location_set &&
        d1l_map_center_source_is_trusted(snapshot->map_center_source);
}

static const char *map_wifi_status(const d1l_app_snapshot_t *snapshot)
{
    if (!snapshot) {
        return "Unavailable";
    }
    if (snapshot->wifi_connected) {
        return "Connected";
    }
    if (snapshot->wifi_connecting) {
        return "Connecting";
    }
    return snapshot->wifi_enabled ? "Not connected" : "Off";
}

static bool map_storage_needs_attention(const d1l_app_snapshot_t *snapshot)
{
    return snapshot &&
           (snapshot->storage_retained_sd_degraded ||
            (snapshot->storage_sd_state &&
             (strcmp(snapshot->storage_sd_state, "error") == 0 ||
              strcmp(snapshot->storage_sd_state, "bridge_reported") == 0)) ||
            (snapshot->storage_setup_action &&
             (strcmp(snapshot->storage_setup_action,
                     "inspect_rp2040_sd_cmd0_firmware_path") == 0 ||
              strcmp(snapshot->storage_setup_action,
                     "inspect_rp2040_sd_mount_error_firmware_path") == 0)));
}

static bool map_storage_is_starting(const d1l_app_snapshot_t *snapshot)
{
    if (!snapshot || !snapshot->storage_setup_action) {
        return false;
    }
    const char *action = snapshot->storage_setup_action;
    return strcmp(action, "bridge_unavailable") == 0 ||
           strcmp(action, "bridge_protocol_pending") == 0 ||
           strcmp(action, "wait_for_storage_mount") == 0 ||
           strcmp(action, "run_storage_mount") == 0 ||
           strcmp(action, "wait_for_storage_reconnect") == 0;
}

static const char *map_sd_status(const d1l_app_snapshot_t *snapshot)
{
    if (!snapshot) {
        return "Unavailable";
    }
    if (map_storage_needs_attention(snapshot)) {
        return "Error";
    }
    if (snapshot->storage_sd_needs_fat32) {
        return "Needs FAT32";
    }
    if (snapshot->map_tile_cache_ready) {
        return "Ready";
    }
    if (snapshot->storage_setup_action &&
        strcmp(snapshot->storage_setup_action, "insert_card") == 0) {
        return "Not inserted";
    }
    if (map_storage_is_starting(snapshot)) {
        return "Starting";
    }
    if (!snapshot->storage_sd_present) {
        return "Not inserted";
    }
    return snapshot->storage_sd_mounted ? "Preparing" : "Not mounted";
}

static uint8_t map_zoom_clamp(uint8_t zoom)
{
    d1l_map_tile_provider_t provider = {0};
    d1l_map_tile_provider_snapshot(&provider);
    uint8_t maximum = D1L_MAP_VIEW_MAX_ZOOM;
    if (provider.max_zoom >= D1L_MAP_VIEW_MIN_ZOOM &&
        provider.max_zoom < maximum) {
        maximum = provider.max_zoom;
    }
    if (zoom < D1L_MAP_VIEW_MIN_ZOOM) {
        return D1L_MAP_VIEW_MIN_ZOOM;
    }
    if (zoom > maximum) {
        return maximum;
    }
    return zoom;
}

static int32_t map_drag_clamp(int32_t value, int32_t limit)
{
    if (value < -limit) {
        return -limit;
    }
    if (value > limit) {
        return limit;
    }
    return value;
}

static void map_viewport_sync_position(const d1l_app_snapshot_t *snapshot)
{
    if (!map_center_available(snapshot)) {
        s_viewport_position_initialized = false;
        return;
    }
    if (s_viewport_position_initialized &&
        s_viewport_saved_lat_e7 == snapshot->map_lat_e7 &&
        s_viewport_saved_lon_e7 == snapshot->map_lon_e7) {
        return;
    }
    s_viewport_saved_lat_e7 = snapshot->map_lat_e7;
    s_viewport_saved_lon_e7 = snapshot->map_lon_e7;
    s_viewport_lat_e7 = snapshot->map_lat_e7;
    s_viewport_lon_e7 = snapshot->map_lon_e7;
    s_viewport_zoom = map_zoom_clamp(snapshot->map_tile_zoom);
    s_viewport_position_initialized = true;
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void map_viewport_apply_acceptance_reset(
    const d1l_app_snapshot_t *snapshot)
{
    bool pending;
    portENTER_CRITICAL(&s_viewport_lease_lock);
    pending = s_viewport_acceptance_reset_pending;
    s_viewport_acceptance_reset_pending = false;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    if (!pending || !map_center_available(snapshot)) {
        return;
    }
    s_viewport_saved_lat_e7 = snapshot->map_lat_e7;
    s_viewport_saved_lon_e7 = snapshot->map_lon_e7;
    s_viewport_lat_e7 = snapshot->map_lat_e7;
    s_viewport_lon_e7 = snapshot->map_lon_e7;
    s_viewport_zoom = map_zoom_clamp(snapshot->map_tile_zoom);
    s_viewport_position_initialized = true;
}
#endif

static bool map_text_contains(const char *text, const char *needle)
{
    return text && needle && strstr(text, needle) != NULL;
}

static void map_viewport_copy(const d1l_app_snapshot_t *snapshot,
                              const char **title, const char **detail,
                              uint32_t *color)
{
    if (!snapshot || !title || !detail || !color) {
        return;
    }
    const char *download_state = snapshot->map_tile_download_state;
    if (map_storage_needs_attention(snapshot) ||
        map_text_contains(download_state, "error") ||
        map_text_contains(download_state, "failed")) {
        *title = "Map unavailable";
        *detail = "Check the SD card, then reopen Map.";
        *color = MAP_COLOR_WARN;
    } else if (!snapshot->map_location_set) {
        *title = "Set a location";
        *detail = "Open Options to choose the area shown here.";
        *color = MAP_COLOR_WARN;
    } else if (!map_center_available(snapshot)) {
        *title = "Center unavailable";
        *detail = "Map center provenance is unknown; save it again.";
        *color = MAP_COLOR_WARN;
    } else if (!snapshot->map_tile_cache_ready) {
        if (snapshot->storage_setup_action &&
            strcmp(snapshot->storage_setup_action, "insert_card") == 0) {
            *title = "SD card required";
            *detail = "Insert a FAT32 card to save and load map tiles.";
        } else if (snapshot->storage_sd_needs_fat32) {
            *title = "FAT32 card required";
            *detail = "Prepare the card as FAT32 on a computer, then reinsert it.";
        } else {
            *title = "Waiting for storage";
            *detail = "The card reader is starting or checking the SD card.";
        }
        *color = MAP_COLOR_WARN;
    } else if (!snapshot->wifi_connected) {
        *title = snapshot->wifi_connecting ? "Connecting to Wi-Fi" : "Wi-Fi needed";
        *detail = "Connect Wi-Fi to load this local map area.";
        *color = MAP_COLOR_WARN;
    } else if (map_text_contains(download_state, "download") ||
               map_text_contains(download_state, "loading") ||
               map_text_contains(download_state, "request")) {
        *title = "Loading map";
        *detail = "Downloading a small area around the saved map center.";
        *color = MAP_COLOR_INFO;
    } else if (!snapshot->map_tile_render_supported) {
        *title = "Map view starting";
        *detail = "The built-in map renderer is not ready yet.";
        *color = MAP_COLOR_INFO;
    } else {
        *title = "Loading map";
        *detail = "Preparing the saved map area.";
        *color = MAP_COLOR_INFO;
    }
}

static void map_set_hidden(lv_obj_t *object, bool hidden)
{
    if (!object) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool map_marker_rects_intersect(const d1l_ui_map_marker_rect_t *left,
                                       const d1l_ui_map_marker_rect_t *right)
{
    return left && right && left->x1 < right->x2 && left->x2 > right->x1 &&
        left->y1 < right->y2 && left->y2 > right->y1;
}

static void map_viewport_foreground_overlays(void)
{
    lv_obj_t *const overlays[] = {
        s_viewport_options_button,
        s_viewport_center_button,
        s_viewport_center_source_label,
        s_viewport_drag_hint_label,
        s_viewport_progress_bar,
        s_viewport_zoom_label,
        s_viewport_zoom_out_button,
        s_viewport_zoom_in_button,
        s_viewport_attribution_label,
        s_viewport_pin_truth_label,
    };
    for (size_t i = 0; i < sizeof(overlays) / sizeof(overlays[0]); ++i) {
        if (overlays[i]) {
            lv_obj_move_foreground(overlays[i]);
        }
    }
}

static void map_viewport_reset_marker_state(void)
{
    s_viewport_marker_count = 0U;
    s_viewport_marker_label_count = 0U;
    s_viewport_marker_store_generation = 0U;
    s_viewport_marker_frame_generation = 0U;
    s_viewport_marker_rendered_context_generation = 0U;
    memset(s_viewport_marker_hits, 0, sizeof(s_viewport_marker_hits));
    memset(s_viewport_marker_bounds, 0, sizeof(s_viewport_marker_bounds));
}

static void map_viewport_hide_markers(void)
{
    map_viewport_reset_marker_state();
    if (s_viewport_marker_layer) {
        lv_obj_clean(s_viewport_marker_layer);
        lv_obj_add_flag(s_viewport_marker_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool map_viewport_marker_lease_valid(uint32_t generation)
{
    portENTER_CRITICAL(&s_viewport_lease_lock);
    const bool valid = generation != 0U &&
                       s_viewport_generation == generation &&
                       s_viewport_suppression_depth == 0U;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    return valid;
}

static void map_viewport_update_pin_truth_label(void)
{
    if (!s_viewport_pin_truth_label ||
        !lv_obj_is_valid(s_viewport_pin_truth_label)) {
        s_viewport_pin_truth_label = NULL;
        return;
    }
    lv_label_set_text(
        s_viewport_pin_truth_label,
        s_viewport_marker_age_reference_valid ?
            "Location shared by node\nrecently verified\naccuracy unknown" :
            "Location hidden\nsource not verified");
    lv_obj_set_style_text_color(
        s_viewport_pin_truth_label,
        lv_color_hex(s_viewport_marker_age_reference_valid ?
                     MAP_COLOR_GOOD : MAP_COLOR_WARN), 0);
}

static bool map_viewport_center_acquire_allowed(void)
{
    return s_viewport_center_trusted_for_acquire &&
        s_viewport_position_initialized;
}

static void map_viewport_invalidate_untrusted_center(
    bool release_retained_lease)
{
    s_viewport_center_trusted_for_acquire = false;
    s_viewport_position_initialized = false;
    s_viewport_saved_lat_e7 = 0;
    s_viewport_saved_lon_e7 = 0;
    s_viewport_lat_e7 = 0;
    s_viewport_lon_e7 = 0;
    if (release_retained_lease) {
        d1l_ui_map_viewport_release();
    }
    map_viewport_hide_markers();
    if (s_viewport_image_obj && lv_obj_is_valid(s_viewport_image_obj)) {
        map_set_hidden(s_viewport_image_obj, true);
    }
}

void d1l_ui_map_viewport_update_truth_context(
    const d1l_app_snapshot_t *snapshot)
{
    d1l_map_viewport_truth_state_t state = {
        .age_reference_valid = s_viewport_marker_age_reference_valid,
        .age_reference_timestamp = s_viewport_marker_reference_timestamp,
        .marker_context_generation = s_viewport_marker_context_generation,
        .center_source = s_viewport_center_source,
        .center_trusted_for_acquire =
            s_viewport_center_trusted_for_acquire,
        .retained_view_valid = s_viewport_position_initialized,
        .retained_lease_active = d1l_ui_map_viewport_lease_active(),
    };
    const d1l_map_viewport_truth_input_t input = {
        .age_reference_valid = snapshot &&
            snapshot->map_marker_age_reference_valid,
        .age_reference_timestamp = snapshot ?
            snapshot->map_marker_reference_timestamp : 0U,
        .center_source = snapshot ? snapshot->map_center_source :
            D1L_MAP_CENTER_SOURCE_UNKNOWN,
        .center_location_set = snapshot && snapshot->map_location_set,
        .retained_center_matches_snapshot = snapshot &&
            s_viewport_position_initialized &&
            snapshot->map_lat_e7 == s_viewport_saved_lat_e7 &&
            snapshot->map_lon_e7 == s_viewport_saved_lon_e7,
    };
    d1l_map_viewport_truth_result_t transition = {0};
    if (!d1l_map_viewport_truth_apply(&state, &input, &transition)) {
        map_viewport_invalidate_untrusted_center(true);
        map_viewport_update_pin_truth_label();
        return;
    }
    s_viewport_marker_age_reference_valid = state.age_reference_valid;
    s_viewport_marker_reference_timestamp = state.age_reference_timestamp;
    s_viewport_marker_context_generation = state.marker_context_generation;
    s_viewport_center_source = state.center_source;
    s_viewport_center_trusted_for_acquire =
        transition.acquisition_allowed;
    if (transition.invalidate_retained_view) {
        map_viewport_invalidate_untrusted_center(
            transition.release_retained_lease);
    }
    map_viewport_update_pin_truth_label();
}

static uint32_t map_marker_role_color(const char *type)
{
    if (type && strcmp(type, "repeater") == 0) {
        return 0xFACC15U;
    }
    if (type && strcmp(type, "room") == 0) {
        return 0xC084FCU;
    }
    if (type && strcmp(type, "sensor") == 0) {
        return 0x38BDF8U;
    }
    if (type && strcmp(type, "chat") == 0) {
        return 0x1E5AEFU;
    }
    return 0xA3E635U;
}

static void map_copy_bounded_text(char *dest, size_t dest_len, const char *source)
{
    if (!dest || dest_len == 0U) {
        return;
    }
    size_t copied = 0U;
    while (source && source[copied] != '\0' && copied + 1U < dest_len) {
        dest[copied] = source[copied];
        ++copied;
    }
    dest[copied] = '\0';
}

static void map_marker_display_name(const d1l_node_marker_t *marker,
                                    char *dest, size_t dest_len)
{
    if (!dest || dest_len == 0U) {
        return;
    }
    dest[0] = '\0';
    if (!marker) {
        return;
    }
    const char *source = marker->name[0] ? marker->name : "Mesh node";
    const size_t source_len = strlen(source);
    if (source_len <= MAP_MARKER_NAME_MAX_CHARS) {
        map_copy_bounded_text(dest, dest_len, source);
        return;
    }
    const size_t prefix_len = MAP_MARKER_NAME_MAX_CHARS - 3U;
    snprintf(dest, dest_len, "%.*s...", (int)prefix_len, source);
}

static int32_t map_e7_to_e6(int32_t value_e7)
{
    const int64_t value = value_e7;
    return (int32_t)(value >= 0 ? (value + 5) / 10 : (value - 5) / 10);
}

static bool map_marker_placement_allowed(const d1l_ui_map_marker_rect_t *bounds)
{
    if (!bounds || bounds->x1 < 0 || bounds->y1 < 0 ||
        bounds->x2 > (int16_t)MAP_VIEWPORT_WIDTH ||
        bounds->y2 > (int16_t)MAP_VIEWPORT_HEIGHT) {
        return false;
    }
    static const d1l_ui_map_marker_rect_t exclusions[] = {
        {0, 0, 112, 64},       /* Options. */
        {108, 10, 412, 64},    /* Status/progress overlay. */
        {412, 0, 478, 152},    /* Zoom controls and badge. */
        {0, 270, 112, 360},    /* Center and explicit source. */
        {108, 294, 232, 360},  /* Pin provenance, age, and accuracy. */
        {220, 326, 478, 360},  /* Attribution. */
    };
    for (size_t i = 0; i < sizeof(exclusions) / sizeof(exclusions[0]); ++i) {
        if (map_marker_rects_intersect(bounds, &exclusions[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < s_viewport_marker_label_count; ++i) {
        if (map_marker_rects_intersect(bounds, &s_viewport_marker_bounds[i])) {
            return false;
        }
    }
    return true;
}

static bool map_viewport_refresh_markers(const d1l_map_view_status_t *status)
{
    if (!status || !s_viewport_marker_layer || !s_viewport_image_obj ||
        !status->visible || !status->frame_ready ||
        !map_viewport_marker_lease_valid(status->generation) ||
        s_viewport_revision == 0U ||
        status->lat_e7 != s_viewport_lat_e7 || status->lon_e7 != s_viewport_lon_e7 ||
        status->zoom != s_viewport_zoom || status->width != MAP_VIEWPORT_WIDTH ||
        status->height != MAP_VIEWPORT_HEIGHT ||
        lv_obj_has_flag(s_viewport_image_obj, LV_OBJ_FLAG_HIDDEN)) {
        map_viewport_hide_markers();
        return false;
    }

    const uint32_t marker_generation = d1l_node_store_marker_generation();
    if (marker_generation == s_viewport_marker_store_generation &&
        status->generation == s_viewport_marker_frame_generation &&
        s_viewport_marker_context_generation ==
            s_viewport_marker_rendered_context_generation) {
        return false;
    }

    lv_obj_clean(s_viewport_marker_layer);
    s_viewport_marker_count = 0U;
    s_viewport_marker_label_count = 0U;
    memset(s_viewport_marker_hits, 0, sizeof(s_viewport_marker_hits));
    memset(s_viewport_marker_bounds, 0, sizeof(s_viewport_marker_bounds));

    const size_t candidate_count = d1l_node_store_copy_markers(
        s_viewport_marker_candidates, MAP_MARKER_QUERY_LIMIT);
    const d1l_map_projection_view_t projection = {
        .center_lat_e6 = map_e7_to_e6(status->lat_e7),
        .center_lon_e6 = map_e7_to_e6(status->lon_e7),
        .zoom = status->zoom,
        .viewport_width = MAP_VIEWPORT_WIDTH,
        .viewport_height = MAP_VIEWPORT_HEIGHT,
        .pan_x_pixels = 0,
        .pan_y_pixels = 0,
    };
    for (size_t i = 0; i < candidate_count; ++i) {
        d1l_map_marker_truth_t truth = {0};
        if (!d1l_map_marker_truth_evaluate(
                &s_viewport_marker_candidates[i],
                s_viewport_marker_age_reference_valid,
                s_viewport_marker_reference_timestamp, &truth)) {
            continue;
        }
        d1l_map_projected_point_t point = {0};
        if (!d1l_map_point_project_e6(&projection,
                                      s_viewport_marker_candidates[i].lat_e6,
                                      s_viewport_marker_candidates[i].lon_e6,
                                      &point) || !point.visible) {
            continue;
        }
        const int16_t dot_radius = MAP_MARKER_DOT_DIAMETER / 2;
        const int16_t label_x = point.screen_x - MAP_MARKER_LABEL_WIDTH / 2;
        const int16_t label_y = point.screen_y + dot_radius + MAP_MARKER_LABEL_GAP;
        const d1l_ui_map_marker_rect_t bounds = {
            .x1 = label_x,
            .y1 = point.screen_y - dot_radius,
            .x2 = label_x + MAP_MARKER_LABEL_WIDTH,
            .y2 = label_y + MAP_MARKER_LABEL_HEIGHT,
        };
        const bool label_allowed =
            s_viewport_marker_label_count < MAP_MARKER_LABEL_LIMIT &&
            map_marker_placement_allowed(&bounds);

        lv_obj_t *dot = lv_obj_create(s_viewport_marker_layer);
        if (!dot) {
            continue;
        }
        lv_obj_set_size(dot, MAP_MARKER_DOT_DIAMETER, MAP_MARKER_DOT_DIAMETER);
        lv_obj_set_pos(dot, point.screen_x - dot_radius, point.screen_y - dot_radius);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(map_marker_role_color(
                                       s_viewport_marker_candidates[i].type)), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(MAP_COLOR_TEXT), 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        const size_t shown = s_viewport_marker_count++;
        s_viewport_marker_hits[shown].screen_x = point.screen_x;
        s_viewport_marker_hits[shown].screen_y = point.screen_y;
        map_copy_bounded_text(s_viewport_marker_hits[shown].fingerprint,
                              sizeof(s_viewport_marker_hits[shown].fingerprint),
                              s_viewport_marker_candidates[i].fingerprint);
        if (!label_allowed) {
            continue;
        }

        char display_name[MAP_MARKER_NAME_MAX_CHARS + 1U];
        map_marker_display_name(&s_viewport_marker_candidates[i], display_name,
                                sizeof(display_name));
        char age[12];
        d1l_map_marker_format_age(truth.age_sec, age, sizeof(age));
        char marker_label[64];
        snprintf(marker_label, sizeof(marker_label), "%s\n%s %s", display_name,
                 d1l_map_marker_role_label(truth.role), age);
        lv_obj_t *name = map_label(s_viewport_marker_layer, marker_label,
                                   MAP_COLOR_TEXT);
        if (!name) {
            continue;
        }
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(name, MAP_MARKER_LABEL_WIDTH, MAP_MARKER_LABEL_HEIGHT);
        lv_obj_set_pos(name, label_x, label_y);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(name, lv_color_hex(MAP_COLOR_BG), 0);
        lv_obj_set_style_bg_opa(name, LV_OPA_90, 0);
        lv_obj_set_style_radius(name, 4, 0);
        lv_obj_set_style_pad_hor(name, 2, 0);
        lv_obj_set_style_pad_ver(name, 1, 0);
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        s_viewport_marker_bounds[s_viewport_marker_label_count++] = bounds;
    }

    s_viewport_marker_store_generation = marker_generation;
    s_viewport_marker_frame_generation = status->generation;
    s_viewport_marker_rendered_context_generation =
        s_viewport_marker_context_generation;
    /* Release/suppression may come from a non-UI task while this UI-only
     * overlay is being rebuilt. Never publish the rebuilt layer for an
     * invalidated lease; the marker arrays themselves remain UI-task-owned. */
    if (!map_viewport_marker_lease_valid(status->generation)) {
        map_viewport_hide_markers();
        return false;
    }
    map_set_hidden(s_viewport_marker_layer, s_viewport_marker_count == 0U);
    lv_obj_invalidate(s_viewport_marker_layer);
    map_viewport_foreground_overlays();
    /* A cross-task release cannot touch LVGL directly. Recheck once after
     * publication so a release that raced the rebuild is hidden on this UI
     * pass; marker taps also validate the lease independently. */
    if (!map_viewport_marker_lease_valid(status->generation)) {
        map_viewport_hide_markers();
        return false;
    }
    return true;
}

static bool map_viewport_try_marker_tap(void)
{
    if (!s_viewport_obj || !s_viewport_open_node_detail ||
        s_viewport_marker_count == 0U ||
        !map_viewport_marker_lease_valid(s_viewport_marker_frame_generation)) {
        return false;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return false;
    }
    lv_point_t point = {0};
    lv_indev_get_point(indev, &point);
    lv_area_t viewport_area = {0};
    lv_obj_get_coords(s_viewport_obj, &viewport_area);
    const int32_t local_x = point.x - viewport_area.x1 - 1;
    const int32_t local_y = point.y - viewport_area.y1 - 1;
    size_t nearest = 0U;
    int32_t nearest_distance_sq = INT32_MAX;
    for (size_t i = 0; i < s_viewport_marker_count; ++i) {
        const int32_t dx = local_x - s_viewport_marker_hits[i].screen_x;
        const int32_t dy = local_y - s_viewport_marker_hits[i].screen_y;
        const int32_t distance_sq = dx * dx + dy * dy;
        if (distance_sq <= MAP_MARKER_HIT_RADIUS * MAP_MARKER_HIT_RADIUS &&
            distance_sq < nearest_distance_sq) {
            nearest = i;
            nearest_distance_sq = distance_sq;
        }
    }
    if (nearest_distance_sq == INT32_MAX) {
        return false;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN];
    snprintf(fingerprint, sizeof(fingerprint), "%s",
             s_viewport_marker_hits[nearest].fingerprint);
    if (!map_viewport_marker_lease_valid(s_viewport_marker_frame_generation)) {
        return false;
    }
    s_viewport_open_node_detail(fingerprint);
    return true;
}

static bool map_viewport_consume_acquire_suppression(void)
{
    bool suppressed;
    portENTER_CRITICAL(&s_viewport_lease_lock);
    suppressed = s_viewport_suppression_depth > 0U || s_viewport_suppress_next_acquire;
    s_viewport_suppress_next_acquire = false;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    return suppressed;
}

static bool map_viewport_ensure_pixels(void)
{
    if (s_viewport_pixels) {
        return true;
    }
    const size_t bytes = (size_t)MAP_VIEWPORT_WIDTH *
        (size_t)MAP_VIEWPORT_HEIGHT * sizeof(uint16_t);
    s_viewport_pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_viewport_pixels) {
        s_viewport_allocation_failed = true;
        return false;
    }
    s_viewport_allocation_failed = false;
    memset(s_viewport_pixels, 0, bytes);
    memset(&s_viewport_image, 0, sizeof(s_viewport_image));
    s_viewport_image.header.always_zero = 0;
    s_viewport_image.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_viewport_image.header.w = MAP_VIEWPORT_WIDTH;
    s_viewport_image.header.h = MAP_VIEWPORT_HEIGHT;
    s_viewport_image.data_size = (uint32_t)bytes;
    s_viewport_image.data = (const uint8_t *)s_viewport_pixels;
    return true;
}

static void map_viewport_apply_service_status(const d1l_map_view_status_t *status)
{
    if (!status || !s_viewport_status_label || !s_viewport_detail_label ||
        !s_viewport_readiness_label) {
        return;
    }
    const char *title = "Map view starting";
    const char *detail = "Preparing the built-in local map.";
    uint32_t color = MAP_COLOR_INFO;
    bool show_overlay = !status->frame_ready;

    if (s_viewport_allocation_failed) {
        title = "Map unavailable";
        detail = "Not enough map memory. Other device features remain available.";
        color = MAP_COLOR_WARN;
        show_overlay = true;
    } else if (status->rate_limited) {
        title = "Map paused";
        detail = "The tile service asked us to wait before retrying.";
        color = MAP_COLOR_WARN;
        show_overlay = true;
    } else if (strcmp(status->phase, "sd_attention") == 0) {
        title = "Map unavailable";
        detail = "Check the SD card, then reopen Map.";
        color = MAP_COLOR_WARN;
        show_overlay = true;
    } else if (strcmp(status->phase, "sd_card_required") == 0) {
        title = "SD card required";
        detail = "Insert a FAT32 card to save and load map tiles.";
        color = MAP_COLOR_WARN;
        show_overlay = true;
    } else if (strcmp(status->phase, "sd_fat32_required") == 0) {
        title = "FAT32 card required";
        detail = "Prepare the card as FAT32 on a computer, then reinsert it.";
        color = MAP_COLOR_WARN;
        show_overlay = true;
    } else if (strcmp(status->phase, "sd_reconnecting") == 0 ||
               strcmp(status->phase, "sd_cache_required") == 0) {
        title = "Waiting for storage";
        detail = "The card reader is starting or checking the SD card.";
        color = MAP_COLOR_WARN;
        show_overlay = true;
    } else if (strcmp(status->phase, "wifi_required") == 0) {
        title = "Wi-Fi needed";
        detail = "Connect Wi-Fi to load this local map area.";
        color = MAP_COLOR_WARN;
        show_overlay = true;
    } else if (strcmp(status->phase, "time_sync") == 0) {
        title = "Secure time needed";
        detail = "Keep Wi-Fi connected, then reopen Map.";
        color = MAP_COLOR_WARN;
        show_overlay = true;
    } else if (strcmp(status->phase, "provider_config") == 0) {
        title = "Map source needs attention";
        detail = "Check the offline provider file on the SD card.";
        color = MAP_COLOR_WARN;
        show_overlay = true;
    } else if (strcmp(status->phase, "provider_preloaded_only") == 0) {
        title = "Tile is not preloaded";
        detail = "Add this area to the SD card or configure an authorized download source.";
        color = MAP_COLOR_WARN;
        show_overlay = true;
    } else if (status->failed_tiles > 0U && !status->frame_ready) {
        title = "Map unavailable";
        detail = "Tiles could not be loaded. Check Wi-Fi and the SD card.";
        color = MAP_COLOR_WARN;
        show_overlay = true;
    } else if (!status->frame_ready &&
               strcmp(status->phase, "loading_cache") == 0) {
        title = "Loading saved map";
        detail = "Reading saved tiles from the SD card.";
        color = MAP_COLOR_INFO;
        show_overlay = true;
    } else if (!status->frame_ready && strcmp(status->phase, "queued") == 0) {
        title = "Preparing map";
        detail = "Checking saved tiles first.";
        color = MAP_COLOR_INFO;
        show_overlay = true;
    } else if (!status->frame_ready) {
        title = "Downloading map";
        detail = "Fetching only the visible current view.";
        color = MAP_COLOR_INFO;
        show_overlay = true;
    }

    lv_label_set_text(s_viewport_status_label, title);
    lv_obj_set_style_text_color(s_viewport_status_label, lv_color_hex(color), 0);
    lv_label_set_text(s_viewport_detail_label, detail);
    char readiness[96];
    snprintf(readiness, sizeof(readiness), "Wi-Fi %s  |  SD %s",
             status->wifi_connected ? "Connected" : "Not connected",
             status->sd_cache_ready ? "Ready" : "Not ready");
    lv_label_set_text(s_viewport_readiness_label, readiness);
    map_set_hidden(s_viewport_status_label, !show_overlay);
    map_set_hidden(s_viewport_detail_label, !show_overlay);
    map_set_hidden(s_viewport_readiness_label, !show_overlay);
    if (s_viewport_drag_hint_label) {
        char progress[32];
        if (strcmp(status->phase, "sd_card_required") == 0) {
            lv_label_set_text(s_viewport_drag_hint_label, "Insert SD card");
            lv_obj_set_style_text_color(s_viewport_drag_hint_label,
                                        lv_color_hex(MAP_COLOR_WARN), 0);
        } else if (strcmp(status->phase, "sd_fat32_required") == 0) {
            lv_label_set_text(s_viewport_drag_hint_label, "Needs FAT32");
            lv_obj_set_style_text_color(s_viewport_drag_hint_label,
                                        lv_color_hex(MAP_COLOR_WARN), 0);
        } else if (strcmp(status->phase, "sd_attention") == 0) {
            lv_label_set_text(s_viewport_drag_hint_label, "Check SD card");
            lv_obj_set_style_text_color(s_viewport_drag_hint_label,
                                        lv_color_hex(MAP_COLOR_WARN), 0);
        } else if (strcmp(status->phase, "sd_reconnecting") == 0 ||
                   strcmp(status->phase, "sd_cache_required") == 0) {
            lv_label_set_text(s_viewport_drag_hint_label, "Storage starting");
            lv_obj_set_style_text_color(s_viewport_drag_hint_label,
                                        lv_color_hex(MAP_COLOR_WARN), 0);
        } else if (strcmp(status->phase, "wifi_required") == 0) {
            lv_label_set_text(s_viewport_drag_hint_label, "Waiting for Wi-Fi");
            lv_obj_set_style_text_color(s_viewport_drag_hint_label,
                                        lv_color_hex(MAP_COLOR_WARN), 0);
        } else if (strcmp(status->phase, "time_sync") == 0) {
            lv_label_set_text(s_viewport_drag_hint_label, "Checking secure time");
            lv_obj_set_style_text_color(s_viewport_drag_hint_label,
                                        lv_color_hex(MAP_COLOR_WARN), 0);
        } else if (status->rate_limited) {
            lv_label_set_text(s_viewport_drag_hint_label, "Map paused");
            lv_obj_set_style_text_color(s_viewport_drag_hint_label,
                                        lv_color_hex(MAP_COLOR_WARN), 0);
        } else if (status->worker_running && status->planned_tiles > 0U &&
            status->rendered_tiles < status->planned_tiles) {
            uint8_t current = status->attempted_tiles;
            if (current == 0U) {
                current = 1U;
            } else if (current > status->planned_tiles) {
                current = status->planned_tiles;
            }
            snprintf(progress, sizeof(progress), "%s %u/%u",
                     strcmp(status->phase, "downloading") == 0 ?
                         "Downloading" : "Loading",
                     (unsigned)current, (unsigned)status->planned_tiles);
            lv_label_set_text(s_viewport_drag_hint_label, progress);
            lv_obj_set_style_text_color(s_viewport_drag_hint_label,
                                        lv_color_hex(MAP_COLOR_INFO), 0);
        } else {
            lv_label_set_text(s_viewport_drag_hint_label, "Drag to pan");
            lv_obj_set_style_text_color(s_viewport_drag_hint_label,
                                        lv_color_hex(MAP_COLOR_DETAIL), 0);
        }
        lv_obj_move_foreground(s_viewport_drag_hint_label);
    }
    if (s_viewport_progress_bar) {
        uint8_t completed = status->rendered_tiles + status->failed_tiles;
        if (completed > status->planned_tiles) {
            completed = status->planned_tiles;
        }
        const bool bounded_progress =
            strcmp(status->phase, "loading_cache") == 0 ||
            strcmp(status->phase, "downloading") == 0;
        const bool show_progress =
            bounded_progress &&
            status->worker_running && status->planned_tiles > 0U &&
            completed < status->planned_tiles;
        if (show_progress) {
            lv_bar_set_range(s_viewport_progress_bar, 0,
                             status->planned_tiles);
            lv_bar_set_value(s_viewport_progress_bar, completed, LV_ANIM_OFF);
            lv_obj_clear_flag(s_viewport_progress_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_viewport_progress_bar);
        } else {
            lv_obj_add_flag(s_viewport_progress_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_viewport_attribution_label) {
        lv_label_set_text(
            s_viewport_attribution_label,
            status->provider_configured && status->attribution[0] != '\0' ?
                status->attribution : "(c) OpenStreetMap contributors");
        lv_obj_move_foreground(s_viewport_attribution_label);
    }
}

static void map_viewport_update_controls(void)
{
    d1l_map_tile_provider_t provider = {0};
    d1l_map_tile_provider_snapshot(&provider);
    const uint8_t maximum =
        provider.max_zoom >= D1L_MAP_VIEW_MIN_ZOOM &&
                provider.max_zoom < D1L_MAP_VIEW_MAX_ZOOM ?
            provider.max_zoom : D1L_MAP_VIEW_MAX_ZOOM;
    if (s_viewport_zoom_label) {
        char zoom[8];
        snprintf(zoom, sizeof(zoom), "z%u", (unsigned)s_viewport_zoom);
        lv_label_set_text(s_viewport_zoom_label, zoom);
    }
    if (s_viewport_zoom_in_button) {
        if (s_viewport_zoom >= maximum) {
            lv_obj_add_state(s_viewport_zoom_in_button, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_viewport_zoom_in_button, LV_STATE_DISABLED);
        }
    }
    if (s_viewport_zoom_out_button) {
        if (s_viewport_zoom <= D1L_MAP_VIEW_MIN_ZOOM) {
            lv_obj_add_state(s_viewport_zoom_out_button, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_viewport_zoom_out_button, LV_STATE_DISABLED);
        }
    }
}

static bool map_viewport_request_interactive_view(void)
{
    uint32_t active_generation;
    bool suppressed;
    portENTER_CRITICAL(&s_viewport_lease_lock);
    active_generation = s_viewport_generation;
    suppressed = s_viewport_suppression_depth > 0U;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    if (!map_viewport_center_acquire_allowed() || active_generation == 0U ||
        suppressed) {
        return false;
    }

    uint32_t generation = 0U;
    esp_err_t ret = d1l_map_view_service_acquire_visible(
        s_viewport_lat_e7, s_viewport_lon_e7, s_viewport_zoom,
        MAP_VIEWPORT_WIDTH, MAP_VIEWPORT_HEIGHT, &generation);
    if (ret != ESP_OK || generation == 0U) {
        return false;
    }

    bool accepted;
    portENTER_CRITICAL(&s_viewport_lease_lock);
    accepted = s_viewport_suppression_depth == 0U &&
        s_viewport_generation != 0U &&
        map_viewport_center_acquire_allowed();
    if (accepted) {
        s_viewport_generation = generation;
        s_viewport_revision = 0U;
    }
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    if (!accepted) {
        d1l_map_view_service_release_visible(generation);
        return false;
    }
    map_viewport_hide_markers();
    map_viewport_update_controls();
    d1l_map_view_status_t status = {0};
    d1l_map_view_service_status(&status);
    map_viewport_apply_service_status(&status);
    return true;
}

static void map_viewport_reset_image_position(void)
{
    if (s_viewport_image_obj) {
        lv_obj_center(s_viewport_image_obj);
    }
    if (s_viewport_marker_layer) {
        lv_obj_set_pos(s_viewport_marker_layer, 1, 1);
    }
}

static void map_viewport_drag_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        portENTER_CRITICAL(&s_viewport_lease_lock);
        s_viewport_drag_active = true;
        s_viewport_drag_x = 0;
        s_viewport_drag_y = 0;
        portEXIT_CRITICAL(&s_viewport_lease_lock);
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) {
            return;
        }
        lv_point_t vector = {0};
        lv_indev_get_vect(indev, &vector);
        int32_t preview_x = 0;
        int32_t preview_y = 0;
        bool drag_active;
        portENTER_CRITICAL(&s_viewport_lease_lock);
        drag_active = s_viewport_drag_active;
        if (drag_active) {
            s_viewport_drag_x = map_drag_clamp(
                s_viewport_drag_x + vector.x, MAP_DRAG_MAX_X_PIXELS);
            s_viewport_drag_y = map_drag_clamp(
                s_viewport_drag_y + vector.y, MAP_DRAG_MAX_Y_PIXELS);
            preview_x = s_viewport_drag_x;
            preview_y = s_viewport_drag_y;
        }
        portEXIT_CRITICAL(&s_viewport_lease_lock);
        if (!drag_active) {
            return;
        }
        if (s_viewport_image_obj) {
            lv_obj_set_pos(s_viewport_image_obj, 1 + preview_x, 1 + preview_y);
        }
        if (s_viewport_marker_layer) {
            lv_obj_set_pos(s_viewport_marker_layer, 1 + preview_x, 1 + preview_y);
        }
        return;
    }
    if (code == LV_EVENT_PRESS_LOST) {
        portENTER_CRITICAL(&s_viewport_lease_lock);
        s_viewport_drag_active = false;
        s_viewport_drag_x = 0;
        s_viewport_drag_y = 0;
        portEXIT_CRITICAL(&s_viewport_lease_lock);
        map_viewport_reset_image_position();
        return;
    }
    if (code != LV_EVENT_RELEASED) {
        return;
    }

    int32_t drag_x;
    int32_t drag_y;
    bool drag_active;
    portENTER_CRITICAL(&s_viewport_lease_lock);
    drag_active = s_viewport_drag_active;
    drag_x = s_viewport_drag_x;
    drag_y = s_viewport_drag_y;
    s_viewport_drag_active = false;
    s_viewport_drag_x = 0;
    s_viewport_drag_y = 0;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    if (!drag_active) {
        map_viewport_reset_image_position();
        return;
    }
    map_viewport_reset_image_position();
    if (abs(drag_x) < MAP_DRAG_THRESHOLD_PIXELS &&
        abs(drag_y) < MAP_DRAG_THRESHOLD_PIXELS) {
        (void)map_viewport_try_marker_tap();
        return;
    }

    const int32_t old_lat_e7 = s_viewport_lat_e7;
    const int32_t old_lon_e7 = s_viewport_lon_e7;
    const int32_t delta_x = -drag_x;
    const int32_t delta_y = -drag_y;
    if (!d1l_map_math_pan_center(old_lat_e7, old_lon_e7, s_viewport_zoom,
                                 delta_x, delta_y, &s_viewport_lat_e7,
                                 &s_viewport_lon_e7) ||
        !map_viewport_request_interactive_view()) {
        s_viewport_lat_e7 = old_lat_e7;
        s_viewport_lon_e7 = old_lon_e7;
    }
}

static void map_viewport_zoom_in_event_cb(lv_event_t *event)
{
    (void)event;
    const uint8_t next_zoom = map_zoom_clamp(
        (uint8_t)(s_viewport_zoom + 1U));
    if (next_zoom <= s_viewport_zoom) {
        return;
    }
    const uint8_t old_zoom = s_viewport_zoom;
    s_viewport_zoom = next_zoom;
    if (!map_viewport_request_interactive_view()) {
        s_viewport_zoom = old_zoom;
    }
    map_viewport_update_controls();
}

static void map_viewport_zoom_out_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_viewport_zoom <= D1L_MAP_VIEW_MIN_ZOOM) {
        return;
    }
    const uint8_t old_zoom = s_viewport_zoom;
    --s_viewport_zoom;
    if (!map_viewport_request_interactive_view()) {
        s_viewport_zoom = old_zoom;
    }
    map_viewport_update_controls();
}

static void map_viewport_recenter_event_cb(lv_event_t *event)
{
    (void)event;
    if (!s_viewport_position_initialized) {
        return;
    }
    const int32_t old_lat_e7 = s_viewport_lat_e7;
    const int32_t old_lon_e7 = s_viewport_lon_e7;
    s_viewport_lat_e7 = s_viewport_saved_lat_e7;
    s_viewport_lon_e7 = s_viewport_saved_lon_e7;
    if (!map_viewport_request_interactive_view()) {
        s_viewport_lat_e7 = old_lat_e7;
        s_viewport_lon_e7 = old_lon_e7;
    }
}

bool d1l_ui_map_viewport_refresh(void)
{
    uint32_t generation;
    portENTER_CRITICAL(&s_viewport_lease_lock);
    generation = s_viewport_generation;
    const bool suppressed = s_viewport_suppression_depth > 0U;
    const bool drag_active = s_viewport_drag_active;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    if (!map_viewport_center_acquire_allowed()) {
        d1l_ui_map_viewport_release();
        map_viewport_hide_markers();
        if (s_viewport_image_obj && lv_obj_is_valid(s_viewport_image_obj)) {
            map_set_hidden(s_viewport_image_obj, true);
        }
        return false;
    }
    if (suppressed) {
        map_viewport_hide_markers();
        return false;
    }
    if (generation == 0U || !s_viewport_image_obj || drag_active) {
        return false;
    }
    d1l_map_view_status_t status = {0};
    d1l_map_view_service_status(&status);
    if (!status.visible || status.generation != generation) {
        return false;
    }

    d1l_map_view_frame_t frame = {0};
    bool copied = false;
    const bool frame_update_available = status.frame_ready &&
        status.frame_revision > s_viewport_revision;
    const esp_err_t ret = frame_update_available ?
        d1l_map_view_service_acquire_frame(s_viewport_revision, &frame) :
        ESP_ERR_NOT_FOUND;
    if (ret == ESP_OK && frame.held) {
        const size_t capacity = (size_t)MAP_VIEWPORT_WIDTH *
            (size_t)MAP_VIEWPORT_HEIGHT;
        if (frame.generation == generation && frame.pixels &&
            frame.pixel_count > 0U && frame.pixel_count <= capacity &&
            frame.width > 0U && frame.height > 0U) {
            if (map_viewport_ensure_pixels()) {
                lv_img_cache_invalidate_src(&s_viewport_image);
                memcpy(s_viewport_pixels, frame.pixels,
                       frame.pixel_count * sizeof(uint16_t));
                s_viewport_image.header.w = frame.width;
                s_viewport_image.header.h = frame.height;
                s_viewport_image.data_size =
                    (uint32_t)(frame.pixel_count * sizeof(uint16_t));
                portENTER_CRITICAL(&s_viewport_lease_lock);
                if (s_viewport_generation == generation &&
                    s_viewport_suppression_depth == 0U) {
                    s_viewport_revision = frame.revision;
                    copied = true;
                }
                portEXIT_CRITICAL(&s_viewport_lease_lock);
            }
        }
        d1l_map_view_service_release_frame(&frame);
    }
    if (copied) {
        lv_img_cache_invalidate_src(&s_viewport_image);
        lv_img_set_src(s_viewport_image_obj, &s_viewport_image);
        lv_obj_set_size(s_viewport_image_obj, s_viewport_image.header.w,
                        s_viewport_image.header.h);
        lv_obj_center(s_viewport_image_obj);
        lv_obj_clear_flag(s_viewport_image_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_viewport_image_obj);
        /*
         * A newly published current-view frame is visible progress, just like
         * a user-facing loading animation. Keep the display awake while real
         * tiles continue to arrive so the normal inactivity lock cannot
         * repeatedly cancel and restart a slow SD-backed view. A stalled
         * worker publishes no frame and therefore still locks on schedule.
         */
        lv_disp_trig_activity(NULL);
    }
    d1l_map_view_service_status(&status);
    (void)map_viewport_refresh_markers(&status);
    map_viewport_apply_service_status(&status);
    return copied;
}

bool d1l_ui_map_viewport_reacquire(void)
{
    bool suppressed;
    uint32_t active_generation;
    portENTER_CRITICAL(&s_viewport_lease_lock);
    suppressed = s_viewport_suppression_depth > 0U;
    active_generation = s_viewport_generation;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    if (!map_viewport_center_acquire_allowed()) {
        d1l_ui_map_viewport_release();
        return false;
    }
    if (active_generation != 0U) {
        return !suppressed;
    }
    if (suppressed || !map_viewport_center_acquire_allowed() || !s_viewport_obj ||
        !s_viewport_image_obj) {
        return false;
    }

    uint32_t generation = 0U;
    const esp_err_t ret = d1l_map_view_service_acquire_visible(
        s_viewport_lat_e7, s_viewport_lon_e7, s_viewport_zoom,
        MAP_VIEWPORT_WIDTH, MAP_VIEWPORT_HEIGHT, &generation);
    if (ret != ESP_OK || generation == 0U) {
        return false;
    }
    bool accepted;
    portENTER_CRITICAL(&s_viewport_lease_lock);
    accepted = s_viewport_suppression_depth == 0U &&
        s_viewport_generation == 0U &&
        map_viewport_center_acquire_allowed();
    if (accepted) {
        s_viewport_generation = generation;
        s_viewport_revision = 0U;
    }
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    if (!accepted) {
        d1l_map_view_service_release_visible(generation);
        return false;
    }
    map_viewport_hide_markers();
    map_viewport_reset_image_position();
    (void)d1l_ui_map_viewport_refresh();
    return true;
}

static lv_obj_t *map_menu_row(lv_obj_t *parent, int y, const char *title,
                              const char *status, uint32_t accent,
                              lv_event_cb_t callback)
{
    lv_obj_t *row = map_panel(parent, 16, y, 448, 68);
    if (!row) {
        return NULL;
    }
    lv_obj_set_style_bg_color(row, lv_color_hex(MAP_COLOR_PANEL_PRESSED), LV_STATE_PRESSED);
    if (callback) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *title_label = map_label(row, title, accent);
    map_label_dot(title_label, 190);
    if (title_label) {
        lv_obj_set_pos(title_label, 28, 24);
    }
    lv_obj_t *status_label = map_label(row, status, MAP_COLOR_MUTED);
    map_label_dot(status_label, 164);
    if (status_label) {
        lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(status_label, 236, 24);
    }
    lv_obj_t *chevron = map_label(row, ">", MAP_COLOR_MUTED);
    if (chevron) {
        lv_obj_set_pos(chevron, 422, 24);
    }
    return row;
}

static void map_render_status_row(lv_obj_t *parent, int y, const char *title,
                                  const char *value, uint32_t color)
{
    lv_obj_t *row = map_panel(parent, 12, y, 424, 48);
    if (!row) {
        return;
    }
    lv_obj_t *title_label = map_label(row, title, MAP_COLOR_DETAIL);
    map_label_dot(title_label, 210);
    if (title_label) {
        lv_obj_set_pos(title_label, 12, 15);
    }
    lv_obj_t *value_label = map_label(row, value, color);
    map_label_dot(value_label, 194);
    if (value_label) {
        lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(value_label, 218, 15);
    }
}

static void map_saved_area_status(const d1l_map_view_status_t *status,
                                  char *value, size_t value_len,
                                  uint32_t *color)
{
    if (!value || value_len == 0U || !color) {
        return;
    }
    *color = MAP_COLOR_MUTED;
    snprintf(value, value_len, "%s", "Open Map to save");
    if (!status || status->planned_tiles == 0U) {
        return;
    }
    if (status->worker_running) {
        snprintf(value, value_len, "Saving %u/%u",
                 (unsigned)status->rendered_tiles,
                 (unsigned)status->planned_tiles);
        *color = MAP_COLOR_INFO;
    } else if (!status->worker_running &&
               status->rendered_tiles == status->planned_tiles &&
               status->failed_tiles == 0U) {
        snprintf(value, value_len, "%s", "Local area saved");
        *color = MAP_COLOR_GOOD;
    } else if (status->rendered_tiles > 0U) {
        snprintf(value, value_len, "%s", "Partly saved");
        *color = MAP_COLOR_WARN;
    }
}

void d1l_ui_map_render(lv_obj_t *parent,
                       const d1l_app_snapshot_t *snapshot,
                       const d1l_ui_map_callbacks_t *callbacks)
{
    if (!parent || !snapshot || !callbacks) {
        return;
    }
    map_viewport_sync_position(snapshot);
#if D1L_ENABLE_QUALIFICATION_HOOKS
    map_viewport_apply_acceptance_reset(snapshot);
#endif
    d1l_ui_map_viewport_release();
    /* Rendering runs on the UI task, so it owns marker hit/cache mutation.
     * Cross-task release only invalidates the lease. */
    map_viewport_reset_marker_state();
    s_viewport_obj = NULL;
    s_viewport_image_obj = NULL;
    s_viewport_marker_layer = NULL;
    s_viewport_status_label = NULL;
    s_viewport_detail_label = NULL;
    s_viewport_readiness_label = NULL;
    s_viewport_attribution_label = NULL;
    s_viewport_drag_hint_label = NULL;
    s_viewport_progress_bar = NULL;
    s_viewport_zoom_label = NULL;
    s_viewport_zoom_in_button = NULL;
    s_viewport_zoom_out_button = NULL;
    s_viewport_options_button = NULL;
    s_viewport_center_button = NULL;
    s_viewport_center_source_label = NULL;
    s_viewport_pin_truth_label = NULL;
    d1l_ui_map_viewport_update_truth_context(snapshot);
    s_viewport_open_node_detail = callbacks->open_node_detail;
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scroll_dir(parent, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(parent, 0, 0);
    lv_obj_t *viewport = map_panel(
        parent, 0, 0, 480, MAP_VIEWPORT_PANEL_HEIGHT);
    if (!viewport) {
        return;
    }
    s_viewport_obj = viewport;
    lv_obj_set_style_bg_color(viewport, lv_color_hex(0x0B151E), 0);
    lv_obj_set_style_radius(viewport, 0, 0);
    lv_obj_set_style_border_width(viewport, 0, 0);
    lv_obj_add_flag(viewport, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(viewport, LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_SCROLL_CHAIN |
                                LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_scroll_dir(viewport, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(viewport, map_viewport_drag_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(viewport, map_viewport_drag_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(viewport, map_viewport_drag_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(viewport, map_viewport_drag_event_cb, LV_EVENT_PRESS_LOST, NULL);
    s_viewport_image_obj = lv_img_create(viewport);
    if (s_viewport_image_obj) {
        lv_obj_set_size(s_viewport_image_obj, MAP_VIEWPORT_WIDTH, MAP_VIEWPORT_HEIGHT);
        lv_obj_center(s_viewport_image_obj);
        lv_obj_add_flag(s_viewport_image_obj, LV_OBJ_FLAG_HIDDEN);
    }
    s_viewport_marker_layer = lv_obj_create(viewport);
    if (s_viewport_marker_layer) {
        lv_obj_set_size(s_viewport_marker_layer, MAP_VIEWPORT_WIDTH,
                        MAP_VIEWPORT_HEIGHT);
        lv_obj_set_pos(s_viewport_marker_layer, 1, 1);
        lv_obj_set_style_bg_opa(s_viewport_marker_layer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_viewport_marker_layer, 0, 0);
        lv_obj_set_style_pad_all(s_viewport_marker_layer, 0, 0);
        lv_obj_clear_flag(s_viewport_marker_layer,
                          LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                          LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(s_viewport_marker_layer, LV_OBJ_FLAG_HIDDEN);
    }
    const char *state_title = "Map unavailable";
    const char *state_detail = "Map status is unavailable.";
    uint32_t state_color = MAP_COLOR_WARN;
    map_viewport_copy(snapshot, &state_title, &state_detail, &state_color);

    lv_obj_t *status = map_label(viewport, state_title, state_color);
    s_viewport_status_label = status;
    if (status) {
        lv_obj_set_style_text_font(status, &lv_font_montserrat_24, 0);
        map_label_dot(status, 420);
        lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(status, 30, (int)MAP_VIEWPORT_HEIGHT / 2 - 72);
    }
    lv_obj_t *detail = map_label(viewport, state_detail, MAP_COLOR_DETAIL);
    s_viewport_detail_label = detail;
    if (detail) {
        map_label_wrap(detail, 420);
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(detail, 30, (int)MAP_VIEWPORT_HEIGHT / 2 - 30);
    }
    char readiness[96];
    snprintf(readiness, sizeof(readiness), "Wi-Fi %s  |  SD %s",
             map_wifi_status(snapshot), map_sd_status(snapshot));
    lv_obj_t *ready = map_label(viewport, readiness, MAP_COLOR_MUTED);
    s_viewport_readiness_label = ready;
    if (ready) {
        map_label_dot(ready, 420);
        lv_obj_set_style_text_align(ready, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(ready, 30, (int)MAP_VIEWPORT_HEIGHT / 2 + 28);
    }
    lv_obj_t *attribution = map_label(viewport, "(c) OpenStreetMap contributors",
                                      MAP_COLOR_DETAIL);
    s_viewport_attribution_label = attribution;
    if (attribution) {
        map_label_dot(attribution, 240);
        lv_obj_set_style_text_align(attribution, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(attribution, 228, (int)MAP_VIEWPORT_HEIGHT - 36);
        lv_obj_set_style_bg_color(attribution, lv_color_hex(0x17191A), 0);
        lv_obj_set_style_bg_opa(attribution, LV_OPA_80, 0);
        lv_obj_set_style_pad_all(attribution, 2, 0);
    }
    s_viewport_pin_truth_label = map_label(
        viewport, "", MAP_COLOR_WARN);
    if (s_viewport_pin_truth_label) {
        lv_label_set_long_mode(s_viewport_pin_truth_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(s_viewport_pin_truth_label, 112, 58);
        lv_obj_set_pos(s_viewport_pin_truth_label, 112,
                       (int)MAP_VIEWPORT_HEIGHT - 74);
        lv_obj_set_style_text_align(s_viewport_pin_truth_label,
                                    LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(s_viewport_pin_truth_label,
                                  lv_color_hex(MAP_COLOR_BG), 0);
        lv_obj_set_style_bg_opa(s_viewport_pin_truth_label, LV_OPA_80, 0);
        lv_obj_set_style_pad_all(s_viewport_pin_truth_label, 2, 0);
        map_viewport_update_pin_truth_label();
    }

    lv_obj_t *options_button = map_button(
        viewport, "Options", 8, 8, 96, 48, callbacks->open_options);
    s_viewport_options_button = options_button;
    map_style_overlay_button(options_button);

    if (snapshot->map_location_set && map_center_available(snapshot)) {
        char center_source[24];
        snprintf(center_source, sizeof(center_source), "%s source",
                 d1l_map_center_source_label(snapshot->map_center_source));
        s_viewport_center_source_label = map_label(
            viewport, center_source, MAP_COLOR_GOOD);
        if (s_viewport_center_source_label) {
            map_label_dot(s_viewport_center_source_label, 96);
            lv_obj_set_style_text_align(s_viewport_center_source_label,
                                        LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(s_viewport_center_source_label, 8,
                           (int)MAP_VIEWPORT_HEIGHT - 94);
            lv_obj_set_style_bg_color(s_viewport_center_source_label,
                                      lv_color_hex(MAP_COLOR_BG), 0);
            lv_obj_set_style_bg_opa(s_viewport_center_source_label,
                                    LV_OPA_80, 0);
        }
        lv_obj_t *center_button = map_button(
            viewport, "Center", 8, (int)MAP_VIEWPORT_HEIGHT - 60, 96, 52,
            map_viewport_recenter_event_cb);
        s_viewport_center_button = center_button;
        map_style_overlay_button(center_button);
        lv_obj_t *drag_hint = map_label(viewport, "Drag to pan", MAP_COLOR_DETAIL);
        s_viewport_drag_hint_label = drag_hint;
        if (drag_hint) {
            map_label_dot(drag_hint, 172);
            lv_obj_set_style_text_align(drag_hint, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(drag_hint, 142, 22);
            lv_obj_set_style_bg_color(drag_hint, lv_color_hex(MAP_COLOR_BG), 0);
            lv_obj_set_style_bg_opa(drag_hint, LV_OPA_80, 0);
            lv_obj_set_style_pad_all(drag_hint, 2, 0);
        }
        s_viewport_progress_bar = lv_bar_create(viewport);
        if (s_viewport_progress_bar) {
            lv_obj_set_size(s_viewport_progress_bar, 280, 8);
            lv_obj_set_pos(s_viewport_progress_bar, 116, 52);
            lv_bar_set_range(s_viewport_progress_bar, 0,
                             D1L_MAP_VIEW_MAX_TILES);
            lv_bar_set_value(s_viewport_progress_bar, 0, LV_ANIM_OFF);
            lv_obj_set_style_radius(s_viewport_progress_bar, 4, LV_PART_MAIN);
            lv_obj_set_style_radius(s_viewport_progress_bar, 4,
                                    LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(s_viewport_progress_bar,
                                      lv_color_hex(MAP_COLOR_BG),
                                      LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_viewport_progress_bar, LV_OPA_80,
                                    LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_viewport_progress_bar,
                                      lv_color_hex(MAP_COLOR_INFO),
                                      LV_PART_INDICATOR);
            lv_obj_clear_flag(s_viewport_progress_bar, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(s_viewport_progress_bar, LV_OBJ_FLAG_HIDDEN);
        }
        s_viewport_zoom_label = map_label(viewport, "z10", MAP_COLOR_DETAIL);
        if (s_viewport_zoom_label) {
            map_label_dot(s_viewport_zoom_label, 52);
            lv_obj_set_style_text_align(s_viewport_zoom_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(s_viewport_zoom_label, 420, 66);
            lv_obj_set_style_bg_color(s_viewport_zoom_label, lv_color_hex(MAP_COLOR_BG), 0);
            lv_obj_set_style_bg_opa(s_viewport_zoom_label, LV_OPA_80, 0);
            lv_obj_set_style_pad_all(s_viewport_zoom_label, 2, 0);
        }
        s_viewport_zoom_out_button = map_button(
            viewport, "-", 420, 92, 52, 52, map_viewport_zoom_out_event_cb);
        s_viewport_zoom_in_button = map_button(
            viewport, "+", 420, 8, 52, 52, map_viewport_zoom_in_event_cb);
        map_style_overlay_button(s_viewport_zoom_out_button);
        map_style_overlay_button(s_viewport_zoom_in_button);
        map_viewport_update_controls();
    }

    const bool acquire_suppressed = map_viewport_consume_acquire_suppression();
    const bool viewport_memory_ready = !acquire_suppressed && s_viewport_image_obj &&
        map_viewport_ensure_pixels();
    if (snapshot->map_location_set && map_center_available(snapshot) &&
        map_viewport_center_acquire_allowed() &&
        viewport_memory_ready && !acquire_suppressed) {
        uint32_t generation = 0U;
        esp_err_t ret = d1l_map_view_service_acquire_visible(
            s_viewport_lat_e7, s_viewport_lon_e7, s_viewport_zoom,
            MAP_VIEWPORT_WIDTH, MAP_VIEWPORT_HEIGHT,
            &generation);
        if (ret == ESP_OK && generation != 0U) {
            bool accepted;
            portENTER_CRITICAL(&s_viewport_lease_lock);
            accepted = s_viewport_suppression_depth == 0U &&
                map_viewport_center_acquire_allowed();
            if (accepted) {
                s_viewport_generation = generation;
                s_viewport_revision = 0U;
            }
            portEXIT_CRITICAL(&s_viewport_lease_lock);
            if (accepted) {
                (void)d1l_ui_map_viewport_refresh();
            } else {
                d1l_map_view_service_release_visible(generation);
            }
        } else if (s_viewport_status_label && s_viewport_detail_label) {
            lv_label_set_text(s_viewport_status_label, "Map unavailable");
            lv_label_set_text(s_viewport_detail_label,
                              "The built-in map service could not start.");
        }
    } else if (snapshot->map_location_set && map_center_available(snapshot) &&
               acquire_suppressed &&
               s_viewport_status_label && s_viewport_detail_label) {
        lv_label_set_text(s_viewport_status_label, "Map preview");
        lv_label_set_text(s_viewport_detail_label,
                          "Automation view - no map tiles requested.");
        lv_obj_set_style_text_color(s_viewport_status_label,
                                    lv_color_hex(MAP_COLOR_INFO), 0);
    } else if (snapshot->map_location_set && map_center_available(snapshot) &&
               !acquire_suppressed &&
               !viewport_memory_ready &&
               s_viewport_status_label && s_viewport_detail_label) {
        lv_label_set_text(s_viewport_status_label, "Map unavailable");
        lv_label_set_text(s_viewport_detail_label,
                          "Not enough map memory. Other device features remain available.");
    }
}

static void map_render_options_root(lv_obj_t *parent,
                                    const d1l_app_snapshot_t *snapshot,
                                    const d1l_ui_map_callbacks_t *callbacks)
{
    d1l_map_prefetch_status_t prefetch = {0};
    d1l_map_prefetch_service_status(&prefetch);
    map_render_header(parent, "Map options", "Location and cache", "Back to Map",
                      callbacks->close_options);
    map_menu_row(parent, 68, "Set location", map_location_status(snapshot),
                 snapshot->map_location_set ? MAP_COLOR_GOOD : MAP_COLOR_WARN,
                 callbacks->open_location);
    const bool cache_ready = snapshot->map_tile_cache_ready;
    const char *cache_status = "Storage starting";
    if (cache_ready) {
        cache_status = "SD ready";
    } else if (map_storage_needs_attention(snapshot)) {
        cache_status = "Check SD";
    } else if (snapshot->storage_setup_action &&
               strcmp(snapshot->storage_setup_action, "insert_card") == 0) {
        cache_status = "Insert SD";
    } else if (snapshot->storage_sd_needs_fat32) {
        cache_status = "Needs FAT32";
    }
    map_menu_row(parent, 140, "Cache status", cache_status,
                  cache_ready ? MAP_COLOR_GOOD : MAP_COLOR_WARN,
                  callbacks->open_cache_status);
    lv_obj_t *policy = map_panel(parent, 16, 280, 448, 80);
    if (policy) {
        lv_obj_t *text = map_label(policy,
            prefetch.provider_configured &&
                    prefetch.background_prefetch_permitted ?
                "The local node area downloads in the background. Opening Map always has priority." :
                "Built-in tiles download only while Map is open. Offline prefetch needs an authorized SD source.",
            MAP_COLOR_MUTED);
        map_label_wrap(text, 418);
        if (text) {
            lv_obj_set_pos(text, 14, 14);
        }
    }
}

static void map_render_cache_status(lv_obj_t *parent,
                                    const d1l_app_snapshot_t *snapshot,
                                    const d1l_ui_map_callbacks_t *callbacks)
{
    map_render_header(parent, "Cache status", "Read-only readiness", "Back",
                      callbacks->back_to_options);
    d1l_map_view_status_t view_status = {0};
    d1l_map_view_service_status(&view_status);
    d1l_map_prefetch_status_t prefetch = {0};
    d1l_map_prefetch_service_status(&prefetch);
    char map_view_value[32];
    uint32_t map_view_color = MAP_COLOR_MUTED;
    map_saved_area_status(&view_status, map_view_value,
                          sizeof(map_view_value), &map_view_color);
    char prefetch_value[40];
    uint32_t prefetch_color = MAP_COLOR_MUTED;
    if (prefetch.complete) {
        snprintf(prefetch_value, sizeof(prefetch_value), "Ready through z%u",
                 (unsigned)prefetch.selected_max_zoom);
        prefetch_color = MAP_COLOR_GOOD;
    } else if (prefetch.running) {
        snprintf(prefetch_value, sizeof(prefetch_value), "%llu / %llu tiles",
                 (unsigned long long)prefetch.visited_tiles,
                 (unsigned long long)prefetch.total_tiles);
        prefetch_color = MAP_COLOR_INFO;
    } else if (prefetch.paused_for_visible_map) {
        snprintf(prefetch_value, sizeof(prefetch_value), "%s", "Paused for Map");
        prefetch_color = MAP_COLOR_INFO;
    } else if (prefetch.phase[0] != '\0') {
        snprintf(prefetch_value, sizeof(prefetch_value), "%s", prefetch.phase);
        prefetch_color =
            prefetch.eligible ? MAP_COLOR_INFO : MAP_COLOR_WARN;
    } else {
        snprintf(prefetch_value, sizeof(prefetch_value), "%s", "Starting");
    }

    lv_obj_t *panel = map_panel(parent, 16, 64, 448, 324);
    if (!panel) {
        return;
    }
    map_render_status_row(panel, 16, "Wi-Fi", map_wifi_status(snapshot),
                          snapshot->wifi_connected ? MAP_COLOR_GOOD : MAP_COLOR_WARN);
    map_render_status_row(panel, 80, "SD card", map_sd_status(snapshot),
                          snapshot->map_tile_cache_ready ? MAP_COLOR_GOOD : MAP_COLOR_WARN);
    map_render_status_row(panel, 144, "Location", map_location_status(snapshot),
                          snapshot->map_location_set ? MAP_COLOR_GOOD : MAP_COLOR_WARN);
    map_render_status_row(panel, 208, "Map view", map_view_value,
                          map_view_color);

    char source_status[96];
    if (view_status.provider_configured) {
        snprintf(source_status, sizeof(source_status), "%s - %s - %lu MB",
                 view_status.source_id[0] != '\0' ?
                    view_status.source_id : "SD provider",
                 prefetch_value,
                 (unsigned long)prefetch.cache_budget_mb);
    } else {
        snprintf(source_status, sizeof(source_status), "%s",
                 "Built-in OpenStreetMap - interactive cache only");
    }
    lv_obj_t *built_in = map_label(
        panel, source_status,
        view_status.provider_configured ? prefetch_color : MAP_COLOR_MUTED);
    if (built_in) {
        map_label_dot(built_in, 420);
        lv_obj_set_style_text_align(built_in, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(built_in, 12, 274);
    }
    lv_obj_t *attribution = map_label(
        panel,
        view_status.provider_configured && view_status.attribution[0] != '\0' ?
            view_status.attribution : "(c) OpenStreetMap contributors",
        MAP_COLOR_DETAIL);
    if (attribution) {
        map_label_dot(attribution, 420);
        lv_obj_set_style_text_align(attribution, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(attribution, 12, 298);
    }
}

static bool map_options_page_is_valid(d1l_ui_map_options_page_t page)
{
    return page == D1L_UI_MAP_OPTIONS_ROOT ||
           page == D1L_UI_MAP_OPTIONS_CACHE_STATUS;
}

static void map_clear_location_controls(
    d1l_ui_map_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    lv_obj_t *keyboard = controller->location_controls.location_keyboard;
    if (keyboard && lv_obj_is_valid(keyboard)) {
        d1l_ui_keyboard_clear_textarea(keyboard);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    memset(&controller->location_controls, 0,
           sizeof(controller->location_controls));
}

static void map_delete_sheet(lv_obj_t **sheet)
{
    if (!sheet || !*sheet) {
        return;
    }
    if (lv_obj_is_valid(*sheet)) {
        d1l_ui_modal_hide(*sheet);
        lv_obj_del(*sheet);
    }
    *sheet = NULL;
}

static void map_destroy_sheets(d1l_ui_map_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    map_clear_location_controls(controller);
    map_delete_sheet(&controller->location_sheet);
    map_delete_sheet(&controller->options_sheet);
    memset(controller, 0, sizeof(*controller));
}

static lv_obj_t *map_create_sheet(lv_obj_t *parent)
{
    lv_obj_t *sheet = lv_obj_create(parent);
    if (!sheet) {
        return NULL;
    }
    lv_obj_set_size(sheet, 480, 424);
    lv_obj_set_pos(sheet, 0, 56);
    lv_obj_set_style_radius(sheet, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(MAP_COLOR_BG), 0);
    lv_obj_set_style_border_width(sheet, 0, 0);
    lv_obj_set_style_pad_all(sheet, 0, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    d1l_ui_modal_hide(sheet);
    return sheet;
}

bool d1l_ui_map_sheets_create(d1l_ui_map_sheets_controller_t *controller,
                               lv_obj_t *parent)
{
    if (!controller) {
        return false;
    }
    map_destroy_sheets(controller);
    if (!parent || !lv_obj_is_valid(parent)) {
        return false;
    }
    controller->location_sheet = map_create_sheet(parent);
    if (!controller->location_sheet) {
        map_destroy_sheets(controller);
        return false;
    }
    controller->options_sheet = map_create_sheet(parent);
    if (!controller->options_sheet) {
        map_destroy_sheets(controller);
        return false;
    }
    controller->options_page = D1L_UI_MAP_OPTIONS_ROOT;
    return true;
}

static void map_invalidate_options(
    d1l_ui_map_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    if (controller->options_sheet &&
        lv_obj_is_valid(controller->options_sheet)) {
        d1l_ui_modal_hide(controller->options_sheet);
        lv_obj_clean(controller->options_sheet);
    }
    controller->options_page = D1L_UI_MAP_OPTIONS_ROOT;
}

static void map_invalidate_location(
    d1l_ui_map_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    map_clear_location_controls(controller);
    if (controller->location_sheet &&
        lv_obj_is_valid(controller->location_sheet)) {
        d1l_ui_modal_hide(controller->location_sheet);
        lv_obj_clean(controller->location_sheet);
    }
}

bool d1l_ui_map_sheets_render_options(
    d1l_ui_map_sheets_controller_t *controller,
    const d1l_app_snapshot_t *snapshot,
    d1l_ui_map_options_page_t page,
    const d1l_ui_map_callbacks_t *callbacks)
{
    if (!controller || !snapshot || !callbacks ||
        !map_options_page_is_valid(page) || !controller->options_sheet ||
        !lv_obj_is_valid(controller->options_sheet)) {
        map_invalidate_options(controller);
        return false;
    }
    d1l_ui_map_render_options(controller->options_sheet, snapshot, page,
                              callbacks);
    controller->options_page = page;
    return true;
}

bool d1l_ui_map_sheets_render_location(
    d1l_ui_map_sheets_controller_t *controller,
    const d1l_app_snapshot_t *snapshot,
    int32_t latitude_e7,
    int32_t longitude_e7,
    const d1l_ui_map_callbacks_t *callbacks)
{
    if (!controller || !snapshot || !callbacks ||
        !controller->location_sheet ||
        !lv_obj_is_valid(controller->location_sheet)) {
        map_invalidate_location(controller);
        return false;
    }
    map_clear_location_controls(controller);
    d1l_ui_map_render_location(controller->location_sheet, snapshot,
                               latitude_e7, longitude_e7, callbacks,
                               &controller->location_controls);
    if (!controller->location_controls.latitude_textarea ||
        !controller->location_controls.longitude_textarea ||
        !controller->location_controls.location_keyboard ||
        !lv_obj_is_valid(controller->location_controls.latitude_textarea) ||
        !lv_obj_is_valid(controller->location_controls.longitude_textarea) ||
        !lv_obj_is_valid(controller->location_controls.location_keyboard)) {
        map_invalidate_location(controller);
        return false;
    }
    return true;
}

void d1l_ui_map_sheets_hide_location(
    d1l_ui_map_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    map_clear_location_controls(controller);
    if (controller->location_sheet &&
        lv_obj_is_valid(controller->location_sheet)) {
        d1l_ui_modal_hide(controller->location_sheet);
    }
}

void d1l_ui_map_sheets_hide_options(
    d1l_ui_map_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    if (controller->options_sheet &&
        lv_obj_is_valid(controller->options_sheet)) {
        d1l_ui_modal_hide(controller->options_sheet);
    }
    controller->options_page = D1L_UI_MAP_OPTIONS_ROOT;
}

void d1l_ui_map_sheets_deactivate(
    d1l_ui_map_sheets_controller_t *controller)
{
    d1l_ui_map_sheets_hide_location(controller);
    d1l_ui_map_sheets_hide_options(controller);
}

static lv_obj_t *map_valid_object(lv_obj_t *object)
{
    return object && lv_obj_is_valid(object) ? object : NULL;
}

lv_obj_t *d1l_ui_map_location_sheet(
    const d1l_ui_map_sheets_controller_t *controller)
{
    return controller ? map_valid_object(controller->location_sheet) : NULL;
}

lv_obj_t *d1l_ui_map_options_sheet(
    const d1l_ui_map_sheets_controller_t *controller)
{
    return controller ? map_valid_object(controller->options_sheet) : NULL;
}

lv_obj_t *d1l_ui_map_latitude_textarea(
    const d1l_ui_map_sheets_controller_t *controller)
{
    return controller ?
        map_valid_object(controller->location_controls.latitude_textarea) :
        NULL;
}

lv_obj_t *d1l_ui_map_longitude_textarea(
    const d1l_ui_map_sheets_controller_t *controller)
{
    return controller ?
        map_valid_object(controller->location_controls.longitude_textarea) :
        NULL;
}

lv_obj_t *d1l_ui_map_location_keyboard(
    const d1l_ui_map_sheets_controller_t *controller)
{
    return controller ?
        map_valid_object(controller->location_controls.location_keyboard) :
        NULL;
}

void d1l_ui_map_render_options(lv_obj_t *parent,
                               const d1l_app_snapshot_t *snapshot,
                               d1l_ui_map_options_page_t page,
                               const d1l_ui_map_callbacks_t *callbacks)
{
    if (!parent || !snapshot || !callbacks) {
        return;
    }
    lv_obj_clean(parent);
    if (page == D1L_UI_MAP_OPTIONS_CACHE_STATUS) {
        map_render_cache_status(parent, snapshot, callbacks);
    } else {
        map_render_options_root(parent, snapshot, callbacks);
    }
}

static void map_format_coordinate(char *dest, size_t dest_len, int32_t value_e7)
{
    if (!dest || dest_len == 0) {
        return;
    }
    const int64_t value = value_e7;
    const bool negative = value < 0;
    const uint64_t magnitude = (uint64_t)(negative ? -value : value);
    snprintf(dest, dest_len, "%s%llu.%07llu", negative ? "-" : "",
             (unsigned long long)(magnitude / 10000000ULL),
             (unsigned long long)(magnitude % 10000000ULL));
}

static void map_configure_textarea(lv_obj_t *textarea, const char *placeholder,
                                   const char *text, uint32_t max_length)
{
    if (!textarea) {
        return;
    }
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_max_length(textarea, max_length);
    lv_textarea_set_placeholder_text(textarea, placeholder);
    lv_textarea_set_text(textarea, text ? text : "");
    lv_obj_set_style_radius(textarea, 8, 0);
    lv_obj_set_style_bg_color(textarea, lv_color_hex(MAP_COLOR_BG), 0);
    lv_obj_set_style_border_color(textarea, lv_color_hex(MAP_COLOR_BORDER), 0);
    lv_obj_set_style_text_color(textarea, lv_color_hex(MAP_COLOR_TEXT), 0);
    lv_obj_set_style_text_color(textarea, lv_color_hex(MAP_COLOR_MUTED),
                                LV_PART_TEXTAREA_PLACEHOLDER);
}

void d1l_ui_map_render_location(lv_obj_t *parent,
                                const d1l_app_snapshot_t *snapshot,
                                int32_t latitude_e7,
                                int32_t longitude_e7,
                                const d1l_ui_map_callbacks_t *callbacks,
                                d1l_ui_map_controls_t *controls)
{
    if (!parent || !snapshot || !callbacks || !controls) {
        return;
    }
    memset(controls, 0, sizeof(*controls));
    lv_obj_clean(parent);
    map_render_header(parent, "Set location", "Map center in decimal degrees",
                      "Back", callbacks->close_location);

    char latitude[20] = "";
    char longitude[20] = "";
    if (snapshot->map_location_set) {
        map_format_coordinate(latitude, sizeof(latitude), latitude_e7);
        map_format_coordinate(longitude, sizeof(longitude), longitude_e7);
    }

    lv_obj_t *intro = map_panel(parent, 16, 64, 448, 50);
    if (intro) {
        lv_obj_t *hint = map_label(intro,
            "Set the center used for the local map area.", MAP_COLOR_MUTED);
        map_label_dot(hint, 420);
        if (hint) {
            lv_obj_set_pos(hint, 12, 16);
        }
    }
    lv_obj_t *latitude_label = map_label(parent, "Latitude", MAP_COLOR_GOOD);
    if (latitude_label) {
        lv_obj_set_pos(latitude_label, 28, 124);
    }
    controls->latitude_textarea = lv_textarea_create(parent);
    if (controls->latitude_textarea) {
        lv_obj_set_size(controls->latitude_textarea, 448, 48);
        lv_obj_set_pos(controls->latitude_textarea, 16, 146);
        map_configure_textarea(controls->latitude_textarea,
                               "Range: -90 to 90", latitude, 14);
        if (callbacks->location_textarea) {
            lv_obj_add_event_cb(controls->latitude_textarea, callbacks->location_textarea,
                                LV_EVENT_FOCUSED, NULL);
            lv_obj_add_event_cb(controls->latitude_textarea, callbacks->location_textarea,
                                LV_EVENT_CLICKED, NULL);
        }
    }
    lv_obj_t *longitude_label = map_label(parent, "Longitude", MAP_COLOR_GOOD);
    if (longitude_label) {
        lv_obj_set_pos(longitude_label, 28, 206);
    }
    controls->longitude_textarea = lv_textarea_create(parent);
    if (controls->longitude_textarea) {
        lv_obj_set_size(controls->longitude_textarea, 448, 48);
        lv_obj_set_pos(controls->longitude_textarea, 16, 228);
        map_configure_textarea(controls->longitude_textarea,
                               "Range: -180 to 180", longitude, 15);
        if (callbacks->location_textarea) {
            lv_obj_add_event_cb(controls->longitude_textarea, callbacks->location_textarea,
                                LV_EVENT_FOCUSED, NULL);
            lv_obj_add_event_cb(controls->longitude_textarea, callbacks->location_textarea,
                                LV_EVENT_CLICKED, NULL);
        }
    }
    lv_obj_t *keyboard_hint = map_panel(parent, 16, 288, 448, 54);
    if (keyboard_hint) {
        lv_obj_t *hint = map_label(keyboard_hint,
            "The keyboard opens only while editing a coordinate.", MAP_COLOR_MUTED);
        map_label_dot(hint, 420);
        if (hint) {
            lv_obj_set_pos(hint, 12, 18);
        }
    }
    const int save_width = snapshot->map_location_set ? 216 : 448;
    lv_obj_t *save = map_button(parent, "Save location", 16, 356, save_width, 52,
                                callbacks->save_location);
    map_style_primary_button(save);
    if (snapshot->map_location_set) {
        lv_obj_t *clear = map_button(parent, "Clear location", 248, 356, 216, 52,
                                     callbacks->clear_location);
        if (clear) {
            lv_obj_set_style_border_color(clear, lv_color_hex(MAP_COLOR_WARN), 0);
            lv_obj_set_style_border_width(clear, 1, 0);
        }
    }
    controls->location_keyboard = lv_keyboard_create(parent);
    if (controls->location_keyboard) {
        d1l_ui_keyboard_configure_input(controls->location_keyboard,
                                        controls->latitude_textarea,
                                        16, 274, 448, 150);
        lv_obj_add_flag(controls->location_keyboard, LV_OBJ_FLAG_HIDDEN);
        if (callbacks->location_keyboard) {
            lv_obj_add_event_cb(controls->location_keyboard,
                                callbacks->location_keyboard, LV_EVENT_READY, NULL);
            lv_obj_add_event_cb(controls->location_keyboard,
                                callbacks->location_keyboard, LV_EVENT_CANCEL, NULL);
        }
    }
}

void d1l_ui_map_viewport_release(void)
{
    uint32_t generation;
    portENTER_CRITICAL(&s_viewport_lease_lock);
    generation = s_viewport_generation;
    s_viewport_generation = 0U;
    s_viewport_revision = 0U;
    s_viewport_drag_active = false;
    s_viewport_drag_x = 0;
    s_viewport_drag_y = 0;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    /* release/suppression can be requested from a non-UI task. Keep this path
     * LVGL-free; the next UI refresh/reacquire/render hides or rebuilds the
     * lightweight marker layer before it can be interacted with. */
    if (generation != 0U) {
        d1l_map_view_service_release_visible(generation);
    }
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
void d1l_ui_map_viewport_prepare_acceptance(void)
{
    portENTER_CRITICAL(&s_viewport_lease_lock);
    s_viewport_acceptance_reset_pending = true;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    d1l_ui_map_viewport_release();
}
#endif

void d1l_ui_map_viewport_prepare_cover(void)
{
    map_viewport_hide_markers();
    map_viewport_reset_image_position();
}

void d1l_ui_map_viewport_set_suppressed(bool suppressed)
{
    portENTER_CRITICAL(&s_viewport_lease_lock);
    if (suppressed) {
        if (s_viewport_suppression_depth != UINT32_MAX) {
            ++s_viewport_suppression_depth;
        }
    } else if (s_viewport_suppression_depth > 0U) {
        --s_viewport_suppression_depth;
    }
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    if (suppressed) {
        d1l_ui_map_viewport_release();
    }
}

void d1l_ui_map_viewport_suppress_next_acquire(void)
{
    portENTER_CRITICAL(&s_viewport_lease_lock);
    s_viewport_suppress_next_acquire = true;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    d1l_ui_map_viewport_release();
}

bool d1l_ui_map_viewport_lease_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_viewport_lease_lock);
    active = s_viewport_generation != 0U && s_viewport_suppression_depth == 0U;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    return active;
}

uint32_t d1l_ui_map_viewport_generation(void)
{
    uint32_t generation;
    portENTER_CRITICAL(&s_viewport_lease_lock);
    generation = s_viewport_generation;
    portEXIT_CRITICAL(&s_viewport_lease_lock);
    return generation;
}
