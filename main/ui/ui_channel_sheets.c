#include "ui_channel_sheets.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(d1l_ui_channel_sheets_controller_t) <=
                   D1L_UI_CHANNEL_SHEETS_CONTROLLER_MAX_BYTES,
               "Channel sheets controller exceeded its size budget");

static void secure_zero(void *data, size_t data_size)
{
    volatile unsigned char *cursor = (volatile unsigned char *)data;
    while (cursor && data_size > 0U) {
        *cursor++ = 0U;
        data_size--;
    }
}

static void advance_generation(d1l_ui_channel_sheets_controller_t *controller)
{
    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
    }
}

static void deactivate_actions(d1l_ui_channel_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    advance_generation(controller);
    controller->action_handler = NULL;
    controller->action_context = NULL;
    memset(controller->bindings, 0, sizeof(controller->bindings));
}

void d1l_ui_channel_sheets_clear_export_uri(
    d1l_ui_channel_sheets_controller_t *controller)
{
    if (controller) {
        secure_zero(controller->export_uri, sizeof(controller->export_uri));
    }
}

const d1l_channel_info_t *d1l_ui_channel_sheets_channel(
    const d1l_ui_channel_sheets_controller_t *controller)
{
    if (!controller || controller->selected.channel_id == 0U ||
        !memchr(controller->selected.name, '\0',
                sizeof(controller->selected.name))) {
        return NULL;
    }
    return &controller->selected;
}

bool d1l_ui_channel_sheets_set_channel(
    d1l_ui_channel_sheets_controller_t *controller,
    const d1l_channel_info_t *channel)
{
    if (!controller) {
        return false;
    }
    deactivate_actions(controller);
    d1l_ui_channel_sheets_clear_export_uri(controller);
    memset(&controller->selected, 0, sizeof(controller->selected));
    if (!channel || channel->channel_id == 0U || channel->name[0] == '\0' ||
        !memchr(channel->name, '\0', sizeof(channel->name))) {
        return false;
    }
    controller->selected = *channel;
    return true;
}

bool d1l_ui_channel_sheets_set_export_uri(
    d1l_ui_channel_sheets_controller_t *controller,
    const char *uri)
{
    if (!controller) {
        return false;
    }
    d1l_ui_channel_sheets_clear_export_uri(controller);
    if (!d1l_ui_channel_sheets_channel(controller) || !uri) {
        return false;
    }
    const char *end = memchr(uri, '\0', sizeof(controller->export_uri));
    if (!end || end == uri) {
        d1l_ui_channel_sheets_clear_export_uri(controller);
        return false;
    }
    const size_t length = (size_t)(end - uri);
    memcpy(controller->export_uri, uri, length + 1U);
    return true;
}

#ifndef D1L_UI_CHANNEL_SHEETS_SECRET_TEST

#include "app/release_profile.h"
#include "lvgl.h"
#include "ui_keyboard.h"
#include "ui_modal.h"

enum {
    BINDING_CREATE_BACK = 0,
    BINDING_CREATE_SUBMIT,
    BINDING_CREATE_KEYBOARD,
    BINDING_IMPORT_BACK,
    BINDING_IMPORT_SUBMIT,
    BINDING_IMPORT_KEYBOARD,
    BINDING_OPTIONS_BACK,
    BINDING_OPTIONS_EDIT,
    BINDING_OPTIONS_TOGGLE,
    BINDING_OPTIONS_DEFAULT,
    BINDING_OPTIONS_EXPORT,
    BINDING_OPTIONS_REMOVE,
    BINDING_EDIT_BACK,
    BINDING_EDIT_SUBMIT,
    BINDING_EDIT_KEYBOARD,
    BINDING_EXPORT_BACK,
    BINDING_REMOVE_BACK,
    BINDING_REMOVE_CANCEL,
    BINDING_REMOVE_CONFIRM,
};

static bool object_is_valid(const lv_obj_t *object)
{
    return object && lv_obj_is_valid(object);
}

