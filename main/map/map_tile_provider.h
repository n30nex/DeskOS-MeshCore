#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "storage/storage_status.h"

#define D1L_MAP_PROVIDER_SOURCE_ID_MAX 24U
#define D1L_MAP_PROVIDER_URL_MAX 192U
#define D1L_MAP_PROVIDER_ATTRIBUTION_MAX 64U
#define D1L_MAP_PROVIDER_LICENSE_URL_MAX 128U
#define D1L_MAP_PROVIDER_CONFIG_PATH "map/offline-provider.json"
#define D1L_MAP_PROVIDER_CONFIG_MAX_BYTES 1024U
#define D1L_MAP_PROVIDER_BACKUP_PATH_MAX 64U
#define D1L_MAP_PROVIDER_REQUEST_INTERVAL_DEFAULT_MS 250U
#define D1L_MAP_PROVIDER_REQUEST_INTERVAL_MIN_MS 100U
#define D1L_MAP_PROVIDER_REQUEST_INTERVAL_MAX_MS 5000U

typedef struct {
    char source_id[D1L_MAP_PROVIDER_SOURCE_ID_MAX + 1U];
    char url_template[D1L_MAP_PROVIDER_URL_MAX + 1U];
    char attribution[D1L_MAP_PROVIDER_ATTRIBUTION_MAX + 1U];
    char license_url[D1L_MAP_PROVIDER_LICENSE_URL_MAX + 1U];
    bool configured;
    bool network_fetch_allowed;
    bool offline_storage_permitted;
    bool background_prefetch_permitted;
    uint8_t max_zoom;
    uint32_t average_tile_bytes;
    uint32_t minimum_request_interval_ms;
} d1l_map_tile_provider_t;

typedef struct {
    bool mutation_performed;
    bool before_valid;
    bool backup_preserved;
    bool final_valid;
    bool final_builtin_exact;
    size_t before_bytes;
    size_t final_bytes;
    char source_id[D1L_MAP_PROVIDER_SOURCE_ID_MAX + 1U];
    char backup_path[D1L_MAP_PROVIDER_BACKUP_PATH_MAX + 1U];
} d1l_map_provider_repair_result_t;

void d1l_map_tile_provider_builtin(d1l_map_tile_provider_t *out_provider);
void d1l_map_tile_provider_snapshot(d1l_map_tile_provider_t *out_provider);
bool d1l_map_tile_provider_uses_osm_standard(
    const d1l_map_tile_provider_t *provider);
esp_err_t d1l_map_tile_provider_refresh(
    const d1l_storage_status_t *storage);
esp_err_t d1l_map_tile_provider_repair_invalid_default(
    const d1l_storage_status_t *storage,
    d1l_map_provider_repair_result_t *out_result);
bool d1l_map_tile_provider_path(
    const d1l_map_tile_provider_t *provider,
    uint8_t z,
    uint32_t x,
    uint32_t y,
    char *dest,
    size_t dest_size);
bool d1l_map_tile_provider_attribution_path(
    const d1l_map_tile_provider_t *provider,
    bool temporary,
    char *dest,
    size_t dest_size);
