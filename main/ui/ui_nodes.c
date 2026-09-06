#include "ui_nodes.h"
#include "ui_nodes_model.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define NODES_ROW_X 16
#define NODES_ROW_WIDTH 448
#define NODES_ROW_HEIGHT 58
#define NODES_ROW_GAP 4
#define NODES_MIN_TOUCH_HEIGHT 44
#define NODES_MAX_RENDERED_ROWS D1L_UI_NODES_PAGE_SIZE

_Static_assert(D1L_UI_NODES_ROW_CAPACITY >= NODES_MAX_RENDERED_ROWS,
               "Contacts row query must cover the visible list");

static lv_obj_t *nodes_create_label(lv_obj_t *parent,
                                    const char *text,
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

static void nodes_set_dot_width(lv_obj_t *label, lv_coord_t width)
{
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
}

static lv_obj_t *nodes_create_panel(lv_obj_t *parent,
                                    int x,
                                    int y,
                                    int width,
                                    int height)
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
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x20262B), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x33404A), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t *nodes_create_button(lv_obj_t *parent,
                                     const char *text,
                                     int x,
                                     int y,
                                     int width,
                                     int height,
                                     uint32_t accent,
                                     bool enabled,
                                     lv_event_cb_t callback,
                                     void *user_data)
{
    if (!parent || !text || height < NODES_MIN_TOUCH_HEIGHT) {
        return NULL;
    }
    lv_obj_t *button = lv_btn_create(parent);
    if (!button) {
        return NULL;
    }
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(
        button, lv_color_hex(accent == 0xF87171 ? 0x2A1118 : 0x252D33), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2E3A43), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(
        button, lv_color_hex(accent == 0xF87171 ? 0x7F1D1D : 0x34566A), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_t *label = nodes_create_label(
        button, text, enabled ? accent : 0x667787);
    if (label) {
        nodes_set_dot_width(label, width - 12);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }
    if (enabled && callback) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    return button;
}

static const char *nodes_role_badge_text(const char *role)
{
    if (!role || role[0] == '\0') {
        return "Node";
    }
    if (strcmp(role, "room") == 0) {
        return "Room";
    }
    if (strcmp(role, "repeater") == 0) {
        return "Repeater";
    }
    if (strcmp(role, "sensor") == 0) {
        return "Sensor";
    }
    if (strcmp(role, "companion") == 0 || strcmp(role, "chat") == 0) {
        return "Chat";
    }
    return "Node";
}

static bool nodes_role_is_managed_service(const char *role)
{
    return role &&
        (strcmp(role, "repeater") == 0 || strcmp(role, "room") == 0);
}

static const char *nodes_sort_label(d1l_node_sort_t sort)
{
    switch (sort) {
    case D1L_NODE_SORT_NAME:
        return "Sort: A-Z";
    case D1L_NODE_SORT_ROLE:
        return "Sort: Role";
    case D1L_NODE_SORT_SIGNAL:
        return "Sort: Signal";
    case D1L_NODE_SORT_FAVORITE:
        return "Sort: Favorites";
    case D1L_NODE_SORT_LAST_HEARD:
    default:
        return "Sort: Recent";
    }
}

static const char *nodes_role_avatar_text(const char *role)
{
    const char *label = nodes_role_badge_text(role);
    if (strcmp(label, "Room") == 0) {
        return "RM";
    }
    if (strcmp(label, "Repeater") == 0) {
        return "R";
    }
    if (strcmp(label, "Sensor") == 0) {
        return "S";
    }
    if (strcmp(label, "Chat") == 0) {
        return "C";
    }
    return "?";
}

static uint32_t nodes_role_color(const char *role)
{
    const char *label = nodes_role_badge_text(role);
    if (strcmp(label, "Room") == 0) {
        return 0x84FF2E;
    }
    if (strcmp(label, "Repeater") == 0) {
        return 0xFBBF24;
    }
    if (strcmp(label, "Sensor") == 0) {
        return 0x7D93FF;
    }
    if (strcmp(label, "Chat") == 0) {
        return 0x20D9ED;
    }
    return 0x4D7FFF;
}

static lv_obj_t *nodes_render_role_avatar(lv_obj_t *parent,
                                          const char *role,
                                          int x,
                                          int y)
{
    lv_obj_t *avatar = nodes_create_panel(parent, x, y, 40, 40);
    if (!avatar) {
        return NULL;
    }
    const uint32_t accent = nodes_role_color(role);
    lv_obj_set_style_radius(avatar, 20, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(0x10202A), 0);
    lv_obj_set_style_border_color(avatar, lv_color_hex(accent), 0);
    lv_obj_t *label = nodes_create_label(
        avatar, nodes_role_avatar_text(role), accent);
    if (label) {
        lv_obj_center(label);
    }
    return avatar;
}

static const char *nodes_contact_route_label(
    const d1l_contact_entry_t *entry)
{
    if (!entry || !entry->out_path_valid) {
        return "Broadcast";
    }
    return entry->path_hops == 0U ? "Direct" : "Saved path";
}

static void nodes_node_route_label(const d1l_node_view_t *view,
                                   char *dest,
                                   size_t dest_size)
{
    if (!dest || dest_size == 0U) {
        return;
    }
    if (!view || !view->reachable) {
        snprintf(dest, dest_size, "%s", "Not heard");
    } else if (view->node.path_hops == 0U) {
        snprintf(dest, dest_size, "%s", "Direct");
    } else {
        snprintf(dest, dest_size, "%u hop%s",
                 (unsigned)view->node.path_hops,
                 view->node.path_hops == 1U ? "" : "s");
    }
}



static void nodes_dispatch_global_event_cb(lv_event_t *event)
{
    d1l_ui_nodes_action_binding_t *binding = event ?
        (d1l_ui_nodes_action_binding_t *)lv_event_get_user_data(event) : NULL;
    if (!binding || !binding->controller ||
        !binding->controller->action_handler) {
        return;
    }
    d1l_ui_nodes_action_t action = D1L_UI_NODES_ACTION_CLEAR_HEARD;
    if (binding == &binding->controller->open_search) {
        action = D1L_UI_NODES_ACTION_OPEN_SEARCH;
    } else if (binding == &binding->controller->cycle_sort) {
        action = D1L_UI_NODES_ACTION_CYCLE_SORT;
    } else if (binding == &binding->controller->find_nearby) {
        action = D1L_UI_NODES_ACTION_FIND_NEARBY;
    } else if (binding == &binding->controller->show_saved) {
        action = D1L_UI_NODES_ACTION_SHOW_SAVED;
    } else if (binding == &binding->controller->show_discovered) {
        action = D1L_UI_NODES_ACTION_SHOW_DISCOVERED;
    } else if (binding == &binding->controller->cycle_filter) {
        action = D1L_UI_NODES_ACTION_CYCLE_FILTER;
    } else if (binding == &binding->controller->previous_page) {
        action = D1L_UI_NODES_ACTION_PREVIOUS_PAGE;
    } else if (binding == &binding->controller->next_page) {
        action = D1L_UI_NODES_ACTION_NEXT_PAGE;
    } else if (binding != &binding->controller->clear_heard) {
        return;
    }
    const d1l_ui_nodes_action_event_t action_event = {
        .action = action,
        .contact = NULL,
        .node = NULL,
    };
    binding->controller->action_handler(
        &action_event, binding->controller->action_context);
}

static void nodes_dispatch_contact_event(
    d1l_ui_nodes_action_binding_t *binding,
    d1l_ui_nodes_action_t action)
{
    if (!binding || !binding->controller) {
        return;
    }
    d1l_ui_nodes_controller_t *controller = binding->controller;
    if (!controller->action_handler ||
        binding->row_index >= controller->rendered.contact_row_count) {
        return;
    }
    if (action == D1L_UI_NODES_ACTION_OPEN_CONTACT_DM &&
        !controller->rendered.contact_can_dm[binding->row_index]) {
        return;
    }
    if (action == D1L_UI_NODES_ACTION_OPEN_CONTACT_ADMIN &&
        !d1l_contact_store_can_admin(
            &controller->rendered.contact_rows[binding->row_index])) {
        return;
    }
    const d1l_ui_nodes_action_event_t action_event = {
        .action = action,
        .contact = &controller->rendered.contact_rows[binding->row_index],
        .node = NULL,
    };
    controller->action_handler(&action_event, controller->action_context);
}

static void nodes_dispatch_contact_open_event_cb(lv_event_t *event)
{
    nodes_dispatch_contact_event(
        event ? (d1l_ui_nodes_action_binding_t *)lv_event_get_user_data(event) :
                NULL,
        D1L_UI_NODES_ACTION_OPEN_CONTACT);
}

static void nodes_dispatch_contact_dm_event_cb(lv_event_t *event)
{
    nodes_dispatch_contact_event(
        event ? (d1l_ui_nodes_action_binding_t *)lv_event_get_user_data(event) :
                NULL,
        D1L_UI_NODES_ACTION_OPEN_CONTACT_DM);
}

static void nodes_dispatch_contact_admin_event_cb(lv_event_t *event)
{
    nodes_dispatch_contact_event(
        event ? (d1l_ui_nodes_action_binding_t *)lv_event_get_user_data(event) :
                NULL,
        D1L_UI_NODES_ACTION_OPEN_CONTACT_ADMIN);
}

static void nodes_dispatch_node_event(
    d1l_ui_nodes_action_binding_t *binding,
    d1l_ui_nodes_action_t action)
{
    if (!binding || !binding->controller) {
        return;
    }
    d1l_ui_nodes_controller_t *controller = binding->controller;
    if (!controller->action_handler ||
        binding->row_index >= controller->rendered.node_row_count) {
        return;
    }
    if (action == D1L_UI_NODES_ACTION_OPEN_NODE_DM &&
        !controller->rendered.node_can_dm[binding->row_index]) {
        return;
    }
    if (action == D1L_UI_NODES_ACTION_OPEN_NODE_ADMIN) {
        const d1l_node_view_t *view =
            &controller->rendered.node_rows[binding->row_index];
        if (!view->keyed || !nodes_role_is_managed_service(view->role)) {
            return;
        }
    }
    const d1l_ui_nodes_action_event_t action_event = {
        .action = action,
        .contact = NULL,
        .node = &controller->rendered.node_rows[binding->row_index],
    };
    controller->action_handler(&action_event, controller->action_context);
}

static void nodes_dispatch_node_open_event_cb(lv_event_t *event)
{
    nodes_dispatch_node_event(
        event ? (d1l_ui_nodes_action_binding_t *)lv_event_get_user_data(event) :
                NULL,
        D1L_UI_NODES_ACTION_OPEN_NODE);
}

static void nodes_dispatch_node_dm_event_cb(lv_event_t *event)
{
    nodes_dispatch_node_event(
        event ? (d1l_ui_nodes_action_binding_t *)lv_event_get_user_data(event) :
                NULL,
        D1L_UI_NODES_ACTION_OPEN_NODE_DM);
}

static void nodes_dispatch_node_admin_event_cb(lv_event_t *event)
{
    nodes_dispatch_node_event(
        event ? (d1l_ui_nodes_action_binding_t *)lv_event_get_user_data(event) :
                NULL,
        D1L_UI_NODES_ACTION_OPEN_NODE_ADMIN);
}

static void nodes_render_page_controls(d1l_ui_nodes_controller_t *controller,
                                       lv_obj_t *parent, int y)
{
    const d1l_ui_nodes_view_model_t *view = &controller->rendered;
    char summary[64];
    const size_t end = view->page_offset + D1L_UI_NODES_PAGE_SIZE;
    snprintf(summary, sizeof(summary), "%u-%u of %u",
             (unsigned)(view->total_matches ? view->page_offset + 1U : 0U),
             (unsigned)(end < view->total_matches ? end : view->total_matches),
             (unsigned)view->total_matches);
    lv_obj_t *range = nodes_create_label(parent, summary, 0xA6B0B7);
    if (range) {
        lv_obj_set_size(range, 252, 24);
        lv_obj_set_style_text_align(range, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(range, 112, y + 12);
    }
    nodes_create_button(parent, "Previous", 16, y, 88, 44, 0x20D9ED,
                        view->page_offset > 0U,
                        nodes_dispatch_global_event_cb, &controller->previous_page);
    nodes_create_button(parent, "Next", 376, y, 88, 44, 0x20D9ED,
                        end < view->total_matches,
                        nodes_dispatch_global_event_cb, &controller->next_page);
}

static void nodes_render_header(d1l_ui_nodes_controller_t *controller,
                                lv_obj_t *parent)
{
    const d1l_ui_nodes_view_model_t *view = &controller->rendered;
    lv_obj_t *title = nodes_create_label(parent, "Contacts", 0xF4F7FB);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_pos(title, 16, 6);
    }
    char summary[64];
    snprintf(summary, sizeof(summary), "%u saved | %u heard",
             (unsigned)view->contact_count, (unsigned)view->heard_count);
    lv_obj_t *meta = nodes_create_label(parent, summary, 0xA6B0B7);
    nodes_set_dot_width(meta, 252);
    if (meta) lv_obj_set_pos(meta, 16, 34);

    d1l_ui_nodes_action_binding_t *bindings[] = {
        &controller->find_nearby, &controller->clear_heard,
        &controller->open_search, &controller->cycle_sort,
        &controller->show_saved, &controller->show_discovered,
        &controller->cycle_filter, &controller->previous_page,
        &controller->next_page,
    };
    for (size_t i = 0U; i < sizeof(bindings) / sizeof(bindings[0]); ++i) {
        *bindings[i] = (d1l_ui_nodes_action_binding_t){.controller = controller};
    }
    nodes_create_button(parent, "Find", 282, 4, 70, 44, 0x84FF2E, true,
                        nodes_dispatch_global_event_cb, &controller->find_nearby);
    nodes_create_button(parent, "Clear heard", 360, 4, 104, 44, 0xF87171,
                        view->heard_count > 0U,
                        nodes_dispatch_global_event_cb, &controller->clear_heard);
    nodes_create_button(parent, "Saved", 16, 54, 218, 44,
                        view->discovered ? 0xA6B0B7 : 0x20D9ED, true,
                        nodes_dispatch_global_event_cb, &controller->show_saved);
    nodes_create_button(parent, "Discovered", 242, 54, 222, 44,
                        view->discovered ? 0x20D9ED : 0xA6B0B7, true,
                        nodes_dispatch_global_event_cb, &controller->show_discovered);
    const char *search_label = view->search_text[0] ?
        view->search_text : "Search contacts";
    nodes_create_button(parent, search_label, 16, 106, 180, 44, 0x4D7FFF, true,
                        nodes_dispatch_global_event_cb, &controller->open_search);
    nodes_create_button(parent, d1l_ui_nodes_filter_label(view->filter),
                        204, 106, 116, 44, 0x20D9ED, true,
                        nodes_dispatch_global_event_cb, &controller->cycle_filter);
    nodes_create_button(parent, nodes_sort_label(view->sort),
                        328, 106, 136, 44, 0x7D93FF, true,
                        nodes_dispatch_global_event_cb, &controller->cycle_sort);
    nodes_render_page_controls(controller, parent, 158);
}

static void nodes_render_contact_row(
    d1l_ui_nodes_controller_t *controller,
    lv_obj_t *parent,
    int y,
    size_t index)
{
    if (!controller || !parent ||
        index >= controller->rendered.contact_row_count) {
        return;
    }
    const d1l_contact_entry_t *entry =
        &controller->rendered.contact_rows[index];
    const bool can_dm = controller->rendered.contact_can_dm[index];
    const bool can_manage = d1l_contact_store_can_admin(entry);
    const bool has_side_action = can_manage || can_dm;
    lv_obj_t *row = nodes_create_panel(
        parent, NODES_ROW_X, y, NODES_ROW_WIDTH, NODES_ROW_HEIGHT);
    if (!row) {
        return;
    }
    d1l_ui_nodes_action_binding_t *binding = &controller->contact_rows[index];
    *binding = (d1l_ui_nodes_action_binding_t) {
        .controller = controller,
        .row_index = index,
    };
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        row, nodes_dispatch_contact_open_event_cb, LV_EVENT_CLICKED, binding);

    nodes_render_role_avatar(row, entry->type, 8, 9);
    const char *display_name = entry->alias[0] ? entry->alias :
        (entry->heard_name[0] ? entry->heard_name : "Saved contact");
    lv_obj_t *name = nodes_create_label(
        row, display_name,
        entry->muted ? 0xA6B0B7 : (entry->favorite ? 0xFBBF24 : 0xF4F7FB));
    nodes_set_dot_width(name, has_side_action ? 246 : 320);
    if (name) {
        lv_obj_set_pos(name, 58, 7);
    }
    char meta[144];
    snprintf(meta, sizeof(meta), "%s | %s%s",
             nodes_role_badge_text(entry->type),
             nodes_contact_route_label(entry),
             entry->muted ? " | Muted" :
             (entry->favorite ? " | Favorite" : ""));
    lv_obj_t *details = nodes_create_label(row, meta, 0xA6B0B7);
    nodes_set_dot_width(details, has_side_action ? 266 : 344);
    if (details) {
        lv_obj_set_pos(details, 58, 31);
    }
    if (can_manage) {
        nodes_create_button(row, "Login", 340, 7, 84, 44, 0xFBBF24, true,
                            nodes_dispatch_contact_admin_event_cb, binding);
    } else if (can_dm) {
        nodes_create_button(row, "Chat", 340, 7, 84, 44, 0x84FF2E, true,
                            nodes_dispatch_contact_dm_event_cb, binding);
    } else {
        lv_obj_t *chevron = nodes_create_label(row, ">", 0xA6B0B7);
        if (chevron) {
            lv_obj_set_pos(chevron, 426, 20);
        }
    }
}

