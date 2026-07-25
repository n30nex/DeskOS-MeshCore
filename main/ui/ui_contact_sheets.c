#include "ui_contact_sheets.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"
#include "lvgl.h"
#include "ui_keyboard.h"
#include "ui_modal.h"

enum {
    BINDING_DETAIL_BACK = 0,
    BINDING_DETAIL_MESSAGE,
    BINDING_DETAIL_OPTIONS,
    BINDING_OPTIONS_BACK,
    BINDING_OPTIONS_ROUTE,
    BINDING_OPTIONS_RENAME,
    BINDING_OPTIONS_FAVORITE,
    BINDING_OPTIONS_MUTE,
    BINDING_OPTIONS_EXPORT,
    BINDING_OPTIONS_FORGET,
    BINDING_FORGET_BACK,
    BINDING_FORGET_CANCEL,
    BINDING_FORGET_CONFIRM,
    BINDING_EXPORT_BACK,
    BINDING_EDIT_BACK,
    BINDING_EDIT_SAVE,
    BINDING_EDIT_KEYBOARD,
};

_Static_assert(sizeof(d1l_ui_contact_sheets_controller_t) <=
                   D1L_UI_CONTACT_SHEETS_CONTROLLER_MAX_BYTES,
               "Contact sheets controller exceeded its persistent-owner size budget");

bool d1l_ui_contact_action_available(d1l_ui_contact_action_t action)
{
    switch (action) {
    case D1L_UI_CONTACT_ACTION_MESSAGE:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_DIRECT_MESSAGES);
    case D1L_UI_CONTACT_ACTION_ROUTE_TRACE:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_USER_TRACE);
    case D1L_UI_CONTACT_ACTION_EXPORT:
    case D1L_UI_CONTACT_ACTION_CLOSE_EXPORT:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI);
    case D1L_UI_CONTACT_ACTION_CLOSE_DETAIL:
    case D1L_UI_CONTACT_ACTION_OPEN_OPTIONS:
    case D1L_UI_CONTACT_ACTION_CLOSE_OPTIONS:
    case D1L_UI_CONTACT_ACTION_RENAME:
    case D1L_UI_CONTACT_ACTION_TOGGLE_FAVORITE:
    case D1L_UI_CONTACT_ACTION_TOGGLE_MUTE:
    case D1L_UI_CONTACT_ACTION_OPEN_FORGET:
    case D1L_UI_CONTACT_ACTION_CANCEL_FORGET:
    case D1L_UI_CONTACT_ACTION_CONFIRM_FORGET:
    case D1L_UI_CONTACT_ACTION_SAVE_EDIT:
    case D1L_UI_CONTACT_ACTION_CANCEL_EDIT:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_BASIC_CONTACTS);
    case D1L_UI_CONTACT_ACTION_NONE:
    default:
        return false;
    }
}

static bool object_is_valid(const lv_obj_t *object)
{
    return object && lv_obj_is_valid(object);
}

static void advance_generation(d1l_ui_contact_sheets_controller_t *controller)
{
    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
    }
}

static void deactivate_actions(d1l_ui_contact_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    advance_generation(controller);
    controller->action_handler = NULL;
    controller->action_context = NULL;
    memset(controller->bindings, 0, sizeof(controller->bindings));
}

