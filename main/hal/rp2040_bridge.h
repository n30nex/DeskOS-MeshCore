#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define D1L_RP2040_FILE_PROTOCOL_VERSION 1U
#define D1L_RP2040_FILE_LINE_MAX 512U
#define D1L_RP2040_SD_DIAG_LINE_MAX 4096U
#define D1L_RP2040_FILE_PATH_MAX 96U
#define D1L_RP2040_FILE_CHUNK_MAX 192U
#define D1L_RP2040_STOCK_PROBE_MAX_BYTES 192U
#define D1L_RP2040_BAUD_PROBE_MAX_RESULTS 8U

typedef struct {
    bool uart_ready;
    esp_err_t init_result;
    uint32_t buffered_bytes;
    int uart_port;
    int tx_gpio;
    int rx_gpio;
    int baud_rate;
    uint8_t reset_expander_pin;
} d1l_rp2040_status_t;

/* Exclusively hold the UART bridge during a bounded system restart. */
esp_err_t d1l_rp2040_bridge_quiesce_begin(uint32_t timeout_ms);
void d1l_rp2040_bridge_quiesce_end(void);

typedef struct {
    bool bridge_ready;
    bool protocol_supported;
    bool atomic_rename_supported;
    bool stream_write_supported;
    bool response_truncated;
    bool sd_touched;
    uint32_t protocol_version;
    uint32_t file_line_max;
    uint32_t file_chunk_max;
    uint32_t path_max;
    esp_err_t last_error;
    char note[64];
} d1l_rp2040_ping_t;

typedef struct {
    bool bridge_ready;
    bool protocol_supported;
    bool card_present;
    bool filesystem_mounted;
    bool deskos_root_ready;
    bool needs_fat32;
    bool data_ready;
    bool file_ops_supported;
    bool atomic_rename_supported;
    bool response_truncated;
    uint32_t capacity_kb;
    uint32_t free_kb;
    uint32_t file_line_max;
    uint32_t file_chunk_max;
    uint32_t path_max;
    uint32_t probe_error;
    uint32_t probe_data;
    uint32_t mount_error;
    uint32_t mount_data;
    esp_err_t last_error;
    char state[32];
    char filesystem[16];
    char note[96];
    char probe_power[8];
    char probe_mode[16];
} d1l_rp2040_sd_status_t;

typedef struct {
    bool bridge_ready;
    bool protocol_supported;
    bool mount_selected;
    bool response_truncated;
    bool high_dedicated_present;
    bool high_shared_present;
    bool low_dedicated_present;
    bool low_shared_present;
    uint32_t spi_hz;
    uint32_t high_dedicated_error;
    uint32_t high_dedicated_data;
    uint32_t high_dedicated_capacity_kb;
    uint32_t high_shared_error;
    uint32_t high_shared_data;
    uint32_t high_shared_capacity_kb;
    uint32_t low_dedicated_error;
    uint32_t low_dedicated_data;
    uint32_t low_dedicated_capacity_kb;
    uint32_t low_shared_error;
    uint32_t low_shared_data;
    uint32_t low_shared_capacity_kb;
    esp_err_t last_error;
    char pins[40];
    char selected_power[8];
    char selected_mode[16];
    char note[64];
    char raw_line[D1L_RP2040_SD_DIAG_LINE_MAX + 1U];
} d1l_rp2040_sd_diag_t;

typedef struct {
    bool bridge_ready;
    bool protocol_supported;
    bool ok;
    bool response_truncated;
    bool exists;
    bool is_directory;
    bool eof;
    bool removed;
    bool removed_known;
    bool cancelled;
    uint16_t request_id;
    uint32_t size;
    uint32_t offset;
    uint32_t length;
    uint32_t next_offset;
    uint32_t crc32;
    esp_err_t last_error;
    char op[12];
    char err[24];
    char note[48];
} d1l_rp2040_file_result_t;

