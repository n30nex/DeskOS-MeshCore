#include "ui_node_detail.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"
#include "lvgl.h"
#include "ui_modal.h"

enum {
    BINDING_CLOSE = 0,
    BINDING_OPEN_DM,
    BINDING_EXPLAIN_DM,
    BINDING_OPEN_ADMIN,
};

_Static_assert(sizeof(d1l_ui_node_detail_controller_t) <=
                   D1L_UI_NODE_DETAIL_CONTROLLER_MAX_BYTES,
               "Node Detail controller exceeded its persistent-owner size budget");

static bool bounded_text_is_terminated(const char *text, size_t capacity)
{
    return text && capacity > 0U && memchr(text, '\0', capacity) != NULL;
}

static bool role_is_managed_service(const char *role)
{
    return role &&
        (strcmp(role, "room") == 0 || strcmp(role, "repeater") == 0);
}

static bool view_model_is_valid(
    const d1l_ui_node_detail_view_model_t *view_model)
{
    if (!view_model ||
        !bounded_text_is_terminated(view_model->node.node.fingerprint,
                                    sizeof(view_model->node.node.fingerprint)) ||
        !bounded_text_is_terminated(view_model->node.node.public_key_hex,
                                    sizeof(view_model->node.node.public_key_hex)) ||
        !bounded_text_is_terminated(view_model->node.node.name,
                                    sizeof(view_model->node.node.name)) ||
        !bounded_text_is_terminated(view_model->node.node.type,
                                    sizeof(view_model->node.node.type)) ||
        !bounded_text_is_terminated(view_model->node.display_name,
                                    sizeof(view_model->node.display_name)) ||
        !bounded_text_is_terminated(view_model->node.role,
                                    sizeof(view_model->node.role)) ||
        view_model->node.node.fingerprint[0] == '\0' ||
        view_model->dm_reason < D1L_UI_DM_IDENTITY_READY ||
        view_model->dm_reason > D1L_UI_DM_IDENTITY_ROLE_UNSUPPORTED) {
        return false;
    }
    return view_model->dm_can_open_compose ==
               (view_model->dm_reason == D1L_UI_DM_IDENTITY_READY) &&
        view_model->management_gated ==
               role_is_managed_service(view_model->node.role);
}

bool d1l_ui_node_detail_build_view_model(
    const d1l_node_view_t *node,
    d1l_ui_dm_identity_eligibility_t dm_eligibility,
    bool return_to_map,
    d1l_ui_node_detail_view_model_t *out_view_model)
{
    if (!out_view_model) {
        return false;
    }
    memset(out_view_model, 0, sizeof(*out_view_model));
    if (!node || dm_eligibility.reason < D1L_UI_DM_IDENTITY_READY ||
        dm_eligibility.reason > D1L_UI_DM_IDENTITY_ROLE_UNSUPPORTED ||
        dm_eligibility.can_open_compose !=
            (dm_eligibility.reason == D1L_UI_DM_IDENTITY_READY)) {
        return false;
    }
    *out_view_model = (d1l_ui_node_detail_view_model_t) {
        .node = *node,
        .dm_reason = dm_eligibility.reason,
        .dm_can_open_compose = dm_eligibility.can_open_compose,
        .management_gated = role_is_managed_service(node->role),
        .return_to_map = return_to_map,
    };
    if (!view_model_is_valid(out_view_model)) {
        memset(out_view_model, 0, sizeof(*out_view_model));
        return false;
    }
    return true;
}

static void advance_generation(d1l_ui_node_detail_controller_t *controller)
{
    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
    }
}

static void deactivate_actions(d1l_ui_node_detail_controller_t *controller)
{
    if (!controller) {
        return;
    }
    advance_generation(controller);
    controller->action_handler = NULL;
    controller->action_context = NULL;
    memset(controller->bindings, 0, sizeof(controller->bindings));
}

static void invalidate_render(d1l_ui_node_detail_controller_t *controller)
{
    if (!controller) {
        return;
    }
    deactivate_actions(controller);
    memset(&controller->rendered, 0, sizeof(controller->rendered));
    if (controller->sheet && lv_obj_is_valid(controller->sheet)) {
        d1l_ui_modal_hide(controller->sheet);
        lv_obj_clean(controller->sheet);
    }
}