static void scrub_textarea(lv_obj_t *textarea, bool sensitive)
{
    if (!object_is_valid(textarea)) {
        return;
    }
    if (sensitive) {
        const char *text = lv_textarea_get_text(textarea);
        if (text) {
            volatile char *cursor = (volatile char *)(uintptr_t)text;
            size_t remaining = D1L_CHANNEL_SHARE_URI_LEN;
            while (remaining > 0U && *cursor != '\0') {
                *cursor++ = '\0';
                remaining--;
            }
        }
    }
    lv_textarea_set_text(textarea, "");
}

static void detach_input(lv_obj_t *keyboard, lv_obj_t *textarea,
                         bool clear, bool sensitive)
{
    if (object_is_valid(keyboard)) {
        lv_keyboard_set_textarea(keyboard, NULL);
    }
    if (clear) {
        scrub_textarea(textarea, sensitive);
    }
}

static void delete_sheet(lv_obj_t **sheet)
{
    if (!sheet) {
        return;
    }
    if (object_is_valid(*sheet)) {
        d1l_ui_modal_hide(*sheet);
        lv_obj_del(*sheet);
    }
    *sheet = NULL;
}

static void destroy_sheets(d1l_ui_channel_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    detach_input(controller->create_keyboard, controller->create_textarea,
                 true, false);
    detach_input(controller->import_keyboard, controller->import_textarea,
                 true, true);
    detach_input(controller->edit_keyboard, controller->edit_textarea,
                 true, false);
    d1l_ui_channel_sheets_clear_export_uri(controller);
    delete_sheet(&controller->create_sheet);
    delete_sheet(&controller->import_sheet);
    delete_sheet(&controller->options_sheet);
    delete_sheet(&controller->edit_sheet);
    delete_sheet(&controller->export_sheet);
    delete_sheet(&controller->remove_sheet);
    secure_zero(controller, sizeof(*controller));
}

static lv_obj_t *create_sheet(lv_obj_t *parent)
{
    if (!object_is_valid(parent)) {
        return NULL;
    }
    lv_obj_t *sheet = lv_obj_create(parent);
    if (!sheet) {
        return NULL;
    }
    lv_obj_set_size(sheet, 480, 424);
    lv_obj_set_pos(sheet, 0, 56);
    lv_obj_set_style_radius(sheet, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(0x20262B), 0);
    lv_obj_set_style_border_width(sheet, 0, 0);
    lv_obj_set_style_pad_all(sheet, 0, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    d1l_ui_modal_hide(sheet);
    return sheet;
}

bool d1l_ui_channel_sheets_create(
    d1l_ui_channel_sheets_controller_t *controller,
    lv_obj_t *parent)
{
    if (!controller) {
        return false;
    }
    destroy_sheets(controller);
    if (!object_is_valid(parent)) {
        return false;
    }
    controller->parent = parent;
    lv_obj_t **sheets[] = {
        &controller->create_sheet,
        &controller->import_sheet,
        &controller->options_sheet,
        &controller->edit_sheet,
        &controller->export_sheet,
        &controller->remove_sheet,
    };
    for (size_t i = 0U; i < sizeof(sheets) / sizeof(sheets[0]); ++i) {
        *sheets[i] = create_sheet(parent);
        if (!*sheets[i]) {
            destroy_sheets(controller);
            return false;
        }
    }
    return true;
}

static d1l_ui_channel_binding_t *set_binding(
    d1l_ui_channel_sheets_controller_t *controller,
    size_t slot,
    d1l_ui_channel_action_t action)
{
    if (!controller || slot >= D1L_UI_CHANNEL_SHEETS_BINDING_COUNT ||
        action <= D1L_UI_CHANNEL_ACTION_NONE ||
        action > D1L_UI_CHANNEL_ACTION_CONFIRM_REMOVE) {
        return NULL;
    }
    d1l_ui_channel_binding_t *binding = &controller->bindings[slot];
    binding->controller = controller;
    binding->action = action;
    binding->generation = controller->generation;
    return binding;
}

static bool binding_is_current(const d1l_ui_channel_binding_t *binding)
{
    return binding && binding->controller && binding->generation != 0U &&
        binding->generation == binding->controller->generation;
}

const char *d1l_ui_channel_sheets_create_text(
    const d1l_ui_channel_sheets_controller_t *controller)
{
    return controller && object_is_valid(controller->create_textarea)
        ? lv_textarea_get_text(controller->create_textarea) : NULL;
}

const char *d1l_ui_channel_sheets_import_text(
    const d1l_ui_channel_sheets_controller_t *controller)
{
    return controller && object_is_valid(controller->import_textarea)
        ? lv_textarea_get_text(controller->import_textarea) : NULL;
}

const char *d1l_ui_channel_sheets_edit_text(
    const d1l_ui_channel_sheets_controller_t *controller)
{
    return controller && object_is_valid(controller->edit_textarea)
        ? lv_textarea_get_text(controller->edit_textarea) : NULL;
}

static const char *action_text(
    const d1l_ui_channel_sheets_controller_t *controller,
    d1l_ui_channel_action_t action)
{
    switch (action) {
    case D1L_UI_CHANNEL_ACTION_SUBMIT_CREATE:
        return d1l_ui_channel_sheets_create_text(controller);
    case D1L_UI_CHANNEL_ACTION_SUBMIT_IMPORT:
        return d1l_ui_channel_sheets_import_text(controller);
    case D1L_UI_CHANNEL_ACTION_SUBMIT_EDIT:
        return d1l_ui_channel_sheets_edit_text(controller);
    default:
        return NULL;
    }
}

static void dispatch_action(d1l_ui_channel_binding_t *binding,
                            d1l_ui_channel_action_t action)
{
    if (!binding_is_current(binding) ||
        !binding->controller->action_handler ||
        action <= D1L_UI_CHANNEL_ACTION_NONE ||
        action > D1L_UI_CHANNEL_ACTION_CONFIRM_REMOVE) {
        return;
    }
    d1l_ui_channel_sheets_controller_t *controller = binding->controller;
    const char *text = action_text(controller, action);
    d1l_ui_channel_action_event_t event = {
        .action = action,
        .channel = d1l_ui_channel_sheets_channel(controller),
        .text = text,
    };
    controller->action_handler(&event, controller->action_context);
    if (action == D1L_UI_CHANNEL_ACTION_SUBMIT_IMPORT ||
        action == D1L_UI_CHANNEL_ACTION_CANCEL_IMPORT) {
        scrub_textarea(controller->import_textarea, true);
    }
}

static void action_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_channel_binding_t *binding =
        (d1l_ui_channel_binding_t *)lv_event_get_user_data(event);
    const d1l_ui_channel_action_t action = binding
        ? binding->action : D1L_UI_CHANNEL_ACTION_NONE;
    dispatch_action(binding, action);
}

