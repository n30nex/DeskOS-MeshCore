#include "ui_typography.h"

#include "lvgl.h"
#include "ui_font_symbols_14.h"

static lv_style_t s_text_style;
static lv_font_t s_large_font;
static bool s_initialized;
static uint8_t s_size;

void d1l_ui_typography_set_size(uint8_t size)
{
    if (size > 1U) size = 0U;
    if (!s_initialized) {
        lv_style_init(&s_text_style);
        s_large_font = lv_font_montserrat_18;
        s_large_font.fallback = &d1l_ui_font_symbols_14;
        s_initialized = true;
    } else if (s_size == size) {
        return;
    }
    s_size = size;
    lv_style_set_text_font(&s_text_style,
        size ? &s_large_font : &d1l_ui_font_symbols_14);
    lv_obj_report_style_change(&s_text_style);
}

void d1l_ui_typography_apply(lv_obj_t *object, uint32_t selector)
{
    if (!object) return;
    if (!s_initialized) d1l_ui_typography_set_size(0U);
    lv_obj_remove_style(object, &s_text_style, selector);
    lv_obj_add_style(object, &s_text_style, selector);
}

lv_obj_t *d1l_ui_label_create(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    d1l_ui_typography_apply(label, 0);
    return label;
}
