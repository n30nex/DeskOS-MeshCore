#include "ui_receive_animation.h"

#include <string.h>
#include <stdio.h>
#include "lvgl.h"
#include "ui_typography.h"

LV_FONT_DECLARE(d1l_ui_font_latin_24);

/* Ordinary alerts coalesce into 2.2 seconds. Presentation mode keeps just the
 * latest message visible for 6.5 seconds, with no queue or automatic sending. */
static void animate(void *context, int32_t progress)
{
    d1l_ui_receive_animation_t *view = context;
    if (!view->panel) return;
    int32_t opacity = 255;
    const int32_t enter = view->presentation ? 40 : 140;
    const int32_t leave = view->presentation ? 920 : 820;
    const int32_t resting_y = view->presentation ? 60 : 4;
    const int32_t travel = view->presentation ? 16 : 44;
    int32_t y = resting_y;
    if (progress < enter) {
        opacity = progress * 255 / enter;
        y = resting_y - travel + progress * travel / enter;
    } else if (progress > leave) {
        opacity = (1000 - progress) * 255 / (1000 - leave);
        y = resting_y - (progress - leave) * travel / (1000 - leave);
    }
    lv_obj_set_y(view->panel, (lv_coord_t)y);
    lv_obj_set_style_opa(view->panel, (lv_opa_t)opacity, 0);
    /* Two gentle envelope nudges while the banner settles. */
    const int32_t period = view->presentation ? 80 : 500;
    const int32_t beat = progress % period;
    const int32_t nudge = view->presentation && progress >= 160 ? 0 :
        (beat < period / 2 ? beat * 3 / (period / 2) : (period - beat) * 3 / (period / 2));
    lv_obj_set_x(view->icon, (lv_coord_t)(12 + nudge));
    lv_obj_set_style_border_opa(view->panel, (lv_opa_t)(160 + nudge * 30), 0);
    if (progress == 1000) {
        lv_obj_add_flag(view->panel, LV_OBJ_FLAG_HIDDEN);
        view->active = false;
        view->kind = 0;
        lv_label_set_text(view->sender, "");
        lv_label_set_text(view->message, "");
        lv_label_set_text(view->label, "");
    }
}

void d1l_ui_receive_animation_stop(d1l_ui_receive_animation_t *view)
{
    if (!view) return;
    lv_anim_del(view, animate);
    if (view->panel) lv_obj_add_flag(view->panel, LV_OBJ_FLAG_HIDDEN);
    if (view->sender) lv_label_set_text(view->sender, "");
    if (view->message) lv_label_set_text(view->message, "");
    if (view->label) lv_label_set_text(view->label, "");
    view->active = false;
    view->kind = 0;
}

static void panel_deleted(lv_event_t *event)
{
    d1l_ui_receive_animation_t *view = lv_event_get_user_data(event);
    lv_anim_del(view, animate);
    view->panel = view->icon = view->label = view->sender = view->message = view->footer = NULL;
    view->active = false;
    view->kind = 0;
}

static void panel_clicked(lv_event_t *event)
{
    d1l_ui_receive_animation_t *view = lv_event_get_user_data(event);
    if (view->presentation) d1l_ui_receive_animation_stop(view);
}

void d1l_ui_receive_animation_destroy(d1l_ui_receive_animation_t *view)
{
    if (!view) return;
    d1l_ui_receive_animation_stop(view);
    if (view->panel) lv_obj_del(view->panel);
    memset(view, 0, sizeof(*view));
}

