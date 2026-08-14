#pragma once

#include <stdbool.h>
#include <stdint.h>

#define D1L_UI_BUTTON_DOUBLE_PRESS_MS 600U

typedef enum {
    D1L_UI_BUTTON_ACTION_NONE = 0,
    D1L_UI_BUTTON_ACTION_ACTIVITY,
    D1L_UI_BUTTON_ACTION_WAKE,
    D1L_UI_BUTTON_ACTION_ADVERT,
} d1l_ui_button_action_t;

typedef struct {
    bool was_pressed;
    bool first_press_pending;
    uint32_t deadline_ms;
} d1l_ui_button_gesture_t;

static inline d1l_ui_button_action_t d1l_ui_button_gesture_update(
    d1l_ui_button_gesture_t *gesture, bool pressed, bool display_locked,
    uint32_t now_ms)
{
    if (!gesture) {
        return D1L_UI_BUTTON_ACTION_NONE;
    }
    if (!pressed) {
        gesture->was_pressed = false;
        if (gesture->first_press_pending &&
            (int32_t)(now_ms - gesture->deadline_ms) >= 0) {
            gesture->first_press_pending = false;
        }
        return D1L_UI_BUTTON_ACTION_NONE;
    }
    if (gesture->was_pressed) {
        return D1L_UI_BUTTON_ACTION_NONE;
    }
    gesture->was_pressed = true;
    if (display_locked) {
        gesture->first_press_pending = false;
        return D1L_UI_BUTTON_ACTION_WAKE;
    }
    if (gesture->first_press_pending &&
        (int32_t)(now_ms - gesture->deadline_ms) < 0) {
        gesture->first_press_pending = false;
        return D1L_UI_BUTTON_ACTION_ADVERT;
    }
    gesture->first_press_pending = true;
    gesture->deadline_ms = now_ms + D1L_UI_BUTTON_DOUBLE_PRESS_MS;
    return D1L_UI_BUTTON_ACTION_ACTIVITY;
}
