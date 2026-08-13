#include "ui_screen.h"

static void configure_scrollable_content_root(lv_obj_t *root)
{
    if (!root) {
        return;
    }
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x17191A), 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_bottom(root, 12, 0);
}

static void configure_home_content_root(lv_obj_t *root)
{
    if (!root) {
        return;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(root, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x17191A), 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_bottom(root, 0, 0);
}

void d1l_ui_screen_configure_content_root(lv_obj_t *root, bool scrollable)
{
    if (scrollable) {
        configure_scrollable_content_root(root);
    } else {
        configure_home_content_root(root);
    }
}

static void prepare_content_root(lv_obj_t *content)
{
    if (!content) {
        return;
    }
    lv_obj_clean(content);
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
}

static bool dispatch_screen(d1l_ui_screen_t screen,
                            lv_obj_t *content,
                            const d1l_app_snapshot_t *snapshot,
                            const d1l_ui_screen_renderer_t *renderers,
                            size_t renderer_count)
{
    if (!content || !snapshot || !renderers) {
        return false;
    }
    for (size_t i = 0; i < renderer_count; ++i) {
        if (renderers[i].screen == screen && renderers[i].render) {
            renderers[i].render(content, snapshot);
            return true;
        }
    }
    return false;
}

bool d1l_ui_screen_render(d1l_ui_screen_t screen,
                          const d1l_app_snapshot_t *snapshot,
                          lv_obj_t *content,
                          const d1l_ui_screen_renderer_t *renderers,
                          size_t renderer_count)
{
    if (!content) {
        return false;
    }
    prepare_content_root(content);
    if (dispatch_screen(screen, content, snapshot, renderers, renderer_count)) {
        return true;
    }
    return screen != D1L_UI_SCREEN_HOME &&
           dispatch_screen(D1L_UI_SCREEN_HOME, content, snapshot, renderers, renderer_count);
}
