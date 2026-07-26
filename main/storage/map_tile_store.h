#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "hal/rp2040_bridge.h"
#include "storage/storage_status.h"

#define D1L_MAP_TILE_CANARY_TOKEN_MAX 31U
#define D1L_MAP_TILE_ZOOM_MAX 18U
#define D1L_MAP_TILE_URL_TEMPLATE_MAX 192U
#define D1L_MAP_TILE_ATTRIBUTION_MAX 64U
#define D1L_MAP_TILE_DOWNLOAD_MAX_BYTES (196U * 1024U)
#define D1L_MAP_TILE_SOURCE_ID "openstreetmap-standard"
#define D1L_MAP_TILE_SOURCE_URL_TEMPLATE "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
#define D1L_MAP_TILE_USER_AGENT "MeshCore-DeskOS-D1L/1.0 (+https://github.com/n30nex/SIGUI)"
#define D1L_MAP_TILE_ATTRIBUTION "\xC2\xA9 OpenStreetMap contributors"
#define D1L_MAP_TILE_LICENSE_URL "https://www.openstreetmap.org/copyright"
#define D1L_MAP_TILE_MIN_CACHE_DAYS 7U
#define D1L_MAP_TILE_CACHE_POLICY "persistent_current_view_cache_min_7_days"
#define D1L_MAP_TILE_CACHE_PATH_TEMPLATE "map/tiles/openstreetmap/z{z}/x{x}/y{y}.png"
#define D1L_MAP_TILE_DOWNLOAD_STATE "current_view_only"
#define D1L_MAP_TILE_DOWNLOAD_REQUIRES "Saved location, connected Wi-Fi, and ready persistent SD cache"
#define D1L_MAP_TILE_PROVIDER_POLICY "built_in_openstreetmap_current_view_only_no_prefetch"
#define D1L_MAP_TILE_PROVIDER_ATTRIBUTION D1L_MAP_TILE_ATTRIBUTION

typedef struct {
    char token[D1L_MAP_TILE_CANARY_TOKEN_MAX + 1U];
    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
    char tmp_path[D1L_RP2040_FILE_PATH_MAX + 1U];
    char step[20];
    uint8_t z;
    uint32_t x;
    uint32_t y;
    size_t bytes;
    bool write_tmp;
    bool read_tmp;
    bool rename_replace;
    bool stat_final;
    bool read_final;
    bool public_rf_tx;
    bool formats_sd;
    esp_err_t last_error;
    d1l_rp2040_file_result_t file;
} d1l_map_tile_canary_result_t;

typedef struct {
    char url[D1L_MAP_TILE_URL_TEMPLATE_MAX + 32U];
    char source_id[25];
    char attribution[D1L_MAP_TILE_ATTRIBUTION_MAX + 1U];
    char license_url[129];
    char path[D1L_RP2040_FILE_PATH_MAX + 1U];
    char tmp_path[D1L_RP2040_FILE_PATH_MAX + 1U];
    char attribution_path[D1L_RP2040_FILE_PATH_MAX + 1U];
    char attribution_tmp_path[D1L_RP2040_FILE_PATH_MAX + 1U];
    char step[24];
    uint8_t z;
    uint32_t x;
    uint32_t y;
    size_t bytes;
    int status_code;
    uint32_t retry_after_sec;
    bool provider_allowed;
    bool provider_configured;
    bool background_prefetch_permitted;
    bool attribution_saved;
    bool cache_hit;
    bool content_type_valid;
    bool png_valid;
    bool cancelled;
    bool wifi_connected;
    bool sd_ready;
    bool write_tmp;
    bool rename_replace;
    bool public_rf_tx;
    bool formats_sd;
    esp_err_t last_error;
    d1l_rp2040_file_result_t file;
} d1l_map_tile_download_result_t;

typedef bool (*d1l_map_tile_continue_cb_t)(void *context);

bool d1l_map_tile_store_token_valid(const char *token);
bool d1l_map_tile_store_sd_ready(const d1l_storage_status_t *status);
bool d1l_map_tile_store_coord_valid(uint8_t z, uint32_t x, uint32_t y);
bool d1l_map_tile_store_path(uint8_t z, uint32_t x, uint32_t y,
                             char *dest, size_t dest_size);
bool d1l_map_tile_png_valid(const uint8_t *data, size_t len);
esp_err_t d1l_map_tile_store_cached(
    uint8_t z,
    uint32_t x,
    uint32_t y,
    const d1l_storage_status_t *status,
    bool *out_cached);
esp_err_t d1l_map_tile_store_read(uint8_t z,
                                  uint32_t x,
                                  uint32_t y,
                                  const d1l_storage_status_t *status,
                                  uint8_t *buffer,
                                  size_t buffer_size,
                                  size_t *out_len,
                                  d1l_map_tile_continue_cb_t should_continue,
                                  void *continue_context,
                                  d1l_map_tile_download_result_t *out_result);
esp_err_t d1l_map_tile_store_fetch(uint8_t z,
                                   uint32_t x,
                                   uint32_t y,
                                   const d1l_storage_status_t *status,
                                   bool wifi_connected,
                                   uint8_t *buffer,
                                   size_t buffer_size,
                                   size_t *out_len,
                                   d1l_map_tile_continue_cb_t should_continue,
                                   void *continue_context,
                                   d1l_map_tile_download_result_t *out_result);
esp_err_t d1l_map_tile_store_write_canary(const char *token,
                                          const d1l_storage_status_t *status,
                                          d1l_map_tile_canary_result_t *out_result);
esp_err_t d1l_map_tile_store_check_canary(const char *token,
                                          const d1l_storage_status_t *status,
                                          d1l_map_tile_canary_result_t *out_result);