static void detach_edit_keyboard(d1l_ui_contact_sheets_controller_t *controller,
                                 bool clear_text)
{
    if (!controller) {
        return;
    }
    if (object_is_valid(controller->edit_keyboard)) {
        lv_keyboard_set_textarea(controller->edit_keyboard, NULL);
    }
    if (clear_text && object_is_valid(controller->edit_textarea)) {
        lv_textarea_set_text(controller->edit_textarea, "");
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

static void destroy_sheets(d1l_ui_contact_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    detach_edit_keyboard(controller, true);
    delete_sheet(&controller->detail_sheet);
    delete_sheet(&controller->options_sheet);
    delete_sheet(&controller->forget_sheet);
    delete_sheet(&controller->edit_sheet);
    delete_sheet(&controller->export_sheet);
    memset(controller, 0, sizeof(*controller));
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
    lv_obj_set_style_bg_color(sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_width(sheet, 0, 0);
    lv_obj_set_style_pad_all(sheet, 0, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    d1l_ui_modal_hide(sheet);
    return sheet;
}

bool d1l_ui_contact_sheets_create(
    d1l_ui_contact_sheets_controller_t *controller,
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
    controller->detail_sheet = create_sheet(parent);
    if (!controller->detail_sheet) {
        destroy_sheets(controller);
        return false;
    }
    controller->options_sheet = create_sheet(parent);
    if (!controller->options_sheet) {
        destroy_sheets(controller);
        return false;
    }
    controller->forget_sheet = create_sheet(parent);
    if (!controller->forget_sheet) {
        destroy_sheets(controller);
        return false;
    }
    if (d1l_release_feature_available(
            D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI)) {
        controller->export_sheet = create_sheet(parent);
        if (!controller->export_sheet) {
            destroy_sheets(controller);
            return false;
        }
    }
    return true;
}

bool d1l_ui_contact_sheets_set_contact(
    d1l_ui_contact_sheets_controller_t *controller,
    const d1l_contact_entry_t *contact,
    d1l_ui_dm_identity_reason_t dm_identity_reason,
    bool can_export,
    uint8_t meshcore_type_id)
{
    if (!controller || !contact || contact->fingerprint[0] == '\0') {
        return false;
    }
    memset(&controller->rendered, 0, sizeof(controller->rendered));
    controller->rendered.contact = *contact;
    controller->rendered.dm_identity_reason = dm_identity_reason;
    controller->rendered.can_dm =
        dm_identity_reason == D1L_UI_DM_IDENTITY_READY;
    controller->rendered.can_export = can_export &&
        d1l_release_feature_available(
            D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI);
    controller->rendered.meshcore_type_id = meshcore_type_id;
    return true;
}

bool d1l_ui_contact_sheets_set_export_uri(
    d1l_ui_contact_sheets_controller_t *controller,
    const char *uri)
{
    if (!controller || !uri || !controller->rendered.can_export ||
        !d1l_release_feature_available(
            D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI)) {
        return false;
    }
    const char *end = memchr(uri, '\0', sizeof(controller->rendered.export_uri));
    if (!end || end == uri) {
        controller->rendered.export_uri[0] = '\0';
        return false;
    }
    size_t length = (size_t)(end - uri);
    memcpy(controller->rendered.export_uri, uri, length + 1U);
    return true;
}

const d1l_contact_entry_t *d1l_ui_contact_sheets_contact(
    const d1l_ui_contact_sheets_controller_t *controller)
{
    if (!controller || controller->rendered.contact.fingerprint[0] == '\0') {
        return NULL;
    }
    return &controller->rendered.contact;
}

const char *d1l_ui_contact_sheets_edit_text(
    const d1l_ui_contact_sheets_controller_t *controller)
{
    if (!controller || !object_is_valid(controller->edit_textarea)) {
        return NULL;
    }
    return lv_textarea_get_text(controller->edit_textarea);
}

static d1l_ui_contact_binding_t *set_binding(
    d1l_ui_contact_sheets_controller_t *controller,
    size_t slot,
    d1l_ui_contact_action_t action)
{
    if (!controller || slot >= D1L_UI_CONTACT_SHEETS_BINDING_COUNT) {
        return NULL;
    }
    d1l_ui_contact_binding_t *binding = &controller->bindings[slot];
    binding->controller = controller;
    binding->action = action;
    binding->generation = controller->generation;
    return binding;
}

static bool binding_is_current(const d1l_ui_contact_binding_t *binding)
{
    return binding && binding->controller && binding->generation != 0U &&
        binding->generation == binding->controller->generation;
}

static void dispatch_action(d1l_ui_contact_binding_t *binding,
                            d1l_ui_contact_action_t action,
                            const char *text)
{
    if (!binding_is_current(binding) ||
        !binding->controller->action_handler ||
        action <= D1L_UI_CONTACT_ACTION_NONE ||
        action > D1L_UI_CONTACT_ACTION_CANCEL_EDIT ||
        !d1l_ui_contact_action_available(action)) {
        return;
    }
    d1l_ui_contact_action_event_t event = {
        .action = action,
        .contact = d1l_ui_contact_sheets_contact(binding->controller),
        .text = text,
    };
    binding->controller->action_handler(
        &event, binding->controller->action_context);
}

static void action_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_contact_binding_t *binding =
        (d1l_ui_contact_binding_t *)lv_event_get_user_data(event);
    const char *text = binding &&
        binding->action == D1L_UI_CONTACT_ACTION_SAVE_EDIT ?
            d1l_ui_contact_sheets_edit_text(binding->controller) : NULL;
    dispatch_action(binding, binding ? binding->action : D1L_UI_CONTACT_ACTION_NONE,
                    text);
}

static void keyboard_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_contact_binding_t *binding =
        (d1l_ui_contact_binding_t *)lv_event_get_user_data(event);
    if (!binding_is_current(binding)) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        dispatch_action(binding, D1L_UI_CONTACT_ACTION_SAVE_EDIT,
                        d1l_ui_contact_sheets_edit_text(binding->controller));
    } else if (code == LV_EVENT_CANCEL) {
        dispatch_action(binding, D1L_UI_CONTACT_ACTION_CANCEL_EDIT, NULL);
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

static lv_obj_t *create_panel(lv_obj_t *parent, int x, int y,
                              int width, int height)
{
    if (!object_is_valid(parent)) {
        return NULL;
    }
    lv_obj_t *panel = lv_obj_create(parent);
    if (!panel) {
        return NULL;
    }
    lv_obj_set_size(panel, width, height);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x263241), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t *create_button(
    d1l_ui_contact_sheets_controller_t *controller,
    lv_obj_t *parent,
    const char *text,
    int x,
    int y,
    int width,
    int height,
    size_t binding_slot,
    d1l_ui_contact_action_t action)
{
    if (!controller || !object_is_valid(parent) || !text) {
        return NULL;
    }
    lv_obj_t *button = lv_btn_create(parent);
    if (!button) {
        return NULL;
    }
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1E2A36), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x263545), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = create_label(button, text, 0xF4F7FB);
    if (!label) {
        lv_obj_del(button);
        return NULL;
    }
    lv_obj_center(label);
    d1l_ui_contact_binding_t *binding =
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
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label) {
        lv_obj_set_style_text_color(label, lv_color_hex(0xFCA5A5), 0);
    }
}

