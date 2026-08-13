#include "ui_messages.h"

#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"
#include "lvgl.h"
#include "ui_font_symbols_14.h"
#include "ui_modal.h"

_Static_assert(sizeof(d1l_ui_messages_controller_t) <=
                   D1L_UI_MESSAGES_CONTROLLER_MAX_BYTES,
               "Messages controller exceeded its persistent-owner size budget");

uint64_t d1l_ui_messages_projected_channel_id(uint64_t active_channel_id)
{
    return d1l_release_feature_available(
               D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT) ?
        active_channel_id : D1L_CHANNEL_PUBLIC_ID;
}

bool d1l_ui_messages_action_available(d1l_ui_messages_action_t action)
{
    switch (action) {
    case D1L_UI_MESSAGES_ACTION_MARK_PUBLIC_READ:
    case D1L_UI_MESSAGES_ACTION_COMPOSE_PUBLIC:
    case D1L_UI_MESSAGES_ACTION_OPEN_HISTORY:
    case D1L_UI_MESSAGES_ACTION_SHOW_PUBLIC:
    case D1L_UI_MESSAGES_ACTION_OPEN_PUBLIC_MESSAGE:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_PUBLIC_MESSAGES);
    case D1L_UI_MESSAGES_ACTION_SHOW_DIRECT:
    case D1L_UI_MESSAGES_ACTION_OPEN_DM_THREAD:
    case D1L_UI_MESSAGES_ACTION_CLOSE_DM_THREAD:
    case D1L_UI_MESSAGES_ACTION_LOAD_OLDER_DM_THREAD:
    case D1L_UI_MESSAGES_ACTION_OPEN_DM_SEARCH:
    case D1L_UI_MESSAGES_ACTION_REPLY_DM_THREAD:
    case D1L_UI_MESSAGES_ACTION_TOGGLE_DM_DETAILS:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_DIRECT_MESSAGES);
    case D1L_UI_MESSAGES_ACTION_OPEN_CHANNEL_SELECTOR:
    case D1L_UI_MESSAGES_ACTION_CLOSE_CHANNEL_SELECTOR:
    case D1L_UI_MESSAGES_ACTION_CREATE_CHANNEL:
    case D1L_UI_MESSAGES_ACTION_IMPORT_CHANNEL:
    case D1L_UI_MESSAGES_ACTION_SELECT_CHANNEL:
    case D1L_UI_MESSAGES_ACTION_MANAGE_CHANNEL:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT);
    case D1L_UI_MESSAGES_ACTION_SHOW_ROOT:
        return true;
    case D1L_UI_MESSAGES_ACTION_NONE:
    default:
        return false;
    }
}

static bool messages_object_is_valid(const lv_obj_t *object)
{
    return object && lv_obj_is_valid(object);
}

static bool messages_text_fits(const char *text, size_t capacity)
{
    if (!text || capacity == 0U) {
        return false;
    }
    size_t length = 0U;
    while (length < capacity && text[length] != '\0') {
        length++;
    }
    return length < capacity;
}

static void messages_advance_generation(d1l_ui_messages_controller_t *controller)
{
    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
    }
}

static void messages_deactivate_actions(d1l_ui_messages_controller_t *controller)
{
    if (!controller) {
        return;
    }
    messages_advance_generation(controller);
    memset(controller->controls, 0, sizeof(controller->controls));
    memset(controller->public_rows, 0, sizeof(controller->public_rows));
    memset(controller->dm_rows, 0, sizeof(controller->dm_rows));
    memset(controller->thread_controls, 0, sizeof(controller->thread_controls));
    memset(controller->thread_rows, 0, sizeof(controller->thread_rows));
    memset(controller->channel_controls, 0, sizeof(controller->channel_controls));
    memset(controller->channel_rows, 0, sizeof(controller->channel_rows));
    memset(controller->channel_manage_rows, 0,
           sizeof(controller->channel_manage_rows));
    controller->action_handler = NULL;
    controller->action_context = NULL;
}

static void messages_begin_actions(
    d1l_ui_messages_controller_t *controller,
    d1l_ui_messages_action_handler_t action_handler,
    void *action_context)
{
    messages_deactivate_actions(controller);
    controller->action_handler = action_handler;
    controller->action_context = action_context;
}

static lv_obj_t *messages_create_full_sheet(lv_obj_t *parent)
{
    if (!messages_object_is_valid(parent)) {
        return NULL;
    }
    lv_obj_t *sheet = lv_obj_create(parent);
    if (!sheet) {
        return NULL;
    }
    lv_obj_set_size(sheet, 480, 480);
    lv_obj_set_pos(sheet, 0, 0);
    lv_obj_set_style_radius(sheet, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_width(sheet, 0, 0);
    lv_obj_set_style_pad_all(sheet, 0, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    d1l_ui_modal_hide(sheet);
    return sheet;
}

bool d1l_ui_messages_create(d1l_ui_messages_controller_t *controller,
                            lv_obj_t *parent)
{
    if (!controller) {
        return false;
    }
    messages_deactivate_actions(controller);
    if (messages_object_is_valid(controller->thread_sheet)) {
        d1l_ui_modal_hide(controller->thread_sheet);
        lv_obj_del(controller->thread_sheet);
    }
    if (messages_object_is_valid(controller->channel_sheet)) {
        d1l_ui_modal_hide(controller->channel_sheet);
        lv_obj_del(controller->channel_sheet);
    }
    memset(controller, 0, sizeof(*controller));
    controller->thread_limit = D1L_UI_MESSAGES_THREAD_INITIAL_ROWS;
    controller->thread_sheet = messages_create_full_sheet(parent);
    const bool channel_management_available =
        d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT);
    if (channel_management_available) {
        controller->channel_sheet = messages_create_full_sheet(parent);
    }
    if (!controller->thread_sheet ||
        (channel_management_available && !controller->channel_sheet)) {
        if (messages_object_is_valid(controller->thread_sheet)) {
            lv_obj_del(controller->thread_sheet);
        }
        if (messages_object_is_valid(controller->channel_sheet)) {
            lv_obj_del(controller->channel_sheet);
        }
        controller->thread_sheet = NULL;
        controller->channel_sheet = NULL;
        return false;
    }
    return true;
}

static lv_obj_t *messages_create_label(lv_obj_t *parent, const char *text, uint32_t color)
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
    lv_obj_set_style_text_font(label, &d1l_ui_font_symbols_14, 0);
    return label;
}

static void messages_set_dot_width(lv_obj_t *label, lv_coord_t width)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
}

