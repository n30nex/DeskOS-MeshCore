#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui_more_view.h"

typedef struct _lv_obj_t lv_obj_t;

#define D1L_UI_SETTINGS_CONTROLLER_MAX_BYTES 4096U

typedef void (*d1l_ui_settings_action_handler_t)(d1l_ui_settings_action_t action,
                                                 void *context);

struct d1l_ui_settings_controller;

typedef struct {
    struct d1l_ui_settings_controller *controller;
    uint32_t generation;
    d1l_ui_more_category_t category;
    size_t item_index;
    d1l_ui_settings_action_t action;
} d1l_ui_settings_action_binding_t;

typedef struct {
    struct d1l_ui_settings_controller *controller;
    uint32_t generation;
    d1l_ui_more_category_t category;
} d1l_ui_settings_category_binding_t;

typedef struct d1l_ui_settings_controller {
    d1l_ui_more_view_model_t rendered;
    d1l_ui_settings_action_handler_t action_handler;
    void *action_context;
    uint32_t generation;
    bool active;
    d1l_ui_more_category_t expanded_category;
    d1l_ui_settings_action_binding_t
        action_bindings[D1L_UI_MORE_CATEGORY_COUNT][D1L_UI_MORE_MAX_ITEMS_PER_CATEGORY];
    d1l_ui_settings_category_binding_t category_bindings[D1L_UI_MORE_CATEGORY_COUNT];
    lv_obj_t *category_children[D1L_UI_MORE_CATEGORY_COUNT];
    lv_obj_t *category_chevrons[D1L_UI_MORE_CATEGORY_COUNT];
    lv_obj_t *menu;
} d1l_ui_settings_controller_t;

bool d1l_ui_settings_action_available(d1l_ui_settings_action_t action);
bool d1l_ui_settings_render(d1l_ui_settings_controller_t *controller,
                            lv_obj_t *parent,
                            const d1l_ui_more_view_model_t *view_model,
                            uint32_t generation,
                            d1l_ui_settings_action_handler_t action_handler,
                            void *action_context);
void d1l_ui_settings_deactivate(d1l_ui_settings_controller_t *controller);
