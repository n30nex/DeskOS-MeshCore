#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app/app_model.h"

typedef struct _lv_obj_t lv_obj_t;

#define D1L_UI_DEVICE_SHEETS_BINDING_COUNT 9U
#define D1L_UI_DEVICE_SHEETS_CONTROLLER_MAX_BYTES 192U

typedef enum {
    D1L_UI_DEVICE_SHEETS_ACTION_NONE = 0,
    D1L_UI_DEVICE_SHEETS_ACTION_CLOSE_DISPLAY,
    D1L_UI_DEVICE_SHEETS_ACTION_BRIGHTNESS,
    D1L_UI_DEVICE_SHEETS_ACTION_NIGHT_MODE,
    D1L_UI_DEVICE_SHEETS_ACTION_HIGH_CONTRAST,
    D1L_UI_DEVICE_SHEETS_ACTION_TIMEOUT,
    D1L_UI_DEVICE_SHEETS_ACTION_TIMEZONE_MINUS,
    D1L_UI_DEVICE_SHEETS_ACTION_TIMEZONE_PLUS,
    D1L_UI_DEVICE_SHEETS_ACTION_CLOSE_DIAGNOSTICS,
    D1L_UI_DEVICE_SHEETS_ACTION_OPEN_TERMINAL,
} d1l_ui_device_sheets_action_t;

typedef void (*d1l_ui_device_sheets_action_handler_t)(
    d1l_ui_device_sheets_action_t action,
    void *context);

struct d1l_ui_device_sheets_controller;

typedef struct {
    struct d1l_ui_device_sheets_controller *controller;
    d1l_ui_device_sheets_action_t action;
    uint32_t generation;
} d1l_ui_device_sheets_binding_t;

typedef struct d1l_ui_device_sheets_controller {
    lv_obj_t *display_sheet;
    lv_obj_t *diagnostics_sheet;
    d1l_ui_device_sheets_action_handler_t action_handler;
    void *action_context;
    d1l_ui_device_sheets_binding_t
        bindings[D1L_UI_DEVICE_SHEETS_BINDING_COUNT];
    uint32_t generation;
} d1l_ui_device_sheets_controller_t;

bool d1l_ui_device_sheets_create(
    d1l_ui_device_sheets_controller_t *controller,
    lv_obj_t *parent);
bool d1l_ui_device_sheets_render_display(
    d1l_ui_device_sheets_controller_t *controller,
    const d1l_app_snapshot_t *snapshot,
    d1l_ui_device_sheets_action_handler_t action_handler,
    void *action_context);
bool d1l_ui_device_sheets_render_diagnostics(
    d1l_ui_device_sheets_controller_t *controller,
    const d1l_app_snapshot_t *snapshot,
    d1l_ui_device_sheets_action_handler_t action_handler,
    void *action_context);
void d1l_ui_device_sheets_hide_display(
    d1l_ui_device_sheets_controller_t *controller);
void d1l_ui_device_sheets_hide_diagnostics(
    d1l_ui_device_sheets_controller_t *controller);
lv_obj_t *d1l_ui_device_sheets_display(
    const d1l_ui_device_sheets_controller_t *controller);
lv_obj_t *d1l_ui_device_sheets_diagnostics(
    const d1l_ui_device_sheets_controller_t *controller);
