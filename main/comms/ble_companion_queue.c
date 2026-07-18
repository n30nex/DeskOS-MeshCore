#include "ble_companion_queue.h"

#include <string.h>

void d1l_ble_companion_queue_init(d1l_ble_companion_queue_t *queue)
{
    if (!queue) {
        return;
    }
    memset(queue, 0, sizeof(*queue));
}

size_t d1l_ble_companion_queue_clear(d1l_ble_companion_queue_t *queue)
{
    if (!queue) {
        return 0U;
    }
    const size_t discarded = queue->count;
    d1l_ble_companion_queue_init(queue);
    return discarded;
}

d1l_ble_companion_queue_result_t d1l_ble_companion_queue_push(
    d1l_ble_companion_queue_t *queue, const uint8_t *frame, size_t frame_len)
{
    if (!queue || !frame || frame_len == 0U) {
        return D1L_BLE_QUEUE_INVALID_ARGUMENT;
    }
    if (frame_len > D1L_BLE_COMPANION_WIRE_FRAME_MAX) {
        return D1L_BLE_QUEUE_FRAME_TOO_LARGE;
    }
    if (queue->count >= D1L_BLE_COMPANION_QUEUE_DEPTH) {
        return D1L_BLE_QUEUE_FULL;
    }

    d1l_ble_companion_queue_slot_t *slot = &queue->slots[queue->tail];
    memcpy(slot->bytes, frame, frame_len);
    slot->length = frame_len;
    queue->tail =
        (uint8_t)((queue->tail + 1U) % D1L_BLE_COMPANION_QUEUE_DEPTH);
    queue->count++;
    return D1L_BLE_QUEUE_OK;
}

d1l_ble_companion_queue_result_t d1l_ble_companion_queue_peek(
    const d1l_ble_companion_queue_t *queue, uint8_t *dest, size_t dest_cap,
    size_t *out_len)
{
    if (!queue || !dest || !out_len) {
        return D1L_BLE_QUEUE_INVALID_ARGUMENT;
    }
    *out_len = 0U;
    if (queue->count == 0U) {
        return D1L_BLE_QUEUE_EMPTY;
    }
    const d1l_ble_companion_queue_slot_t *slot = &queue->slots[queue->head];
    if (dest_cap < slot->length) {
        *out_len = slot->length;
        return D1L_BLE_QUEUE_DESTINATION_TOO_SMALL;
    }
    memcpy(dest, slot->bytes, slot->length);
    *out_len = slot->length;
    return D1L_BLE_QUEUE_OK;
}

d1l_ble_companion_queue_result_t d1l_ble_companion_queue_pop(
    d1l_ble_companion_queue_t *queue)
{
    if (!queue) {
        return D1L_BLE_QUEUE_INVALID_ARGUMENT;
    }
    if (queue->count == 0U) {
        return D1L_BLE_QUEUE_EMPTY;
    }
    d1l_ble_companion_queue_slot_t *slot = &queue->slots[queue->head];
    memset(slot, 0, sizeof(*slot));
    queue->head =
        (uint8_t)((queue->head + 1U) % D1L_BLE_COMPANION_QUEUE_DEPTH);
    queue->count--;
    return D1L_BLE_QUEUE_OK;
}

d1l_ble_companion_queue_result_t d1l_ble_companion_queue_take(
    d1l_ble_companion_queue_t *queue, uint8_t *dest, size_t dest_cap,
    size_t *out_len)
{
    const d1l_ble_companion_queue_result_t result =
        d1l_ble_companion_queue_peek(queue, dest, dest_cap, out_len);
    if (result != D1L_BLE_QUEUE_OK) {
        return result;
    }
    return d1l_ble_companion_queue_pop(queue);
}