static lv_obj_t *messages_create_panel(lv_obj_t *parent, int x, int y, int width, int height)
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
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111923), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x263241), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static void messages_dispatch_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_messages_action_binding_t *binding =
        (d1l_ui_messages_action_binding_t *)lv_event_get_user_data(event);
    if (!binding || !binding->controller || binding->generation == 0U ||
        binding->generation != binding->controller->generation ||
        !d1l_ui_messages_action_available(binding->action)) {
        return;
    }
    d1l_ui_messages_controller_t *controller = binding->controller;
    if (!controller->action_handler) {
        return;
    }

    d1l_ui_messages_action_event_t action_event = {
        .action = binding->action,
        .public_message = NULL,
        .dm_message = NULL,
        .channel = NULL,
    };
    if (binding->action == D1L_UI_MESSAGES_ACTION_OPEN_PUBLIC_MESSAGE) {
        if (binding->row_index >= controller->rendered.public_row_count) {
            return;
        }
        action_event.public_message = &controller->rendered.public_rows[binding->row_index];
    } else if (binding->action == D1L_UI_MESSAGES_ACTION_OPEN_DM_THREAD) {
        if (binding->row_index >= controller->rendered.dm_row_count) {
            return;
        }
        action_event.dm_message = &controller->rendered.dm_rows[binding->row_index];
    } else if (binding->action == D1L_UI_MESSAGES_ACTION_TOGGLE_DM_DETAILS) {
        if (binding->row_index >= controller->thread_row_count) {
            return;
        }
        action_event.dm_message = &controller->thread_entries[binding->row_index];
    } else if (binding->action == D1L_UI_MESSAGES_ACTION_SELECT_CHANNEL ||
               binding->action == D1L_UI_MESSAGES_ACTION_MANAGE_CHANNEL) {
        if (binding->row_index >= controller->rendered.channel_count) {
            return;
        }
        action_event.channel = &controller->rendered.channels[binding->row_index];
    }
    controller->action_handler(&action_event, controller->action_context);
}

static lv_obj_t *messages_create_button(lv_obj_t *parent,
                                        const char *text,
                                        int x,
                                        int y,
                                        int width,
                                        int height,
                                        d1l_ui_messages_action_binding_t *binding)
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
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1E2A36), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x263545), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *label = messages_create_label(button, text, 0xF4F7FB);
    if (!label) {
        lv_obj_del(button);
        return NULL;
    }
    lv_obj_center(label);
    if (binding) {
        lv_obj_add_event_cb(button, messages_dispatch_event_cb, LV_EVENT_CLICKED, binding);
    }
    return button;
}

static d1l_ui_messages_action_binding_t *messages_bind_control(
    d1l_ui_messages_controller_t *controller,
    size_t index,
    d1l_ui_messages_action_t action)
{
    if (!controller || index >= D1L_UI_MESSAGES_CONTROL_BINDING_COUNT) {
        return NULL;
    }
    d1l_ui_messages_action_binding_t *binding = &controller->controls[index];
    *binding = (d1l_ui_messages_action_binding_t) {
        .controller = controller,
        .action = action,
        .row_index = 0U,
        .generation = controller->generation,
    };
    return binding;
}

static const char *messages_inbound_state(const d1l_dm_entry_t *entry,
                                          bool unread)
{
    if (unread) {
        return "New";
    }
    if (!entry->identity_digest_valid) {
        return "Received (legacy)";
    }
    switch (entry->ack_state) {
    case D1L_DM_ACK_STATE_PENDING:
        return "Received / confirming";
    case D1L_DM_ACK_STATE_SENT:
        return "Received / confirmed";
    case D1L_DM_ACK_STATE_RETRYABLE:
        if (entry->ack_dispatch_count == 0U &&
            entry->ack_last_error == ESP_OK) {
            return "Received / confirming";
        }
        return "Received / confirming";
    case D1L_DM_ACK_STATE_TERMINAL:
        return "Received / confirmation failed";
    case D1L_DM_ACK_STATE_LEGACY_UNVERIFIED:
    default:
        return "Received";
    }
}

const char *d1l_ui_messages_delivery_label(const d1l_dm_entry_t *entry,
                                           bool unread)
{
    if (!entry) {
        return "Status unavailable";
    }
    if (entry->direction[0] != 't') {
        return messages_inbound_state(entry, unread);
    }
    switch (entry->delivery_state) {
    case D1L_DM_DELIVERY_QUEUED:
        return "Queued";
    case D1L_DM_DELIVERY_WAITING_RADIO:
        return "Waiting for radio";
    case D1L_DM_DELIVERY_TX_ACTIVE:
        return "Sending";
    case D1L_DM_DELIVERY_TX_DONE:
        return "Sent by radio";
    case D1L_DM_DELIVERY_AWAITING_ACK:
        return "Sent / checking delivery";
    case D1L_DM_DELIVERY_ACKNOWLEDGED:
        return "Delivered";
    case D1L_DM_DELIVERY_RETRY_WAIT:
        return "Will retry";
    case D1L_DM_DELIVERY_RETRY_TX:
        return "Retrying";
    case D1L_DM_DELIVERY_FAILED_RADIO:
    case D1L_DM_DELIVERY_FAILED_TIMEOUT:
    case D1L_DM_DELIVERY_FAILED_QUEUE:
        return "Failed";
    case D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT:
        return "Interrupted by reboot";
    case D1L_DM_DELIVERY_CANCELLED:
        return "Cancelled";
    case D1L_DM_DELIVERY_NOT_APPLICABLE:
    default:
        return "Status unavailable";
    }
}

static void messages_format_snr(char *dest, size_t dest_size, int snr_tenths)
{
    if (!dest || dest_size == 0U) {
        return;
    }
    const int absolute = snr_tenths < 0 ? -snr_tenths : snr_tenths;
    snprintf(dest, dest_size, "%s%d.%d", snr_tenths < 0 ? "-" : "",
             absolute / 10, absolute % 10);
}

static lv_obj_t *messages_create_scroll_body(lv_obj_t *parent,
                                             int x,
                                             int y,
                                             int width,
                                             int height)
{
    lv_obj_t *body = messages_create_panel(parent, x, y, width, height);
    if (!body) {
        return NULL;
    }
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    return body;
}

static void messages_make_clickable(
    lv_obj_t *object,
    d1l_ui_messages_action_binding_t *binding)
{
    if (!object || !binding) {
        return;
    }
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(object, messages_dispatch_event_cb, LV_EVENT_CLICKED,
                        binding);
}

static const char *messages_public_time_label(const d1l_message_entry_t *entry)
{
    /* Retained rows have uptime but no persisted boot epoch.  Never present
     * that value as wall-clock or relative time across reboot. */
    (void)entry;
    return "time unknown";
}

static const char *messages_public_state(const d1l_message_entry_t *entry,
                                         bool unread)
{
    if (!entry) {
        return "Status unknown";
    }
    if (entry->direction[0] == 't') {
        /* Public rows are retained only after the radio reports TxDone. */
        return "Sent by radio";
    }
    return unread ? "New" : "Received";
}

