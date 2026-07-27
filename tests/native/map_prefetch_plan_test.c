#include <assert.h>
#include <stdint.h>

#include "map/map_prefetch_plan.h"

#define CARD_32GB_CLASS_KB (31U * 1024U * 1024U)

static void test_local_area_uses_highest_provider_detail(void)
{
    d1l_map_prefetch_plan_t plan = {0};
    assert(d1l_map_prefetch_plan_build(
        434279770, -803164780, NULL, 0U,
        CARD_32GB_CLASS_KB, 18ULL * 1024ULL * 1024ULL * 1024ULL,
        65536U, 18U, &plan));
    assert(plan.valid);
    assert(plan.min_zoom == 8U);
    assert(plan.max_zoom == 18U);
    assert(plan.range_count == 11U);
    assert(plan.node_count_included == 0U);
    assert(plan.total_tiles > 0U);
    assert(plan.estimated_bytes <= plan.allocation_bytes);
    assert(plan.reserve_bytes == 8ULL * 1024ULL * 1024ULL * 1024ULL);
}

static void test_radius_filter_and_capacity_select_detail(void)
{
    const d1l_map_prefetch_point_t nodes[] = {
        {.lat_e6 = 1796000, .lon_e6 = 0},
        {.lat_e6 = -1796000, .lon_e6 = 0},
        {.lat_e6 = 0, .lon_e6 = 1796000},
        {.lat_e6 = 0, .lon_e6 = -1796000},
        {.lat_e6 = 3000000, .lon_e6 = 0},
    };
    d1l_map_prefetch_plan_t plan = {0};
    assert(d1l_map_prefetch_plan_build(
        0, 0, nodes, sizeof(nodes) / sizeof(nodes[0]),
        CARD_32GB_CLASS_KB, 18ULL * 1024ULL * 1024ULL * 1024ULL,
        65536U, 18U, &plan));
    assert(plan.node_count_included == 4U);
    assert(plan.node_count_outside_radius == 1U);
    assert(plan.max_zoom == 18U);
    assert(plan.priority_tiles > 0U);
    assert(plan.priority_max_zoom == 18U);
    assert(plan.priority_min_zoom < plan.priority_max_zoom);
    assert(plan.estimated_bytes <= plan.allocation_bytes);
}

static void test_provider_limit_and_antimeridian_wrap(void)
{
    const d1l_map_prefetch_point_t nodes[] = {
        {.lat_e6 = 0, .lon_e6 = -179900000},
    };
    d1l_map_prefetch_plan_t plan = {0};
    assert(d1l_map_prefetch_plan_build(
        0, 1799000000, nodes, 1U,
        CARD_32GB_CLASS_KB, 18ULL * 1024ULL * 1024ULL * 1024ULL,
        65536U, 14U, &plan));
    assert(plan.node_count_included == 1U);
    assert(plan.max_zoom == 14U);
    assert(plan.range_count == 7U);

    uint64_t visited = 0U;
    for (; visited < plan.total_tiles; ++visited) {
        uint8_t zoom = 0U;
        uint32_t x = 0U;
        uint32_t y = 0U;
        assert(d1l_map_prefetch_plan_tile_at(
            &plan, visited, &zoom, &x, &y));
        assert(zoom >= plan.min_zoom && zoom <= plan.max_zoom);
        assert(x < (1UL << zoom));
        assert(y < (1UL << zoom));
    }
    uint8_t zoom = 0U;
    uint32_t x = 0U;
    uint32_t y = 0U;
    assert(!d1l_map_prefetch_plan_tile_at(
        &plan, visited, &zoom, &x, &y));
}

static void test_invalid_and_reserved_capacity_fail_closed(void)
{
    d1l_map_prefetch_plan_t plan = {0};
    assert(!d1l_map_prefetch_plan_build(
        0, 0, NULL, 0U,
        8U * 1024U * 1024U, 1024U, 65536U, 18U, &plan));
    assert(!d1l_map_prefetch_plan_build(
        0, 0, NULL, 0U,
        CARD_32GB_CLASS_KB, 1024U, 0U, 18U, &plan));
    assert(!d1l_map_prefetch_plan_build(
        0, 0, NULL, 0U,
        CARD_32GB_CLASS_KB, 1024U, 65536U, 7U, &plan));
}