typedef bool (*d1l_rp2040_file_continue_cb_t)(void *context);

typedef struct {
    bool bridge_ready;
    bool response_seen;
    bool json_seen;
    bool response_truncated;
    uint32_t bytes_read;
    esp_err_t last_error;
    char sent_cobs_hex[8];
    char response_ascii[D1L_RP2040_STOCK_PROBE_MAX_BYTES + 1U];
    char response_hex[(D1L_RP2040_STOCK_PROBE_MAX_BYTES * 2U) + 1U];
    char note[96];
} d1l_rp2040_stock_probe_t;

typedef struct {
    uint32_t baud;
    bool deskos_ping_ok;
    bool stock_response_seen;
    bool stock_json_seen;
    bool stock_response_truncated;
    uint32_t stock_bytes_read;
    esp_err_t ping_error;
    esp_err_t stock_error;
    char stock_response_ascii[65];
    char stock_response_hex[129];
} d1l_rp2040_baud_probe_result_t;

typedef struct {
    bool bridge_ready;
    bool found_deskos;
    bool found_stock;
    uint32_t original_baud;
    uint32_t selected_baud;
    uint32_t tested_count;
    esp_err_t last_error;
    d1l_rp2040_baud_probe_result_t results[D1L_RP2040_BAUD_PROBE_MAX_RESULTS];
} d1l_rp2040_baud_probe_t;

esp_err_t d1l_rp2040_bridge_init(void);
esp_err_t d1l_rp2040_bridge_status(d1l_rp2040_status_t *out_status);
esp_err_t d1l_rp2040_bridge_set_baud(uint32_t baud);
esp_err_t d1l_rp2040_bridge_reset(uint32_t hold_ms, uint32_t settle_ms);
esp_err_t d1l_rp2040_bridge_double_reset(uint32_t hold_ms, uint32_t gap_ms, uint32_t settle_ms);
esp_err_t d1l_rp2040_bridge_ping(d1l_rp2040_ping_t *out_ping, uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_enter_bootloader(uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_stock_probe(d1l_rp2040_stock_probe_t *out_probe, uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_baud_probe(d1l_rp2040_baud_probe_t *out_probe, uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_probe_sd(d1l_rp2040_sd_status_t *out_status, uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_mount_sd(d1l_rp2040_sd_status_t *out_status, uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_sd_diag(d1l_rp2040_sd_diag_t *out_diag, uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_file_stat(const char *path,
                                      d1l_rp2040_file_result_t *out_result,
                                      uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_file_read(const char *path,
                                      uint32_t offset,
                                      uint8_t *out_data,
                                      size_t max_len,
                                      d1l_rp2040_file_result_t *out_result,
                                      uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_file_write(const char *path,
                                       uint32_t offset,
                                       const uint8_t *data,
                                       size_t len,
                                       bool truncate,
                                       d1l_rp2040_file_result_t *out_result,
                                       uint32_t timeout_ms);
/*
 * Streams one complete file through the RP2040-owned verified put session.
 * The operation is all-or-abort, holds sole bridge ownership across the
 * session, and never falls back to legacy chunk writes when unsupported.
 */
esp_err_t d1l_rp2040_bridge_file_write_verified(
    const char *path,
    const uint8_t *data,
    size_t len,
    uint32_t expected_crc32,
    d1l_rp2040_file_continue_cb_t should_continue,
    void *continue_context,
    d1l_rp2040_file_result_t *out_result,
    uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_file_append(const char *path,
                                        const uint8_t *data,
                                        size_t len,
                                        d1l_rp2040_file_result_t *out_result,
                                        uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_file_delete(const char *path,
                                        d1l_rp2040_file_result_t *out_result,
                                        uint32_t timeout_ms);
esp_err_t d1l_rp2040_bridge_file_rename(const char *from_path,
                                        const char *to_path,
                                        bool replace,
                                        d1l_rp2040_file_result_t *out_result,
                                        uint32_t timeout_ms);