static void nodes_render_node_row(d1l_ui_nodes_controller_t *controller,
                                  lv_obj_t *parent,
                                  int y,
                                  size_t index)
{
    if (!controller || !parent ||
        index >= controller->rendered.node_row_count) {
        return;
    }
    const d1l_node_view_t *view = &controller->rendered.node_rows[index];
    const d1l_node_entry_t *entry = &view->node;
    const bool can_dm = controller->rendered.node_can_dm[index];
    const bool can_manage = view->keyed && nodes_role_is_managed_service(view->role);
    const bool has_side_action = can_manage || can_dm;
    lv_obj_t *row = nodes_create_panel(
        parent, NODES_ROW_X, y, NODES_ROW_WIDTH, NODES_ROW_HEIGHT);
    if (!row) {
        return;
    }
    d1l_ui_nodes_action_binding_t *binding = &controller->node_rows[index];
    *binding = (d1l_ui_nodes_action_binding_t) {
        .controller = controller,
        .row_index = index,
    };
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        row, nodes_dispatch_node_open_event_cb, LV_EVENT_CLICKED, binding);

    nodes_render_role_avatar(row, view->role, 8, 9);
    const char *display_name = view->display_name[0] ? view->display_name :
        (entry->name[0] ? entry->name : "Nearby node");
    lv_obj_t *name = nodes_create_label(
        row, display_name, view->muted ? 0xA6B0B7 :
        (view->favorite ? 0xFBBF24 : 0xF4F7FB));
    nodes_set_dot_width(name, has_side_action ? 246 : 320);
    if (name) {
        lv_obj_set_pos(name, 58, 7);
    }
    char route[32];
    nodes_node_route_label(view, route, sizeof(route));
    char meta[144];
    snprintf(meta, sizeof(meta), "%s | %s%s",
             nodes_role_badge_text(view->role), route,
             view->muted ? " | Muted" :
             (view->favorite ? " | Favorite" : ""));
    lv_obj_t *details = nodes_create_label(row, meta, 0xA6B0B7);
    nodes_set_dot_width(details, has_side_action ? 266 : 344);
    if (details) {
        lv_obj_set_pos(details, 58, 31);
    }
    if (can_manage) {
        nodes_create_button(row, "Login", 340, 7, 84, 44, 0xFBBF24, true,
                            nodes_dispatch_node_admin_event_cb, binding);
    } else if (can_dm) {
        nodes_create_button(row, "Chat", 340, 7, 84, 44, 0x84FF2E, true,
                            nodes_dispatch_node_dm_event_cb, binding);
    } else {
        lv_obj_t *chevron = nodes_create_label(row, ">", 0xA6B0B7);
        if (chevron) {
            lv_obj_set_pos(chevron, 426, 20);
        }
    }
}