static bool binding_is_current(const d1l_ui_node_detail_binding_t *binding)
{
    return binding && binding->controller && binding->generation != 0U &&
        binding->generation == binding->controller->generation;
}

static void action_event_cb(lv_event_t *event)
{
    if (!event) {
        return;
    }
    d1l_ui_node_detail_binding_t *binding =
        (d1l_ui_node_detail_binding_t *)lv_event_get_user_data(event);
    if (!binding_is_current(binding) ||
        !binding->controller->action_handler ||
        binding->action <= D1L_UI_NODE_DETAIL_ACTION_NONE ||
        binding->action > D1L_UI_NODE_DETAIL_ACTION_OPEN_ADMIN) {
        return;
    }
    d1l_ui_node_detail_controller_t *controller = binding->controller;
    const d1l_ui_node_detail_action_event_t action_event = {
        .action = binding->action,
        .node = &controller->rendered.node,
        .return_to_map = controller->rendered.return_to_map,
    };
    controller->action_handler(&action_event, controller->action_context);
}

static d1l_ui_node_detail_binding_t *set_binding(
    d1l_ui_node_detail_controller_t *controller,
    size_t slot,
    d1l_ui_node_detail_action_t action)
{
    if (!controller || slot >= D1L_UI_NODE_DETAIL_BINDING_COUNT ||
        action <= D1L_UI_NODE_DETAIL_ACTION_NONE ||
        action > D1L_UI_NODE_DETAIL_ACTION_OPEN_ADMIN) {
        return NULL;
    }
    d1l_ui_node_detail_binding_t *binding = &controller->bindings[slot];
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

static void configure_dot_label(lv_obj_t *label, int width, int x, int y)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
    lv_obj_set_pos(label, x, y);
}

static lv_obj_t *create_button(d1l_ui_node_detail_controller_t *controller,
                               const char *text,
                               int x,
                               int y,
                               int width,
                               int height,
                               size_t binding_slot,
                               d1l_ui_node_detail_action_t action)
{
    if (!controller || !controller->sheet || !text) {
        return NULL;
    }
    lv_obj_t *button = lv_btn_create(controller->sheet);
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
    d1l_ui_node_detail_binding_t *binding =
        set_binding(controller, binding_slot, action);
    if (!label || !binding) {
        lv_obj_del(button);
        return NULL;
    }
    lv_obj_center(label);
    lv_obj_add_event_cb(button, action_event_cb, LV_EVENT_CLICKED, binding);
    return button;
}

static const char *role_badge_text(const char *role)
{
    if (!role || role[0] == '\0') {
        return "NODE";
    }
    if (strcmp(role, "room") == 0) {
        return "ROOM";
    }
    if (strcmp(role, "repeater") == 0) {
        return "RPT";
    }
    if (strcmp(role, "sensor") == 0) {
        return "SNS";
    }
    if (strcmp(role, "companion") == 0) {
        return "CMP";
    }
    return "NODE";
}

static const char *role_display_label(const char *role)
{
    if (!role || role[0] == '\0') {
        return "Node";
    }
    if (strcmp(role, "room") == 0) {
        return "Room Server";
    }
    if (strcmp(role, "repeater") == 0) {
        return "Repeater";
    }
    if (strcmp(role, "sensor") == 0) {
        return "Sensor";
    }
    if (strcmp(role, "companion") == 0) {
        return "Companion";
    }
    return role;
}

static uint32_t role_color(const char *role)
{
    if (!role || role[0] == '\0') {
        return 0x4D7FFF;
    }
    if (strcmp(role, "room") == 0) {
        return 0x84FF2E;
    }
    if (strcmp(role, "repeater") == 0) {
        return 0xFBBF24;
    }
    if (strcmp(role, "sensor") == 0) {
        return 0x7D93FF;
    }
    if (strcmp(role, "companion") == 0) {
        return 0x20D9ED;
    }
    return 0x4D7FFF;
}

static lv_obj_t *render_role_badge(lv_obj_t *parent, const char *role)
{
    lv_obj_t *badge = lv_obj_create(parent);
    if (!badge) {
        return NULL;
    }
    lv_obj_set_size(badge, 74, 24);
    lv_obj_set_pos(badge, 8, 78);
    lv_obj_set_style_radius(badge, 6, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x10202A), 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(role_color(role)), 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = create_label(badge, role_badge_text(role),
                                   role_color(role));
    if (!label) {
        lv_obj_del(badge);
        return NULL;
    }
    configure_dot_label(label, 68, 0, 0);
    lv_obj_center(label);
    return badge;
}