static void messages_render_public_row(d1l_ui_messages_controller_t *controller,
                                       lv_obj_t *body,
                                       int y,
                                       size_t index)
{
    if (!controller || !body || index >= controller->rendered.public_row_count) {
        return;
    }
    const d1l_message_entry_t *entry = &controller->rendered.public_rows[index];
    const bool outgoing = entry->direction[0] == 't';
    const bool unread = !outgoing &&
        entry->seq > controller->rendered.last_channel_read_seq;
    lv_obj_t *bubble = messages_create_panel(body, outgoing ? 76 : 8, y, 332, 68);
    if (!bubble) {
        return;
    }
    lv_obj_set_style_bg_color(
        bubble, lv_color_hex(outgoing ? 0x19263A : 0x122D2A), 0);
    lv_obj_set_style_border_color(
        bubble, lv_color_hex(outgoing ? 0x3B5B86 : 0x28635A), 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    d1l_ui_messages_action_binding_t *binding = &controller->public_rows[index];
    *binding = (d1l_ui_messages_action_binding_t) {
        .controller = controller,
        .action = D1L_UI_MESSAGES_ACTION_OPEN_PUBLIC_MESSAGE,
        .row_index = index,
        .generation = controller->generation,
    };
    messages_make_clickable(bubble, binding);

    char state_and_time[64];
    snprintf(state_and_time, sizeof(state_and_time), "%s | %s",
             messages_public_state(entry, unread),
             messages_public_time_label(entry));

    lv_obj_t *author = messages_create_label(
        bubble, outgoing ? "You" :
        (entry->author[0] ? entry->author : "Unknown sender"),
        unread ? 0xFBBF24 : (outgoing ? 0x93C5FD : 0x5EEAD4));
    messages_set_dot_width(author, 132);
    if (author) {
        lv_obj_set_pos(author, 8, 2);
    }
    lv_obj_t *state = messages_create_label(bubble, state_and_time, 0x8EA0AE);
    messages_set_dot_width(state, 168);
    if (state) {
        lv_obj_set_pos(state, 148, 2);
    }
    lv_obj_t *text = messages_create_label(
        bubble, entry->text[0] ? entry->text : "No message text", 0xE5EDF5);
    if (text) {
        lv_obj_set_pos(text, 8, 26);
        lv_obj_set_size(text, 308, 28);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    }
}

static void messages_render_dm_row(d1l_ui_messages_controller_t *controller,
                                   lv_obj_t *parent,
                                   int y,
                                   size_t index)
{
    if (!controller || !parent || index >= controller->rendered.dm_row_count) {
        return;
    }
    const d1l_dm_entry_t *entry = &controller->rendered.dm_rows[index];
    const uint32_t unread_count =
        controller->rendered.dm_row_unread_count[index];
    const bool unread = unread_count > 0U;
    const bool muted = controller->rendered.dm_row_muted[index];
    lv_obj_t *row = messages_create_panel(parent, 8, y, 408, 72);
    if (!row) {
        return;
    }
    d1l_ui_messages_action_binding_t *binding = &controller->dm_rows[index];
    *binding = (d1l_ui_messages_action_binding_t) {
        .controller = controller,
        .action = D1L_UI_MESSAGES_ACTION_OPEN_DM_THREAD,
        .row_index = index,
        .generation = controller->generation,
    };
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, messages_dispatch_event_cb, LV_EVENT_CLICKED, binding);
    lv_obj_set_style_pad_all(row, 8, 0);

    lv_obj_t *alias = messages_create_label(
        row, entry->contact_alias[0] ? entry->contact_alias : "Mesh contact",
        unread && !muted ? 0xFBBF24 :
        (muted ? 0x8EA0AE :
         (entry->direction[0] == 't' ? 0xC4B5FD : 0xA7F3D0)));
    messages_set_dot_width(alias, 200);
    if (alias) {
        lv_obj_set_pos(alias, 8, 4);
    }
    const char *latest_state;
    if (entry->direction[0] != 't') {
        latest_state = "Received";
    } else {
        switch (entry->delivery_state) {
        case D1L_DM_DELIVERY_QUEUED:
            latest_state = "Queued";
            break;
        case D1L_DM_DELIVERY_WAITING_RADIO:
            latest_state = "Waiting for radio";
            break;
        case D1L_DM_DELIVERY_TX_ACTIVE:
            latest_state = "Sending";
            break;
        case D1L_DM_DELIVERY_TX_DONE:
            latest_state = "Sent";
            break;
        case D1L_DM_DELIVERY_AWAITING_ACK:
            latest_state = "Checking delivery";
            break;
        case D1L_DM_DELIVERY_ACKNOWLEDGED:
            latest_state = "Delivered";
            break;
        case D1L_DM_DELIVERY_RETRY_WAIT:
            latest_state = "Will retry";
            break;
        case D1L_DM_DELIVERY_RETRY_TX:
            latest_state = "Retrying";
            break;
        case D1L_DM_DELIVERY_FAILED_RADIO:
        case D1L_DM_DELIVERY_FAILED_TIMEOUT:
        case D1L_DM_DELIVERY_FAILED_QUEUE:
            latest_state = "Failed";
            break;
        case D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT:
            latest_state = "Interrupted";
            break;
        case D1L_DM_DELIVERY_CANCELLED:
            latest_state = "Cancelled";
            break;
        case D1L_DM_DELIVERY_NOT_APPLICABLE:
        default:
            latest_state = "Status unknown";
            break;
        }
    }
    char state_text[96];
    if (unread) {
        snprintf(state_text, sizeof(state_text), "%lu unread%s | %s",
                 (unsigned long)unread_count,
                 muted ? " (excluded from total)" : "",
                 latest_state);
    } else {
        snprintf(state_text, sizeof(state_text), "%s", latest_state);
    }
    lv_obj_t *state = messages_create_label(
        row, state_text, unread && !muted ? 0xFBBF24 : 0x8EA0AE);
    messages_set_dot_width(state, 168);
    if (state) {
        lv_obj_set_pos(state, 232, 4);
    }
    lv_obj_t *text = messages_create_label(row, entry->text[0] ? entry->text : "-", 0xE5EDF5);
    messages_set_dot_width(text, 392);
    if (text) {
        lv_obj_set_pos(text, 8, 28);
    }
}

static void messages_render_channel_row(
    d1l_ui_messages_controller_t *controller,
    lv_obj_t *parent,
    int y,
    size_t index)
{
    if (!controller || !parent || index >= controller->rendered.channel_count) {
        return;
    }
    const d1l_channel_info_t *channel = &controller->rendered.channels[index];
    const bool active =
        channel->channel_id == controller->rendered.active_channel_id;
    lv_obj_t *row = messages_create_panel(parent, 8, y, 408, 60);
    if (!row) {
        return;
    }
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(active ? 0x122D2A : 0x111923), 0);
    if (active) {
        lv_obj_set_style_border_color(row, lv_color_hex(0x5EEAD4), 0);
    }
    if (channel->enabled) {
        d1l_ui_messages_action_binding_t *binding =
            &controller->channel_rows[index];
        *binding = (d1l_ui_messages_action_binding_t) {
            .controller = controller,
            .action = D1L_UI_MESSAGES_ACTION_SELECT_CHANNEL,
            .row_index = index,
            .generation = controller->generation,
        };
        messages_make_clickable(row, binding);
    }
    char display_name[D1L_CHANNEL_NAME_LEN + 2U];
    const char *name = channel->name[0] ? channel->name : "Unnamed";
    if (name[0] == '#') {
        snprintf(display_name, sizeof(display_name), "%s", name);
    } else {
        snprintf(display_name, sizeof(display_name), "#%s", name);
    }
    lv_obj_t *title = messages_create_label(
        row, display_name,
        channel->enabled ? (active ? 0x5EEAD4 : 0xF4F7FB) : 0x8EA0AE);
    messages_set_dot_width(title, 250);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(title, 8, 2);
    }
    char state[96];
    snprintf(state, sizeof(state), "%s%s",
             channel->enabled ? (active ? "Open now" : "Tap to open") :
                 "Disabled - open settings to enable",
             channel->channel_id == D1L_CHANNEL_PUBLIC_ID ? " | default" : "");
    lv_obj_t *meta = messages_create_label(row, state, 0x8EA0AE);
    messages_set_dot_width(meta, 310);
    if (meta) {
        lv_obj_set_pos(meta, 8, 32);
    }
    if (channel->unread_count > 0U) {
        char unread[16];
        snprintf(unread, sizeof(unread), "%lu",
                 (unsigned long)channel->unread_count);
        lv_obj_t *badge = messages_create_label(row, unread, 0xFBBF24);
        if (badge) {
            lv_obj_set_pos(badge, 350, 6);
        }
    }
    lv_obj_t *arrow = messages_create_label(row, ">", 0x8EA0AE);
    if (arrow) {
        lv_obj_set_pos(arrow, 382, 28);
    }
}

