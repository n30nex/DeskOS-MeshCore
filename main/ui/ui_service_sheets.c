#include "ui_service_sheets.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "lvgl.h"
#include "ui_modal.h"

enum {
    BINDING_CLOSE_TERMINAL = 0,
    BINDING_TERMINAL_LEVEL,
    BINDING_TERMINAL_CLEAR,
    BINDING_CLOSE_OBSERVER,
    BINDING_OBSERVER_TOGGLE,
    BINDING_CLOSE_UPDATE,
    BINDING_UPDATE_INSTALL,
    BINDING_UPDATE_CANCEL,
    BINDING_UPDATE_REBOOT,
    BINDING_CLOSE_NOTIFICATIONS,
    BINDING_NOTIFICATIONS_MODE,
    BINDING_OPEN_MESSAGES,
    BINDING_CLOSE_ADMIN,
    BINDING_ADMIN_REFRESH,
    BINDING_ADMIN_CLEAR_STATS,
    BINDING_ADMIN_ADVERTISE_ZERO_HOP,
    BINDING_ADMIN_LOGOUT,
};

_Static_assert(sizeof(d1l_ui_service_sheets_controller_t) <=
                   D1L_UI_SERVICE_SHEETS_CONTROLLER_MAX_BYTES,
               "Service sheets controller exceeded its size budget");

static bool action_valid(d1l_ui_service_action_t action)
{
    return action > D1L_UI_SERVICE_ACTION_NONE &&
           action <= D1L_UI_SERVICE_ACTION_ADMIN_LOGOUT;
}

static void advance_generation(
    d1l_ui_service_sheets_controller_t *controller)
{
    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
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
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1E2A36), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x263545),
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
    lv_obj_set_style_bg_color(sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(sheet, lv_color_hex(0x334155), 0);
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
    deactivate_actions(controller);
    controller->action_handler = handler;
    controller->action_context = context;
    lv_obj_clean(sheet);
    return true;
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
    lv_obj_t *status = create_label(sheet, summary, 0x5EEAD4);
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
            entry->level == D1L_EVENT_LOG_LEVEL_INFO ? 0xE5EDF5 :
                                                       0x8EA0AE;
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
    snprintf(line, sizeof(line), "State %s  queued %lu/%lu  PUBACK %lu",
             d1l_observer_state_name(status->state),
             (unsigned long)status->queued,
             (unsigned long)status->queue_capacity,
             (unsigned long)status->acknowledged_total);
    lv_obj_t *state = create_label(sheet, line,
                                   status->connected ? 0x5EEAD4 : 0xFBBF24);
    position_dot(state, 8, 54, 408);
    complete = state && complete;
    snprintf(line, sizeof(line), "Broker %s",
             status->broker_host[0] ? status->broker_host :
                                      "not configured");
    lv_obj_t *broker = create_label(sheet, line, 0xE5EDF5);
    position_dot(broker, 8, 84, 408);
    complete = broker && complete;
    snprintf(line, sizeof(line), "Topic %s%s",
             status->topic[0] ? status->topic : "-",
             status->include_location ? "  + location" : "");
    lv_obj_t *topic = create_label(sheet, line, 0x8EA0AE);
    position_dot(topic, 8, 112, 408);
    complete = topic && complete;
    lv_obj_t *privacy = create_label(
        sheet,
        "Opt-in TLS only. Publishes device health counters and optional manual/companion location; never message text, keys, contacts, or RF forwarding.",
        0x93C5FD);
    position_wrap(privacy, 8, 146, 408);
    complete = privacy && complete;
    if (status->configured) {
        complete = create_button(
            controller, sheet, status->enabled ? "Disable Uploads" :
                                                 "Enable Uploads",
            8, 238, 170, 44, BINDING_OBSERVER_TOGGLE,
            D1L_UI_SERVICE_ACTION_OBSERVER_TOGGLE) != NULL && complete;
    } else {
        lv_obj_t *setup = create_label(
            sheet,
            "Configure once over local USB with `observer configure`; credentials stay off screen and out of logs.",
            0xFBBF24);
        position_wrap(setup, 8, 238, 408);
        complete = setup && complete;
    }
    return complete;
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
        status->state == D1L_UPDATE_STATE_REBOOT_REQUIRED ? 0x5EEAD4 :
                                                            0xFBBF24);
    position_dot(state, 8, 54, 408);
    complete = state && complete;
    snprintf(line, sizeof(line), "Running %s  target %s",
             status->running_partition[0] ? status->running_partition : "-",
             status->target_partition[0] ? status->target_partition : "-");
    lv_obj_t *partitions = create_label(sheet, line, 0xE5EDF5);
    position_dot(partitions, 8, 84, 408);
    complete = partitions && complete;
    snprintf(line, sizeof(line), "Version %s  sequence %lu/%lu",
             status->version[0] ? status->version : "not staged",
             (unsigned long)status->security_sequence,
             (unsigned long)status->highest_security_sequence);
    lv_obj_t *version = create_label(sheet, line, 0x8EA0AE);
    position_dot(version, 8, 112, 408);
    complete = version && complete;
    lv_obj_t *policy = create_label(
        sheet,
        "Local SD only. Manifest, target, partition table, image hash, Ed25519 signature, and anti-downgrade sequence are verified before the inactive slot is written.",
        0x93C5FD);
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
        input->public_unread || input->dm_unread ? 0xFBBF24 : 0x5EEAD4);
    position_dot(counts, 8, 58, 408);
    complete = counts && complete;
    snprintf(line, sizeof(line), "Backlight: %s",
             d1l_notification_mode_name(input->mode));
    lv_obj_t *mode = create_label(sheet, line, 0xE5EDF5);
    position_dot(mode, 8, 96, 408);
    complete = mode && complete;
    lv_obj_t *privacy = create_label(
        sheet,
        "Badges follow retained read cursors. Duplicate packets do not create duplicate counts. Quiet hours suppress only the backlight pulse from 22:00 to 07:00; no audio is claimed.",
        0x93C5FD);
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
    case D1L_MESHCORE_ADMIN_TIMED_OUT:
        return "timed_out";
    default:
        return "invalid";
    }
}

