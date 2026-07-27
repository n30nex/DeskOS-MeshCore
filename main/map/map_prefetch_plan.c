#include "map_prefetch_plan.h"

#include <math.h>
#include <string.h>

#define D1L_MAP_PREFETCH_PI 3.14159265358979323846
#define D1L_MAP_PREFETCH_EARTH_RADIUS_KM 6371.0088
#define D1L_MAP_PREFETCH_MERCATOR_MAX_LAT_DEG 85.05112878

static double clamp_double(double value, double low, double high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static double radians(double degrees)
{
    return degrees * D1L_MAP_PREFETCH_PI / 180.0;
}

static double wrap_delta_longitude(double delta)
{
    while (delta > 180.0) {
        delta -= 360.0;
    }
    while (delta < -180.0) {
        delta += 360.0;
    }
    return delta;
}

static double distance_km(double lat_a,
                          double lon_a,
                          double lat_b,
                          double lon_b)
{
    const double lat_delta = radians(lat_b - lat_a);
    const double lon_delta =
        radians(wrap_delta_longitude(lon_b - lon_a));
    const double sin_lat = sin(lat_delta * 0.5);
    const double sin_lon = sin(lon_delta * 0.5);
    const double haversine =
        (sin_lat * sin_lat) +
        cos(radians(lat_a)) * cos(radians(lat_b)) *
            (sin_lon * sin_lon);
    const double bounded = clamp_double(haversine, 0.0, 1.0);
    return 2.0 * D1L_MAP_PREFETCH_EARTH_RADIUS_KM *
           atan2(sqrt(bounded), sqrt(1.0 - bounded));
}

static bool point_valid(const d1l_map_prefetch_point_t *point)
{
    return point &&
           point->lat_e6 >= -90000000 &&
           point->lat_e6 <= 90000000 &&
           point->lon_e6 >= -180000000 &&
           point->lon_e6 <= 180000000;
}

static double tile_x_unwrapped(double longitude, uint32_t tile_count)
{
    return ((longitude + 180.0) / 360.0) * (double)tile_count;
}

static double tile_y(double latitude, uint32_t tile_count)
{
    const double bounded_latitude = clamp_double(
        latitude, -D1L_MAP_PREFETCH_MERCATOR_MAX_LAT_DEG,
        D1L_MAP_PREFETCH_MERCATOR_MAX_LAT_DEG);
    const double latitude_rad = radians(bounded_latitude);
    return (1.0 -
            log(tan(latitude_rad) + (1.0 / cos(latitude_rad))) /
                D1L_MAP_PREFETCH_PI) *
           0.5 * (double)tile_count;
}

static uint32_t clamp_tile_y(int64_t value, uint32_t tile_count)
{
    if (value < 0) {
        return 0U;
    }
    if ((uint64_t)value >= tile_count) {
        return tile_count - 1U;
    }
    return (uint32_t)value;
}

static bool build_range(double min_latitude,
                        double max_latitude,
                        double min_unwrapped_longitude,
                        double max_unwrapped_longitude,
                        uint8_t zoom,
                        d1l_map_prefetch_range_t *out_range)
{
    if (!out_range || zoom > D1L_MAP_PREFETCH_MAX_ZOOM ||
        min_latitude > max_latitude ||
        min_unwrapped_longitude > max_unwrapped_longitude) {
        return false;
    }
    const uint32_t tile_count = 1UL << zoom;
    int64_t first_x =
        (int64_t)floor(tile_x_unwrapped(
            min_unwrapped_longitude, tile_count));
    int64_t last_x =
        (int64_t)floor(tile_x_unwrapped(
            max_unwrapped_longitude, tile_count));
    if (last_x - first_x + 1LL > (int64_t)tile_count) {
        last_x = first_x + (int64_t)tile_count - 1LL;
    }
    const uint32_t first_y = clamp_tile_y(
        (int64_t)floor(tile_y(max_latitude, tile_count)), tile_count);
    const uint32_t last_y = clamp_tile_y(
        (int64_t)floor(tile_y(min_latitude, tile_count)), tile_count);
    if (first_x < INT32_MIN || last_x > INT32_MAX ||
        last_x < first_x || last_y < first_y) {
        return false;
    }
    const uint64_t x_count = (uint64_t)(last_x - first_x + 1LL);
    const uint64_t y_count = (uint64_t)last_y - first_y + 1ULL;
    if (x_count == 0U || y_count == 0U ||
        x_count > UINT64_MAX / y_count) {
        return false;
    }
    *out_range = (d1l_map_prefetch_range_t) {
        .zoom = zoom,
        .first_unwrapped_x = (int32_t)first_x,
        .last_unwrapped_x = (int32_t)last_x,
        .first_y = first_y,
        .last_y = last_y,
        .tile_count = x_count * y_count,
    };
    return true;
}

static uint64_t map_allocation_bytes(uint32_t card_capacity_kb,
                                     uint64_t cache_budget_bytes)
{
    const uint64_t capacity_bytes =
        (uint64_t)card_capacity_kb * 1024ULL;
    const uint64_t reserve_bytes =
        D1L_MAP_PREFETCH_CARD_RESERVE_KB * 1024ULL;
    if (capacity_bytes <= reserve_bytes) {
        return 0U;
    }
    const uint64_t percent_bytes =
        (capacity_bytes / 100ULL) *
        D1L_MAP_PREFETCH_CARD_ALLOCATION_PERCENT;
    const uint64_t after_reserve = capacity_bytes - reserve_bytes;
    const uint64_t card_allocation =
        percent_bytes < after_reserve ?
        percent_bytes : after_reserve;
    return cache_budget_bytes < card_allocation ?
        cache_budget_bytes : card_allocation;
}

static bool priority_cluster_cell(
    const d1l_map_prefetch_point_t *point,
    uint8_t zoom,
    uint32_t *out_x,
    uint32_t *out_y)
{
    if (!point_valid(point) || !out_x || !out_y ||
        zoom > D1L_MAP_PREFETCH_MAX_ZOOM) {
        return false;
    }
    const uint32_t tile_count = 1UL << zoom;
    int64_t x = (int64_t)floor(tile_x_unwrapped(
        (double)point->lon_e6 / 1000000.0, tile_count));
    x %= (int64_t)tile_count;
    if (x < 0) {
        x += tile_count;
    }
    *out_x = (uint32_t)x;
    *out_y = clamp_tile_y(
        (int64_t)floor(tile_y(
            (double)point->lat_e6 / 1000000.0, tile_count)),
        tile_count);
    return true;
}

static uint32_t priority_cluster_hash(uint32_t x, uint32_t y)
{
    return (x * UINT32_C(2654435761)) ^
           (y * UINT32_C(2246822519));
}

static uint8_t select_priority_focuses(
    d1l_map_prefetch_plan_t *plan,
    const d1l_map_prefetch_point_t *center,
    const d1l_map_prefetch_point_t *viewport,
    const d1l_map_prefetch_point_t *nodes,
    size_t node_count,
    uint8_t focus_zoom)
{
    if (!plan || !point_valid(center)) {
        return 0U;
    }
    plan->priority_focuses[0] = *center;
    uint32_t focus_hashes[D1L_MAP_PREFETCH_PRIORITY_FOCUS_MAX] = {0};
    uint32_t focus_cells_x[D1L_MAP_PREFETCH_PRIORITY_FOCUS_MAX] = {0};
    uint32_t focus_cells_y[D1L_MAP_PREFETCH_PRIORITY_FOCUS_MAX] = {0};
    if (!priority_cluster_cell(
            center, focus_zoom,
            &focus_cells_x[0], &focus_cells_y[0])) {
        return 0U;
    }
    uint8_t focus_count = 1U;
    if (point_valid(viewport)) {
        plan->priority_includes_viewport = true;
        uint32_t center_x = 0U;
        uint32_t center_y = 0U;
        uint32_t viewport_x = 0U;
        uint32_t viewport_y = 0U;
        center_x = focus_cells_x[0];
        center_y = focus_cells_y[0];
        if (priority_cluster_cell(
                viewport, focus_zoom,
                &viewport_x, &viewport_y) &&
            (center_x != viewport_x ||
             center_y != viewport_y)) {
            plan->priority_focuses[focus_count] = *viewport;
            focus_cells_x[focus_count] = viewport_x;
            focus_cells_y[focus_count] = viewport_y;
            ++focus_count;
        }
    }
    const uint8_t reserved_focus_count = focus_count;

    const double center_latitude =
        (double)center->lat_e6 / 1000000.0;
    const double center_longitude =
        (double)center->lon_e6 / 1000000.0;
    for (size_t i = 0U; nodes && i < node_count; ++i) {
        if (!point_valid(&nodes[i])) {
            continue;
        }
        const double node_latitude =
            (double)nodes[i].lat_e6 / 1000000.0;
        const double node_longitude =
            (double)nodes[i].lon_e6 / 1000000.0;
        if (distance_km(
                center_latitude, center_longitude,
                node_latitude, node_longitude) >
            D1L_MAP_PREFETCH_NODE_RADIUS_KM) {
            continue;
        }
        uint32_t cell_x = 0U;
        uint32_t cell_y = 0U;
        if (!priority_cluster_cell(
                &nodes[i], focus_zoom, &cell_x, &cell_y)) {
            continue;
        }
        bool covered = false;
        for (uint8_t focus = 0U;
             focus < focus_count; ++focus) {
            if (focus_cells_x[focus] == cell_x &&
                focus_cells_y[focus] == cell_y) {
                if (focus >= reserved_focus_count &&
                    (nodes[i].lat_e6 <
                         plan->priority_focuses[focus].lat_e6 ||
                     (nodes[i].lat_e6 ==
                          plan->priority_focuses[focus].lat_e6 &&
                      nodes[i].lon_e6 <
                          plan->priority_focuses[focus].lon_e6))) {
                    plan->priority_focuses[focus] = nodes[i];
                }
                covered = true;
                break;
            }
        }
        if (covered) {
            continue;
        }
        const uint32_t hash =
            priority_cluster_hash(cell_x, cell_y);
        if (focus_count < D1L_MAP_PREFETCH_PRIORITY_FOCUS_MAX) {
            plan->priority_focuses[focus_count] = nodes[i];
            focus_hashes[focus_count] = hash;
            focus_cells_x[focus_count] = cell_x;
            focus_cells_y[focus_count] = cell_y;
            ++focus_count;
            continue;
        }
        uint8_t replace = reserved_focus_count;
        for (uint8_t focus =
                 (uint8_t)(reserved_focus_count + 1U);
             focus < focus_count; ++focus) {
            if (focus_hashes[focus] >
                focus_hashes[replace]) {
                replace = focus;
            }
        }
        if (hash < focus_hashes[replace]) {
            plan->priority_focuses[replace] = nodes[i];
            focus_hashes[replace] = hash;
            focus_cells_x[replace] = cell_x;
            focus_cells_y[replace] = cell_y;
        }
    }
    return focus_count;
}

bool d1l_map_prefetch_plan_build_with_viewport(
    int32_t center_lat_e7,
    int32_t center_lon_e7,
    const d1l_map_prefetch_point_t *nodes,
    size_t node_count,
    const d1l_map_prefetch_point_t *viewport,
    uint32_t card_capacity_kb,
    uint64_t cache_budget_bytes,
    uint32_t average_tile_bytes,
    uint8_t provider_max_zoom,
    d1l_map_prefetch_plan_t *out_plan)
{
    if (!out_plan ||
        center_lat_e7 < -900000000 ||
        center_lat_e7 > 900000000 ||
        center_lon_e7 < -1800000000LL ||
        center_lon_e7 > 1800000000LL ||
        cache_budget_bytes == 0U ||
        average_tile_bytes == 0U ||
        provider_max_zoom < D1L_MAP_PREFETCH_MIN_ZOOM) {
        return false;
    }
    memset(out_plan, 0, sizeof(*out_plan));
    out_plan->center_lat_e7 = center_lat_e7;
    out_plan->center_lon_e7 = center_lon_e7;
    out_plan->node_count_seen = node_count;
    out_plan->average_tile_bytes = average_tile_bytes;
    out_plan->min_zoom = D1L_MAP_PREFETCH_MIN_ZOOM;
    out_plan->reserve_bytes =
        D1L_MAP_PREFETCH_CARD_RESERVE_KB * 1024ULL;
    out_plan->allocation_bytes =
        map_allocation_bytes(card_capacity_kb, cache_budget_bytes);
    if (out_plan->allocation_bytes == 0U) {
        return false;
    }

    const double center_geographic_latitude =
        (double)center_lat_e7 / 10000000.0;
    const double center_latitude =
        clamp_double(center_geographic_latitude,
                     -D1L_MAP_PREFETCH_MERCATOR_MAX_LAT_DEG,
                     D1L_MAP_PREFETCH_MERCATOR_MAX_LAT_DEG);
    const double center_longitude =
        (double)center_lon_e7 / 10000000.0;
    double min_latitude = center_latitude;
    double max_latitude = center_latitude;
    double min_longitude = center_longitude;
    double max_longitude = center_longitude;

    for (size_t i = 0U; nodes && i < node_count; ++i) {
        if (!point_valid(&nodes[i])) {
            continue;
        }
        const double latitude = (double)nodes[i].lat_e6 / 1000000.0;
        const double longitude = (double)nodes[i].lon_e6 / 1000000.0;
        if (distance_km(center_geographic_latitude, center_longitude,
                        latitude, longitude) >
            D1L_MAP_PREFETCH_NODE_RADIUS_KM) {
            ++out_plan->node_count_outside_radius;
            continue;
        }
        const double unwrapped_longitude =
            center_longitude +
            wrap_delta_longitude(longitude - center_longitude);
        const double bounded_latitude =
            clamp_double(latitude,
                         -D1L_MAP_PREFETCH_MERCATOR_MAX_LAT_DEG,
                         D1L_MAP_PREFETCH_MERCATOR_MAX_LAT_DEG);
        if (bounded_latitude < min_latitude) {
            min_latitude = bounded_latitude;
        }
        if (bounded_latitude > max_latitude) {
            max_latitude = bounded_latitude;
        }
        if (unwrapped_longitude < min_longitude) {
            min_longitude = unwrapped_longitude;
        }
        if (unwrapped_longitude > max_longitude) {
            max_longitude = unwrapped_longitude;
        }
        ++out_plan->node_count_included;
    }

    const double latitude_padding =
        D1L_MAP_PREFETCH_PADDING_KM / 111.32;
    double cosine = fabs(cos(radians(center_latitude)));
    if (cosine < 0.01) {
        cosine = 0.01;
    }
    double longitude_padding =
        D1L_MAP_PREFETCH_PADDING_KM / (111.32 * cosine);
    if (longitude_padding > 180.0) {
        longitude_padding = 180.0;
    }
    min_latitude = clamp_double(
        min_latitude - latitude_padding,
        -D1L_MAP_PREFETCH_MERCATOR_MAX_LAT_DEG,
        D1L_MAP_PREFETCH_MERCATOR_MAX_LAT_DEG);
    max_latitude = clamp_double(
        max_latitude + latitude_padding,
        -D1L_MAP_PREFETCH_MERCATOR_MAX_LAT_DEG,
        D1L_MAP_PREFETCH_MERCATOR_MAX_LAT_DEG);
    min_longitude -= longitude_padding;
    max_longitude += longitude_padding;

    const uint8_t bounded_provider_zoom =
        provider_max_zoom > D1L_MAP_PREFETCH_MAX_ZOOM ?
            D1L_MAP_PREFETCH_MAX_ZOOM : provider_max_zoom;
    uint64_t cumulative_tiles = 0U;
    uint8_t selected_count = 0U;
    for (uint8_t zoom = D1L_MAP_PREFETCH_MIN_ZOOM;
         zoom <= bounded_provider_zoom; ++zoom) {
        d1l_map_prefetch_range_t range = {0};
        if (!build_range(min_latitude, max_latitude,
                         min_longitude, max_longitude,
                         zoom, &range) ||
            cumulative_tiles > UINT64_MAX - range.tile_count) {
            break;
        }
        const uint64_t candidate_tiles =
            cumulative_tiles + range.tile_count;
        if (candidate_tiles >
            UINT64_MAX / (uint64_t)average_tile_bytes) {
            break;
        }
        const uint64_t candidate_bytes =
            candidate_tiles * (uint64_t)average_tile_bytes;
        if (candidate_bytes > out_plan->allocation_bytes) {
            break;
        }
        out_plan->ranges[selected_count++] = range;
        cumulative_tiles = candidate_tiles;
        out_plan->estimated_bytes = candidate_bytes;
        out_plan->max_zoom = zoom;
    }
    if (selected_count == 0U) {
        memset(out_plan, 0, sizeof(*out_plan));
        return false;
    }
    out_plan->range_count = selected_count;
    out_plan->base_tiles = cumulative_tiles;

    const uint64_t tile_capacity =
        out_plan->allocation_bytes / (uint64_t)average_tile_bytes;
    if (out_plan->max_zoom < bounded_provider_zoom &&
        cumulative_tiles < tile_capacity) {
        const uint8_t first_priority_zoom =
            (uint8_t)(out_plan->max_zoom + 1U);
        const uint8_t focus_zoom =
            first_priority_zoom < 12U ?
                first_priority_zoom : 12U;
        const d1l_map_prefetch_point_t center = {
            .lat_e6 = center_lat_e7 / 10,
            .lon_e6 = center_lon_e7 / 10,
        };
        out_plan->priority_focus_zoom = focus_zoom;
        out_plan->priority_focus_count = select_priority_focuses(
            out_plan, &center, viewport, nodes, node_count,
            focus_zoom);
        uint64_t remaining_tiles =
            tile_capacity - cumulative_tiles;
        for (uint8_t zoom = first_priority_zoom;
             out_plan->priority_focus_count > 0U &&
             remaining_tiles > 0U &&
             zoom <= bounded_provider_zoom; ++zoom) {
            const uint8_t zoom_delta =
                (uint8_t)(zoom - focus_zoom);
            const uint64_t side = 1ULL << zoom_delta;
            const uint64_t per_focus = side * side;
            const uint64_t level_candidates =
                per_focus *
                (uint64_t)out_plan->priority_focus_count;
            const uint64_t planned =
                remaining_tiles < level_candidates ?
                    remaining_tiles : level_candidates;
            out_plan->priority_zoom_tile_counts[
                zoom - D1L_MAP_PREFETCH_MIN_ZOOM] = planned;
            if (out_plan->priority_tiles == 0U) {
                out_plan->priority_min_zoom = zoom;
            }
            out_plan->priority_tiles += planned;
            out_plan->priority_max_zoom = zoom;
            out_plan->max_zoom = zoom;
            remaining_tiles -= planned;
            if (planned < level_candidates) {
                break;
            }
        }
    }
    out_plan->total_tiles =
        out_plan->base_tiles + out_plan->priority_tiles;
    out_plan->estimated_bytes =
        out_plan->total_tiles * (uint64_t)average_tile_bytes;
    out_plan->valid = true;
    return true;
}

bool d1l_map_prefetch_plan_build(
    int32_t center_lat_e7,
    int32_t center_lon_e7,
    const d1l_map_prefetch_point_t *nodes,
    size_t node_count,
    uint32_t card_capacity_kb,
    uint64_t cache_budget_bytes,
    uint32_t average_tile_bytes,
    uint8_t provider_max_zoom,
    d1l_map_prefetch_plan_t *out_plan)
{
    return d1l_map_prefetch_plan_build_with_viewport(
        center_lat_e7, center_lon_e7, nodes, node_count, NULL,
        card_capacity_kb, cache_budget_bytes, average_tile_bytes,
        provider_max_zoom, out_plan);
}

static uint32_t wrap_x(int64_t value, uint32_t tile_count)
{
    int64_t wrapped = value % (int64_t)tile_count;
    if (wrapped < 0) {
        wrapped += tile_count;
    }
    return (uint32_t)wrapped;
}

bool d1l_map_prefetch_plan_tile_at(
    const d1l_map_prefetch_plan_t *plan,
    uint64_t index,
    uint8_t *out_zoom,
    uint32_t *out_x,
    uint32_t *out_y)
{
    if (!plan || !plan->valid || !out_zoom || !out_x || !out_y ||
        index >= plan->total_tiles) {
        return false;
    }
    if (index < plan->base_tiles) {
        for (uint8_t i = 0U; i < plan->range_count; ++i) {
            const d1l_map_prefetch_range_t *range = &plan->ranges[i];
            if (index >= range->tile_count) {
                index -= range->tile_count;
                continue;
            }
            const uint64_t x_count =
                (uint64_t)((int64_t)range->last_unwrapped_x -
                           range->first_unwrapped_x + 1LL);
            const uint64_t y_offset = index / x_count;
            const uint64_t x_offset = index % x_count;
            const int64_t unwrapped_x =
                (int64_t)range->first_unwrapped_x +
                (int64_t)x_offset;
            const uint32_t tile_count = 1UL << range->zoom;
            *out_zoom = range->zoom;
            *out_x = wrap_x(unwrapped_x, tile_count);
            *out_y = range->first_y + (uint32_t)y_offset;
            return *out_y <= range->last_y;
        }
        return false;
    }
    index -= plan->base_tiles;
    if (index >= plan->priority_tiles ||
        plan->priority_focus_count == 0U ||
        plan->priority_min_zoom > plan->priority_max_zoom) {
        return false;
    }
    for (uint8_t zoom = plan->priority_min_zoom;
         zoom <= plan->priority_max_zoom; ++zoom) {
        const uint64_t level_count =
            plan->priority_zoom_tile_counts[
                zoom - D1L_MAP_PREFETCH_MIN_ZOOM];
        if (index >= level_count) {
            index -= level_count;
            continue;
        }
        const uint8_t focus_index = (uint8_t)(
            index % plan->priority_focus_count);
        const uint64_t child_index =
            index / plan->priority_focus_count;
        const d1l_map_prefetch_point_t *focus =
            &plan->priority_focuses[focus_index];
        uint32_t owner_x = 0U;
        uint32_t owner_y = 0U;
        if (!priority_cluster_cell(
                focus, plan->priority_focus_zoom,
                &owner_x, &owner_y)) {
            return false;
        }
        const uint8_t zoom_delta =
            (uint8_t)(zoom - plan->priority_focus_zoom);
        const uint32_t side = 1UL << zoom_delta;
        const uint32_t first_x = owner_x << zoom_delta;
        const uint32_t first_y = owner_y << zoom_delta;
        const uint32_t tile_count = 1UL << zoom;
        const uint32_t focus_x = wrap_x(
            (int64_t)floor(tile_x_unwrapped(
                (double)focus->lon_e6 / 1000000.0,
                tile_count)),
            tile_count);
        const uint32_t focus_y = clamp_tile_y(
            (int64_t)floor(tile_y(
                (double)focus->lat_e6 / 1000000.0,
                tile_count)),
            tile_count);
        const uint64_t focus_linear =
            (uint64_t)(focus_y - first_y) * side +
            (uint64_t)(focus_x - first_x);
        uint64_t linear = 0U;
        if (child_index == 0U) {
            linear = focus_linear;
        } else {
            linear = child_index - 1U;
            if (linear >= focus_linear) {
                ++linear;
            }
        }
        if (linear >= (uint64_t)side * side) {
            return false;
        }
        *out_zoom = zoom;
        *out_x = first_x + (uint32_t)(linear % side);
        *out_y = first_y + (uint32_t)(linear / side);
        return true;
    }
    return false;
}
