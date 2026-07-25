#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ui_home_view.h"

typedef struct _lv_obj_t lv_obj_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
} d1l_ui_home_box_t;

typedef enum {
    D1L_UI_HOME_DESTINATION_MESSAGES = 0,
    D1L_UI_HOME_DESTINATION_NETWORK,
    D1L_UI_HOME_DESTINATION_MAP,
    D1L_UI_HOME_DESTINATION_MORE,
    D1L_UI_HOME_DESTINATION_COUNT,
} d1l_ui_home_destination_slot_t;

typedef enum {
    D1L_UI_HOME_STATUS_MESH = 0,
    D1L_UI_HOME_STATUS_WIFI,
    D1L_UI_HOME_STATUS_BLE,
    D1L_UI_HOME_STATUS_SD,
    D1L_UI_HOME_STATUS_ATTENTION,
    D1L_UI_HOME_STATUS_COUNT,
} d1l_ui_home_status_slot_t;

typedef enum {
    D1L_UI_HOME_ACTION_NONE = 0,
    D1L_UI_HOME_ACTION_MESSAGES,
    D1L_UI_HOME_ACTION_NETWORK,
    D1L_UI_HOME_ACTION_MAP,
    D1L_UI_HOME_ACTION_MORE,
    D1L_UI_HOME_ACTION_RADIO,
    D1L_UI_HOME_ACTION_WIFI,
    D1L_UI_HOME_ACTION_BLE,
    D1L_UI_HOME_ACTION_STORAGE,
    D1L_UI_HOME_ACTION_ATTENTION,
    D1L_UI_HOME_ACTION_PACKETS,
} d1l_ui_home_action_t;

typedef void (*d1l_ui_home_action_handler_t)(d1l_ui_home_action_t action,
                                             void *context);

struct d1l_ui_home_controller;

typedef struct {
    struct d1l_ui_home_controller *controller;
    d1l_ui_home_action_t action;
} d1l_ui_home_action_binding_t;

#define D1L_UI_HOME_ACTION_BINDING_COUNT \
    (D1L_UI_HOME_DESTINATION_COUNT + D1L_UI_HOME_STATUS_COUNT)

typedef struct d1l_ui_home_controller {
    d1l_ui_home_view_model_t rendered;
    d1l_ui_home_action_handler_t action_handler;
    void *action_context;
    d1l_ui_home_action_binding_t bindings[D1L_UI_HOME_ACTION_BINDING_COUNT];
} d1l_ui_home_controller_t;

d1l_ui_home_box_t d1l_ui_home_destination_box(d1l_ui_home_destination_slot_t slot);
d1l_ui_home_box_t d1l_ui_home_device_box(void);
d1l_ui_home_box_t d1l_ui_home_status_box(d1l_ui_home_status_slot_t slot);
bool d1l_ui_home_action_available(d1l_ui_home_action_t action);
void d1l_ui_home_render(d1l_ui_home_controller_t *controller,
                        lv_obj_t *parent,
                        const d1l_ui_home_view_model_t *view_model,
                        d1l_ui_home_action_handler_t action_handler,
                        void *action_context);
void d1l_ui_home_deactivate(d1l_ui_home_controller_t *controller);