static bool style_option_button(lv_obj_t *button, uint32_t accent,
                                const char *detail)
{
    if (!object_is_valid(button)) {
        return false;
    }
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (!label) {
        return false;
    }
    lv_obj_set_style_text_color(label, lv_color_hex(accent), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *detail_label = create_label(button, detail ? detail : "", 0x8EA0AE);
    if (!detail_label) {
        return false;
    }
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(detail_label, 220);
    lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(detail_label, LV_ALIGN_RIGHT_MID, -12, 0);
    return true;
}

static bool begin_render(d1l_ui_contact_sheets_controller_t *controller,
                         lv_obj_t *sheet,
                         d1l_ui_contact_action_handler_t action_handler,
                         void *action_context)
{
    if (!controller || !object_is_valid(sheet) || !action_handler ||
        !d1l_ui_contact_sheets_contact(controller)) {
        return false;
    }
    deactivate_actions(controller);
    controller->action_handler = action_handler;
    controller->action_context = action_context;
    lv_obj_clean(sheet);
    return true;
}

static void invalidate_render(d1l_ui_contact_sheets_controller_t *controller,
                              lv_obj_t *sheet)
{
    if (!controller) {
        return;
    }
    deactivate_actions(controller);
    if (sheet == controller->edit_sheet) {
        detach_edit_keyboard(controller, false);
    }
    if (object_is_valid(sheet)) {
        d1l_ui_modal_hide(sheet);
        lv_obj_clean(sheet);
    }
    if (sheet == controller->edit_sheet) {
        controller->edit_title = NULL;
        controller->edit_textarea = NULL;
        controller->edit_keyboard = NULL;
    }
}

static bool finish_render(d1l_ui_contact_sheets_controller_t *controller,
                          lv_obj_t *sheet, bool complete)
{
    if (!complete) {
        invalidate_render(controller, sheet);
        return false;
    }
    return true;
}

static bool configure_title(lv_obj_t *title, int y)
{
    if (!title) {
        return false;
    }
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 364);
    lv_obj_set_pos(title, 100, y);
    return true;
}

