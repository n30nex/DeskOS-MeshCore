#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_connectivity.h"

typedef struct _lv_obj_t lv_obj_t;

#define D1L_UI_BLE_BINDING_COUNT 4U
#define D1L_UI_BLE_CONTROLLER_MAX_BYTES 640U

typedef enum {
    D1L_UI_BLE_ACTION_NONE = 0,
    D1L_UI_BLE_ACTION_CLOSE,
    D1L_UI_BLE_ACTION_TOGGLE,
    D1L_UI_BLE_ACTION_PAIR,
    D1L_UI_BLE_ACTION_FORGET,
} d1l_ui_ble_action_t;

typedef void (*d1l_ui_ble_action_handler_t)(d1l_ui_ble_action_t action,
                                            void *context);

struct d1l_ui_ble_controller;

typedef struct {
    struct d1l_ui_ble_controller *controller;
    d1l_ui_ble_action_t action;
    uint32_t generation;
} d1l_ui_ble_binding_t;

typedef struct d1l_ui_ble_controller {
    lv_obj_t *sheet;
    d1l_ui_ble_view_model_t rendered;
    d1l_ui_ble_action_handler_t action_handler;
    void *action_context;
    d1l_ui_ble_binding_t bindings[D1L_UI_BLE_BINDING_COUNT];
    uint32_t generation;
} d1l_ui_ble_controller_t;

bool d1l_ui_ble_create(d1l_ui_ble_controller_t *controller, lv_obj_t *parent);
bool d1l_ui_ble_render(d1l_ui_ble_controller_t *controller,
                       const d1l_ui_ble_view_model_t *view_model,
                       d1l_ui_ble_action_handler_t action_handler,
                       void *action_context);
void d1l_ui_ble_deactivate(d1l_ui_ble_controller_t *controller);
lv_obj_t *d1l_ui_ble_sheet(const d1l_ui_ble_controller_t *controller);
