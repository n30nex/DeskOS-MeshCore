#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct _lv_obj_t lv_obj_t;

typedef struct {
    lv_obj_t *panel;
    lv_obj_t *icon;
    lv_obj_t *label;
    lv_obj_t *sender;
    lv_obj_t *message;
    lv_obj_t *footer;
    uint32_t groups_seen;
    uint32_t dms_seen;
    uint8_t kind;
    bool active;
    bool presentation;
} d1l_ui_receive_animation_t;

/* UI-owner only. Presentation text is cleared when the alert is dismissed. */
bool d1l_ui_receive_animation_create(d1l_ui_receive_animation_t *view,
    lv_obj_t *parent, uint32_t groups, uint32_t dms);
void d1l_ui_receive_animation_stop(d1l_ui_receive_animation_t *view);
void d1l_ui_receive_animation_destroy(d1l_ui_receive_animation_t *view);
void d1l_ui_receive_animation_set_presentation(d1l_ui_receive_animation_t *view, bool enabled);
void d1l_ui_receive_animation_set_message(d1l_ui_receive_animation_t *view,
    bool direct, const char *conversation, const char *sender, const char *text);
/* Consumes events even when disabled. Returns true for a fresh eligible event. */
bool d1l_ui_receive_animation_update(d1l_ui_receive_animation_t *view,
    uint32_t groups, uint32_t dms, bool enabled);