static void format_advert_coordinate(char *dest, size_t dest_len,
                                     int32_t value_e6)
{
    if (!dest || dest_len == 0U) {
        return;
    }
    const int64_t value = value_e6;
    const bool negative = value < 0;
    const uint64_t magnitude = (uint64_t)(negative ? -value : value);
    snprintf(dest, dest_len, "%s%llu.%06llu", negative ? "-" : "",
             (unsigned long long)(magnitude / 1000000ULL),
             (unsigned long long)(magnitude % 1000000ULL));
}

bool d1l_ui_node_detail_create(d1l_ui_node_detail_controller_t *controller,
                               lv_obj_t *parent)
{
    if (!controller) {
        return false;
    }
    if (controller->sheet && lv_obj_is_valid(controller->sheet)) {
        d1l_ui_modal_hide(controller->sheet);
        lv_obj_del(controller->sheet);
    }
    memset(controller, 0, sizeof(*controller));
    if (!parent || !lv_obj_is_valid(parent)) {
        return false;
    }
    controller->sheet = lv_obj_create(parent);
    if (!controller->sheet) {
        return false;
    }
    lv_obj_set_size(controller->sheet, 480, 480);
    lv_obj_set_pos(controller->sheet, 0, 0);
    lv_obj_set_style_radius(controller->sheet, 0, 0);
    lv_obj_set_style_bg_color(controller->sheet, lv_color_hex(0x20262B), 0);
    lv_obj_set_style_border_color(controller->sheet, lv_color_hex(0x3C4A54), 0);
    lv_obj_set_style_border_width(controller->sheet, 1, 0);
    lv_obj_set_style_pad_all(controller->sheet, 12, 0);
    lv_obj_clear_flag(controller->sheet, LV_OBJ_FLAG_SCROLLABLE);
    d1l_ui_modal_hide(controller->sheet);
    return true;
}

