#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t d1l_backlight_set_percent(int percent);
uint8_t d1l_backlight_get_percent(void);
