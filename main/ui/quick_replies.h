#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define D1L_QUICK_REPLY_COUNT 6U
#define D1L_QUICK_REPLY_BYTES 81U
typedef char d1l_quick_replies_t[D1L_QUICK_REPLY_COUNT][D1L_QUICK_REPLY_BYTES];

/* UI-owned preferences, never sent until the ordinary composer is submitted. */
esp_err_t d1l_quick_replies_load(d1l_quick_replies_t replies);
esp_err_t d1l_quick_reply_save(size_t index, const char *text);
bool d1l_quick_reply_fits(const char *draft, const char *reply);
