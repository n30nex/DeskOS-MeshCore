#include "ui_home.h"

#include <string.h>

#include "app/release_profile.h"
#include "lvgl.h"

static const d1l_ui_home_box_t k_destination_boxes[D1L_UI_HOME_DESTINATION_COUNT] = {
    [D1L_UI_HOME_DESTINATION_MESSAGES] = {12, 0, 222, 140},
    [D1L_UI_HOME_DESTINATION_NETWORK] = {246, 0, 222, 140},
    [D1L_UI_HOME_DESTINATION_MAP] = {12, 148, 222, 140},
    [D1L_UI_HOME_DESTINATION_MORE] = {246, 148, 222, 140},
};

static const d1l_ui_home_box_t k_device_box = {12, 296, 456, 88};

static const d1l_ui_home_box_t k_status_boxes[D1L_UI_HOME_STATUS_COUNT] = {
    [D1L_UI_HOME_STATUS_MESH] = {14, 298, 88, 84},
    [D1L_UI_HOME_STATUS_WIFI] = {105, 298, 88, 84},
    [D1L_UI_HOME_STATUS_BLE] = {196, 298, 88, 84},
    [D1L_UI_HOME_STATUS_SD] = {287, 298, 88, 84},
    [D1L_UI_HOME_STATUS_ATTENTION] = {378, 298, 88, 84},
};

static const d1l_ui_home_box_t k_compact_status_boxes[D1L_UI_HOME_STATUS_COUNT] = {
    [D1L_UI_HOME_STATUS_MESH] = {14, 298, 144, 84},
    [D1L_UI_HOME_STATUS_SD] = {160, 298, 144, 84},
    [D1L_UI_HOME_STATUS_ATTENTION] = {306, 298, 144, 84},
};


d1l_ui_home_box_t d1l_ui_home_destination_box(d1l_ui_home_destination_slot_t slot)
{
    const int index = (int)slot;
    if (index < 0 || index >= D1L_UI_HOME_DESTINATION_COUNT) {
        return (d1l_ui_home_box_t){0, 0, 0, 0};
    }
    return k_destination_boxes[index];
}

d1l_ui_home_box_t d1l_ui_home_device_box(void)
{
    return k_device_box;
}

d1l_ui_home_box_t d1l_ui_home_status_box(d1l_ui_home_status_slot_t slot)
{
    const int index = (int)slot;
    if (index < 0 || index >= D1L_UI_HOME_STATUS_COUNT) {
        return (d1l_ui_home_box_t){0, 0, 0, 0};
    }
    return k_status_boxes[index];
}

bool d1l_ui_home_action_available(d1l_ui_home_action_t action)
{
    switch (action) {
    case D1L_UI_HOME_ACTION_MESSAGES:
        return d1l_release_feature_available(
                   D1L_RELEASE_FEATURE_PUBLIC_MESSAGES) ||
               d1l_release_feature_available(
                   D1L_RELEASE_FEATURE_DIRECT_MESSAGES);
    case D1L_UI_HOME_ACTION_NETWORK:
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_NODES);
    case D1L_UI_HOME_ACTION_MAP:
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP);
    case D1L_UI_HOME_ACTION_PACKETS:
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_PACKETS);
    case D1L_UI_HOME_ACTION_RADIO:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_RADIO_SETTINGS);
    case D1L_UI_HOME_ACTION_WIFI:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL);
    case D1L_UI_HOME_ACTION_BLE:
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE);
    case D1L_UI_HOME_ACTION_STORAGE:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_RETAINED_NVS);
    case D1L_UI_HOME_ACTION_ATTENTION:
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_DIAGNOSTICS);
    case D1L_UI_HOME_ACTION_MORE:
        return true;
    case D1L_UI_HOME_ACTION_NONE:
    default:
        return false;
    }
}

static lv_obj_t *home_create_label(lv_obj_t *parent, const char *text, uint32_t color)
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

static void home_set_dot_width(lv_obj_t *label, lv_coord_t width)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
}

static lv_obj_t *home_create_panel(lv_obj_t *parent, d1l_ui_home_box_t box)
{
    if (!parent) {
        return NULL;
    }
    lv_obj_t *panel = lv_obj_create(parent);
    if (!panel) {
        return NULL;
    }
    lv_obj_set_size(panel, box.width, box.height);
    lv_obj_set_pos(panel, box.x, box.y);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0F1712), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static void home_action_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_home_action_binding_t *binding =
        (d1l_ui_home_action_binding_t *)lv_event_get_user_data(event);
    if (!binding || !binding->controller ||
        !binding->controller->action_handler) {
        return;
    }
    const d1l_ui_home_action_t action = binding->action;
    if (action > D1L_UI_HOME_ACTION_NONE &&
        action <= D1L_UI_HOME_ACTION_PACKETS &&
        d1l_ui_home_action_available(action)) {
        binding->controller->action_handler(
            action, binding->controller->action_context);
    }
}