static void keyboard_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_channel_binding_t *binding =
        (d1l_ui_channel_binding_t *)lv_event_get_user_data(event);
    if (!binding_is_current(binding)) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        dispatch_action(binding, binding->action);
    } else if (code == LV_EVENT_CANCEL) {
        d1l_ui_channel_action_t cancel = D1L_UI_CHANNEL_ACTION_NONE;
        if (binding->action == D1L_UI_CHANNEL_ACTION_SUBMIT_CREATE) {
            cancel = D1L_UI_CHANNEL_ACTION_CANCEL_CREATE;
        } else if (binding->action == D1L_UI_CHANNEL_ACTION_SUBMIT_IMPORT) {
            cancel = D1L_UI_CHANNEL_ACTION_CANCEL_IMPORT;
        } else if (binding->action == D1L_UI_CHANNEL_ACTION_SUBMIT_EDIT) {
            cancel = D1L_UI_CHANNEL_ACTION_CANCEL_EDIT;
        }
        dispatch_action(binding, cancel);
    }
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text,
                              uint32_t color)
{
    if (!object_is_valid(parent) || !text) {
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

static lv_obj_t *create_button(
    d1l_ui_channel_sheets_controller_t *controller,
    lv_obj_t *parent,
    const char *text,
    int x,
    int y,
    int width,
    int height,
    size_t binding_slot,
    d1l_ui_channel_action_t action)
{
    if (!controller || !object_is_valid(parent) || !text ||
        width < 44 || height < 44) {
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
    if (action == D1L_UI_CHANNEL_ACTION_NONE) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
        return button;
    }
    d1l_ui_channel_binding_t *binding =
        set_binding(controller, binding_slot, action);
    if (!binding) {
        lv_obj_del(button);
        return NULL;
    }
    lv_obj_add_event_cb(button, action_event_cb, LV_EVENT_CLICKED, binding);
    return button;
}

static void style_danger_button(lv_obj_t *button)
{
    if (!object_is_valid(button)) {
        return;
    }
    lv_obj_set_style_bg_color(button, lv_color_hex(0x3A1720), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x55202B), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(0xF87171), 0);
    lv_obj_set_style_border_width(button, 1, 0);
}

static bool configure_title(lv_obj_t *title)
{
    if (!title) {
        return false;
    }
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 364);
    lv_obj_set_pos(title, 100, 10);
    return true;
}