bool d1l_ui_contact_sheets_render_detail(
    d1l_ui_contact_sheets_controller_t *controller,
    d1l_ui_contact_action_handler_t action_handler,
    void *action_context)
{
    if (!begin_render(controller, controller ? controller->detail_sheet : NULL,
                      action_handler, action_context)) {
        invalidate_render(controller, controller ? controller->detail_sheet : NULL);
        return false;
    }
    lv_obj_t *sheet = controller->detail_sheet;
    const d1l_contact_entry_t *entry = &controller->rendered.contact;
    bool complete = true;
    complete = create_button(
        controller, sheet, "Back", 12, 6, 72, 44, BINDING_DETAIL_BACK,
        D1L_UI_CONTACT_ACTION_CLOSE_DETAIL) != NULL && complete;
    complete = configure_title(create_label(
        sheet, entry->alias[0] ? entry->alias : entry->fingerprint, 0xF4F7FB),
        10) && complete;

    char line[160];
    snprintf(line, sizeof(line), "%s  %s  %s",
             entry->type[0] ? entry->type : "node",
             entry->favorite ? "favorite" : "normal",
             entry->muted ? "unread excluded" : "unread counted");
    lv_obj_t *flags = create_label(sheet, line, 0x8EA0AE);
    if (flags) {
        lv_obj_set_pos(flags, 16, 64);
    } else {
        complete = false;
    }
    snprintf(line, sizeof(line), "fp %.16s", entry->fingerprint);
    lv_obj_t *fingerprint = create_label(sheet, line, 0xE5EDF5);
    if (fingerprint) {
        lv_obj_set_pos(fingerprint, 16, 98);
    } else {
        complete = false;
    }
    snprintf(line, sizeof(line), "%s  route %s  hops %u",
             entry->public_key_hex[0] ? "public key retained" : "no public key",
             entry->out_path_valid ? "direct" : "flood",
             (unsigned)(entry->out_path_valid ? entry->path_hops : 0U));
    lv_obj_t *key = create_label(sheet, line, 0x8EA0AE);
    if (key) {
        lv_label_set_long_mode(key, LV_LABEL_LONG_DOT);
        lv_obj_set_width(key, 448);
        lv_obj_set_pos(key, 16, 132);
    } else {
        complete = false;
    }
    const int snr_abs = entry->last_snr_tenths < 0 ?
        -entry->last_snr_tenths : entry->last_snr_tenths;
    snprintf(line, sizeof(line), "rssi %d  snr %s%d.%d  heard %.18s",
             entry->last_rssi_dbm,
             entry->last_snr_tenths < 0 ? "-" : "",
             snr_abs / 10, snr_abs % 10,
             entry->heard_name[0] ? entry->heard_name : "-");
    lv_obj_t *signal = create_label(sheet, line, 0x8EA0AE);
    if (signal) {
        lv_label_set_long_mode(signal, LV_LABEL_LONG_DOT);
        lv_obj_set_width(signal, 448);
        lv_obj_set_pos(signal, 16, 166);
    } else {
        complete = false;
    }
    lv_obj_t *actions = create_label(sheet, "Actions", 0x5EEAD4);
    if (actions) {
        lv_obj_set_pos(actions, 16, 210);
    } else {
        complete = false;
    }
    if (controller->rendered.can_dm) {
        complete = create_button(
            controller, sheet, "Message", 16, 238, 448, 52,
            BINDING_DETAIL_MESSAGE, D1L_UI_CONTACT_ACTION_MESSAGE) != NULL &&
            complete;
    } else {
        char dm_status[96];
        snprintf(dm_status, sizeof(dm_status), "DM unavailable [%s]",
                 d1l_ui_dm_identity_reason_code(
                     controller->rendered.dm_identity_reason));
        lv_obj_t *unavailable = create_label(sheet, dm_status, 0xFBBF24);
        if (unavailable) {
            lv_obj_set_pos(unavailable, 16, 238);
        } else {
            complete = false;
        }
        lv_obj_t *reason = create_label(
            sheet,
            d1l_ui_dm_identity_reason_text(
                controller->rendered.dm_identity_reason),
            0x8EA0AE);
        if (reason) {
            lv_label_set_long_mode(reason, LV_LABEL_LONG_WRAP);
            lv_obj_set_size(reason, 448, 42);
            lv_obj_set_pos(reason, 16, 262);
        } else {
            complete = false;
        }
    }
    complete = create_button(
        controller, sheet, "Contact options", 16, 304, 448, 52,
        BINDING_DETAIL_OPTIONS, D1L_UI_CONTACT_ACTION_OPEN_OPTIONS) != NULL &&
        complete;
    return finish_render(controller, sheet, complete);
}

