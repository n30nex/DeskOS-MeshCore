#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define D1L_DISPLAY_BRIGHTNESS_DEFAULT 70U
#define D1L_DISPLAY_TIMEOUT_DEFAULT_SECONDS 60U

typedef enum {
    D1L_NOTIFICATION_MODE_OFF = 0,
    D1L_NOTIFICATION_MODE_PULSE,
    D1L_NOTIFICATION_MODE_QUIET_HOURS,
} d1l_notification_mode_t;

typedef struct {
    uint8_t brightness_percent;
    uint16_t timeout_seconds;
    d1l_notification_mode_t notification_mode;
} d1l_display_preferences_t;

esp_err_t d1l_display_preferences_init(void);
void d1l_display_preferences_get(d1l_display_preferences_t *out_preferences);
esp_err_t d1l_display_preferences_set_brightness(uint8_t percent);
esp_err_t d1l_display_preferences_set_timeout(uint16_t seconds);
esp_err_t d1l_display_preferences_set_notification_mode(
    d1l_notification_mode_t mode);
const char *d1l_notification_mode_name(d1l_notification_mode_t mode);
bool d1l_display_timeout_valid(uint16_t seconds);

#ifdef __cplusplus
}
#endif
