#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_navigation.h"

#ifndef EXPECT_CORE
#define EXPECT_CORE 0
#endif

static void assert_surface(const char *name,
                           const char *canonical,
                           d1l_ui_screen_t expected_screen,
                           bool expected_available)
{
    char surface[24] = {0};
    d1l_ui_screen_t screen = D1L_UI_SCREEN_HOME;
    assert(d1l_ui_scroll_surface_from_name(
        name, surface, sizeof(surface), &screen));
    assert(strcmp(surface, canonical) == 0);
    assert(screen == expected_screen);
    assert(d1l_ui_scroll_surface_available(surface, screen) ==
           expected_available);
}

int main(void)
{
    d1l_ui_screen_t parsed = D1L_UI_SCREEN_HOME;
    assert(d1l_ui_screen_from_name("map", &parsed));
    assert(parsed == D1L_UI_SCREEN_MAP);
    assert(d1l_ui_screen_available(D1L_UI_SCREEN_HOME));
    assert(d1l_ui_screen_available(D1L_UI_SCREEN_MESSAGES));
    assert(d1l_ui_screen_available(D1L_UI_SCREEN_NODES));
    assert(d1l_ui_screen_available(D1L_UI_SCREEN_PACKETS));
    assert(d1l_ui_screen_available(D1L_UI_SCREEN_SETTINGS));
    assert(d1l_ui_screen_available(D1L_UI_SCREEN_MAP) == !EXPECT_CORE);

    assert_surface("public", "public_messages",
                   D1L_UI_SCREEN_MESSAGES, true);
    assert_surface("dm", "dm_thread", D1L_UI_SCREEN_MESSAGES, true);
    assert_surface("storage", "storage", D1L_UI_SCREEN_SETTINGS, true);
    assert_surface("storage-card", "storage_card",
                   D1L_UI_SCREEN_SETTINGS, !EXPECT_CORE);
    assert_surface("wifi", "wifi", D1L_UI_SCREEN_SETTINGS, !EXPECT_CORE);
    assert_surface("contact-route", "contact_route",
                   D1L_UI_SCREEN_NODES, !EXPECT_CORE);
    assert_surface("mesh-roles", "mesh_roles",
                   D1L_UI_SCREEN_PACKETS, !EXPECT_CORE);
    assert_surface("map-location", "map_location",
                   D1L_UI_SCREEN_MAP, !EXPECT_CORE);

    assert(!d1l_ui_navigation_switch_pending());
    d1l_ui_navigation_request(D1L_UI_SCREEN_MAP);
    assert(d1l_ui_navigation_switch_pending() == !EXPECT_CORE);
    if (!EXPECT_CORE) {
        d1l_ui_screen_t requested = D1L_UI_SCREEN_HOME;
        assert(d1l_ui_navigation_begin_pending(&requested));
        assert(requested == D1L_UI_SCREEN_MAP);
        d1l_ui_navigation_finish(requested);
    }
    assert(!d1l_ui_navigation_switch_pending());

    d1l_ui_navigation_request(D1L_UI_SCREEN_PACKETS);
    assert(d1l_ui_navigation_switch_pending());
    d1l_ui_screen_t requested = D1L_UI_SCREEN_HOME;
    assert(d1l_ui_navigation_begin_pending(&requested));
    assert(requested == D1L_UI_SCREEN_PACKETS);
    assert(d1l_ui_navigation_active() == D1L_UI_SCREEN_PACKETS);
    d1l_ui_navigation_finish(requested);
    assert(!d1l_ui_navigation_switch_pending());

    puts("native UI Core navigation: ok");
    return 0;
}
