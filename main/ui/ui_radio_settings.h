#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app/app_model.h"

typedef struct _lv_obj_t lv_obj_t;

#define D1L_UI_RADIO_SETTINGS_BINDING_COUNT 12U
#define D1L_UI_RADIO_SETTINGS_CONTROLLER_MAX_BYTES 320U

typedef enum {
    D1L_UI_RADIO_SETTINGS_ACTION_NONE = 0,
    D1L_UI_RADIO_SETTINGS_ACTION_FREQ_DOWN,
    D1L_UI_RADIO_SETTINGS_ACTION_FREQ_UP,
    D1L_UI_RADIO_SETTINGS_ACTION_BANDWIDTH,
    D1L_UI_RADIO_SETTINGS_ACTION_SF_DOWN,
    D1L_UI_RADIO_SETTINGS_ACTION_SF_UP,
    D1L_UI_RADIO_SETTINGS_ACTION_CODING_RATE,
    D1L_UI_RADIO_SETTINGS_ACTION_TX_DOWN,
    D1L_UI_RADIO_SETTINGS_ACTION_TX_UP,
    D1L_UI_RADIO_SETTINGS_ACTION_RX_BOOST,
    D1L_UI_RADIO_SETTINGS_ACTION_DEFAULTS,
    D1L_UI_RADIO_SETTINGS_ACTION_SAVE,
    D1L_UI_RADIO_SETTINGS_ACTION_CLOSE,
} d1l_ui_radio_settings_action_t;

typedef void (*d1l_ui_radio_settings_action_handler_t)(
    d1l_ui_radio_settings_action_t action,
    const d1l_app_radio_profile_edit_t *edit,
    void *context);

struct d1l_ui_radio_settings_controller;

typedef struct {
    struct d1l_ui_radio_settings_controller *controller;
    d1l_ui_radio_settings_action_t action;
    uint32_t generation;
} d1l_ui_radio_settings_binding_t;

typedef struct d1l_ui_radio_settings_controller {
    lv_obj_t *sheet;
    d1l_app_radio_profile_edit_t edit;
    d1l_ui_radio_settings_action_handler_t action_handler;
    void *action_context;
    d1l_ui_radio_settings_binding_t
        bindings[D1L_UI_RADIO_SETTINGS_BINDING_COUNT];
    uint32_t generation;
} d1l_ui_radio_settings_controller_t;

bool d1l_ui_radio_settings_create(
    d1l_ui_radio_settings_controller_t *controller,
    lv_obj_t *parent);
bool d1l_ui_radio_settings_set_edit(
    d1l_ui_radio_settings_controller_t *controller,
    const d1l_app_radio_profile_edit_t *edit);
const d1l_app_radio_profile_edit_t *d1l_ui_radio_settings_edit(
    const d1l_ui_radio_settings_controller_t *controller);
bool d1l_ui_radio_settings_render(
    d1l_ui_radio_settings_controller_t *controller,
    bool radio_applied,
    bool radio_apply_pending,
    d1l_ui_radio_settings_action_handler_t action_handler,
    void *action_context);
void d1l_ui_radio_settings_deactivate(
    d1l_ui_radio_settings_controller_t *controller);
lv_obj_t *d1l_ui_radio_settings_sheet(
    const d1l_ui_radio_settings_controller_t *controller);
