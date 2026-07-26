#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "map/map_point_projection.h"

static d1l_map_projection_view_t test_view(int32_t latitude_e6,
                                           int32_t longitude_e6,
                                           uint8_t zoom)
{
    const d1l_map_projection_view_t view = {
        .center_lat_e6 = latitude_e6,
        .center_lon_e6 = longitude_e6,
        .zoom = zoom,
        .viewport_width = 478U,
        .viewport_height = 360U,
        .pan_x_pixels = 0,
        .pan_y_pixels = 0,
    };
    return view;
}

static void test_exact_center_and_pan_offsets(void)
{
    d1l_map_projection_view_t view =
        test_view(43427977, -80316478, 10U);
    d1l_map_projected_point_t point = {0};
    for (uint8_t zoom = 8U; zoom <= 18U; ++zoom) {
        view.zoom = zoom;
        assert(d1l_map_point_project_e6(
            &view, 43427977, -80316478, &point));
        assert(point.visible);
        assert(!point.latitude_clipped);
        assert(point.screen_x == 239);
        assert(point.screen_y == 180);
    }

    view.zoom = 10U;
    view.pan_x_pixels = 23;
    view.pan_y_pixels = -17;
    assert(d1l_map_point_project_e6(
        &view, 43427977, -80316478, &point));
    assert(point.visible);
    assert(point.screen_x == 262);
    assert(point.screen_y == 163);
}

static void test_shortest_antimeridian_wrap(void)
{
    d1l_map_projected_point_t point = {0};
    d1l_map_projection_view_t view = test_view(0, 179800000, 8U);
    assert(d1l_map_point_project_e6(&view, 0, -179800000, &point));
    assert(point.visible);
    assert(point.screen_x == 312);
    assert(point.screen_y == 180);

    view.center_lon_e6 = -179800000;
    assert(d1l_map_point_project_e6(&view, 0, 179800000, &point));
    assert(point.visible);
    assert(point.screen_x == 166);
    assert(point.screen_y == 180);

    view.center_lon_e6 = 180000000;
    assert(d1l_map_point_project_e6(&view, 0, -180000000, &point));
    assert(point.visible);
    assert(point.screen_x == 239);
    assert(point.screen_y == 180);
}

static void test_tile_pixel_semantics_and_edge_bounds(void)
{
    d1l_map_projection_view_t view = test_view(0, 0, 8U);
    d1l_map_projected_point_t point = {0};

    /* At z8 one degree is 65536 / 360 = 182.044... pixels. */
    assert(d1l_map_point_project_e6(&view, 0, 1000000, &point));
    assert(point.visible);
    assert(point.screen_x == 421);
    assert(point.screen_y == 180);

    assert(d1l_map_point_project_e6(&view, 0, -1000000, &point));
    assert(point.visible);
    assert(point.screen_x == 57);

    assert(d1l_map_point_project_e6(&view, 0, 10000000, &point));
    assert(!point.visible);
    assert(point.screen_x == 477);
    assert(point.screen_y == 180);

    view.pan_x_pixels = INT32_MAX;
    view.pan_y_pixels = INT32_MIN;
    assert(d1l_map_point_project_e6(&view, 0, 0, &point));
    assert(!point.visible);
    assert(point.screen_x == 477);
    assert(point.screen_y == 0);
}

static void test_latitude_clipping_and_invalid_inputs(void)
{
    d1l_map_projection_view_t view = test_view(0, 0, 8U);
    d1l_map_projected_point_t point = {0};

    assert(d1l_map_point_project_e6(&view, 90000000, 0, &point));
    assert(point.latitude_clipped);
    assert(!point.visible);
    assert(point.screen_y == 0);

    memset(&point, 0x5a, sizeof(point));
    assert(!d1l_map_point_project_e6(&view, 0, 180000001, &point));
    assert(point.screen_x == 0);
    assert(point.screen_y == 0);
    assert(!point.visible);
    assert(!point.latitude_clipped);

    view.zoom = 7U;
    assert(!d1l_map_point_project_e6(&view, 0, 0, &point));
    view.zoom = 19U;
    assert(!d1l_map_point_project_e6(&view, 0, 0, &point));
    view.zoom = 8U;
    view.viewport_width = 0U;
    assert(!d1l_map_point_project_e6(&view, 0, 0, &point));
    view.viewport_width = 481U;
    assert(!d1l_map_point_project_e6(&view, 0, 0, &point));
    assert(!d1l_map_point_project_e6(NULL, 0, 0, &point));
    assert(!d1l_map_point_project_e6(&view, 0, 0, NULL));
}

int main(void)
{
    test_exact_center_and_pan_offsets();
    test_shortest_antimeridian_wrap();
    test_tile_pixel_semantics_and_edge_bounds();
    test_latitude_clipping_and_invalid_inputs();
    return 0;
}
