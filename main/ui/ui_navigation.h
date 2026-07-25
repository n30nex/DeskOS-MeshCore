#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    D1L_UI_SCREEN_HOME = 0,
    D1L_UI_SCREEN_MESSAGES,
    D1L_UI_SCREEN_NODES,
    D1L_UI_SCREEN_MAP,
    D1L_UI_SCREEN_PACKETS,
    D1L_UI_SCREEN_SETTINGS,
} d1l_ui_screen_t;

const char *d1l_ui_screen_name(d1l_ui_screen_t screen);
bool d1l_ui_screen_from_name(const char *name, d1l_ui_screen_t *out_screen);
bool d1l_ui_screen_available(d1l_ui_screen_t screen);
bool d1l_ui_scroll_surface_from_name(const char *name,
                                     char *out_surface,
                                     size_t out_surface_len,
                                     d1l_ui_screen_t *out_screen);
bool d1l_ui_scroll_surface_available(const char *surface,
                                     d1l_ui_screen_t screen);

void d1l_ui_navigation_request(d1l_ui_screen_t screen);
bool d1l_ui_navigation_begin_pending(d1l_ui_screen_t *out_screen);
void d1l_ui_navigation_finish(d1l_ui_screen_t rendered_screen);
d1l_ui_screen_t d1l_ui_navigation_active(void);
d1l_ui_screen_t d1l_ui_navigation_pending(void);
bool d1l_ui_navigation_switch_pending(void);
