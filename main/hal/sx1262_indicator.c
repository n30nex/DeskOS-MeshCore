#include "sx1262_indicator.h"

#include <string.h>

#include "driver/spi_master.h"
#include "bsp_sx126x.h"
#include "sx126x-board.h"
#include "sdkconfig.h"
#include "tca9535.h"

static void fill_failure(d1l_radiohw_status_t *status, const char *code)
{
    memset(status, 0, sizeof(*status));
    status->tcxo_default = "NONE";
    status->failure_code = code;
}

static esp_err_t read_radio_pins(uint16_t *pins)
{
#if CONFIG_LCD_BOARD_SENSECAP_INDICATOR_D1L
    return tca9535_read_input_pins(pins);
#else
    uint8_t pins8 = 0;
    esp_err_t ret = indicator_io_expander->read_input_pins(&pins8);
    *pins = pins8;
    return ret;
#endif
}

esp_err_t d1l_sx1262_probe(d1l_radiohw_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    fill_failure(status, "UNPROBED");

    if (!indicator_io_expander) {
        fill_failure(status, "EXPANDER_NOT_READY");
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t pins = 0;
    esp_err_t ret = read_radio_pins(&pins);
    if (ret != ESP_OK) {
        fill_failure(status, "EXPANDER_READ_FAILED");
        return ret;
    }

    status->expander_ready = true;
    status->busy = (pins & (1U << EXPANDER_IO_RADIO_BUSY)) ? 1 : 0;
    status->dio1 = (pins & (1U << EXPANDER_IO_RADIO_DIO_1)) ? 1 : 0;
    status->ver_pin = (pins & (1U << EXPANDER_IO_RADIO_VER)) ? 1 : 0;
    status->tcxo_default = "NONE";

    spi_device_handle_t spi = bsp_sx126x_spi_handle_get();
    if (!spi) {
        status->failure_code = "SPI_NOT_READY";
        return ESP_ERR_INVALID_STATE;
    }

    /* Use the driver's mutex and bounded BUSY handling, so a diagnostic read
     * cannot interleave chip-select changes with a live radio transaction. */
    status->status_byte = SX126xReadCommand(RADIO_GET_STATUS, NULL, 0U);
    if (bsp_sx126x_fault_get() != 0U) {
        status->failure_code = "RADIO_BUS_FAULT";
        return ESP_ERR_TIMEOUT;
    }
    status->present = true;
    status->failure_code = NULL;
    return ESP_OK;
}
