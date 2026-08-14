#include "ui_device_sheets.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"
#include "lvgl.h"
#include "ui_modal.h"

enum {
    BINDING_CLOSE_DISPLAY = 0,
    BINDING_BRIGHTNESS,
    BINDING_NIGHT_MODE,
    BINDING_HIGH_CONTRAST,
    BINDING_TIMEOUT,
    BINDING_CLOSE_DIAGNOSTICS,
    BINDING_OPEN_TERMINAL,
};

_Static_assert(sizeof(d1l_ui_device_sheets_controller_t) <=
                   D1L_UI_DEVICE_SHEETS_CONTROLLER_MAX_BYTES,
               "Device sheets controller exceeded its persistent-owner size budget");

static void advance_generation(
    d1l_ui_device_sheets_controller_t *controller)
{
    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
    }
}

static void deactivate_actions(
    d1l_ui_device_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    advance_generation(controller);
    controller->action_handler = NULL;
    controller->action_context = NULL;
    memset(controller->bindings, 0, sizeof(controller->bindings));
}

static bool binding_is_current(
    const d1l_ui_device_sheets_binding_t *binding)
{
    return binding && binding->controller && binding->generation != 0U &&
        binding->generation == binding->controller->generation;
}

static void action_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_device_sheets_binding_t *binding =
        (d1l_ui_device_sheets_binding_t *)lv_event_get_user_data(event);
    if (!binding_is_current(binding) ||
        !binding->controller->action_handler ||
        binding->action <= D1L_UI_DEVICE_SHEETS_ACTION_NONE ||
        binding->action > D1L_UI_DEVICE_SHEETS_ACTION_OPEN_TERMINAL) {
        return;
    }
    binding->controller->action_handler(
        binding->action, binding->controller->action_context);
}

static d1l_ui_device_sheets_binding_t *set_binding(
    d1l_ui_device_sheets_controller_t *controller,
    size_t slot,
    d1l_ui_device_sheets_action_t action)
{
    if (!controller || slot >= D1L_UI_DEVICE_SHEETS_BINDING_COUNT) {
        return NULL;
    }
    d1l_ui_device_sheets_binding_t *binding = &controller->bindings[slot];
    binding->controller = controller;
    binding->action = action;
    binding->generation = controller->generation;
    return binding;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text,
                              uint32_t color)
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

static void configure_wrapped_label(lv_obj_t *label, int x, int y)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 408);
    lv_obj_set_pos(label, x, y);
}

static void configure_dot_label(lv_obj_t *label, int x, int y, int width)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
    lv_obj_set_pos(label, x, y);
}

static lv_obj_t *create_button(
    d1l_ui_device_sheets_controller_t *controller,
    lv_obj_t *parent,
    const char *text,
    int x,
    int y,
    int width,
    int height,
    size_t binding_slot,
    d1l_ui_device_sheets_action_t action)
{
    if (!controller || !parent || !text) {
        return NULL;
    }
    lv_obj_t *button = lv_btn_create(parent);
    if (!button) {
        return NULL;
    }
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x252D33), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2E3A43), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = create_label(button, text, 0xF4F7FB);
    if (!label) {
        lv_obj_del(button);
        return NULL;
    }
    lv_obj_center(label);
    if (action != D1L_UI_DEVICE_SHEETS_ACTION_NONE) {
        d1l_ui_device_sheets_binding_t *binding =
            set_binding(controller, binding_slot, action);
        if (!binding) {
            lv_obj_del(button);
            return NULL;
        }
        lv_obj_add_event_cb(button, action_event_cb, LV_EVENT_CLICKED,
                            binding);
    }
    return button;
}