static int messages_render_notice(lv_obj_t *parent,
                                  int y,
                                  const char *text,
                                  uint32_t color);
static int messages_render_store_notice(
    lv_obj_t *parent,
    int y,
    d1l_ui_messages_store_state_t state);

static void messages_render_root(d1l_ui_messages_controller_t *controller,
                                 lv_obj_t *parent)
{
    lv_obj_t *title = messages_create_label(parent, "Channels", 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_pos(title, 18, 10);
    }
    messages_create_button(
        parent, "Direct", 342, 6, 100, 44,
        messages_bind_control(controller, 1U,
                              D1L_UI_MESSAGES_ACTION_SHOW_DIRECT));
    lv_obj_t *subtitle = messages_create_label(
        parent, "Group conversations", 0x8EA0AE);
    if (subtitle) {
        lv_obj_set_pos(subtitle, 18, 40);
    }
    lv_obj_t *body = messages_create_scroll_body(parent, 18, 66, 424, 288);
    if (!body) {
        return;
    }
    int row_y = messages_render_store_notice(
        body, 8, controller->rendered.public_store_state);
    if (!controller->rendered.channel_store_loaded) {
        (void)messages_render_notice(
            body, row_y, "Loading channels...", 0x93C5FD);
        return;
    }
    if (controller->rendered.channel_count == 0U) {
        (void)messages_render_notice(
            body, row_y, "No channels yet. Add or import a channel.",
            0x8EA0AE);
        if (d1l_ui_messages_action_available(
                D1L_UI_MESSAGES_ACTION_OPEN_CHANNEL_SELECTOR)) {
            messages_create_button(
                body, "Add a channel", 8, row_y + 60, 408, 48,
                messages_bind_control(
                    controller, 4U,
                    D1L_UI_MESSAGES_ACTION_OPEN_CHANNEL_SELECTOR));
        }
        return;
    }
    for (size_t i = 0U; i < controller->rendered.channel_count; ++i) {
        messages_render_channel_row(controller, body, row_y + (int)i * 68, i);
    }
}

static int messages_render_notice(lv_obj_t *parent,
                                  int y,
                                  const char *text,
                                  uint32_t color)
{
    if (!parent || !text || text[0] == '\0') {
        return y;
    }
    lv_obj_t *panel = messages_create_panel(parent, 8, y, 408, 44);
    if (!panel) {
        return y;
    }
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_t *label = messages_create_label(panel, text, color);
    messages_set_dot_width(label, 390);
    if (label) {
        lv_obj_set_pos(label, 0, 4);
    }
    return y + 52;
}

static int messages_render_store_notice(
    lv_obj_t *parent,
    int y,
    d1l_ui_messages_store_state_t state)
{
    switch (state) {
    case D1L_UI_MESSAGES_STORE_LOADING:
        return messages_render_notice(
            parent, y, "Loading saved messages...", 0x93C5FD);
    case D1L_UI_MESSAGES_STORE_DEGRADED:
        return messages_render_notice(
            parent, y,
            "Storage needs attention. Recent messages are still available.",
            0xFBBF24);
    case D1L_UI_MESSAGES_STORE_UNAVAILABLE:
        return messages_render_notice(
            parent, y,
            "Messages cannot be saved. Recent messages are still available.",
            0xF87171);
    case D1L_UI_MESSAGES_STORE_READY:
    default:
        return y;
    }
}

static void messages_render_public(d1l_ui_messages_controller_t *controller,
                                   lv_obj_t *parent)
{
    messages_create_button(
        parent, "Back", 18, 8, 72, 44,
        messages_bind_control(controller, 0U,
                              D1L_UI_MESSAGES_ACTION_SHOW_ROOT));
    lv_obj_t *title = messages_create_label(
        parent,
        controller->rendered.active_channel_name[0] ?
            controller->rendered.active_channel_name : "Channel unavailable",
        0x5EEAD4);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        messages_set_dot_width(title, 226);
        lv_obj_set_pos(title, 104, 10);
    }
    messages_create_button(
        parent, "Read", 306, 8, 64, 44,
        messages_bind_control(controller, 1U,
                              D1L_UI_MESSAGES_ACTION_MARK_PUBLIC_READ));
    messages_create_button(
        parent, "...", 378, 8, 64, 44,
        messages_bind_control(controller, 2U,
                              D1L_UI_MESSAGES_ACTION_OPEN_HISTORY));
    lv_obj_t *body = messages_create_scroll_body(parent, 18, 60, 424, 238);
    if (!body) {
        return;
    }
    int row_y = messages_render_store_notice(
        body, 8, controller->rendered.public_store_state);
    for (size_t i = 0; i < controller->rendered.public_row_count; ++i) {
        messages_render_public_row(controller, body, row_y + (int)i * 74, i);
    }
    if (controller->rendered.public_row_count > 0U) {
        lv_obj_update_layout(body);
        if (controller->rendered.public_store_state ==
            D1L_UI_MESSAGES_STORE_READY) {
            lv_obj_scroll_to_y(body, LV_COORD_MAX, LV_ANIM_OFF);
        } else {
            lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
        }
    }
    if (controller->rendered.public_row_count == 0U) {
        const char *empty_text =
            !controller->rendered.active_channel_enabled ?
                "Channel unavailable or disabled." :
            controller->rendered.public_store_state ==
                    D1L_UI_MESSAGES_STORE_LOADING ?
                "Loading saved messages..." :
            controller->rendered.public_store_state ==
                    D1L_UI_MESSAGES_STORE_UNAVAILABLE ?
                "No readable channel history in RAM." :
                "No messages in this channel yet";
        lv_obj_t *empty = messages_create_label(
            body, empty_text, 0x8EA0AE);
        if (empty) {
            lv_obj_set_pos(empty, 16, row_y + 8);
        }
    }
    lv_obj_t *compose = messages_create_button(
        parent, controller->rendered.active_channel_enabled ?
            "Message this channel                         >" :
            "Channel unavailable",
        18, 306, 424, 48,
        controller->rendered.active_channel_enabled ?
            messages_bind_control(controller, 3U,
                                  D1L_UI_MESSAGES_ACTION_COMPOSE_PUBLIC) :
            NULL);
    if (compose && !controller->rendered.active_channel_enabled) {
        lv_obj_add_state(compose, LV_STATE_DISABLED);
    }
}

