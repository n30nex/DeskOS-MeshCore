#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int lv_font_montserrat_24 = 24;

static lv_obj_t *create_object(lv_obj_t *parent, int kind)
{
    if (parent && (!parent->valid || parent->child_count >= 64U)) {
        return NULL;
    }
    lv_obj_t *object = (lv_obj_t *)calloc(1U, sizeof(*object));
    if (!object) {
        return NULL;
    }
    object->valid = true;
    object->kind = kind;
    object->parent = parent;
    if (parent) {
        parent->children[parent->child_count++] = object;
    }
    return object;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    return create_object(parent, 1);
}

lv_obj_t *lv_label_create(lv_obj_t *parent)
{
    return create_object(parent, 2);
}

lv_obj_t *lv_btn_create(lv_obj_t *parent)
{
    return create_object(parent, 3);
}

bool lv_obj_is_valid(const lv_obj_t *object)
{
    return object && object->valid;
}

static void detach_from_parent(lv_obj_t *object)
{
    if (!object || !object->parent) {
        return;
    }
    lv_obj_t *parent = object->parent;
    for (size_t index = 0U; index < parent->child_count; ++index) {
        if (parent->children[index] == object) {
            memmove(&parent->children[index], &parent->children[index + 1U],
                    (parent->child_count - index - 1U) *
                        sizeof(parent->children[0]));
            parent->child_count--;
            parent->children[parent->child_count] = NULL;
            break;
        }
    }
    object->parent = NULL;
}

void lv_obj_del(lv_obj_t *object)
{
    if (!object || !object->valid) {
        return;
    }
    while (object->child_count > 0U) {
        lv_obj_del(object->children[object->child_count - 1U]);
    }
    detach_from_parent(object);
    object->valid = false;
}

void lv_obj_clean(lv_obj_t *object)
{
    if (!lv_obj_is_valid(object)) {
        return;
    }
    while (object->child_count > 0U) {
        lv_obj_del(object->children[object->child_count - 1U]);
    }
}

void lv_obj_set_size(lv_obj_t *object, int width, int height)
{
    if (object) {
        object->width = width;
        object->height = height;
    }
}

void lv_obj_set_width(lv_obj_t *object, int width)
{
    if (object) {
        object->width = width;
    }
}

void lv_obj_set_pos(lv_obj_t *object, int x, int y)
{
    if (object) {
        object->x = x;
        object->y = y;
    }
}

#define NOOP_STYLE(name, type) \
    void name(lv_obj_t *object, type value, int selector) \
    { \
        (void)object; \
        (void)value; \
        (void)selector; \
    }

NOOP_STYLE(lv_obj_set_style_radius, int)
NOOP_STYLE(lv_obj_set_style_bg_color, lv_color_t)
NOOP_STYLE(lv_obj_set_style_border_color, lv_color_t)
NOOP_STYLE(lv_obj_set_style_border_width, int)
NOOP_STYLE(lv_obj_set_style_pad_all, int)
NOOP_STYLE(lv_obj_set_style_shadow_width, int)
NOOP_STYLE(lv_obj_set_style_text_color, lv_color_t)

void lv_obj_set_style_text_font(lv_obj_t *object, const void *font,
                                int selector)
{
    (void)object;
    (void)font;
    (void)selector;
}

void lv_obj_clear_flag(lv_obj_t *object, uint32_t flag)
{
    if (object) {
        object->flags &= ~flag;
    }
}

void lv_obj_add_flag(lv_obj_t *object, uint32_t flag)
{
    if (object) {
        object->flags |= flag;
    }
}

bool lv_obj_has_flag(const lv_obj_t *object, uint32_t flag)
{
    return object && (object->flags & flag) != 0U;
}

void lv_obj_center(lv_obj_t *object)
{
    (void)object;
}

void lv_obj_move_foreground(lv_obj_t *object)
{
    (void)object;
}

void lv_obj_add_event_cb(lv_obj_t *object, lv_event_cb_t callback,
                         lv_event_code_t code, void *user_data)
{
    (void)code;
    if (object) {
        object->event_cb = callback;
        object->event_user_data = user_data;
    }
}

void *lv_event_get_user_data(lv_event_t *event)
{
    return event ? event->user_data : NULL;
}

void lv_label_set_text(lv_obj_t *label, const char *text)
{
    if (label) {
        snprintf(label->text, sizeof(label->text), "%s", text ? text : "");
    }
}

void lv_label_set_long_mode(lv_obj_t *label, int mode)
{
    (void)label;
    (void)mode;
}

lv_color_t lv_color_hex(uint32_t color)
{
    return color;
}

static bool object_has_label(const lv_obj_t *object, const char *text)
{
    if (!lv_obj_is_valid(object) || !text) {
        return false;
    }
    if (object->kind == 2 && strcmp(object->text, text) == 0) {
        return true;
    }
    for (size_t index = 0U; index < object->child_count; ++index) {
        if (object_has_label(object->children[index], text)) {
            return true;
        }
    }
    return false;
}

lv_obj_t *lv_test_find_button(lv_obj_t *root, const char *label)
{
    if (!lv_obj_is_valid(root)) {
        return NULL;
    }
    if (root->kind == 3 && object_has_label(root, label)) {
        return root;
    }
    for (size_t index = 0U; index < root->child_count; ++index) {
        lv_obj_t *found = lv_test_find_button(root->children[index], label);
        if (found) {
            return found;
        }
    }
    return NULL;
}

bool lv_test_has_label(const lv_obj_t *root, const char *text)
{
    return object_has_label(root, text);
}

void lv_test_click(lv_obj_t *button)
{
    if (!lv_obj_is_valid(button) || !button->event_cb) {
        return;
    }
    lv_event_t event = {
        .user_data = button->event_user_data,
        .code = LV_EVENT_CLICKED,
    };
    button->event_cb(&event);
}

lv_obj_t *d1l_ui_label_create(lv_obj_t *parent) { return lv_label_create(parent); }