bool d1l_ui_node_detail_render(
    d1l_ui_node_detail_controller_t *controller,
    const d1l_ui_node_detail_view_model_t *view_model,
    d1l_ui_node_detail_action_handler_t action_handler,
    void *action_context)
{
    if (!controller) {
        return false;
    }
    if (!controller->sheet || !lv_obj_is_valid(controller->sheet) ||
        !action_handler || !view_model_is_valid(view_model)) {
        invalidate_render(controller);
        return false;
    }
    deactivate_actions(controller);
    controller->rendered = *view_model;
    controller->action_handler = action_handler;
    controller->action_context = action_context;
    lv_obj_clean(controller->sheet);

    const d1l_node_view_t *view = &controller->rendered.node;
    const d1l_node_entry_t *entry = &view->node;
    const char *name = view->display_name[0] ? view->display_name :
        (entry->name[0] ? entry->name : entry->fingerprint);
    bool complete = true;
    char line[192];

    lv_obj_t *title = create_label(controller->sheet, "Contact", 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        configure_dot_label(title, 160, 8, 4);
    } else {
        complete = false;
    }
    if (controller->rendered.dm_can_open_compose) {
        complete = create_button(controller, "DM", 208, 0, 54, 44,
                                 BINDING_OPEN_DM,
                                 D1L_UI_NODE_DETAIL_ACTION_OPEN_DM) != NULL &&
                   complete;
    } else {
        complete = create_button(controller, "Why no DM?", 174, 0, 120, 44,
                                 BINDING_EXPLAIN_DM,
                                 D1L_UI_NODE_DETAIL_ACTION_EXPLAIN_DM) != NULL &&
                   complete;
    }
    complete = create_button(controller, "Close", 316, 0, 76, 44,
                             BINDING_CLOSE,
                             D1L_UI_NODE_DETAIL_ACTION_CLOSE) != NULL && complete;

    lv_obj_t *name_label = create_label(controller->sheet, name, 0xF4F7FB);
    configure_dot_label(name_label, 392, 8, 48);
    complete = name_label != NULL && complete;

    complete = render_role_badge(controller->sheet, view->role) != NULL && complete;
    snprintf(line, sizeof(line), "Role %s", role_display_label(view->role));
    lv_obj_t *role = create_label(controller->sheet, line, role_color(view->role));
    configure_dot_label(role, 300, 94, 82);
    complete = role != NULL && complete;

    snprintf(line, sizeof(line), "%s route  |  %u hop%s  |  %s",
             entry->path_hops > 0U ? "Direct" : "Flood",
             (unsigned)entry->path_hops,
             entry->path_hops == 1U ? "" : "s",
             view->reachable ? "reachable" : "quiet");
    lv_obj_t *route = create_label(controller->sheet, line, 0x4D7FFF);
    configure_dot_label(route, 392, 8, 116);
    complete = route != NULL && complete;

    const int snr_abs = entry->snr_tenths < 0 ?
        -entry->snr_tenths : entry->snr_tenths;
    snprintf(line, sizeof(line), "Last signal %d dBm  |  SNR %s%d.%d",
             entry->rssi_dbm, entry->snr_tenths < 0 ? "-" : "",
             snr_abs / 10, snr_abs % 10);
    lv_obj_t *signal = create_label(controller->sheet, line, 0xA6B0B7);
    configure_dot_label(signal, 392, 8, 150);
    complete = signal != NULL && complete;

    if (d1l_release_feature_available(D1L_RELEASE_FEATURE_LOCATION)) {
        if (entry->location_valid) {
            char latitude[16];
            char longitude[16];
            format_advert_coordinate(latitude, sizeof(latitude), entry->lat_e6);
            format_advert_coordinate(longitude, sizeof(longitude), entry->lon_e6);
            snprintf(line, sizeof(line), "Shared location %s, %s", latitude,
                     longitude);
        } else {
            snprintf(line, sizeof(line), "Location not shared");
        }
        lv_obj_t *location = create_label(controller->sheet, line, 0x4D7FFF);
        configure_dot_label(location, 392, 8, 184);
        complete = location != NULL && complete;
    }

    snprintf(line, sizeof(line), "Heard on this boot  |  %lu sighting%s",
             (unsigned long)entry->heard_count,
             entry->heard_count == 1U ? "" : "s");
    lv_obj_t *heard = create_label(controller->sheet, line, 0xA6B0B7);
    configure_dot_label(heard, 392, 8, 218);
    complete = heard != NULL && complete;

    snprintf(line, sizeof(line), "Messaging %s: %s",
             controller->rendered.dm_can_open_compose ? "ready" : "unavailable",
             d1l_ui_dm_identity_reason_text(controller->rendered.dm_reason));
    lv_obj_t *dm_reason = create_label(
        controller->sheet, line,
        controller->rendered.dm_can_open_compose ? 0x20D9ED : 0xFBBF24);
    if (dm_reason) {
        lv_label_set_long_mode(dm_reason, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(dm_reason, 392, 54);
        lv_obj_set_pos(dm_reason, 8, 250);
    } else {
        complete = false;
    }
    snprintf(line, sizeof(line), "Advanced identity  %.16s  |  key %s",
             entry->fingerprint[0] ? entry->fingerprint : "-",
             view->keyed ? "saved" : "missing");
    lv_obj_t *identity = create_label(controller->sheet, line, 0xA6B0B7);
    configure_dot_label(identity, 392, 8, 314);
    complete = identity != NULL && complete;
    if (controller->rendered.management_gated &&
        d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN)) {
        complete = create_button(
            controller, "Login", 8, 352, 104, 44,
            BINDING_OPEN_ADMIN,
            D1L_UI_NODE_DETAIL_ACTION_OPEN_ADMIN) != NULL && complete;
        lv_obj_t *managed_reason = create_label(
            controller->sheet, "Sign in to open repeater or room controls.",
            0xA6B0B7);
        if (managed_reason) {
            lv_label_set_long_mode(managed_reason, LV_LABEL_LONG_DOT);
            lv_obj_set_size(managed_reason, 276, 40);
            lv_obj_set_pos(managed_reason, 120, 360);
        } else {
            complete = false;
        }
    }
    if (!complete) {
        invalidate_render(controller);
        return false;
    }
    return true;
}

void d1l_ui_node_detail_deactivate(
    d1l_ui_node_detail_controller_t *controller)
{
    invalidate_render(controller);
}

lv_obj_t *d1l_ui_node_detail_sheet(
    const d1l_ui_node_detail_controller_t *controller)
{
    if (!controller || !controller->sheet ||
        !lv_obj_is_valid(controller->sheet)) {
        return NULL;
    }
    return controller->sheet;
}
