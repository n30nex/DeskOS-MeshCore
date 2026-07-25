#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mesh/node_store.h"
#include "ui_dm_identity.h"

typedef struct _lv_obj_t lv_obj_t;

#define D1L_UI_NODE_DETAIL_BINDING_COUNT 4U
#define D1L_UI_NODE_DETAIL_CONTROLLER_MAX_BYTES 512U

typedef struct {
    d1l_node_view_t node;
    d1l_ui_dm_identity_reason_t dm_reason;
    bool dm_can_open_compose;
    bool management_gated;
    bool return_to_map;
} d1l_ui_node_detail_view_model_t;

typedef enum {
    D1L_UI_NODE_DETAIL_ACTION_NONE = 0,
    D1L_UI_NODE_DETAIL_ACTION_CLOSE,
    D1L_UI_NODE_DETAIL_ACTION_OPEN_DM,
    D1L_UI_NODE_DETAIL_ACTION_EXPLAIN_DM,
    D1L_UI_NODE_DETAIL_ACTION_OPEN_ADMIN,
} d1l_ui_node_detail_action_t;

typedef struct {
    d1l_ui_node_detail_action_t action;
    const d1l_node_view_t *node;
    bool return_to_map;
} d1l_ui_node_detail_action_event_t;

typedef void (*d1l_ui_node_detail_action_handler_t)(
    const d1l_ui_node_detail_action_event_t *event,
    void *context);

struct d1l_ui_node_detail_controller;

typedef struct {
    struct d1l_ui_node_detail_controller *controller;
    d1l_ui_node_detail_action_t action;
    uint32_t generation;
} d1l_ui_node_detail_binding_t;

typedef struct d1l_ui_node_detail_controller {
    lv_obj_t *sheet;
    d1l_ui_node_detail_view_model_t rendered;
    d1l_ui_node_detail_action_handler_t action_handler;
    void *action_context;
    d1l_ui_node_detail_binding_t bindings[D1L_UI_NODE_DETAIL_BINDING_COUNT];
    uint32_t generation;
} d1l_ui_node_detail_controller_t;

bool d1l_ui_node_detail_build_view_model(
    const d1l_node_view_t *node,
    d1l_ui_dm_identity_eligibility_t dm_eligibility,
    bool return_to_map,
    d1l_ui_node_detail_view_model_t *out_view_model);
bool d1l_ui_node_detail_create(d1l_ui_node_detail_controller_t *controller,
                               lv_obj_t *parent);
bool d1l_ui_node_detail_render(
    d1l_ui_node_detail_controller_t *controller,
    const d1l_ui_node_detail_view_model_t *view_model,
    d1l_ui_node_detail_action_handler_t action_handler,
    void *action_context);
void d1l_ui_node_detail_deactivate(
    d1l_ui_node_detail_controller_t *controller);
lv_obj_t *d1l_ui_node_detail_sheet(
    const d1l_ui_node_detail_controller_t *controller);
