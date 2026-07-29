#include "ui_chrome.h"

d1l_ui_chrome_layout_t d1l_ui_chrome_layout_for_screen(d1l_ui_screen_t screen)
{
    if (screen == D1L_UI_SCREEN_HOME) {
        return (d1l_ui_chrome_layout_t){
            .content_y = 16,
            .content_height = 464,
            .content_scrollable = false,
            .dock_visible = false,
            .header_detail_visible = false,
            .title = "DeskOS",
        };
    }
    return (d1l_ui_chrome_layout_t){
        .content_y = D1L_UI_DOCKED_CONTENT_Y,
        .content_height = D1L_UI_DOCKED_CONTENT_HEIGHT,
        .content_scrollable = true,
        .dock_visible = true,
        .header_detail_visible = false,
        .title = "",
    };
}
