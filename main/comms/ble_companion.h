#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define D1L_BLE_COMPANION_SERVICE_UUID \
    "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define D1L_BLE_COMPANION_RX_UUID \
    "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define D1L_BLE_COMPANION_TX_UUID \
    "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define D1L_BLE_COMPANION_DEVICE_NAME "MeshCore DeskOS"
#define D1L_BLE_COMPANION_DEFAULT_ATT_MTU 23U

typedef struct {
    bool build_enabled;
    bool stack_initialized;
    bool start_requested;
    bool advertising;
    bool connected;
    bool encrypted;
    bool authenticated;
    bool bonded;
    bool notification_requested;
    bool notification_enabled;
    bool transport_ready;
    uint16_t connection_handle;
    uint16_t att_mtu;
    uint8_t rx_queue_depth;
    uint8_t tx_queue_depth;
    uint32_t connect_count;
    uint32_t disconnect_count;
    uint32_t rx_frame_count;
    uint32_t tx_frame_count;
    uint32_t rx_drop_count;
    uint32_t tx_drop_count;
    uint32_t malformed_frame_count;
    uint32_t security_reject_count;
    esp_err_t last_error;
    int last_nimble_error;
    const char *state;
    const char *security_policy;
    const char *wire_policy;
} d1l_ble_companion_status_t;

bool d1l_ble_companion_build_enabled(void);
esp_err_t d1l_ble_companion_start(void);
esp_err_t d1l_ble_companion_stop(void);
esp_err_t d1l_ble_companion_prepare_reboot(void);
void d1l_ble_companion_status(d1l_ble_companion_status_t *out_status);

/*
 * RX frames are returned in the existing MeshCore three-byte transport form:
 * '<', uint16 little-endian payload length, raw companion payload. The caller
 * owns protocol dispatch; this transport never invokes radio or Mesh runtime
 * APIs from a NimBLE callback.
 */
esp_err_t d1l_ble_companion_take_rx_frame(uint8_t *dest, size_t dest_cap,
                                          size_t *out_len);

/*
 * TX accepts exactly one existing three-byte radio-to-app frame ('>').
 * Notifications remain raw MeshCore BLE characteristic values, matching the
 * pinned upstream BLE contract rather than exposing the serial header on air.
 */
esp_err_t d1l_ble_companion_queue_tx_frame(const uint8_t *frame,
                                           size_t frame_len);