static void messages_render_direct(d1l_ui_messages_controller_t *controller,
                                   lv_obj_t *parent)
{
    messages_create_button(
        parent, "Back", 18, 8, 72, 44,
        messages_bind_control(controller, 0U,
                              D1L_UI_MESSAGES_ACTION_SHOW_ROOT));
    lv_obj_t *title = messages_create_label(
        parent, "Direct messages", 0xA7F3D0);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        messages_set_dot_width(title, 250);
        lv_obj_set_pos(title, 104, 10);
    }
    if (controller->rendered.dm_unread > 0U) {
        char unread[48];
        snprintf(unread, sizeof(unread), "%lu unread",
                 (unsigned long)controller->rendered.dm_unread);
        lv_obj_t *meta = messages_create_label(parent, unread, 0xFBBF24);
        messages_set_dot_width(meta, 100);
        if (meta) {
            lv_obj_set_pos(meta, 342, 18);
        }
    }
    lv_obj_t *body = messages_create_scroll_body(parent, 18, 68, 424, 286);
    if (!body) {
        return;
    }
    int row_y = messages_render_store_notice(
        body, 8, controller->rendered.dm_store_state);
    if (controller->rendered.dm_retry_active) {
        row_y = messages_render_notice(
            body, row_y, "Trying to deliver the message again.",
            0x93C5FD);
    }
    if (controller->rendered.dm_failure_latched) {
        row_y = messages_render_notice(
            body, row_y,
            "A message could not be delivered. Open it for details.",
            0xF87171);
    }
    for (size_t i = 0; i < controller->rendered.dm_row_count; ++i) {
        messages_render_dm_row(controller, body, row_y + (int)i * 80, i);
    }
    if (controller->rendered.dm_row_count == 0U) {
        const char *empty_text;
        if (controller->rendered.dm_store_state ==
            D1L_UI_MESSAGES_STORE_LOADING) {
            empty_text = "Loading saved conversations...";
        } else if (controller->rendered.dm_store_state ==
                   D1L_UI_MESSAGES_STORE_UNAVAILABLE) {
            empty_text = "No readable direct-message history in RAM.";
        } else if (controller->rendered.dm_capable_contact_count == 0U) {
            empty_text = "No messaging contacts yet. Add a verified contact.";
        } else {
            empty_text = "No direct-message history yet.";
        }
        lv_obj_t *empty = messages_create_label(
            body, empty_text, 0x8EA0AE);
        if (empty) {
            lv_obj_set_pos(empty, 16, row_y + 8);
        }
    }
}

void d1l_ui_messages_render(d1l_ui_messages_controller_t *controller,
                            lv_obj_t *parent,
                            const d1l_ui_messages_view_model_t *view_model,
                            d1l_ui_messages_action_handler_t action_handler,
                            void *action_context)
{
    if (!controller || !messages_object_is_valid(parent) || !view_model ||
        !action_handler) {
        return;
    }
    messages_begin_actions(controller, action_handler, action_context);
    controller->rendered = *view_model;
    if (controller->rendered.public_row_count >
        D1L_UI_MESSAGES_PUBLIC_PREVIEW_ROWS) {
        controller->rendered.public_row_count =
            D1L_UI_MESSAGES_PUBLIC_PREVIEW_ROWS;
    }
    if (controller->rendered.dm_row_count >
        D1L_UI_MESSAGES_DM_PREVIEW_ROWS) {
        controller->rendered.dm_row_count = D1L_UI_MESSAGES_DM_PREVIEW_ROWS;
    }

    switch (controller->rendered.mode) {
    case D1L_UI_MESSAGES_MODE_PUBLIC:
        messages_render_public(controller, parent);
        break;
    case D1L_UI_MESSAGES_MODE_DIRECT:
        messages_render_direct(controller, parent);
        break;
    case D1L_UI_MESSAGES_MODE_ROOT:
    default:
        messages_render_root(controller, parent);
        break;
    }
}

static d1l_ui_messages_action_binding_t *messages_bind_channel_control(
    d1l_ui_messages_controller_t *controller,
    size_t index,
    d1l_ui_messages_action_t action)
{
    if (!controller ||
        index >= D1L_UI_MESSAGES_CHANNEL_CONTROL_BINDING_COUNT) {
        return NULL;
    }
    d1l_ui_messages_action_binding_t *binding =
        &controller->channel_controls[index];
    *binding = (d1l_ui_messages_action_binding_t) {
        .controller = controller,
        .action = action,
        .row_index = 0U,
        .generation = controller->generation,
    };
    return binding;
}

static bool messages_render_channel_selector_notice(lv_obj_t *parent,
                                                    const char *text,
                                                    uint32_t color)
{
    lv_obj_t *panel = messages_create_panel(parent, 8, 8, 408, 44);
    if (!panel) {
        return false;
    }
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_t *label = messages_create_label(panel, text, color);
    if (!label) {
        lv_obj_del(panel);
        return false;
    }
    messages_set_dot_width(label, 390);
    lv_obj_set_pos(label, 0, 4);
    return true;
}

bool d1l_ui_messages_render_channel_selector(
    d1l_ui_messages_controller_t *controller,
    d1l_ui_messages_action_handler_t action_handler,
    void *action_context)
{
    if (!controller || !messages_object_is_valid(controller->channel_sheet) ||
        !action_handler ||
        !d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT)) {
        return false;
    }
    if (controller->rendered.channel_count > D1L_CHANNEL_STORE_CAPACITY) {
        controller->rendered.channel_count = D1L_CHANNEL_STORE_CAPACITY;
    }
    messages_begin_actions(controller, action_handler, action_context);
    lv_obj_t *sheet = controller->channel_sheet;
    lv_obj_clean(sheet);
    bool complete = messages_create_button(
        sheet, "Close", 392, 6, 72, 44,
        messages_bind_channel_control(
            controller, 0U,
            D1L_UI_MESSAGES_ACTION_CLOSE_CHANNEL_SELECTOR)) != NULL;
    complete = messages_create_button(
        sheet, "New", 304, 6, 72, 44,
        messages_bind_channel_control(
            controller, 1U,
            D1L_UI_MESSAGES_ACTION_CREATE_CHANNEL)) != NULL && complete;
    complete = messages_create_button(
        sheet, "Import", 216, 6, 80, 44,
        messages_bind_channel_control(
            controller, 2U,
            D1L_UI_MESSAGES_ACTION_IMPORT_CHANNEL)) != NULL && complete;
    lv_obj_t *title = messages_create_label(sheet, "Channels", 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        messages_set_dot_width(title, 190);
        lv_obj_set_pos(title, 16, 10);
    } else {
        complete = false;
    }
    lv_obj_t *body = messages_create_scroll_body(sheet, 16, 60, 448, 352);
    if (!body) {
        complete = false;
    } else if (!controller->rendered.channel_store_loaded) {
        complete = messages_render_channel_selector_notice(
            body, "Channel list unavailable; selection is unchanged.",
            0xF87171) && complete;
    } else if (controller->rendered.channel_count == 0U) {
        complete = messages_render_channel_selector_notice(
            body, "No configured channels.", 0x8EA0AE) && complete;
    } else {
        for (size_t i = 0U; i < controller->rendered.channel_count; ++i) {
            const d1l_channel_info_t *channel =
                &controller->rendered.channels[i];
            lv_obj_t *row = messages_create_panel(
                body, 8, 8 + (int)i * 60, 416, 52);
            if (!row) {
                complete = false;
                continue;
            }
            lv_obj_set_style_pad_all(row, 8, 0);
            const bool active = channel->channel_id ==
                controller->rendered.active_channel_id;
            if (active) {
                lv_obj_set_style_border_color(row, lv_color_hex(0x5EEAD4), 0);
            }
            if (channel->enabled) {
                d1l_ui_messages_action_binding_t *binding =
                    &controller->channel_rows[i];
                *binding = (d1l_ui_messages_action_binding_t) {
                    .controller = controller,
                    .action = D1L_UI_MESSAGES_ACTION_SELECT_CHANNEL,
                    .row_index = i,
                    .generation = controller->generation,
                };
                messages_make_clickable(row, binding);
            }
            d1l_ui_messages_action_binding_t *manage_binding =
                &controller->channel_manage_rows[i];
            *manage_binding = (d1l_ui_messages_action_binding_t) {
                .controller = controller,
                .action = D1L_UI_MESSAGES_ACTION_MANAGE_CHANNEL,
                .row_index = i,
                .generation = controller->generation,
            };
            if (!messages_create_button(
                    row, "Manage", 310, 0, 96, 44, manage_binding)) {
                complete = false;
            }
            lv_obj_t *name = messages_create_label(
                row, channel->name[0] ? channel->name : "Unnamed channel",
                channel->enabled ? (active ? 0x5EEAD4 : 0xF4F7FB) :
                    0x8EA0AE);
            if (name) {
                messages_set_dot_width(name, 184);
                lv_obj_set_pos(name, 0, 2);
            } else {
                complete = false;
            }
            char state[80];
            snprintf(state, sizeof(state), "%s%s%lu unread",
                     active ? "active | " : "",
                     channel->enabled ? "" : "disabled | ",
                     (unsigned long)channel->unread_count);
            lv_obj_t *meta = messages_create_label(row, state, 0x8EA0AE);
            if (meta) {
                messages_set_dot_width(meta, 108);
                lv_obj_set_pos(meta, 190, 2);
            } else {
                complete = false;
            }
        }
    }
    if (!complete) {
        messages_deactivate_actions(controller);
        lv_obj_clean(sheet);
        d1l_ui_modal_hide(sheet);
    }
    return complete;
}

