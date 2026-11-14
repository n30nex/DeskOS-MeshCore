#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "ui/ui_receive_animation.h"
#include "ui/ui_typography.h"

static uint16_t pixels[480 * 480];
static unsigned clicks;
static bool pressed;
static const char *output_dir;

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            pixels[y * 480 + x] = colors[(y - area->y1) * (area->x2 - area->x1 + 1) + x - area->x1].full;
    lv_disp_flush_ready(driver);
}

static void tick(unsigned milliseconds)
{
    for (unsigned i = 0; i < milliseconds; i += 20) {
        lv_tick_inc(20); lv_timer_handler();
    }
}

static void capture(const char *name, unsigned frame)
{
    lv_refr_now(NULL);
    if (!output_dir) return;
    char path[512]; snprintf(path, sizeof(path), "%s/%s-%03u.rgb565", output_dir, name, frame);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(pixels, sizeof(pixels), 1, file) == 1); fclose(file);
}

static void pointer_read(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver; data->point.x = 240; data->point.y = 20;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void clicked(lv_event_t *event) { (void)event; ++clicks; }

int main(int argc, char **argv)
{
    output_dir = argc > 1 ? argv[1] : NULL;
    lv_init();
    static lv_color_t draw_pixels[480 * 48];
    static lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw, draw_pixels, NULL, 480 * 48);
    static lv_disp_drv_t display; lv_disp_drv_init(&display);
    display.hor_res = display.ver_res = 480; display.draw_buf = &draw; display.flush_cb = flush;
    lv_disp_drv_register(&display);
    static lv_indev_drv_t pointer; lv_indev_drv_init(&pointer);
    pointer.type = LV_INDEV_TYPE_POINTER; pointer.read_cb = pointer_read; lv_indev_drv_register(&pointer);
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x17191A), 0);
    lv_obj_t *title = d1l_ui_label_create(screen); lv_label_set_text(title, "DeskOS");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF4F7FB), 0); lv_obj_set_pos(title, 12, 12);
    lv_obj_t *button = lv_btn_create(screen); lv_obj_set_pos(button, 126, 4); lv_obj_set_size(button, 240, 38);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clock = d1l_ui_label_create(screen); lv_label_set_text(clock, "12:34");
    lv_obj_set_style_text_color(clock, lv_color_hex(0x20D9ED), 0); lv_obj_set_pos(clock, 336, 16);
    lv_obj_t *lock = lv_btn_create(screen); lv_obj_set_pos(lock, 382, 0); lv_obj_set_size(lock, 86, 44);
    lv_obj_t *lock_text = d1l_ui_label_create(lock); lv_label_set_text(lock_text, "Lock"); lv_obj_center(lock_text);
    lv_obj_t *text = lv_textarea_create(screen); lv_obj_set_pos(text, 16, 100); lv_obj_set_size(text, 448, 100);
    lv_textarea_set_text(text, "Keep my draft"); lv_textarea_set_cursor_pos(text, 5);
    lv_group_t *group = lv_group_create(); lv_group_add_obj(group, text); lv_group_focus_obj(text);
    d1l_ui_receive_animation_t view = {0};
    assert(d1l_ui_receive_animation_create(&view, lv_layer_top(), 7, 11));
    assert(!d1l_ui_receive_animation_update(&view, 7, 11, true));
    assert(!view.active && lv_obj_has_flag(view.panel, LV_OBJ_FLAG_HIDDEN));
    assert(d1l_ui_receive_animation_update(&view, 8, 11, true));
    lv_obj_update_layout(view.panel);
    assert(view.active && view.kind == 1 && lv_obj_get_y(view.panel) < 0);
    assert(!lv_obj_has_flag(view.panel, LV_OBJ_FLAG_CLICKABLE));
    assert(!lv_obj_has_flag(view.icon, LV_OBJ_FLAG_CLICKABLE));
    assert(!lv_obj_has_flag(view.label, LV_OBJ_FLAG_CLICKABLE));
    for (unsigned frame = 0; frame < 28; ++frame) {
        tick(80); capture("group", frame);
        if (frame == 5) {
            assert(lv_obj_get_y(view.panel) == 4);
            assert(lv_group_get_focused(group) == text);
            assert(lv_textarea_get_cursor_pos(text) == 5);
            pressed = true; tick(60); pressed = false; tick(60);
            assert(clicks == 1); /* The animated banner passes touches through. */
            assert(d1l_ui_receive_animation_update(&view, 9, 11, true));
        }
    }
    assert(!view.active && lv_obj_has_flag(view.panel, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_textarea_get_text(text), "Keep my draft") == 0);
    assert(!d1l_ui_receive_animation_update(&view, 9, 11, true));
    d1l_ui_typography_set_size(1);
    assert(d1l_ui_receive_animation_update(&view, 9, 12, true));
    assert(view.kind == 2 && strcmp(lv_label_get_text(view.label), "Direct message") == 0);
    for (unsigned frame = 0; frame < 28; ++frame) { tick(80); capture("dm", frame); }
    assert(!view.active);
    assert(d1l_ui_receive_animation_update(&view, 10, 12, true)); tick(400);
    assert(d1l_ui_receive_animation_update(&view, 10, 13, true));
    assert(view.kind == 3 && strcmp(lv_label_get_text(view.label), "New messages") == 0);
    tick(1880); assert(!view.active); /* Bursts never restart the deadline. */
    assert(!d1l_ui_receive_animation_update(&view, 20, 30, false));
    assert(!d1l_ui_receive_animation_update(&view, 20, 30, true));
    assert(!d1l_ui_receive_animation_update(&view, 0, 0, true)); /* Counter reset. */
    view.groups_seen = UINT32_MAX;
    assert(d1l_ui_receive_animation_update(&view, 0, 0, true)); /* Real wrap. */
    assert(!d1l_ui_receive_animation_update(&view, 0, 1, false));
    assert(!view.active && lv_obj_has_flag(view.panel, LV_OBJ_FLAG_HIDDEN));
    assert(d1l_ui_receive_animation_update(&view, 1, 1, true));
    lv_obj_del(view.panel); tick(2400);
    assert(!view.panel && !view.active);
    assert(d1l_ui_receive_animation_create(&view, lv_layer_top(), 5, 5));
    assert(!d1l_ui_receive_animation_update(&view, 5, 5, true));
    assert(d1l_ui_receive_animation_update(&view, 5, 6, true));
    d1l_ui_receive_animation_destroy(&view); tick(2400);
    assert(!view.panel && !view.active);
    puts("native receive animation: timing, bursts, suppression, touch and lifecycle passed");
    return 0;
}
