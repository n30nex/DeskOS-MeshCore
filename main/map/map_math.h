#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D1L_MAP_TILE_PIXEL_SIZE 256U
#define D1L_MAP_VIEW_MAX_WIDTH 480U
#define D1L_MAP_VIEW_MAX_HEIGHT 480U
#define D1L_MAP_VIEW_MAX_TILES 9U
#define D1L_MAP_VIEW_DEFAULT_ZOOM 10U
#define D1L_MAP_VIEW_MIN_ZOOM 8U
#define D1L_MAP_VIEW_MAX_ZOOM 18U

typedef struct {
    uint8_t zoom;
    uint32_t x;
    uint32_t y;
    int16_t screen_x;
    int16_t screen_y;
} d1l_map_tile_placement_t;

typedef struct {
    int32_t lat_e7;
    int32_t lon_e7;
    uint8_t zoom;
    uint16_t width;
    uint16_t height;
    uint8_t count;
    d1l_map_tile_placement_t tiles[D1L_MAP_VIEW_MAX_TILES];
} d1l_map_tile_plan_t;

bool d1l_map_math_plan_current_view(int32_t lat_e7,
                                    int32_t lon_e7,
                                    uint8_t zoom,
                                    uint16_t width,
                                    uint16_t height,
                                    d1l_map_tile_plan_t *out_plan);

bool d1l_map_math_pan_center(int32_t lat_e7,
                             int32_t lon_e7,
                             uint8_t zoom,
                             int32_t delta_x_pixels,
                             int32_t delta_y_pixels,
                             int32_t *out_lat_e7,
                             int32_t *out_lon_e7);