static bool begin_render(d1l_ui_channel_sheets_controller_t *controller,
                         lv_obj_t *sheet,
                         bool require_channel,
                         d1l_ui_channel_action_handler_t action_handler,
                         void *action_context)
{
    if (!controller || !object_is_valid(sheet) || !action_handler ||
        (require_channel && !d1l_ui_channel_sheets_channel(controller))) {
        return false;
    }
    deactivate_actions(controller);
    controller->action_handler = action_handler;
    controller->action_context = action_context;
    lv_obj_clean(sheet);
    return true;
}

static void clear_input_refs(d1l_ui_channel_sheets_controller_t *controller,
                             lv_obj_t *sheet)
{
    if (sheet == controller->create_sheet) {
        controller->create_textarea = NULL;
        controller->create_keyboard = NULL;
    } else if (sheet == controller->import_sheet) {
        controller->import_textarea = NULL;
        controller->import_keyboard = NULL;
    } else if (sheet == controller->edit_sheet) {
        controller->edit_textarea = NULL;
        controller->edit_keyboard = NULL;
    }
}

static void detach_sheet_input(d1l_ui_channel_sheets_controller_t *controller,
                               lv_obj_t *sheet, bool clear)
{
    if (sheet == controller->create_sheet) {
        detach_input(controller->create_keyboard, controller->create_textarea,
                     clear, false);
    } else if (sheet == controller->import_sheet) {
        detach_input(controller->import_keyboard, controller->import_textarea,
                     clear, true);
    } else if (sheet == controller->edit_sheet) {
        detach_input(controller->edit_keyboard, controller->edit_textarea,
                     clear, false);
    }
}

static void invalidate_render(d1l_ui_channel_sheets_controller_t *controller,
                              lv_obj_t *sheet)
{
    if (!controller) {
        return;
    }
    deactivate_actions(controller);
    detach_sheet_input(controller, sheet, true);
    if (sheet == controller->export_sheet) {
        d1l_ui_channel_sheets_clear_export_uri(controller);
    }
    if (object_is_valid(sheet)) {
        d1l_ui_modal_hide(sheet);
        lv_obj_clean(sheet);
    }
    clear_input_refs(controller, sheet);
}

static bool finish_render(d1l_ui_channel_sheets_controller_t *controller,
                          lv_obj_t *sheet, bool complete)
{
    if (!complete) {
        invalidate_render(controller, sheet);
        return false;
    }
    return true;
}

