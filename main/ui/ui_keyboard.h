#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/qualification_hooks.h"

typedef struct _lv_obj_t lv_obj_t;
typedef struct _lv_event_t lv_event_t;

void d1l_ui_keyboard_configure_compose(lv_obj_t *keyboard);
void d1l_ui_keyboard_configure_input(lv_obj_t *keyboard,
                                     lv_obj_t *textarea,
                                     int32_t x,
                                     int32_t y,
                                     int32_t width,
                                     int32_t height);
bool d1l_ui_keyboard_focus_textarea_from_event(lv_obj_t *keyboard,
                                               lv_event_t *event,
                                               lv_obj_t *primary_textarea,
                                               lv_obj_t *secondary_textarea);
void d1l_ui_keyboard_clear_textarea(lv_obj_t *keyboard);
#if D1L_ENABLE_QUALIFICATION_HOOKS
bool d1l_ui_keyboard_normalize_probe_target(const char *name, char *out_target,
                                            size_t out_target_len);
bool d1l_ui_keyboard_probe_target_is_dm(const char *target);
bool d1l_ui_keyboard_probe_target_is_compose(const char *target);
bool d1l_ui_keyboard_probe_target_is_onboarding(const char *target);
bool d1l_ui_keyboard_probe_requires_hidden_dock(const char *target);
int32_t d1l_ui_keyboard_probe_min_width(const char *target);
int32_t d1l_ui_keyboard_probe_min_height(const char *target);
#endif