static void nodes_render_empty_state(d1l_ui_nodes_controller_t *controller,
                                     lv_obj_t *parent)
{
    const bool filtered = controller->rendered.search_text[0] != '\0' ||
        controller->rendered.filter != D1L_NODE_FILTER_ALL;
    lv_obj_t *panel = nodes_create_panel(parent, 16, 212, 448, 160);
    if (!panel) return;
    lv_obj_t *title = nodes_create_label(panel,
        filtered ? "No matches" : (controller->rendered.discovered ?
        "No new nodes" : "No saved contacts yet"), 0xF4F7FB);
    if (title) lv_obj_set_pos(title, 16, 20);
    lv_obj_t *copy = nodes_create_label(panel, filtered ?
        "Try another search or role filter." :
        "Open Discovered to browse nodes heard by this device.", 0xA6B0B7);
    if (copy) {
        lv_obj_set_width(copy, 408);
        lv_label_set_long_mode(copy, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(copy, 16, 52);
    }
}

void d1l_ui_nodes_render(d1l_ui_nodes_controller_t *controller,
                         lv_obj_t *parent,
                         const d1l_ui_nodes_view_model_t *view_model,
                         d1l_ui_nodes_action_handler_t action_handler,
                         void *action_context)
{
    if (!controller || !parent || !view_model || !action_handler) {
        return;
    }
    if (view_model != &controller->rendered) {
        controller->rendered = *view_model;
    }
    if (controller->rendered.contact_row_count >
        D1L_CONTACT_STORE_CAPACITY) {
        controller->rendered.contact_row_count =
            D1L_CONTACT_STORE_CAPACITY;
    }
    if (controller->rendered.node_row_count > D1L_UI_NODES_ROW_CAPACITY) {
        controller->rendered.node_row_count = D1L_UI_NODES_ROW_CAPACITY;
    }
    memset(controller->contact_rows, 0, sizeof(controller->contact_rows));
    memset(controller->node_rows, 0, sizeof(controller->node_rows));
    controller->action_handler = action_handler;
    controller->action_context = action_context;

    nodes_render_header(controller, parent);
    if (controller->rendered.total_matches == 0U) {
        nodes_render_empty_state(controller, parent);
        return;
    }
    int y = 212;
    const size_t start = controller->rendered.discovered ? 0U :
        controller->rendered.page_offset;
    const size_t count = controller->rendered.discovered ?
        controller->rendered.node_row_count : controller->rendered.contact_row_count;
    for (size_t i = start; i < count && i - start < NODES_MAX_RENDERED_ROWS; ++i) {
        if (controller->rendered.discovered) {
            nodes_render_node_row(controller, parent, y, i);
        } else {
            nodes_render_contact_row(controller, parent, y, i);
        }
        y += NODES_ROW_HEIGHT + NODES_ROW_GAP;
    }
    if (y > 432) {
        nodes_render_page_controls(controller, parent, y + 8);
    }
}

void d1l_ui_nodes_deactivate(d1l_ui_nodes_controller_t *controller)
{
    if (!controller) {
        return;
    }
    memset(&controller->rendered, 0, sizeof(controller->rendered));
    memset(controller->contact_rows, 0, sizeof(controller->contact_rows));
    memset(controller->node_rows, 0, sizeof(controller->node_rows));
    memset(&controller->open_search, 0, sizeof(controller->open_search));
    memset(&controller->cycle_sort, 0, sizeof(controller->cycle_sort));
    memset(&controller->find_nearby, 0, sizeof(controller->find_nearby));
    memset(&controller->clear_heard, 0, sizeof(controller->clear_heard));
    memset(&controller->show_saved, 0, sizeof(controller->show_saved));
    memset(&controller->show_discovered, 0, sizeof(controller->show_discovered));
    memset(&controller->cycle_filter, 0, sizeof(controller->cycle_filter));
    memset(&controller->previous_page, 0, sizeof(controller->previous_page));
    memset(&controller->next_page, 0, sizeof(controller->next_page));
    controller->action_handler = NULL;
    controller->action_context = NULL;
}
