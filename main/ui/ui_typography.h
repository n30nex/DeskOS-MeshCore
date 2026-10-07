#pragma once

#include <stdint.h>

typedef struct _lv_obj_t lv_obj_t;

/* UI-task owned. A shared style keeps open inputs and drafts intact. */
void d1l_ui_typography_set_size(uint8_t size);
void d1l_ui_typography_apply(lv_obj_t *object, uint32_t selector);
lv_obj_t *d1l_ui_label_create(lv_obj_t *parent);
