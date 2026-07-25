#include "ui_navigation.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_navigation_lock = portMUX_INITIALIZER_UNLOCKED;
static d1l_ui_screen_t s_active_screen = D1L_UI_SCREEN_HOME;
static d1l_ui_screen_t s_pending_screen = D1L_UI_SCREEN_HOME;
static bool s_switch_pending;

const char *d1l_ui_screen_name(d1l_ui_screen_t screen)
{
    switch (screen) {
    case D1L_UI_SCREEN_HOME:
        return "home";
    case D1L_UI_SCREEN_MESSAGES:
        return "messages";
    case D1L_UI_SCREEN_NODES:
        return "nodes";
    case D1L_UI_SCREEN_MAP:
        return "map";
    case D1L_UI_SCREEN_PACKETS:
        return "packets";
    case D1L_UI_SCREEN_SETTINGS:
        return "settings";
    default:
        return "unknown";
    }
}

bool d1l_ui_screen_from_name(const char *name, d1l_ui_screen_t *out_screen)
{
    if (!name || !out_screen) {
        return false;
    }
    if (strcmp(name, "home") == 0) {
        *out_screen = D1L_UI_SCREEN_HOME;
    } else if (strcmp(name, "messages") == 0 || strcmp(name, "msg") == 0) {
        *out_screen = D1L_UI_SCREEN_MESSAGES;
    } else if (strcmp(name, "nodes") == 0) {
        *out_screen = D1L_UI_SCREEN_NODES;
    } else if (strcmp(name, "map") == 0) {
        *out_screen = D1L_UI_SCREEN_MAP;
    } else if (strcmp(name, "packets") == 0 || strcmp(name, "pkts") == 0) {
        *out_screen = D1L_UI_SCREEN_PACKETS;
    } else if (strcmp(name, "settings") == 0 || strcmp(name, "set") == 0) {
        *out_screen = D1L_UI_SCREEN_SETTINGS;
    } else {
        return false;
    }
    return true;
}

bool d1l_ui_screen_available(d1l_ui_screen_t screen)
{
    switch (screen) {
    case D1L_UI_SCREEN_HOME:
    case D1L_UI_SCREEN_SETTINGS:
        return true;
    case D1L_UI_SCREEN_MESSAGES:
        return d1l_release_feature_available(
                   D1L_RELEASE_FEATURE_PUBLIC_MESSAGES) ||
               d1l_release_feature_available(
                   D1L_RELEASE_FEATURE_DIRECT_MESSAGES);
    case D1L_UI_SCREEN_NODES:
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_NODES);
    case D1L_UI_SCREEN_MAP:
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP);
    case D1L_UI_SCREEN_PACKETS:
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_PACKETS);
    default:
        return false;
    }
}

bool d1l_ui_scroll_surface_from_name(const char *name,
                                     char *out_surface,
                                     size_t out_surface_len,
                                     d1l_ui_screen_t *out_screen)
{
    char normalized[24] = {0};
    if (!name || !out_surface || out_surface_len == 0 || !out_screen) {
        return false;
    }
    size_t len = 0;
    while (*name && len + 1U < sizeof(normalized)) {
        char ch = (char)tolower((unsigned char)*name++);
        normalized[len++] = (ch == '-' || ch == ' ') ? '_' : ch;
    }
    normalized[len] = '\0';
    if (*name || len == 0) {
        return false;
    }

    const char *surface = NULL;
    d1l_ui_screen_t screen = D1L_UI_SCREEN_HOME;
    if (strcmp(normalized, "home") == 0) {
        surface = "home";
        screen = D1L_UI_SCREEN_HOME;
    } else if (strcmp(normalized, "public") == 0 ||
               strcmp(normalized, "messages") == 0 ||
               strcmp(normalized, "public_messages") == 0) {
        surface = "public_messages";
        screen = D1L_UI_SCREEN_MESSAGES;
    } else if (strcmp(normalized, "dm") == 0 ||
               strcmp(normalized, "dm_thread") == 0) {
        surface = "dm_thread";
        screen = D1L_UI_SCREEN_MESSAGES;
    } else if (strcmp(normalized, "nodes") == 0) {
        surface = "nodes";
        screen = D1L_UI_SCREEN_NODES;
    } else if (strcmp(normalized, "contact") == 0 ||
               strcmp(normalized, "contact_detail") == 0) {
        surface = "contact_detail";
        screen = D1L_UI_SCREEN_NODES;
    } else if (strcmp(normalized, "contact_options") == 0) {
        surface = "contact_options";
        screen = D1L_UI_SCREEN_NODES;
    } else if (strcmp(normalized, "contact_forget") == 0 ||
               strcmp(normalized, "forget_contact") == 0) {
        surface = "contact_forget";
        screen = D1L_UI_SCREEN_NODES;
    } else if (strcmp(normalized, "contact_route") == 0) {
        surface = "contact_route";
        screen = D1L_UI_SCREEN_NODES;
    } else if (strcmp(normalized, "mesh_roles") == 0) {
        surface = "mesh_roles";
        screen = D1L_UI_SCREEN_PACKETS;
    } else if (strcmp(normalized, "mesh_rooms") == 0) {
        surface = "mesh_rooms";
        screen = D1L_UI_SCREEN_PACKETS;
    } else if (strcmp(normalized, "mesh_repeaters") == 0) {
        surface = "mesh_repeaters";
        screen = D1L_UI_SCREEN_PACKETS;
    } else if (strcmp(normalized, "packets") == 0 ||
               strcmp(normalized, "pkts") == 0) {
        surface = "packets";
        screen = D1L_UI_SCREEN_PACKETS;
    } else if (strcmp(normalized, "settings") == 0 ||
               strcmp(normalized, "set") == 0) {
        surface = "settings";
        screen = D1L_UI_SCREEN_SETTINGS;
    } else if (strcmp(normalized, "storage") == 0 ||
               strcmp(normalized, "sd") == 0) {
        surface = "storage";
        screen = D1L_UI_SCREEN_SETTINGS;
    } else if (strcmp(normalized, "storage_card") == 0 ||
               strcmp(normalized, "sd_card") == 0) {
        surface = "storage_card";
        screen = D1L_UI_SCREEN_SETTINGS;
    } else if (strcmp(normalized, "storage_data") == 0) {
        surface = "storage_data";
        screen = D1L_UI_SCREEN_SETTINGS;
    } else if (strcmp(normalized, "wifi") == 0 ||
               strcmp(normalized, "wi_fi") == 0) {
        surface = "wifi";
        screen = D1L_UI_SCREEN_SETTINGS;
    } else if (strcmp(normalized, "map") == 0) {
        surface = "map";
        screen = D1L_UI_SCREEN_MAP;
    } else if (strcmp(normalized, "map_options") == 0 ||
               strcmp(normalized, "map_options_sheet") == 0 ||
               strcmp(normalized, "map_options_page") == 0 ||
               strcmp(normalized, "map_menu") == 0) {
        surface = "map_options";
        screen = D1L_UI_SCREEN_MAP;
    } else if (strcmp(normalized, "map_location") == 0 ||
               strcmp(normalized, "map_location_sheet") == 0 ||
               strcmp(normalized, "map_location_page") == 0) {
        surface = "map_location";
        screen = D1L_UI_SCREEN_MAP;
    } else if (strcmp(normalized, "map_cache") == 0 ||
               strcmp(normalized, "map_cache_page") == 0 ||
               strcmp(normalized, "tile_cache") == 0) {
        surface = "map_cache";
        screen = D1L_UI_SCREEN_MAP;
    } else {
        return false;
    }

    snprintf(out_surface, out_surface_len, "%s", surface);
    *out_screen = screen;
    return true;
}