static lv_obj_t *create_sheet(lv_obj_t *parent, bool scrollable)
{
    lv_obj_t *sheet = lv_obj_create(parent);
    if (!sheet) {
        return NULL;
    }
    lv_obj_set_size(sheet, 448, 320);
    lv_obj_set_pos(sheet, 16, 82);
    lv_obj_set_style_radius(sheet, 8, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(0x20262B), 0);
    lv_obj_set_style_border_color(sheet, lv_color_hex(0x3C4A54), 0);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_pad_all(sheet, 12, 0);
    if (scrollable) {
        lv_obj_set_scrollbar_mode(sheet, LV_SCROLLBAR_MODE_AUTO);
    } else {
        lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    }
    d1l_ui_modal_hide(sheet);
    return sheet;
}

static void delete_sheet(lv_obj_t **sheet)
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

static void destroy_sheets(d1l_ui_device_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    delete_sheet(&controller->display_sheet);
    delete_sheet(&controller->diagnostics_sheet);
    memset(controller, 0, sizeof(*controller));
}

bool d1l_ui_device_sheets_create(
    d1l_ui_device_sheets_controller_t *controller,
    lv_obj_t *parent)
{
    if (!controller) {
        return false;
    }
    destroy_sheets(controller);
    if (!parent || !lv_obj_is_valid(parent)) {
        return false;
    }
    controller->display_sheet = create_sheet(parent, false);
    if (!controller->display_sheet) {
        destroy_sheets(controller);
        return false;
    }
    controller->diagnostics_sheet = create_sheet(parent, true);
    if (!controller->diagnostics_sheet) {
        destroy_sheets(controller);
        return false;
    }
    return true;
}

static void invalidate_sheet(
    d1l_ui_device_sheets_controller_t *controller,
    lv_obj_t *sheet)
{
    deactivate_actions(controller);
    if (sheet && lv_obj_is_valid(sheet)) {
        d1l_ui_modal_hide(sheet);
        lv_obj_clean(sheet);
    }
}

static bool begin_render(
    d1l_ui_device_sheets_controller_t *controller,
    lv_obj_t *sheet,
    d1l_ui_device_sheets_action_handler_t action_handler,
    void *action_context)
{
    if (!controller || !sheet || !lv_obj_is_valid(sheet) ||
        !action_handler) {
        invalidate_sheet(controller, sheet);
        return false;
    }
    deactivate_actions(controller);
    controller->action_handler = action_handler;
    controller->action_context = action_context;
    lv_obj_clean(sheet);
    return true;
}

