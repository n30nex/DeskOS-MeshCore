#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/app_model.h"
#include "ui_nodes_summary.h"

typedef struct _lv_obj_t lv_obj_t;

typedef struct {
    d1l_ui_node_role_counts_t role_counts;
    size_t contact_count;
    d1l_contact_entry_t contact_rows[D1L_CONTACT_STORE_CAPACITY];
    bool contact_can_dm[D1L_CONTACT_STORE_CAPACITY];
    size_t contact_row_count;
    d1l_node_view_t node_rows[D1L_NODE_STORE_CAPACITY];
    bool node_can_dm[D1L_NODE_STORE_CAPACITY];
    size_t node_row_count;
    char search_text[D1L_NODE_PUBLIC_KEY_HEX_LEN];
    d1l_node_sort_t sort;
} d1l_ui_nodes_view_model_t;

typedef enum {
    D1L_UI_NODES_ACTION_NONE = 0,
    D1L_UI_NODES_ACTION_OPEN_CONTACT,
    D1L_UI_NODES_ACTION_OPEN_CONTACT_DM,
    D1L_UI_NODES_ACTION_OPEN_NODE,
    D1L_UI_NODES_ACTION_OPEN_NODE_DM,
    D1L_UI_NODES_ACTION_OPEN_SEARCH,
    D1L_UI_NODES_ACTION_CYCLE_SORT,
    D1L_UI_NODES_ACTION_FIND_NEARBY,
    D1L_UI_NODES_ACTION_CLEAR_HEARD,
} d1l_ui_nodes_action_t;

typedef struct {
    d1l_ui_nodes_action_t action;
    const d1l_contact_entry_t *contact;
    const d1l_node_view_t *node;
} d1l_ui_nodes_action_event_t;

typedef void (*d1l_ui_nodes_action_handler_t)(
    const d1l_ui_nodes_action_event_t *event,
    void *context);

struct d1l_ui_nodes_controller;

typedef struct {
    struct d1l_ui_nodes_controller *controller;
    size_t row_index;
} d1l_ui_nodes_action_binding_t;

typedef struct d1l_ui_nodes_controller {
    d1l_ui_nodes_view_model_t rendered;
    d1l_ui_nodes_action_handler_t action_handler;
    void *action_context;
    d1l_ui_nodes_action_binding_t contact_rows[D1L_CONTACT_STORE_CAPACITY];
    d1l_ui_nodes_action_binding_t node_rows[D1L_NODE_STORE_CAPACITY];
    d1l_ui_nodes_action_binding_t open_search;
    d1l_ui_nodes_action_binding_t cycle_sort;
    d1l_ui_nodes_action_binding_t find_nearby;
    d1l_ui_nodes_action_binding_t clear_heard;
} d1l_ui_nodes_controller_t;

void d1l_ui_nodes_render(d1l_ui_nodes_controller_t *controller,
                         lv_obj_t *parent,
                         const d1l_ui_nodes_view_model_t *view_model,
                         d1l_ui_nodes_action_handler_t action_handler,
                         void *action_context);
void d1l_ui_nodes_deactivate(d1l_ui_nodes_controller_t *controller);
