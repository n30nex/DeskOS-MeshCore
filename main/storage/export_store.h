#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "app/qualification_hooks.h"
#include "hal/rp2040_bridge.h"
#include "storage/storage_status.h"

#define D1L_EXPORT_CANARY_TOKEN_MAX 31U
#define D1L_EXPORT_DIAGNOSTIC_PAYLOAD_MAX 4096U
#define D1L_EXPORT_DATA_PAYLOAD_MAX 12288U

typedef struct {
    char token[D1L_EXPORT_CANARY_TOKEN_MAX + 1U];
    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
    char tmp_path[D1L_RP2040_FILE_PATH_MAX + 1U];
    char step[20];
    size_t bytes;
    bool write_tmp;
    bool read_tmp;
    bool rename_replace;
    bool stat_final;
    bool read_final;
    bool public_rf_tx;
    bool formats_sd;
    uint32_t chunks_written;
    uint32_t chunks_verified_tmp;
    uint32_t chunks_verified_final;
    size_t tmp_verified_bytes;
    size_t final_verified_bytes;
    esp_err_t last_error;
    d1l_rp2040_file_result_t file;
} d1l_export_canary_result_t;

bool d1l_export_store_token_valid(const char *token);
bool d1l_export_store_sd_ready(const d1l_storage_status_t *status);
#if D1L_ENABLE_QUALIFICATION_HOOKS
esp_err_t d1l_export_store_write_canary(const char *token,
                                        const d1l_storage_status_t *status,
                                        d1l_export_canary_result_t *out_result);
#endif
esp_err_t d1l_export_store_write_diagnostics(const char *token,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             const d1l_storage_status_t *status,
                                             d1l_export_canary_result_t *out_result);
esp_err_t d1l_export_store_write_data(const char *token,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      const d1l_storage_status_t *status,
                                      d1l_export_canary_result_t *out_result);
