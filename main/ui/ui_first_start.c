#include "ui_first_start.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "lvgl.h"
#include "ui_keyboard.h"

#define D1L_FIRST_START_COLOR_BACKGROUND 0x071018U
#define D1L_FIRST_START_COLOR_PANEL 0x111923U
#define D1L_FIRST_START_COLOR_TEXT 0xF4F7FBU
#define D1L_FIRST_START_COLOR_MUTED 0x93A4B5U
#define D1L_FIRST_START_COLOR_GREEN 0x4ADE80U
#define D1L_FIRST_START_COLOR_AMBER 0xFBBF24U
#define D1L_FIRST_START_COLOR_RED 0xFB7185U
#define D1L_FIRST_START_COLOR_CYAN 0x22D3EEU
#define D1L_FIRST_START_READY_HOLD_MS 650U
#define D1L_FIRST_START_CANADIAN_FREQUENCY_HZ 910525000UL
#define D1L_FIRST_START_CANADIAN_BANDWIDTH_TENTHS_KHZ 625U
#define D1L_FIRST_START_CANADIAN_SF 7U
#define D1L_FIRST_START_CANADIAN_CR 5U

enum {
    BINDING_BACK = 0,
    BINDING_SKIP,
    BINDING_NEXT,
    BINDING_PRIMARY_FOCUS,
    BINDING_SECONDARY_FOCUS,
    BINDING_KEYBOARD,
};

static void handle_action(d1l_ui_first_start_controller_t *controller,
                          d1l_ui_first_start_action_t action);

static bool object_valid(const lv_obj_t *object)
{
    return object && lv_obj_is_valid(object);
}

static const char *textarea_text_or_empty(lv_obj_t *textarea)
{
    return object_valid(textarea) ? lv_textarea_get_text(textarea) : "";
}

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    if (!destination || destination_size == 0U) {
        return;
    }
    (void)snprintf(destination, destination_size, "%s",
                   source ? source : "");
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text,
                              uint32_t color, int x, int y, int width,
                              bool wrap)
{
    if (!parent || !text) {
        return NULL;
    }
    lv_obj_t *label = lv_label_create(parent);
    if (!label) {
        return NULL;
    }
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_long_mode(
        label, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    return label;
}

static void advance_generation(d1l_ui_first_start_controller_t *controller)
{
    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
    }
}

static d1l_ui_first_start_binding_t *set_binding(
    d1l_ui_first_start_controller_t *controller, size_t slot,
    d1l_ui_first_start_action_t action)
{
    if (!controller || slot >= D1L_UI_FIRST_START_BINDING_COUNT) {
        return NULL;
    }
    d1l_ui_first_start_binding_t *binding = &controller->bindings[slot];
    binding->controller = controller;
    binding->action = action;
    binding->generation = controller->generation;
    return binding;
}

static bool binding_current(const d1l_ui_first_start_binding_t *binding)
{
    return binding && binding->controller &&
        binding->generation != 0U &&
        binding->generation == binding->controller->generation;
}

static void action_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_first_start_binding_t *binding =
        (d1l_ui_first_start_binding_t *)lv_event_get_user_data(event);
    if (!binding_current(binding)) {
        return;
    }
    handle_action(binding->controller, binding->action);
}

static void focus_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_first_start_binding_t *binding =
        (d1l_ui_first_start_binding_t *)lv_event_get_user_data(event);
    if (!binding_current(binding)) {
        return;
    }
    d1l_ui_first_start_controller_t *controller = binding->controller;
    (void)d1l_ui_keyboard_focus_textarea_from_event(
        controller->keyboard, event, controller->primary_textarea,
        controller->secondary_textarea);
}

static void keyboard_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_first_start_binding_t *binding =
        (d1l_ui_first_start_binding_t *)lv_event_get_user_data(event);
    if (!binding_current(binding)) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        handle_action(binding->controller, D1L_UI_FIRST_START_ACTION_NEXT);
    } else if (code == LV_EVENT_CANCEL) {
        handle_action(binding->controller, D1L_UI_FIRST_START_ACTION_BACK);
    }
}

static lv_obj_t *create_button(
    d1l_ui_first_start_controller_t *controller, const char *text,
    int x, int width, size_t binding_slot,
    d1l_ui_first_start_action_t action, bool primary)
{
    if (!controller || !controller->overlay || !text) {
        return NULL;
    }
    lv_obj_t *button = lv_btn_create(controller->overlay);
    if (!button) {
        return NULL;
    }
    lv_obj_set_pos(button, x, 424);
    lv_obj_set_size(button, width, 48);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(
        button,
        lv_color_hex(primary ? 0x0E7490U : 0x1E2A36U), 0);
    lv_obj_set_style_bg_color(
        button,
        lv_color_hex(primary ? 0x0891B2U : 0x2A3A4AU),
        LV_STATE_PRESSED);
    lv_obj_t *label = create_label(
        button, text, D1L_FIRST_START_COLOR_TEXT, 0, 0, width - 8, false);
    if (label) {
        lv_obj_center(label);
    }
    d1l_ui_first_start_binding_t *binding =
        set_binding(controller, binding_slot, action);
    if (binding) {
        lv_obj_add_event_cb(button, action_event_cb, LV_EVENT_CLICKED,
                            binding);
    }
    return button;
}

