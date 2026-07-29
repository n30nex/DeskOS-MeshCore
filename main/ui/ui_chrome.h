#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_navigation.h"

#define D1L_UI_DISPLAY_HEIGHT 480U
#define D1L_UI_DOCKED_CONTENT_Y 0U
#define D1L_UI_DOCK_Y 432U
#define D1L_UI_DOCK_HEIGHT (D1L_UI_DISPLAY_HEIGHT - D1L_UI_DOCK_Y)
#define D1L_UI_DOCKED_CONTENT_HEIGHT \
    (D1L_UI_DOCK_Y - D1L_UI_DOCKED_CONTENT_Y)

typedef struct {
    uint16_t content_y;
    uint16_t content_height;
    bool content_scrollable;
    bool dock_visible;
    bool header_detail_visible;
    const char *title;
} d1l_ui_chrome_layout_t;

d1l_ui_chrome_layout_t d1l_ui_chrome_layout_for_screen(d1l_ui_screen_t screen);
