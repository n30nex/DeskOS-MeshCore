#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "comms/ble_companion_queue.h"

static void test_fifo_and_non_destructive_small_destination(void)
{
    d1l_ble_companion_queue_t queue;
    d1l_ble_companion_queue_init(&queue);
    const uint8_t first[] = {'<', 1U, 0U, 0x11U};
    const uint8_t second[] = {'>', 2U, 0U, 0x22U, 0x33U};
    assert(d1l_ble_companion_queue_push(
               &queue, first, sizeof(first)) == D1L_BLE_QUEUE_OK);
    assert(d1l_ble_companion_queue_push(
               &queue, second, sizeof(second)) == D1L_BLE_QUEUE_OK);

    uint8_t output[D1L_BLE_COMPANION_WIRE_FRAME_MAX] = {0};
    size_t output_len = 0U;
    assert(d1l_ble_companion_queue_take(
               &queue, output, sizeof(first) - 1U, &output_len) ==
           D1L_BLE_QUEUE_DESTINATION_TOO_SMALL);
    assert(output_len == sizeof(first));
    assert(queue.count == 2U);

    assert(d1l_ble_companion_queue_take(
               &queue, output, sizeof(output), &output_len) ==
           D1L_BLE_QUEUE_OK);
    assert(output_len == sizeof(first));
    assert(memcmp(output, first, sizeof(first)) == 0);
    assert(d1l_ble_companion_queue_take(
               &queue, output, sizeof(output), &output_len) ==
           D1L_BLE_QUEUE_OK);
    assert(output_len == sizeof(second));
    assert(memcmp(output, second, sizeof(second)) == 0);
    assert(d1l_ble_companion_queue_take(
               &queue, output, sizeof(output), &output_len) ==
           D1L_BLE_QUEUE_EMPTY);
}

static void test_queue_is_bounded_and_wraps(void)
{
    d1l_ble_companion_queue_t queue;
    d1l_ble_companion_queue_init(&queue);
    uint8_t frame[] = {'<', 1U, 0U, 0U};
    for (uint8_t index = 0U;
         index < D1L_BLE_COMPANION_QUEUE_DEPTH; ++index) {
        frame[3] = index;
        assert(d1l_ble_companion_queue_push(
                   &queue, frame, sizeof(frame)) == D1L_BLE_QUEUE_OK);
    }
    assert(d1l_ble_companion_queue_push(
               &queue, frame, sizeof(frame)) == D1L_BLE_QUEUE_FULL);

    uint8_t output[8] = {0};
    size_t output_len = 0U;
    for (uint8_t expected = 0U;
         expected < D1L_BLE_COMPANION_QUEUE_DEPTH; ++expected) {
        assert(d1l_ble_companion_queue_take(
                   &queue, output, sizeof(output), &output_len) ==
               D1L_BLE_QUEUE_OK);
        assert(output[3] == expected);
        frame[3] = (uint8_t)(expected + D1L_BLE_COMPANION_QUEUE_DEPTH);
        assert(d1l_ble_companion_queue_push(
                   &queue, frame, sizeof(frame)) == D1L_BLE_QUEUE_OK);
    }
    for (uint8_t expected = D1L_BLE_COMPANION_QUEUE_DEPTH;
         expected < 2U * D1L_BLE_COMPANION_QUEUE_DEPTH; ++expected) {
        assert(d1l_ble_companion_queue_take(
                   &queue, output, sizeof(output), &output_len) ==
               D1L_BLE_QUEUE_OK);
        assert(output[3] == expected);
    }
}

static void test_invalid_and_clear_paths(void)
{
    d1l_ble_companion_queue_t queue;
    d1l_ble_companion_queue_init(&queue);
    uint8_t frame[D1L_BLE_COMPANION_WIRE_FRAME_MAX + 1U] = {0};
    assert(d1l_ble_companion_queue_push(
               NULL, frame, 1U) == D1L_BLE_QUEUE_INVALID_ARGUMENT);
    assert(d1l_ble_companion_queue_push(
               &queue, NULL, 1U) == D1L_BLE_QUEUE_INVALID_ARGUMENT);
    assert(d1l_ble_companion_queue_push(
               &queue, frame, 0U) == D1L_BLE_QUEUE_INVALID_ARGUMENT);
    assert(d1l_ble_companion_queue_push(
               &queue, frame, sizeof(frame)) ==
           D1L_BLE_QUEUE_FRAME_TOO_LARGE);
    assert(d1l_ble_companion_queue_push(
               &queue, frame, 3U) == D1L_BLE_QUEUE_OK);
    assert(d1l_ble_companion_queue_clear(&queue) == 1U);
    assert(queue.count == 0U);
    assert(d1l_ble_companion_queue_clear(NULL) == 0U);
}

int main(void)
{
    test_fifo_and_non_destructive_small_destination();
    test_queue_is_bounded_and_wraps();
    test_invalid_and_clear_paths();
    puts("native BLE companion queue: ok");
    return 0;
}
