#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "map/map_math.h"

typedef struct {
    bool initialized;
    bool visible;
    bool worker_running;
    bool frame_ready;
    bool sd_cache_ready;
    bool wifi_connected;
    bool rate_limited;
    bool current_view_only;
    bool provider_configured;
    bool background_prefetch_permitted;
    bool public_rf_tx;
    bool formats_sd;
    uint32_t generation;
    uint32_t frame_revision;
    uint32_t retry_after_sec;
    uint32_t decode_total_us;
    uint32_t decode_max_us;
    int32_t lat_e7;
    int32_t lon_e7;
    uint16_t width;
    uint16_t height;
    uint8_t zoom;
    uint8_t provider_max_zoom;
    uint8_t planned_tiles;
    uint8_t attempted_tiles;
    uint8_t cache_hits;
    uint8_t network_requests;
    uint8_t downloaded_tiles;
    uint8_t rendered_tiles;
    uint8_t failed_tiles;
    uint8_t decode_samples;
    char source_id[25];
    char attribution[65];
    char phase[24];
    char message[80];
} d1l_map_view_status_t;

typedef struct {
    const uint16_t *pixels;
    size_t pixel_count;
    uint16_t width;
    uint16_t height;
    uint32_t generation;
    uint32_t revision;
    uint8_t slot;
    bool held;
} d1l_map_view_frame_t;

esp_err_t d1l_map_view_service_init(void);
esp_err_t d1l_map_view_service_acquire_visible(int32_t lat_e7,
                                               int32_t lon_e7,
                                               uint8_t zoom,
                                               uint16_t width,
                                               uint16_t height,
                                               uint32_t *out_generation);
void d1l_map_view_service_release_visible(uint32_t generation);
void d1l_map_view_service_status(d1l_map_view_status_t *out_status);
esp_err_t d1l_map_view_service_acquire_frame(uint32_t after_revision,
                                             d1l_map_view_frame_t *out_frame);
void d1l_map_view_service_release_frame(d1l_map_view_frame_t *frame);