static void test_configured_cache_budget_caps_card_allocation(void)
{
    const uint64_t budget = 64ULL * 1024ULL * 1024ULL;
    d1l_map_prefetch_plan_t plan = {0};
    assert(d1l_map_prefetch_plan_build(
        434279770, -803164780, NULL, 0U,
        CARD_32GB_CLASS_KB, budget, 65536U, 18U, &plan));
    assert(plan.valid);
    assert(plan.allocation_bytes == budget);
    assert(plan.estimated_bytes == budget);
    assert(plan.max_zoom > plan.ranges[plan.range_count - 1U].zoom);
    assert(plan.max_zoom <= 18U);
    assert(plan.priority_tiles > 0U);
}

static void test_remaining_budget_prioritizes_device_nodes_and_viewport(void)
{
    const d1l_map_prefetch_point_t nodes[] = {
        {.lat_e6 = 43000000, .lon_e6 = -80800000},
        {.lat_e6 = 44000000, .lon_e6 = -79800000},
    };
    const d1l_map_prefetch_point_t viewport = {
        .lat_e6 = 43700000,
        .lon_e6 = -80600000,
    };
    const uint64_t budget = 64ULL * 1024ULL * 1024ULL;
    d1l_map_prefetch_plan_t plan = {0};
    assert(d1l_map_prefetch_plan_build_with_viewport(
        434279770, -803164780, nodes,
        sizeof(nodes) / sizeof(nodes[0]), &viewport,
        CARD_32GB_CLASS_KB, budget, 65536U, 18U, &plan));
    assert(plan.valid);
    assert(plan.base_tiles > 0U);
    assert(plan.priority_tiles > 0U);
    assert(plan.total_tiles == budget / 65536U);
    assert(plan.estimated_bytes == budget);
    assert(plan.max_zoom >= plan.priority_min_zoom);
    assert(plan.max_zoom <= 18U);
    assert(plan.priority_focus_count >= 3U);
    assert(plan.priority_includes_viewport);
    for (uint8_t zoom = plan.priority_min_zoom;
         zoom <= plan.priority_max_zoom; ++zoom) {
        assert(plan.priority_zoom_tile_counts[
            zoom - D1L_MAP_PREFETCH_MIN_ZOOM] > 0U);
    }

    uint8_t first_zoom = 0U;
    uint32_t first_x = 0U;
    uint32_t first_y = 0U;
    uint8_t second_zoom = 0U;
    uint32_t second_x = 0U;
    uint32_t second_y = 0U;
    assert(d1l_map_prefetch_plan_tile_at(
        &plan, plan.base_tiles,
        &first_zoom, &first_x, &first_y));
    assert(d1l_map_prefetch_plan_tile_at(
        &plan, plan.base_tiles + 1U,
        &second_zoom, &second_x, &second_y));
    assert(first_zoom == plan.priority_min_zoom);
    assert(second_zoom == plan.priority_min_zoom);
    assert(first_x != second_x || first_y != second_y);

    uint8_t zooms[1024];
    uint32_t xs[1024];
    uint32_t ys[1024];
    assert(plan.total_tiles <= 1024U);
    for (uint64_t i = 0U; i < plan.total_tiles; ++i) {
        assert(d1l_map_prefetch_plan_tile_at(
            &plan, i, &zooms[i], &xs[i], &ys[i]));
        for (uint64_t prior = 0U; prior < i; ++prior) {
            assert(zooms[prior] != zooms[i] ||
                   xs[prior] != xs[i] ||
                   ys[prior] != ys[i]);
        }
    }
}

int main(void)
{
    test_local_area_uses_highest_provider_detail();
    test_radius_filter_and_capacity_select_detail();
    test_provider_limit_and_antimeridian_wrap();
    test_invalid_and_reserved_capacity_fail_closed();
    test_configured_cache_budget_caps_card_allocation();
    test_remaining_budget_prioritizes_device_nodes_and_viewport();
    return 0;
}
