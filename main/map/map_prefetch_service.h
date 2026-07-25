#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "map/map_tile_provider.h"

typedef struct {
    bool initialized;
    bool running;
    bool eligible;
    bool complete;
    bool paused_for_visible_map;
    bool location_set;
    bool wifi_connected;
    bool sd_ready;
    bool provider_configured;
    bool background_prefetch_permitted;
    bool storage_reserve_reached;
    uint8_t selected_max_zoom;
    uint32_t marker_generation;
    uint32_t retry_after_sec;
    uint32_t storage_capacity_kb;
    uint32_t storage_free_kb;
    size_t nodes_seen;
    size_t nodes_included;
    size_t nodes_outside_radius;
    uint64_t total_tiles;
    uint64_t visited_tiles;
    uint64_t cached_tiles;
    uint64_t downloaded_tiles;
    uint64_t failed_tiles;
    uint64_t downloaded_bytes;
    uint64_t estimated_bytes;
    uint64_t allocation_bytes;
    esp_err_t last_error;
    char source_id[D1L_MAP_PROVIDER_SOURCE_ID_MAX + 1U];
    char phase[32];
    char message[96];
} d1l_map_prefetch_status_t;

esp_err_t d1l_map_prefetch_service_init(void);
void d1l_map_prefetch_service_status(
    d1l_map_prefetch_status_t *out_status);