static bool render_input_sheet(
    d1l_ui_channel_sheets_controller_t *controller,
    lv_obj_t *sheet,
    const char *title,
    const char *hint,
    const char *placeholder,
    const char *initial,
    size_t max_length,
    bool password,
    size_t back_slot,
    d1l_ui_channel_action_t cancel_action,
    size_t submit_slot,
    d1l_ui_channel_action_t submit_action,
    size_t keyboard_slot,
    lv_obj_t **out_textarea,
    lv_obj_t **out_keyboard,
    d1l_ui_channel_action_handler_t action_handler,
    void *action_context)
{
    detach_sheet_input(controller, sheet, true);
    clear_input_refs(controller, sheet);
    if (!begin_render(controller, sheet, false, action_handler, action_context)) {
        invalidate_render(controller, sheet);
        return false;
    }
    bool complete = true;
    complete = create_button(controller, sheet, "Back", 12, 6, 72, 44,
                             back_slot, cancel_action) != NULL && complete;
    complete = configure_title(create_label(sheet, title, 0xF4F7FB)) && complete;
    lv_obj_t *help = create_label(sheet, hint, 0xA6B0B7);
    if (help) {
        lv_label_set_long_mode(help, LV_LABEL_LONG_DOT);
        lv_obj_set_width(help, 448);
        lv_obj_set_pos(help, 16, 58);
    } else {
        complete = false;
    }
    *out_textarea = lv_textarea_create(sheet);
    if (!*out_textarea) {
        complete = false;
    } else {
        lv_obj_set_size(*out_textarea, 448, 48);
        lv_obj_set_pos(*out_textarea, 16, 82);
        lv_textarea_set_one_line(*out_textarea, true);
        lv_textarea_set_max_length(*out_textarea, max_length);
        lv_textarea_set_password_mode(*out_textarea, password);
        lv_textarea_set_placeholder_text(*out_textarea, placeholder);
        lv_textarea_set_text(*out_textarea, initial ? initial : "");
        lv_obj_set_style_radius(*out_textarea, 8, 0);
        lv_obj_set_style_bg_color(*out_textarea, lv_color_hex(0x17191A), 0);
        lv_obj_set_style_border_color(*out_textarea, lv_color_hex(0x33404A), 0);
        lv_obj_set_style_text_color(*out_textarea, lv_color_hex(0xF4F7FB), 0);
        lv_obj_set_style_text_color(*out_textarea, lv_color_hex(0xA6B0B7),
                                    LV_PART_TEXTAREA_PLACEHOLDER);
    }
    *out_keyboard = lv_keyboard_create(sheet);
    if (!*out_keyboard || !*out_textarea) {
        complete = false;
    } else {
        d1l_ui_keyboard_configure_input(*out_keyboard, *out_textarea,
                                        16, 138, 448, 204);
        d1l_ui_channel_binding_t *binding =
            set_binding(controller, keyboard_slot, submit_action);
        if (!binding) {
            complete = false;
        } else {
            lv_obj_add_event_cb(*out_keyboard, keyboard_event_cb,
                                LV_EVENT_READY, binding);
            lv_obj_add_event_cb(*out_keyboard, keyboard_event_cb,
                                LV_EVENT_CANCEL, binding);
            lv_keyboard_set_textarea(*out_keyboard, *out_textarea);
        }
    }
    complete = create_button(controller, sheet, "Save", 16, 356, 448, 52,
                             submit_slot, submit_action) != NULL && complete;
    return finish_render(controller, sheet, complete);
}

bool d1l_ui_channel_sheets_render_create(
    d1l_ui_channel_sheets_controller_t *controller,
    d1l_ui_channel_action_handler_t action_handler,
    void *action_context)
{
    if (!controller) {
        return false;
    }
    return render_input_sheet(
        controller, controller->create_sheet, "Add Channel",
        "Start with # to join a shared hashtag; other names stay private.",
        "#yyc or private name", "", D1L_CHANNEL_NAME_LEN - 1U, false,
        BINDING_CREATE_BACK, D1L_UI_CHANNEL_ACTION_CANCEL_CREATE,
        BINDING_CREATE_SUBMIT, D1L_UI_CHANNEL_ACTION_SUBMIT_CREATE,
        BINDING_CREATE_KEYBOARD, &controller->create_textarea,
        &controller->create_keyboard, action_handler, action_context);
}

bool d1l_ui_channel_sheets_render_import(
    d1l_ui_channel_sheets_controller_t *controller,
    d1l_ui_channel_action_handler_t action_handler,
    void *action_context)
{
    if (!controller) {
        return false;
    }
    return render_input_sheet(
        controller, controller->import_sheet, "Import Channel",
        "Paste an official MeshCore channel share URI.",
        "meshcore://channel/add?...", "", D1L_CHANNEL_SHARE_URI_LEN - 1U, true,
        BINDING_IMPORT_BACK, D1L_UI_CHANNEL_ACTION_CANCEL_IMPORT,
        BINDING_IMPORT_SUBMIT, D1L_UI_CHANNEL_ACTION_SUBMIT_IMPORT,
        BINDING_IMPORT_KEYBOARD, &controller->import_textarea,
        &controller->import_keyboard, action_handler, action_context);
}

