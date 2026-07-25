#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_connectivity.h"

typedef struct _lv_obj_t lv_obj_t;

#define D1L_UI_WIFI_BINDING_COUNT 10U
#define D1L_UI_WIFI_CONTROLLER_MAX_BYTES 1024U

typedef enum {
    D1L_UI_WIFI_ACTION_NONE = 0,
    D1L_UI_WIFI_ACTION_CLOSE,
    D1L_UI_WIFI_ACTION_SAVE,
    D1L_UI_WIFI_ACTION_CLEAR,
    D1L_UI_WIFI_ACTION_SCAN,
    D1L_UI_WIFI_ACTION_CONNECT,
    D1L_UI_WIFI_ACTION_NEXT_PROFILE,
    D1L_UI_WIFI_ACTION_TOGGLE,
} d1l_ui_wifi_action_t;

typedef void (*d1l_ui_wifi_action_handler_t)(d1l_ui_wifi_action_t action,
                                             const char *ssid,
                                             const char *password,
                                             void *context);

struct d1l_ui_wifi_controller;

typedef struct {
    struct d1l_ui_wifi_controller *controller;
    d1l_ui_wifi_action_t action;
    uint32_t generation;
} d1l_ui_wifi_binding_t;

typedef struct d1l_ui_wifi_controller {
    lv_obj_t *sheet;
    lv_obj_t *ssid_textarea;
    lv_obj_t *password_textarea;
    lv_obj_t *keyboard;
    d1l_ui_wifi_view_model_t rendered;
    d1l_ui_wifi_action_handler_t action_handler;
    void *action_context;
    d1l_ui_wifi_binding_t bindings[D1L_UI_WIFI_BINDING_COUNT];
    uint32_t generation;
} d1l_ui_wifi_controller_t;

bool d1l_ui_wifi_create(d1l_ui_wifi_controller_t *controller, lv_obj_t *parent);
bool d1l_ui_wifi_render(d1l_ui_wifi_controller_t *controller,
                        const d1l_ui_wifi_view_model_t *view_model,
                        d1l_ui_wifi_action_handler_t action_handler,
                        void *action_context);
void d1l_ui_wifi_deactivate(d1l_ui_wifi_controller_t *controller);
lv_obj_t *d1l_ui_wifi_sheet(const d1l_ui_wifi_controller_t *controller);
lv_obj_t *d1l_ui_wifi_ssid_textarea(
    const d1l_ui_wifi_controller_t *controller);
lv_obj_t *d1l_ui_wifi_password_textarea(
    const d1l_ui_wifi_controller_t *controller);
lv_obj_t *d1l_ui_wifi_keyboard(const d1l_ui_wifi_controller_t *controller);
