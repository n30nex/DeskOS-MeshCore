#include "ui_service_sheets.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "lvgl.h"
#include "mesh/user_text.h"
#include "ui_keyboard.h"
#include "ui_modal.h"

enum {
    BINDING_CLOSE_TERMINAL = 0,
    BINDING_TERMINAL_LEVEL,
    BINDING_TERMINAL_CLEAR,
    BINDING_CLOSE_OBSERVER,
    BINDING_OBSERVER_TOGGLE,
    BINDING_OBSERVER_REGION_FOCUS,
    BINDING_OBSERVER_REGION_SAVE,
    BINDING_OBSERVER_KEYBOARD,
    BINDING_CLOSE_UPDATE,
    BINDING_UPDATE_INSTALL,
    BINDING_UPDATE_CANCEL,
    BINDING_UPDATE_REBOOT,
    BINDING_CLOSE_NOTIFICATIONS,
    BINDING_NOTIFICATIONS_MODE,
    BINDING_OPEN_MESSAGES,
    BINDING_CLOSE_ADMIN,
    BINDING_ADMIN_REFRESH,
    BINDING_ADMIN_TELEMETRY,
    BINDING_ADMIN_NEIGHBOURS,
    BINDING_ADMIN_NEIGHBOURS_NEXT,
    BINDING_ADMIN_ACCESS_LIST,
    BINDING_ADMIN_CLEAR_STATS,
    BINDING_ADMIN_ADVERTISE_ZERO_HOP,
    BINDING_ADMIN_LOGOUT,
    BINDING_ADMIN_PASSWORD_FOCUS,
    BINDING_ADMIN_LOGIN,
    BINDING_ADMIN_ROOM_FOCUS,
    BINDING_ADMIN_ROOM_SEND,
    BINDING_ADMIN_ACL_FOCUS,
    BINDING_ADMIN_ACL_APPLY,
    BINDING_ADMIN_ROOM_READ_ONLY_ON,
    BINDING_ADMIN_ROOM_READ_ONLY_OFF,
    BINDING_ADMIN_CLI_FOCUS,
    BINDING_ADMIN_CLI_SEND,
    BINDING_ADMIN_CLI_SECURE_TOGGLE,
    BINDING_ADMIN_KEYBOARD,
    BINDING_ADMIN_SHOW_HUB,
    BINDING_ADMIN_SHOW_STATUS,
    BINDING_ADMIN_SHOW_TELEMETRY,
    BINDING_ADMIN_SHOW_NEIGHBOURS,
    BINDING_ADMIN_SHOW_ACCESS,
    BINDING_ADMIN_SHOW_TOOLS,
    BINDING_ADMIN_SHOW_ROOM,
    BINDING_ADMIN_SHOW_TERMINAL,
    BINDING_ADMIN_SHOW_ACL,
    BINDING_ADMIN_REMEMBER_TOGGLE,
    BINDING_ADMIN_FORGET_PASSWORD,
};

_Static_assert(sizeof(d1l_ui_service_sheets_controller_t) <=
                   D1L_UI_SERVICE_SHEETS_CONTROLLER_MAX_BYTES,
               "Service sheets controller exceeded its size budget");

static bool action_valid(d1l_ui_service_action_t action)
{
    return action > D1L_UI_SERVICE_ACTION_NONE &&
           action <= D1L_UI_SERVICE_ACTION_ADMIN_FORGET_PASSWORD;
}

static void advance_generation(
    d1l_ui_service_sheets_controller_t *controller)
{
    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
    }
}

static void clear_admin_sensitive_input(
    d1l_ui_service_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    if (controller->admin_keyboard &&
        lv_obj_is_valid(controller->admin_keyboard)) {
        d1l_ui_keyboard_clear_textarea(controller->admin_keyboard);
    }
    if (controller->admin_password_textarea &&
        lv_obj_is_valid(controller->admin_password_textarea)) {
        lv_textarea_set_text(controller->admin_password_textarea, "");
    }
    if (controller->admin_room_textarea &&
        lv_obj_is_valid(controller->admin_room_textarea)) {
        lv_textarea_set_text(controller->admin_room_textarea, "");
    }
    if (controller->admin_acl_textarea &&
        lv_obj_is_valid(controller->admin_acl_textarea)) {
        lv_textarea_set_text(controller->admin_acl_textarea, "");
    }
    if (controller->admin_cli_textarea &&
        lv_obj_is_valid(controller->admin_cli_textarea)) {
        lv_textarea_set_text(controller->admin_cli_textarea, "");
    }
}

static void deactivate_actions(
    d1l_ui_service_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    advance_generation(controller);
    controller->action_handler = NULL;
    controller->action_context = NULL;
    memset(controller->bindings, 0, sizeof(controller->bindings));
}

static bool binding_current(const d1l_ui_service_binding_t *binding)
{
    return binding && binding->controller && binding->generation != 0U &&
           binding->generation == binding->controller->generation;
}

static void action_event_cb(lv_event_t *event)
{
    d1l_ui_service_binding_t *binding = event ?
        (d1l_ui_service_binding_t *)lv_event_get_user_data(event) : NULL;
    if (!binding_current(binding) || !action_valid(binding->action) ||
        !binding->controller->action_handler) {
        return;
    }
    binding->controller->action_handler(
        binding->action, binding->controller->action_context);
}

static void admin_password_focus_event_cb(lv_event_t *event)
{
    d1l_ui_service_binding_t *binding = event ?
        (d1l_ui_service_binding_t *)lv_event_get_user_data(event) : NULL;
    if (!binding_current(binding)) {
        return;
    }
    d1l_ui_service_sheets_controller_t *controller = binding->controller;
    (void)d1l_ui_keyboard_focus_textarea_from_event(
        controller->admin_keyboard, event,
        controller->admin_password_textarea, NULL);
}

static void observer_region_focus_event_cb(lv_event_t *event)
{
    d1l_ui_service_binding_t *binding = event ?
        (d1l_ui_service_binding_t *)lv_event_get_user_data(event) : NULL;
    if (!binding_current(binding)) {
        return;
    }
    d1l_ui_service_sheets_controller_t *controller = binding->controller;
    if (d1l_ui_keyboard_focus_textarea_from_event(
            controller->observer_keyboard, event,
            controller->observer_region_textarea, NULL)) {
        lv_obj_clear_flag(controller->observer_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(controller->observer_keyboard);
    }
}

static void observer_keyboard_event_cb(lv_event_t *event)
{
    d1l_ui_service_binding_t *binding = event ?
        (d1l_ui_service_binding_t *)lv_event_get_user_data(event) : NULL;
    if (!binding_current(binding) ||
        !binding->controller->action_handler) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CANCEL) {
        d1l_ui_keyboard_clear_textarea(
            binding->controller->observer_keyboard);
        lv_obj_add_flag(binding->controller->observer_keyboard,
                        LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_READY) {
        binding->controller->action_handler(
            D1L_UI_SERVICE_ACTION_OBSERVER_REGION_SAVE,
            binding->controller->action_context);
    }
}

static void admin_cli_focus_event_cb(lv_event_t *event)
{
    d1l_ui_service_binding_t *binding = event ?
        (d1l_ui_service_binding_t *)lv_event_get_user_data(event) : NULL;
    if (!binding_current(binding)) {
        return;
    }
    d1l_ui_service_sheets_controller_t *controller = binding->controller;
    (void)d1l_ui_keyboard_focus_textarea_from_event(
        controller->admin_keyboard, event,
        controller->admin_cli_textarea, NULL);
}

static void admin_room_focus_event_cb(lv_event_t *event)
{
    d1l_ui_service_binding_t *binding = event ?
        (d1l_ui_service_binding_t *)lv_event_get_user_data(event) : NULL;
    if (!binding_current(binding)) {
        return;
    }
    d1l_ui_service_sheets_controller_t *controller = binding->controller;
    (void)d1l_ui_keyboard_focus_textarea_from_event(
        controller->admin_keyboard, event,
        controller->admin_room_textarea, NULL);
}

static void admin_acl_focus_event_cb(lv_event_t *event)
{
    d1l_ui_service_binding_t *binding = event ?
        (d1l_ui_service_binding_t *)lv_event_get_user_data(event) : NULL;
    if (!binding_current(binding)) {
        return;
    }
    d1l_ui_service_sheets_controller_t *controller = binding->controller;
    (void)d1l_ui_keyboard_focus_textarea_from_event(
        controller->admin_keyboard, event,
        controller->admin_acl_textarea, NULL);
}

static void admin_keyboard_event_cb(lv_event_t *event)
{
    d1l_ui_service_binding_t *binding = event ?
        (d1l_ui_service_binding_t *)lv_event_get_user_data(event) : NULL;
    if (!binding_current(binding) ||
        !binding->controller->action_handler) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) {
        return;
    }
    if (code == LV_EVENT_CANCEL) {
        d1l_ui_keyboard_clear_textarea(
            binding->controller->admin_keyboard);
        return;
    }
    d1l_ui_service_action_handler_t handler =
        binding->controller->action_handler;
    void *context = binding->controller->action_context;
    lv_obj_t *active_textarea = lv_keyboard_get_textarea(
        binding->controller->admin_keyboard);
    d1l_ui_service_action_t action =
        D1L_UI_SERVICE_ACTION_ADMIN_LOGIN;
    if (active_textarea == binding->controller->admin_room_textarea) {
        action = D1L_UI_SERVICE_ACTION_ADMIN_ROOM_SEND;
    } else if (active_textarea ==
               binding->controller->admin_acl_textarea) {
        action = D1L_UI_SERVICE_ACTION_ADMIN_ACL_APPLY;
    } else if (active_textarea ==
               binding->controller->admin_cli_textarea) {
        action = D1L_UI_SERVICE_ACTION_ADMIN_CLI_SEND;
    }
    handler(action, context);
}

static d1l_ui_service_binding_t *set_binding(
    d1l_ui_service_sheets_controller_t *controller,
    size_t slot,
    d1l_ui_service_action_t action)
{
    if (!controller || slot >= D1L_UI_SERVICE_SHEETS_BINDING_COUNT ||
        !action_valid(action)) {
        return NULL;
    }
    d1l_ui_service_binding_t *binding = &controller->bindings[slot];
    *binding = (d1l_ui_service_binding_t) {
        .controller = controller,
        .action = action,
        .generation = controller->generation,
    };
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

static void position_dot(lv_obj_t *label, int x, int y, int width)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
    lv_obj_set_pos(label, x, y);
}

static void position_wrap(lv_obj_t *label, int x, int y, int width)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width);
    lv_obj_set_pos(label, x, y);
}

static lv_obj_t *create_button(
    d1l_ui_service_sheets_controller_t *controller,
    lv_obj_t *parent,
    const char *text,
    int x,
    int y,
    int width,
    int height,
    size_t slot,
    d1l_ui_service_action_t action)
{
    d1l_ui_service_binding_t *binding =
        set_binding(controller, slot, action);
    if (!parent || !text || !binding) {
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
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2E3A43),
                              LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = create_label(button, text, 0xF4F7FB);
    if (!label) {
        lv_obj_del(button);
        return NULL;
    }
    lv_obj_center(label);
    lv_obj_add_event_cb(button, action_event_cb, LV_EVENT_CLICKED, binding);
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
    if (sheet && *sheet && lv_obj_is_valid(*sheet)) {
        d1l_ui_modal_hide(*sheet);
        lv_obj_del(*sheet);
    }
    if (sheet) {
        *sheet = NULL;
    }
}

static void destroy_sheets(
    d1l_ui_service_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    clear_admin_sensitive_input(controller);
    d1l_ui_keyboard_clear_textarea(controller->observer_keyboard);
    delete_sheet(&controller->terminal_sheet);
    delete_sheet(&controller->observer_sheet);
    delete_sheet(&controller->update_sheet);
    delete_sheet(&controller->notifications_sheet);
    delete_sheet(&controller->admin_sheet);
    memset(controller, 0, sizeof(*controller));
}

bool d1l_ui_service_sheets_create(
    d1l_ui_service_sheets_controller_t *controller,
    lv_obj_t *parent)
{
    if (!controller) {
        return false;
    }
    destroy_sheets(controller);
    if (!parent || !lv_obj_is_valid(parent)) {
        return false;
    }
    controller->terminal_sheet = create_sheet(parent, true);
    controller->observer_sheet = create_sheet(parent, false);
    controller->update_sheet = create_sheet(parent, false);
    controller->notifications_sheet = create_sheet(parent, false);
    controller->admin_sheet = create_sheet(parent, true);
    if (!controller->terminal_sheet || !controller->observer_sheet ||
        !controller->update_sheet || !controller->notifications_sheet ||
        !controller->admin_sheet) {
        destroy_sheets(controller);
        return false;
    }
    return true;
}

static bool begin_render(
    d1l_ui_service_sheets_controller_t *controller,
    lv_obj_t *sheet,
    d1l_ui_service_action_handler_t handler,
    void *context)
{
    if (!controller || !sheet || !lv_obj_is_valid(sheet) || !handler) {
        return false;
    }
    if (sheet == controller->admin_sheet) {
        lv_obj_update_layout(sheet);
        controller->admin_scroll_y = (int32_t)lv_obj_get_scroll_y(sheet);
        controller->admin_scroll_valid = true;
    }
    clear_admin_sensitive_input(controller);
    deactivate_actions(controller);
    controller->action_handler = handler;
    controller->action_context = context;
    lv_obj_clean(sheet);
    if (sheet == controller->observer_sheet) {
        controller->observer_region_textarea = NULL;
        controller->observer_keyboard = NULL;
    }
    if (sheet == controller->admin_sheet) {
        controller->admin_password_textarea = NULL;
        controller->admin_room_textarea = NULL;
        controller->admin_acl_textarea = NULL;
        controller->admin_cli_textarea = NULL;
        controller->admin_keyboard = NULL;
    }
    return true;
}

