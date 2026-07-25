#include "display_preferences.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define D1L_DISPLAY_PREFERENCES_NAMESPACE "d1l_display"

static SemaphoreHandle_t s_lock;
static d1l_display_preferences_t s_preferences = {
    .brightness_percent = D1L_DISPLAY_BRIGHTNESS_DEFAULT,
    .timeout_seconds = D1L_DISPLAY_TIMEOUT_DEFAULT_SECONDS,
    .notification_mode = D1L_NOTIFICATION_MODE_PULSE,
};
static bool s_initialized;

static bool brightness_valid(uint8_t percent)
{
    return percent >= 10U && percent <= 100U;
}

bool d1l_display_timeout_valid(uint16_t seconds)
{
    return seconds == 0U || seconds == 30U || seconds == 60U ||
           seconds == 120U || seconds == 300U;
}

static bool notification_mode_valid(d1l_notification_mode_t mode)
{
    return mode >= D1L_NOTIFICATION_MODE_OFF &&
           mode <= D1L_NOTIFICATION_MODE_QUIET_HOURS;
}

const char *d1l_notification_mode_name(d1l_notification_mode_t mode)
{
    switch (mode) {
    case D1L_NOTIFICATION_MODE_OFF:
        return "off";
    case D1L_NOTIFICATION_MODE_PULSE:
        return "pulse";
    case D1L_NOTIFICATION_MODE_QUIET_HOURS:
        return "quiet_22_07";
    default:
        return "invalid";
    }
}

static esp_err_t save_locked(void)
{
    nvs_handle_t handle = 0U;
    esp_err_t ret = nvs_open(
        D1L_DISPLAY_PREFERENCES_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, "brightness",
                         s_preferences.brightness_percent);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u16(handle, "timeout",
                          s_preferences.timeout_seconds);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, "notify",
                         (uint8_t)s_preferences.notification_mode);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    return ret;
}

esp_err_t d1l_display_preferences_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    d1l_display_preferences_t loaded = s_preferences;
    nvs_handle_t handle = 0U;
    esp_err_t ret = nvs_open(
        D1L_DISPLAY_PREFERENCES_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_OK) {
        uint8_t mode = (uint8_t)loaded.notification_mode;
        const esp_err_t brightness_ret =
            nvs_get_u8(handle, "brightness", &loaded.brightness_percent);
        const esp_err_t timeout_ret =
            nvs_get_u16(handle, "timeout", &loaded.timeout_seconds);
        const esp_err_t mode_ret = nvs_get_u8(handle, "notify", &mode);
        nvs_close(handle);
        if (brightness_ret != ESP_OK &&
            brightness_ret != ESP_ERR_NVS_NOT_FOUND) {
            ret = brightness_ret;
        } else if (timeout_ret != ESP_OK &&
                   timeout_ret != ESP_ERR_NVS_NOT_FOUND) {
            ret = timeout_ret;
        } else if (mode_ret != ESP_OK &&
                   mode_ret != ESP_ERR_NVS_NOT_FOUND) {
            ret = mode_ret;
        } else {
            loaded.notification_mode = (d1l_notification_mode_t)mode;
            ret = ESP_OK;
        }
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }

    if (ret == ESP_OK &&
        (!brightness_valid(loaded.brightness_percent) ||
         !d1l_display_timeout_valid(loaded.timeout_seconds) ||
         !notification_mode_valid(loaded.notification_mode))) {
        loaded = (d1l_display_preferences_t) {
            .brightness_percent = D1L_DISPLAY_BRIGHTNESS_DEFAULT,
            .timeout_seconds = D1L_DISPLAY_TIMEOUT_DEFAULT_SECONDS,
            .notification_mode = D1L_NOTIFICATION_MODE_PULSE,
        };
        ret = save_locked();
    }
    if (ret == ESP_OK) {
        s_preferences = loaded;
        s_initialized = true;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

void d1l_display_preferences_get(d1l_display_preferences_t *out_preferences)
{
    if (!out_preferences) {
        return;
    }
    if (!s_lock ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
        *out_preferences = (d1l_display_preferences_t) {
            .brightness_percent = D1L_DISPLAY_BRIGHTNESS_DEFAULT,
            .timeout_seconds = D1L_DISPLAY_TIMEOUT_DEFAULT_SECONDS,
            .notification_mode = D1L_NOTIFICATION_MODE_PULSE,
        };
        return;
    }
    *out_preferences = s_preferences;
    xSemaphoreGive(s_lock);
}

esp_err_t d1l_display_preferences_set_brightness(uint8_t percent)
{
    if (!brightness_valid(percent) || !s_lock) {
        return !brightness_valid(percent) ? ESP_ERR_INVALID_ARG :
                                            ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const uint8_t previous = s_preferences.brightness_percent;
    s_preferences.brightness_percent = percent;
    const esp_err_t ret = save_locked();
    if (ret != ESP_OK) {
        s_preferences.brightness_percent = previous;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t d1l_display_preferences_set_timeout(uint16_t seconds)
{
    if (!d1l_display_timeout_valid(seconds) || !s_lock) {
        return !d1l_display_timeout_valid(seconds) ? ESP_ERR_INVALID_ARG :
                                                     ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const uint16_t previous = s_preferences.timeout_seconds;
    s_preferences.timeout_seconds = seconds;
    const esp_err_t ret = save_locked();
    if (ret != ESP_OK) {
        s_preferences.timeout_seconds = previous;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t d1l_display_preferences_set_notification_mode(
    d1l_notification_mode_t mode)
{
    if (!notification_mode_valid(mode) || !s_lock) {
        return !notification_mode_valid(mode) ? ESP_ERR_INVALID_ARG :
                                                ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const d1l_notification_mode_t previous = s_preferences.notification_mode;
    s_preferences.notification_mode = mode;
    const esp_err_t ret = save_locked();
    if (ret != ESP_OK) {
        s_preferences.notification_mode = previous;
    }
    xSemaphoreGive(s_lock);
    return ret;
}