bool d1l_ui_contact_sheets_render_options(
    d1l_ui_contact_sheets_controller_t *controller,
    d1l_ui_contact_action_handler_t action_handler,
    void *action_context)
{
    if (!begin_render(controller, controller ? controller->options_sheet : NULL,
                      action_handler, action_context)) {
        invalidate_render(controller, controller ? controller->options_sheet : NULL);
        return false;
    }
    lv_obj_t *sheet = controller->options_sheet;
    const d1l_contact_entry_t *entry = &controller->rendered.contact;
    bool complete = true;
    complete = create_button(
        controller, sheet, "Back", 12, 6, 72, 44, BINDING_OPTIONS_BACK,
        D1L_UI_CONTACT_ACTION_CLOSE_OPTIONS) != NULL && complete;
    complete = configure_title(create_label(sheet, "Contact Options", 0xF4F7FB), 8) &&
        complete;
    lv_obj_t *subtitle = create_label(
        sheet, entry->alias[0] ? entry->alias : entry->fingerprint, 0x8EA0AE);
    if (subtitle) {
        lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
        lv_obj_set_width(subtitle, 364);
        lv_obj_set_pos(subtitle, 100, 38);
    } else {
        complete = false;
    }

    lv_obj_t *button = NULL;
    if (d1l_ui_contact_action_available(
            D1L_UI_CONTACT_ACTION_ROUTE_TRACE)) {
        button = create_button(
            controller, sheet, "Route trace", 16, 64, 448, 48,
            BINDING_OPTIONS_ROUTE, D1L_UI_CONTACT_ACTION_ROUTE_TRACE);
        complete = style_option_button(button, 0x93C5FD, "Trace path  >") &&
            complete;
    }
    button = create_button(
        controller, sheet, "Rename", 16, 118, 448, 48,
        BINDING_OPTIONS_RENAME, D1L_UI_CONTACT_ACTION_RENAME);
    complete = style_option_button(button, 0x5EEAD4, "Change alias  >") && complete;
    button = create_button(
        controller, sheet,
        entry->favorite ? "Remove from favorites" : "Add to favorites",
        16, 172, 448, 48, BINDING_OPTIONS_FAVORITE,
        D1L_UI_CONTACT_ACTION_TOGGLE_FAVORITE);
    complete = style_option_button(
        button, 0xFBBF24, entry->favorite ? "On  >" : "Off  >") && complete;
    button = create_button(
        controller, sheet,
        entry->muted ? "Include in unread count" : "Exclude from unread count",
        16, 226, 448, 48, BINDING_OPTIONS_MUTE,
        D1L_UI_CONTACT_ACTION_TOGGLE_MUTE);
    complete = style_option_button(
        button, 0xC4B5FD,
        entry->muted ? "Excluded  >" : "Included  >") && complete;
    if (d1l_ui_contact_action_available(D1L_UI_CONTACT_ACTION_EXPORT)) {
        if (controller->rendered.can_export) {
            button = create_button(
                controller, sheet, "Export QR", 16, 280, 448, 48,
                BINDING_OPTIONS_EXPORT, D1L_UI_CONTACT_ACTION_EXPORT);
            complete = style_option_button(
                button, 0xA7F3D0, "Share QR  >") && complete;
        } else {
            lv_obj_t *panel = create_panel(sheet, 16, 280, 448, 48);
            if (!panel) {
                complete = false;
            } else {
                lv_obj_set_style_pad_all(panel, 0, 0);
                lv_obj_t *title = create_label(
                    panel, "Export unavailable", 0x8EA0AE);
                lv_obj_t *reason = create_label(
                    panel, "Missing key or role", 0x8EA0AE);
                if (title && reason) {
                    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);
                    lv_obj_align(reason, LV_ALIGN_RIGHT_MID, -12, 0);
                } else {
                    complete = false;
                }
            }
        }
    }
    button = create_button(
        controller, sheet, "Forget contact", 16, 334, 448, 48,
        BINDING_OPTIONS_FORGET, D1L_UI_CONTACT_ACTION_OPEN_FORGET);
    style_danger_button(button);
    complete = style_option_button(button, 0xFCA5A5, "Requires confirmation  >") &&
        complete;
    return finish_render(controller, sheet, complete);
}

