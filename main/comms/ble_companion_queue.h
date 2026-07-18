#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "comms/companion_3byte.h"

#define D1L_BLE_COMPANION_QUEUE_DEPTH 4U
#define D1L_BLE_COMPANION_WIRE_FRAME_MAX \
    (D1L_COMPANION3_HEADER_SIZE + D1L_COMPANION3_MAX_FRAME_SIZE)

typedef enum {
    D1L_BLE_QUEUE_OK = 0,
    D1L_BLE_QUEUE_EMPTY,
    D1L_BLE_QUEUE_FULL,
    D1L_BLE_QUEUE_INVALID_ARGUMENT,
    D1L_BLE_QUEUE_FRAME_TOO_LARGE,
    D1L_BLE_QUEUE_DESTINATION_TOO_SMALL,
} d1l_ble_companion_queue_result_t;

typedef struct {
    size_t length;
    uint8_t bytes[D1L_BLE_COMPANION_WIRE_FRAME_MAX];
} d1l_ble_companion_queue_slot_t;

typedef struct {
    d1l_ble_companion_queue_slot_t slots[D1L_BLE_COMPANION_QUEUE_DEPTH];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} d1l_ble_companion_queue_t;

void d1l_ble_companion_queue_init(d1l_ble_companion_queue_t *queue);
size_t d1l_ble_companion_queue_clear(d1l_ble_companion_queue_t *queue);
d1l_ble_companion_queue_result_t d1l_ble_companion_queue_push(
    d1l_ble_companion_queue_t *queue, const uint8_t *frame, size_t frame_len);
d1l_ble_companion_queue_result_t d1l_ble_companion_queue_peek(
    const d1l_ble_companion_queue_t *queue, uint8_t *dest, size_t dest_cap,
    size_t *out_len);
d1l_ble_companion_queue_result_t d1l_ble_companion_queue_pop(
    d1l_ble_companion_queue_t *queue);
d1l_ble_companion_queue_result_t d1l_ble_companion_queue_take(
    d1l_ble_companion_queue_t *queue, uint8_t *dest, size_t dest_cap,
    size_t *out_len);