bool d1l_ui_device_sheets_render_display(
    d1l_ui_device_sheets_controller_t *controller,
    const d1l_app_snapshot_t *snapshot,
    d1l_ui_device_sheets_action_handler_t action_handler,
    void *action_context)
{
    lv_obj_t *sheet = controller ? controller->display_sheet : NULL;
    if (!snapshot) {
        invalidate_sheet(controller, sheet);
        return false;
    }
    if (!begin_render(controller, sheet, action_handler, action_context)) {
        return false;
    }
    bool complete = true;
    lv_obj_t *title = create_label(sheet, "Display", 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_pos(title, 8, 4);
    } else {
        complete = false;
    }
    complete = create_button(
        controller, sheet, "Close", 340, 0, 76, 40,
        BINDING_CLOSE_DISPLAY,
        D1L_UI_DEVICE_SHEETS_ACTION_CLOSE_DISPLAY) != NULL && complete;

    lv_obj_t *state = create_label(sheet, "Local display time", 0x20D9ED);
    if (state) {
        lv_obj_set_pos(state, 8, 54);
    }
    complete = state != NULL && complete;
    char time_summary[96];
    snprintf(time_summary, sizeof(time_summary), "%s  %s",
             snapshot->timezone_settings_ready &&
                     snapshot->timezone_label[0] != '\0' ?
                 snapshot->timezone_label : "UTC fallback",
             snapshot->time_available && snapshot->time_label[0] != '\0' ?
                 snapshot->time_label : "time not synced");
    lv_obj_t *summary = create_label(sheet, time_summary, 0xF4F7FB);
    configure_wrapped_label(summary, 8, 88);
    complete = summary != NULL && complete;

    char brightness_text[24];
    snprintf(brightness_text, sizeof(brightness_text), "Brightness %u%%",
             (unsigned)snapshot->display_brightness_percent);
    char night_text[18];
    snprintf(night_text, sizeof(night_text), "Night %s",
             snapshot->night_mode ? "On" : "Off");
    char contrast_text[22];
    snprintf(contrast_text, sizeof(contrast_text), "Contrast %s",
             snapshot->high_contrast ? "On" : "Off");
    char timeout_text[24];
    if (snapshot->display_timeout_seconds == 0U) {
        snprintf(timeout_text, sizeof(timeout_text), "Timeout Off");
    } else if (snapshot->display_timeout_seconds % 60U == 0U) {
        snprintf(timeout_text, sizeof(timeout_text), "Timeout %u min",
                 (unsigned)(snapshot->display_timeout_seconds / 60U));
    } else {
        snprintf(timeout_text, sizeof(timeout_text), "Timeout %us",
                 (unsigned)snapshot->display_timeout_seconds);
    }
    complete = create_button(
        controller, sheet, brightness_text, 8, 144, 154, 44,
        BINDING_BRIGHTNESS,
        D1L_UI_DEVICE_SHEETS_ACTION_BRIGHTNESS) != NULL && complete;
    complete = create_button(
        controller, sheet, night_text, 172, 144, 116, 44,
        BINDING_NIGHT_MODE,
        D1L_UI_DEVICE_SHEETS_ACTION_NIGHT_MODE) != NULL && complete;
    complete = create_button(
        controller, sheet, contrast_text, 298, 144, 118, 44,
        BINDING_HIGH_CONTRAST,
        D1L_UI_DEVICE_SHEETS_ACTION_HIGH_CONTRAST) != NULL && complete;
    complete = create_button(
        controller, sheet, timeout_text, 8, 196, 154, 44,
        BINDING_TIMEOUT,
        D1L_UI_DEVICE_SHEETS_ACTION_TIMEOUT) != NULL && complete;

    lv_obj_t *note = create_label(
        sheet,
        "Fixed UTC offset only; daylight saving is not automatic. Touch wakes and "
        "unlocks; the top button wakes too. Night mode keeps daytime brightness.",
        0xFBBF24);
    configure_wrapped_label(note, 8, 260);
    complete = note != NULL && complete;
    if (!complete) {
        invalidate_sheet(controller, sheet);
        return false;
    }
    return true;
}

static lv_obj_t *create_diagnostic_line(lv_obj_t *sheet, const char *text,
                                        int x, int y, uint32_t color,
                                        int width)
{
    lv_obj_t *label = create_label(sheet, text, color);
    configure_dot_label(label, x, y, width);
    return label;
}