bool d1l_ui_messages_channel_selector_active(
    const d1l_ui_messages_controller_t *controller)
{
    return controller && messages_object_is_valid(controller->channel_sheet) &&
        d1l_ui_modal_visible(controller->channel_sheet);
}

lv_obj_t *d1l_ui_messages_channel_selector_sheet(
    const d1l_ui_messages_controller_t *controller)
{
    return controller && messages_object_is_valid(controller->channel_sheet) ?
        controller->channel_sheet : NULL;
}

void d1l_ui_messages_hide_channel_selector(
    d1l_ui_messages_controller_t *controller)
{
    if (!controller) {
        return;
    }
    if (d1l_ui_messages_channel_selector_active(controller)) {
        messages_deactivate_actions(controller);
    }
    if (messages_object_is_valid(controller->channel_sheet)) {
        d1l_ui_modal_hide(controller->channel_sheet);
    }
}

static d1l_ui_messages_action_binding_t *messages_bind_thread_control(
    d1l_ui_messages_controller_t *controller,
    size_t index,
    d1l_ui_messages_action_t action)
{
    if (!controller || index >= D1L_UI_MESSAGES_THREAD_CONTROL_BINDING_COUNT) {
        return NULL;
    }
    d1l_ui_messages_action_binding_t *binding =
        &controller->thread_controls[index];
    *binding = (d1l_ui_messages_action_binding_t) {
        .controller = controller,
        .action = action,
        .row_index = 0U,
        .generation = controller->generation,
    };
    return binding;
}

static lv_obj_t *messages_create_thread_body(lv_obj_t *sheet)
{
    if (!messages_object_is_valid(sheet)) {
        return NULL;
    }
    lv_obj_t *body = lv_obj_create(sheet);
    if (!body) {
        return NULL;
    }
    lv_obj_set_size(body, 448, 292);
    lv_obj_set_pos(body, 16, 60);
    lv_obj_set_style_radius(body, 8, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x071018), 0);
    lv_obj_set_style_border_color(body, lv_color_hex(0x263241), 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return body;
}

