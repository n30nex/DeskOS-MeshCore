#include "indicator_pins.h"

static const d1l_display_pins_t s_display = {
    .width = 480,
    .height = 480,
    .hsync_gpio = 16,
    .vsync_gpio = 17,
    .de_gpio = 18,
    .pclk_gpio = 21,
    .backlight_gpio = 45,
    .data_gpio_nums = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
};

static const d1l_i2c_pins_t s_i2c = {
    .sda_gpio = 39,
    .scl_gpio = 40,
    .expander_address = 0x20,
};

static const d1l_sx1262_pins_t s_sx1262 = {
    .sclk_gpio = 41,
    .mosi_gpio = 48,
    .miso_gpio = 47,
    .expander_cs = 0,
    .expander_reset = 1,
    .expander_busy = 2,
    .expander_dio1 = 3,
    .tcxo_default = "NONE",
};

static const d1l_rp2040_pins_t s_rp2040 = {
    .esp_rx_gpio = 20,
    .esp_tx_gpio = 19,
    .expander_reset = 8,
    .uart_port = 2,
    .baud_rate = 115200,
};

const d1l_display_pins_t *d1l_display_pins(void)
{
    return &s_display;
}

const d1l_i2c_pins_t *d1l_i2c_pins(void)
{
    return &s_i2c;
}

const d1l_sx1262_pins_t *d1l_sx1262_pins(void)
{
    return &s_sx1262;
}

const d1l_rp2040_pins_t *d1l_rp2040_pins(void)
{
    return &s_rp2040;
}