bool d1l_ui_device_sheets_render_diagnostics(
    d1l_ui_device_sheets_controller_t *controller,
    const d1l_app_snapshot_t *snapshot,
    d1l_ui_device_sheets_action_handler_t action_handler,
    void *action_context)
{
    lv_obj_t *sheet = controller ? controller->diagnostics_sheet : NULL;
    if (!snapshot) {
        invalidate_sheet(controller, sheet);
        return false;
    }
    if (!begin_render(controller, sheet, action_handler, action_context)) {
        return false;
    }
    bool complete = true;
    lv_obj_t *title = create_label(sheet, "Diagnostics", 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_pos(title, 8, 4);
    } else {
        complete = false;
    }
    complete = create_button(
        controller, sheet, "Close", 340, 0, 76, 40,
        BINDING_CLOSE_DIAGNOSTICS,
        D1L_UI_DEVICE_SHEETS_ACTION_CLOSE_DIAGNOSTICS) != NULL && complete;
    lv_obj_t *health = create_label(sheet, "Health", 0x20D9ED);
    if (health) {
        lv_obj_set_pos(health, 8, 50);
    }
    complete = health != NULL && complete;

    char text[128];
    snprintf(text, sizeof(text), "reset %s  uptime %lus  mesh %s",
             snapshot->reset_reason ? snapshot->reset_reason : "UNKNOWN",
             (unsigned long)(snapshot->uptime_ms / 1000U),
             snapshot->mesh_state ? snapshot->mesh_state : "unknown");
    complete = create_diagnostic_line(sheet, text, 8, 76, 0xF4F7FB, 408) != NULL &&
               complete;
    snprintf(text, sizeof(text), "heap %luK free  min %luK  largest %luK",
             (unsigned long)(snapshot->heap_free / 1024U),
             (unsigned long)(snapshot->heap_min_free / 1024U),
             (unsigned long)(snapshot->heap_largest_free / 1024U));
    complete = create_diagnostic_line(sheet, text, 8, 104, 0xA6B0B7, 408) != NULL &&
               complete;
    snprintf(text, sizeof(text), "LVGL free %lu  largest %lu  used %u%%",
             (unsigned long)snapshot->lvgl_free_bytes,
             (unsigned long)snapshot->lvgl_largest_free_bytes,
             (unsigned)snapshot->lvgl_used_pct);
    complete = create_diagnostic_line(sheet, text, 8, 132, 0xA6B0B7, 408) != NULL &&
               complete;
    snprintf(text, sizeof(text), "ui stack %lu words  console stack %lu",
             (unsigned long)snapshot->ui_task_stack_free_words,
             (unsigned long)snapshot->current_task_stack_free_words);
    complete = create_diagnostic_line(sheet, text, 8, 160, 0xA6B0B7, 408) != NULL &&
               complete;
    snprintf(text, sizeof(text), "packets rx %lu tx %lu  rejected %lu",
             (unsigned long)snapshot->rx_packets,
             (unsigned long)snapshot->tx_packets,
             (unsigned long)snapshot->rejected_commands);
    complete = create_diagnostic_line(sheet, text, 8, 188, 0x4D7FFF, 408) != NULL &&
               complete;

    lv_obj_t *stores = create_label(sheet, "Crashlog  Events  Serial", 0xF4F7FB);
    if (stores) {
        lv_obj_set_pos(stores, 8, 210);
    }
    complete = stores != NULL && complete;
    lv_obj_t *note = create_label(
        sheet,
        "Terminal shows recent events and the current log level.",
        0xFBBF24);
    configure_wrapped_label(note, 8, 230);
    complete = note != NULL && complete;

    if (d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MUTABLE_TERMINAL)) {
        complete = create_button(
            controller, sheet, "Open Terminal", 8, 250, 150, 44,
            BINDING_OPEN_TERMINAL,
            D1L_UI_DEVICE_SHEETS_ACTION_OPEN_TERMINAL) != NULL && complete;
    }
    if (!complete) {
        invalidate_sheet(controller, sheet);
        return false;
    }
    return true;
}

void d1l_ui_device_sheets_hide_display(
    d1l_ui_device_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    deactivate_actions(controller);
    if (controller->display_sheet &&
        lv_obj_is_valid(controller->display_sheet)) {
        d1l_ui_modal_hide(controller->display_sheet);
    }
}

void d1l_ui_device_sheets_hide_diagnostics(
    d1l_ui_device_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    deactivate_actions(controller);
    if (controller->diagnostics_sheet &&
        lv_obj_is_valid(controller->diagnostics_sheet)) {
        d1l_ui_modal_hide(controller->diagnostics_sheet);
    }
}

lv_obj_t *d1l_ui_device_sheets_display(
    const d1l_ui_device_sheets_controller_t *controller)
{
    return controller && controller->display_sheet &&
                   lv_obj_is_valid(controller->display_sheet) ?
        controller->display_sheet : NULL;
}

lv_obj_t *d1l_ui_device_sheets_diagnostics(
    const d1l_ui_device_sheets_controller_t *controller)
{
    return controller && controller->diagnostics_sheet &&
                   lv_obj_is_valid(controller->diagnostics_sheet) ?
        controller->diagnostics_sheet : NULL;
}
