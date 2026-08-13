#include "ui_boot_scene.h"

#include <stddef.h>
#include <string.h>

#include "ui_boot_scene_model.h"

#define DESKOS_CHARCOAL 0x17191AU
#define DESKOS_SURFACE 0x20262BU
#define DESKOS_CYAN 0x20D9EDU
#define DESKOS_COBALT 0x1E5AEFU
#define DESKOS_LIME 0x84FF2EU
#define DESKOS_TEXT 0xF4F7FBU
#define DESKOS_MUTED 0xA6B0B7U

static const lv_point_t LINK_POINTS[3][2] = {
    {{50, 96}, {100, 24}},
    {{100, 24}, {150, 96}},
    {{50, 96}, {150, 96}},
};

static const lv_coord_t NODE_X[3] = {50, 150, 100};
static const lv_coord_t NODE_Y[3] = {96, 96, 24};
static const lv_coord_t STAR_X[8] = {36, 92, 402, 438, 54, 416, 126, 354};
static const lv_coord_t STAR_Y[8] = {92, 314, 104, 286, 392, 374, 46, 336};

static lv_obj_t *plain_object(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    if (!object) {
        return NULL;
    }
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *plain_label(lv_obj_t *parent, const char *text,
                             uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (!label) {
        return NULL;
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static bool discard_scene(d1l_ui_boot_scene_t *scene)
{
    if (scene && scene->overlay) {
        lv_obj_del(scene->overlay);
    }
    if (scene) {
        memset(scene, 0, sizeof(*scene));
    }
    return false;
}

static void finish_scene(d1l_ui_boot_scene_t *scene, lv_timer_t *timer)
{
    scene->complete = true;
    scene->timer = NULL;
    if (scene->overlay) {
        lv_obj_t *overlay = scene->overlay;
        scene->overlay = NULL;
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_del_async(overlay);
    }
    lv_timer_del(timer);
}

static void boot_timer_cb(lv_timer_t *timer)
{
    d1l_ui_boot_scene_t *scene = (d1l_ui_boot_scene_t *)timer->user_data;
    if (!scene || !scene->overlay) {
        lv_timer_del(timer);
        return;
    }

    const uint32_t elapsed = lv_tick_elaps(scene->started_tick);
    d1l_ui_boot_scene_state_t state = {0};
    d1l_ui_boot_scene_state_at(elapsed, &state);
    if (state.complete) {
        finish_scene(scene, timer);
        return;
    }

    lv_obj_set_style_opa(scene->frame, state.frame_opacity, 0);
    lv_obj_set_style_opa(scene->screen, state.frame_opacity, 0);
    lv_obj_set_style_opa(scene->title, state.title_opacity, 0);
    lv_obj_set_style_opa(scene->subtitle, state.title_opacity, 0);
    for (size_t i = 0; i < 3U; ++i) {
        lv_obj_set_style_opa(
            scene->nodes[i], i < state.node_count ? state.pulse_opacity : 0U,
            0);
        lv_obj_set_style_opa(
            scene->links[i], i < state.link_count ? state.frame_opacity : 0U,
            0);
        const uint8_t travel = (uint8_t)((state.spark_percent + i * 31U) % 100U);
        lv_obj_set_pos(scene->sparks[i],
                       306 + ((lv_coord_t)travel * 52) / 100,
                       108 - ((lv_coord_t)travel * 52) / 100);
        lv_obj_set_style_opa(
            scene->sparks[i], state.link_count > 0U ?
                (uint8_t)(120U + (travel * 135U) / 100U) : 0U, 0);
    }
    for (size_t i = 0; i < 8U; ++i) {
        const uint8_t offset = (uint8_t)(i * 27U);
        lv_obj_set_style_opa(scene->stars[i],
            (uint8_t)(40U + ((state.pulse_opacity + offset) % 100U)), 0);
    }
    lv_label_set_text(scene->status, d1l_ui_boot_phase_text(state.phase));
    lv_obj_set_width(scene->progress,
                     4 + ((lv_coord_t)state.progress_percent * 344) / 100);
}

bool d1l_ui_boot_scene_create(d1l_ui_boot_scene_t *scene, lv_obj_t *parent)
{
    if (!scene || !parent) {
        return false;
    }
    memset(scene, 0, sizeof(*scene));
    scene->overlay = plain_object(parent);
    if (!scene->overlay) {
        return false;
    }
    lv_obj_set_size(scene->overlay, 480, 480);
    lv_obj_set_pos(scene->overlay, 0, 0);
    lv_obj_set_style_bg_color(scene->overlay, lv_color_hex(DESKOS_CHARCOAL), 0);
    lv_obj_set_style_bg_opa(scene->overlay, LV_OPA_COVER, 0);
    lv_obj_move_foreground(scene->overlay);

    for (size_t i = 0; i < 8U; ++i) {
        scene->stars[i] = plain_object(scene->overlay);
        if (!scene->stars[i]) {
            return discard_scene(scene);
        }
        lv_obj_set_size(scene->stars[i], i % 3U == 0U ? 4 : 2,
                        i % 3U == 0U ? 4 : 2);
        lv_obj_set_pos(scene->stars[i], STAR_X[i], STAR_Y[i]);
        lv_obj_set_style_radius(scene->stars[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(scene->stars[i],
            lv_color_hex(i % 2U == 0U ? DESKOS_CYAN : DESKOS_COBALT), 0);
    }

    scene->frame = plain_object(scene->overlay);
    if (!scene->frame) {
        return discard_scene(scene);
    }
    scene->screen = plain_object(scene->frame);
    if (!scene->screen) {
        return discard_scene(scene);
    }
    lv_obj_set_pos(scene->frame, 124, 38);
    lv_obj_set_size(scene->frame, 232, 224);
    lv_obj_set_style_radius(scene->frame, 22, 0);
    lv_obj_set_style_bg_color(scene->frame, lv_color_hex(DESKOS_CYAN), 0);
    lv_obj_set_style_opa(scene->frame, 0, 0);
    lv_obj_set_pos(scene->screen, 12, 12);
    lv_obj_set_size(scene->screen, 208, 166);
    lv_obj_set_style_radius(scene->screen, 14, 0);
    lv_obj_set_style_bg_color(scene->screen, lv_color_hex(DESKOS_SURFACE), 0);
    lv_obj_set_style_opa(scene->screen, 0, 0);

    for (size_t i = 0; i < 3U; ++i) {
        scene->links[i] = lv_line_create(scene->screen);
        if (!scene->links[i]) {
            return discard_scene(scene);
        }
        lv_line_set_points(scene->links[i], LINK_POINTS[i], 2);
        lv_obj_set_style_line_width(scene->links[i], 7, 0);
        lv_obj_set_style_line_rounded(scene->links[i], true, 0);
        lv_obj_set_style_line_color(scene->links[i], lv_color_hex(DESKOS_COBALT), 0);
        lv_obj_set_style_opa(scene->links[i], 0, 0);
    }
    for (size_t i = 0; i < 3U; ++i) {
        scene->nodes[i] = plain_object(scene->screen);
        if (!scene->nodes[i]) {
            return discard_scene(scene);
        }
        lv_obj_set_size(scene->nodes[i], 26, 26);
        lv_obj_set_pos(scene->nodes[i], NODE_X[i] - 13, NODE_Y[i] - 13);
        lv_obj_set_style_radius(scene->nodes[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(scene->nodes[i], lv_color_hex(DESKOS_LIME), 0);
        lv_obj_set_style_opa(scene->nodes[i], 0, 0);
    }
    lv_obj_t *touch = plain_object(scene->frame);
    if (!touch) {
        return discard_scene(scene);
    }
    lv_obj_set_size(touch, 22, 22);
    lv_obj_set_pos(touch, 105, 190);
    lv_obj_set_style_radius(touch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(touch, lv_color_hex(DESKOS_LIME), 0);

    for (size_t i = 0; i < 3U; ++i) {
        scene->sparks[i] = plain_object(scene->overlay);
        if (!scene->sparks[i]) {
            return discard_scene(scene);
        }
        const lv_coord_t size = 7 + (lv_coord_t)i * 3;
        lv_obj_set_size(scene->sparks[i], size, size);
        lv_obj_set_style_radius(scene->sparks[i], 2, 0);
        lv_obj_set_style_bg_color(scene->sparks[i], lv_color_hex(DESKOS_CYAN), 0);
        lv_obj_set_style_opa(scene->sparks[i], 0, 0);
    }

    scene->title = plain_label(scene->overlay, "DeskOS", DESKOS_TEXT);
    scene->subtitle = plain_label(scene->overlay, "Touch the mesh", DESKOS_CYAN);
    scene->status = plain_label(scene->overlay, "Starting DeskOS", DESKOS_MUTED);
    if (!scene->title || !scene->subtitle || !scene->status) {
        return discard_scene(scene);
    }
    lv_obj_set_style_text_font(scene->title, &lv_font_montserrat_24, 0);
    lv_obj_align(scene->title, LV_ALIGN_TOP_MID, 0, 282);
    lv_obj_align(scene->subtitle, LV_ALIGN_TOP_MID, 0, 318);
    lv_obj_align(scene->status, LV_ALIGN_TOP_MID, 0, 416);
    lv_obj_set_style_opa(scene->title, 0, 0);
    lv_obj_set_style_opa(scene->subtitle, 0, 0);

    lv_obj_t *progress_track = plain_object(scene->overlay);
    if (!progress_track) {
        return discard_scene(scene);
    }
    scene->progress = plain_object(progress_track);
    if (!scene->progress) {
        return discard_scene(scene);
    }
    lv_obj_set_pos(progress_track, 64, 390);
    lv_obj_set_size(progress_track, 352, 6);
    lv_obj_set_style_radius(progress_track, 3, 0);
    lv_obj_set_style_bg_color(progress_track, lv_color_hex(0x303A42U), 0);
    lv_obj_set_pos(scene->progress, 0, 0);
    lv_obj_set_size(scene->progress, 4, 6);
    lv_obj_set_style_radius(scene->progress, 3, 0);
    lv_obj_set_style_bg_color(scene->progress, lv_color_hex(DESKOS_CYAN), 0);

    scene->started_tick = lv_tick_get();
    scene->timer = lv_timer_create(boot_timer_cb, 40U, scene);
    if (!scene->timer) {
        return discard_scene(scene);
    }
    return true;
}

bool d1l_ui_boot_scene_visible(const d1l_ui_boot_scene_t *scene)
{
    return scene && scene->overlay && !scene->complete;
}