static void home_bind_action(lv_obj_t *object,
                             d1l_ui_home_action_binding_t *binding,
                             d1l_ui_home_controller_t *controller,
                             d1l_ui_home_action_t action)
{
    if (!object || !binding || !controller || !controller->action_handler ||
        action <= D1L_UI_HOME_ACTION_NONE ||
        action > D1L_UI_HOME_ACTION_PACKETS ||
        !d1l_ui_home_action_available(action)) {
        return;
    }
    binding->controller = controller;
    binding->action = action;
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x17241D), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(object, lv_color_hex(0x5EEAD4), LV_STATE_FOCUSED);
    lv_obj_add_event_cb(object, home_action_event_cb, LV_EVENT_CLICKED, binding);
}

static void render_destination_card(lv_obj_t *parent,
                                    d1l_ui_home_destination_slot_t slot,
                                    const char *icon,
                                    const char *title,
                                    const char *detail,
                                    const char *status,
                                    uint32_t accent,
                                    d1l_ui_home_action_t action,
                                    d1l_ui_home_action_binding_t *binding,
                                    d1l_ui_home_controller_t *controller)
{
    lv_obj_t *card = home_create_panel(parent, d1l_ui_home_destination_box(slot));
    if (!card) {
        return;
    }
    lv_obj_set_style_border_color(card, lv_color_hex(0x1F372E), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    home_bind_action(card, binding, controller, action);

    lv_obj_t *icon_label = home_create_label(card, icon, accent);
    if (icon_label) {
        lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_24, 0);
        lv_obj_set_pos(icon_label, 14, 14);
    }

    lv_obj_t *title_label = home_create_label(card, title, 0xF4F7FB);
    if (title_label) {
        lv_obj_set_style_text_font(title_label, &lv_font_montserrat_24, 0);
        home_set_dot_width(title_label, 142);
        lv_obj_set_pos(title_label, 52, 16);
    }

    lv_obj_t *arrow_label = home_create_label(card, LV_SYMBOL_RIGHT, 0x8EA0AE);
    if (arrow_label) {
        lv_obj_set_pos(arrow_label, 192, 18);
    }

    lv_obj_t *detail_label = home_create_label(card, detail, 0xA7B4BE);
    if (detail_label) {
        lv_label_set_long_mode(detail_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(detail_label, 194, 40);
        lv_obj_set_pos(detail_label, 14, 57);
    }

    lv_obj_t *status_label = home_create_label(card, status, accent);
    if (status_label) {
        home_set_dot_width(status_label, 194);
        lv_obj_set_pos(status_label, 14, 108);
    }
}

static void render_status_item(lv_obj_t *card,
                               d1l_ui_home_status_slot_t slot,
                               const char *icon,
                               const char *label,
                               const char *value,
                               uint32_t color,
                               d1l_ui_home_action_t action,
                               d1l_ui_home_action_binding_t *binding,
                               d1l_ui_home_controller_t *controller)
{
    if (!card || !icon || !label || !value) {
        return;
    }
    const d1l_ui_home_box_t card_box = d1l_ui_home_device_box();
    const bool compact_status =
        !d1l_ui_home_action_available(D1L_UI_HOME_ACTION_WIFI) &&
        !d1l_ui_home_action_available(D1L_UI_HOME_ACTION_BLE);
    const d1l_ui_home_box_t box = compact_status ?
        k_compact_status_boxes[slot] : d1l_ui_home_status_box(slot);
    lv_obj_t *item = lv_obj_create(card);
    if (!item) {
        return;
    }
    lv_obj_set_size(item, box.width, box.height);
    lv_obj_set_pos(item, box.x - card_box.x, box.y - card_box.y);
    lv_obj_set_style_radius(item, 10, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    home_bind_action(item, binding, controller, action);
    lv_obj_set_style_bg_opa(item, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(item, 1, LV_STATE_FOCUSED);

    lv_obj_t *icon_label = home_create_label(item, icon, color);
    if (icon_label) {
        lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_24, 0);
        lv_obj_align(icon_label, LV_ALIGN_TOP_MID, 0, 4);
    }
    lv_obj_t *name_label = home_create_label(item, label, 0xA7B4BE);
    if (name_label) {
        home_set_dot_width(name_label, box.width - 8);
        lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(name_label, 4, 32);
    }
    lv_obj_t *value_label = home_create_label(item, value, color);
    if (value_label) {
        home_set_dot_width(value_label, box.width - 8);
        lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(value_label, 4, 55);
    }
}

static void render_device_status(lv_obj_t *parent,
                                 const d1l_ui_home_view_model_t *view_model,
                                 d1l_ui_home_action_binding_t *bindings,
                                 d1l_ui_home_controller_t *controller)
{
    lv_obj_t *card = home_create_panel(parent, d1l_ui_home_device_box());
    if (!card) {
        return;
    }
    lv_obj_set_style_border_color(card, lv_color_hex(0x28463A), 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    render_status_item(card, D1L_UI_HOME_STATUS_MESH, LV_SYMBOL_LOOP,
                       "Mesh", view_model->mesh_value,
                       view_model->mesh_value_color, D1L_UI_HOME_ACTION_RADIO,
                       &bindings[D1L_UI_HOME_STATUS_MESH], controller);
    if (d1l_ui_home_action_available(D1L_UI_HOME_ACTION_WIFI)) {
        render_status_item(card, D1L_UI_HOME_STATUS_WIFI, LV_SYMBOL_WIFI,
                           "Wi-Fi", view_model->wifi_value,
                           view_model->wifi_value_color, D1L_UI_HOME_ACTION_WIFI,
                           &bindings[D1L_UI_HOME_STATUS_WIFI], controller);
    }
    if (d1l_ui_home_action_available(D1L_UI_HOME_ACTION_BLE)) {
        render_status_item(card, D1L_UI_HOME_STATUS_BLE, LV_SYMBOL_BLUETOOTH,
                           "BLE", view_model->ble_value,
                           view_model->ble_value_color, D1L_UI_HOME_ACTION_BLE,
                           &bindings[D1L_UI_HOME_STATUS_BLE], controller);
    }
    const bool sd_history_available = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_SD_HISTORY);
    render_status_item(card, D1L_UI_HOME_STATUS_SD,
                       sd_history_available ? LV_SYMBOL_SD_CARD : LV_SYMBOL_SAVE,
                       sd_history_available ? "SD" : "Storage",
                       sd_history_available ?
                           view_model->sd_compact_value : "Internal",
                       view_model->sd_value_color, D1L_UI_HOME_ACTION_STORAGE,
                       &bindings[D1L_UI_HOME_STATUS_SD], controller);
    render_status_item(card, D1L_UI_HOME_STATUS_ATTENTION, LV_SYMBOL_WARNING,
                       "Attention", view_model->attention_value,
                       view_model->attention_value_color,
                       D1L_UI_HOME_ACTION_ATTENTION,
                       &bindings[D1L_UI_HOME_STATUS_ATTENTION], controller);
}

void d1l_ui_home_render(d1l_ui_home_controller_t *controller,
                        lv_obj_t *parent,
                        const d1l_ui_home_view_model_t *view_model,
                        d1l_ui_home_action_handler_t action_handler,
                        void *action_context)
{
    if (!controller || !parent || !view_model) {
        return;
    }
    controller->rendered = *view_model;
    controller->action_handler = action_handler;
    controller->action_context = action_context;
    memset(controller->bindings, 0, sizeof(controller->bindings));
    const bool map_available =
        d1l_ui_home_action_available(D1L_UI_HOME_ACTION_MAP);
    const char *third_title = map_available ? "Map" : "Packets";
    const char *third_detail = map_available ?
        "Saved center and a small local map area" :
        "Read-only packet log, search, and signal details";
    const char *third_status = map_available ?
        controller->rendered.map_status : controller->rendered.more_status;
    const uint32_t third_status_color = map_available ?
        controller->rendered.map_status_color :
        controller->rendered.more_status_color;
    const d1l_ui_home_action_t third_action = map_available ?
        D1L_UI_HOME_ACTION_MAP : D1L_UI_HOME_ACTION_PACKETS;
    const char *messages_detail =
        d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT) ?
        "Public, direct, and room conversations" :
        "Public and direct conversations";

    render_destination_card(parent, D1L_UI_HOME_DESTINATION_MESSAGES,
                            LV_SYMBOL_ENVELOPE, "Messages",
                            messages_detail,
                            controller->rendered.messages_status,
                            controller->rendered.messages_status_color,
                            D1L_UI_HOME_ACTION_MESSAGES,
                            &controller->bindings[D1L_UI_HOME_DESTINATION_MESSAGES],
                            controller);

    render_destination_card(parent, D1L_UI_HOME_DESTINATION_NETWORK,
                            LV_SYMBOL_LIST, "Nodes",
                            "Contacts, nearby nodes, and routing",
                            controller->rendered.network_status,
                            controller->rendered.network_status_color,
                            D1L_UI_HOME_ACTION_NETWORK,
                            &controller->bindings[D1L_UI_HOME_DESTINATION_NETWORK],
                            controller);

    render_destination_card(parent, D1L_UI_HOME_DESTINATION_MAP,
                            map_available ? LV_SYMBOL_IMAGE : LV_SYMBOL_LIST,
                            third_title, third_detail, third_status,
                            third_status_color, third_action,
                            &controller->bindings[D1L_UI_HOME_DESTINATION_MAP],
                            controller);

    render_destination_card(parent, D1L_UI_HOME_DESTINATION_MORE, LV_SYMBOL_SETTINGS,
                            map_available ? "Tools" : "Settings",
                            "Device settings, utilities, and support",
                            controller->rendered.more_status,
                            controller->rendered.more_status_color,
                            D1L_UI_HOME_ACTION_MORE,
                            &controller->bindings[D1L_UI_HOME_DESTINATION_MORE],
                            controller);

    render_device_status(parent, &controller->rendered,
                         &controller->bindings[D1L_UI_HOME_DESTINATION_COUNT],
                         controller);
}

void d1l_ui_home_deactivate(d1l_ui_home_controller_t *controller)
{
    if (!controller) {
        return;
    }
    controller->action_handler = NULL;
    controller->action_context = NULL;
    memset(controller->bindings, 0, sizeof(controller->bindings));
}