bool d1l_ui_receive_animation_create(d1l_ui_receive_animation_t *view,
    lv_obj_t *parent, uint32_t groups, uint32_t dms)
{
    if (!view) return false;
    d1l_ui_receive_animation_destroy(view);
    view->groups_seen = groups;
    view->dms_seen = dms;
    if (!parent) return false;
    view->panel = lv_obj_create(parent);
    if (!view->panel) return false;
    lv_obj_add_event_cb(view->panel, panel_deleted, LV_EVENT_DELETE, view);
    lv_obj_add_event_cb(view->panel, panel_clicked, LV_EVENT_CLICKED, view);
    lv_obj_set_size(view->panel, 250, 38);
    lv_obj_set_pos(view->panel, 126, 4);
    lv_obj_clear_flag(view->panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(view->panel, 0, 0);
    lv_obj_set_style_radius(view->panel, 9, 0);
    lv_obj_set_style_border_width(view->panel, 2, 0);
    lv_obj_set_style_bg_color(view->panel, lv_color_hex(0x102327), 0);
    lv_obj_set_style_bg_opa(view->panel, LV_OPA_COVER, 0);
    view->icon = lv_label_create(view->panel);
    view->label = d1l_ui_label_create(view->panel);
    view->sender = lv_label_create(view->panel);
    view->message = lv_label_create(view->panel);
    view->footer = d1l_ui_label_create(view->panel);
    if (!view->icon || !view->label || !view->sender || !view->message || !view->footer) {
        d1l_ui_receive_animation_destroy(view);
        view->groups_seen = groups;
        view->dms_seen = dms;
        return false;
    }
    lv_label_set_text(view->icon, LV_SYMBOL_ENVELOPE);
    lv_obj_set_pos(view->icon, 12, 10);
    lv_obj_set_style_text_font(view->icon, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(view->label, 40, 8);
    lv_obj_set_width(view->label, 190);
    lv_label_set_long_mode(view->label, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(view->icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(view->label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(view->label, lv_color_hex(0xF4F7FB), 0);
    lv_obj_set_pos(view->sender, 16, 46);
    lv_obj_set_size(view->sender, 424, 32);
    lv_obj_set_style_text_font(view->sender, &d1l_ui_font_latin_24, 0);
    lv_obj_set_style_text_color(view->sender, lv_color_hex(0xF4F7FB), 0);
    lv_label_set_long_mode(view->sender, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(view->message, 16, 88);
    lv_obj_set_size(view->message, 424, 204);
    lv_obj_set_style_text_font(view->message, &d1l_ui_font_latin_24, 0);
    lv_obj_set_style_text_color(view->message, lv_color_hex(0xF4F7FB), 0);
    lv_obj_set_style_text_line_space(view->message, 4, 0);
    lv_label_set_long_mode(view->message, LV_LABEL_LONG_DOT);
    lv_label_set_text(view->sender, "");
    lv_label_set_text(view->message, "");
    lv_label_set_text(view->footer, "Presentation | Tap to dismiss");
    lv_obj_set_pos(view->footer, 16, 310);
    lv_obj_set_width(view->footer, 424);
    lv_obj_set_style_text_color(view->footer, lv_color_hex(0xA6B0B7), 0);
    lv_obj_t *extra[] = {view->sender, view->message, view->footer};
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_clear_flag(extra[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(extra[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(view->panel, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void d1l_ui_receive_animation_set_presentation(d1l_ui_receive_animation_t *view, bool enabled)
{
    if (!view || view->presentation == enabled) return;
    d1l_ui_receive_animation_stop(view);
    view->presentation = enabled;
    if (!view->panel) return;
    lv_obj_set_size(view->panel, enabled ? 456 : 250, enabled ? 344 : 38);
    lv_obj_set_pos(view->panel, enabled ? 12 : 126, enabled ? 60 : 4);
    lv_obj_set_style_border_width(view->panel, enabled ? 3 : 2, 0);
    lv_obj_set_pos(view->label, 40, enabled ? 12 : 8);
    lv_obj_set_width(view->label, enabled ? 404 : 190);
    if (enabled) lv_obj_add_flag(view->panel, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_clear_flag(view->panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *extra[] = {view->sender, view->message, view->footer};
    for (unsigned i = 0; i < 3; ++i) {
        if (enabled) lv_obj_clear_flag(extra[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(extra[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void d1l_ui_receive_animation_set_message(d1l_ui_receive_animation_t *view,
    bool direct, const char *conversation, const char *sender, const char *text)
{
    if (!view || !view->panel || !view->active || !view->presentation) return;
    char title[64];
    if (direct) snprintf(title, sizeof(title), "Direct message");
    else snprintf(title, sizeof(title), "Channel | %.47s",
                  conversation && conversation[0] ? conversation : "Channel");
    lv_label_set_text(view->label, title);
    lv_label_set_text(view->sender, sender && sender[0] ? sender : "Unknown sender");
    lv_label_set_text(view->message, text ? text : "");
}

static bool advanced(uint32_t current, uint32_t previous)
{
    const uint32_t delta = current - previous;
    return delta != 0 && delta <= UINT32_MAX / 2U;
}

bool d1l_ui_receive_animation_update(d1l_ui_receive_animation_t *view,
    uint32_t groups, uint32_t dms, bool enabled)
{
    if (!view) return false;
    const uint8_t kind = (advanced(groups, view->groups_seen) ? 1U : 0U) |
                         (advanced(dms, view->dms_seen) ? 2U : 0U);
    view->groups_seen = groups;
    view->dms_seen = dms;
    if (!enabled) {
        d1l_ui_receive_animation_stop(view);
        return false;
    }
    if (!kind) return false;
    if (!view->panel) return true; /* Backlight fallback still works. */
    if (view->presentation) d1l_ui_receive_animation_stop(view);
    view->kind |= kind;
    const uint32_t color = view->kind == 1U ? 0x20D9ED :
                           view->kind == 2U ? 0x84FF2E : 0xFBBF24;
    lv_obj_set_style_border_color(view->panel, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(view->icon, lv_color_hex(color), 0);
    lv_label_set_text(view->label, view->kind == 1U ? "Group message" :
        view->kind == 2U ? "Direct message" : "New messages");
    if (!view->active) {
        view->active = true;
        lv_obj_move_foreground(view->panel);
        lv_obj_clear_flag(view->panel, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, view);
        lv_anim_set_exec_cb(&animation, animate);
        lv_anim_set_values(&animation, 0, 1000);
        lv_anim_set_time(&animation, view->presentation ? 6500 : 2200);
        if (!lv_anim_start(&animation)) d1l_ui_receive_animation_stop(view);
    }
    return true;
}
