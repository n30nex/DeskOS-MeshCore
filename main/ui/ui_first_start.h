#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/app_model.h"

typedef struct _lv_obj_t lv_obj_t;

#define D1L_UI_FIRST_START_BINDING_COUNT 8U

typedef enum {
    D1L_UI_FIRST_START_READINESS = 0,
    D1L_UI_FIRST_START_NAME,
    D1L_UI_FIRST_START_LOCATION,
    D1L_UI_FIRST_START_WIFI,
    D1L_UI_FIRST_START_RADIO,
    D1L_UI_FIRST_START_STORAGE_MAP,
    D1L_UI_FIRST_START_CHANNELS,
    D1L_UI_FIRST_START_FINISHING,
    D1L_UI_FIRST_START_DONE,
} d1l_ui_first_start_stage_t;

typedef enum {
    D1L_UI_FIRST_START_ACTION_NONE = 0,
    D1L_UI_FIRST_START_ACTION_BACK,
    D1L_UI_FIRST_START_ACTION_NEXT,
    D1L_UI_FIRST_START_ACTION_SKIP,
    D1L_UI_FIRST_START_ACTION_FOCUS_PRIMARY,
    D1L_UI_FIRST_START_ACTION_FOCUS_SECONDARY,
    D1L_UI_FIRST_START_ACTION_KEYBOARD,
} d1l_ui_first_start_action_t;

struct d1l_ui_first_start_controller;

typedef struct {
    struct d1l_ui_first_start_controller *controller;
    d1l_ui_first_start_action_t action;
    uint32_t generation;
} d1l_ui_first_start_binding_t;

typedef struct d1l_ui_first_start_controller {
    lv_obj_t *overlay;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *progress_label;
    lv_obj_t *progress_bar;
    lv_obj_t *readiness_rows[5];
    lv_obj_t *media_line;
    lv_obj_t *status_line;
    lv_obj_t *primary_textarea;
    lv_obj_t *secondary_textarea;
    lv_obj_t *keyboard;
    lv_obj_t *back_button;
    lv_obj_t *skip_button;
    lv_obj_t *next_button;
    d1l_ui_first_start_binding_t
        bindings[D1L_UI_FIRST_START_BINDING_COUNT];
    d1l_ui_first_start_stage_t stage;
    uint32_t generation;
    uint32_t ready_since_tick;
    bool ready_hold_started;
    bool offline_selected;
    bool location_saved;
    bool wifi_saved;
    bool radio_confirmed;
    bool media_ready;
    bool finishing;
    char node_name[D1L_NODE_NAME_LEN];
    char latitude[20];
    char longitude[20];
    char wifi_ssid[D1L_WIFI_SSID_LEN];
} d1l_ui_first_start_controller_t;

bool d1l_ui_first_start_create(
    d1l_ui_first_start_controller_t *controller, lv_obj_t *parent);
void d1l_ui_first_start_update(
    d1l_ui_first_start_controller_t *controller,
    const d1l_app_snapshot_t *snapshot);
void d1l_ui_first_start_deactivate(
    d1l_ui_first_start_controller_t *controller);
bool d1l_ui_first_start_visible(
    const d1l_ui_first_start_controller_t *controller);
lv_obj_t *d1l_ui_first_start_overlay(
    const d1l_ui_first_start_controller_t *controller);

bool d1l_ui_first_start_essential_ready(
    const d1l_app_snapshot_t *snapshot);
bool d1l_ui_first_start_sd_prepared(
    const d1l_app_snapshot_t *snapshot);
bool d1l_ui_first_start_map_prepared(
    const d1l_app_snapshot_t *snapshot);
bool d1l_ui_first_start_parse_coordinate_e7(
    const char *text, int32_t minimum, int32_t maximum, int32_t *out_value);