bool d1l_ui_channel_sheets_render_options(
    d1l_ui_channel_sheets_controller_t *controller,
    d1l_ui_channel_action_handler_t action_handler,
    void *action_context)
{
    lv_obj_t *sheet = controller ? controller->options_sheet : NULL;
    if (!begin_render(controller, sheet, true, action_handler, action_context)) {
        invalidate_render(controller, sheet);
        return false;
    }
    const d1l_channel_info_t *channel = &controller->selected;
    const bool is_public = channel->channel_id == D1L_CHANNEL_PUBLIC_ID;
    bool complete = true;
    complete = create_button(controller, sheet, "Back", 12, 6, 72, 44,
                             BINDING_OPTIONS_BACK,
                             D1L_UI_CHANNEL_ACTION_CLOSE_OPTIONS) != NULL && complete;
    complete = configure_title(create_label(sheet, channel->name, 0xF4F7FB)) && complete;
    complete = create_button(
        controller, sheet, is_public ? "Rename (protected)" : "Rename",
        16, 60, 448, 48, BINDING_OPTIONS_EDIT,
        is_public ? D1L_UI_CHANNEL_ACTION_NONE :
                    D1L_UI_CHANNEL_ACTION_OPEN_EDIT) != NULL && complete;
    const char *toggle_label = is_public ? "Public is always enabled" :
        (channel->enabled ? "Disable channel" : "Enable channel");
    complete = create_button(
        controller, sheet, toggle_label, 16, 116, 448, 48,
        BINDING_OPTIONS_TOGGLE,
        is_public ? D1L_UI_CHANNEL_ACTION_NONE :
                    D1L_UI_CHANNEL_ACTION_TOGGLE_ENABLED) != NULL && complete;
    const bool can_make_default = channel->enabled && !channel->is_default;
    const char *default_label = channel->is_default ? "Default channel" :
        (channel->enabled ? "Make default" : "Enable before making default");
    complete = create_button(
        controller, sheet, default_label,
        16, 172, 448, 48, BINDING_OPTIONS_DEFAULT,
        can_make_default ? D1L_UI_CHANNEL_ACTION_MAKE_DEFAULT :
                           D1L_UI_CHANNEL_ACTION_NONE) != NULL && complete;
    const bool qr_sharing_available = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI);
    if (qr_sharing_available) {
        complete = create_button(controller, sheet, "Export one-time QR",
                                 16, 228, 448, 48, BINDING_OPTIONS_EXPORT,
                                 D1L_UI_CHANNEL_ACTION_OPEN_EXPORT) != NULL &&
            complete;
    }
    lv_obj_t *remove = create_button(
        controller, sheet, is_public ? "Remove (protected)" : "Remove channel",
        16, qr_sharing_available ? 284 : 228, 448, 52,
        BINDING_OPTIONS_REMOVE,
        is_public ? D1L_UI_CHANNEL_ACTION_NONE : D1L_UI_CHANNEL_ACTION_OPEN_REMOVE);
    if (!is_public) {
        style_danger_button(remove);
    }
    complete = remove != NULL && complete;
    lv_obj_t *note = create_label(
        sheet,
        is_public ? "Public cannot be renamed, disabled, or removed."
                  : "Channel details are redacted; secrets are never shown here.",
        is_public ? 0xFBBF24 : 0xA6B0B7);
    if (note) {
        lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);
        lv_obj_set_width(note, 448);
        lv_obj_set_pos(note, 16, qr_sharing_available ? 356 : 300);
    } else {
        complete = false;
    }
    return finish_render(controller, sheet, complete);
}

bool d1l_ui_channel_sheets_render_edit(
    d1l_ui_channel_sheets_controller_t *controller,
    d1l_ui_channel_action_handler_t action_handler,
    void *action_context)
{
    if (!controller || !d1l_ui_channel_sheets_channel(controller) ||
        controller->selected.channel_id == D1L_CHANNEL_PUBLIC_ID) {
        invalidate_render(controller, controller ? controller->edit_sheet : NULL);
        return false;
    }
    return render_input_sheet(
        controller, controller->edit_sheet, "Rename Channel",
        "Only the local display name changes.", "Channel name",
        controller->selected.name, D1L_CHANNEL_NAME_LEN - 1U, false,
        BINDING_EDIT_BACK, D1L_UI_CHANNEL_ACTION_CANCEL_EDIT,
        BINDING_EDIT_SUBMIT, D1L_UI_CHANNEL_ACTION_SUBMIT_EDIT,
        BINDING_EDIT_KEYBOARD, &controller->edit_textarea,
        &controller->edit_keyboard, action_handler, action_context);
}

