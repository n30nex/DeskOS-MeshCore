#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef uint8_t nvs_open_mode_t;

#define NVS_READWRITE 1U
#define NVS_READONLY 0U

typedef struct {
    size_t used_entries;
    size_t free_entries;
    size_t available_entries;
    size_t total_entries;
    size_t namespace_count;
} nvs_stats_t;

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle);
esp_err_t nvs_open_from_partition(const char *part_name,
                                  const char *namespace_name,
                                  nvs_open_mode_t open_mode,
                                  nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *nvs_stats);
