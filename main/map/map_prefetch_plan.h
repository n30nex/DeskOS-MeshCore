#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D1L_MAP_PREFETCH_MIN_ZOOM 8U
#define D1L_MAP_PREFETCH_MAX_ZOOM 18U
#define D1L_MAP_PREFETCH_ZOOM_COUNT \
    (D1L_MAP_PREFETCH_MAX_ZOOM - D1L_MAP_PREFETCH_MIN_ZOOM + 1U)
#define D1L_MAP_PREFETCH_NODE_RADIUS_KM 200.0
#define D1L_MAP_PREFETCH_PADDING_KM 5.0
#define D1L_MAP_PREFETCH_CARD_RESERVE_KB (8ULL * 1024ULL * 1024ULL)
#define D1L_MAP_PREFETCH_CARD_ALLOCATION_PERCENT 60U

typedef struct {
    int32_t lat_e6;
    int32_t lon_e6;
} d1l_map_prefetch_point_t;

typedef struct {
    uint8_t zoom;
    int32_t first_unwrapped_x;
    int32_t last_unwrapped_x;
    uint32_t first_y;
    uint32_t last_y;
    uint64_t tile_count;
} d1l_map_prefetch_range_t;

typedef struct {
    bool valid;
    int32_t center_lat_e7;
    int32_t center_lon_e7;
    size_t node_count_seen;
    size_t node_count_included;
    size_t node_count_outside_radius;
    uint8_t min_zoom;
    uint8_t max_zoom;
    uint8_t range_count;
    uint32_t average_tile_bytes;
    uint64_t total_tiles;
    uint64_t estimated_bytes;
    uint64_t allocation_bytes;
    uint64_t reserve_bytes;
    d1l_map_prefetch_range_t ranges[D1L_MAP_PREFETCH_ZOOM_COUNT];
} d1l_map_prefetch_plan_t;

/*
 * Builds a deterministic tile pyramid around the configured device location
 * and every valid node no farther than 200 km from it. The highest provider
 * zoom that fits both the configured tile budget and the safe card allocation
 * is selected; eight GiB always remains outside the map allocation on a
 * 32 GB-class or larger card.
 */
bool d1l_map_prefetch_plan_build(
    int32_t center_lat_e7,
    int32_t center_lon_e7,
    const d1l_map_prefetch_point_t *nodes,
    size_t node_count,
    uint32_t card_capacity_kb,
    uint64_t cache_budget_bytes,
    uint32_t average_tile_bytes,
    uint8_t provider_max_zoom,
    d1l_map_prefetch_plan_t *out_plan);

/* Resolves a global center-first-independent plan index without allocating a
 * tile list. X coordinates wrap at the antimeridian. */
bool d1l_map_prefetch_plan_tile_at(
    const d1l_map_prefetch_plan_t *plan,
    uint64_t index,
    uint8_t *out_zoom,
    uint32_t *out_x,
    uint32_t *out_y);