bool d1l_ui_channel_sheets_render_export(
    d1l_ui_channel_sheets_controller_t *controller,
    d1l_ui_channel_action_handler_t action_handler,
    void *action_context)
{
    lv_obj_t *sheet = controller ? controller->export_sheet : NULL;
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI) ||
        !begin_render(controller, sheet, true, action_handler, action_context) ||
        controller->export_uri[0] == '\0') {
        invalidate_render(controller, sheet);
        return false;
    }
    bool complete = true;
    complete = create_button(controller, sheet, "Close", 12, 6, 72, 44,
                             BINDING_EXPORT_BACK,
                             D1L_UI_CHANNEL_ACTION_CLOSE_EXPORT) != NULL && complete;
    complete = configure_title(create_label(sheet, "One-time Channel QR", 0xF4F7FB)) && complete;
#if LV_USE_QRCODE
    lv_obj_t *qr = lv_qrcode_create(
        sheet, 238, lv_color_hex(0x0E0F10), lv_color_hex(0xF8FAFC));
    if (!qr) {
        complete = false;
    } else {
        lv_obj_set_pos(qr, 121, 70);
        lv_obj_set_style_border_width(qr, 6, 0);
        lv_obj_set_style_border_color(qr, lv_color_hex(0xF8FAFC), 0);
        if (lv_qrcode_update(qr, controller->export_uri,
                             strlen(controller->export_uri)) != LV_RES_OK) {
            lv_obj_del(qr);
            complete = false;
        }
    }
#else
    complete = false;
#endif
    d1l_ui_channel_sheets_clear_export_uri(controller);
    lv_obj_t *name = create_label(sheet, controller->selected.name, 0xF4F7FB);
    if (name) {
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 448);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(name, 16, 326);
    } else {
        complete = false;
    }
    lv_obj_t *note = create_label(
        sheet, "URI is held only long enough to encode this QR.", 0xA6B0B7);
    if (note) {
        lv_obj_set_width(note, 448);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(note, 16, 358);
    } else {
        complete = false;
    }
    return finish_render(controller, sheet, complete);
}

bool d1l_ui_channel_sheets_render_remove(
    d1l_ui_channel_sheets_controller_t *controller,
    d1l_ui_channel_action_handler_t action_handler,
    void *action_context)
{
    lv_obj_t *sheet = controller ? controller->remove_sheet : NULL;
    if (!controller || !d1l_ui_channel_sheets_channel(controller) ||
        controller->selected.channel_id == D1L_CHANNEL_PUBLIC_ID ||
        !begin_render(controller, sheet, true, action_handler, action_context)) {
        invalidate_render(controller, sheet);
        return false;
    }
    bool complete = true;
    complete = create_button(controller, sheet, "Back", 12, 6, 72, 44,
                             BINDING_REMOVE_BACK,
                             D1L_UI_CHANNEL_ACTION_CANCEL_REMOVE) != NULL && complete;
    complete = configure_title(create_label(sheet, "Remove Channel?", 0xF4F7FB)) && complete;
    lv_obj_t *name = create_label(sheet, controller->selected.name, 0xFCA5A5);
    if (name) {
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 448);
        lv_obj_set_pos(name, 16, 78);
    } else {
        complete = false;
    }
    lv_obj_t *warning = create_label(
        sheet,
        "This removes channel access from this device. Stored message history "
        "is not erased by this action.",
        0xF4F7FB);
    if (warning) {
        lv_label_set_long_mode(warning, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(warning, 448);
        lv_obj_set_pos(warning, 16, 124);
    } else {
        complete = false;
    }
    complete = create_button(controller, sheet, "Cancel", 16, 272, 448, 52,
                             BINDING_REMOVE_CANCEL,
                             D1L_UI_CHANNEL_ACTION_CANCEL_REMOVE) != NULL && complete;
    lv_obj_t *confirm = create_button(
        controller, sheet, "Remove Channel", 16, 340, 448, 52,
        BINDING_REMOVE_CONFIRM, D1L_UI_CHANNEL_ACTION_CONFIRM_REMOVE);
    style_danger_button(confirm);
    complete = confirm != NULL && complete;
    return finish_render(controller, sheet, complete);
}