bool d1l_ui_scroll_surface_available(const char *surface,
                                     d1l_ui_screen_t screen)
{
    if (!surface || !d1l_ui_screen_available(screen)) {
        return false;
    }
    if (strcmp(surface, "public_messages") == 0) {
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_PUBLIC_MESSAGES);
    }
    if (strcmp(surface, "dm_thread") == 0) {
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_DIRECT_MESSAGES);
    }
    if (strcmp(surface, "wifi") == 0) {
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_WIFI_USER_CONTROL);
    }
    if (strcmp(surface, "storage_card") == 0 ||
        strcmp(surface, "storage_data") == 0) {
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_SD_HISTORY);
    }
    if (strcmp(surface, "contact_route") == 0) {
        return d1l_release_feature_available(
            D1L_RELEASE_FEATURE_USER_TRACE);
    }
    if (strcmp(surface, "mesh_roles") == 0 ||
        strcmp(surface, "mesh_rooms") == 0 ||
        strcmp(surface, "mesh_repeaters") == 0) {
        return d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN);
    }
    return true;
}

void d1l_ui_navigation_request(d1l_ui_screen_t screen)
{
    if (!d1l_ui_screen_available(screen)) {
        return;
    }
    portENTER_CRITICAL(&s_navigation_lock);
    s_pending_screen = screen;
    s_switch_pending = true;
    portEXIT_CRITICAL(&s_navigation_lock);
}

bool d1l_ui_navigation_begin_pending(d1l_ui_screen_t *out_screen)
{
    bool pending;
    d1l_ui_screen_t screen;

    portENTER_CRITICAL(&s_navigation_lock);
    pending = s_switch_pending;
    screen = s_pending_screen;
    if (pending) {
        s_active_screen = screen;
    }
    portEXIT_CRITICAL(&s_navigation_lock);

    if (!pending) {
        return false;
    }
    if (out_screen) {
        *out_screen = screen;
    }
    return true;
}

void d1l_ui_navigation_finish(d1l_ui_screen_t rendered_screen)
{
    portENTER_CRITICAL(&s_navigation_lock);
    if (s_pending_screen == rendered_screen) {
        s_switch_pending = false;
    }
    portEXIT_CRITICAL(&s_navigation_lock);
}

d1l_ui_screen_t d1l_ui_navigation_active(void)
{
    d1l_ui_screen_t screen;

    portENTER_CRITICAL(&s_navigation_lock);
    screen = s_active_screen;
    portEXIT_CRITICAL(&s_navigation_lock);
    return screen;
}

d1l_ui_screen_t d1l_ui_navigation_pending(void)
{
    d1l_ui_screen_t screen;

    portENTER_CRITICAL(&s_navigation_lock);
    screen = s_pending_screen;
    portEXIT_CRITICAL(&s_navigation_lock);
    return screen;
}

bool d1l_ui_navigation_switch_pending(void)
{
    bool pending;

    portENTER_CRITICAL(&s_navigation_lock);
    pending = s_switch_pending;
    portEXIT_CRITICAL(&s_navigation_lock);
    return pending;
}