static const char *admin_role_name(d1l_meshcore_admin_role_t role)
{
    return role == D1L_MESHCORE_ADMIN_ROLE_REPEATER ? "repeater" :
           role == D1L_MESHCORE_ADMIN_ROLE_ROOM ? "room" : "none";
}

bool d1l_ui_service_sheets_render_admin(
    d1l_ui_service_sheets_controller_t *controller,
    const d1l_meshcore_admin_snapshot_t *status,
    const char *selected_fingerprint,
    d1l_meshcore_admin_mutation_t armed_mutation,
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
        status->state == D1L_MESHCORE_ADMIN_AUTHENTICATED ? 0x5EEAD4 :
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
    lv_obj_t *server = create_label(sheet, line, 0xE5EDF5);
    position_dot(server, 8, 84, 408);
    complete = server && complete;
    if (status->status_valid) {
        snprintf(line, sizeof(line),
                 "RX %lu  TX %lu  uptime %lus  errors 0x%04x",
                 (unsigned long)status->status.packets_received,
                 (unsigned long)status->status.packets_sent,
                 (unsigned long)status->status.uptime_seconds,
                 (unsigned)status->status.error_flags);
    } else {
        snprintf(line, sizeof(line), "No authenticated status response yet");
    }
    lv_obj_t *metrics = create_label(sheet, line, 0x8EA0AE);
    position_dot(metrics, 8, 114, 408);
    complete = metrics && complete;
    snprintf(
        line, sizeof(line), "Last change %s  result %s",
        d1l_meshcore_admin_mutation_name(status->last_mutation),
        status->last_mutation == D1L_MESHCORE_ADMIN_MUTATION_NONE ?
            "not_run" :
            (status->last_mutation_success ? "confirmed" : "not_confirmed"));
    lv_obj_t *mutation = create_label(sheet, line, 0x8EA0AE);
    position_dot(mutation, 8, 140, 408);
    complete = mutation && complete;
    lv_obj_t *policy = create_label(
        sheet,
        "Compatible verified room/repeater only. Credentials are volatile and redacted. Login is accepted only over local USB; RF changes require authenticated capability plus a separate local confirmation.",
        0x93C5FD);
    position_wrap(policy, 8, 166, 408);
    complete = policy && complete;

    if (status->state == D1L_MESHCORE_ADMIN_AUTHENTICATED) {
        const bool can_mutate =
            (status->permissions & D1L_MESHCORE_ADMIN_PERMISSION_ADMIN) ==
                D1L_MESHCORE_ADMIN_PERMISSION_ADMIN &&
            ((status->role == D1L_MESHCORE_ADMIN_ROLE_REPEATER &&
              status->firmware_level == 2U) ||
             (status->role == D1L_MESHCORE_ADMIN_ROLE_ROOM &&
              status->firmware_level == 1U));
        if (can_mutate) {
            complete = create_button(
                controller, sheet,
                armed_mutation ==
                        D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS ?
                    "Confirm Clear Stats" : "Clear Remote Stats",
                8, 238, 190, 44, BINDING_ADMIN_CLEAR_STATS,
                D1L_UI_SERVICE_ACTION_ADMIN_CLEAR_STATS) != NULL && complete;
            complete = create_button(
                controller, sheet,
                armed_mutation ==
                        D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP ?
                    "Confirm Zero-Hop" : "Send Zero-Hop Advert",
                210, 238, 190, 44,
                BINDING_ADMIN_ADVERTISE_ZERO_HOP,
                D1L_UI_SERVICE_ACTION_ADMIN_ADVERTISE_ZERO_HOP) != NULL &&
                complete;
        }
        complete = create_button(
            controller, sheet, "Refresh Status", 8, 292, 152, 44,
            BINDING_ADMIN_REFRESH,
            D1L_UI_SERVICE_ACTION_ADMIN_REFRESH) != NULL && complete;
        complete = create_button(
            controller, sheet, "Logout", 172, 292, 100, 44,
            BINDING_ADMIN_LOGOUT,
            D1L_UI_SERVICE_ACTION_ADMIN_LOGOUT) != NULL && complete;
    } else if (status->state == D1L_MESHCORE_ADMIN_LOGIN_PENDING ||
               status->state == D1L_MESHCORE_ADMIN_STATUS_PENDING ||
               status->state == D1L_MESHCORE_ADMIN_MUTATION_PENDING) {
        complete = create_button(
            controller, sheet, "Logout / Cancel", 8, 258, 160, 44,
            BINDING_ADMIN_LOGOUT,
            D1L_UI_SERVICE_ACTION_ADMIN_LOGOUT) != NULL && complete;
    } else {
        if (selected_fingerprint && selected_fingerprint[0]) {
            snprintf(line, sizeof(line),
                     "Local USB: admin login %.16s <password>",
                     selected_fingerprint);
        } else {
            snprintf(line, sizeof(line),
                     "Local USB: admin login <fingerprint> <password>");
        }
        lv_obj_t *login = create_label(sheet, line, 0xFBBF24);
        position_dot(login, 8, 266, 408);
        complete = login && complete;
    }
    return complete;
}

void d1l_ui_service_sheets_hide_all(
    d1l_ui_service_sheets_controller_t *controller)
{
    if (!controller) {
        return;
    }
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