static void clear_sensitive_input(
    d1l_ui_first_start_controller_t *controller)
{
    if (!controller) {
        return;
    }
    if (object_valid(controller->keyboard)) {
        d1l_ui_keyboard_clear_textarea(controller->keyboard);
    }
    if (controller->stage == D1L_UI_FIRST_START_WIFI &&
        object_valid(controller->secondary_textarea)) {
        lv_textarea_set_text(controller->secondary_textarea, "");
    }
}

static void clear_page_references(
    d1l_ui_first_start_controller_t *controller)
{
    controller->title = NULL;
    controller->subtitle = NULL;
    controller->progress_label = NULL;
    controller->progress_bar = NULL;
    memset(controller->readiness_rows, 0,
           sizeof(controller->readiness_rows));
    controller->media_line = NULL;
    controller->status_line = NULL;
    controller->primary_textarea = NULL;
    controller->secondary_textarea = NULL;
    controller->keyboard = NULL;
    controller->back_button = NULL;
    controller->skip_button = NULL;
    controller->next_button = NULL;
}

static void begin_page(d1l_ui_first_start_controller_t *controller,
                       d1l_ui_first_start_stage_t stage)
{
    clear_sensitive_input(controller);
    advance_generation(controller);
    memset(controller->bindings, 0, sizeof(controller->bindings));
    lv_obj_clean(controller->overlay);
    clear_page_references(controller);
    controller->stage = stage;
    lv_obj_clear_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(controller->overlay);
}

static void create_header(d1l_ui_first_start_controller_t *controller,
                          const char *title, const char *step)
{
    controller->title = create_label(
        controller->overlay, title, D1L_FIRST_START_COLOR_TEXT,
        16, 14, 448, false);
    if (controller->title) {
        lv_obj_set_style_text_font(
            controller->title, &lv_font_montserrat_24, 0);
    }
    controller->subtitle = create_label(
        controller->overlay, step, D1L_FIRST_START_COLOR_CYAN,
        16, 48, 448, false);
}

