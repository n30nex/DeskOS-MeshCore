#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool running;
    bool transport_ready;
    uint8_t client_protocol_version;
    uint8_t last_command;
    uint32_t session_count;
    uint32_t command_count;
    uint32_t response_count;
    uint32_t unsupported_count;
    uint32_t malformed_count;
    uint32_t transport_error_count;
    esp_err_t last_error;
} d1l_ble_companion_protocol_status_t;

esp_err_t d1l_ble_companion_protocol_start(void);
esp_err_t d1l_ble_companion_protocol_stop(void);
void d1l_ble_companion_protocol_status(
    d1l_ble_companion_protocol_status_t *out_status);