static lv_obj_t *messages_create_wrapped_label(lv_obj_t *parent,
                                                const char *text,
                                                uint32_t color)
{
    lv_obj_t *label = messages_create_label(parent, text ? text : "", color);
    if (!label) {
        return NULL;
    }
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static bool messages_thread_entry_is_expanded(
    const d1l_ui_messages_controller_t *controller,
    const d1l_dm_entry_t *entry)
{
    if (!controller || !entry || !controller->expanded_row_valid) {
        return false;
    }
    if (entry->direction[0] == 't' && entry->delivery_session_id != 0U) {
        return controller->expanded_delivery_session_id ==
            entry->delivery_session_id;
    }
    return controller->expanded_delivery_session_id == 0U &&
        controller->expanded_row_seq == entry->seq;
}

static bool messages_delivery_failed_or_interrupted(
    d1l_dm_delivery_state_t state)
{
    return state == D1L_DM_DELIVERY_FAILED_RADIO ||
        state == D1L_DM_DELIVERY_FAILED_TIMEOUT ||
        state == D1L_DM_DELIVERY_FAILED_QUEUE ||
        state == D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT;
}

static bool messages_render_thread_bubble(
    d1l_ui_messages_controller_t *controller,
    lv_obj_t *body,
    size_t index)
{
    if (!messages_object_is_valid(body) ||
        !d1l_ui_messages_thread_row_index_valid(controller, index)) {
        return false;
    }
    const d1l_dm_entry_t *entry = &controller->thread_entries[index];
    const bool outgoing = entry->direction[0] == 't';
    const bool unread = !outgoing && controller->thread_unread[index];
    const bool expanded = messages_thread_entry_is_expanded(controller, entry);
    lv_obj_t *row = lv_obj_create(body);
    if (!row) {
        return false;
    }
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        row, outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bubble = lv_obj_create(row);
    if (!bubble) {
        return false;
    }
    lv_obj_set_width(bubble, 344);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 10, 0);
    lv_obj_set_style_bg_color(
        bubble, lv_color_hex(outgoing ? 0x19263A : 0x122D2A), 0);
    lv_obj_set_style_border_color(
        bubble, lv_color_hex(outgoing ? 0x3B5B86 : 0x28635A), 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_pad_all(bubble, 9, 0);
    lv_obj_set_style_pad_row(bubble, 4, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);

    d1l_ui_messages_action_binding_t *binding = &controller->thread_rows[index];
    *binding = (d1l_ui_messages_action_binding_t) {
        .controller = controller,
        .action = D1L_UI_MESSAGES_ACTION_TOGGLE_DM_DETAILS,
        .row_index = index,
        .generation = controller->generation,
    };
    lv_obj_add_event_cb(
        bubble, messages_dispatch_event_cb, LV_EVENT_CLICKED, binding);

    const char *who = outgoing ? "You" :
        (controller->thread_alias[0] ? controller->thread_alias :
         controller->thread_fingerprint);
    char heading[160];
    snprintf(heading, sizeof(heading), "%s  |  %s  %s",
             who, d1l_ui_messages_delivery_label(entry, unread),
             expanded ? "v" : ">");
    if (!messages_create_wrapped_label(
            bubble, heading, unread ? 0xFBBF24 :
            (outgoing ? 0xC4B5FD : 0xA7F3D0))) {
        return false;
    }
    if (!messages_create_wrapped_label(
            bubble, entry->text[0] ? entry->text : "-", 0xF4F7FB)) {
        return false;
    }
    if (!expanded) {
        return true;
    }

    char snr[16];
    messages_format_snr(snr, sizeof(snr), entry->snr_tenths);
    char technical[256];
    snprintf(technical, sizeof(technical),
             "state %s | reason %s | error %d | revision %lu | session %llu",
             d1l_dm_delivery_state_name(entry->delivery_state),
             d1l_dm_delivery_reason_name(entry->delivery_reason),
             (int)entry->delivery_last_error,
             (unsigned long)entry->delivery_revision,
             (unsigned long long)entry->delivery_session_id);
    if (!messages_create_wrapped_label(bubble, technical, 0x8EA0AE)) {
        return false;
    }
    if (outgoing) {
        snprintf(technical, sizeof(technical),
                 "seq %lu | attempt %u | retries %u | expected ACK %08lX | "
                 "rssi %d | snr %s | path bytes %u | hops %u",
                 (unsigned long)entry->seq,
                 (unsigned)entry->attempt,
                 (unsigned)entry->delivery_retry_count,
                 (unsigned long)entry->ack_hash,
                 entry->rssi_dbm, snr,
                 (unsigned)entry->path_hash_bytes,
                 (unsigned)entry->path_hops);
    } else {
        snprintf(technical, sizeof(technical),
                 "seq %lu | ACK dispatch %s | count %u | error %d | hash %08lX | "
                 "identity %s | rssi %d | snr %s | path bytes %u | hops %u",
                 (unsigned long)entry->seq,
                 d1l_dm_ack_state_name(entry->ack_state),
                 (unsigned)entry->ack_dispatch_count,
                 (int)entry->ack_last_error,
                 (unsigned long)entry->ack_hash,
                 entry->identity_digest_valid ? "retained" : "legacy",
                 entry->rssi_dbm, snr,
                 (unsigned)entry->path_hash_bytes,
                 (unsigned)entry->path_hops);
    }
    if (!messages_create_wrapped_label(bubble, technical, 0x8EA0AE)) {
        return false;
    }
    if (outgoing && d1l_ui_messages_delivery_retry_active(
            entry->delivery_state)) {
        if (!messages_create_wrapped_label(
                bubble,
                "Delivery retry is in progress. You do not need to resend.",
                0x93C5FD)) {
            return false;
        }
    } else if (outgoing && messages_delivery_failed_or_interrupted(
                   entry->delivery_state)) {
        if (!messages_create_wrapped_label(
                bubble,
                "Delivery failed. Your message is saved; replying sends a new message.",
                0xFBBF24)) {
            return false;
        }
    }
    return true;
}

bool d1l_ui_messages_select_thread(d1l_ui_messages_controller_t *controller,
                                   const char *fingerprint,
                                   const char *alias)
{
    if (!controller || !fingerprint || fingerprint[0] == '\0' ||
        !messages_text_fits(
            fingerprint, sizeof(controller->thread_fingerprint))) {
        return false;
    }
    const char *display = alias && alias[0] ? alias : fingerprint;
    if (!messages_text_fits(display, sizeof(controller->thread_alias))) {
        return false;
    }
    snprintf(controller->thread_fingerprint,
             sizeof(controller->thread_fingerprint), "%s", fingerprint);
    snprintf(controller->thread_alias,
             sizeof(controller->thread_alias), "%s", display);
    controller->thread_limit = D1L_UI_MESSAGES_THREAD_INITIAL_ROWS;
    controller->thread_row_count = 0U;
    controller->thread_total_matches = 0U;
    controller->thread_search_text[0] = '\0';
    controller->expanded_delivery_session_id = 0U;
    controller->expanded_row_seq = 0U;
    controller->expanded_row_valid = false;
    memset(controller->thread_entries, 0, sizeof(controller->thread_entries));
    memset(controller->thread_unread, 0, sizeof(controller->thread_unread));
    return true;
}

bool d1l_ui_messages_render_thread(
    d1l_ui_messages_controller_t *controller,
    d1l_ui_messages_thread_loader_t loader,
    void *loader_context,
    bool reply_available,
    d1l_ui_messages_action_handler_t action_handler,
    void *action_context)
{
    if (!controller || !messages_object_is_valid(controller->thread_sheet) ||
        !loader || !action_handler || controller->thread_fingerprint[0] == '\0') {
        return false;
    }
    if (controller->thread_limit < D1L_UI_MESSAGES_THREAD_INITIAL_ROWS) {
        controller->thread_limit = D1L_UI_MESSAGES_THREAD_INITIAL_ROWS;
    }
    if (controller->thread_limit > D1L_UI_MESSAGES_THREAD_MAX_ROWS) {
        controller->thread_limit = D1L_UI_MESSAGES_THREAD_MAX_ROWS;
    }
    messages_begin_actions(controller, action_handler, action_context);
    controller->thread_total_matches = 0U;
    controller->thread_row_count = loader(
        controller->thread_fingerprint,
        controller->thread_entries,
        controller->thread_unread,
        controller->thread_limit,
        0U,
        controller->thread_search_text,
        &controller->thread_total_matches,
        loader_context);
    if (controller->thread_row_count > controller->thread_limit) {
        controller->thread_row_count = controller->thread_limit;
    }
    if (controller->thread_row_count > D1L_UI_MESSAGES_THREAD_MAX_ROWS) {
        controller->thread_row_count = D1L_UI_MESSAGES_THREAD_MAX_ROWS;
    }

    lv_obj_t *sheet = controller->thread_sheet;
    lv_obj_clean(sheet);
    bool complete = true;
    complete = messages_create_button(
        sheet, "Back", 12, 6, 72, 44,
        messages_bind_thread_control(
            controller, 0U, D1L_UI_MESSAGES_ACTION_CLOSE_DM_THREAD)) != NULL &&
        complete;
    lv_obj_t *title = messages_create_label(
        sheet, controller->thread_alias[0] ? controller->thread_alias :
        controller->thread_fingerprint, 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        messages_set_dot_width(title, 260);
        lv_obj_set_pos(title, 100, 10);
    } else {
        complete = false;
    }
    lv_obj_t *body = messages_create_thread_body(sheet);
    if (!body) {
        complete = false;
    } else {
        (void)messages_render_store_notice(
            body, 0, controller->rendered.dm_store_state);
        if (!reply_available) {
            (void)messages_render_notice(
                body, 0,
                "This contact is unavailable. Saved messages remain readable.",
                0xFBBF24);
        }
        char meta_text[96];
        if (controller->thread_search_text[0]) {
            snprintf(meta_text, sizeof(meta_text),
                     "Showing %u of %u | search active",
                     (unsigned)controller->thread_row_count,
                     (unsigned)controller->thread_total_matches);
        } else {
            snprintf(meta_text, sizeof(meta_text), "Showing %u of %u",
                     (unsigned)controller->thread_row_count,
                     (unsigned)controller->thread_total_matches);
        }
        lv_obj_t *meta = messages_create_wrapped_label(body, meta_text, 0x8EA0AE);
        complete = meta != NULL && complete;
        if (controller->thread_row_count < controller->thread_total_matches &&
            controller->thread_limit < D1L_UI_MESSAGES_THREAD_MAX_ROWS) {
            lv_obj_t *older = messages_create_button(
                body, "Load Older", 0, 0, 424, 48,
                messages_bind_thread_control(
                    controller, 1U,
                    D1L_UI_MESSAGES_ACTION_LOAD_OLDER_DM_THREAD));
            if (older) {
                lv_obj_set_width(older, LV_PCT(100));
            } else {
                complete = false;
            }
        }
        if (controller->thread_row_count == 0U) {
            complete = messages_create_wrapped_label(
                body, controller->rendered.dm_store_state ==
                        D1L_UI_MESSAGES_STORE_LOADING ?
                    "Loading saved messages..." :
                controller->thread_search_text[0] ?
                    "No saved messages match this search." :
                    "No saved messages in this conversation.",
                0x8EA0AE) != NULL && complete;
        }
        for (size_t i = 0U; i < controller->thread_row_count; ++i) {
            complete = messages_render_thread_bubble(controller, body, i) &&
                complete;
        }
        if (controller->thread_row_count > 0U) {
            lv_obj_update_layout(body);
            if (controller->rendered.dm_store_state ==
                    D1L_UI_MESSAGES_STORE_READY &&
                reply_available) {
                lv_obj_scroll_to_y(body, LV_COORD_MAX, LV_ANIM_OFF);
            } else {
                lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
            }
        }
    }
    complete = messages_create_button(
        sheet, "Search", 376, 6, 88, 44,
        messages_bind_thread_control(
            controller, 3U,
            D1L_UI_MESSAGES_ACTION_OPEN_DM_SEARCH)) != NULL &&
        complete;
    lv_obj_t *reply = reply_available ?
        messages_create_button(
            sheet, "Message this contact                         >",
            16, 360, 448, 52,
            messages_bind_thread_control(
                controller, 2U, D1L_UI_MESSAGES_ACTION_REPLY_DM_THREAD)) :
        messages_create_button(
            sheet, "Contact unavailable", 16, 360, 448, 52, NULL);
    if (reply && !reply_available) {
        lv_obj_add_state(reply, LV_STATE_DISABLED);
    }
    complete = reply != NULL && complete;
    if (!complete) {
        messages_deactivate_actions(controller);
        lv_obj_clean(sheet);
        d1l_ui_modal_hide(sheet);
    }
    return complete;
}

bool d1l_ui_messages_expand_thread(d1l_ui_messages_controller_t *controller)
{
    if (!controller || controller->thread_fingerprint[0] == '\0' ||
        controller->thread_limit >= D1L_UI_MESSAGES_THREAD_MAX_ROWS ||
        controller->thread_row_count >= controller->thread_total_matches) {
        return false;
    }
    controller->thread_limit += D1L_UI_MESSAGES_THREAD_LOAD_OLDER_STEP;
    if (controller->thread_limit > D1L_UI_MESSAGES_THREAD_MAX_ROWS) {
        controller->thread_limit = D1L_UI_MESSAGES_THREAD_MAX_ROWS;
    }
    return true;
}

bool d1l_ui_messages_set_thread_search(
    d1l_ui_messages_controller_t *controller,
    const char *query)
{
    if (!controller || controller->thread_fingerprint[0] == '\0') {
        return false;
    }
    const char *source = query ? query : "";
    size_t length = 0U;
    while (length < sizeof(controller->thread_search_text) &&
           source[length] != '\0') {
        length++;
    }
    if (length >= sizeof(controller->thread_search_text)) {
        return false;
    }
    memcpy(controller->thread_search_text, source, length + 1U);
    controller->thread_limit = D1L_UI_MESSAGES_THREAD_INITIAL_ROWS;
    controller->thread_row_count = 0U;
    controller->thread_total_matches = 0U;
    controller->expanded_delivery_session_id = 0U;
    controller->expanded_row_seq = 0U;
    controller->expanded_row_valid = false;
    memset(controller->thread_entries, 0, sizeof(controller->thread_entries));
    memset(controller->thread_unread, 0, sizeof(controller->thread_unread));
    return true;
}

const char *d1l_ui_messages_thread_search(
    const d1l_ui_messages_controller_t *controller)
{
    return controller && controller->thread_fingerprint[0] != '\0' ?
        controller->thread_search_text : NULL;
}

bool d1l_ui_messages_toggle_thread_details(
    d1l_ui_messages_controller_t *controller,
    const d1l_dm_entry_t *entry)
{
    if (!controller || !entry || controller->thread_fingerprint[0] == '\0') {
        return false;
    }
    const uint64_t session_id =
        entry->direction[0] == 't' ? entry->delivery_session_id : 0U;
    const uint32_t row_seq = entry->seq;
    const bool already_expanded = controller->expanded_row_valid &&
        (session_id != 0U ?
            controller->expanded_delivery_session_id == session_id :
            controller->expanded_delivery_session_id == 0U &&
            controller->expanded_row_seq == row_seq);
    controller->expanded_delivery_session_id = session_id;
    controller->expanded_row_seq = row_seq;
    controller->expanded_row_valid = !already_expanded;
    return true;
}

bool d1l_ui_messages_thread_active(
    const d1l_ui_messages_controller_t *controller)
{
    return controller && controller->thread_fingerprint[0] != '\0' &&
        messages_object_is_valid(controller->thread_sheet);
}

const char *d1l_ui_messages_thread_fingerprint(
    const d1l_ui_messages_controller_t *controller)
{
    return d1l_ui_messages_thread_active(controller) ?
        controller->thread_fingerprint : NULL;
}

lv_obj_t *d1l_ui_messages_thread_sheet(
    const d1l_ui_messages_controller_t *controller)
{
    return controller && messages_object_is_valid(controller->thread_sheet) ?
        controller->thread_sheet : NULL;
}

void d1l_ui_messages_hide_thread(d1l_ui_messages_controller_t *controller)
{
    if (!controller) {
        return;
    }
    if (controller->thread_fingerprint[0] != '\0') {
        messages_deactivate_actions(controller);
    }
    if (messages_object_is_valid(controller->thread_sheet)) {
        d1l_ui_modal_hide(controller->thread_sheet);
    }
    controller->thread_fingerprint[0] = '\0';
    controller->thread_alias[0] = '\0';
    controller->thread_search_text[0] = '\0';
    controller->thread_limit = D1L_UI_MESSAGES_THREAD_INITIAL_ROWS;
    controller->thread_row_count = 0U;
    controller->thread_total_matches = 0U;
    controller->expanded_delivery_session_id = 0U;
    controller->expanded_row_seq = 0U;
    controller->expanded_row_valid = false;
    memset(controller->thread_entries, 0, sizeof(controller->thread_entries));
    memset(controller->thread_unread, 0, sizeof(controller->thread_unread));
}

void d1l_ui_messages_deactivate(d1l_ui_messages_controller_t *controller)
{
    if (!controller) {
        return;
    }
    const bool thread_was_active = controller->thread_fingerprint[0] != '\0';
    const bool channel_was_active =
        d1l_ui_messages_channel_selector_active(controller);
    d1l_ui_messages_hide_channel_selector(controller);
    d1l_ui_messages_hide_thread(controller);
    if (!thread_was_active && !channel_was_active) {
        messages_deactivate_actions(controller);
    }
    memset(&controller->rendered, 0, sizeof(controller->rendered));
}