static void finish_admin_render(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet)
{
    if (!controller || sheet != controller->admin_sheet ||
        !controller->admin_scroll_valid || !lv_obj_is_valid(sheet)) {
        return;
    }
    lv_obj_update_layout(sheet);
    lv_obj_scroll_to_y(
        sheet, (lv_coord_t)controller->admin_scroll_y, LV_ANIM_OFF);
}

static bool render_header(
    d1l_ui_service_sheets_controller_t *controller,
    lv_obj_t *sheet,
    const char *title_text,
    size_t close_slot,
    d1l_ui_service_action_t close_action)
{
    lv_obj_t *title = create_label(sheet, title_text, 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        position_dot(title, 8, 4, 290);
    }
    lv_obj_t *close = create_button(
        controller, sheet, "Close", 340, 0, 76, 40,
        close_slot, close_action);
    return title && close;
}

bool d1l_ui_service_sheets_render_terminal(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_ui_terminal_sheet_input_t *input,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context)
{
    lv_obj_t *sheet = controller ? controller->terminal_sheet : NULL;
    if (!input || input->entry_count > D1L_UI_TERMINAL_PREVIEW_COUNT ||
        !begin_render(controller, sheet, action_handler, action_context)) {
        return false;
    }
    bool complete = render_header(
        controller, sheet, "Terminal", BINDING_CLOSE_TERMINAL,
        D1L_UI_SERVICE_ACTION_CLOSE_TERMINAL);
    char summary[128];
    snprintf(summary, sizeof(summary),
             "%u/%u events  level %s  dropped %lu",
             (unsigned)input->status.count,
             (unsigned)input->status.capacity,
             d1l_event_log_level_name(input->status.runtime_level),
             (unsigned long)input->status.dropped_oldest);
    lv_obj_t *status = create_label(sheet, summary, 0x20D9ED);
    position_dot(status, 8, 50, 408);
    complete = status && complete;
    char level_button[40];
    snprintf(level_button, sizeof(level_button), "Level: %s",
             d1l_event_log_level_name(input->status.runtime_level));
    complete = create_button(
        controller, sheet, level_button, 8, 78, 148, 44,
        BINDING_TERMINAL_LEVEL,
        D1L_UI_SERVICE_ACTION_TERMINAL_LEVEL) != NULL && complete;
    complete = create_button(
        controller, sheet,
        input->clear_armed ? "Confirm Clear" : "Clear Logs",
        166, 78, 132, 44, BINDING_TERMINAL_CLEAR,
        D1L_UI_SERVICE_ACTION_TERMINAL_CLEAR) != NULL && complete;
    lv_obj_t *policy = create_label(
        sheet,
        "Read-only structured events; secrets and raw remote commands are never retained.",
        0xFBBF24);
    position_wrap(policy, 8, 130, 408);
    complete = policy && complete;

    int y = 178;
    for (size_t i = 0U; i < input->entry_count; ++i) {
        const d1l_event_log_entry_t *entry = &input->entries[i];
        char line[160];
        snprintf(line, sizeof(line), "#%lu %s/%s: %s",
                 (unsigned long)entry->sequence,
                 entry->source, entry->kind, entry->message);
        const uint32_t color =
            entry->level == D1L_EVENT_LOG_LEVEL_ERROR ? 0xF87171 :
            entry->level == D1L_EVENT_LOG_LEVEL_WARN ? 0xFBBF24 :
            entry->level == D1L_EVENT_LOG_LEVEL_INFO ? 0xF4F7FB :
                                                       0xA6B0B7;
        lv_obj_t *label = create_label(sheet, line, color);
        position_wrap(label, 8, y, 408);
        complete = label && complete;
        y += 42;
    }
    return complete;
}

