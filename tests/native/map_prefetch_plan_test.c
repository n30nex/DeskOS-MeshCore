#include <assert.h>
#include <stdint.h>

#include "map/map_prefetch_plan.h"

#define CARD_32GB_CLASS_KB (31U * 1024U * 1024U)

static void test_local_area_uses_highest_provider_detail(void)
{
    d1l_map_prefetch_plan_t plan = {0};
    assert(d1l_map_prefetch_plan_build(
        434279770, -803164780, NULL, 0U,
        CARD_32GB_CLASS_KB, 65536U, 18U, &plan));
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
        CARD_32GB_CLASS_KB, 65536U, 18U, &plan));
    assert(plan.node_count_included == 4U);
    assert(plan.node_count_outside_radius == 1U);
    assert(plan.max_zoom >= 14U);
    assert(plan.max_zoom < 18U);
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
        CARD_32GB_CLASS_KB, 65536U, 14U, &plan));
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
        8U * 1024U * 1024U, 65536U, 18U, &plan));
    assert(!d1l_map_prefetch_plan_build(
        0, 0, NULL, 0U,
        CARD_32GB_CLASS_KB, 0U, 18U, &plan));
    assert(!d1l_map_prefetch_plan_build(
        0, 0, NULL, 0U,
        CARD_32GB_CLASS_KB, 65536U, 7U, &plan));
}

int main(void)
{
    test_local_area_uses_highest_provider_detail();
    test_radius_filter_and_capacity_select_detail();
    test_provider_limit_and_antimeridian_wrap();
    test_invalid_and_reserved_capacity_fail_closed();
    return 0;
}