bool d1l_ui_contact_sheets_render_forget(
    d1l_ui_contact_sheets_controller_t *controller,
    d1l_ui_contact_action_handler_t action_handler,
    void *action_context)
{
    if (!begin_render(controller, controller ? controller->forget_sheet : NULL,
                      action_handler, action_context)) {
        invalidate_render(controller, controller ? controller->forget_sheet : NULL);
        return false;
    }
    lv_obj_t *sheet = controller->forget_sheet;
    const d1l_contact_entry_t *entry = &controller->rendered.contact;
    bool complete = true;
    complete = create_button(
        controller, sheet, "Back", 12, 6, 72, 44, BINDING_FORGET_BACK,
        D1L_UI_CONTACT_ACTION_CANCEL_FORGET) != NULL && complete;
    complete = configure_title(create_label(sheet, "Forget Contact?", 0xF4F7FB), 10) &&
        complete;
    lv_obj_t *alias = create_label(
        sheet, entry->alias[0] ? entry->alias : entry->fingerprint, 0xFCA5A5);
    if (alias) {
        lv_label_set_long_mode(alias, LV_LABEL_LONG_DOT);
        lv_obj_set_width(alias, 448);
        lv_obj_set_pos(alias, 16, 76);
    } else {
        complete = false;
    }
    lv_obj_t *warning = create_panel(sheet, 16, 112, 448, 122);
    if (!warning) {
        complete = false;
    } else {
        lv_obj_t *line1 = create_label(
            warning, "This removes the saved contact and its routing preferences.",
            0xF4F7FB);
        lv_obj_t *line2 = create_label(
            warning, "Message history remains on this device.", 0x8EA0AE);
        if (line1 && line2) {
            lv_label_set_long_mode(line1, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(line1, 424);
            lv_obj_set_pos(line1, 0, 2);
            lv_label_set_long_mode(line2, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(line2, 424);
            lv_obj_set_pos(line2, 0, 62);
        } else {
            complete = false;
        }
    }
    complete = create_button(
        controller, sheet, "Cancel", 16, 274, 448, 52,
        BINDING_FORGET_CANCEL, D1L_UI_CONTACT_ACTION_CANCEL_FORGET) != NULL &&
        complete;
    lv_obj_t *confirm = create_button(
        controller, sheet, "Forget Contact", 16, 340, 448, 52,
        BINDING_FORGET_CONFIRM, D1L_UI_CONTACT_ACTION_CONFIRM_FORGET);
    style_danger_button(confirm);
    complete = confirm != NULL && complete;
    return finish_render(controller, sheet, complete);
}

bool d1l_ui_contact_sheets_render_export(
    d1l_ui_contact_sheets_controller_t *controller,
    d1l_ui_contact_action_handler_t action_handler,
    void *action_context)
{
    if (!d1l_ui_contact_action_available(D1L_UI_CONTACT_ACTION_EXPORT)) {
        return false;
    }
    if (!begin_render(controller, controller ? controller->export_sheet : NULL,
                      action_handler, action_context)) {
        invalidate_render(controller, controller ? controller->export_sheet : NULL);
        return false;
    }
    lv_obj_t *sheet = controller->export_sheet;
    const d1l_contact_entry_t *entry = &controller->rendered.contact;
    bool complete = true;
    complete = configure_title(create_label(sheet, "Contact Export", 0xF4F7FB), 10) &&
        complete;
    complete = create_button(
        controller, sheet, "Back", 12, 6, 72, 44, BINDING_EXPORT_BACK,
        D1L_UI_CONTACT_ACTION_CLOSE_EXPORT) != NULL && complete;
    char subtitle_text[96];
    snprintf(subtitle_text, sizeof(subtitle_text), "MeshCore QR  %.16s  type %u",
             entry->fingerprint, (unsigned)controller->rendered.meshcore_type_id);
    lv_obj_t *subtitle = create_label(sheet, subtitle_text, 0x8EA0AE);
    if (subtitle) {
        lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
        lv_obj_set_width(subtitle, 448);
        lv_obj_set_pos(subtitle, 16, 60);
    } else {
        complete = false;
    }
    if (controller->rendered.export_uri[0] == '\0') {
        lv_obj_t *missing = create_label(
            sheet, "No retained public key for QR export", 0xF87171);
        if (missing) {
            lv_obj_set_pos(missing, 16, 104);
        } else {
            complete = false;
        }
        return finish_render(controller, sheet, complete);
    }

#if LV_USE_QRCODE
    lv_obj_t *qr = lv_qrcode_create(
        sheet, 166, lv_color_hex(0x02060A), lv_color_hex(0xF8FAFC));
    if (!qr) {
        complete = false;
    } else {
        lv_obj_set_pos(qr, 16, 92);
        lv_obj_set_style_border_width(qr, 5, 0);
        lv_obj_set_style_border_color(qr, lv_color_hex(0xF8FAFC), 0);
        if (lv_qrcode_update(
                qr, controller->rendered.export_uri,
                strlen(controller->rendered.export_uri)) != LV_RES_OK) {
            lv_obj_del(qr);
            lv_obj_t *error = create_label(sheet, "QR payload too long", 0xF87171);
            if (error) {
                lv_obj_set_pos(error, 24, 150);
            } else {
                complete = false;
            }
        }
    }
#else
    lv_obj_t *disabled = create_label(
        sheet, "QR renderer disabled in LVGL", 0xFBBF24);
    if (disabled) {
        lv_obj_set_pos(disabled, 24, 150);
    } else {
        complete = false;
    }
#endif

    lv_obj_t *name = create_label(
        sheet, entry->alias[0] ? entry->alias : entry->fingerprint, 0xE5EDF5);
    if (name) {
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 264);
        lv_obj_set_pos(name, 200, 100);
    } else {
        complete = false;
    }
    lv_obj_t *key = create_label(sheet, "public key retained", 0x5EEAD4);
    if (key) {
        lv_obj_set_pos(key, 200, 134);
    } else {
        complete = false;
    }
    lv_obj_t *uri_title = create_label(sheet, "URI", 0x8EA0AE);
    if (uri_title) {
        lv_obj_set_pos(uri_title, 200, 168);
    } else {
        complete = false;
    }
    lv_obj_t *uri = create_label(
        sheet, controller->rendered.export_uri, 0x93C5FD);
    if (uri) {
        lv_label_set_long_mode(uri, LV_LABEL_LONG_DOT);
        lv_obj_set_width(uri, 264);
        lv_obj_set_pos(uri, 200, 194);
    } else {
        complete = false;
    }
    lv_obj_t *note = create_label(
        sheet, "Scan with a MeshCore client or copy from serial", 0x8EA0AE);
    if (note) {
        lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);
        lv_obj_set_width(note, 448);
        lv_obj_set_pos(note, 16, 278);
    } else {
        complete = false;
    }
    return finish_render(controller, sheet, complete);
}