static void create_navigation(
    d1l_ui_first_start_controller_t *controller, bool allow_back,
    bool allow_skip, const char *next_text)
{
    controller->back_button = create_button(
        controller, "Back", 8, 104, BINDING_BACK,
        D1L_UI_FIRST_START_ACTION_BACK, false);
    if (!allow_back && controller->back_button) {
        lv_obj_add_state(controller->back_button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(
            controller->back_button, LV_OPA_40, LV_STATE_DISABLED);
    }
    if (allow_skip) {
        controller->skip_button = create_button(
            controller, "Skip", 120, 104, BINDING_SKIP,
            D1L_UI_FIRST_START_ACTION_SKIP, false);
        controller->next_button = create_button(
            controller, next_text, 232, 240, BINDING_NEXT,
            D1L_UI_FIRST_START_ACTION_NEXT, true);
    } else {
        controller->next_button = create_button(
            controller, next_text, 120, 352, BINDING_NEXT,
            D1L_UI_FIRST_START_ACTION_NEXT, true);
    }
}

static lv_obj_t *create_textarea(
    d1l_ui_first_start_controller_t *controller, int y,
    const char *placeholder, const char *value, size_t maximum_length,
    bool password, size_t binding_slot)
{
    lv_obj_t *textarea = lv_textarea_create(controller->overlay);
    if (!textarea) {
        return NULL;
    }
    lv_obj_set_pos(textarea, 16, y);
    lv_obj_set_size(textarea, 448, 44);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_password_mode(textarea, password);
    lv_textarea_set_max_length(textarea, (uint32_t)maximum_length);
    lv_textarea_set_placeholder_text(textarea, placeholder);
    lv_textarea_set_text(textarea, value ? value : "");
    lv_obj_set_style_radius(textarea, 8, 0);
    lv_obj_set_style_bg_color(
        textarea, lv_color_hex(D1L_FIRST_START_COLOR_PANEL), 0);
    lv_obj_set_style_border_color(
        textarea, lv_color_hex(0x334155U), 0);
    lv_obj_set_style_text_color(
        textarea, lv_color_hex(D1L_FIRST_START_COLOR_TEXT), 0);
    d1l_ui_first_start_binding_t *binding =
        set_binding(controller, binding_slot,
                    binding_slot == BINDING_PRIMARY_FOCUS ?
                        D1L_UI_FIRST_START_ACTION_FOCUS_PRIMARY :
                        D1L_UI_FIRST_START_ACTION_FOCUS_SECONDARY);
    if (binding) {
        lv_obj_add_event_cb(
            textarea, focus_event_cb, LV_EVENT_FOCUSED, binding);
        lv_obj_add_event_cb(
            textarea, focus_event_cb, LV_EVENT_CLICKED, binding);
    }
    return textarea;
}

static void create_keyboard(d1l_ui_first_start_controller_t *controller,
                            lv_obj_t *initial_textarea)
{
    controller->keyboard = lv_keyboard_create(controller->overlay);
    if (!controller->keyboard) {
        return;
    }
    d1l_ui_keyboard_configure_compose(controller->keyboard);
    d1l_ui_keyboard_configure_input(
        controller->keyboard, initial_textarea, 8, 214, 464, 202);
    d1l_ui_first_start_binding_t *binding = set_binding(
        controller, BINDING_KEYBOARD,
        D1L_UI_FIRST_START_ACTION_KEYBOARD);
    if (binding) {
        lv_obj_add_event_cb(
            controller->keyboard, keyboard_event_cb, LV_EVENT_READY, binding);
        lv_obj_add_event_cb(
            controller->keyboard, keyboard_event_cb, LV_EVENT_CANCEL, binding);
    }
}

static void set_status(d1l_ui_first_start_controller_t *controller,
                       const char *text, uint32_t color)
{
    if (!controller || !object_valid(controller->status_line)) {
        return;
    }
    lv_label_set_text(controller->status_line, text ? text : "");
    lv_obj_set_style_text_color(
        controller->status_line, lv_color_hex(color), 0);
}

static void render_readiness(
    d1l_ui_first_start_controller_t *controller)
{
    begin_page(controller, D1L_UI_FIRST_START_READINESS);
    create_header(controller, "DeskOS is getting ready", "STARTUP CHECK");
    controller->progress_label = create_label(
        controller->overlay, "0 of 5 essential systems ready",
        D1L_FIRST_START_COLOR_MUTED, 16, 78, 448, false);
    controller->progress_bar = lv_bar_create(controller->overlay);
    if (controller->progress_bar) {
        lv_obj_set_pos(controller->progress_bar, 16, 105);
        lv_obj_set_size(controller->progress_bar, 448, 8);
        lv_bar_set_range(controller->progress_bar, 0, 5);
        lv_bar_set_value(controller->progress_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(
            controller->progress_bar, lv_color_hex(0x1E293BU),
            LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            controller->progress_bar,
            lv_color_hex(D1L_FIRST_START_COLOR_CYAN),
            LV_PART_INDICATOR);
    }
    static const char *const names[] = {
        "Display", "Identity", "Radio", "Storage", "UI",
    };
    for (size_t i = 0U; i < 5U; ++i) {
        controller->readiness_rows[i] = create_label(
            controller->overlay, names[i], D1L_FIRST_START_COLOR_MUTED,
            20, 130 + (int)(i * 42U), 440, false);
    }
    controller->media_line = create_label(
        controller->overlay,
        "SD/maps: checking prepared media and provider",
        D1L_FIRST_START_COLOR_AMBER, 16, 350, 448, true);
    controller->status_line = create_label(
        controller->overlay,
        "Home stays covered until every essential row is green.",
        D1L_FIRST_START_COLOR_MUTED, 16, 392, 448, true);
}

static void render_name(d1l_ui_first_start_controller_t *controller)
{
    begin_page(controller, D1L_UI_FIRST_START_NAME);
    create_header(controller, "Name this desk", "STEP 1 OF 6 - REQUIRED");
    create_label(
        controller->overlay,
        "Enter the name people will see on MeshCore. DeskOS will not "
        "invent or auto-complete it.",
        D1L_FIRST_START_COLOR_MUTED, 16, 72, 448, true);
    controller->primary_textarea = create_textarea(
        controller, 124, "Required: 1-31 characters", controller->node_name,
        D1L_NODE_NAME_LEN - 1U, false, BINDING_PRIMARY_FOCUS);
    controller->status_line = create_label(
        controller->overlay, "A non-blank explicit name is required.",
        D1L_FIRST_START_COLOR_MUTED, 16, 178, 448, false);
    create_keyboard(controller, controller->primary_textarea);
    create_navigation(controller, false, false, "Next");
}

static void render_location(d1l_ui_first_start_controller_t *controller)
{
    begin_page(controller, D1L_UI_FIRST_START_LOCATION);
    create_header(controller, "Set map location", "STEP 2 OF 6 - OPTIONAL");
    create_label(
        controller->overlay,
        "Enter decimal latitude and longitude manually. No coordinates "
        "are assumed or baked into the device.",
        D1L_FIRST_START_COLOR_MUTED, 16, 68, 448, true);
    controller->primary_textarea = create_textarea(
        controller, 112, "Latitude (-90 to 90)", controller->latitude,
        sizeof(controller->latitude) - 1U, false, BINDING_PRIMARY_FOCUS);
    controller->secondary_textarea = create_textarea(
        controller, 160, "Longitude (-180 to 180)", controller->longitude,
        sizeof(controller->longitude) - 1U, false,
        BINDING_SECONDARY_FOCUS);
    if (controller->primary_textarea) {
        lv_textarea_set_accepted_chars(
            controller->primary_textarea, "+-0123456789.");
    }
    if (controller->secondary_textarea) {
        lv_textarea_set_accepted_chars(
            controller->secondary_textarea, "+-0123456789.");
    }
    controller->status_line = create_label(
        controller->overlay,
        "Both fields are required together, or choose Skip.",
        D1L_FIRST_START_COLOR_MUTED, 16, 196, 448, false);
    create_keyboard(controller, controller->primary_textarea);
    create_navigation(controller, true, true, "Save & Next");
}

static void render_wifi(d1l_ui_first_start_controller_t *controller)
{
    begin_page(controller, D1L_UI_FIRST_START_WIFI);
    create_header(controller, "Connect Wi-Fi", "STEP 3 OF 6 - OPTIONAL");
    create_label(
        controller->overlay,
        "Enter a network or choose Skip for offline use. Password text is "
        "masked and wiped as soon as this page is left.",
        D1L_FIRST_START_COLOR_MUTED, 16, 68, 448, true);
    controller->primary_textarea = create_textarea(
        controller, 112, "Wi-Fi network name", controller->wifi_ssid,
        D1L_WIFI_SSID_LEN - 1U, false, BINDING_PRIMARY_FOCUS);
    controller->secondary_textarea = create_textarea(
        controller, 160, "Wi-Fi password", "",
        D1L_WIFI_PASSWORD_LEN - 1U, true, BINDING_SECONDARY_FOCUS);
    controller->status_line = create_label(
        controller->overlay,
        controller->wifi_saved ?
            "Profile saved securely. Next keeps it; enter a password to replace." :
            "Skip keeps Wi-Fi off. You can configure it later.",
        D1L_FIRST_START_COLOR_MUTED, 16, 196, 448, false);
    create_keyboard(controller, controller->primary_textarea);
    create_navigation(controller, true, true, "Save & Next");
}

static void render_radio(d1l_ui_first_start_controller_t *controller)
{
    begin_page(controller, D1L_UI_FIRST_START_RADIO);
    create_header(controller, "Confirm Canadian radio", "STEP 4 OF 6");
    create_label(
        controller->overlay,
        "This production preset is fixed for the initial setup:",
        D1L_FIRST_START_COLOR_MUTED, 16, 82, 448, false);
    lv_obj_t *preset = create_label(
        controller->overlay,
        "910.525 MHz\nBandwidth 62.5 kHz\nSpreading factor 7\n"
        "Coding rate 5",
        D1L_FIRST_START_COLOR_TEXT, 24, 124, 432, true);
    if (preset) {
        lv_obj_set_style_text_font(preset, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_line_space(preset, 10, 0);
    }
    create_label(
        controller->overlay,
        "Confirm & Next saves this preset through the normal radio settings "
        "API. No message is transmitted.",
        D1L_FIRST_START_COLOR_MUTED, 16, 292, 448, true);
    controller->status_line = create_label(
        controller->overlay,
        controller->radio_confirmed ? "Canadian preset saved." :
            "Confirmation is required before setup can finish.",
        controller->radio_confirmed ? D1L_FIRST_START_COLOR_GREEN :
            D1L_FIRST_START_COLOR_AMBER,
        16, 364, 448, true);
    create_navigation(controller, true, false, "Confirm & Next");
}

static void render_storage_map(
    d1l_ui_first_start_controller_t *controller)
{
    begin_page(controller, D1L_UI_FIRST_START_STORAGE_MAP);
    create_header(controller, "Verify storage & maps", "STEP 5 OF 6 - REQUIRED");
    create_label(
        controller->overlay,
        "1.0 requires a prepared FAT32 SD card. Prepare it on a computer, "
        "then insert it. DeskOS firmware never formats cards.",
        D1L_FIRST_START_COLOR_MUTED, 16, 74, 448, true);
    create_label(
        controller->overlay,
        "NRCan tiles use map/offline-provider.json. The prepared-card tool "
        "installs the authorized Natural Resources Canada provider manifest.",
        D1L_FIRST_START_COLOR_MUTED, 16, 154, 448, true);
    controller->media_line = create_label(
        controller->overlay, "SD card: checking...",
        D1L_FIRST_START_COLOR_AMBER, 16, 246, 448, true);
    controller->status_line = create_label(
        controller->overlay, "NRCan provider: checking...",
        D1L_FIRST_START_COLOR_AMBER, 16, 302, 448, true);
    create_label(
        controller->overlay,
        "Continue unlocks when both the prepared card and NRCan provider "
        "manifest are ready.",
        D1L_FIRST_START_COLOR_MUTED, 16, 370, 448, true);
    create_navigation(controller, true, false, "Continue");
    if (controller->next_button && !controller->media_ready) {
        lv_obj_add_state(controller->next_button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(
            controller->next_button, LV_OPA_40, LV_STATE_DISABLED);
    }
}

static void render_channels(d1l_ui_first_start_controller_t *controller)
{
    begin_page(controller, D1L_UI_FIRST_START_CHANNELS);
    create_header(controller, "Your starting channels", "STEP 6 OF 6");
    create_label(
        controller->overlay,
        "DeskOS will create these standard channels at first completion:",
        D1L_FIRST_START_COLOR_MUTED, 16, 82, 448, true);
    lv_obj_t *channels = create_label(
        controller->overlay,
        "Public   selected by default\n#bot      bot traffic\n"
        "#test     community traffic",
        D1L_FIRST_START_COLOR_TEXT, 24, 134, 432, true);
    if (channels) {
        lv_obj_set_style_text_font(channels, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_line_space(channels, 14, 0);
    }
    create_label(
        controller->overlay,
        "Finish persists your explicit name and seeds the channels through "
        "the canonical onboarding API. Existing configured users are never "
        "sent through this wizard.",
        D1L_FIRST_START_COLOR_MUTED, 16, 290, 448, true);
    controller->status_line = create_label(
        controller->overlay, "Ready to finish setup.",
        D1L_FIRST_START_COLOR_GREEN, 16, 374, 448, false);
    create_navigation(controller, true, false, "Finish setup");
}

static void render_finishing(
    d1l_ui_first_start_controller_t *controller)
{
    begin_page(controller, D1L_UI_FIRST_START_FINISHING);
    create_header(controller, "Finishing setup", "SAVING");
    create_label(
        controller->overlay,
        "DeskOS is persisting your identity, name, and initial channels. "
        "The Home screen will open when the saved onboarding state is "
        "confirmed.",
        D1L_FIRST_START_COLOR_MUTED, 16, 110, 448, true);
    controller->status_line = create_label(
        controller->overlay, "Please wait...",
        D1L_FIRST_START_COLOR_CYAN, 16, 220, 448, false);
}

bool d1l_ui_first_start_parse_coordinate_e7(
    const char *text, int32_t minimum, int32_t maximum, int32_t *out_value)
{
    if (!text || !out_value || minimum > maximum) {
        return false;
    }
    while (*text == ' ') {
        ++text;
    }
    bool negative = false;
    if (*text == '+' || *text == '-') {
        negative = *text == '-';
        ++text;
    }
    int64_t whole = 0;
    size_t whole_digits = 0U;
    while (*text >= '0' && *text <= '9') {
        if (whole > 1000) {
            return false;
        }
        whole = whole * 10 + (int64_t)(*text - '0');
        ++whole_digits;
        ++text;
    }
    int64_t fraction = 0;
    size_t fraction_digits = 0U;
    if (*text == '.') {
        ++text;
        while (*text >= '0' && *text <= '9') {
            if (fraction_digits >= 7U) {
                return false;
            }
            fraction = fraction * 10 + (int64_t)(*text - '0');
            ++fraction_digits;
            ++text;
        }
    }
    if (whole_digits == 0U && fraction_digits == 0U) {
        return false;
    }
    while (*text == ' ') {
        ++text;
    }
    if (*text != '\0') {
        return false;
    }
    while (fraction_digits < 7U) {
        fraction *= 10;
        ++fraction_digits;
    }
    int64_t scaled = whole * 10000000LL + fraction;
    if (negative) {
        scaled = -scaled;
    }
    if (scaled < minimum || scaled > maximum) {
        return false;
    }
    *out_value = (int32_t)scaled;
    return true;
}

bool d1l_ui_first_start_sd_prepared(
    const d1l_app_snapshot_t *snapshot)
{
    return snapshot && snapshot->storage_sd_present &&
        snapshot->storage_sd_mounted &&
        snapshot->storage_sd_data_root_ready &&
        !snapshot->storage_sd_needs_fat32;
}

bool d1l_ui_first_start_map_prepared(
    const d1l_app_snapshot_t *snapshot)
{
    return d1l_ui_first_start_sd_prepared(snapshot) &&
        snapshot->map_tile_cache_ready &&
        snapshot->map_tile_provider_configured;
}

bool d1l_ui_first_start_essential_ready(
    const d1l_app_snapshot_t *snapshot)
{
    if (!snapshot) {
        return false;
    }
    const bool storage_service_ready =
        !snapshot->release_capabilities.sd_history ||
        !snapshot->storage_rp2040_bridge_required ||
        snapshot->storage_direct_supported ||
        snapshot->storage_rp2040_bridge_ready;
    const bool storage_and_maps_ready =
        storage_service_ready &&
        (!snapshot->onboarding_complete ||
         d1l_ui_first_start_map_prepared(snapshot));
    return snapshot->board_ready &&
        snapshot->identity_ready &&
        snapshot->radio_ready &&
        snapshot->radio_applied &&
        !snapshot->radio_apply_pending &&
        storage_and_maps_ready &&
        snapshot->ui_ready;
}

static void update_readiness_rows(
    d1l_ui_first_start_controller_t *controller,
    const d1l_app_snapshot_t *snapshot)
{
    const bool storage_service_ready =
        !snapshot->release_capabilities.sd_history ||
        !snapshot->storage_rp2040_bridge_required ||
        snapshot->storage_direct_supported ||
        snapshot->storage_rp2040_bridge_ready;
    const bool values[5] = {
        snapshot->board_ready,
        snapshot->identity_ready,
        snapshot->radio_ready && snapshot->radio_applied &&
            !snapshot->radio_apply_pending,
        storage_service_ready &&
            (!snapshot->onboarding_complete ||
             d1l_ui_first_start_map_prepared(snapshot)),
        snapshot->ui_ready,
    };
    const char *const names[5] = {
        "Display", "Identity", "Radio",
        snapshot->onboarding_complete ? "Storage & maps" : "Storage service",
        "UI",
    };
    unsigned ready_count = 0U;
    for (size_t i = 0U; i < 5U; ++i) {
        ready_count += values[i] ? 1U : 0U;
        if (object_valid(controller->readiness_rows[i])) {
            char line[64];
            (void)snprintf(
                line, sizeof(line), "%s   %s", names[i],
                values[i] ? "Ready" : "Starting...");
            lv_label_set_text(controller->readiness_rows[i], line);
            lv_obj_set_style_text_color(
                controller->readiness_rows[i],
                lv_color_hex(values[i] ? D1L_FIRST_START_COLOR_GREEN :
                    D1L_FIRST_START_COLOR_MUTED), 0);
        }
    }
    if (object_valid(controller->progress_label)) {
        char progress[64];
        (void)snprintf(
            progress, sizeof(progress),
            "%u of 5 essential systems ready", ready_count);
        lv_label_set_text(controller->progress_label, progress);
    }
    if (object_valid(controller->progress_bar)) {
        lv_bar_set_value(
            controller->progress_bar, (int32_t)ready_count, LV_ANIM_ON);
    }
    if (object_valid(controller->media_line)) {
        const bool sd_ready = d1l_ui_first_start_sd_prepared(snapshot);
        const bool map_ready = d1l_ui_first_start_map_prepared(snapshot);
        char media[112];
        (void)snprintf(
            media, sizeof(media), "Prepared SD: %s   NRCan maps: %s",
            sd_ready ? "Ready" :
                (snapshot->storage_sd_needs_fat32 ?
                    "Needs FAT32" : "Not ready"),
            map_ready ? "Ready" : "Not ready");
        lv_label_set_text(controller->media_line, media);
        lv_obj_set_style_text_color(
            controller->media_line,
            lv_color_hex(sd_ready && map_ready ?
                D1L_FIRST_START_COLOR_GREEN :
                D1L_FIRST_START_COLOR_AMBER), 0);
    }
}

static void update_storage_map_page(
    d1l_ui_first_start_controller_t *controller,
    const d1l_app_snapshot_t *snapshot)
{
    const bool sd_ready = d1l_ui_first_start_sd_prepared(snapshot);
    const bool map_ready = d1l_ui_first_start_map_prepared(snapshot);
    controller->media_ready = sd_ready && map_ready;
    if (object_valid(controller->media_line)) {
        const char *detail = "not detected; prepare FAT32 externally";
        if (snapshot->storage_sd_needs_fat32) {
            detail = "detected, but FAT32 preparation is required";
        } else if (sd_ready) {
            detail = "prepared FAT32 data root is ready";
        } else if (snapshot->storage_sd_present) {
            detail = "detected, but not ready";
        }
        char line[128];
        (void)snprintf(line, sizeof(line), "SD card: %s", detail);
        lv_label_set_text(controller->media_line, line);
        lv_obj_set_style_text_color(
            controller->media_line,
            lv_color_hex(sd_ready ? D1L_FIRST_START_COLOR_GREEN :
                D1L_FIRST_START_COLOR_AMBER), 0);
    }
    if (object_valid(controller->status_line)) {
        lv_label_set_text(
            controller->status_line,
            map_ready ?
                "NRCan provider: ready for authorized offline tiles" :
                "NRCan provider: not ready; install the prepared-card manifest");
        lv_obj_set_style_text_color(
            controller->status_line,
            lv_color_hex(map_ready ? D1L_FIRST_START_COLOR_GREEN :
                D1L_FIRST_START_COLOR_AMBER), 0);
    }
    if (object_valid(controller->next_button)) {
        if (controller->media_ready) {
            lv_obj_clear_state(
                controller->next_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(
                controller->next_button, LV_STATE_DISABLED);
            lv_obj_set_style_opa(
                controller->next_button, LV_OPA_40, LV_STATE_DISABLED);
        }
    }
}

static void save_name_and_advance(
    d1l_ui_first_start_controller_t *controller)
{
    copy_text(controller->node_name, sizeof(controller->node_name),
              textarea_text_or_empty(controller->primary_textarea));
    if (!d1l_settings_node_name_valid(controller->node_name) ||
        strcmp(controller->node_name, D1L_NODE_NAME_FACTORY_DEFAULT) == 0) {
        set_status(
            controller,
            "Enter your own non-blank name; the factory placeholder is not accepted.",
            D1L_FIRST_START_COLOR_RED);
        return;
    }
    render_location(controller);
}

static void save_location_and_advance(
    d1l_ui_first_start_controller_t *controller)
{
    copy_text(controller->latitude, sizeof(controller->latitude),
              textarea_text_or_empty(controller->primary_textarea));
    copy_text(controller->longitude, sizeof(controller->longitude),
              textarea_text_or_empty(controller->secondary_textarea));
    int32_t latitude_e7 = 0;
    int32_t longitude_e7 = 0;
    if (!d1l_ui_first_start_parse_coordinate_e7(
            controller->latitude, -900000000, 900000000, &latitude_e7) ||
        !d1l_ui_first_start_parse_coordinate_e7(
            controller->longitude, -1800000000, 1800000000,
            &longitude_e7)) {
        set_status(
            controller,
            "Enter valid decimal latitude and longitude, or choose Skip.",
            D1L_FIRST_START_COLOR_RED);
        return;
    }
    const esp_err_t ret =
        d1l_app_model_set_map_location(latitude_e7, longitude_e7);
    if (ret != ESP_OK) {
        char error[96];
        (void)snprintf(
            error, sizeof(error), "Location was not saved: %s",
            esp_err_to_name(ret));
        set_status(controller, error, D1L_FIRST_START_COLOR_RED);
        return;
    }
    controller->location_saved = true;
    render_wifi(controller);
}

static void save_wifi_and_advance(
    d1l_ui_first_start_controller_t *controller)
{
    const char *ssid =
        textarea_text_or_empty(controller->primary_textarea);
    const char *password =
        textarea_text_or_empty(controller->secondary_textarea);
    if (!ssid || ssid[0] == '\0') {
        set_status(
            controller, "Enter a Wi-Fi network name or choose Skip.",
            D1L_FIRST_START_COLOR_RED);
        return;
    }
    if (controller->wifi_saved && (!password || password[0] == '\0') &&
        strcmp(ssid, controller->wifi_ssid) == 0) {
        render_radio(controller);
        return;
    }
    const esp_err_t save_ret =
        d1l_app_model_save_wifi_profile(ssid, password ? password : "");
    if (save_ret != ESP_OK) {
        clear_sensitive_input(controller);
        char error[96];
        (void)snprintf(
            error, sizeof(error), "Wi-Fi profile was not saved: %s",
            esp_err_to_name(save_ret));
        set_status(controller, error, D1L_FIRST_START_COLOR_RED);
        return;
    }
    copy_text(controller->wifi_ssid, sizeof(controller->wifi_ssid), ssid);
    clear_sensitive_input(controller);
    const esp_err_t enable_ret = d1l_app_model_set_wifi_enabled(true);
    if (enable_ret != ESP_OK) {
        char error[112];
        (void)snprintf(
            error, sizeof(error),
            "Profile saved, but Wi-Fi could not be enabled: %s. You may Skip.",
            esp_err_to_name(enable_ret));
        controller->wifi_saved = true;
        set_status(controller, error, D1L_FIRST_START_COLOR_RED);
        return;
    }
    controller->wifi_saved = true;
    controller->offline_selected = false;
    render_radio(controller);
}

static void confirm_radio_and_advance(
    d1l_ui_first_start_controller_t *controller)
{
    d1l_app_radio_profile_edit_t profile = {0};
    d1l_app_model_default_radio_profile(&profile);
    if (profile.frequency_hz !=
            D1L_FIRST_START_CANADIAN_FREQUENCY_HZ ||
        profile.bandwidth_tenths_khz !=
            D1L_FIRST_START_CANADIAN_BANDWIDTH_TENTHS_KHZ ||
        profile.spreading_factor != D1L_FIRST_START_CANADIAN_SF ||
        profile.coding_rate != D1L_FIRST_START_CANADIAN_CR) {
        set_status(
            controller,
            "The built-in radio preset does not match the required Canadian profile.",
            D1L_FIRST_START_COLOR_RED);
        return;
    }
    const esp_err_t ret = d1l_app_model_save_radio_profile(&profile);
    if (ret != ESP_OK) {
        char error[96];
        (void)snprintf(
            error, sizeof(error), "Radio preset was not saved: %s",
            esp_err_to_name(ret));
        set_status(controller, error, D1L_FIRST_START_COLOR_RED);
        return;
    }
    controller->radio_confirmed = true;
    render_storage_map(controller);
}

static void finish_onboarding(
    d1l_ui_first_start_controller_t *controller)
{
    if (!d1l_settings_node_name_valid(controller->node_name) ||
        strcmp(controller->node_name, D1L_NODE_NAME_FACTORY_DEFAULT) == 0 ||
        !controller->radio_confirmed) {
        set_status(
            controller,
            "Required name or radio confirmation is missing. Go Back to review.",
            D1L_FIRST_START_COLOR_RED);
        return;
    }
    const esp_err_t ret =
        d1l_app_model_complete_onboarding(controller->node_name);
    clear_sensitive_input(controller);
    if (ret != ESP_OK) {
        char error[112];
        (void)snprintf(
            error, sizeof(error), "Setup was not completed: %s",
            esp_err_to_name(ret));
        set_status(controller, error, D1L_FIRST_START_COLOR_RED);
        return;
    }
    controller->finishing = true;
    render_finishing(controller);
}

static void handle_back(d1l_ui_first_start_controller_t *controller)
{
    switch (controller->stage) {
    case D1L_UI_FIRST_START_LOCATION:
        copy_text(controller->latitude, sizeof(controller->latitude),
                  textarea_text_or_empty(controller->primary_textarea));
        copy_text(controller->longitude, sizeof(controller->longitude),
                  textarea_text_or_empty(controller->secondary_textarea));
        render_name(controller);
        break;
    case D1L_UI_FIRST_START_WIFI:
        copy_text(controller->wifi_ssid, sizeof(controller->wifi_ssid),
                  textarea_text_or_empty(controller->primary_textarea));
        clear_sensitive_input(controller);
        render_location(controller);
        break;
    case D1L_UI_FIRST_START_RADIO:
        render_wifi(controller);
        break;
    case D1L_UI_FIRST_START_STORAGE_MAP:
        render_radio(controller);
        break;
    case D1L_UI_FIRST_START_CHANNELS:
        render_storage_map(controller);
        break;
    default:
        break;
    }
}

static void handle_skip(d1l_ui_first_start_controller_t *controller)
{
    if (controller->stage == D1L_UI_FIRST_START_LOCATION) {
        if (controller->location_saved) {
            const esp_err_t ret = d1l_app_model_clear_map_location();
            if (ret != ESP_OK) {
                char error[96];
                (void)snprintf(
                    error, sizeof(error),
                    "Saved location could not be cleared: %s",
                    esp_err_to_name(ret));
                set_status(
                    controller, error, D1L_FIRST_START_COLOR_RED);
                return;
            }
        }
        controller->latitude[0] = '\0';
        controller->longitude[0] = '\0';
        controller->location_saved = false;
        render_wifi(controller);
    } else if (controller->stage == D1L_UI_FIRST_START_WIFI) {
        copy_text(controller->wifi_ssid, sizeof(controller->wifi_ssid),
                  textarea_text_or_empty(controller->primary_textarea));
        clear_sensitive_input(controller);
        const esp_err_t ret = d1l_app_model_set_wifi_enabled(false);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
            char error[96];
            (void)snprintf(
                error, sizeof(error), "Offline mode could not be saved: %s",
                esp_err_to_name(ret));
            set_status(controller, error, D1L_FIRST_START_COLOR_RED);
            return;
        }
        controller->offline_selected = true;
        render_radio(controller);
    }
}

static void handle_next(d1l_ui_first_start_controller_t *controller)
{
    switch (controller->stage) {
    case D1L_UI_FIRST_START_NAME:
        save_name_and_advance(controller);
        break;
    case D1L_UI_FIRST_START_LOCATION:
        save_location_and_advance(controller);
        break;
    case D1L_UI_FIRST_START_WIFI:
        save_wifi_and_advance(controller);
        break;
    case D1L_UI_FIRST_START_RADIO:
        confirm_radio_and_advance(controller);
        break;
    case D1L_UI_FIRST_START_STORAGE_MAP:
        if (!controller->media_ready) {
            set_status(
                controller,
                "Insert the prepared FAT32 card with its NRCan provider manifest.",
                D1L_FIRST_START_COLOR_RED);
        } else {
            render_channels(controller);
        }
        break;
    case D1L_UI_FIRST_START_CHANNELS:
        finish_onboarding(controller);
        break;
    default:
        break;
    }
}

static void handle_action(d1l_ui_first_start_controller_t *controller,
                          d1l_ui_first_start_action_t action)
{
    if (!controller || !object_valid(controller->overlay)) {
        return;
    }
    switch (action) {
    case D1L_UI_FIRST_START_ACTION_BACK:
        handle_back(controller);
        break;
    case D1L_UI_FIRST_START_ACTION_SKIP:
        handle_skip(controller);
        break;
    case D1L_UI_FIRST_START_ACTION_NEXT:
        handle_next(controller);
        break;
    default:
        break;
    }
}

bool d1l_ui_first_start_create(
    d1l_ui_first_start_controller_t *controller, lv_obj_t *parent)
{
    if (!controller || !parent) {
        return false;
    }
    if (object_valid(controller->overlay)) {
        d1l_ui_first_start_deactivate(controller);
        lv_obj_del(controller->overlay);
    }
    memset(controller, 0, sizeof(*controller));
    controller->overlay = lv_obj_create(parent);
    if (!controller->overlay) {
        return false;
    }
    lv_obj_set_pos(controller->overlay, 0, 0);
    lv_obj_set_size(controller->overlay, 480, 480);
    lv_obj_set_style_radius(controller->overlay, 0, 0);
    lv_obj_set_style_border_width(controller->overlay, 0, 0);
    lv_obj_set_style_pad_all(controller->overlay, 0, 0);
    lv_obj_set_style_bg_color(
        controller->overlay,
        lv_color_hex(D1L_FIRST_START_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(controller->overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(controller->overlay, LV_OBJ_FLAG_SCROLLABLE);
    render_readiness(controller);
    return true;
}

void d1l_ui_first_start_update(
    d1l_ui_first_start_controller_t *controller,
    const d1l_app_snapshot_t *snapshot)
{
    if (!controller || !snapshot || !object_valid(controller->overlay) ||
        controller->stage == D1L_UI_FIRST_START_DONE) {
        return;
    }
    lv_obj_move_foreground(controller->overlay);
    if (controller->stage == D1L_UI_FIRST_START_STORAGE_MAP) {
        update_storage_map_page(controller, snapshot);
    }
    if (controller->stage == D1L_UI_FIRST_START_FINISHING) {
        if (snapshot->onboarding_complete) {
            d1l_ui_first_start_deactivate(controller);
        }
        return;
    }
    if (controller->stage != D1L_UI_FIRST_START_READINESS) {
        if (snapshot->onboarding_complete) {
            d1l_ui_first_start_deactivate(controller);
        }
        return;
    }
    update_readiness_rows(controller, snapshot);
    if (!d1l_ui_first_start_essential_ready(snapshot)) {
        controller->ready_hold_started = false;
        controller->ready_since_tick = 0U;
        return;
    }
    const uint32_t now = lv_tick_get();
    if (!controller->ready_hold_started) {
        controller->ready_hold_started = true;
        controller->ready_since_tick = now;
        set_status(
            controller, "Essential systems are ready.",
            D1L_FIRST_START_COLOR_GREEN);
        return;
    }
    if ((uint32_t)(now - controller->ready_since_tick) <
        D1L_FIRST_START_READY_HOLD_MS) {
        return;
    }
    if (snapshot->onboarding_complete) {
        d1l_ui_first_start_deactivate(controller);
    } else {
        render_name(controller);
    }
}

void d1l_ui_first_start_deactivate(
    d1l_ui_first_start_controller_t *controller)
{
    if (!controller) {
        return;
    }
    clear_sensitive_input(controller);
    advance_generation(controller);
    memset(controller->bindings, 0, sizeof(controller->bindings));
    if (object_valid(controller->overlay)) {
        lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
    }
    controller->stage = D1L_UI_FIRST_START_DONE;
}

bool d1l_ui_first_start_visible(
    const d1l_ui_first_start_controller_t *controller)
{
    return controller && object_valid(controller->overlay) &&
        !lv_obj_has_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *d1l_ui_first_start_overlay(
    const d1l_ui_first_start_controller_t *controller)
{
    return controller ? controller->overlay : NULL;
}