static void hide_sheet(d1l_ui_channel_sheets_controller_t *controller,
                       lv_obj_t *sheet, bool clear_input, bool clean)
{
    if (!controller) {
        return;
    }
    deactivate_actions(controller);
    if (clear_input) {
        detach_sheet_input(controller, sheet, true);
    }
    if (sheet == controller->export_sheet) {
        d1l_ui_channel_sheets_clear_export_uri(controller);
    }
    if (object_is_valid(sheet)) {
        d1l_ui_modal_hide(sheet);
        if (clean) {
            lv_obj_clean(sheet);
            clear_input_refs(controller, sheet);
        }
    }
}

void d1l_ui_channel_sheets_hide_all(
    d1l_ui_channel_sheets_controller_t *controller,
    bool clear_channel)
{
    if (!controller) {
        return;
    }
    deactivate_actions(controller);
    detach_sheet_input(controller, controller->create_sheet, true);
    detach_sheet_input(controller, controller->import_sheet, true);
    detach_sheet_input(controller, controller->edit_sheet, true);
    d1l_ui_channel_sheets_clear_export_uri(controller);
    lv_obj_t *sheets[] = {
        controller->create_sheet,
        controller->import_sheet,
        controller->options_sheet,
        controller->edit_sheet,
        controller->export_sheet,
        controller->remove_sheet,
    };
    for (size_t i = 0U; i < sizeof(sheets) / sizeof(sheets[0]); ++i) {
        if (object_is_valid(sheets[i])) {
            d1l_ui_modal_hide(sheets[i]);
        }
    }
    if (object_is_valid(controller->export_sheet)) {
        lv_obj_clean(controller->export_sheet);
    }
    if (clear_channel) {
        memset(&controller->selected, 0, sizeof(controller->selected));
    }
}

void d1l_ui_channel_sheets_hide_create(
    d1l_ui_channel_sheets_controller_t *controller)
{
    hide_sheet(controller, controller ? controller->create_sheet : NULL,
               true, false);
}

void d1l_ui_channel_sheets_hide_import(
    d1l_ui_channel_sheets_controller_t *controller)
{
    hide_sheet(controller, controller ? controller->import_sheet : NULL,
               true, false);
}

void d1l_ui_channel_sheets_hide_options(
    d1l_ui_channel_sheets_controller_t *controller)
{
    hide_sheet(controller, controller ? controller->options_sheet : NULL,
               false, false);
}

void d1l_ui_channel_sheets_hide_edit(
    d1l_ui_channel_sheets_controller_t *controller)
{
    hide_sheet(controller, controller ? controller->edit_sheet : NULL,
               true, false);
}

void d1l_ui_channel_sheets_hide_export(
    d1l_ui_channel_sheets_controller_t *controller)
{
    hide_sheet(controller, controller ? controller->export_sheet : NULL,
               false, true);
}

void d1l_ui_channel_sheets_hide_remove(
    d1l_ui_channel_sheets_controller_t *controller)
{
    hide_sheet(controller, controller ? controller->remove_sheet : NULL,
               false, false);
}

static lv_obj_t *valid_object(lv_obj_t *object)
{
    return object_is_valid(object) ? object : NULL;
}

#define D1L_CHANNEL_SHEET_GETTER(function_name, member)                       \
    lv_obj_t *function_name(                                                  \
        const d1l_ui_channel_sheets_controller_t *controller)                 \
    {                                                                         \
        return controller ? valid_object(controller->member) : NULL;          \
    }

D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_create_sheet, create_sheet)
D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_create_textarea, create_textarea)
D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_create_keyboard, create_keyboard)
D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_import_sheet, import_sheet)
D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_import_textarea, import_textarea)
D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_import_keyboard, import_keyboard)
D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_options, options_sheet)
D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_edit, edit_sheet)
D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_edit_textarea, edit_textarea)
D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_edit_keyboard, edit_keyboard)
D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_export, export_sheet)
D1L_CHANNEL_SHEET_GETTER(d1l_ui_channel_sheets_remove, remove_sheet)

#endif /* D1L_UI_CHANNEL_SHEETS_SECRET_TEST */