bool d1l_ui_service_sheets_render_observer(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_observer_status_t *status,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context)
{
    lv_obj_t *sheet = controller ? controller->observer_sheet : NULL;
    if (!status ||
        !begin_render(controller, sheet, action_handler, action_context)) {
        return false;
    }
    bool complete = render_header(
        controller, sheet, "Observer / MQTT", BINDING_CLOSE_OBSERVER,
        D1L_UI_SERVICE_ACTION_CLOSE_OBSERVER);
    char line[160];
    snprintf(line, sizeof(line), "State %s  links %u/%u  queued %lu/%lu",
             d1l_observer_state_name(status->state),
             (unsigned)status->connected_brokers,
             (unsigned)status->broker_count,
             (unsigned long)status->queued,
             (unsigned long)status->queue_capacity);
    lv_obj_t *state = create_label(sheet, line,
                                   status->connected ? 0x20D9ED : 0xFBBF24);
    position_dot(state, 8, 54, 408);
    complete = state && complete;
    snprintf(line, sizeof(line), "Canada 1 %s  Canada 2 %s  Custom %s",
             status->primary_connected ? "up" : "down",
             status->secondary_connected ? "up" : "down",
             status->custom_configured ?
                 (status->custom_connected ? "up" : "down") : "off");
    lv_obj_t *broker = create_label(sheet, line, 0xF4F7FB);
    position_dot(broker, 8, 84, 408);
    complete = broker && complete;
    snprintf(line, sizeof(line), "IATA %s  Topic %s%s",
             status->region[0] ? status->region : "---",
             status->topic[0] ? status->topic : "-",
             status->include_location ? "  + location" : "");
    lv_obj_t *topic = create_label(sheet, line, 0xA6B0B7);
    position_dot(topic, 8, 112, 408);
    complete = topic && complete;
    lv_obj_t *privacy = create_label(
        sheet,
        "Secure opt-in upload to both brokers; never forwards RF or exposes private keys.",
        0x4D7FFF);
    position_wrap(privacy, 8, 140, 408);
    complete = privacy && complete;

    lv_obj_t *region_label = create_label(
        sheet, "IATA region (3 letters)", 0xA6B0B7);
    position_dot(region_label, 8, 166, 132);
    complete = region_label && complete;
    controller->observer_region_textarea = lv_textarea_create(sheet);
    if (!controller->observer_region_textarea) {
        complete = false;
    } else {
        lv_obj_set_size(controller->observer_region_textarea, 96, 44);
        lv_obj_set_pos(controller->observer_region_textarea, 8, 188);
        lv_textarea_set_one_line(controller->observer_region_textarea, true);
        lv_textarea_set_max_length(controller->observer_region_textarea, 3U);
        lv_textarea_set_accepted_chars(controller->observer_region_textarea,
                                       "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
        lv_textarea_set_text(controller->observer_region_textarea,
                             status->region);
        d1l_ui_service_binding_t *focus = set_binding(
            controller, BINDING_OBSERVER_REGION_FOCUS,
            D1L_UI_SERVICE_ACTION_OBSERVER_REGION_SAVE);
        if (!focus) {
            complete = false;
        } else {
            lv_obj_add_event_cb(controller->observer_region_textarea,
                                observer_region_focus_event_cb,
                                LV_EVENT_FOCUSED, focus);
            lv_obj_add_event_cb(controller->observer_region_textarea,
                                observer_region_focus_event_cb,
                                LV_EVENT_CLICKED, focus);
        }
    }
    complete = create_button(
        controller, sheet, "Save", 112, 188, 82, 44,
        BINDING_OBSERVER_REGION_SAVE,
        D1L_UI_SERVICE_ACTION_OBSERVER_REGION_SAVE) != NULL && complete;
    if (status->configured) {
        complete = create_button(
            controller, sheet, status->enabled ? "Disable Uploads" :
                                                 "Enable Uploads",
            204, 188, 204, 44, BINDING_OBSERVER_TOGGLE,
            D1L_UI_SERVICE_ACTION_OBSERVER_TOGGLE) != NULL && complete;
    } else {
        lv_obj_t *setup = create_label(
            sheet,
            "Device identity is not ready. Complete setup before enabling the secure Observer uplink.",
            0xFBBF24);
        position_wrap(setup, 204, 188, 204);
        complete = setup && complete;
    }

    controller->observer_keyboard = lv_keyboard_create(sheet);
    if (!controller->observer_keyboard ||
        !controller->observer_region_textarea) {
        complete = false;
    } else {
        d1l_ui_keyboard_configure_compose(controller->observer_keyboard);
        d1l_ui_keyboard_configure_input(
            controller->observer_keyboard,
            controller->observer_region_textarea, 8, 76, 408, 220);
        lv_keyboard_set_mode(controller->observer_keyboard,
                             LV_KEYBOARD_MODE_TEXT_UPPER);
        d1l_ui_service_binding_t *keyboard = set_binding(
            controller, BINDING_OBSERVER_KEYBOARD,
            D1L_UI_SERVICE_ACTION_OBSERVER_REGION_SAVE);
        if (!keyboard) {
            complete = false;
        } else {
            lv_obj_add_event_cb(controller->observer_keyboard,
                                observer_keyboard_event_cb,
                                LV_EVENT_READY, keyboard);
            lv_obj_add_event_cb(controller->observer_keyboard,
                                observer_keyboard_event_cb,
                                LV_EVENT_CANCEL, keyboard);
        }
        lv_obj_add_flag(controller->observer_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    return complete;
}

bool d1l_ui_service_sheets_copy_observer_region(
    const d1l_ui_service_sheets_controller_t *controller,
    char out_region[D1L_OBSERVER_REGION_LEN])
{
    if (!controller || !out_region ||
        !controller->observer_region_textarea ||
        !lv_obj_is_valid(controller->observer_region_textarea)) {
        return false;
    }
    const char *text = lv_textarea_get_text(
        controller->observer_region_textarea);
    if (!text || strlen(text) != 3U) {
        return false;
    }
    memcpy(out_region, text, 3U);
    out_region[3] = '\0';
    return true;
}

bool d1l_ui_service_sheets_observer_edit_active(
    const d1l_ui_service_sheets_controller_t *controller)
{
    return controller && controller->observer_keyboard &&
        lv_obj_is_valid(controller->observer_keyboard) &&
        !lv_obj_has_flag(controller->observer_keyboard, LV_OBJ_FLAG_HIDDEN);
}

bool d1l_ui_service_sheets_render_update(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_update_status_t *status,
    bool install_armed,
    bool reboot_armed,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context)
{
    lv_obj_t *sheet = controller ? controller->update_sheet : NULL;
    if (!status ||
        !begin_render(controller, sheet, action_handler, action_context)) {
        return false;
    }
    bool complete = render_header(
        controller, sheet, "Signed Update", BINDING_CLOSE_UPDATE,
        D1L_UI_SERVICE_ACTION_CLOSE_UPDATE);
    char line[160];
    snprintf(line, sizeof(line), "State %s  %u%%  error %s",
             d1l_update_state_name(status->state),
             (unsigned)status->progress_percent,
             esp_err_to_name(status->last_error));
    lv_obj_t *state = create_label(
        sheet, line,
        status->state == D1L_UPDATE_STATE_ERROR ? 0xF87171 :
        status->state == D1L_UPDATE_STATE_REBOOT_REQUIRED ? 0x20D9ED :
                                                            0xFBBF24);
    position_dot(state, 8, 54, 408);
    complete = state && complete;
    snprintf(line, sizeof(line), "Running %s  target %s",
             status->running_partition[0] ? status->running_partition : "-",
             status->target_partition[0] ? status->target_partition : "-");
    lv_obj_t *partitions = create_label(sheet, line, 0xF4F7FB);
    position_dot(partitions, 8, 84, 408);
    complete = partitions && complete;
    snprintf(line, sizeof(line), "Version %s  sequence %lu/%lu",
             status->version[0] ? status->version : "not staged",
             (unsigned long)status->security_sequence,
             (unsigned long)status->highest_security_sequence);
    lv_obj_t *version = create_label(sheet, line, 0xA6B0B7);
    position_dot(version, 8, 112, 408);
    complete = version && complete;
    lv_obj_t *policy = create_label(
        sheet,
        "Local SD only. Manifest, target, partition table, image hash, Ed25519 signature, and anti-downgrade sequence are verified before the inactive slot is written.",
        0x4D7FFF);
    position_wrap(policy, 8, 148, 408);
    complete = policy && complete;

    if (status->reboot_required) {
        complete = create_button(
            controller, sheet,
            reboot_armed ? "Confirm Reboot" : "Reboot to Update",
            8, 250, 176, 44, BINDING_UPDATE_REBOOT,
            D1L_UI_SERVICE_ACTION_UPDATE_REBOOT) != NULL && complete;
    } else if (status->cancel_allowed || status->install_requested) {
        complete = create_button(
            controller, sheet, "Cancel Before Write",
            8, 250, 188, 44, BINDING_UPDATE_CANCEL,
            D1L_UI_SERVICE_ACTION_UPDATE_CANCEL) != NULL && complete;
    } else {
        complete = create_button(
            controller, sheet,
            install_armed ? "Confirm Install" : "Install from SD",
            8, 250, 176, 44, BINDING_UPDATE_INSTALL,
            D1L_UI_SERVICE_ACTION_UPDATE_INSTALL) != NULL && complete;
    }
    return complete;
}

bool d1l_ui_service_sheets_render_notifications(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_ui_notifications_sheet_input_t *input,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context)
{
    lv_obj_t *sheet = controller ? controller->notifications_sheet : NULL;
    if (!input ||
        !begin_render(controller, sheet, action_handler, action_context)) {
        return false;
    }
    bool complete = render_header(
        controller, sheet, "Notifications",
        BINDING_CLOSE_NOTIFICATIONS,
        D1L_UI_SERVICE_ACTION_CLOSE_NOTIFICATIONS);
    char line[128];
    snprintf(line, sizeof(line),
             "Public %lu  Direct %lu  muted/excluded %lu",
             (unsigned long)input->public_unread,
             (unsigned long)input->dm_unread,
             (unsigned long)input->muted_unread);
    lv_obj_t *counts = create_label(
        sheet, line,
        input->public_unread || input->dm_unread ? 0xFBBF24 : 0x20D9ED);
    position_dot(counts, 8, 58, 408);
    complete = counts && complete;
    snprintf(line, sizeof(line), "Backlight: %s",
             d1l_notification_mode_name(input->mode));
    lv_obj_t *mode = create_label(sheet, line, 0xF4F7FB);
    position_dot(mode, 8, 96, 408);
    complete = mode && complete;
    lv_obj_t *privacy = create_label(
        sheet,
        "Badges follow retained read cursors. Duplicate packets do not create duplicate counts. Quiet hours suppress only the backlight pulse from 22:00 to 07:00; no audio is claimed.",
        0x4D7FFF);
    position_wrap(privacy, 8, 132, 408);
    complete = privacy && complete;
    complete = create_button(
        controller, sheet, "Cycle Backlight", 8, 238, 166, 44,
        BINDING_NOTIFICATIONS_MODE,
        D1L_UI_SERVICE_ACTION_NOTIFICATIONS_MODE) != NULL && complete;
    complete = create_button(
        controller, sheet, "Open Messages", 186, 238, 158, 44,
        BINDING_OPEN_MESSAGES,
        D1L_UI_SERVICE_ACTION_OPEN_MESSAGES) != NULL && complete;
    return complete;
}

static const char *admin_state_name(d1l_meshcore_admin_state_t state)
{
    switch (state) {
    case D1L_MESHCORE_ADMIN_IDLE:
        return "idle";
    case D1L_MESHCORE_ADMIN_LOGIN_PENDING:
        return "login_pending";
    case D1L_MESHCORE_ADMIN_AUTHENTICATED:
        return "authenticated";
    case D1L_MESHCORE_ADMIN_STATUS_PENDING:
        return "status_pending";
    case D1L_MESHCORE_ADMIN_MUTATION_PENDING:
        return "mutation_pending";
    case D1L_MESHCORE_ADMIN_CLI_PENDING:
        return "cli_pending";
    case D1L_MESHCORE_ADMIN_QUERY_PENDING:
        return "query_pending";
    case D1L_MESHCORE_ADMIN_TIMED_OUT:
        return "timed_out";
    case D1L_MESHCORE_ADMIN_REJECTED_CREDENTIALS:
        return "rejected_credentials";
    case D1L_MESHCORE_ADMIN_DISCONNECTED:
        return "disconnected";
    case D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL:
        return "unsupported_protocol";
    case D1L_MESHCORE_ADMIN_RADIO_BUSY:
        return "radio_busy";
    case D1L_MESHCORE_ADMIN_VOLATILE_REPLAY_REJECTED:
        return "volatile_replay_rejected";
    case D1L_MESHCORE_ADMIN_DURABLE_REPLAY_REJECTED:
        return "durable_replay_rejected";
    case D1L_MESHCORE_ADMIN_LOCAL_STORAGE_FAILED:
        return "local_storage_failed";
    default:
        return "invalid";
    }
}

static const char *admin_role_name(d1l_meshcore_admin_role_t role)
{
    return role == D1L_MESHCORE_ADMIN_ROLE_REPEATER ? "repeater" :
           role == D1L_MESHCORE_ADMIN_ROLE_ROOM ? "room" : "none";
}

static bool admin_state_pending(d1l_meshcore_admin_state_t state)
{
    return state == D1L_MESHCORE_ADMIN_LOGIN_PENDING ||
        state == D1L_MESHCORE_ADMIN_STATUS_PENDING ||
        state == D1L_MESHCORE_ADMIN_MUTATION_PENDING ||
        state == D1L_MESHCORE_ADMIN_CLI_PENDING ||
        state == D1L_MESHCORE_ADMIN_QUERY_PENDING;
}

static const char *admin_permission_name(uint8_t permissions)
{
    switch (permissions & D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK) {
    case D1L_MESHCORE_ADMIN_PERMISSION_ADMIN:
        return "Admin access";
    case D1L_MESHCORE_ADMIN_PERMISSION_WRITE:
        return "Write access";
    case D1L_MESHCORE_ADMIN_PERMISSION_READ_ONLY:
        return "Read-only access";
    case D1L_MESHCORE_ADMIN_PERMISSION_GUEST:
    default:
        return "Guest access";
    }
}

static const char *admin_failure_message(d1l_meshcore_admin_state_t state)
{
    switch (state) {
    case D1L_MESHCORE_ADMIN_TIMED_OUT:
        return "The server did not reply. Check the route and try again.";
    case D1L_MESHCORE_ADMIN_REJECTED_CREDENTIALS:
        return "That password was rejected. Check it and try again.";
    case D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL:
        return "This server firmware does not support remote management.";
    case D1L_MESHCORE_ADMIN_RADIO_BUSY:
        return "The radio is busy. Wait a moment and try again.";
    case D1L_MESHCORE_ADMIN_VOLATILE_REPLAY_REJECTED:
    case D1L_MESHCORE_ADMIN_DURABLE_REPLAY_REJECTED:
        return "An old reply was ignored. Start a fresh login.";
    case D1L_MESHCORE_ADMIN_LOCAL_STORAGE_FAILED:
        return "DeskOS could not protect this session. Check storage.";
    case D1L_MESHCORE_ADMIN_DISCONNECTED:
        return "The management session ended. Sign in again.";
    case D1L_MESHCORE_ADMIN_IDLE:
    default:
        return "Enter the password used by this server.";
    }
}

static lv_obj_t *create_admin_panel(lv_obj_t *parent, int x, int y,
                                    int width, int height, uint32_t border)
{
    lv_obj_t *panel = lv_obj_create(parent);
    if (!panel) {
        return NULL;
    }
    lv_obj_set_size(panel, width, height);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x171F24), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static bool render_admin_compact_header(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    const char *title_text, bool show_back)
{
    lv_obj_t *title = create_label(sheet, title_text, 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        position_dot(title, 8, 4, show_back ? 226 : 290);
    }
    bool complete = title != NULL;
    if (show_back) {
        complete = create_button(
            controller, sheet, "Back", 254, 0, 76, 44,
            BINDING_ADMIN_SHOW_HUB,
            D1L_UI_SERVICE_ACTION_ADMIN_SHOW_HUB) != NULL && complete;
    }
    complete = create_button(
        controller, sheet, "Close", 340, 0, 76, 44,
        BINDING_CLOSE_ADMIN,
        show_back ? D1L_UI_SERVICE_ACTION_ADMIN_SHOW_HUB :
                    D1L_UI_SERVICE_ACTION_CLOSE_ADMIN) != NULL && complete;
    return complete;
}

static bool render_admin_target(
    lv_obj_t *sheet, const d1l_meshcore_admin_snapshot_t *status,
    const char *selected_fingerprint, const char *selected_name,
    int y, int height)
{
    lv_obj_t *panel = create_admin_panel(sheet, 8, y, 408, height, 0x34566A);
    if (!panel) {
        return false;
    }
    const char *name = selected_name && selected_name[0] ? selected_name :
        (status->role == D1L_MESHCORE_ADMIN_ROLE_ROOM ?
             "Room server" : "Repeater");
    const char *fingerprint = status->fingerprint[0] ? status->fingerprint :
        (selected_fingerprint && selected_fingerprint[0] ?
             selected_fingerprint : "-");
    lv_obj_t *name_label = create_label(panel, name, 0xF4F7FB);
    position_dot(name_label, 4, 0, 378);
    char meta[96];
    snprintf(meta, sizeof(meta), "%s  |  %.16s",
             status->state == D1L_MESHCORE_ADMIN_AUTHENTICATED ?
                 admin_permission_name(status->permissions) : "Verified server",
             fingerprint);
    lv_obj_t *meta_label = create_label(panel, meta, 0xA6B0B7);
    position_dot(meta_label, 4, 22, 378);
    return name_label && meta_label;
}

static lv_obj_t *create_admin_grid_button(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    const char *text, int x, int y, size_t slot,
    d1l_ui_service_action_t action, uint32_t accent)
{
    lv_obj_t *button = create_button(
        controller, sheet, text, x, y, 128, 62, slot, action);
    if (!button) {
        return NULL;
    }
    lv_obj_set_style_border_color(button, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x17252C), 0);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label) {
        lv_obj_set_style_text_color(label, lv_color_hex(accent), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    return button;
}

static void admin_progress_value(void *object, int32_t value)
{
    lv_obj_t *bar = (lv_obj_t *)object;
    if (bar && lv_obj_is_valid(bar)) {
        lv_bar_set_value(bar, value, LV_ANIM_OFF);
    }
}

static bool render_admin_pending(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    const d1l_meshcore_admin_snapshot_t *status,
    const char *selected_fingerprint, const char *selected_name)
{
    bool complete = render_admin_compact_header(
        controller, sheet, "Working", false);
    complete = render_admin_target(
        sheet, status, selected_fingerprint, selected_name, 48, 52) &&
        complete;
    const char *title = "Waiting for the server";
    const char *detail = "The request was sent. Waiting for a signed reply.";
    if (status->state == D1L_MESHCORE_ADMIN_LOGIN_PENDING) {
        title = "Signing in";
        detail = "Checking the password and opening a secure session.";
    } else if (status->state == D1L_MESHCORE_ADMIN_STATUS_PENDING) {
        title = "Updating status";
        detail = "Reading current radio and traffic information.";
    } else if (status->state == D1L_MESHCORE_ADMIN_QUERY_PENDING) {
        title = status->pending_query == D1L_MESHCORE_ADMIN_QUERY_TELEMETRY ?
            "Loading telemetry" :
            (status->pending_query == D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS ?
                 "Loading neighbours" : "Loading access list");
        detail = "The repeater is preparing the requested information.";
    } else if (status->state == D1L_MESHCORE_ADMIN_MUTATION_PENDING) {
        title = status->pending_mutation ==
                D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS ?
            "Clearing statistics" : "Sending zero-hop advert";
        detail = "Waiting for the repeater to confirm the change.";
    } else if (status->state == D1L_MESHCORE_ADMIN_CLI_PENDING) {
        title = "Running server command";
        detail = "Waiting for the command result from the server.";
    }
    lv_obj_t *title_label = create_label(sheet, title, 0x20D9ED);
    if (title_label) {
        lv_obj_set_style_text_font(title_label, &lv_font_montserrat_24, 0);
        lv_obj_set_width(title_label, 408);
        lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(title_label, 8, 118);
    }
    complete = title_label && complete;
    lv_obj_t *progress = lv_bar_create(sheet);
    if (progress) {
        lv_obj_set_size(progress, 320, 12);
        lv_obj_set_pos(progress, 52, 164);
        lv_bar_set_range(progress, 0, 100);
        lv_bar_set_value(progress, 8, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(
            progress, lv_color_hex(0x253447), LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            progress, lv_color_hex(0x20D9ED), LV_PART_INDICATOR);
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, progress);
        lv_anim_set_exec_cb(&animation, admin_progress_value);
        lv_anim_set_values(&animation, 8, 92);
        lv_anim_set_time(&animation, 1100);
        lv_anim_set_playback_time(&animation, 450);
        lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&animation);
    }
    complete = progress && complete;
    lv_obj_t *detail_label = create_label(sheet, detail, 0xF4F7FB);
    if (detail_label) {
        position_wrap(detail_label, 40, 194, 344);
        lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    complete = detail_label && complete;
    lv_obj_t *timeout = create_label(
        sheet, "A slow mesh route can take up to 60 seconds.", 0xA6B0B7);
    if (timeout) {
        lv_obj_set_width(timeout, 408);
        lv_obj_set_style_text_align(timeout, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(timeout, 8, 236);
    }
    complete = timeout && complete;
    complete = create_button(
        controller, sheet, "Cancel", 132, 254, 160, 44,
        BINDING_ADMIN_LOGOUT,
        D1L_UI_SERVICE_ACTION_ADMIN_LOGOUT) != NULL && complete;
    return complete;
}

static bool configure_admin_keyboard(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    lv_obj_t *textarea, int y, int height,
    d1l_ui_service_action_t ready_action)
{
    controller->admin_keyboard = lv_keyboard_create(sheet);
    if (!controller->admin_keyboard) {
        return false;
    }
    d1l_ui_keyboard_configure_compose(controller->admin_keyboard);
    d1l_ui_keyboard_configure_input(
        controller->admin_keyboard, textarea, 8, y, 408, height);
    d1l_ui_service_binding_t *binding = set_binding(
        controller, BINDING_ADMIN_KEYBOARD, ready_action);
    if (!binding) {
        return false;
    }
    lv_obj_add_event_cb(
        controller->admin_keyboard, admin_keyboard_event_cb,
        LV_EVENT_READY, binding);
    lv_obj_add_event_cb(
        controller->admin_keyboard, admin_keyboard_event_cb,
        LV_EVENT_CANCEL, binding);
    return true;
}

static bool render_admin_login_compact(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    const d1l_meshcore_admin_snapshot_t *status,
    const char *selected_fingerprint, const char *selected_name,
    bool saved_password_available, bool remember_password,
    const char *feedback, bool feedback_error)
{
    const bool room = status->role == D1L_MESHCORE_ADMIN_ROLE_ROOM;
    bool complete = render_admin_compact_header(
        controller, sheet, room ? "Room login" : "Repeater login", false);
    char target[96];
    snprintf(target, sizeof(target), "%s  |  %.16s",
             selected_name && selected_name[0] ? selected_name :
                 (room ? "Room server" : "Verified repeater"),
             selected_fingerprint && selected_fingerprint[0] ?
                 selected_fingerprint : "-");
    lv_obj_t *target_label = create_label(sheet, target, 0x20D9ED);
    position_dot(target_label, 8, 46, 408);
    complete = target_label && complete;
    const char *message = feedback && feedback[0] ? feedback :
        (saved_password_available ?
             "Saved password ready. Type to replace it." :
             admin_failure_message(status->state));
    lv_obj_t *message_label = create_label(
        sheet, message,
        (feedback_error ||
         (status->state != D1L_MESHCORE_ADMIN_IDLE &&
          status->state != D1L_MESHCORE_ADMIN_AUTHENTICATED)) ?
            0xF87171 : 0xA6B0B7);
    position_dot(message_label, 8, 68, 408);
    complete = message_label && complete;

    controller->admin_password_textarea = lv_textarea_create(sheet);
    if (controller->admin_password_textarea) {
        lv_obj_set_size(controller->admin_password_textarea, 408, 44);
        lv_obj_set_pos(controller->admin_password_textarea, 8, 90);
        lv_textarea_set_one_line(controller->admin_password_textarea, true);
        lv_textarea_set_password_mode(
            controller->admin_password_textarea, true);
        lv_textarea_set_max_length(
            controller->admin_password_textarea,
            D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES);
        lv_textarea_set_placeholder_text(
            controller->admin_password_textarea,
            saved_password_available ?
                "Saved password" : "Password (blank is allowed)");
        lv_textarea_set_text(controller->admin_password_textarea, "");
        lv_obj_set_style_radius(controller->admin_password_textarea, 8, 0);
        lv_obj_set_style_bg_color(
            controller->admin_password_textarea,
            lv_color_hex(0x101719), 0);
        lv_obj_set_style_border_color(
            controller->admin_password_textarea,
            lv_color_hex(0x20D9ED), 0);
        lv_obj_set_style_border_width(
            controller->admin_password_textarea, 2, 0);
        lv_obj_set_style_text_color(
            controller->admin_password_textarea,
            lv_color_hex(0xF4F7FB), 0);
        d1l_ui_service_binding_t *focus = set_binding(
            controller, BINDING_ADMIN_PASSWORD_FOCUS,
            D1L_UI_SERVICE_ACTION_ADMIN_LOGIN);
        if (focus) {
            lv_obj_add_event_cb(
                controller->admin_password_textarea,
                admin_password_focus_event_cb, LV_EVENT_FOCUSED, focus);
            lv_obj_add_event_cb(
                controller->admin_password_textarea,
                admin_password_focus_event_cb, LV_EVENT_CLICKED, focus);
        }
    }
    complete = controller->admin_password_textarea && complete;
    complete = configure_admin_keyboard(
        controller, sheet, controller->admin_password_textarea,
        140, 104, D1L_UI_SERVICE_ACTION_ADMIN_LOGIN) && complete;
    complete = create_button(
        controller, sheet, "Login", 8, 250, 108, 46,
        BINDING_ADMIN_LOGIN,
        D1L_UI_SERVICE_ACTION_ADMIN_LOGIN) != NULL && complete;
    complete = create_button(
        controller, sheet,
        remember_password ? "Save: On" : "Save: Off",
        124, 250, 132, 46,
        BINDING_ADMIN_REMEMBER_TOGGLE,
        D1L_UI_SERVICE_ACTION_ADMIN_REMEMBER_TOGGLE) != NULL && complete;
    if (saved_password_available) {
        complete = create_button(
            controller, sheet, "Forget saved", 264, 250, 152, 46,
            BINDING_ADMIN_FORGET_PASSWORD,
            D1L_UI_SERVICE_ACTION_ADMIN_FORGET_PASSWORD) != NULL && complete;
    } else {
        lv_obj_t *local = create_label(
            sheet, "Saved only on this D1L", 0x667787);
        position_dot(local, 270, 264, 140);
        complete = local && complete;
    }
    return complete;
}

static bool render_admin_hub_compact(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    const d1l_meshcore_admin_snapshot_t *status,
    const char *selected_fingerprint, const char *selected_name,
    const char *feedback, bool feedback_error)
{
    bool complete = render_admin_compact_header(
        controller, sheet,
        status->role == D1L_MESHCORE_ADMIN_ROLE_ROOM ?
            "Room manager" : "Repeater manager", false);
    complete = render_admin_target(
        sheet, status, selected_fingerprint, selected_name, 48, 50) &&
        complete;
    if (feedback && feedback[0]) {
        lv_obj_t *notice = create_label(
            sheet, feedback, feedback_error ? 0xF87171 : 0x84FF2E);
        position_dot(notice, 12, 100, 400);
        complete = notice && complete;
    }
    complete = create_admin_grid_button(
        controller, sheet, LV_SYMBOL_REFRESH "\nStatus",
        8, 120, BINDING_ADMIN_SHOW_STATUS,
        D1L_UI_SERVICE_ACTION_ADMIN_SHOW_STATUS, 0x20D9ED) != NULL && complete;
    complete = create_admin_grid_button(
        controller, sheet, LV_SYMBOL_CHARGE "\nTelemetry",
        144, 120, BINDING_ADMIN_SHOW_TELEMETRY,
        D1L_UI_SERVICE_ACTION_ADMIN_SHOW_TELEMETRY, 0x84FF2E) != NULL &&
        complete;
    complete = create_admin_grid_button(
        controller, sheet,
        status->role == D1L_MESHCORE_ADMIN_ROLE_ROOM ?
            LV_SYMBOL_ENVELOPE "\nRoom" : LV_SYMBOL_LIST "\nNeighbours",
        280, 120,
        status->role == D1L_MESHCORE_ADMIN_ROLE_ROOM ?
            BINDING_ADMIN_SHOW_ROOM : BINDING_ADMIN_SHOW_NEIGHBOURS,
        status->role == D1L_MESHCORE_ADMIN_ROLE_ROOM ?
            D1L_UI_SERVICE_ACTION_ADMIN_SHOW_ROOM :
            D1L_UI_SERVICE_ACTION_ADMIN_SHOW_NEIGHBOURS,
        0xFBBF24) != NULL && complete;
    complete = create_admin_grid_button(
        controller, sheet, LV_SYMBOL_EYE_OPEN "\nAccess",
        8, 190, BINDING_ADMIN_SHOW_ACCESS,
        D1L_UI_SERVICE_ACTION_ADMIN_SHOW_ACCESS, 0x7D93FF) != NULL && complete;
    complete = create_admin_grid_button(
        controller, sheet, LV_SYMBOL_SETTINGS "\nTools",
        144, 190, BINDING_ADMIN_SHOW_TOOLS,
        D1L_UI_SERVICE_ACTION_ADMIN_SHOW_TOOLS, 0xFBBF24) != NULL && complete;
    complete = create_admin_grid_button(
        controller, sheet, LV_SYMBOL_EDIT "\nConsole",
        280, 190, BINDING_ADMIN_SHOW_TERMINAL,
        D1L_UI_SERVICE_ACTION_ADMIN_SHOW_TERMINAL, 0x20D9ED) != NULL &&
        complete;
    char session[96];
    snprintf(session, sizeof(session), "%s  |  firmware level %u",
             admin_permission_name(status->permissions),
             (unsigned)status->firmware_level);
    lv_obj_t *session_label = create_label(sheet, session, 0xA6B0B7);
    position_dot(session_label, 8, 270, 286);
    complete = session_label && complete;
    complete = create_button(
        controller, sheet, "Logout", 304, 254, 112, 44,
        BINDING_ADMIN_LOGOUT,
        D1L_UI_SERVICE_ACTION_ADMIN_LOGOUT) != NULL && complete;
    return complete;
}

static bool render_admin_metric(lv_obj_t *sheet, const char *title,
                                const char *value, int x, int y,
                                uint32_t accent)
{
    lv_obj_t *panel = create_admin_panel(sheet, x, y, 128, 60, accent);
    if (!panel) {
        return false;
    }
    lv_obj_t *title_label = create_label(panel, title, 0xA6B0B7);
    position_dot(title_label, 4, 0, 104);
    lv_obj_t *value_label = create_label(panel, value, accent);
    if (value_label) {
        lv_obj_set_style_text_font(value_label, &lv_font_montserrat_24, 0);
        position_dot(value_label, 4, 20, 104);
    }
    return title_label && value_label;
}

static bool render_admin_status_compact(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    const d1l_meshcore_admin_snapshot_t *status,
    const char *feedback, bool feedback_error)
{
    bool complete = render_admin_compact_header(
        controller, sheet, "Server status", true);
    if (!status->status_valid) {
        lv_obj_t *empty = create_label(
            sheet, "No current status has been received yet.", 0xFBBF24);
        position_wrap(empty, 36, 92, 352);
        complete = empty && complete;
    } else {
        char signal[32];
        const int snr = status->status.last_snr_quarter_db;
        snprintf(signal, sizeof(signal), "%d / %s%d.%02d",
                 (int)status->status.last_rssi_dbm,
                 snr < 0 ? "-" : "", (snr < 0 ? -snr : snr) / 4,
                 ((snr < 0 ? -snr : snr) % 4) * 25);
        char queue[24];
        snprintf(queue, sizeof(queue), "%u queued",
                 (unsigned)status->status.tx_queue_length);
        char packets[32];
        snprintf(packets, sizeof(packets), "%lu / %lu",
                 (unsigned long)status->status.packets_received,
                 (unsigned long)status->status.packets_sent);
        char uptime[24];
        snprintf(uptime, sizeof(uptime), "%luh %02lum",
                 (unsigned long)(status->status.uptime_seconds / 3600U),
                 (unsigned long)((status->status.uptime_seconds / 60U) % 60U));
        char airtime[28];
        snprintf(airtime, sizeof(airtime), "%lus / %lus",
                 (unsigned long)status->status.tx_air_time_seconds,
                 (unsigned long)status->status.rx_air_time_seconds);
        char errors[24];
        snprintf(errors, sizeof(errors), "0x%04x",
                 (unsigned)status->status.error_flags);
        complete = render_admin_metric(
            sheet, "RSSI / SNR", signal, 8, 52, 0x20D9ED) && complete;
        complete = render_admin_metric(
            sheet, "Send queue", queue, 144, 52, 0x84FF2E) && complete;
        complete = render_admin_metric(
            sheet, "RX / TX packets", packets, 280, 52, 0x7D93FF) &&
            complete;
        complete = render_admin_metric(
            sheet, "Uptime", uptime, 8, 120, 0xFBBF24) && complete;
        complete = render_admin_metric(
            sheet, "TX / RX airtime", airtime, 144, 120, 0x20D9ED) &&
            complete;
        complete = render_admin_metric(
            sheet, "Error flags", errors, 280, 120,
            status->status.error_flags ? 0xF87171 : 0x84FF2E) && complete;
        char detail[160];
        snprintf(detail, sizeof(detail),
                 "Noise %d dBm  |  duplicates %u direct, %u flood  |  voltage %u mV",
                 (int)status->status.noise_floor_dbm,
                 (unsigned)status->status.direct_duplicates,
                 (unsigned)status->status.flood_duplicates,
                 (unsigned)status->status.battery_millivolts);
        lv_obj_t *detail_label = create_label(sheet, detail, 0xA6B0B7);
        position_dot(detail_label, 8, 194, 408);
        complete = detail_label && complete;
    }
    if (feedback && feedback[0]) {
        lv_obj_t *notice = create_label(
            sheet, feedback, feedback_error ? 0xF87171 : 0x84FF2E);
        position_dot(notice, 8, 224, 408);
        complete = notice && complete;
    }
    complete = create_button(
        controller, sheet, "Refresh status", 112, 254, 200, 44,
        BINDING_ADMIN_REFRESH,
        D1L_UI_SERVICE_ACTION_ADMIN_REFRESH) != NULL && complete;
    return complete;
}

static d1l_ui_service_action_t admin_query_action(d1l_ui_admin_page_t page)
{
    return page == D1L_UI_ADMIN_PAGE_NEIGHBOURS ?
        D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS :
        (page == D1L_UI_ADMIN_PAGE_ACCESS ?
             D1L_UI_SERVICE_ACTION_ADMIN_ACCESS_LIST :
             D1L_UI_SERVICE_ACTION_ADMIN_TELEMETRY);
}

static bool render_admin_data_compact(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    const d1l_meshcore_admin_snapshot_t *status, d1l_ui_admin_page_t page,
    const char *feedback, bool feedback_error)
{
    const char *title = page == D1L_UI_ADMIN_PAGE_NEIGHBOURS ?
        "Neighbours" :
        (page == D1L_UI_ADMIN_PAGE_ACCESS ? "Access list" : "Telemetry");
    bool complete = render_admin_compact_header(
        controller, sheet, title, true);
    lv_obj_t *result = create_admin_panel(sheet, 8, 52, 408, 172, 0x34566A);
    if (result) {
        lv_obj_add_flag(result, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(result, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(result, LV_SCROLLBAR_MODE_AUTO);
        const bool matches = status->query_result.valid &&
            ((page == D1L_UI_ADMIN_PAGE_TELEMETRY &&
              status->query_result.kind == D1L_MESHCORE_ADMIN_QUERY_TELEMETRY) ||
             (page == D1L_UI_ADMIN_PAGE_NEIGHBOURS &&
              status->query_result.kind == D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS) ||
             (page == D1L_UI_ADMIN_PAGE_ACCESS &&
              status->query_result.kind == D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST));
        lv_obj_t *text = create_label(
            result, matches ? status->query_result.text :
                "No result yet. Tap Refresh to ask the server.",
            matches && status->query_result.truncated ? 0xFBBF24 : 0xF4F7FB);
        position_wrap(text, 4, 2, 376);
        complete = text && complete;
    }
    complete = result && complete;
    if (feedback && feedback[0]) {
        lv_obj_t *notice = create_label(
            sheet, feedback, feedback_error ? 0xF87171 : 0x84FF2E);
        position_dot(notice, 8, 230, 408);
        complete = notice && complete;
    }
    if (page == D1L_UI_ADMIN_PAGE_NEIGHBOURS &&
        status->query_result.valid &&
        status->query_result.kind == D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS &&
        status->query_result.count > 0U &&
        (uint32_t)status->query_result.offset +
                (uint32_t)status->query_result.count <
            status->query_result.total) {
        complete = create_button(
            controller, sheet, "Next page", 8, 254, 128, 44,
            BINDING_ADMIN_NEIGHBOURS_NEXT,
            D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS_NEXT) != NULL && complete;
    }
    complete = create_button(
        controller, sheet, "Refresh", 146, 254, 128, 44,
        page == D1L_UI_ADMIN_PAGE_TELEMETRY ? BINDING_ADMIN_TELEMETRY :
            (page == D1L_UI_ADMIN_PAGE_NEIGHBOURS ?
                 BINDING_ADMIN_NEIGHBOURS : BINDING_ADMIN_ACCESS_LIST),
        admin_query_action(page)) != NULL && complete;
    if (page == D1L_UI_ADMIN_PAGE_ACCESS &&
        (status->permissions & D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK) ==
            D1L_MESHCORE_ADMIN_PERMISSION_ADMIN) {
        complete = create_button(
            controller, sheet, "Edit access", 284, 254, 132, 44,
            BINDING_ADMIN_SHOW_ACL,
            D1L_UI_SERVICE_ACTION_ADMIN_SHOW_ACL) != NULL && complete;
    }
    return complete;
}

static bool render_admin_tools_compact(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    const d1l_meshcore_admin_snapshot_t *status,
    d1l_meshcore_admin_mutation_t armed_mutation,
    bool room_read_only_on_armed, bool room_read_only_off_armed,
    const char *feedback, bool feedback_error)
{
    bool complete = render_admin_compact_header(
        controller, sheet, "Server tools", true);
    const uint8_t permission =
        status->permissions & D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK;
    if (permission != D1L_MESHCORE_ADMIN_PERMISSION_ADMIN) {
        lv_obj_t *readonly = create_label(
            sheet, "These tools require server administrator access.",
            0xFBBF24);
        position_wrap(readonly, 24, 92, 376);
        complete = readonly && complete;
    } else {
        complete = create_button(
            controller, sheet,
            armed_mutation == D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS ?
                "Confirm clear stats" : "Clear statistics",
            8, 56, 196, 64,
            BINDING_ADMIN_CLEAR_STATS,
            D1L_UI_SERVICE_ACTION_ADMIN_CLEAR_STATS) != NULL && complete;
        complete = create_button(
            controller, sheet,
            armed_mutation ==
                    D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP ?
                "Confirm advert" : "Zero-hop advert",
            220, 56, 196, 64,
            BINDING_ADMIN_ADVERTISE_ZERO_HOP,
            D1L_UI_SERVICE_ACTION_ADMIN_ADVERTISE_ZERO_HOP) != NULL &&
            complete;
        if (status->role == D1L_MESHCORE_ADMIN_ROLE_ROOM) {
            complete = create_button(
                controller, sheet,
                room_read_only_on_armed ?
                    "Confirm guest read" : "Allow guest reading",
                8, 132, 196, 54,
                BINDING_ADMIN_ROOM_READ_ONLY_ON,
                D1L_UI_SERVICE_ACTION_ADMIN_ROOM_READ_ONLY_ON) != NULL &&
                complete;
            complete = create_button(
                controller, sheet,
                room_read_only_off_armed ?
                    "Confirm guest off" : "Block guest reading",
                220, 132, 196, 54,
                BINDING_ADMIN_ROOM_READ_ONLY_OFF,
                D1L_UI_SERVICE_ACTION_ADMIN_ROOM_READ_ONLY_OFF) != NULL &&
                complete;
        }
    }
    char result[128];
    if (status->last_mutation == D1L_MESHCORE_ADMIN_MUTATION_NONE) {
        snprintf(result, sizeof(result), "No server change has run this session.");
    } else {
        snprintf(result, sizeof(result), "%s: %s",
                 d1l_meshcore_admin_mutation_name(status->last_mutation),
                 status->last_mutation_success ?
                     "confirmed by server" : "not confirmed");
    }
    lv_obj_t *result_panel = create_admin_panel(
        sheet, 8, status->role == D1L_MESHCORE_ADMIN_ROLE_ROOM ? 198 : 134,
        408, 54, status->last_mutation_success ? 0x84FF2E : 0x34566A);
    if (result_panel) {
        lv_obj_t *result_label = create_label(result_panel, result, 0xF4F7FB);
        position_wrap(result_label, 4, 2, 376);
        complete = result_label && complete;
    }
    complete = result_panel && complete;
    const char *notice_text = feedback && feedback[0] ? feedback :
        "Changes require a second tap and a confirmed server reply.";
    lv_obj_t *notice = create_label(
        sheet, notice_text,
        feedback_error ? 0xF87171 : (feedback && feedback[0] ?
            0x84FF2E : 0xA6B0B7));
    position_wrap(notice, 8, 264, 408);
    complete = notice && complete;
    return complete;
}

static bool render_admin_room_compact(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    const d1l_meshcore_admin_snapshot_t *status,
    const char *room_transcript, const char *feedback, bool feedback_error)
{
    bool complete = render_admin_compact_header(
        controller, sheet, "Room console", true);
    lv_obj_t *transcript = create_admin_panel(sheet, 8, 48, 408, 72, 0x34566A);
    if (transcript) {
        lv_obj_add_flag(transcript, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(transcript, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(transcript, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_t *text = create_label(
            transcript,
            room_transcript && room_transcript[0] ?
                room_transcript : "No room posts received yet.",
            0xF4F7FB);
        position_wrap(text, 4, 2, 376);
        complete = text && complete;
    }
    complete = transcript && complete;
    const bool can_post =
        (status->permissions & D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK) >=
        D1L_MESHCORE_ADMIN_PERMISSION_WRITE;
    if (!can_post) {
        lv_obj_t *readonly = create_label(
            sheet, "This guest session can read room posts but cannot send.",
            0xFBBF24);
        position_wrap(readonly, 28, 150, 368);
        complete = readonly && complete;
        return complete;
    }
    controller->admin_room_textarea = lv_textarea_create(sheet);
    if (controller->admin_room_textarea) {
        lv_obj_set_size(controller->admin_room_textarea, 300, 44);
        lv_obj_set_pos(controller->admin_room_textarea, 8, 126);
        lv_textarea_set_one_line(controller->admin_room_textarea, true);
        lv_textarea_set_max_length(
            controller->admin_room_textarea, D1L_USER_TEXT_MAX_BYTES);
        lv_textarea_set_placeholder_text(
            controller->admin_room_textarea, "Message the room");
        lv_textarea_set_text(controller->admin_room_textarea, "");
        d1l_ui_service_binding_t *focus = set_binding(
            controller, BINDING_ADMIN_ROOM_FOCUS,
            D1L_UI_SERVICE_ACTION_ADMIN_ROOM_SEND);
        if (focus) {
            lv_obj_add_event_cb(
                controller->admin_room_textarea,
                admin_room_focus_event_cb, LV_EVENT_FOCUSED, focus);
            lv_obj_add_event_cb(
                controller->admin_room_textarea,
                admin_room_focus_event_cb, LV_EVENT_CLICKED, focus);
        }
    }
    complete = controller->admin_room_textarea && complete;
    complete = create_button(
        controller, sheet, "Send", 316, 126, 100, 44,
        BINDING_ADMIN_ROOM_SEND,
        D1L_UI_SERVICE_ACTION_ADMIN_ROOM_SEND) != NULL && complete;
    if (feedback && feedback[0]) {
        lv_obj_t *notice = create_label(
            sheet, feedback, feedback_error ? 0xF87171 : 0x84FF2E);
        position_dot(notice, 8, 174, 408);
        complete = notice && complete;
    }
    complete = configure_admin_keyboard(
        controller, sheet, controller->admin_room_textarea,
        188, 106, D1L_UI_SERVICE_ACTION_ADMIN_ROOM_SEND) && complete;
    return complete;
}

static bool render_admin_terminal_compact(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    const d1l_meshcore_admin_snapshot_t *status,
    bool cli_command_armed, bool cli_secure_input,
    const char *feedback, bool feedback_error)
{
    bool complete = render_admin_compact_header(
        controller, sheet, "Server console", true);
    lv_obj_t *reply = create_admin_panel(sheet, 8, 48, 408, 48, 0x34566A);
    if (reply) {
        lv_obj_t *text = create_label(
            reply,
            status->cli_reply_valid ? status->cli_reply :
                "Command results appear here.",
            status->cli_reply_valid && !status->cli_reply_success ?
                0xF87171 : 0xF4F7FB);
        position_dot(text, 4, 4, 376);
        complete = text && complete;
    }
    complete = reply && complete;
    complete = create_button(
        controller, sheet,
        cli_secure_input ? "Secure input: On" : "Secure input: Off",
        8, 98, 176, 44,
        BINDING_ADMIN_CLI_SECURE_TOGGLE,
        D1L_UI_SERVICE_ACTION_ADMIN_CLI_SECURE_TOGGLE) != NULL && complete;
    if (feedback && feedback[0]) {
        lv_obj_t *notice = create_label(
            sheet, feedback, feedback_error ? 0xF87171 : 0x84FF2E);
        position_dot(notice, 194, 112, 222);
        complete = notice && complete;
    }
    controller->admin_cli_textarea = lv_textarea_create(sheet);
    if (controller->admin_cli_textarea) {
        lv_obj_set_size(controller->admin_cli_textarea, 300, 44);
        lv_obj_set_pos(controller->admin_cli_textarea, 8, 146);
        lv_textarea_set_one_line(controller->admin_cli_textarea, true);
        lv_textarea_set_password_mode(
            controller->admin_cli_textarea, cli_secure_input);
        lv_textarea_set_max_length(
            controller->admin_cli_textarea,
            D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES);
        lv_textarea_set_placeholder_text(
            controller->admin_cli_textarea,
            cli_command_armed ? "Tap Confirm to run" : "e.g. ver or get name");
        lv_textarea_set_text(controller->admin_cli_textarea, "");
        d1l_ui_service_binding_t *focus = set_binding(
            controller, BINDING_ADMIN_CLI_FOCUS,
            D1L_UI_SERVICE_ACTION_ADMIN_CLI_SEND);
        if (focus) {
            lv_obj_add_event_cb(
                controller->admin_cli_textarea,
                admin_cli_focus_event_cb, LV_EVENT_FOCUSED, focus);
            lv_obj_add_event_cb(
                controller->admin_cli_textarea,
                admin_cli_focus_event_cb, LV_EVENT_CLICKED, focus);
        }
    }
    complete = controller->admin_cli_textarea && complete;
    complete = create_button(
        controller, sheet,
        cli_command_armed ? "Confirm" : "Send",
        316, 146, 100, 44,
        BINDING_ADMIN_CLI_SEND,
        D1L_UI_SERVICE_ACTION_ADMIN_CLI_SEND) != NULL && complete;
    complete = configure_admin_keyboard(
        controller, sheet, controller->admin_cli_textarea,
        196, 98, D1L_UI_SERVICE_ACTION_ADMIN_CLI_SEND) && complete;
    return complete;
}

static bool render_admin_acl_compact(
    d1l_ui_service_sheets_controller_t *controller, lv_obj_t *sheet,
    bool acl_command_armed, const char *feedback, bool feedback_error)
{
    bool complete = render_admin_compact_header(
        controller, sheet, "Edit access", true);
    lv_obj_t *help = create_label(
        sheet,
        "Enter a 64-character public key, a space, then 0 remove, 1 read, 2 write, or 3 admin.",
        0xA6B0B7);
    position_wrap(help, 8, 48, 408);
    complete = help && complete;
    controller->admin_acl_textarea = lv_textarea_create(sheet);
    if (controller->admin_acl_textarea) {
        lv_obj_set_size(controller->admin_acl_textarea, 300, 44);
        lv_obj_set_pos(controller->admin_acl_textarea, 8, 100);
        lv_textarea_set_one_line(controller->admin_acl_textarea, true);
        lv_textarea_set_max_length(
            controller->admin_acl_textarea,
            D1L_MESHCORE_ADMIN_ACL_EDIT_MAX_BYTES);
        lv_textarea_set_placeholder_text(
            controller->admin_acl_textarea,
            acl_command_armed ? "Tap Confirm to apply" : "public-key permission");
        lv_textarea_set_text(controller->admin_acl_textarea, "");
        d1l_ui_service_binding_t *focus = set_binding(
            controller, BINDING_ADMIN_ACL_FOCUS,
            D1L_UI_SERVICE_ACTION_ADMIN_ACL_APPLY);
        if (focus) {
            lv_obj_add_event_cb(
                controller->admin_acl_textarea,
                admin_acl_focus_event_cb, LV_EVENT_FOCUSED, focus);
            lv_obj_add_event_cb(
                controller->admin_acl_textarea,
                admin_acl_focus_event_cb, LV_EVENT_CLICKED, focus);
        }
    }
    complete = controller->admin_acl_textarea && complete;
    complete = create_button(
        controller, sheet,
        acl_command_armed ? "Confirm" : "Apply",
        316, 100, 100, 44,
        BINDING_ADMIN_ACL_APPLY,
        D1L_UI_SERVICE_ACTION_ADMIN_ACL_APPLY) != NULL && complete;
    if (feedback && feedback[0]) {
        lv_obj_t *notice = create_label(
            sheet, feedback, feedback_error ? 0xF87171 : 0x84FF2E);
        position_dot(notice, 8, 150, 408);
        complete = notice && complete;
    }
    complete = configure_admin_keyboard(
        controller, sheet, controller->admin_acl_textarea,
        184, 110, D1L_UI_SERVICE_ACTION_ADMIN_ACL_APPLY) && complete;
    return complete;
}

bool d1l_ui_service_sheets_render_admin_compact(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_meshcore_admin_snapshot_t *status,
    const char *selected_fingerprint,
    const char *selected_name,
    d1l_ui_admin_page_t page,
    bool saved_password_available,
    bool remember_password,
    d1l_meshcore_admin_mutation_t armed_mutation,
    bool cli_command_armed,
    bool acl_command_armed,
    bool room_read_only_on_armed,
    bool room_read_only_off_armed,
    bool cli_secure_input,
    const char *feedback,
    bool feedback_error,
    const char *room_transcript,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context)
{
    lv_obj_t *sheet = controller ? controller->admin_sheet : NULL;
    if (!status || page > D1L_UI_ADMIN_PAGE_ACL ||
        !begin_render(controller, sheet, action_handler, action_context)) {
        return false;
    }
    controller->admin_scroll_valid = false;
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_scroll_to_y(sheet, 0, LV_ANIM_OFF);
    if (admin_state_pending(status->state)) {
        return render_admin_pending(
            controller, sheet, status, selected_fingerprint, selected_name);
    }
    if (status->state != D1L_MESHCORE_ADMIN_AUTHENTICATED) {
        return render_admin_login_compact(
            controller, sheet, status, selected_fingerprint, selected_name,
            saved_password_available, remember_password,
            feedback, feedback_error);
    }
    switch (page) {
    case D1L_UI_ADMIN_PAGE_STATUS:
        return render_admin_status_compact(
            controller, sheet, status, feedback, feedback_error);
    case D1L_UI_ADMIN_PAGE_TELEMETRY:
    case D1L_UI_ADMIN_PAGE_NEIGHBOURS:
    case D1L_UI_ADMIN_PAGE_ACCESS:
        return render_admin_data_compact(
            controller, sheet, status, page, feedback, feedback_error);
    case D1L_UI_ADMIN_PAGE_TOOLS:
        return render_admin_tools_compact(
            controller, sheet, status, armed_mutation,
            room_read_only_on_armed, room_read_only_off_armed,
            feedback, feedback_error);
    case D1L_UI_ADMIN_PAGE_ROOM:
        return render_admin_room_compact(
            controller, sheet, status, room_transcript,
            feedback, feedback_error);
    case D1L_UI_ADMIN_PAGE_TERMINAL:
        return render_admin_terminal_compact(
            controller, sheet, status, cli_command_armed,
            cli_secure_input, feedback, feedback_error);
    case D1L_UI_ADMIN_PAGE_ACL:
        return render_admin_acl_compact(
            controller, sheet, acl_command_armed,
            feedback, feedback_error);
    case D1L_UI_ADMIN_PAGE_LOGIN:
    case D1L_UI_ADMIN_PAGE_HUB:
    default:
        return render_admin_hub_compact(
            controller, sheet, status,
            selected_fingerprint, selected_name,
            feedback, feedback_error);
    }
}

bool d1l_ui_service_sheets_render_admin(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_meshcore_admin_snapshot_t *status,
    const char *selected_fingerprint,
    d1l_meshcore_admin_mutation_t armed_mutation,
    bool cli_command_armed,
    bool acl_command_armed,
    bool room_read_only_on_armed,
    bool room_read_only_off_armed,
    bool cli_secure_input,
    const char *room_transcript,
    d1l_ui_service_action_handler_t action_handler,
    void *action_context)
{
    lv_obj_t *sheet = controller ? controller->admin_sheet : NULL;
    if (!status ||
        !begin_render(controller, sheet, action_handler, action_context)) {
        return false;
    }
    bool complete = render_header(
        controller, sheet, "Server Admin", BINDING_CLOSE_ADMIN,
        D1L_UI_SERVICE_ACTION_CLOSE_ADMIN);
    char line[160];
    snprintf(line, sizeof(line), "State %s  role %s  permissions %u",
             admin_state_name(status->state), admin_role_name(status->role),
             (unsigned)status->permissions);
    lv_obj_t *state = create_label(
        sheet, line,
        status->state == D1L_MESHCORE_ADMIN_AUTHENTICATED ? 0x20D9ED :
                                                            0xFBBF24);
    position_dot(state, 8, 54, 408);
    complete = state && complete;
    const char *display_fingerprint = status->fingerprint[0] ?
        status->fingerprint :
        (selected_fingerprint && selected_fingerprint[0] ?
             selected_fingerprint : "-");
    snprintf(line, sizeof(line), "Server %.16s  firmware level %u",
             display_fingerprint,
             (unsigned)status->firmware_level);
    lv_obj_t *server = create_label(sheet, line, 0xF4F7FB);
    position_dot(server, 8, 84, 408);
    complete = server && complete;
    char status_details[512] = {0};
    if (status->status_valid) {
        const int snr_magnitude =
            status->status.last_snr_quarter_db < 0 ?
                -(int)status->status.last_snr_quarter_db :
                (int)status->status.last_snr_quarter_db;
        if (status->role == D1L_MESHCORE_ADMIN_ROLE_REPEATER) {
            snprintf(
                status_details, sizeof(status_details),
                "Battery %umV  queue %u  noise %ddBm  RSSI %ddBm  SNR %s%d.%02ddB\n"
                "Packets RX %lu  TX %lu  uptime %lus  errors 0x%04x\n"
                "Sent flood %lu  direct %lu  received flood %lu  direct %lu\n"
                "Airtime TX %lus  RX %lus  receive errors %lu\n"
                "Duplicates direct %u  flood %u",
                (unsigned)status->status.battery_millivolts,
                (unsigned)status->status.tx_queue_length,
                (int)status->status.noise_floor_dbm,
                (int)status->status.last_rssi_dbm,
                status->status.last_snr_quarter_db < 0 ? "-" : "",
                snr_magnitude / 4, (snr_magnitude % 4) * 25,
                (unsigned long)status->status.packets_received,
                (unsigned long)status->status.packets_sent,
                (unsigned long)status->status.uptime_seconds,
                (unsigned)status->status.error_flags,
                (unsigned long)status->status.sent_flood,
                (unsigned long)status->status.sent_direct,
                (unsigned long)status->status.received_flood,
                (unsigned long)status->status.received_direct,
                (unsigned long)status->status.tx_air_time_seconds,
                (unsigned long)status->status.rx_air_time_seconds,
                (unsigned long)status->status.receive_errors,
                (unsigned)status->status.direct_duplicates,
                (unsigned)status->status.flood_duplicates);
        } else {
            snprintf(
                status_details, sizeof(status_details),
                "Battery %umV  queue %u  noise %ddBm  RSSI %ddBm  SNR %s%d.%02ddB\n"
                "Packets RX %lu  TX %lu  uptime %lus  errors 0x%04x\n"
                "Sent flood %lu  direct %lu  received flood %lu  direct %lu\n"
                "TX airtime %lus  room posts created %u  pushed %u\n"
                "Duplicates direct %u  flood %u",
                (unsigned)status->status.battery_millivolts,
                (unsigned)status->status.tx_queue_length,
                (int)status->status.noise_floor_dbm,
                (int)status->status.last_rssi_dbm,
                status->status.last_snr_quarter_db < 0 ? "-" : "",
                snr_magnitude / 4, (snr_magnitude % 4) * 25,
                (unsigned long)status->status.packets_received,
                (unsigned long)status->status.packets_sent,
                (unsigned long)status->status.uptime_seconds,
                (unsigned)status->status.error_flags,
                (unsigned long)status->status.sent_flood,
                (unsigned long)status->status.sent_direct,
                (unsigned long)status->status.received_flood,
                (unsigned long)status->status.received_direct,
                (unsigned long)status->status.tx_air_time_seconds,
                (unsigned)status->status.posts_created,
                (unsigned)status->status.posts_pushed,
                (unsigned)status->status.direct_duplicates,
                (unsigned)status->status.flood_duplicates);
        }
    } else {
        snprintf(
            status_details, sizeof(status_details),
            "No authenticated status response yet");
    }
    lv_obj_t *metrics =
        create_label(sheet, status_details, 0xA6B0B7);
    position_wrap(metrics, 8, 114, 408);
    int32_t metrics_height = 24;
    if (metrics) {
        lv_obj_update_layout(metrics);
        if (lv_obj_get_height(metrics) > metrics_height) {
            metrics_height = lv_obj_get_height(metrics);
        }
    }
    complete = metrics && complete;
    const int32_t mutation_y = 114 + metrics_height + 8;
    snprintf(
        line, sizeof(line), "Last change %s  result %s",
        d1l_meshcore_admin_mutation_name(status->last_mutation),
        status->last_mutation == D1L_MESHCORE_ADMIN_MUTATION_NONE ?
            "not_run" :
            (status->last_mutation_success ? "confirmed" : "not_confirmed"));
    lv_obj_t *mutation = create_label(sheet, line, 0xA6B0B7);
    position_dot(mutation, 8, mutation_y, 408);
    complete = mutation && complete;
    const int32_t policy_y = mutation_y + 26;
    lv_obj_t *policy = create_label(
        sheet,
        "Compatible verified room/repeater only. Credentials stay volatile and redacted. Login is sent from this device using a fresh saved route when available, with flood fallback.",
        0x4D7FFF);
    position_wrap(policy, 8, policy_y, 408);
    int32_t policy_height = 56;
    if (policy) {
        lv_obj_update_layout(policy);
        if (lv_obj_get_height(policy) > policy_height) {
            policy_height = lv_obj_get_height(policy);
        }
    }
    complete = policy && complete;

    if (status->state == D1L_MESHCORE_ADMIN_AUTHENTICATED) {
        const uint8_t permission_role =
            status->permissions & D1L_MESHCORE_ADMIN_PERMISSION_ROLE_MASK;
        const bool protocol_level_valid =
            (status->role == D1L_MESHCORE_ADMIN_ROLE_REPEATER &&
             status->firmware_level == 2U) ||
            (status->role == D1L_MESHCORE_ADMIN_ROLE_ROOM &&
             status->firmware_level == 1U);
        const bool can_mutate =
            permission_role == D1L_MESHCORE_ADMIN_PERMISSION_ADMIN &&
            protocol_level_valid;
        const bool room_session =
            status->role == D1L_MESHCORE_ADMIN_ROLE_ROOM &&
            protocol_level_valid;
        const bool can_post_room =
            room_session &&
            permission_role >= D1L_MESHCORE_ADMIN_PERMISSION_WRITE;
        bool needs_keyboard = false;
        lv_obj_t *initial_keyboard_target = NULL;
        int32_t keyboard_y = 0;
        const int32_t mutation_buttons_y =
            policy_y + policy_height + 14;
        const int32_t refresh_buttons_y =
            mutation_buttons_y + (can_mutate ? 54 : 0);
        if (can_mutate) {
            complete = create_button(
                controller, sheet,
                armed_mutation ==
                        D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS ?
                    "Confirm Clear Stats" : "Clear Remote Stats",
                8, mutation_buttons_y, 190, 44,
                BINDING_ADMIN_CLEAR_STATS,
                D1L_UI_SERVICE_ACTION_ADMIN_CLEAR_STATS) != NULL && complete;
            complete = create_button(
                controller, sheet,
                armed_mutation ==
                        D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP ?
                    "Confirm Zero-Hop" : "Send Zero-Hop Advert",
                210, mutation_buttons_y, 190, 44,
                BINDING_ADMIN_ADVERTISE_ZERO_HOP,
                D1L_UI_SERVICE_ACTION_ADMIN_ADVERTISE_ZERO_HOP) != NULL &&
                complete;
        }
        complete = create_button(
            controller, sheet, "Refresh Status", 8,
            refresh_buttons_y, 152, 44,
            BINDING_ADMIN_REFRESH,
            D1L_UI_SERVICE_ACTION_ADMIN_REFRESH) != NULL && complete;
        complete = create_button(
            controller, sheet, "Logout", 172,
            refresh_buttons_y, 100, 44,
            BINDING_ADMIN_LOGOUT,
            D1L_UI_SERVICE_ACTION_ADMIN_LOGOUT) != NULL && complete;

        const int32_t query_title_y = refresh_buttons_y + 58;
        const int32_t query_buttons_y = query_title_y + 28;
        const int32_t query_result_y = query_buttons_y + 60;
        lv_obj_t *query_title = create_label(
            sheet, "Authenticated server data", 0x20D9ED);
        position_dot(query_title, 8, query_title_y, 408);
        complete = query_title && complete;
        complete = create_button(
            controller, sheet, "Telemetry", 8,
            query_buttons_y, 124, 44,
            BINDING_ADMIN_TELEMETRY,
            D1L_UI_SERVICE_ACTION_ADMIN_TELEMETRY) != NULL && complete;
        if (status->role == D1L_MESHCORE_ADMIN_ROLE_REPEATER) {
            complete = create_button(
                controller, sheet, "Neighbours", 142,
                query_buttons_y, 124, 44,
                BINDING_ADMIN_NEIGHBOURS,
                D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS) != NULL && complete;
        }
        if (can_mutate) {
            complete = create_button(
                controller, sheet, "Access List", 276,
                query_buttons_y, 124, 44,
                BINDING_ADMIN_ACCESS_LIST,
                D1L_UI_SERVICE_ACTION_ADMIN_ACCESS_LIST) != NULL && complete;
        }

        int32_t content_y = query_result_y + 48;
        if (status->query_result.valid) {
            lv_obj_t *query_result = create_label(
                sheet, status->query_result.text,
                status->query_result.truncated ? 0xFBBF24 : 0xF4F7FB);
            position_wrap(query_result, 8, query_result_y, 408);
            int32_t result_height = 48;
            if (query_result) {
                lv_obj_update_layout(query_result);
                const lv_coord_t measured_height =
                    lv_obj_get_height(query_result);
                if (measured_height > result_height) {
                    result_height = measured_height;
                }
            }
            complete = query_result && complete;
            content_y = query_result_y + result_height + 14;
            const uint32_t next_offset =
                (uint32_t)status->query_result.offset +
                (uint32_t)status->query_result.count;
            if (status->query_result.kind ==
                    D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS &&
                status->query_result.count > 0U &&
                next_offset < status->query_result.total) {
                complete = create_button(
                    controller, sheet, "Next Neighbours",
                    8, content_y, 180, 44,
                    BINDING_ADMIN_NEIGHBOURS_NEXT,
                    D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS_NEXT) != NULL &&
                    complete;
                content_y += 58;
            }
        } else {
            lv_obj_t *query_help = create_label(
                sheet,
                "Read telemetry for any authenticated session. Repeater neighbours are paged. Access-list reads require admin permission.",
                0x4D7FFF);
            position_wrap(query_help, 8, query_result_y, 408);
            complete = query_help && complete;
        }

        int32_t cli_y = content_y;
        if (room_session) {
            int32_t room_y = content_y;
            if (can_mutate) {
                lv_obj_t *room_access_title = create_label(
                    sheet, "Room guest access", 0x20D9ED);
                position_dot(room_access_title, 8, room_y, 408);
                complete = room_access_title && complete;
                complete = create_button(
                    controller, sheet,
                    room_read_only_on_armed ?
                        "Confirm Guest On" : "Enable Guest Read",
                    8, room_y + 28, 190, 44,
                    BINDING_ADMIN_ROOM_READ_ONLY_ON,
                    D1L_UI_SERVICE_ACTION_ADMIN_ROOM_READ_ONLY_ON) != NULL &&
                    complete;
                complete = create_button(
                    controller, sheet,
                    room_read_only_off_armed ?
                        "Confirm Guest Off" : "Disable Guest Read",
                    210, room_y + 28, 190, 44,
                    BINDING_ADMIN_ROOM_READ_ONLY_OFF,
                    D1L_UI_SERVICE_ACTION_ADMIN_ROOM_READ_ONLY_OFF) != NULL &&
                    complete;
                lv_obj_t *room_access_help = create_label(
                    sheet,
                    "Controls allow.read.only. Each change requires a second tap and a peer-confirmed response.",
                    0x4D7FFF);
                position_wrap(room_access_help, 8, room_y + 82, 408);
                complete = room_access_help && complete;
                room_y += 132;
            }
            lv_obj_t *room_title = create_label(
                sheet, "Live room console", 0x20D9ED);
            position_dot(room_title, 8, room_y, 408);
            complete = room_title && complete;

            lv_obj_t *transcript = create_label(
                sheet,
                room_transcript && room_transcript[0] ?
                    room_transcript : "No room posts received yet.",
                0xF4F7FB);
            position_wrap(transcript, 8, room_y + 26, 408);
            if (transcript) {
                lv_obj_set_height(transcript, 132);
            }
            complete = transcript && complete;

            if (can_post_room) {
                lv_obj_t *post_label = create_label(
                    sheet, "Room message", 0xF4F7FB);
                position_dot(post_label, 8, room_y + 166, 408);
                complete = post_label && complete;

                controller->admin_room_textarea =
                    lv_textarea_create(sheet);
                if (controller->admin_room_textarea) {
                    lv_obj_set_size(
                        controller->admin_room_textarea, 408, 40);
                    lv_obj_set_pos(
                        controller->admin_room_textarea, 8,
                        room_y + 190);
                    lv_textarea_set_one_line(
                        controller->admin_room_textarea, true);
                    lv_textarea_set_max_length(
                        controller->admin_room_textarea,
                        D1L_USER_TEXT_MAX_BYTES);
                    lv_textarea_set_placeholder_text(
                        controller->admin_room_textarea,
                        "Type a room post");
                    lv_textarea_set_text(
                        controller->admin_room_textarea, "");
                    lv_obj_set_style_radius(
                        controller->admin_room_textarea, 8, 0);
                    lv_obj_set_style_bg_color(
                        controller->admin_room_textarea,
                        lv_color_hex(0x17191A), 0);
                    lv_obj_set_style_border_color(
                        controller->admin_room_textarea,
                        lv_color_hex(0x33404A), 0);
                    lv_obj_set_style_text_color(
                        controller->admin_room_textarea,
                        lv_color_hex(0xF4F7FB), 0);
                    d1l_ui_service_binding_t *focus_binding =
                        set_binding(
                            controller, BINDING_ADMIN_ROOM_FOCUS,
                            D1L_UI_SERVICE_ACTION_ADMIN_ROOM_SEND);
                    if (focus_binding) {
                        lv_obj_add_event_cb(
                            controller->admin_room_textarea,
                            admin_room_focus_event_cb,
                            LV_EVENT_FOCUSED, focus_binding);
                        lv_obj_add_event_cb(
                            controller->admin_room_textarea,
                            admin_room_focus_event_cb,
                            LV_EVENT_CLICKED, focus_binding);
                    }
                }
                complete = controller->admin_room_textarea && complete;
                complete = create_button(
                    controller, sheet, "Send Room Post",
                    8, room_y + 240, 184, 44,
                    BINDING_ADMIN_ROOM_SEND,
                    D1L_UI_SERVICE_ACTION_ADMIN_ROOM_SEND) != NULL &&
                    complete;
                lv_obj_t *room_help = create_label(
                    sheet,
                    "Posts use the authenticated room session. Guest permission is read-only.",
                    0x4D7FFF);
                position_wrap(room_help, 8, room_y + 294, 408);
                complete = room_help && complete;
                needs_keyboard = true;
                initial_keyboard_target =
                    controller->admin_room_textarea;
                keyboard_y = room_y + 368;
                cli_y = room_y + 340;
            } else {
                lv_obj_t *read_only = create_label(
                    sheet,
                    "Guest permission: live room posts are read-only.",
                    0xFBBF24);
                position_wrap(read_only, 8, room_y + 166, 408);
                complete = read_only && complete;
                cli_y = room_y + 216;
            }
        }

        if (can_mutate) {
            lv_obj_t *acl_title = create_label(
                sheet, "Access-list editor", 0x20D9ED);
            position_dot(acl_title, 8, cli_y, 408);
            complete = acl_title && complete;
            lv_obj_t *acl_help = create_label(
                sheet,
                "Enter the full 64-hex public key and permission: 0 remove, 1 read, 2 write, 3 admin.",
                0x4D7FFF);
            position_wrap(acl_help, 8, cli_y + 26, 408);
            complete = acl_help && complete;

            controller->admin_acl_textarea = lv_textarea_create(sheet);
            if (controller->admin_acl_textarea) {
                lv_obj_set_size(controller->admin_acl_textarea, 408, 40);
                lv_obj_set_pos(
                    controller->admin_acl_textarea, 8, cli_y + 78);
                lv_textarea_set_one_line(
                    controller->admin_acl_textarea, true);
                lv_textarea_set_max_length(
                    controller->admin_acl_textarea,
                    D1L_MESHCORE_ADMIN_ACL_EDIT_MAX_BYTES);
                lv_textarea_set_placeholder_text(
                    controller->admin_acl_textarea,
                    acl_command_armed ?
                        "ACL change armed; tap Confirm ACL" :
                        "64-hex-public-key 0|1|2|3");
                lv_textarea_set_text(controller->admin_acl_textarea, "");
                lv_obj_set_style_radius(
                    controller->admin_acl_textarea, 8, 0);
                lv_obj_set_style_bg_color(
                    controller->admin_acl_textarea,
                    lv_color_hex(0x17191A), 0);
                lv_obj_set_style_border_color(
                    controller->admin_acl_textarea,
                    lv_color_hex(0x33404A), 0);
                lv_obj_set_style_text_color(
                    controller->admin_acl_textarea,
                    lv_color_hex(0xF4F7FB), 0);
                d1l_ui_service_binding_t *focus_binding = set_binding(
                    controller, BINDING_ADMIN_ACL_FOCUS,
                    D1L_UI_SERVICE_ACTION_ADMIN_ACL_APPLY);
                if (focus_binding) {
                    lv_obj_add_event_cb(
                        controller->admin_acl_textarea,
                        admin_acl_focus_event_cb,
                        LV_EVENT_FOCUSED, focus_binding);
                    lv_obj_add_event_cb(
                        controller->admin_acl_textarea,
                        admin_acl_focus_event_cb,
                        LV_EVENT_CLICKED, focus_binding);
                }
            }
            complete = controller->admin_acl_textarea && complete;
            complete = create_button(
                controller, sheet,
                acl_command_armed ? "Confirm ACL" : "Apply ACL Change",
                8, cli_y + 128, 184, 44,
                BINDING_ADMIN_ACL_APPLY,
                D1L_UI_SERVICE_ACTION_ADMIN_ACL_APPLY) != NULL && complete;
            needs_keyboard = true;
            if (!initial_keyboard_target) {
                initial_keyboard_target = controller->admin_acl_textarea;
            }
            cli_y += 190;

            lv_obj_t *cli_title = create_label(
                sheet, "Authenticated server command", 0x20D9ED);
            position_dot(cli_title, 8, cli_y, 408);
            complete = cli_title && complete;

            const char *reply_text = status->cli_reply_valid ?
                status->cli_reply :
                "No command response yet.";
            lv_obj_t *reply = create_label(
                sheet, reply_text,
                status->cli_reply_valid ?
                    (status->cli_reply_success ? 0xF4F7FB : 0xFCA5A5) :
                    0xA6B0B7);
            position_wrap(reply, 8, cli_y + 26, 408);
            if (reply) {
                lv_obj_set_height(reply, 104);
            }
            complete = reply && complete;

            lv_obj_t *command_label = create_label(
                sheet,
                cli_secure_input ?
                    "Command (secure masked input)" :
                    "Command (visible input)",
                cli_secure_input ? 0xFBBF24 : 0xF4F7FB);
            position_dot(command_label, 8, cli_y + 142, 408);
            complete = command_label && complete;

            controller->admin_cli_textarea = lv_textarea_create(sheet);
            if (controller->admin_cli_textarea) {
                lv_obj_set_size(controller->admin_cli_textarea, 408, 40);
                lv_obj_set_pos(
                    controller->admin_cli_textarea, 8, cli_y + 166);
                lv_textarea_set_one_line(
                    controller->admin_cli_textarea, true);
                lv_textarea_set_password_mode(
                    controller->admin_cli_textarea, cli_secure_input);
                lv_textarea_set_max_length(
                    controller->admin_cli_textarea,
                    D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES);
                lv_textarea_set_placeholder_text(
                    controller->admin_cli_textarea,
                    cli_command_armed ?
                        "Command armed; tap Confirm Command" :
                        (cli_secure_input ?
                            "Type a password/key command" :
                            "e.g. ver, get name, region"));
                lv_textarea_set_text(controller->admin_cli_textarea, "");
                lv_obj_set_style_radius(
                    controller->admin_cli_textarea, 8, 0);
                lv_obj_set_style_bg_color(
                    controller->admin_cli_textarea,
                    lv_color_hex(0x17191A), 0);
                lv_obj_set_style_border_color(
                    controller->admin_cli_textarea,
                    lv_color_hex(cli_secure_input ? 0xB45309 : 0x33404A),
                    0);
                lv_obj_set_style_text_color(
                    controller->admin_cli_textarea,
                    lv_color_hex(0xF4F7FB), 0);
                d1l_ui_service_binding_t *focus_binding = set_binding(
                    controller, BINDING_ADMIN_CLI_FOCUS,
                    D1L_UI_SERVICE_ACTION_ADMIN_CLI_SEND);
                if (focus_binding) {
                    lv_obj_add_event_cb(
                        controller->admin_cli_textarea,
                        admin_cli_focus_event_cb,
                        LV_EVENT_FOCUSED, focus_binding);
                    lv_obj_add_event_cb(
                        controller->admin_cli_textarea,
                        admin_cli_focus_event_cb,
                        LV_EVENT_CLICKED, focus_binding);
                }
            }
            complete = controller->admin_cli_textarea && complete;
            complete = create_button(
                controller, sheet,
                cli_command_armed ? "Confirm Command" : "Send Command",
                8, cli_y + 218, 184, 44, BINDING_ADMIN_CLI_SEND,
                D1L_UI_SERVICE_ACTION_ADMIN_CLI_SEND) != NULL && complete;
            complete = create_button(
                controller, sheet,
                cli_secure_input ? "Secure Input: On" : "Secure Input: Off",
                204, cli_y + 218, 196, 44,
                BINDING_ADMIN_CLI_SECURE_TOGGLE,
                D1L_UI_SERVICE_ACTION_ADMIN_CLI_SECURE_TOGGLE) != NULL &&
                complete;

            lv_obj_t *cli_help = create_label(
                sheet,
                "Read-only commands send immediately. Only documented role-compatible commands are accepted; unsupported, serial-only, OTA, reboot and power commands fail closed. Changes require a second tap. Use Secure Input for passwords, secrets or private keys. Responses are bounded; sensitive responses are hidden.",
                0x4D7FFF);
            position_wrap(cli_help, 8, cli_y + 274, 408);
            complete = cli_help && complete;

            needs_keyboard = true;
            if (!initial_keyboard_target) {
                initial_keyboard_target =
                    controller->admin_cli_textarea;
            }
            keyboard_y = cli_y + 368;
        } else if (!room_session) {
            lv_obj_t *guest = create_label(
                sheet,
                "Read-only session: status and permitted server data are available; server commands require admin permission.",
                0xFBBF24);
            position_wrap(guest, 8, content_y, 408);
            complete = guest && complete;
        }

        if (needs_keyboard) {
            controller->admin_keyboard = lv_keyboard_create(sheet);
            if (controller->admin_keyboard) {
                d1l_ui_keyboard_configure_input(
                    controller->admin_keyboard,
                    initial_keyboard_target,
                    8, keyboard_y, 408, 106);
                d1l_ui_service_binding_t *keyboard_binding = set_binding(
                    controller, BINDING_ADMIN_KEYBOARD,
                    initial_keyboard_target ==
                            controller->admin_room_textarea ?
                        D1L_UI_SERVICE_ACTION_ADMIN_ROOM_SEND :
                        D1L_UI_SERVICE_ACTION_ADMIN_CLI_SEND);
                if (keyboard_binding) {
                    lv_obj_add_event_cb(
                        controller->admin_keyboard,
                        admin_keyboard_event_cb,
                        LV_EVENT_READY, keyboard_binding);
                    lv_obj_add_event_cb(
                        controller->admin_keyboard,
                        admin_keyboard_event_cb,
                        LV_EVENT_CANCEL, keyboard_binding);
                }
            }
            complete = controller->admin_keyboard && complete;
        }
    } else if (status->state == D1L_MESHCORE_ADMIN_LOGIN_PENDING ||
               status->state == D1L_MESHCORE_ADMIN_STATUS_PENDING ||
               status->state == D1L_MESHCORE_ADMIN_MUTATION_PENDING ||
               status->state == D1L_MESHCORE_ADMIN_CLI_PENDING ||
               status->state == D1L_MESHCORE_ADMIN_QUERY_PENDING) {
        complete = create_button(
            controller, sheet, "Logout / Cancel", 8, 258, 160, 44,
            BINDING_ADMIN_LOGOUT,
            D1L_UI_SERVICE_ACTION_ADMIN_LOGOUT) != NULL && complete;
    } else {
        if (selected_fingerprint && selected_fingerprint[0]) {
            lv_obj_t *password_label = create_label(
                sheet, "Password (empty allowed by peer)", 0x20D9ED);
            position_dot(password_label, 8, 232, 408);
            complete = password_label && complete;

            controller->admin_password_textarea =
                lv_textarea_create(sheet);
            if (controller->admin_password_textarea) {
                lv_obj_set_size(controller->admin_password_textarea, 408, 36);
                lv_obj_set_pos(controller->admin_password_textarea, 8, 254);
                lv_textarea_set_one_line(
                    controller->admin_password_textarea, true);
                lv_textarea_set_password_mode(
                    controller->admin_password_textarea, true);
                lv_textarea_set_max_length(
                    controller->admin_password_textarea,
                    D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES);
                lv_textarea_set_placeholder_text(
                    controller->admin_password_textarea,
                    "Repeater or room password");
                lv_textarea_set_text(
                    controller->admin_password_textarea, "");
                lv_obj_set_style_radius(
                    controller->admin_password_textarea, 8, 0);
                lv_obj_set_style_bg_color(
                    controller->admin_password_textarea,
                    lv_color_hex(0x17191A), 0);
                lv_obj_set_style_border_color(
                    controller->admin_password_textarea,
                    lv_color_hex(0x33404A), 0);
                lv_obj_set_style_text_color(
                    controller->admin_password_textarea,
                    lv_color_hex(0xF4F7FB), 0);
                d1l_ui_service_binding_t *focus_binding = set_binding(
                    controller, BINDING_ADMIN_PASSWORD_FOCUS,
                    D1L_UI_SERVICE_ACTION_ADMIN_LOGIN);
                if (focus_binding) {
                    lv_obj_add_event_cb(
                        controller->admin_password_textarea,
                        admin_password_focus_event_cb,
                        LV_EVENT_FOCUSED, focus_binding);
                    lv_obj_add_event_cb(
                        controller->admin_password_textarea,
                        admin_password_focus_event_cb,
                        LV_EVENT_CLICKED, focus_binding);
                }
            }
            complete = controller->admin_password_textarea && complete;
            complete = create_button(
                controller, sheet, "Login", 8, 298, 120, 44,
                BINDING_ADMIN_LOGIN,
                D1L_UI_SERVICE_ACTION_ADMIN_LOGIN) != NULL && complete;

            controller->admin_keyboard = lv_keyboard_create(sheet);
            if (controller->admin_keyboard) {
                d1l_ui_keyboard_configure_input(
                    controller->admin_keyboard,
                    controller->admin_password_textarea,
                    8, 350, 408, 82);
                d1l_ui_service_binding_t *keyboard_binding = set_binding(
                    controller, BINDING_ADMIN_KEYBOARD,
                    D1L_UI_SERVICE_ACTION_ADMIN_LOGIN);
                if (keyboard_binding) {
                    lv_obj_add_event_cb(
                        controller->admin_keyboard,
                        admin_keyboard_event_cb,
                        LV_EVENT_READY, keyboard_binding);
                    lv_obj_add_event_cb(
                        controller->admin_keyboard,
                        admin_keyboard_event_cb,
                        LV_EVENT_CANCEL, keyboard_binding);
                }
            }
            complete = controller->admin_keyboard && complete;
        } else {
            lv_obj_t *login = create_label(
                sheet,
                "Select a repeater or room in Nodes, open its details, then tap Admin.",
                0xFBBF24);
            position_wrap(login, 8, 246, 408);
            complete = login && complete;
        }
    }
    finish_admin_render(controller, sheet);
    return complete;
}

bool d1l_ui_service_sheets_take_admin_password(
    d1l_ui_service_sheets_controller_t *controller,
    char *out_password,
    size_t out_password_size)
{
    if (!controller || !out_password ||
        out_password_size < D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U ||
        !controller->admin_password_textarea ||
        !lv_obj_is_valid(controller->admin_password_textarea)) {
        return false;
    }
    const char *password =
        lv_textarea_get_text(controller->admin_password_textarea);
    const size_t password_len = password ?
        strnlen(password, D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U) :
        D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U;
    if (password_len > D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES) {
        clear_admin_sensitive_input(controller);
        return false;
    }
    memcpy(out_password, password, password_len);
    out_password[password_len] = '\0';
    clear_admin_sensitive_input(controller);
    return true;
}

bool d1l_ui_service_sheets_take_admin_cli(
    d1l_ui_service_sheets_controller_t *controller,
    char *out_command,
    size_t out_command_size)
{
    if (!controller || !out_command ||
        out_command_size < D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U ||
        !controller->admin_cli_textarea ||
        !lv_obj_is_valid(controller->admin_cli_textarea)) {
        return false;
    }
    const char *command =
        lv_textarea_get_text(controller->admin_cli_textarea);
    const size_t command_len = command ?
        strnlen(command, D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U) :
        D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U;
    if (command_len == 0U ||
        command_len > D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES) {
        clear_admin_sensitive_input(controller);
        return false;
    }
    memcpy(out_command, command, command_len);
    out_command[command_len] = '\0';
    clear_admin_sensitive_input(controller);
    return true;
}

bool d1l_ui_service_sheets_take_admin_acl(
    d1l_ui_service_sheets_controller_t *controller,
    char *out_acl_edit,
    size_t out_acl_edit_size)
{
    if (!controller || !out_acl_edit ||
        out_acl_edit_size < D1L_MESHCORE_ADMIN_ACL_EDIT_MAX_BYTES + 1U ||
        !controller->admin_acl_textarea ||
        !lv_obj_is_valid(controller->admin_acl_textarea)) {
        return false;
    }
    const char *edit =
        lv_textarea_get_text(controller->admin_acl_textarea);
    const size_t edit_len = edit ?
        strnlen(edit, D1L_MESHCORE_ADMIN_ACL_EDIT_MAX_BYTES + 1U) :
        D1L_MESHCORE_ADMIN_ACL_EDIT_MAX_BYTES + 1U;
    if (edit_len == 0U ||
        edit_len > D1L_MESHCORE_ADMIN_ACL_EDIT_MAX_BYTES) {
        clear_admin_sensitive_input(controller);
        return false;
    }
    memcpy(out_acl_edit, edit, edit_len);
    out_acl_edit[edit_len] = '\0';
    clear_admin_sensitive_input(controller);
    return true;
}

bool d1l_ui_service_sheets_take_admin_room_post(
    d1l_ui_service_sheets_controller_t *controller,
    char *out_text,
    size_t out_text_size)
{
    if (!controller || !out_text ||
        out_text_size < D1L_USER_TEXT_MAX_BYTES + 1U ||
        !controller->admin_room_textarea ||
        !lv_obj_is_valid(controller->admin_room_textarea)) {
        return false;
    }
    const char *text =
        lv_textarea_get_text(controller->admin_room_textarea);
    const d1l_user_text_info_t text_info = d1l_user_text_validate(text);
    if (text_info.result != D1L_USER_TEXT_OK ||
        text_info.byte_count + 1U > out_text_size) {
        clear_admin_sensitive_input(controller);
        return false;
    }
    memcpy(out_text, text, text_info.byte_count);
    out_text[text_info.byte_count] = '\0';
    clear_admin_sensitive_input(controller);
    return true;
}

static bool admin_textarea_has_text(lv_obj_t *textarea)
{
    if (!textarea || !lv_obj_is_valid(textarea)) {
        return false;
    }
    const char *text = lv_textarea_get_text(textarea);
    return text && text[0] != '\0';
}

bool d1l_ui_service_sheets_admin_edit_has_text(
    const d1l_ui_service_sheets_controller_t *controller)
{
    return controller &&
        (admin_textarea_has_text(controller->admin_password_textarea) ||
         admin_textarea_has_text(controller->admin_room_textarea) ||
         admin_textarea_has_text(controller->admin_acl_textarea) ||
         admin_textarea_has_text(controller->admin_cli_textarea));
}

void d1l_ui_service_sheets_hide_all(
    d1l_ui_service_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
    clear_admin_sensitive_input(controller);
    d1l_ui_keyboard_clear_textarea(controller->observer_keyboard);
    deactivate_actions(controller);
    lv_obj_t *sheets[] = {
        controller->terminal_sheet,
        controller->observer_sheet,
        controller->update_sheet,
        controller->notifications_sheet,
        controller->admin_sheet,
    };
    for (size_t i = 0U; i < sizeof(sheets) / sizeof(sheets[0]); ++i) {
        if (sheets[i] && lv_obj_is_valid(sheets[i])) {
            d1l_ui_modal_hide(sheets[i]);
        }
    }
}

static lv_obj_t *valid_sheet(lv_obj_t *sheet)
{
    return sheet && lv_obj_is_valid(sheet) ? sheet : NULL;
}

lv_obj_t *d1l_ui_service_sheets_terminal(
    const d1l_ui_service_sheets_controller_t *controller)
{
    return valid_sheet(controller ? controller->terminal_sheet : NULL);
}

lv_obj_t *d1l_ui_service_sheets_observer(
    const d1l_ui_service_sheets_controller_t *controller)
{
    return valid_sheet(controller ? controller->observer_sheet : NULL);
}

lv_obj_t *d1l_ui_service_sheets_update(
    const d1l_ui_service_sheets_controller_t *controller)
{
    return valid_sheet(controller ? controller->update_sheet : NULL);
}

lv_obj_t *d1l_ui_service_sheets_notifications(
    const d1l_ui_service_sheets_controller_t *controller)
{
    return valid_sheet(controller ? controller->notifications_sheet : NULL);
}

lv_obj_t *d1l_ui_service_sheets_admin(
    const d1l_ui_service_sheets_controller_t *controller)
{
    return valid_sheet(controller ? controller->admin_sheet : NULL);
}
