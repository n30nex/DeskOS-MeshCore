#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *frame;
    lv_obj_t *screen;
    lv_obj_t *nodes[3];
    lv_obj_t *links[3];
    lv_obj_t *sparks[3];
    lv_obj_t *stars[8];
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *status;
    lv_obj_t *progress;
    lv_timer_t *timer;
    uint32_t started_tick;
    bool complete;
} d1l_ui_boot_scene_t;

bool d1l_ui_boot_scene_create(d1l_ui_boot_scene_t *scene, lv_obj_t *parent);
bool d1l_ui_boot_scene_visible(const d1l_ui_boot_scene_t *scene);