bool d1l_ui_contact_sheets_render_edit(
    d1l_ui_contact_sheets_controller_t *controller,
    d1l_ui_contact_action_handler_t action_handler,
    void *action_context)
{
    if (!controller || !object_is_valid(controller->parent) ||
        !d1l_ui_contact_sheets_contact(controller)) {
        return false;
    }
    if (!object_is_valid(controller->edit_sheet)) {
        controller->edit_sheet = create_sheet(controller->parent);
        if (!controller->edit_sheet) {
            return false;
        }
    }
    detach_edit_keyboard(controller, false);
    controller->edit_title = NULL;
    controller->edit_textarea = NULL;
    controller->edit_keyboard = NULL;
    if (!begin_render(controller, controller->edit_sheet,
                      action_handler, action_context)) {
        invalidate_render(controller, controller->edit_sheet);
        return false;
    }
    lv_obj_t *sheet = controller->edit_sheet;
    const d1l_contact_entry_t *entry = &controller->rendered.contact;
    bool complete = true;
    char title_text[48];
    snprintf(title_text, sizeof(title_text), "Rename %.32s",
             entry->alias[0] ? entry->alias : entry->fingerprint);
    controller->edit_title = create_label(sheet, title_text, 0xF4F7FB);
    complete = configure_title(controller->edit_title, 10) && complete;
    complete = create_button(
        controller, sheet, "Back", 12, 6, 72, 44, BINDING_EDIT_BACK,
        D1L_UI_CONTACT_ACTION_CANCEL_EDIT) != NULL && complete;
    complete = create_button(
        controller, sheet, "Save", 16, 360, 448, 52, BINDING_EDIT_SAVE,
        D1L_UI_CONTACT_ACTION_SAVE_EDIT) != NULL && complete;
    lv_obj_t *meta = create_label(
        sheet, "Alias only; retained history remains", 0x8EA0AE);
    if (meta) {
        lv_obj_set_pos(meta, 16, 60);
    } else {
        complete = false;
    }
    controller->edit_textarea = lv_textarea_create(sheet);
    if (!controller->edit_textarea) {
        complete = false;
    } else {
        lv_obj_set_size(controller->edit_textarea, 448, 48);
        lv_obj_set_pos(controller->edit_textarea, 16, 88);
        lv_textarea_set_one_line(controller->edit_textarea, true);
        lv_textarea_set_max_length(
            controller->edit_textarea, D1L_CONTACT_ALIAS_LEN - 1U);
        lv_textarea_set_placeholder_text(
            controller->edit_textarea, "Contact alias");
        lv_obj_set_style_radius(controller->edit_textarea, 8, 0);
        lv_obj_set_style_bg_color(
            controller->edit_textarea, lv_color_hex(0x071018), 0);
        lv_obj_set_style_border_color(
            controller->edit_textarea, lv_color_hex(0x263241), 0);
        lv_obj_set_style_text_color(
            controller->edit_textarea, lv_color_hex(0xF4F7FB), 0);
        lv_obj_set_style_text_color(
            controller->edit_textarea, lv_color_hex(0x8EA0AE),
            LV_PART_TEXTAREA_PLACEHOLDER);
        lv_textarea_set_text(
            controller->edit_textarea,
            entry->alias[0] ? entry->alias : entry->fingerprint);
    }
    controller->edit_keyboard = lv_keyboard_create(sheet);
    if (!controller->edit_keyboard || !controller->edit_textarea) {
        complete = false;
    } else {
        d1l_ui_keyboard_configure_input(
            controller->edit_keyboard, controller->edit_textarea,
            16, 148, 448, 200);
        d1l_ui_contact_binding_t *binding = set_binding(
            controller, BINDING_EDIT_KEYBOARD, D1L_UI_CONTACT_ACTION_NONE);
        if (!binding) {
            complete = false;
        } else {
            lv_obj_add_event_cb(
                controller->edit_keyboard, keyboard_event_cb,
                LV_EVENT_READY, binding);
            lv_obj_add_event_cb(
                controller->edit_keyboard, keyboard_event_cb,
                LV_EVENT_CANCEL, binding);
            lv_keyboard_set_textarea(
                controller->edit_keyboard, controller->edit_textarea);
        }
    }
    return finish_render(controller, sheet, complete);
}

