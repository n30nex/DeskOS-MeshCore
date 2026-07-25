#include "backlight.h"

#include "driver/ledc.h"
#include "esp_check.h"

#define D1L_BACKLIGHT_GPIO 45
#define D1L_BACKLIGHT_TIMER LEDC_TIMER_0
#define D1L_BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define D1L_BACKLIGHT_MODE LEDC_LOW_SPEED_MODE
#define D1L_BACKLIGHT_MAX_DUTY 8191

static bool s_backlight_ready = false;
static uint8_t s_backlight_percent;

static esp_err_t backlight_init_once(void)
{
    if (s_backlight_ready) {
        return ESP_OK;
    }

    ledc_timer_config_t timer = {
        .speed_mode = D1L_BACKLIGHT_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = D1L_BACKLIGHT_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), "d1l_bl", "timer config failed");

    ledc_channel_config_t channel = {
        .gpio_num = D1L_BACKLIGHT_GPIO,
        .speed_mode = D1L_BACKLIGHT_MODE,
        .channel = D1L_BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = D1L_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), "d1l_bl", "channel config failed");
    s_backlight_ready = true;
    return ESP_OK;
}

esp_err_t d1l_backlight_set_percent(int percent)
{
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(backlight_init_once(), "d1l_bl", "init failed");
    uint32_t duty = (uint32_t)((D1L_BACKLIGHT_MAX_DUTY * percent) / 100);
    ESP_RETURN_ON_ERROR(ledc_set_duty(D1L_BACKLIGHT_MODE, D1L_BACKLIGHT_CHANNEL, duty), "d1l_bl", "set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(D1L_BACKLIGHT_MODE, D1L_BACKLIGHT_CHANNEL),
                        "d1l_bl", "update duty failed");
    s_backlight_percent = (uint8_t)percent;
    return ESP_OK;
}

uint8_t d1l_backlight_get_percent(void)
{
    return s_backlight_percent;
}
