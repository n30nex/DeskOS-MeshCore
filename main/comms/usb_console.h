#pragma once

#include "esp_err.h"
#include "storage/factory_reset.h"

esp_err_t d1l_usb_console_init(void);
void d1l_usb_console_run(void);
void d1l_usb_console_run_factory_reset_recovery(
    const d1l_factory_reset_status_t *boot_status);