static void hide_sheet(d1l_ui_contact_sheets_controller_t *controller,
                       lv_obj_t *sheet, bool edit)
{
    if (!controller) {
        return;
    }
    deactivate_actions(controller);
    if (edit) {
        detach_edit_keyboard(controller, true);
    }
    if (object_is_valid(sheet)) {
        d1l_ui_modal_hide(sheet);
    }
}

void d1l_ui_contact_sheets_hide_all(
    d1l_ui_contact_sheets_controller_t *controller,
    bool clear_contact)
{
    if (!controller) {
        return;
    }
    deactivate_actions(controller);
    detach_edit_keyboard(controller, true);
    lv_obj_t *sheets[] = {
        controller->detail_sheet,
        controller->options_sheet,
        controller->forget_sheet,
        controller->edit_sheet,
        controller->export_sheet,
    };
    for (size_t i = 0U; i < sizeof(sheets) / sizeof(sheets[0]); ++i) {
        if (object_is_valid(sheets[i])) {
            d1l_ui_modal_hide(sheets[i]);
        }
    }
    if (clear_contact) {
        memset(&controller->rendered, 0, sizeof(controller->rendered));
    }
}

void d1l_ui_contact_sheets_hide_detail(
    d1l_ui_contact_sheets_controller_t *controller)
{
    hide_sheet(controller, controller ? controller->detail_sheet : NULL, false);
}

void d1l_ui_contact_sheets_hide_options(
    d1l_ui_contact_sheets_controller_t *controller)
{
    hide_sheet(controller, controller ? controller->options_sheet : NULL, false);
}

void d1l_ui_contact_sheets_hide_forget(
    d1l_ui_contact_sheets_controller_t *controller)
{
    hide_sheet(controller, controller ? controller->forget_sheet : NULL, false);
}

void d1l_ui_contact_sheets_hide_edit(
    d1l_ui_contact_sheets_controller_t *controller)
{
    hide_sheet(controller, controller ? controller->edit_sheet : NULL, true);
}

void d1l_ui_contact_sheets_hide_export(
    d1l_ui_contact_sheets_controller_t *controller)
{
    hide_sheet(controller, controller ? controller->export_sheet : NULL, false);
    if (controller) {
        controller->rendered.export_uri[0] = '\0';
    }
}

static lv_obj_t *valid_object(lv_obj_t *object)
{
    return object_is_valid(object) ? object : NULL;
}

lv_obj_t *d1l_ui_contact_sheets_detail(
    const d1l_ui_contact_sheets_controller_t *controller)
{
    return controller ? valid_object(controller->detail_sheet) : NULL;
}

lv_obj_t *d1l_ui_contact_sheets_options(
    const d1l_ui_contact_sheets_controller_t *controller)
{
    return controller ? valid_object(controller->options_sheet) : NULL;
}

lv_obj_t *d1l_ui_contact_sheets_forget(
    const d1l_ui_contact_sheets_controller_t *controller)
{
    return controller ? valid_object(controller->forget_sheet) : NULL;
}

lv_obj_t *d1l_ui_contact_sheets_edit(
    const d1l_ui_contact_sheets_controller_t *controller)
{
    return controller ? valid_object(controller->edit_sheet) : NULL;
}

lv_obj_t *d1l_ui_contact_sheets_edit_textarea(
    const d1l_ui_contact_sheets_controller_t *controller)
{
    return controller ? valid_object(controller->edit_textarea) : NULL;
}

lv_obj_t *d1l_ui_contact_sheets_edit_keyboard(
    const d1l_ui_contact_sheets_controller_t *controller)
{
    return controller ? valid_object(controller->edit_keyboard) : NULL;
}

lv_obj_t *d1l_ui_contact_sheets_export(
    const d1l_ui_contact_sheets_controller_t *controller)
{
    return controller ? valid_object(controller->export_sheet) : NULL;
}
