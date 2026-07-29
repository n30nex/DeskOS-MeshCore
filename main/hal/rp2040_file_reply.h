#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hal/rp2040_bridge.h"

/* Pure parser for one DESKOS_SD_FILE reply. Kept separate from UART ownership
 * so exact wire responses, including a zero-byte EOF read, are host-testable. */
esp_err_t d1l_rp2040_file_reply_parse(
    const char *line, uint16_t expected_id, const char *expected_op,
    uint8_t *out_data, size_t out_data_size,
    d1l_rp2040_file_result_t *result);

esp_err_t d1l_rp2040_file_reply_bind_read(
    d1l_rp2040_file_result_t *result, uint32_t requested_offset,
    size_t requested_max_len);
esp_err_t d1l_rp2040_file_reply_bind_write(
    d1l_rp2040_file_result_t *result, uint32_t requested_offset,
    size_t requested_len);
esp_err_t d1l_rp2040_file_reply_bind_append(
    d1l_rp2040_file_result_t *result, size_t requested_len);
esp_err_t d1l_rp2040_file_reply_bind_put_begin(
    d1l_rp2040_file_result_t *result, uint32_t expected_size,
    uint32_t expected_crc32);
esp_err_t d1l_rp2040_file_reply_bind_put_chunk(
    d1l_rp2040_file_result_t *result, uint32_t requested_offset,
    size_t requested_len);
esp_err_t d1l_rp2040_file_reply_bind_put_end(
    d1l_rp2040_file_result_t *result, uint32_t expected_size,
    uint32_t expected_crc32);
esp_err_t d1l_rp2040_file_reply_bind_put_abort(
    d1l_rp2040_file_result_t *result);
