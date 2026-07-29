#include "usb_console.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "d1l_config.h"
#include "app/app_model.h"
#include "app/qualification_hooks.h"
#include "app/settings_model.h"
#include "comms/connectivity_manager.h"
#include "comms/companion_3byte.h"
#include "comms/observer_manager.h"
#include "comms/usb_command_parser.h"
#include "diagnostics/crash_log.h"
#include "diagnostics/event_log.h"
#include "diagnostics/health_monitor.h"
#include "hal/backlight.h"
#include "hal/indicator_board.h"
#include "hal/rp2040_bridge.h"
#include "hal/sx1262_indicator.h"
#include "mesh/contact_store.h"
#include "mesh/dm_store.h"
#include "mesh/mesh_inspector.h"
#include "mesh/message_store.h"
#include "mesh/node_store.h"
#include "mesh/packet_log.h"
#include "mesh/read_state.h"
#include "mesh/route_store.h"
#include "mesh/route_store_worker.h"
#include "mesh/meshcore_radio_profile.h"
#include "mesh/meshcore_route_selection.h"
#include "mesh/meshcore_service.h"
#include "mesh/user_text.h"
#include "map/map_png_decoder.h"
#include "map/map_prefetch_plan.h"
#include "map/map_prefetch_service.h"
#include "map/map_tile_provider.h"
#include "map/map_view_service.h"
#include "platform/time_service.h"
#include "storage/export_store.h"
#include "storage/factory_reset.h"
#include "storage/map_tile_store.h"
#include "storage/retained_blob_store.h"
#include "storage/storage_status.h"
#include "ui/ui_phase1.h"
#include "update/update_manager.h"

static const size_t D1L_CONSOLE_MESSAGE_PAGE_SIZE = 8U;
#if D1L_ENABLE_QUALIFICATION_HOOKS
static const uint32_t D1L_STORAGE_FILE_CANARY_OP_TIMEOUT_MS = 30000U;
static const uint32_t D1L_STORAGE_FILE_CANARY_MANAGER_PAUSE_MS = 60000U;
static const uint32_t D1L_RETAINED_CANARY_QUIESCE_TIMEOUT_MS = 15000U;
#endif
static const uint32_t D1L_REBOOT_QUIESCE_TIMEOUT_MS = 15000U;
/* The default ESP-IDF console writes directly to UART0 without installing the
 * UART driver. Give the final reboot receipt enough bounded wire time before
 * the already-quiesced system reset. The receipt is under 300 bytes (under
 * 27 ms at 115200 baud). */
static const uint32_t D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS = 50U;
static const uint32_t D1L_REBOOT_RESTART_MARGIN_MS = 100U;

static bool parse_next_u32_arg(const char **cursor, uint32_t *out_value);
static bool parse_channel_id_hex(const char *text, uint64_t *out_value);

static void wipe_console_bytes(void *data, size_t length)
{
    if (!data) {
        return;
    }
    volatile uint8_t *bytes = (volatile uint8_t *)data;
    for (size_t i = 0; i < length; ++i) {
        bytes[i] = 0U;
    }
}

/* Internal generated-text helper.  Raw USB input is canonicalized by
 * d1l_usb_command_admit_in_place() with its explicit receive length instead. */
static void trim_line(char *line)
{
    size_t n = strlen(line);
    while (n > 0U && isspace((unsigned char)line[n - 1U])) {
        line[--n] = '\0';
    }
}

static uint32_t deadline_remaining_ms(int64_t started_us, uint32_t timeout_ms)
{
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    const int64_t deadline_us = (int64_t)timeout_ms * 1000LL;
    if (elapsed_us >= deadline_us) {
        return 0U;
    }
    return (uint32_t)((deadline_us - elapsed_us + 999LL) / 1000LL);
}

static uint32_t reboot_deadline_remaining_ms(int64_t started_us)
{
    return deadline_remaining_ms(started_us, D1L_REBOOT_QUIESCE_TIMEOUT_MS);
}

static void ok_begin(const char *cmd)
{
    printf("{\"schema\":%d,\"ok\":true,\"cmd\":\"%s\"", D1L_CONSOLE_SCHEMA, cmd);
}

static void err_result(const char *cmd, const char *code, const char *hint)
{
    printf("{\"schema\":%d,\"ok\":false,\"cmd\":\"%s\",\"code\":\"%s\",\"hint\":\"%s\"}\n",
           D1L_CONSOLE_SCHEMA, cmd, code, hint ? hint : "");
}

static const char *bool_json(bool value)
{
    return value ? "true" : "false";
}

static const char *map_location_source_name(const d1l_settings_t *settings)
{
    if (!settings || !settings->map_location_set) {
        return "unset";
    }
    return settings->map_location_source ==
            D1L_MAP_LOCATION_SOURCE_AUTHENTICATED_COMPANION ?
        "authenticated_companion" : "manual";
}

static void print_e7_json(int32_t value)
{
    int64_t scaled = value;
    if (scaled < 0) {
        putchar('-');
        scaled = -scaled;
    }
    printf("%lld.%07lld", (long long)(scaled / 10000000LL),
           (long long)(scaled % 10000000LL));
}

static bool parse_coord_e7(const char *text, const char **out_end,
                           int32_t min_e7, int32_t max_e7, int32_t *out_value)
{
    if (!text || !out_value) {
        return false;
    }
    char *end = NULL;
    const double value = strtod(text, &end);
    if (end == text || value != value) {
        return false;
    }
    const double min_value = ((double)min_e7) / 10000000.0;
    const double max_value = ((double)max_e7) / 10000000.0;
    if (value < min_value || value > max_value) {
        return false;
    }
    const double scaled = value * 10000000.0;
    const int64_t rounded = (int64_t)(scaled >= 0 ? scaled + 0.5 : scaled - 0.5);
    if (rounded < min_e7 || rounded > max_e7) {
        return false;
    }
    *out_value = (int32_t)rounded;
    if (out_end) {
        *out_end = end;
    }
    return true;
}

static void print_json_string(const char *text)
{
    putchar('"');
    if (text) {
        for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
            switch (*p) {
            case '"':
                fputs("\\\"", stdout);
                break;
            case '\\':
                fputs("\\\\", stdout);
                break;
            case '\b':
                fputs("\\b", stdout);
                break;
            case '\f':
                fputs("\\f", stdout);
                break;
            case '\n':
                fputs("\\n", stdout);
                break;
            case '\r':
                fputs("\\r", stdout);
                break;
            case '\t':
                fputs("\\t", stdout);
                break;
            default:
                if (*p < 0x20U) {
                    printf("\\u%04x", (unsigned)*p);
                } else {
                    putchar((int)*p);
                }
                break;
            }
        }
    }
    putchar('"');
}

typedef enum {
    D1L_RELEASE_COMMAND_EXACT = 0,
    D1L_RELEASE_COMMAND_TOKEN_OR_ARGUMENT,
} d1l_release_command_match_t;

typedef enum {
    D1L_RELEASE_COMMAND_READ_ONLY = 0,
    D1L_RELEASE_COMMAND_MUTATION,
} d1l_release_command_access_t;

typedef struct {
    const char *pattern;
    size_t pattern_length;
    d1l_release_command_match_t match;
    d1l_release_command_access_t access;
    d1l_release_feature_id_t feature;
} d1l_release_command_rule_t;

#define D1L_RELEASE_RULE_EXACT(literal, command_access, feature_id)            \
    {                                                                          \
        .pattern = (literal), .pattern_length = sizeof(literal) - 1U,          \
        .match = D1L_RELEASE_COMMAND_EXACT, .access = (command_access),         \
        .feature = (feature_id),                                                \
    }

#define D1L_RELEASE_RULE_TOKEN(literal, command_access, feature_id)            \
    {                                                                          \
        .pattern = (literal), .pattern_length = sizeof(literal) - 1U,          \
        .match = D1L_RELEASE_COMMAND_TOKEN_OR_ARGUMENT,                         \
        .access = (command_access), .feature = (feature_id),                    \
    }

/*
 * Rules are ordered from narrow read-only exceptions to broad fail-closed
 * families.  The table is consulted before handle_line() invokes any command
 * handler, so a Core-only denial cannot parse secrets, persist settings,
 * create a task, touch storage, or transmit RF.
 */
static const d1l_release_command_rule_t s_release_command_rules[] = {
    D1L_RELEASE_RULE_EXACT(
        "wifi status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL),
    D1L_RELEASE_RULE_EXACT(
        "wifi profiles", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL),
    D1L_RELEASE_RULE_TOKEN(
        "wifi", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL),
    D1L_RELEASE_RULE_EXACT(
        "ble status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_BLE),
    D1L_RELEASE_RULE_TOKEN(
        "ble", D1L_RELEASE_COMMAND_MUTATION, D1L_RELEASE_FEATURE_BLE),

    /*
     * SD qualification is allowed while the immutable mode is conditional,
     * but every bridge/media mutation fails closed once the candidate is
     * frozen with SD history disabled.  Keep the narrow read-only probes
     * ahead of the mutation rules so their admission remains explicit.
     */
    D1L_RELEASE_RULE_EXACT(
        "storage status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_EXACT(
        "storage diag", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_EXACT(
        "storage diag raw", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_EXACT(
        "storage setup", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_EXACT(
        "rp2040 status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_EXACT(
        "rp2040 ping", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_EXACT(
        "rp2040 stock-probe", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_SD_HISTORY),
#if D1L_ENABLE_QUALIFICATION_HOOKS
    D1L_RELEASE_RULE_EXACT(
        "ui scroll-probe storage_card", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_EXACT(
        "ui scroll-probe storage_data", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
#endif
    D1L_RELEASE_RULE_EXACT(
        "storage mount", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_EXACT(
        "storage remount", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_EXACT(
        "storage reset-bridge", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
#if D1L_ENABLE_QUALIFICATION_HOOKS
    D1L_RELEASE_RULE_EXACT(
        "storage filecanary", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_TOKEN(
        "storage export-canary", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
#endif
    D1L_RELEASE_RULE_TOKEN(
        "storage export-diagnostics", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_TOKEN(
        "storage export-data", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
#if D1L_ENABLE_QUALIFICATION_HOOKS
    D1L_RELEASE_RULE_TOKEN(
        "storage retained-canary", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
#endif
    D1L_RELEASE_RULE_EXACT(
        "storage force-nvs off", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_TOKEN(
        "rp2040 set-baud", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_TOKEN(
        "rp2040 baud-probe", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_EXACT(
        "rp2040 bootloader", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_EXACT(
        "rp2040 reset", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
    D1L_RELEASE_RULE_TOKEN(
        "rp2040 double-reset", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SD_HISTORY),
#if D1L_ENABLE_QUALIFICATION_HOOKS
    D1L_RELEASE_RULE_TOKEN(
        "core retained-witness", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_RETAINED_NVS),
#endif

    D1L_RELEASE_RULE_EXACT(
        "map center", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_LOCATION),
    D1L_RELEASE_RULE_EXACT(
        "map tiles status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_MAP),
    D1L_RELEASE_RULE_EXACT(
        "map provider status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_MAP),
#if D1L_ENABLE_QUALIFICATION_HOOKS
    D1L_RELEASE_RULE_EXACT(
        "map acceptance open", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MAP),
#endif
    D1L_RELEASE_RULE_EXACT(
        "storage map-policy", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_MAP),
    D1L_RELEASE_RULE_TOKEN(
        "map", D1L_RELEASE_COMMAND_MUTATION, D1L_RELEASE_FEATURE_MAP),
    D1L_RELEASE_RULE_TOKEN(
        "settings set location", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_LOCATION),
    D1L_RELEASE_RULE_EXACT(
        "settings clear location", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_LOCATION),
    D1L_RELEASE_RULE_TOKEN(
        "location", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_LOCATION),
    D1L_RELEASE_RULE_TOKEN(
        "gps", D1L_RELEASE_COMMAND_MUTATION, D1L_RELEASE_FEATURE_LOCATION),
#if D1L_ENABLE_QUALIFICATION_HOOKS
    D1L_RELEASE_RULE_TOKEN(
        "storage map-tile-canary", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MAP),
    D1L_RELEASE_RULE_TOKEN(
        "storage map-tile-check", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MAP),
#endif

    D1L_RELEASE_RULE_TOKEN(
        "ui tab map", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MAP),
    D1L_RELEASE_RULE_TOKEN(
        "ui tab wifi", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL),
    D1L_RELEASE_RULE_TOKEN(
        "ui tab ble", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_BLE),
#if D1L_ENABLE_QUALIFICATION_HOOKS
    D1L_RELEASE_RULE_TOKEN(
        "ui scroll-probe map", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MAP),
    D1L_RELEASE_RULE_TOKEN(
        "ui scroll-probe map_options", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MAP),
    D1L_RELEASE_RULE_TOKEN(
        "ui scroll-probe map_location", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_LOCATION),
    D1L_RELEASE_RULE_TOKEN(
        "ui scroll-probe map_cache", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MAP),
    D1L_RELEASE_RULE_TOKEN(
        "ui scroll-probe wifi", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL),
    D1L_RELEASE_RULE_TOKEN(
        "ui compose-probe map-location", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_LOCATION),
    D1L_RELEASE_RULE_TOKEN(
        "ui compose-probe map_location", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_LOCATION),
    D1L_RELEASE_RULE_TOKEN(
        "ui compose-probe wifi-ssid", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL),
    D1L_RELEASE_RULE_TOKEN(
        "ui compose-probe wifi-password", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL),
#endif

    D1L_RELEASE_RULE_EXACT(
        "channels", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT),
    D1L_RELEASE_RULE_TOKEN(
        "channels", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT),
    D1L_RELEASE_RULE_TOKEN(
        "channel", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT),
    D1L_RELEASE_RULE_TOKEN(
        "messages channel", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT),
    D1L_RELEASE_RULE_TOKEN(
        "mesh send channel", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT),

    D1L_RELEASE_RULE_EXACT(
        "routes trace status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_USER_TRACE),
    D1L_RELEASE_RULE_TOKEN(
        "routes trace contact", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_USER_TRACE),
    D1L_RELEASE_RULE_TOKEN(
        "routes probe", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_USER_TRACE),
    D1L_RELEASE_RULE_TOKEN(
        "routes telemetry", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_USER_TRACE),
    D1L_RELEASE_RULE_TOKEN(
        "routes reset", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_USER_TRACE),
    D1L_RELEASE_RULE_EXACT(
        "routes clear", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_USER_TRACE),
    D1L_RELEASE_RULE_TOKEN(
        "settings set pathhash", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_USER_TRACE),
    D1L_RELEASE_RULE_TOKEN(
        "trace", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_USER_TRACE),
    D1L_RELEASE_RULE_TOKEN(
        "path", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_USER_TRACE),
#if D1L_ENABLE_QUALIFICATION_HOOKS
    D1L_RELEASE_RULE_TOKEN(
        "ui scroll-probe contact_route", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_USER_TRACE),
#endif

    D1L_RELEASE_RULE_EXACT(
        "roomservers", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_EXACT(
        "repeaters", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_EXACT(
        "repeater ping status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_TOKEN(
        "repeater ping", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_EXACT(
        "admin status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_EXACT(
        "admin telemetry", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_TOKEN(
        "admin neighbours", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_EXACT(
        "admin access-list", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_TOKEN(
        "admin", D1L_RELEASE_COMMAND_MUTATION, D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_TOKEN(
        "room", D1L_RELEASE_COMMAND_MUTATION, D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_TOKEN(
        "repeater", D1L_RELEASE_COMMAND_MUTATION, D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_TOKEN(
        "mesh admin", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_TOKEN(
        "mesh room", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_TOKEN(
        "mesh repeater", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_ADMIN),
#if D1L_ENABLE_QUALIFICATION_HOOKS
    D1L_RELEASE_RULE_TOKEN(
        "ui scroll-probe mesh_roles", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_TOKEN(
        "ui scroll-probe mesh_rooms", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_ADMIN),
    D1L_RELEASE_RULE_TOKEN(
        "ui scroll-probe mesh_repeaters", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_ADMIN),
#endif

    D1L_RELEASE_RULE_EXACT(
        "observer status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_OBSERVER_MQTT),
    D1L_RELEASE_RULE_TOKEN(
        "observer", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_OBSERVER_MQTT),
    D1L_RELEASE_RULE_TOKEN(
        "mqtt", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_OBSERVER_MQTT),
    D1L_RELEASE_RULE_TOKEN(
        "settings set observer", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_OBSERVER_MQTT),

    D1L_RELEASE_RULE_EXACT(
        "update status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_SIGNED_UPDATE),
    D1L_RELEASE_RULE_TOKEN(
        "update", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SIGNED_UPDATE),
    D1L_RELEASE_RULE_TOKEN(
        "ota", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SIGNED_UPDATE),
    D1L_RELEASE_RULE_TOKEN(
        "install", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SIGNED_UPDATE),
    D1L_RELEASE_RULE_TOKEN(
        "firmware update", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SIGNED_UPDATE),
    D1L_RELEASE_RULE_TOKEN(
        "storage install", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SIGNED_UPDATE),
    D1L_RELEASE_RULE_TOKEN(
        "storage update", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_SIGNED_UPDATE),

    D1L_RELEASE_RULE_EXACT(
        "terminal status", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_MUTABLE_TERMINAL),
    D1L_RELEASE_RULE_EXACT(
        "logs", D1L_RELEASE_COMMAND_READ_ONLY,
        D1L_RELEASE_FEATURE_MUTABLE_TERMINAL),
    D1L_RELEASE_RULE_TOKEN(
        "terminal", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MUTABLE_TERMINAL),
    D1L_RELEASE_RULE_TOKEN(
        "shell", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MUTABLE_TERMINAL),
    D1L_RELEASE_RULE_TOKEN(
        "log level", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MUTABLE_TERMINAL),
    D1L_RELEASE_RULE_TOKEN(
        "logs", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_MUTABLE_TERMINAL),

    D1L_RELEASE_RULE_TOKEN(
        "qr", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI),
    D1L_RELEASE_RULE_TOKEN(
        "emoji", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI),
#if D1L_ENABLE_QUALIFICATION_HOOKS
    D1L_RELEASE_RULE_TOKEN(
        "ui compose-probe qr", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI),
    D1L_RELEASE_RULE_TOKEN(
        "ui compose-probe emoji", D1L_RELEASE_COMMAND_MUTATION,
        D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI),
#endif
};

#if D1L_ENABLE_QUALIFICATION_HOOKS
typedef struct {
    const char *normalized_alias;
    d1l_release_feature_id_t feature;
} d1l_release_ui_probe_alias_t;

/*
 * Keep this alias map aligned with the canonicalizers in ui_navigation.c and
 * ui_keyboard.c.  The console release boundary must classify every accepted
 * spelling before a probe handler can navigate, create UI state, or touch a
 * feature-specific service.
 */
static const d1l_release_ui_probe_alias_t s_release_scroll_probe_aliases[] = {
    {"storage_card", D1L_RELEASE_FEATURE_SD_HISTORY},
    {"sd_card", D1L_RELEASE_FEATURE_SD_HISTORY},
    {"storage_data", D1L_RELEASE_FEATURE_SD_HISTORY},
    {"wifi", D1L_RELEASE_FEATURE_WIFI_USER_CONTROL},
    {"wi_fi", D1L_RELEASE_FEATURE_WIFI_USER_CONTROL},
    {"contact_route", D1L_RELEASE_FEATURE_USER_TRACE},
    {"mesh_roles", D1L_RELEASE_FEATURE_ADMIN},
    {"mesh_rooms", D1L_RELEASE_FEATURE_ADMIN},
    {"mesh_repeaters", D1L_RELEASE_FEATURE_ADMIN},
    {"map", D1L_RELEASE_FEATURE_MAP},
    {"map_options", D1L_RELEASE_FEATURE_MAP},
    {"map_options_sheet", D1L_RELEASE_FEATURE_MAP},
    {"map_options_page", D1L_RELEASE_FEATURE_MAP},
    {"map_menu", D1L_RELEASE_FEATURE_MAP},
    {"map_location", D1L_RELEASE_FEATURE_LOCATION},
    {"map_location_sheet", D1L_RELEASE_FEATURE_LOCATION},
    {"map_location_page", D1L_RELEASE_FEATURE_LOCATION},
    {"map_cache", D1L_RELEASE_FEATURE_MAP},
    {"map_cache_page", D1L_RELEASE_FEATURE_MAP},
    {"tile_cache", D1L_RELEASE_FEATURE_MAP},
};

static const d1l_release_ui_probe_alias_t s_release_compose_probe_aliases[] = {
    {"map_location", D1L_RELEASE_FEATURE_LOCATION},
    {"wifi", D1L_RELEASE_FEATURE_WIFI_USER_CONTROL},
    {"wifi_ssid", D1L_RELEASE_FEATURE_WIFI_USER_CONTROL},
    {"wifi_password", D1L_RELEASE_FEATURE_WIFI_USER_CONTROL},
    {"wifi_pass", D1L_RELEASE_FEATURE_WIFI_USER_CONTROL},
};

static bool release_ui_probe_normalize_argument(
    const d1l_usb_command_view_t *command, size_t prefix_length,
    bool first_token_only, char *normalized, size_t normalized_capacity)
{
    if (!command || !command->text || !normalized ||
        normalized_capacity == 0U || prefix_length >= command->length) {
        return false;
    }

    size_t source_index = prefix_length;
    while (source_index < command->length &&
           command->text[source_index] == ' ') {
        source_index++;
    }

    size_t output_index = 0U;
    while (source_index < command->length) {
        unsigned char value =
            (unsigned char)command->text[source_index++];
        if (first_token_only && value == ' ') {
            break;
        }
        if (output_index + 1U >= normalized_capacity) {
            normalized[0] = '\0';
            return false;
        }
        normalized[output_index++] =
            (value == '-' || value == ' ')
                ? '_'
                : (char)tolower(value);
    }
    normalized[output_index] = '\0';
    return output_index > 0U;
}

static bool release_ui_probe_alias_feature(
    const char *normalized,
    const d1l_release_ui_probe_alias_t *aliases, size_t alias_count,
    d1l_release_feature_id_t *out_feature)
{
    if (!normalized || !aliases || !out_feature) {
        return false;
    }
    for (size_t i = 0U; i < alias_count; ++i) {
        if (strcmp(normalized, aliases[i].normalized_alias) == 0) {
            *out_feature = aliases[i].feature;
            return true;
        }
    }
    return false;
}

static bool release_ui_probe_feature(
    const d1l_usb_command_view_t *command,
    d1l_release_feature_id_t *out_feature)
{
    static const char scroll_prefix[] = "ui scroll-probe ";
    static const char compose_prefix[] = "ui compose-probe ";
    const d1l_release_ui_probe_alias_t *aliases = NULL;
    size_t alias_count = 0U;
    size_t prefix_length = 0U;

    if (d1l_usb_command_has_argument(
            command, scroll_prefix, sizeof(scroll_prefix) - 1U)) {
        aliases = s_release_scroll_probe_aliases;
        alias_count = sizeof(s_release_scroll_probe_aliases) /
                      sizeof(s_release_scroll_probe_aliases[0]);
        prefix_length = sizeof(scroll_prefix) - 1U;
    } else if (d1l_usb_command_has_argument(
                   command, compose_prefix,
                   sizeof(compose_prefix) - 1U)) {
        aliases = s_release_compose_probe_aliases;
        alias_count = sizeof(s_release_compose_probe_aliases) /
                      sizeof(s_release_compose_probe_aliases[0]);
        prefix_length = sizeof(compose_prefix) - 1U;
    } else {
        return false;
    }

    char normalized[48] = {0};
    if (release_ui_probe_normalize_argument(
            command, prefix_length, false, normalized,
            sizeof(normalized)) &&
        release_ui_probe_alias_feature(
            normalized, aliases, alias_count, out_feature)) {
        return true;
    }

    /*
     * Probe handlers consume one whitespace-delimited token.  Classify that
     * token too, so adding an ignored suffix cannot smuggle an excluded alias
     * through the pre-handler boundary.
     */
    return release_ui_probe_normalize_argument(
               command, prefix_length, true, normalized,
               sizeof(normalized)) &&
           release_ui_probe_alias_feature(
               normalized, aliases, alias_count, out_feature);
}
#endif

static bool release_command_token_matches(
    const d1l_usb_command_view_t *command,
    const char *pattern, size_t pattern_length)
{
    if (!command || !command->text || !pattern || pattern_length == 0U) {
        return false;
    }
    size_t command_index = 0U;
    size_t pattern_index = 0U;
    while (pattern_index < pattern_length) {
        if (pattern[pattern_index] == ' ') {
            if (command_index >= command->length ||
                command->text[command_index] != ' ') {
                return false;
            }
            while (pattern_index < pattern_length &&
                   pattern[pattern_index] == ' ') {
                pattern_index++;
            }
            while (command_index < command->length &&
                   command->text[command_index] == ' ') {
                command_index++;
            }
            continue;
        }
        if (command_index >= command->length ||
            tolower((unsigned char)command->text[command_index]) !=
                tolower((unsigned char)pattern[pattern_index])) {
            return false;
        }
        command_index++;
        pattern_index++;
    }
    return command_index == command->length ||
           command->text[command_index] == ' ';
}

static bool release_command_rule_matches(
    const d1l_usb_command_view_t *command,
    const d1l_release_command_rule_t *rule)
{
    if (!command || !rule) {
        return false;
    }
    if (rule->match == D1L_RELEASE_COMMAND_EXACT) {
        return d1l_usb_command_equals(
            command, rule->pattern, rule->pattern_length);
    }
    return release_command_token_matches(
        command, rule->pattern, rule->pattern_length);
}

static const d1l_release_command_rule_t *release_command_rule(
    const d1l_usb_command_view_t *command)
{
    for (size_t i = 0U;
         i < sizeof(s_release_command_rules) /
                 sizeof(s_release_command_rules[0]);
         ++i) {
        if (release_command_rule_matches(
                command, &s_release_command_rules[i])) {
            return &s_release_command_rules[i];
        }
    }
    return NULL;
}

static void print_release_profile_fields(void)
{
    printf(",\"build_commit\":");
    print_json_string(D1L_BUILD_GIT_COMMIT);
    printf(",\"release_profile\":");
    print_json_string(d1l_release_profile_name());
    printf(",\"release_profile_id\":%u,\"sd_history_mode\":",
           (unsigned)d1l_release_profile_id());
    print_json_string(d1l_release_sd_history_mode_name());
    printf(",\"sd_history_mode_id\":%u",
           (unsigned)d1l_release_sd_history_mode());
}

static void release_unavailable_status(
    const char *command, d1l_release_feature_id_t feature)
{
    ok_begin(command);
    printf(",\"available\":false");
    print_release_profile_fields();
    printf(",\"feature\":");
    print_json_string(d1l_release_feature_name(feature));
    printf(",\"mutation_allowed\":false,"
           "\"reason\":\"unavailable_in_release_profile\"}\n");
}

static void release_unsupported_result(
    const d1l_usb_command_view_t *command,
    d1l_release_feature_id_t feature)
{
    printf("{\"schema\":%d,\"ok\":false,\"cmd\":", D1L_CONSOLE_SCHEMA);
    print_json_string(command && command->text ? command->text : "");
    printf(",\"code\":\"ESP_ERR_NOT_SUPPORTED\",\"release_profile\":");
    print_json_string(d1l_release_profile_name());
    printf(",\"feature\":");
    print_json_string(d1l_release_feature_name(feature));
    printf("}\n");
}

static bool release_command_feature_available(
    d1l_release_feature_id_t feature)
{
    if (feature == D1L_RELEASE_FEATURE_SD_HISTORY) {
        return d1l_release_sd_history_mode() !=
               D1L_SD_HISTORY_MODE_DISABLED;
    }
    return d1l_release_feature_available(feature);
}

static bool enforce_release_command_policy(
    const d1l_usb_command_view_t *command)
{
#if D1L_ENABLE_QUALIFICATION_HOOKS
    d1l_release_feature_id_t ui_probe_feature =
        D1L_RELEASE_FEATURE_COUNT;
    if (release_ui_probe_feature(command, &ui_probe_feature) &&
        !release_command_feature_available(ui_probe_feature)) {
        release_unsupported_result(command, ui_probe_feature);
        return false;
    }
#endif

    const d1l_release_command_rule_t *rule = release_command_rule(command);
    if (d1l_release_profile_is_core() && rule &&
        rule->feature == D1L_RELEASE_FEATURE_SD_HISTORY &&
        rule->access == D1L_RELEASE_COMMAND_READ_ONLY &&
        d1l_release_sd_history_mode() == D1L_SD_HISTORY_MODE_DISABLED &&
        !d1l_usb_command_equals(
            command, "storage status", sizeof("storage status") - 1U)) {
        /*
         * A disabled-SD Core build must not actively interrogate the excluded
         * RP2040 bridge or media and must not imply that either is supported.
         * Keep storage status as the one passive NVS-authority projection.
         */
        release_unavailable_status(
            command && command->text ? command->text : rule->pattern,
            D1L_RELEASE_FEATURE_SD_HISTORY);
        return false;
    }
    if (!rule || release_command_feature_available(rule->feature) ||
        rule->access == D1L_RELEASE_COMMAND_READ_ONLY) {
        return true;
    }
    release_unsupported_result(command, rule->feature);
    return false;
}

static void print_hex_bytes_json(const uint8_t *bytes, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    putchar('"');
    for (size_t i = 0; bytes && i < len; ++i) {
        putchar(hex[(bytes[i] >> 4U) & 0x0FU]);
        putchar(hex[bytes[i] & 0x0FU]);
    }
    putchar('"');
}

static void print_retained_sd_store_json(const d1l_retained_blob_store_sd_stats_t *stats)
{
    const d1l_retained_blob_store_sd_stats_t empty = {0};
    if (!stats) {
        stats = &empty;
    }
    printf("{\"sd_read_fail_count\":%lu,\"sd_write_fail_count\":%lu,"
           "\"sd_rename_fail_count\":%lu,\"sd_last_error\":\"%s\","
           "\"sd_degraded_latched\":%s,\"nvs_mirror_fail_count\":%lu,"
           "\"nvs_mirror_last_error\":\"%s\"}",
           (unsigned long)stats->sd_read_fail_count,
           (unsigned long)stats->sd_write_fail_count,
           (unsigned long)stats->sd_rename_fail_count,
           esp_err_to_name(stats->sd_last_error),
           bool_json(stats->sd_degraded_latched),
           (unsigned long)stats->nvs_mirror_fail_count,
           esp_err_to_name(stats->nvs_mirror_last_error));
}

static void print_retained_nvs_store_telemetry_json(
    const d1l_retained_blob_store_nvs_store_telemetry_t *telemetry)
{
    const d1l_retained_blob_store_nvs_store_telemetry_t empty = {0};
    if (!telemetry) {
        telemetry = &empty;
    }
    printf("{\"write_attempts\":%" PRIu64 ",\"write_commits\":%" PRIu64
           ",\"write_failures\":%" PRIu64
           ",\"write_bytes_attempted\":%" PRIu64
           ",\"write_bytes_committed\":%" PRIu64
           ",\"erase_attempts\":%" PRIu64
           ",\"erase_commits\":%" PRIu64
           ",\"erase_failures\":%" PRIu64 ",\"last_error\":\"%s\"}",
           telemetry->write_attempt_count,
           telemetry->write_commit_count,
           telemetry->write_fail_count,
           telemetry->write_bytes_attempted,
           telemetry->write_bytes_committed,
           telemetry->erase_attempt_count,
           telemetry->erase_commit_count,
           telemetry->erase_fail_count,
           esp_err_to_name(telemetry->last_error));
}

static void print_retained_nvs_telemetry_json(
    const d1l_retained_blob_store_nvs_telemetry_t *telemetry)
{
    const d1l_retained_blob_store_nvs_telemetry_t empty = {0};
    if (!telemetry) {
        telemetry = &empty;
    }
    printf("{\"scope\":\"boot_runtime_api_calls\","
           "\"physical_flash_cycles_measured\":false,\"capacity\":{"
           "\"valid\":%s,\"error\":\"%s\",\"used_entries\":%lu,"
           "\"free_entries\":%lu,\"available_entries\":%lu,"
           "\"total_entries\":%lu,\"namespace_count\":%lu},"
           "\"write_amplification\":{\"write_attempts\":%" PRIu64
           ",\"write_commits\":%" PRIu64
           ",\"write_failures\":%" PRIu64
           ",\"write_bytes_attempted\":%" PRIu64
           ",\"write_bytes_committed\":%" PRIu64
           ",\"erase_attempts\":%" PRIu64
           ",\"erase_commits\":%" PRIu64
           ",\"erase_failures\":%" PRIu64 ",\"last_error\":\"%s\"},"
           "\"stores\":{\"messages\":",
           bool_json(telemetry->capacity_valid),
           esp_err_to_name(telemetry->capacity_error),
           (unsigned long)telemetry->used_entries,
           (unsigned long)telemetry->free_entries,
           (unsigned long)telemetry->available_entries,
           (unsigned long)telemetry->total_entries,
           (unsigned long)telemetry->namespace_count,
           telemetry->write_attempt_count,
           telemetry->write_commit_count,
           telemetry->write_fail_count,
           telemetry->write_bytes_attempted,
           telemetry->write_bytes_committed,
           telemetry->erase_attempt_count,
           telemetry->erase_commit_count,
           telemetry->erase_fail_count,
           esp_err_to_name(telemetry->last_error));
    print_retained_nvs_store_telemetry_json(
        &telemetry->stores[D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES]);
    printf(",\"dm\":");
    print_retained_nvs_store_telemetry_json(
        &telemetry->stores[D1L_RETAINED_BLOB_STORE_DM_MESSAGES]);
    printf(",\"routes\":");
    print_retained_nvs_store_telemetry_json(
        &telemetry->stores[D1L_RETAINED_BLOB_STORE_ROUTES]);
    printf(",\"packets\":");
    print_retained_nvs_store_telemetry_json(
        &telemetry->stores[D1L_RETAINED_BLOB_STORE_PACKET_LOG]);
    printf(",\"nodes\":");
    print_retained_nvs_store_telemetry_json(
        &telemetry->stores[D1L_RETAINED_BLOB_STORE_NODES]);
    printf("}}");
}

static void print_retained_scheduler_json(
    const d1l_retained_store_worker_status_t *status)
{
    const d1l_retained_store_pass_t *pass = &status->last_pass;
    printf("{\"running\":%s,\"active\":{\"force\":%s,"
           "\"deadline_us\":%" PRId64 ",\"store_active\":%s,"
           "\"store_index\":%u,\"store_started_us\":%" PRId64
           ",\"store\":",
           bool_json(status->running), bool_json(status->active_force),
           status->active_deadline_us,
           bool_json(status->active_store_name[0] != '\0'),
           (unsigned)status->active_store_index,
           status->active_store_started_us);
    print_json_string(status->active_store_name);
    printf("},\"passes\":%lu,\"forced_passes\":%lu,"
           "\"background_passes\":%lu,\"deadline_exhaustions\":%lu,"
           "\"quiesce_cancellations\":%lu,\"last_pass\":{\"force\":%s,"
           "\"deadline_us\":%" PRId64 ",\"started_us\":%" PRId64 ","
           "\"finished_us\":%" PRId64 ",\"result\":\"%s\","
           "\"deadline_exhausted\":%s,\"quiesce_cancelled\":%s,"
           "\"attempted\":%u,\"committed\":%u,\"coalesced\":%u,"
           "\"skipped_clean\":%u,\"failed\":%u,\"stores\":[",
           (unsigned long)status->pass_count,
           (unsigned long)status->forced_pass_count,
           (unsigned long)status->background_pass_count,
           (unsigned long)status->deadline_exhausted_count,
           (unsigned long)status->quiesce_cancelled_count,
           bool_json(pass->force), pass->deadline_us, pass->started_us,
           pass->finished_us, esp_err_to_name(pass->result),
           bool_json(pass->deadline_exhausted),
           bool_json(pass->quiesce_cancelled),
           (unsigned)pass->attempted_count,
           (unsigned)pass->committed_count,
           (unsigned)pass->coalesced_count,
           (unsigned)pass->skipped_clean_count,
           (unsigned)pass->failed_count);
    for (size_t i = 0; i < pass->store_count; ++i) {
        const d1l_retained_store_result_t *store = &pass->stores[i];
        if (i > 0U) {
            printf(",");
        }
        printf("{\"name\":");
        print_json_string(store->name);
        printf(",\"outcome\":");
        print_json_string(d1l_retained_store_outcome_name(store->outcome));
        printf(",\"result\":\"%s\",\"before\":{\"revision\":%" PRIu64
               ",\"dirty\":%s,\"reconcile_pending\":%s,\"commits\":%lu,"
               "\"failures\":%lu},\"after\":{\"revision\":%" PRIu64
               ",\"dirty\":%s,\"reconcile_pending\":%s,\"commits\":%lu,"
               "\"failures\":%lu}}",
               esp_err_to_name(store->result),
               store->before.revision, bool_json(store->before.dirty),
               bool_json(store->before.reconcile_pending),
               (unsigned long)store->before.commit_count,
               (unsigned long)store->before.failure_count,
               store->after.revision, bool_json(store->after.dirty),
               bool_json(store->after.reconcile_pending),
               (unsigned long)store->after.commit_count,
               (unsigned long)store->after.failure_count);
    }
    printf("]}}");
}

static bool parse_fingerprint_token(const char *src, char *dest, size_t dest_size)
{
    if (!src || !dest || dest_size < D1L_NODE_FINGERPRINT_LEN) {
        return false;
    }
    size_t len = 0;
    while (src[len] != '\0' && !isspace((unsigned char)src[len])) {
        len++;
    }
    if (len != D1L_NODE_FINGERPRINT_LEN - 1U) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (!isxdigit(c)) {
            return false;
        }
        dest[i] = (char)tolower(c);
    }
    dest[len] = '\0';
    return true;
}

static bool parse_bool_token(const char *src, bool *out_value)
{
    if (!src || !out_value) {
        return false;
    }
    if (strcmp(src, "1") == 0 || strcmp(src, "true") == 0 || strcmp(src, "on") == 0) {
        *out_value = true;
        return true;
    }
    if (strcmp(src, "0") == 0 || strcmp(src, "false") == 0 || strcmp(src, "off") == 0) {
        *out_value = false;
        return true;
    }
    return false;
}

static void hex_prefix(char *dest, size_t dest_size, const uint8_t *src, size_t src_len)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!dest || dest_size == 0) {
        return;
    }
    size_t out = 0;
    for (size_t i = 0; src && i < src_len && out + 2U < dest_size; ++i) {
        dest[out++] = hex[(src[i] >> 4) & 0x0fU];
        dest[out++] = hex[src[i] & 0x0fU];
    }
    dest[out] = '\0';
}

static void print_hex_field(const char *name, const uint8_t *src, size_t src_len)
{
    char text[(D1L_TOUCH_RAW_REG_BYTES * 2U) + 1U] = {0};
    hex_prefix(text, sizeof(text), src, src_len);
    printf(",\"%s\":\"%s\"", name, text);
}

static void sanitize_node_name(char *name)
{
    if (name == NULL) {
        return;
    }
    for (size_t i = 0; i < D1L_NODE_NAME_LEN && name[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (c < 32 || c > 126 || c == '"' || c == '\\') {
            name[i] = '_';
        }
    }
}

static const char *time_protocol_recovery(
    const d1l_time_service_status_t *status)
{
    if (!status) {
        return "service_recovery_required";
    }
    if (status->protocol_tx_ready) {
        return "none";
    }
    if (status->protocol_migration.state ==
        D1L_TIME_PROTOCOL_MIGRATION_REQUIRED) {
        return "exact_device_protocol_upper_bound_confirmation_required";
    }
    if (status->protocol_migration.state ==
        D1L_TIME_PROTOCOL_MIGRATION_PENDING) {
        return "resume_exact_device_protocol_migration";
    }
    if (status->protocol_migration.write_blocked) {
        return "preserve_and_repair_protocol_migration_receipt";
    }
    if (!status->protocol_persistence_ready) {
        return "protocol_persistence_migration_or_repair";
    }
    if (status->clock.protocol_exhausted) {
        return "protocol_upgrade_required";
    }
    if (status->clock.protocol_wall_admission ==
        D1L_TIME_PROTOCOL_WALL_SNTP_FORWARD_BLOCKED) {
        return "authenticated_companion_or_newer_firmware";
    }
    if (status->clock.protocol_wall_admission ==
        D1L_TIME_PROTOCOL_WALL_UNREPRESENTABLE) {
        return "representable_authenticated_companion_or_protocol_upgrade";
    }
    return "service_recovery_required";
}

static const char *time_protocol_tx_block(
    const d1l_time_service_status_t *status)
{
    if (!status) {
        return "service_unavailable";
    }
    if (status->protocol_tx_ready) {
        return "none";
    }
    if (status->protocol_migration.state ==
        D1L_TIME_PROTOCOL_MIGRATION_REQUIRED) {
        return "legacy_protocol_lower_bound_unconfirmed";
    }
    if (status->protocol_migration.state ==
        D1L_TIME_PROTOCOL_MIGRATION_PENDING) {
        return "legacy_protocol_migration_incomplete";
    }
    if (status->protocol_migration.write_blocked) {
        return "legacy_protocol_migration_quarantined";
    }
    if (!status->protocol_persistence_ready) {
        return "persistence_unavailable";
    }
    if (status->clock.protocol_exhausted) {
        return "permanent_u32_ceiling_exhausted";
    }
    if (status->clock.protocol_wall_admission ==
        D1L_TIME_PROTOCOL_WALL_SNTP_FORWARD_BLOCKED) {
        return "recoverable_sntp_ahead_rejection";
    }
    if (status->clock.protocol_wall_admission ==
        D1L_TIME_PROTOCOL_WALL_UNREPRESENTABLE) {
        return "recoverable_wall_unrepresentable";
    }
    return "service_unavailable";
}

static const char *time_protocol_migration_stage(
    const d1l_time_protocol_migration_status_t *status)
{
    if (!status) {
        return "status_unavailable";
    }
    if (status->state == D1L_TIME_PROTOCOL_MIGRATION_REQUIRED) {
        return "awaiting_operator_confirmation";
    }
    if (status->state == D1L_TIME_PROTOCOL_MIGRATION_PENDING) {
        if (!status->high_water_present) {
            return "intent_committed_high_water_pending";
        }
        if (status->legacy_present) {
            return "high_water_committed_legacy_erase_pending";
        }
        return "legacy_erased_completion_receipt_pending";
    }
    if (status->state == D1L_TIME_PROTOCOL_MIGRATION_COMPLETE) {
        return "completion_receipt_committed";
    }
    if (status->state == D1L_TIME_PROTOCOL_MIGRATION_ABSENT) {
        return "legacy_not_present";
    }
    if (status->state == D1L_TIME_PROTOCOL_MIGRATION_STORAGE_ERROR) {
        return "storage_repair_required";
    }
    return "quarantined_preserve_nvs";
}

static void cmd_time_migration_status(void)
{
    d1l_time_service_status_t time_status;
    d1l_time_service_status(&time_status);
    const d1l_time_protocol_migration_status_t *status =
        &time_status.protocol_migration;
    ok_begin("time migration status");
    printf(
        ",\"automatic\":false,\"wall_time_inferred\":false,"
        "\"state\":\"%s\",\"state_code\":%u,\"stage\":\"%s\","
        "\"error\":\"%s\",\"error_code\":%ld,"
        "\"receipt\":{\"present\":%s,\"schema_version\":%lu,"
        "\"phase\":%lu,\"revision\":%lu,\"intent_committed\":%s,"
        "\"completion_committed\":%s},"
        "\"legacy\":{\"present\":%s,\"observed_mesh_ts\":%lu,"
        "\"attested_mesh_ts\":%lu},"
        "\"high_water\":{\"present\":%s,\"observed\":%lu,"
        "\"confirmed_upper_bound\":%lu,\"target\":%lu},"
        "\"confirmation_required\":%s,\"resume_required\":%s,"
        "\"write_blocked\":%s,\"protocol_tx_ready\":%s,"
        "\"protocol_tx_block\":\"%s\",\"retry_idempotent\":true,"
        "\"supplied_confirmation_logged\":false,"
        "\"settings_reset_scope\":\"settings_model_only\","
        "\"settings_reset_preserves_protocol_migration\":true,"
        "\"factory_reset_supported\":true}\n",
        d1l_time_protocol_migration_state_name(status->state),
        (unsigned)status->state,
        time_protocol_migration_stage(status),
        esp_err_to_name(status->error), (long)status->error,
        bool_json(status->receipt_found),
        (unsigned long)status->receipt_schema_version,
        (unsigned long)status->receipt_phase,
        (unsigned long)status->revision,
        bool_json(status->intent_committed),
        bool_json(status->completion_committed),
        bool_json(status->legacy_present),
        (unsigned long)status->observed_legacy_value,
        (unsigned long)status->legacy_value,
        bool_json(status->high_water_present),
        (unsigned long)status->observed_high_water,
        (unsigned long)status->confirmed_upper_bound,
        (unsigned long)status->target_high_water,
        bool_json(status->confirmation_required),
        bool_json(status->resume_required),
        bool_json(status->write_blocked),
        bool_json(time_status.protocol_tx_ready),
        time_protocol_tx_block(&time_status));
}

static void cmd_version(void)
{
    d1l_time_service_status_t time_status;
    d1l_time_service_status(&time_status);
    const char *recovery = time_protocol_recovery(&time_status);
    const char *tx_block = time_protocol_tx_block(&time_status);
    ok_begin("version");
    printf(",\"firmware\":\"%s\",\"version\":\"%s\",\"build_commit\":\"%s\","
           "\"idf\":\"%s\",\"release_profile\":\"%s\","
           "\"release_profile_id\":%u,\"sd_history_mode\":\"%s\","
           "\"sd_history_mode_id\":%u,\"meshcore_ca_desk_mode\":true,"
           "\"time\":{\"build_epoch_sec\":%lu,\"wall_valid\":%s,"
           "\"wall_epoch_sec\":%" PRId64 ",\"validity\":\"%s\","
           "\"source\":\"%s\",\"display_time_valid\":%s,"
           "\"display_time\":\"%s\",\"display_approximate\":%s,"
           "\"timezone\":{\"settings_ready\":%s,\"schema_version\":%u,"
           "\"model\":\"fixed_utc_offset\",\"offset_minutes\":%d,"
           "\"label\":\"%s\",\"auto_dst\":false,"
           "\"settings_error\":\"%s\"},"
           "\"protocol_wall_admission\":\"%s\","
           "\"protocol_persistence_state\":\"%s\","
           "\"protocol_persistence_state_code\":%u,"
           "\"protocol_persistence_error\":\"%s\","
           "\"protocol_persistence_error_code\":%ld,"
           "\"protocol_tx_ready\":%s,\"protocol_tx_error\":\"%s\","
           "\"protocol_tx_block\":\"%s\","
           "\"protocol_trust_anchor\":%lu,\"sntp_ceiling\":%lu,"
            "\"protocol_ahead_of_wall_sec\":%" PRId64 ","
            "\"recovery\":\"%s\","
            "\"migration\":{\"automatic\":false,\"state\":\"%s\","
            "\"state_code\":%u,\"error\":\"%s\",\"error_code\":%ld,"
            "\"receipt_schema_version\":%lu,\"receipt_phase\":%lu,"
            "\"receipt_revision\":%lu,\"receipt_present\":%s,"
            "\"legacy_present\":%s,\"legacy_value\":%lu,"
            "\"observed_legacy_value\":%lu,"
            "\"confirmed_upper_bound\":%lu,\"target_high_water\":%lu,"
            "\"high_water_present\":%s,\"observed_high_water\":%lu,"
            "\"confirmation_required\":%s,\"resume_required\":%s,"
            "\"write_blocked\":%s,\"intent_committed\":%s,"
            "\"completion_committed\":%s,\"wall_time_inferred\":false},"
            "\"checkpoint\":{\"state\":\"%s\",\"state_code\":%u,"
           "\"error\":\"%s\",\"error_code\":%ld,"
           "\"revision\":%lu,\"epoch_sec\":%" PRId64 ","
           "\"protocol_reserved_through\":%lu,\"source\":\"%s\","
           "\"present\":%s,\"recovered\":%s,\"pending\":%s,"
           "\"write_blocked\":%s,"
           "\"recovery_error\":\"%s\",\"recovery_error_code\":%ld,"
           "\"write_count\":%lu,\"skip_count\":%lu,"
           "\"failure_count\":%lu,\"retry_not_before_us\":%" PRIu64
           "}}}\n",
           D1L_FIRMWARE_NAME, D1L_FIRMWARE_VERSION, D1L_BUILD_GIT_COMMIT,
           esp_get_idf_version(), d1l_release_profile_name(),
           (unsigned)d1l_release_profile_id(),
           d1l_release_sd_history_mode_name(),
           (unsigned)d1l_release_sd_history_mode(),
           (unsigned long)time_status.clock.build_epoch_sec,
           bool_json(time_status.clock.wall_valid),
           time_status.clock.wall_epoch_sec,
           d1l_time_validity_name(time_status.clock.wall_validity),
           d1l_time_source_name(time_status.clock.wall_source),
           bool_json(time_status.display_time_valid),
           time_status.display_time,
           bool_json(time_status.display_time_approximate),
           bool_json(time_status.timezone_settings_ready),
           (unsigned)time_status.timezone_schema_version,
           (int)time_status.timezone_offset_minutes,
           time_status.timezone_label,
           esp_err_to_name(time_status.timezone_settings_error),
           d1l_time_protocol_wall_admission_name(
               time_status.clock.protocol_wall_admission),
           d1l_time_protocol_persistence_state_name(
               time_status.protocol_persistence_state),
           (unsigned)time_status.protocol_persistence_state,
           esp_err_to_name(time_status.protocol_persistence_error),
           (long)time_status.protocol_persistence_error,
           bool_json(time_status.protocol_tx_ready),
           esp_err_to_name(time_status.protocol_tx_error),
           tx_block,
           (unsigned long)time_status.clock.protocol_trust_anchor,
           (unsigned long)time_status.clock.protocol_sntp_ceiling,
            time_status.clock.protocol_ahead_of_wall_sec,
            recovery,
            d1l_time_protocol_migration_state_name(
                time_status.protocol_migration.state),
            (unsigned)time_status.protocol_migration.state,
             esp_err_to_name(time_status.protocol_migration.error),
             (long)time_status.protocol_migration.error,
             (unsigned long)
                 time_status.protocol_migration.receipt_schema_version,
             (unsigned long)time_status.protocol_migration.receipt_phase,
             (unsigned long)time_status.protocol_migration.revision,
            bool_json(time_status.protocol_migration.receipt_found),
            bool_json(time_status.protocol_migration.legacy_present),
            (unsigned long)time_status.protocol_migration.legacy_value,
            (unsigned long)
                time_status.protocol_migration.observed_legacy_value,
            (unsigned long)
                time_status.protocol_migration.confirmed_upper_bound,
            (unsigned long)time_status.protocol_migration.target_high_water,
            bool_json(time_status.protocol_migration.high_water_present),
            (unsigned long)
                time_status.protocol_migration.observed_high_water,
            bool_json(time_status.protocol_migration.confirmation_required),
             bool_json(time_status.protocol_migration.resume_required),
             bool_json(time_status.protocol_migration.write_blocked),
             bool_json(time_status.protocol_migration.intent_committed),
             bool_json(time_status.protocol_migration.completion_committed),
            d1l_settings_time_checkpoint_state_name(
               time_status.wall_checkpoint.state),
           (unsigned)time_status.wall_checkpoint.state,
           esp_err_to_name(time_status.wall_checkpoint.error),
           (long)time_status.wall_checkpoint.error,
           (unsigned long)time_status.wall_checkpoint.revision,
           time_status.wall_checkpoint.epoch_sec,
           (unsigned long)
               time_status.wall_checkpoint.protocol_reserved_through,
           d1l_settings_time_checkpoint_source_name(
               time_status.wall_checkpoint.source),
           bool_json(time_status.wall_checkpoint.found),
           bool_json(time_status.wall_checkpoint_recovered),
           bool_json(time_status.wall_checkpoint_pending),
           bool_json(time_status.wall_checkpoint_write_blocked),
           esp_err_to_name(time_status.wall_checkpoint_recovery_error),
           (long)time_status.wall_checkpoint_recovery_error,
           (unsigned long)time_status.wall_checkpoint_write_count,
           (unsigned long)time_status.wall_checkpoint_skip_count,
           (unsigned long)time_status.wall_checkpoint_failure_count,
           time_status.wall_checkpoint_retry_not_before_us);
}

static void cmd_time_migrate_legacy(const char *line)
{
    const char *cursor = line + strlen("time migrate-legacy ");
    uint32_t expected_legacy_value = 0U;
    uint32_t confirmed_upper_bound = 0U;
    if (!parse_next_u32_arg(&cursor, &expected_legacy_value) ||
        !parse_next_u32_arg(&cursor, &confirmed_upper_bound) ||
        strcmp(cursor, D1L_TIME_PROTOCOL_MIGRATION_CONFIRMATION) != 0) {
        err_result(
            "time migrate-legacy", "CONFIRMATION_REQUIRED",
            "usage: time migrate-legacy <expected-mesh-ts> <confirmed-upper-bound> "
            D1L_TIME_PROTOCOL_MIGRATION_CONFIRMATION);
        return;
    }
    bool written = false;
    d1l_time_protocol_migration_status_t status = {
        .state = D1L_TIME_PROTOCOL_MIGRATION_UNINITIALIZED,
        .error = ESP_ERR_INVALID_STATE,
    };
    const esp_err_t ret =
        d1l_time_service_migrate_legacy_protocol_timestamp(
            expected_legacy_value, confirmed_upper_bound, cursor,
            &written, &status);
    if (ret != ESP_OK) {
        printf("{\"schema\":%d,\"cmd\":\"time migrate-legacy\","
               "\"ok\":false,\"error\":\"%s\",\"error_code\":%ld,"
               "\"state\":\"%s\",\"stage\":\"%s\",\"written\":%s,"
               "\"retry_idempotent\":true,"
               "\"supplied_confirmation_logged\":false}\n",
               D1L_CONSOLE_SCHEMA, esp_err_to_name(ret), (long)ret,
               d1l_time_protocol_migration_state_name(status.state),
               time_protocol_migration_stage(&status), bool_json(written));
        return;
    }
    d1l_time_service_status_t time_status;
    d1l_time_service_status(&time_status);
    const char *tx_block = time_protocol_tx_block(&time_status);
    ok_begin("time migrate-legacy");
    printf(",\"written\":%s,\"automatic\":false,\"state\":\"%s\","
           "\"receipt_revision\":%lu,\"legacy_value\":%lu,"
           "\"confirmed_upper_bound\":%lu,\"target_high_water\":%lu,"
           "\"protocol_tx_unblocked\":%s,\"protocol_tx_ready\":%s,"
           "\"protocol_tx_block\":\"%s\",\"wall_time_inferred\":false,"
           "\"retry_idempotent\":true,"
           "\"supplied_confirmation_logged\":false,"
           "\"settings_reset_preserves_protocol_migration\":true,"
           "\"factory_reset_supported\":true}\n",
           bool_json(written),
           d1l_time_protocol_migration_state_name(status.state),
           (unsigned long)status.revision,
           (unsigned long)status.legacy_value,
           (unsigned long)status.confirmed_upper_bound,
           (unsigned long)status.target_high_water,
           bool_json(time_status.protocol_tx_ready),
           bool_json(time_status.protocol_tx_ready), tx_block);
}

static void cmd_board(void)
{
    const d1l_board_status_t *status = d1l_board_status();
    ok_begin("board");
    printf(",\"target\":\"seeed_indicator_d1l\",\"ready\":%s,\"init_result\":\"%s\",\"display\":{\"width\":480,\"height\":480},\"button_gpio\":38,\"backlight_gpio\":45}\n",
           status->ready ? "true" : "false", esp_err_to_name(status->init_result));
}

static void cmd_settings_get(void)
{
    d1l_settings_t settings_snapshot = {0};
    const esp_err_t settings_snapshot_error =
        d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    const d1l_meshcore_service_status_t mesh = d1l_meshcore_service_status();
    char timezone_label[D1L_TIMEZONE_LABEL_LEN] = {0};
    (void)d1l_time_display_timezone_label(
        settings->timezone_offset_minutes, timezone_label,
        sizeof(timezone_label));
    const bool wifi_available = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL);
    const bool ble_available = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_BLE);
    const bool observer_available = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_OBSERVER_MQTT);
    const bool map_available = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_MAP);
    const bool location_available = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_LOCATION);
    const bool location_set =
        location_available && settings->map_location_set;
    ok_begin("settings get");
    print_release_profile_fields();
    printf(",\"capabilities\":{\"wifi_user_control\":%s,\"ble\":%s,"
           "\"observer_mqtt\":%s,\"map\":%s,\"location\":%s},"
           "\"node_name\":\"%s\",\"role\":\"%s\","
           "\"onboarding_complete\":%s,\"wifi_enabled\":%s,"
           "\"ble_companion_enabled\":%s,\"observer_enabled\":%s,"
           "\"high_contrast\":%s,\"night_mode\":%s,\"path_hash_bytes\":%u,"
           "\"timezone\":{\"settings_ready\":%s,\"settings_error\":\"%s\","
           "\"schema_version\":%u,\"model\":\"fixed_utc_offset\","
           "\"offset_minutes\":%d,\"label\":\"%s\",\"auto_dst\":false},"
           "\"map_location\":{\"available\":%s,\"set\":%s,\"lat\":",
           bool_json(wifi_available), bool_json(ble_available),
           bool_json(observer_available), bool_json(map_available),
           bool_json(location_available),
           settings->node_name, d1l_settings_role_name(settings->role),
           bool_json(settings->onboarding_complete),
           bool_json(wifi_available && settings->wifi_enabled),
           bool_json(ble_available && settings->ble_companion_enabled),
           bool_json(observer_available && settings->observer_enabled),
           bool_json(settings->high_contrast),
           bool_json(settings->night_mode), settings->path_hash_bytes,
           bool_json(settings_snapshot_error == ESP_OK),
           esp_err_to_name(settings_snapshot_error),
           (unsigned)settings->timezone_schema_version,
           (int)settings->timezone_offset_minutes, timezone_label,
           bool_json(location_available), bool_json(location_set));
    print_e7_json(location_set ? settings->map_lat_e7 : 0);
    printf(",\"lon\":");
    print_e7_json(location_set ? settings->map_lon_e7 : 0);
    printf(",\"source\":");
    print_json_string(!location_available ? "unavailable_in_release_profile" :
                      map_location_source_name(settings));
    printf("},\"map_tiles\":{\"available\":%s,\"source\":",
           bool_json(map_available));
    print_json_string(map_available ? D1L_MAP_TILE_SOURCE_ID : "");
    printf(",\"built_in\":%s,\"zoom\":%u,\"url_template\":",
           bool_json(map_available),
           map_available ? (unsigned)settings->map_tile_zoom : 0U);
    print_json_string(map_available ? D1L_MAP_TILE_SOURCE_URL_TEMPLATE : "");
    printf(",\"attribution\":");
    print_json_string(map_available ? D1L_MAP_TILE_ATTRIBUTION : "");
    printf(",\"policy\":");
    print_json_string(map_available ? D1L_MAP_TILE_PROVIDER_POLICY :
                      "unavailable_in_release_profile");
    printf("},\"radio\":{\"frequency_hz\":%lu,\"bandwidth_khz\":%.1f,"
           "\"sf\":%u,\"cr\":%u,\"tx_power_dbm\":%d,\"rx_boost\":%s,"
           "\"tcxo\":\"%s\",\"applied_to_radio\":%s,"
           "\"radio_apply_pending\":%s,\"radio_apply_error\":\"%s\"}}\n",
           (unsigned long)settings->frequency_hz,
           ((float)settings->bandwidth_tenths_khz) / 10.0f,
           settings->spreading_factor, settings->coding_rate, settings->tx_power_dbm,
           bool_json(settings->rx_boost), d1l_settings_tcxo_name(settings->tcxo_mode),
           bool_json(mesh.radio_applied), bool_json(mesh.radio_apply_pending),
           esp_err_to_name(mesh.radio_apply_error));
}

static void cmd_settings_reset(void)
{
    esp_err_t ret = d1l_settings_reset();
    if (ret != ESP_OK) {
        err_result("settings reset", esp_err_to_name(ret), "settings reset failed");
        return;
    }
    d1l_meshcore_service_init();
    ok_begin("settings reset");
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    printf(",\"persisted\":true,\"node_name\":\"%s\","
           "\"reset_scope\":\"settings_model_only\","
           "\"protocol_migration_preserved\":true,"
           "\"protocol_high_water_preserved\":true,"
           "\"factory_reset\":false}\n", settings.node_name);
}

static void cmd_settings_onboarding_status(void)
{
    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    ok_begin("settings onboarding status");
    print_release_profile_fields();
    printf(",\"complete\":%s,\"node_name\":\"%s\",\"role\":\"%s\","
           "\"region\":\"Canada/USA\","
           "\"radio_profile\":\"uscan-meshcore-default\","
           "\"wifi_enabled\":%s,\"ble_companion_enabled\":%s,"
           "\"observer_enabled\":%s}\n",
           bool_json(settings->onboarding_complete), settings->node_name,
           d1l_settings_role_name(settings->role),
           bool_json(
               d1l_release_feature_available(
                   D1L_RELEASE_FEATURE_WIFI_USER_CONTROL) &&
               settings->wifi_enabled),
           bool_json(
               d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE) &&
               settings->ble_companion_enabled),
           bool_json(
               d1l_release_feature_available(
                   D1L_RELEASE_FEATURE_OBSERVER_MQTT) &&
               settings->observer_enabled));
}

static void cmd_settings_onboarding_reset(void)
{
    esp_err_t ret = d1l_settings_reset_onboarding();
    if (ret != ESP_OK) {
        err_result("settings onboarding reset", esp_err_to_name(ret), "could not reset onboarding state");
        return;
    }
    ok_begin("settings onboarding reset");
    printf(",\"complete\":false,\"persisted\":true}\n");
}

static void cmd_settings_onboarding_complete(const char *line)
{
    const char *arg = line + strlen("settings onboarding complete ");
    while (*arg == ' ') {
        arg++;
    }
    if (*arg == '\0') {
        err_result("settings onboarding complete", "INVALID_NAME",
                   "usage: settings onboarding complete <1-31 chars>");
        return;
    }
    size_t arg_len = strlen(arg);
    if (arg_len >= D1L_NODE_NAME_LEN) {
        err_result("settings onboarding complete", "INVALID_NAME", "usage: settings onboarding complete <1-31 chars>");
        return;
    }
    esp_err_t ret = d1l_app_model_complete_onboarding(arg);
    if (ret != ESP_OK) {
        err_result("settings onboarding complete", esp_err_to_name(ret), "could not persist onboarding choices");
        return;
    }
    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    ok_begin("settings onboarding complete");
    printf(",\"complete\":true,\"persisted\":true,\"node_name\":\"%s\",\"role\":\"%s\",\"wifi_enabled\":false,\"ble_companion_enabled\":false,\"observer_enabled\":false,\"identity_ready\":%s}\n",
           settings->node_name, d1l_settings_role_name(settings->role),
           bool_json(settings->identity_ready));
}

static void cmd_settings_set_name(const char *line)
{
    const char *arg = line + strlen("settings set name ");
    while (*arg == ' ') {
        arg++;
    }
    if (*arg == '\0') {
        err_result("settings set name", "INVALID_NAME", "usage: settings set name <1-31 chars>");
        return;
    }
    size_t arg_len = strlen(arg);
    if (arg_len >= D1L_NODE_NAME_LEN) {
        err_result("settings set name", "INVALID_NAME", "usage: settings set name <1-31 chars>");
        return;
    }
    d1l_settings_t settings = {0};
    memcpy(settings.node_name, arg, arg_len + 1U);
    sanitize_node_name(settings.node_name);
    esp_err_t ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_NODE_NAME);
    if (ret != ESP_OK) {
        err_result("settings set name", esp_err_to_name(ret), "could not persist node name");
        return;
    }
    ok_begin("settings set name");
    printf(",\"persisted\":true,\"node_name\":\"%s\"}\n", settings.node_name);
}

static void cmd_settings_set_pathhash(const char *line)
{
    int value = 0;
    if (!d1l_usb_command_parse_int_exact(
            line + strlen("settings set pathhash "), &value) ||
        value < 1 || value > 3) {
        err_result("settings set pathhash", "INVALID_PATH_HASH_BYTES", "usage: settings set pathhash <1|2|3>");
        return;
    }
    d1l_settings_t settings = {0};
    settings.path_hash_bytes = (uint8_t)value;
    esp_err_t ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_PATH_HASH);
    if (ret != ESP_OK) {
        err_result("settings set pathhash", esp_err_to_name(ret), "could not persist path hash setting");
        return;
    }
    d1l_meshcore_service_init();
    ok_begin("settings set pathhash");
    printf(",\"persisted\":true,\"path_hash_bytes\":%u,\"legacy_warning\":%s}\n",
           (unsigned)settings.path_hash_bytes,
           value > 1 ? "true" : "false");
}

static void cmd_settings_set_timezone(const char *line)
{
    const char *arg = line + strlen("settings set timezone ");
    while (*arg == ' ') {
        arg++;
    }
    int16_t offset_minutes = 0;
    if (!d1l_time_display_parse_timezone(arg, &offset_minutes)) {
        err_result("settings set timezone", "INVALID_TIMEZONE",
                   "usage: settings set timezone <UTC|UTC+HH:MM|UTC-HH:MM>");
        return;
    }
    const esp_err_t ret =
        d1l_app_model_set_timezone_offset_minutes(offset_minutes);
    if (ret != ESP_OK) {
        err_result("settings set timezone", esp_err_to_name(ret),
                   "could not persist fixed UTC display offset");
        return;
    }
    char label[D1L_TIMEZONE_LABEL_LEN] = {0};
    (void)d1l_time_display_timezone_label(
        offset_minutes, label, sizeof(label));
    ok_begin("settings set timezone");
    printf(",\"persisted\":true,\"model\":\"fixed_utc_offset\","
           "\"offset_minutes\":%d,\"label\":\"%s\","
           "\"auto_dst\":false}\n",
           (int)offset_minutes, label);
}

static void print_map_location_result(const char *cmd, const d1l_settings_t *settings,
                                      bool persisted)
{
    ok_begin(cmd);
    printf(",\"persisted\":%s,\"map_location\":{\"set\":%s,\"lat\":",
           bool_json(persisted), bool_json(settings && settings->map_location_set));
    print_e7_json(settings && settings->map_location_set ? settings->map_lat_e7 : 0);
    printf(",\"lon\":");
    print_e7_json(settings && settings->map_location_set ? settings->map_lon_e7 : 0);
    printf(",\"lat_e7\":%ld,\"lon_e7\":%ld,\"source\":\"%s\"}}\n",
           (long)(settings && settings->map_location_set ? settings->map_lat_e7 : 0),
           (long)(settings && settings->map_location_set ? settings->map_lon_e7 : 0),
           map_location_source_name(settings));
}

static void persist_map_location_from_args(const char *cmd, const char *arg)
{
    while (*arg == ' ') {
        arg++;
    }
    int32_t lat_e7 = 0;
    int32_t lon_e7 = 0;
    const char *end = NULL;
    if (!parse_coord_e7(arg, &end, D1L_MAP_LOCATION_LAT_E7_MIN,
                        D1L_MAP_LOCATION_LAT_E7_MAX, &lat_e7)) {
        err_result(cmd, "INVALID_LOCATION",
                   "usage: map center set <lat -90..90> <lon -180..180>");
        return;
    }
    while (end && *end == ' ') {
        end++;
    }
    if (!parse_coord_e7(end, &end, D1L_MAP_LOCATION_LON_E7_MIN,
                        D1L_MAP_LOCATION_LON_E7_MAX, &lon_e7)) {
        err_result(cmd, "INVALID_LOCATION",
                   "usage: map center set <lat -90..90> <lon -180..180>");
        return;
    }
    while (end && *end == ' ') {
        end++;
    }
    if (end && *end != '\0') {
        err_result(cmd, "INVALID_LOCATION",
                   "usage: map center set <lat -90..90> <lon -180..180>");
        return;
    }

    esp_err_t ret = d1l_app_model_set_map_location(lat_e7, lon_e7);
    if (ret != ESP_OK) {
        err_result(cmd, esp_err_to_name(ret), "could not persist map location");
        return;
    }
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    print_map_location_result(cmd, &settings, true);
}

static void clear_map_location(const char *cmd)
{
    esp_err_t ret = d1l_app_model_clear_map_location();
    if (ret != ESP_OK) {
        err_result(cmd, esp_err_to_name(ret), "could not clear map location");
        return;
    }
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    print_map_location_result(cmd, &settings, true);
}

static void cmd_settings_set_location(const char *line)
{
    persist_map_location_from_args("settings set location",
                                   line + strlen("settings set location "));
}

static void cmd_settings_clear_location(void)
{
    clear_map_location("settings clear location");
}

static void cmd_map_center(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_LOCATION)) {
        release_unavailable_status(
            "map center", D1L_RELEASE_FEATURE_LOCATION);
        return;
    }
    d1l_settings_t settings = {0};
    (void)d1l_settings_public_snapshot(&settings);
    print_map_location_result("map center", &settings, false);
}

static void cmd_map_center_set(const char *line)
{
    persist_map_location_from_args("map center set",
                                   line + strlen("map center set "));
}

static void cmd_map_center_clear(void)
{
    clear_map_location("map center clear");
}

static void cmd_ui_status(void)
{
    ok_begin("ui status");
    print_release_profile_fields();
    printf(",\"started\":%s,\"active_tab\":",
           bool_json(d1l_ui_phase1_started()));
    print_json_string(d1l_ui_phase1_active_tab_name());
    printf(",\"pending\":%s,\"pending_tab\":",
           bool_json(d1l_ui_phase1_tab_switch_pending()));
    print_json_string(d1l_ui_phase1_pending_tab_name());
    printf("}\n");
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void print_ui_capture_status_fields(const d1l_ui_capture_status_t *status)
{
    const d1l_ui_capture_status_t empty = {
        .width = D1L_UI_CAPTURE_WIDTH,
        .height = D1L_UI_CAPTURE_HEIGHT,
        .bytes_per_pixel = D1L_UI_CAPTURE_BYTES_PER_PIXEL,
        .total_bytes = D1L_UI_CAPTURE_TOTAL_BYTES,
        .max_chunk_bytes = D1L_UI_CAPTURE_MAX_CHUNK_BYTES,
    };
    if (!status) {
        status = &empty;
    }
    printf(",\"available\":%s,\"active\":%s,\"shadow_ready\":%s,\"started\":%s",
           bool_json(status->available), bool_json(status->active),
           bool_json(status->shadow_ready), bool_json(status->started));
    printf(",\"width\":%u,\"height\":%u,\"bytes_per_pixel\":%u,\"pixel_format\":\"rgb565-le\"",
           (unsigned)status->width,
           (unsigned)status->height,
           (unsigned)status->bytes_per_pixel);
    printf(",\"total_bytes\":%lu,\"max_chunk_bytes\":%lu",
           (unsigned long)status->total_bytes,
           (unsigned long)status->max_chunk_bytes);
    printf(",\"frame_seq\":%lu,\"flush_count\":%lu,\"crc32\":\"%08lX\"",
           (unsigned long)status->frame_seq,
           (unsigned long)status->flush_count,
           (unsigned long)status->capture_crc32);
    printf(",\"active_tab\":");
    print_json_string(status->active_tab);
    printf(",\"pending_tab\":");
    print_json_string(status->pending_tab);
    printf(",\"tab_pending\":%s,\"content_pending\":%s,\"render_pending\":%s",
           bool_json(status->tab_pending),
           bool_json(status->content_pending),
           bool_json(status->render_pending));
    printf(",\"onboarding_visible\":%s,\"lock_visible\":%s",
           bool_json(status->onboarding_visible),
           bool_json(status->lock_visible));
    printf(",\"public_rf_tx\":false,\"formats_sd\":false");
}

static void cmd_ui_capture_status(void)
{
    d1l_ui_capture_status_t status = {0};
    esp_err_t ret = d1l_ui_capture_status(&status);
    if (ret != ESP_OK) {
        err_result("ui capture status", esp_err_to_name(ret),
                   "UI capture status is unavailable");
        return;
    }
    ok_begin("ui capture status");
    print_ui_capture_status_fields(&status);
    printf("}\n");
}

static void cmd_ui_capture_begin(void)
{
    d1l_ui_capture_status_t status = {0};
    esp_err_t ret = d1l_ui_capture_begin(&status);
    if (ret != ESP_OK) {
        err_result("ui capture begin", esp_err_to_name(ret),
                   "UI capture requires a started display with at least one flushed frame");
        return;
    }
    ok_begin("ui capture begin");
    print_ui_capture_status_fields(&status);
    printf("}\n");
}

static bool parse_ui_capture_chunk_args(const char *line, size_t *out_offset, size_t *out_len)
{
    static const char prefix[] = "ui capture chunk ";
    if (strncmp(line, prefix, strlen(prefix)) != 0 || !out_offset || !out_len) {
        return false;
    }
    const char *cursor = line + strlen(prefix);
    char *end = NULL;
    unsigned long offset = strtoul(cursor, &end, 10);
    if (end == cursor || !isspace((unsigned char)*end)) {
        return false;
    }
    while (*end && isspace((unsigned char)*end)) {
        end++;
    }
    cursor = end;
    unsigned long len = strtoul(cursor, &end, 10);
    if (end == cursor) {
        return false;
    }
    while (*end && isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *out_offset = (size_t)offset;
    *out_len = (size_t)len;
    return true;
}

static void cmd_ui_capture_chunk(const char *line)
{
    size_t offset = 0;
    size_t requested_len = 0;
    if (!parse_ui_capture_chunk_args(line, &offset, &requested_len)) {
        err_result("ui capture chunk", "INVALID_ARG",
                   "usage: ui capture chunk <offset> <len>");
        return;
    }
    uint8_t bytes[D1L_UI_CAPTURE_MAX_CHUNK_BYTES];
    size_t len = 0;
    uint32_t chunk_crc32 = 0;
    d1l_ui_capture_status_t status = {0};
    esp_err_t ret = d1l_ui_capture_chunk(offset, requested_len, bytes, sizeof(bytes),
                                         &len, &chunk_crc32, &status);
    if (ret != ESP_OK) {
        err_result("ui capture chunk", esp_err_to_name(ret),
                   "call ui capture begin first, then request chunks inside total_bytes");
        return;
    }
    ok_begin("ui capture chunk");
    print_ui_capture_status_fields(&status);
    printf(",\"offset\":%lu,\"requested_len\":%lu,\"len\":%lu,\"chunk_crc32\":\"%08lX\",\"data_hex\":",
           (unsigned long)offset,
           (unsigned long)requested_len,
           (unsigned long)len,
           (unsigned long)chunk_crc32);
    print_hex_bytes_json(bytes, len);
    printf("}\n");
}

static void cmd_ui_capture_end(void)
{
    d1l_ui_capture_status_t status = {0};
    esp_err_t ret = d1l_ui_capture_end(&status);
    if (ret != ESP_OK) {
        err_result("ui capture end", esp_err_to_name(ret),
                   "UI capture was not initialized");
        return;
    }
    ok_begin("ui capture end");
    print_ui_capture_status_fields(&status);
    printf("}\n");
}
#endif

static void cmd_ui_tab(const char *line)
{
    const char *arg = line + strlen("ui tab ");
    while (*arg == ' ') {
        arg++;
    }
    char tab[16] = {0};
    size_t len = 0;
    while (arg[len] != '\0' && !isspace((unsigned char)arg[len]) && len + 1U < sizeof(tab)) {
        tab[len] = (char)tolower((unsigned char)arg[len]);
        len++;
    }
    if (len == 0 || (arg[len] != '\0' && !isspace((unsigned char)arg[len]))) {
        err_result("ui tab", "INVALID_TAB",
                   "usage: ui tab <home|messages|nodes|map|packets|settings>");
        return;
    }
    esp_err_t ret = d1l_ui_phase1_request_tab(tab);
    if (ret != ESP_OK) {
        err_result("ui tab", esp_err_to_name(ret),
                   "usage: ui tab <home|messages|nodes|map|packets|settings>");
        return;
    }
    ok_begin("ui tab");
    printf(",\"requested_tab\":");
    print_json_string(tab);
    printf(",\"pending\":true,\"active_tab\":");
    print_json_string(d1l_ui_phase1_active_tab_name());
    printf("}\n");
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void cmd_ui_scroll_probe(const char *line)
{
    const char *arg = line + strlen("ui scroll-probe ");
    while (*arg == ' ') {
        arg++;
    }
    char surface[24] = {0};
    size_t len = 0;
    while (arg[len] != '\0' && !isspace((unsigned char)arg[len]) &&
           len + 1U < sizeof(surface)) {
        surface[len] = (char)tolower((unsigned char)arg[len]);
        len++;
    }
    if (len == 0 || (arg[len] != '\0' && !isspace((unsigned char)arg[len]))) {
        err_result("ui scroll-probe", "INVALID_SURFACE",
                   "usage: ui scroll-probe <home|public_messages|dm_thread|nodes|contact_detail|contact_options|contact_forget|contact_route|mesh_roles|mesh_rooms|mesh_repeaters|packets|settings|storage|storage_card|storage_data|wifi|map|map_options|map_location|map_cache>");
        return;
    }

    d1l_ui_scroll_probe_result_t probe = {0};
    esp_err_t ret = d1l_ui_phase1_scroll_probe(surface, &probe);
    if (ret != ESP_OK) {
        err_result("ui scroll-probe", esp_err_to_name(ret),
                   "usage: ui scroll-probe <home|public_messages|dm_thread|nodes|contact_detail|contact_options|contact_forget|contact_route|mesh_roles|mesh_rooms|mesh_repeaters|packets|settings|storage|storage_card|storage_data|wifi|map|map_options|map_location|map_cache>");
        return;
    }

    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":\"ui scroll-probe\",\"surface\":",
           D1L_CONSOLE_SCHEMA, bool_json(probe.ok));
    print_json_string(probe.surface);
    printf(",\"tab\":");
    print_json_string(probe.tab);
    printf(",\"surface_supported\":%s,\"target_found\":%s,\"scrollable\":%s,"
           "\"movement_required\":%s,\"moved\":%s",
           bool_json(probe.surface_supported), bool_json(probe.target_found),
           bool_json(probe.scrollable), bool_json(probe.movement_required),
           bool_json(probe.moved));
    printf(",\"before_y\":%ld,\"after_y\":%ld",
           (long)probe.before_y, (long)probe.after_y);
    printf(",\"scroll_top_before\":%ld,\"scroll_bottom_before\":%ld",
           (long)probe.scroll_top_before, (long)probe.scroll_bottom_before);
    printf(",\"scroll_top_after\":%ld,\"scroll_bottom_after\":%ld}\n",
           (long)probe.scroll_top_after, (long)probe.scroll_bottom_after);
}

static void cmd_ui_compose_probe(const char *line)
{
    const char *arg = line + strlen("ui compose-probe ");
    while (*arg == ' ') {
        arg++;
    }
    char target[16] = {0};
    size_t len = 0;
    while (arg[len] != '\0' && !isspace((unsigned char)arg[len]) &&
           len + 1U < sizeof(target)) {
        target[len] = (char)tolower((unsigned char)arg[len]);
        if (target[len] == '-') {
            target[len] = '_';
        }
        len++;
    }
    if (len == 0 || (arg[len] != '\0' && !isspace((unsigned char)arg[len]))) {
        err_result("ui compose-probe", "INVALID_TARGET",
                   "usage: ui compose-probe <public|public-long|dm|dm-long|public-search|dm-search|packet-search|contact-edit|onboarding|map-location|wifi-ssid|wifi-password>");
        return;
    }

    d1l_ui_compose_probe_result_t probe = {0};
    esp_err_t ret = d1l_ui_phase1_compose_probe(target, &probe);
    if (ret != ESP_OK) {
        err_result("ui compose-probe", esp_err_to_name(ret),
                   "usage: ui compose-probe <public|public-long|dm|dm-long|public-search|dm-search|packet-search|contact-edit|onboarding|map-location|wifi-ssid|wifi-password>");
        return;
    }

    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":\"ui compose-probe\",\"target\":",
           D1L_CONSOLE_SCHEMA, bool_json(probe.ok));
    print_json_string(probe.target);
    printf(",\"target_supported\":%s,\"sheet_visible\":%s,\"textarea_visible\":%s,"
           "\"keyboard_visible\":%s,\"onboarding_visible\":%s,"
           "\"dock_hidden\":%s,\"dm_mode\":%s,\"tx_suppressed\":%s,"
           "\"send_enabled\":%s,"
           "\"active_tab\":",
           bool_json(probe.target_supported),
           bool_json(probe.sheet_visible),
           bool_json(probe.textarea_visible),
           bool_json(probe.keyboard_visible),
           bool_json(probe.onboarding_visible),
           bool_json(probe.dock_hidden),
           bool_json(probe.dm_mode),
           bool_json(probe.tx_suppressed),
           bool_json(probe.send_enabled));
    print_json_string(probe.active_tab);
    printf(",\"sheet\":{\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld},"
           "\"textarea\":{\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld},"
           "\"keyboard\":{\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld},"
           "\"public_rf_tx\":false,\"formats_sd\":false}\n",
           (long)probe.sheet_x, (long)probe.sheet_y,
           (long)probe.sheet_w, (long)probe.sheet_h,
           (long)probe.textarea_x, (long)probe.textarea_y,
           (long)probe.textarea_w, (long)probe.textarea_h,
           (long)probe.keyboard_x, (long)probe.keyboard_y,
           (long)probe.keyboard_w, (long)probe.keyboard_h);
}
#endif

static void cmd_identity_status(void)
{
    esp_err_t ret = d1l_meshcore_service_ensure_identity();
    if (ret != ESP_OK) {
        err_result("identity status", esp_err_to_name(ret), "could not generate or persist MeshCore local identity");
        return;
    }
    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    char fingerprint[17] = {0};
    hex_prefix(fingerprint, sizeof(fingerprint), settings->identity_public_key, 8U);
    ok_begin("identity status");
    printf(",\"node_name\":\"%s\",\"role\":\"%s\",\"meshcore_local_identity\":\"stored_nvs_ed25519\",\"public_key_ready\":%s,\"public_key\":",
           settings->node_name, d1l_settings_role_name(settings->role),
           bool_json(settings->identity_ready));
    print_hex_bytes_json(settings->identity_public_key,
                         sizeof(settings->identity_public_key));
    printf(",\"fingerprint\":\"%s\"}\n", fingerprint);
}

static void cmd_i2c(void)
{
    d1l_board_status_t scan = *d1l_board_status();
    esp_err_t ret = d1l_board_i2c_scan(&scan);
    if (ret != ESP_OK) {
        err_result("i2c", esp_err_to_name(ret), "I2C scan failed; verify SDA GPIO39, SCL GPIO40, and D1L power");
        return;
    }
    ok_begin("i2c");
    printf(",\"count\":%u,\"addresses\":[", scan.i2c_count);
    for (uint8_t i = 0; i < scan.i2c_count; ++i) {
        printf("%s\"0x%02X\"", i ? "," : "", scan.i2c_addresses[i]);
    }
    printf("],\"expected\":[\"0x20\",\"0x48\"]}\n");
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void cmd_display_test(void)
{
    esp_err_t ret = d1l_board_display_color_test();
    if (ret != ESP_OK) {
        err_result("display test", esp_err_to_name(ret), "display test requires initialized D1L RGB panel");
        return;
    }
    ok_begin("display test");
    printf(",\"pattern\":\"rgb_white_black_yellow_bars\",\"manual_confirm\":true}\n");
}

static void cmd_touch_test(void)
{
    d1l_board_touch_state_t touch = {0};
    esp_err_t ret = d1l_board_touch_read(&touch);
    if (ret != ESP_OK) {
        printf("{\"schema\":%d,\"ok\":false,\"cmd\":\"touch test\",\"code\":\"%s\","
               "\"init_result\":\"%s\",\"read_result\":\"%s\",\"init_attempts\":%lu,"
               "\"hint\":\"touch sample requires initialized D1L touch controller\"}\n",
               D1L_CONSOLE_SCHEMA, esp_err_to_name(ret),
               esp_err_to_name(touch.init_result), esp_err_to_name(touch.read_result),
               (unsigned long)touch.init_attempts);
        return;
    }
    ok_begin("touch test");
    printf(",\"touches\":%u,\"pressed\":%s,\"x\":%u,\"y\":%u,\"raw_x\":%ld,\"raw_y\":%ld,"
           "\"init_result\":\"%s\",\"read_result\":\"%s\",\"init_attempts\":%lu,"
           "\"coordinate_valid\":%s,\"controller\":\"FT5x06\",\"i2c_address\":\"0x48\","
           "\"manual_confirm\":true}\n",
           touch.touches, bool_json(touch.pressed), touch.x, touch.y,
           (long)touch.raw_x, (long)touch.raw_y,
           esp_err_to_name(touch.init_result), esp_err_to_name(touch.read_result),
           (unsigned long)touch.init_attempts,
           bool_json(touch.coordinate_valid));
}
#endif

static void cmd_touch_raw(void)
{
    d1l_board_touch_raw_state_t raw = {0};
    esp_err_t ret = d1l_board_touch_raw_read(&raw);
    if (ret != ESP_OK) {
        printf("{\"schema\":%d,\"ok\":false,\"cmd\":\"touch raw\",\"code\":\"%s\","
               "\"init_result\":\"%s\",\"read_result\":\"%s\",\"init_attempts\":%lu,"
               "\"hint\":\"raw FT5x06 register read failed\"}\n",
               D1L_CONSOLE_SCHEMA, esp_err_to_name(ret),
               esp_err_to_name(raw.init_result), esp_err_to_name(raw.read_result),
               (unsigned long)raw.init_attempts);
        return;
    }
    ok_begin("touch raw");
    printf(",\"init_result\":\"%s\",\"read_result\":\"%s\",\"init_attempts\":%lu,"
           "\"controller\":\"FT5x06\",\"i2c_address\":\"0x48\","
           "\"touch_points_raw\":%u,\"touch_count\":%u,\"event_flag\":%u,"
           "\"touch_id\":%u,\"raw_x\":%u,\"raw_y\":%u",
           esp_err_to_name(raw.init_result), esp_err_to_name(raw.read_result),
           (unsigned long)raw.init_attempts, raw.touch_points_raw, raw.touch_count,
           raw.event_flag, raw.touch_id, raw.raw_x, raw.raw_y);
    print_hex_field("registers_00_1f", raw.registers_00_1f, sizeof(raw.registers_00_1f));
    print_hex_field("config_80_89", raw.config_80_89, sizeof(raw.config_80_89));
    print_hex_field("id_a1_a9", raw.id_a1_a9, sizeof(raw.id_a1_a9));
    printf("}\n");
}

static void cmd_button(void)
{
    ok_begin("button");
    printf(",\"pressed\":%s,\"user_gpio\":38,\"active_level\":0,\"board_ready\":%s}\n",
           d1l_board_button_pressed() ? "true" : "false",
           d1l_board_status()->ready ? "true" : "false");
}

static void cmd_backlight(const char *line)
{
    const char *arg = line + strlen("backlight");
    while (*arg == ' ') {
        arg++;
    }
    int percent = 0;
    if (!d1l_usb_command_parse_int_exact(arg, &percent)) {
        err_result("backlight", "INVALID_ARGUMENT", "usage: backlight <0-100>");
        return;
    }
    esp_err_t ret = d1l_backlight_set_percent(percent);
    if (ret != ESP_OK) {
        err_result("backlight", esp_err_to_name(ret), "usage: backlight <0-100>");
        return;
    }
    ok_begin("backlight");
    printf(",\"percent\":%d}\n", percent);
}

static void cmd_radiohw(void)
{
    d1l_radiohw_status_t status;
    esp_err_t ret = d1l_sx1262_probe(&status);
    if (ret != ESP_OK) {
        printf("{\"schema\":%d,\"ok\":false,\"cmd\":\"radiohw\",\"code\":\"%s\",\"sx1262\":{\"present\":false,\"expander_ready\":%s,\"busy\":%d,\"dio1\":%d,\"ver_pin\":%d,\"tcxo_default\":\"NONE\"}}\n",
               D1L_CONSOLE_SCHEMA, status.failure_code ? status.failure_code : esp_err_to_name(ret),
               status.expander_ready ? "true" : "false", status.busy, status.dio1, status.ver_pin);
        return;
    }
    ok_begin("radiohw");
    printf(",\"sx1262\":{\"present\":%s,\"status_byte\":%u,\"busy\":%d,\"dio1\":%d,\"ver_pin\":%d,\"tcxo_default\":\"NONE\"}}\n",
           status.present ? "true" : "false", status.status_byte, status.busy, status.dio1, status.ver_pin);
}

static void print_radio_profile_result(const char *cmd)
{
    d1l_radio_profile_t profile = d1l_settings_radio_profile(NULL);
    d1l_meshcore_service_status_t mesh = d1l_meshcore_service_status();
    ok_begin(cmd);
    printf(",\"profile_id\":\"%s\",\"region\":\"%s\",\"frequency_hz\":%lu,\"bandwidth_khz\":%.1f,\"sf\":%u,\"cr\":%u,\"tx_power_dbm\":%d,\"tcxo\":\"%s\",\"rx_boost\":%s,\"persisted\":true,\"applied_to_radio\":%s,\"radio_apply_pending\":%s,\"radio_apply_error\":\"%s\"}\n",
           profile.profile_id, profile.region_label, (unsigned long)profile.frequency_hz,
           profile.bandwidth_khz, profile.spreading_factor, profile.coding_rate,
           profile.tx_power_dbm, profile.tcxo, bool_json(profile.rx_boost),
           bool_json(mesh.radio_applied), bool_json(mesh.radio_apply_pending),
           esp_err_to_name(mesh.radio_apply_error));
}

static void cmd_radio_get(void)
{
    print_radio_profile_result("radio get");
}

static void cmd_radio_set_preset_uscan(void)
{
    d1l_settings_t settings = {0};
    const d1l_radio_profile_t *defaults = d1l_radio_profile_uscan_default();
    settings.frequency_hz = defaults->frequency_hz;
    settings.bandwidth_tenths_khz = (uint16_t)((defaults->bandwidth_khz * 10.0f) + 0.5f);
    settings.spreading_factor = defaults->spreading_factor;
    settings.coding_rate = defaults->coding_rate;
    settings.tx_power_dbm = defaults->tx_power_dbm;
    settings.rx_boost = defaults->rx_boost;
    settings.tcxo_mode = D1L_TCXO_NONE;
    esp_err_t ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_RADIO_PROFILE);
    if (ret != ESP_OK) {
        err_result("radio set preset uscan", esp_err_to_name(ret), "could not persist Canada/USA radio defaults");
        return;
    }
    print_radio_profile_result("radio set preset uscan");
}

static void cmd_radio_set_freq(const char *line)
{
    double mhz = 0.0;
    if (!d1l_usb_command_parse_double_exact(
            line + strlen("radio set freq "), &mhz) ||
        mhz < 902.0 || mhz > 928.0) {
        err_result("radio set freq", "INVALID_FREQ", "Canada/USA D1L range is 902.000-928.000 MHz");
        return;
    }
    d1l_settings_t settings = {0};
    settings.frequency_hz = (uint32_t)((mhz * 1000000.0) + 0.5);
    esp_err_t ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_FREQUENCY);
    if (ret != ESP_OK) {
        err_result("radio set freq", esp_err_to_name(ret), "could not persist frequency");
        return;
    }
    print_radio_profile_result("radio set freq");
}

static void cmd_radio_set_bw(const char *line)
{
    double khz = 0.0;
    if (!d1l_usb_command_parse_double_exact(
            line + strlen("radio set bw "), &khz) ||
        khz < 7.8 || khz > 500.0) {
        err_result("radio set bw", "INVALID_BW", "usage: radio set bw <7.8-500.0>");
        return;
    }
    d1l_settings_t settings = {0};
    settings.bandwidth_tenths_khz = (uint16_t)((khz * 10.0) + 0.5);
    esp_err_t ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_BANDWIDTH);
    if (ret != ESP_OK) {
        err_result("radio set bw", esp_err_to_name(ret), "could not persist bandwidth");
        return;
    }
    print_radio_profile_result("radio set bw");
}

static void cmd_radio_set_sf(const char *line)
{
    int sf = 0;
    if (!d1l_usb_command_parse_int_exact(
            line + strlen("radio set sf "), &sf) ||
        sf < 5 || sf > 12) {
        err_result("radio set sf", "INVALID_SF", "usage: radio set sf <5-12>");
        return;
    }
    d1l_settings_t settings = {0};
    settings.spreading_factor = (uint8_t)sf;
    esp_err_t ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_SPREADING);
    if (ret != ESP_OK) {
        err_result("radio set sf", esp_err_to_name(ret), "could not persist spreading factor");
        return;
    }
    print_radio_profile_result("radio set sf");
}

static void cmd_radio_set_cr(const char *line)
{
    int cr = 0;
    if (!d1l_usb_command_parse_int_exact(
            line + strlen("radio set cr "), &cr) ||
        cr < 5 || cr > 8) {
        err_result("radio set cr", "INVALID_CR", "usage: radio set cr <5-8>");
        return;
    }
    d1l_settings_t settings = {0};
    settings.coding_rate = (uint8_t)cr;
    esp_err_t ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_CODING_RATE);
    if (ret != ESP_OK) {
        err_result("radio set cr", esp_err_to_name(ret), "could not persist coding rate");
        return;
    }
    print_radio_profile_result("radio set cr");
}

static void cmd_radio_set_txpower(const char *line)
{
    int tx_power = 0;
    if (!d1l_usb_command_parse_int_exact(
            line + strlen("radio set txpower "), &tx_power) ||
        tx_power < -9 || tx_power > D1L_RADIO_TX_POWER_DBM) {
        err_result("radio set txpower", "INVALID_TX_POWER", "usage: radio set txpower <-9-20>");
        return;
    }
    d1l_settings_t settings = {0};
    settings.tx_power_dbm = (int8_t)tx_power;
    esp_err_t ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_TX_POWER);
    if (ret != ESP_OK) {
        err_result("radio set txpower", esp_err_to_name(ret), "could not persist TX power");
        return;
    }
    print_radio_profile_result("radio set txpower");
}

static void cmd_radio_set_rxboost(const char *line)
{
    const char *arg = line + strlen("radio set rxboost ");
    while (*arg == ' ') {
        arg++;
    }
    bool enabled = false;
    if (strcmp(arg, "1") == 0 || strcmp(arg, "on") == 0 || strcmp(arg, "true") == 0) {
        enabled = true;
    } else if (strcmp(arg, "0") == 0 || strcmp(arg, "off") == 0 || strcmp(arg, "false") == 0) {
        enabled = false;
    } else {
        err_result("radio set rxboost", "INVALID_RX_BOOST", "usage: radio set rxboost <0|1>");
        return;
    }
    d1l_settings_t settings = {0};
    settings.rx_boost = enabled;
    esp_err_t ret = d1l_settings_update_fields(
        &settings, D1L_SETTINGS_UPDATE_RX_BOOST);
    if (ret != ESP_OK) {
        err_result("radio set rxboost", esp_err_to_name(ret), "could not persist RX boost");
        return;
    }
    print_radio_profile_result("radio set rxboost");
}

static void cmd_mesh_status(void)
{
    d1l_meshcore_service_status_t status = d1l_meshcore_service_status();
    const bool user_trace_available =
        d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE);
    ok_begin("mesh status");
    print_release_profile_fields();
    printf(",\"phase\":\"phase2_public_rf\",\"state\":\"%s\",\"radio_profile\":\"uscan-meshcore-default\",\"identity_ready\":%s,\"radio_ready\":%s,\"companion_framing_ready\":%s,\"path_hash_bytes\":%u,\"rx_packets\":%lu,\"rx_adverts\":%lu,\"tx_packets\":%lu,\"rejected_commands\":%lu,\"ack_tx\":{\"queued\":%lu,\"done\":%lu,\"failed\":%lu,\"duplicate_rows_suppressed\":%lu,\"last_hash\":%lu,\"last_error\":\"%s\"},\"dm_route\":{\"direct_selected\":%lu,\"flood_selected\":%lu,\"missing_fallback\":%lu,\"preboot_fallback\":%lu,\"stale_fallback\":%lu,\"malformed_fallback\":%lu,\"expired_fallback\":%lu,\"failed_fallback\":%lu,\"direct_retry_fallback\":%lu,\"last_reason\":\"%s\",\"last_path_age_ms\":%lu}",
           d1l_meshcore_service_state_name(status.state), bool_json(status.identity_ready),
           bool_json(status.radio_ready), bool_json(status.companion_framing_ready),
           status.path_hash_bytes, (unsigned long)status.rx_packets,
           (unsigned long)status.rx_adverts, (unsigned long)status.tx_packets,
           (unsigned long)status.rejected_commands,
           (unsigned long)status.ack_tx_queued,
           (unsigned long)status.ack_tx_done,
           (unsigned long)status.ack_tx_failed,
           (unsigned long)status.ack_tx_duplicate_rows_suppressed,
           (unsigned long)status.ack_tx_last_hash,
           esp_err_to_name(status.ack_tx_last_error),
           (unsigned long)status.dm_route_direct_selected,
           (unsigned long)status.dm_route_flood_selected,
           (unsigned long)status.dm_route_missing_fallback,
           (unsigned long)status.dm_route_preboot_fallback,
           (unsigned long)status.dm_route_stale_fallback,
           (unsigned long)status.dm_route_malformed_fallback,
           (unsigned long)status.dm_route_expired_fallback,
           (unsigned long)status.dm_route_failed_fallback,
           (unsigned long)status.dm_route_direct_retry_fallback,
           d1l_meshcore_route_selection_reason_name(
               (d1l_meshcore_route_selection_reason_t)status.dm_route_last_reason),
           (unsigned long)status.dm_route_last_path_age_ms);
    printf(
        ",\"public_tx_history\":{\"pending\":%s,\"retries\":%lu,"
        "\"failures\":%lu}",
        bool_json(status.public_tx_history_pending),
        (unsigned long)status.public_tx_history_retries,
        (unsigned long)status.public_tx_history_failures);
    printf(
        ",\"advert_tx\":{\"queued\":%lu,\"done\":%lu,\"failed\":%lu,"
        "\"boot_queued\":%lu,\"boot_done\":%lu,\"boot_failed\":%lu,"
        "\"last_boot\":%s,\"last_flood\":%s,\"last_node_name\":",
        (unsigned long)status.advert_tx_queued,
        (unsigned long)status.advert_tx_done,
        (unsigned long)status.advert_tx_failed,
        (unsigned long)status.boot_advert_tx_queued,
        (unsigned long)status.boot_advert_tx_done,
        (unsigned long)status.boot_advert_tx_failed,
        bool_json(status.last_advert_boot),
        bool_json(status.last_advert_flood));
    print_json_string(status.last_advert_node_name);
    printf(",\"last_public_key_prefix\":");
    print_json_string(status.last_advert_public_key_prefix);
    printf(",\"boot_flood\":%s,\"boot_node_name\":",
           bool_json(status.boot_advert_flood));
    print_json_string(status.boot_advert_node_name);
    printf(",\"boot_public_key_prefix\":");
    print_json_string(status.boot_advert_public_key_prefix);
    printf("}");
    printf(",\"runtime\":{\"owner\":\"meshcore_service\",\"callback_boundary\":\"copy_timestamp_enqueue\",\"command_queue_depth\":%lu,\"command_queue_high_water\":%lu,\"priority_queue_depth\":%lu,\"priority_queue_high_water\":%lu,\"event_queue_depth\":%lu,\"event_queue_high_water\":%lu,\"queue_drops\":%lu,\"callback_event_drops\":%lu,\"command_queue_saturation\":%lu,\"priority_queue_saturation\":%lu,\"fairness_forced_commands\":%lu,\"priority_burst_high_water\":%lu,\"priority_burst_bound\":%u,\"owner_maintenance_runs\":%lu,\"terminal_recovery_dispatches\":%lu,\"heartbeat\":%lu,\"stack_free_words\":%lu,\"last_event_monotonic_us\":%llu}",
           (unsigned long)status.runtime_command_queue_depth,
           (unsigned long)status.runtime_command_queue_high_water,
           (unsigned long)status.runtime_priority_queue_depth,
           (unsigned long)status.runtime_priority_queue_high_water,
           (unsigned long)status.runtime_event_queue_depth,
           (unsigned long)status.runtime_event_queue_high_water,
           (unsigned long)status.runtime_queue_drops,
           (unsigned long)status.runtime_callback_event_drops,
           (unsigned long)status.runtime_command_queue_saturation,
           (unsigned long)status.runtime_priority_queue_saturation,
           (unsigned long)status.runtime_fairness_forced_commands,
           (unsigned long)status.runtime_priority_burst_high_water,
           (unsigned)status.runtime_priority_burst_bound,
           (unsigned long)status.runtime_owner_maintenance_runs,
           (unsigned long)status.runtime_terminal_recovery_dispatches,
           (unsigned long)status.runtime_task_heartbeat,
           (unsigned long)status.runtime_task_stack_free_words,
           (unsigned long long)status.runtime_last_event_monotonic_us);
    printf(",\"trace\":{\"available\":%s,\"internal_dm_route_only\":%s,"
           "\"tx_queued\":%lu,\"rx_matched\":%lu,\"rx_duplicates\":%lu,"
           "\"pending_expired\":%lu,\"no_response\":%lu,"
           "\"rx_expired\":%lu,\"rx_unmatched\":%lu,"
           "\"rx_correlation_code_mismatch\":%lu,"
           "\"rx_path_mismatch\":%lu,\"rx_malformed\":%lu,"
           "\"rx_source_ignored\":%lu,\"rx_in_flight_ignored\":%lu,"
           "\"rx_unsupported\":%lu,\"timeout_ms\":%u,\"cooldown_ms\":%u,"
           "\"flags_zero_direct_only\":false,"
           "\"trace_flags_supported\":%s,"
           "\"trace_wire_hash_bytes_supported\":%s,"
           "\"requires_current_boot_proven_contact_path\":%s,"
           "\"contact_trace_supported\":%s,"
           "\"operator_path_accepted\":false,\"one_byte_hash_only\":false,"
           "\"contact_trace_path_hash_bytes_supported\":%s,"
           "\"contact_route_hash_bytes_accepted\":[1,2,3],"
           "\"contact_route_hash_bytes_normalized_to_two\":[3],"
           "\"contact_route_hash_bytes_rejected\":%s,"
           "\"hardware_verified\":false},\"note\":\"%s\"}\n",
           bool_json(user_trace_available),
           bool_json(!user_trace_available),
           (unsigned long)status.trace_tx_queued,
           (unsigned long)status.trace_rx_matched,
           (unsigned long)status.trace_rx_duplicates,
           (unsigned long)status.trace_pending_expired,
           (unsigned long)status.trace_pending_expired,
           (unsigned long)status.trace_rx_expired,
           (unsigned long)status.trace_rx_unmatched,
           (unsigned long)status.trace_rx_auth_mismatch,
           (unsigned long)status.trace_rx_path_mismatch,
           (unsigned long)status.trace_rx_malformed,
           (unsigned long)status.trace_rx_source_ignored,
           (unsigned long)status.trace_rx_in_flight_ignored,
           (unsigned long)status.trace_rx_unsupported,
           D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS,
           D1L_MESHCORE_TRACE_COOLDOWN_MS,
           user_trace_available ? "[0,1,2,3]" : "[]",
           user_trace_available ? "[1,2,4,8]" : "[]",
           bool_json(user_trace_available),
           bool_json(user_trace_available),
           user_trace_available ? "[1,2]" : "[]",
           "[]",
           user_trace_available ?
               "Public group text TX/RX and signed advert TX/RX enabled; "
               "inbound DM ACK dispatch and route selection enabled; TRACE is "
               "bounded to a runtime-derived canonical contact loop, monotonic "
               "no-response timeout, cooldown, terminal parsing, and correlation" :
               "Public and direct messaging, ACK/PATH handling, and internal "
               "route selection are enabled; user TRACE/PATH tooling is "
               "unavailable in Core 1.0");
}

static void cmd_mesh_advert(const char *cmd, bool flood)
{
    esp_err_t ret = d1l_meshcore_service_request_advert(flood);
    if (ret != ESP_OK) {
        err_result(cmd,
                   ret == ESP_ERR_INVALID_STATE ? "MESHCORE_TX_BUSY_OR_RADIO_NOT_READY" :
                   ret == ESP_ERR_NOT_SUPPORTED ? "UNSUPPORTED_RADIO_PROFILE" :
                   "MESHCORE_ADVERT_FAILED",
                   "verify identity, radio profile, and TX state before retrying");
        return;
    }
    ok_begin(cmd);
    printf(",\"queued\":true,\"flood\":%s}\n", bool_json(flood));
}

static void cmd_mesh_send_public(const char *line)
{
    const char *text = line + strlen("mesh send public ");
    const d1l_user_text_info_t text_info = d1l_user_text_validate(text);
    if (text_info.result != D1L_USER_TEXT_OK) {
        err_result("mesh send public",
                   text_info.result == D1L_USER_TEXT_EMPTY ? "EMPTY_MESSAGE" :
                   text_info.result == D1L_USER_TEXT_TOO_LONG ? "MESSAGE_TOO_LONG" :
                   "INVALID_MESSAGE",
                   text_info.result == D1L_USER_TEXT_TOO_LONG ?
                       "max 138 UTF-8 bytes" :
                       "message must be valid printable UTF-8");
        return;
    }
    esp_err_t ret = d1l_meshcore_service_send_public(text);
    if (ret != ESP_OK) {
        err_result("mesh send public",
                   ret == ESP_ERR_INVALID_ARG ? "INVALID_MESSAGE" :
                   ret == ESP_ERR_INVALID_SIZE ? "MESSAGE_TOO_LONG" :
                   ret == ESP_ERR_INVALID_STATE ? "MESHCORE_TX_BUSY_OR_RADIO_NOT_READY" :
                   ret == ESP_ERR_NOT_SUPPORTED ? "UNSUPPORTED_RADIO_PROFILE" : "MESHCORE_SEND_FAILED",
                   ret == ESP_ERR_INVALID_SIZE ? "max 138 UTF-8 bytes" :
                   "verify radio profile is US/CAN 910.525 BW62.5 SF7 CR5 and try again");
        return;
    }
    ok_begin("mesh send public");
    printf(",\"queued\":true,\"text\":");
    print_json_string(text);
    printf("}\n");
}

static void cmd_companion_status(void)
{
    d1l_connectivity_status_t connectivity = {0};
    d1l_connectivity_status(&connectivity);
    const bool wifi_available = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL);
    const bool ble_available =
        d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE);
    ok_begin("companion status");
    print_release_profile_fields();
    printf(",\"usb_console\":\"ready\",\"framing\":\"meshcore-3byte\","
           "\"header_bytes\":%u,\"app_to_radio\":\"%c\","
           "\"radio_to_app\":\"%c\",\"length\":\"uint16_le\","
           "\"max_payload_bytes\":%u,\"transport_codec\":\"ready\","
           "\"meshcore_bridge\":\"ready\","
           "\"wifi\":{\"available\":%s,\"setting_enabled\":%s,"
           "\"configured_setting_enabled\":%s,\"build_enabled\":%s,"
           "\"stack_active\":%s,\"state\":\"%s\"},"
           "\"ble\":{\"available\":%s,\"setting_enabled\":%s,"
           "\"configured_setting_enabled\":%s,\"build_enabled\":%s,"
           "\"stack_active\":%s,\"state\":\"%s\"},"
           "\"coexistence_policy\":\"%s\","
           "\"path_hash_bytes_supported\":[1,2,3],"
           "\"default_path_hash_bytes\":1}\n",
           D1L_COMPANION3_HEADER_SIZE, D1L_COMPANION3_APP_TO_RADIO,
           D1L_COMPANION3_RADIO_TO_APP, D1L_COMPANION3_MAX_FRAME_SIZE,
           bool_json(wifi_available),
           bool_json(wifi_available && connectivity.wifi_enabled_setting),
           bool_json(connectivity.wifi_enabled_setting),
           bool_json(connectivity.wifi_build_enabled),
           bool_json(connectivity.wifi_stack_active),
           connectivity.wifi_state,
           bool_json(ble_available),
           bool_json(
               ble_available && connectivity.ble_companion_enabled_setting),
           bool_json(connectivity.ble_companion_enabled_setting),
           bool_json(connectivity.ble_build_enabled),
           bool_json(connectivity.ble_stack_active),
           connectivity.ble_state,
           connectivity.coexistence_policy);
}

static void cmd_rp2040_status(void)
{
    d1l_rp2040_status_t status = {0};
    esp_err_t ret = d1l_rp2040_bridge_status(&status);
    ok_begin("rp2040 status");
    printf(",\"uart_ready\":%s,\"init_result\":\"%s\",\"uart_port\":%d,\"tx_gpio\":%d,\"rx_gpio\":%d,\"baud\":%d,\"reset_expander_pin\":%u,\"buffered_bytes\":%lu,\"hardware_validated\":false}\n",
           status.uart_ready ? "true" : "false", esp_err_to_name(ret), status.uart_port,
           status.tx_gpio, status.rx_gpio, status.baud_rate, status.reset_expander_pin,
           (unsigned long)status.buffered_bytes);
}

static void cmd_rp2040_set_baud(const char *line)
{
    const char *args = line + strlen("rp2040 set-baud ");
    while (*args == ' ') {
        args++;
    }
    uint32_t baud = 0;
    if (!parse_next_u32_arg(&args, &baud) || *args != '\0') {
        err_result("rp2040 set-baud", "INVALID_ARG",
                   "usage: rp2040 set-baud <9600|38400|57600|115200|230400|460800|921600|1000000>");
        return;
    }
    esp_err_t ret = d1l_rp2040_bridge_set_baud(baud);
    if (ret != ESP_OK) {
        err_result("rp2040 set-baud", esp_err_to_name(ret),
                   "could not set RP2040 UART baud; supported rates are bounded for safe probing");
        return;
    }
    d1l_rp2040_status_t status = {0};
    (void)d1l_rp2040_bridge_status(&status);
    ok_begin("rp2040 set-baud");
    printf(",\"baud\":%lu,\"uart_ready\":%s,\"public_rf_tx\":false,\"formats_sd\":false,\"sd_touched\":false}\n",
           (unsigned long)status.baud_rate,
           bool_json(status.uart_ready));
}

static void cmd_rp2040_baud_probe(const char *line)
{
    uint32_t timeout_ms = 700U;
    const char *args = line + strlen("rp2040 baud-probe");
    while (*args == ' ') {
        args++;
    }
    if (*args != '\0') {
        if (!parse_next_u32_arg(&args, &timeout_ms) || *args != '\0' ||
            timeout_ms < 100U || timeout_ms > 3000U) {
            err_result("rp2040 baud-probe", "INVALID_ARG",
                       "usage: rp2040 baud-probe [timeout_ms 100-3000]");
            return;
        }
    }

    d1l_rp2040_baud_probe_t *probe = calloc(1, sizeof(*probe));
    if (probe == NULL) {
        err_result("rp2040 baud-probe", "ESP_ERR_NO_MEM",
                   "could not allocate RP2040 baud probe report");
        return;
    }

    esp_err_t ret = d1l_rp2040_bridge_baud_probe(probe, timeout_ms);
    d1l_rp2040_status_t status = {0};
    (void)d1l_rp2040_bridge_status(&status);
    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":\"rp2040 baud-probe\",\"code\":\"%s\","
           "\"bridge_ready\":%s,\"found_deskos\":%s,\"found_stock\":%s,"
           "\"selected_baud\":%lu,\"original_baud\":%lu,\"restored_baud\":%lu,"
           "\"timeout_ms\":%lu,\"tested_count\":%lu,"
           "\"public_rf_tx\":false,\"formats_sd\":false,\"sd_touched\":false,"
           "\"results\":[",
           D1L_CONSOLE_SCHEMA,
           bool_json(ret == ESP_OK),
           esp_err_to_name(ret),
           bool_json(probe->bridge_ready),
           bool_json(probe->found_deskos),
           bool_json(probe->found_stock),
           (unsigned long)probe->selected_baud,
           (unsigned long)probe->original_baud,
           (unsigned long)status.baud_rate,
           (unsigned long)timeout_ms,
           (unsigned long)probe->tested_count);
    for (uint32_t i = 0; i < probe->tested_count && i < D1L_RP2040_BAUD_PROBE_MAX_RESULTS; ++i) {
        const d1l_rp2040_baud_probe_result_t *result = &probe->results[i];
        if (i > 0U) {
            putchar(',');
        }
        printf("{\"baud\":%lu,\"deskos_ping_ok\":%s,\"ping_code\":\"%s\","
               "\"stock_response_seen\":%s,\"stock_json_seen\":%s,"
               "\"stock_code\":\"%s\",\"stock_bytes_read\":%lu,"
               "\"stock_response_truncated\":%s,\"stock_response_hex\":",
               (unsigned long)result->baud,
               bool_json(result->deskos_ping_ok),
               esp_err_to_name(result->ping_error),
               bool_json(result->stock_response_seen),
               bool_json(result->stock_json_seen),
               esp_err_to_name(result->stock_error),
               (unsigned long)result->stock_bytes_read,
               bool_json(result->stock_response_truncated));
        print_json_string(result->stock_response_hex);
        printf(",\"stock_response_ascii\":");
        print_json_string(result->stock_response_ascii);
        putchar('}');
    }
    printf("]}\n");
    free(probe);
}

static void cmd_rp2040_reset(void)
{
    const uint32_t hold_ms = 100U;
    const uint32_t settle_ms = 8000U;
    esp_err_t ret = d1l_rp2040_bridge_reset(hold_ms, settle_ms);
    if (ret != ESP_OK) {
        err_result("rp2040 reset", esp_err_to_name(ret),
                   "could not toggle RP2040 reset through the D1L IO expander");
        return;
    }
    d1l_rp2040_status_t status = {0};
    (void)d1l_rp2040_bridge_status(&status);
    ok_begin("rp2040 reset");
    printf(",\"reset_expander_pin\":%u,\"hold_ms\":%lu,\"settle_ms\":%lu,\"uart_ready\":%s,\"public_rf_tx\":false,\"formats_sd\":false}\n",
           status.reset_expander_pin,
           (unsigned long)hold_ms,
           (unsigned long)settle_ms,
           status.uart_ready ? "true" : "false");
}

static void cmd_rp2040_double_reset(const char *line)
{
    uint32_t hold_ms = 50U;
    uint32_t gap_ms = 150U;
    uint32_t settle_ms = 1500U;
    const char *args = line + strlen("rp2040 double-reset");
    while (*args == ' ') {
        args++;
    }
    if (*args != '\0') {
        if (!parse_next_u32_arg(&args, &hold_ms) ||
            !parse_next_u32_arg(&args, &gap_ms)) {
            err_result(line, "INVALID_ARG",
                       "usage: rp2040 double-reset [hold_ms gap_ms [settle_ms]]");
            return;
        }
        if (*args != '\0' && !parse_next_u32_arg(&args, &settle_ms)) {
            err_result(line, "INVALID_ARG",
                       "usage: rp2040 double-reset [hold_ms gap_ms [settle_ms]]");
            return;
        }
        if (*args != '\0') {
            err_result(line, "INVALID_ARG",
                       "usage: rp2040 double-reset [hold_ms gap_ms [settle_ms]]");
            return;
        }
    }
    if (hold_ms < 10U || hold_ms > 250U ||
        gap_ms < 25U || gap_ms > 1000U ||
        settle_ms < 500U || settle_ms > 5000U) {
        err_result(line, "INVALID_ARG",
                   "bounds: hold 10-250 ms, gap 25-1000 ms, settle 500-5000 ms");
        return;
    }
    esp_err_t ret = d1l_rp2040_bridge_double_reset(hold_ms, gap_ms, settle_ms);
    if (ret != ESP_OK) {
        err_result(line, esp_err_to_name(ret),
                   "could not perform RP2040 double reset through the D1L IO expander");
        return;
    }
    d1l_rp2040_status_t status = {0};
    (void)d1l_rp2040_bridge_status(&status);
    ok_begin(line);
    printf(",\"reset_expander_pin\":%u,\"hold_ms\":%lu,\"gap_ms\":%lu,\"settle_ms\":%lu,\"uart_ready\":%s,\"public_rf_tx\":false,\"formats_sd\":false,\"sd_touched\":false}\n",
           status.reset_expander_pin,
           (unsigned long)hold_ms,
           (unsigned long)gap_ms,
           (unsigned long)settle_ms,
           status.uart_ready ? "true" : "false");
}

static void cmd_rp2040_ping(void)
{
    d1l_rp2040_ping_t ping = {0};
    esp_err_t ret = d1l_rp2040_bridge_ping(&ping, D1L_STORAGE_RP2040_SD_BOOT_PROBE_TIMEOUT_MS);
    if (ret == ESP_OK && ping.bridge_ready && ping.protocol_supported) {
        d1l_storage_status_note_valid_bridge_response();
    }
    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":\"rp2040 ping\",\"code\":\"%s\",\"bridge_ready\":%s,\"protocol_supported\":%s,\"response_truncated\":%s,\"version\":%lu,\"file_line_max\":%lu,\"file_chunk_max\":%lu,\"path_max\":%lu,\"atomic_rename\":%s,\"sd_touched\":%s,\"public_rf_tx\":false,\"formats_sd\":false,\"note\":",
           D1L_CONSOLE_SCHEMA,
           bool_json(ret == ESP_OK),
           esp_err_to_name(ret),
           bool_json(ping.bridge_ready),
           bool_json(ping.protocol_supported),
           bool_json(ping.response_truncated),
           (unsigned long)ping.protocol_version,
           (unsigned long)ping.file_line_max,
           (unsigned long)ping.file_chunk_max,
           (unsigned long)ping.path_max,
           bool_json(ping.atomic_rename_supported),
           bool_json(ping.sd_touched));
    print_json_string(ping.note[0] ? ping.note : "");
    printf("}\n");
}

static void cmd_rp2040_bootloader(void)
{
    esp_err_t ret = d1l_rp2040_bridge_enter_bootloader(D1L_STORAGE_RP2040_SD_BOOT_PROBE_TIMEOUT_MS);
    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":\"rp2040 bootloader\",\"code\":\"%s\",\"public_rf_tx\":false,\"formats_sd\":false,\"sd_touched\":false,\"note\":",
           D1L_CONSOLE_SCHEMA,
           bool_json(ret == ESP_OK),
           esp_err_to_name(ret));
    print_json_string(ret == ESP_OK ?
                      "RP2040 bridge accepted UF2 bootloader request" :
                      "RP2040 bridge did not accept UF2 bootloader request");
    printf("}\n");
}

static void cmd_rp2040_stock_probe(void)
{
    d1l_rp2040_stock_probe_t probe = {0};
    esp_err_t ret = d1l_rp2040_bridge_stock_probe(&probe, D1L_STORAGE_RP2040_SD_BOOT_PROBE_TIMEOUT_MS);
    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":\"rp2040 stock-probe\",\"code\":\"%s\",\"bridge_ready\":%s,\"response_seen\":%s,\"json_seen\":%s,\"response_truncated\":%s,\"bytes_read\":%lu,\"sent_cobs_hex\":",
           D1L_CONSOLE_SCHEMA,
           bool_json(ret == ESP_OK),
           esp_err_to_name(ret),
           bool_json(probe.bridge_ready),
           bool_json(probe.response_seen),
           bool_json(probe.json_seen),
           bool_json(probe.response_truncated),
           (unsigned long)probe.bytes_read);
    print_json_string(probe.sent_cobs_hex);
    printf(",\"response_hex\":");
    print_json_string(probe.response_hex);
    printf(",\"response_ascii\":");
    print_json_string(probe.response_ascii);
    printf(",\"public_rf_tx\":false,\"formats_sd\":false,\"sd_touched\":false,\"note\":");
    print_json_string(probe.note[0] ? probe.note : "");
    printf("}\n");
}

static void cmd_storage_status(void)
{
    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);
    d1l_connectivity_status_t connectivity = {0};
    d1l_connectivity_status(&connectivity);
    d1l_retained_store_worker_status_t retained_scheduler = {0};
    d1l_retained_store_worker_status(&retained_scheduler);
    d1l_retained_blob_store_nvs_telemetry_t retained_nvs_telemetry = {0};
    (void)d1l_retained_blob_store_nvs_telemetry(&retained_nvs_telemetry);
    const bool map_available =
        d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP);
    ok_begin("storage status");
    print_release_profile_fields();
    printf(",\"manager\":{\"running\":%s,\"state\":",
           bool_json(status.manager_running));
    print_json_string(status.manager_state ? status.manager_state : "BRIDGE_WAIT");
    printf(",\"attempt\":%lu,\"backoff_ms\":%lu,\"force_nvs\":%s,"
           "\"initial_ping_timeouts\":%lu,\"bridge_response_seen\":%s,"
           "\"auto_reset_pending\":%s,\"auto_reset_attempted\":%s},\"sd\":{\"state\":",
           (unsigned long)status.manager_attempt,
           (unsigned long)status.manager_backoff_ms,
           bool_json(status.force_nvs),
           (unsigned long)status.manager_initial_ping_timeouts,
           bool_json(status.manager_bridge_response_seen),
           bool_json(status.manager_auto_reset_pending),
           bool_json(status.manager_auto_reset_attempted));
    print_json_string(status.sd_state ? status.sd_state : "unknown");
    printf(",\"interface\":");
    print_json_string(status.sd_interface ? status.sd_interface : "unknown");
    printf(",\"filesystem\":");
    print_json_string(status.sd_filesystem ? status.sd_filesystem : "unknown");
    printf(",\"direct_supported\":%s,\"rp2040_bridge_required\":%s,\"rp2040_bridge_ready\":%s,\"rp2040_protocol_supported\":%s,\"present\":%s,\"presence_stale\":%s,\"mounted\":%s,\"data_root_ready\":%s,\"needs_fat32\":%s,\"capacity_kb\":%lu,\"free_kb\":%lu,\"file_ops\":%s,\"file_line_max\":%lu,\"file_chunk_max\":%lu,\"path_max\":%lu,\"atomic_rename\":%s,\"response_truncated\":%s,\"status_stale\":%s,\"refresh_failures\":%lu,\"mount_point\":",
           bool_json(status.direct_supported),
           bool_json(status.rp2040_bridge_required),
           bool_json(status.rp2040_bridge_ready),
           bool_json(status.rp2040_sd_protocol_supported),
           bool_json(status.sd_present),
           bool_json(status.sd_presence_stale),
           bool_json(status.sd_mounted),
           bool_json(status.sd_data_root_ready),
           bool_json(status.sd_needs_fat32),
           (unsigned long)status.capacity_kb,
           (unsigned long)status.free_kb,
           bool_json(status.file_ops_supported),
           (unsigned long)status.file_line_max,
           (unsigned long)status.file_chunk_max,
           (unsigned long)status.path_max,
           bool_json(status.atomic_rename_supported),
           bool_json(status.response_truncated),
           bool_json(status.bridge_status_stale),
           (unsigned long)status.bridge_status_refresh_failures);
    print_json_string(status.mount_point ? status.mount_point : "");
    printf(",\"data_root\":");
    print_json_string(status.data_root ? status.data_root : "");
    printf(",\"probe_power\":");
    print_json_string(status.sd_probe_power[0] ? status.sd_probe_power : "unknown");
    printf(",\"probe_mode\":");
    print_json_string(status.sd_probe_mode[0] ? status.sd_probe_mode : "unknown");
    printf(",\"probe_error\":%lu,\"probe_data\":%lu,\"mount_error\":%lu,\"mount_data\":%lu,\"last_error\":\"%s\"},\"data_enabled\":%s,\"data_backend\":",
           (unsigned long)status.sd_probe_error,
           (unsigned long)status.sd_probe_data,
           (unsigned long)status.sd_mount_error,
           (unsigned long)status.sd_mount_data,
           esp_err_to_name(status.last_error), bool_json(status.data_enabled));
    print_json_string(status.data_backend ? status.data_backend : "volatile");
    printf(",\"message_store_backend\":");
    print_json_string(status.message_store_backend ? status.message_store_backend : "volatile");
    printf(",\"dm_store_backend\":");
    print_json_string(status.dm_store_backend ? status.dm_store_backend : "volatile");
    printf(",\"packet_log_backend\":");
    print_json_string(status.packet_log_backend ? status.packet_log_backend : "volatile");
    printf(",\"route_store_backend\":");
    print_json_string(status.route_store_backend ? status.route_store_backend : "volatile");
    printf(",\"node_store_backend\":");
    print_json_string(status.node_store_backend ?
                          status.node_store_backend : "volatile");
    printf(",\"contact_store_backend\":");
    print_json_string(status.contact_store_backend ?
                          status.contact_store_backend : "volatile");
    printf(",\"read_state_backend\":");
    print_json_string(status.read_state_backend ?
                          status.read_state_backend : "volatile");
    printf(",\"map_tile_backend\":");
    print_json_string(
        map_available && status.map_tile_backend ?
            status.map_tile_backend : "unavailable");
    printf(",\"map_tile_cache_ready\":%s,\"map_tile_cache_policy\":",
           bool_json(
               map_available && d1l_map_tile_store_sd_ready(&status)));
    print_json_string(
        map_available ? D1L_MAP_TILE_CACHE_POLICY :
                        "unavailable_in_release_profile");
    printf(",\"map_tile_cache_path_template\":");
    print_json_string(
        map_available ? D1L_MAP_TILE_CACHE_PATH_TEMPLATE : "");
    printf(",\"map_tile_download_supported\":%s,\"map_tile_source\":",
           bool_json(map_available &&
                     d1l_release_feature_available(
                         D1L_RELEASE_FEATURE_WIFI_USER_CONTROL) &&
                     connectivity.wifi_build_enabled &&
                      d1l_map_tile_store_sd_ready(&status)));
    print_json_string(map_available ? D1L_MAP_TILE_SOURCE_ID : "");
    printf(",\"map_tile_download_state\":");
    print_json_string(
        !map_available ? "unavailable_in_release_profile" :
        !d1l_map_tile_store_sd_ready(&status) ? "sd_cache_required" :
        !connectivity.wifi_connected ? "wifi_required" :
        D1L_MAP_TILE_DOWNLOAD_STATE);
    printf(",\"map_tile_policy\":");
    print_json_string(
        map_available ? D1L_MAP_TILE_PROVIDER_POLICY :
                        "unavailable_in_release_profile");
    printf(",\"map_tile_attribution\":");
    print_json_string(map_available ? D1L_MAP_TILE_ATTRIBUTION : "");
    printf(",\"export_backend\":");
    print_json_string(status.export_backend ? status.export_backend : "serial");
    printf(",\"retained_nvs\":{\"partition\":\"d1l_retained\",\"marker_ready\":%s,\"markers_complete\":%s,\"anchor_ready\":%s,\"sentinel_ready\":%s,\"external_init_required\":%s,\"initialized_this_boot\":%s,\"ready\":%s,\"init_error\":\"%s\",\"migrated_keys\":%lu,\"migration_error\":\"%s\",\"telemetry\":",
           bool_json(d1l_retained_blob_store_nvs_marker_ready()),
           bool_json(d1l_retained_blob_store_nvs_markers_complete()),
           bool_json(d1l_retained_blob_store_nvs_anchor_ready()),
           bool_json(d1l_retained_blob_store_nvs_sentinel_ready()),
           bool_json(d1l_retained_blob_store_nvs_external_init_required()),
           bool_json(d1l_retained_blob_store_nvs_initialized_this_boot()),
           bool_json(d1l_retained_blob_store_nvs_ready()),
           esp_err_to_name(d1l_retained_blob_store_nvs_error()),
           (unsigned long)d1l_retained_blob_store_nvs_migrated_keys(),
           esp_err_to_name(d1l_retained_blob_store_nvs_migration_error()));
    print_retained_nvs_telemetry_json(&retained_nvs_telemetry);
    printf("}");
    printf(",\"retained_scheduler\":");
    print_retained_scheduler_json(&retained_scheduler);
    printf(",\"retained_sd\":{\"degraded\":%s,\"backup_degraded\":%s,\"note\":",
           bool_json(status.retained_sd_degraded),
           bool_json(status.retained_backup_degraded));
    print_json_string(status.retained_sd_degraded ?
                      D1L_RETAINED_BLOB_STORE_SD_DEGRADED_NOTE : "");
    printf(",\"stores\":{\"messages\":");
    print_retained_sd_store_json(
        &status.retained_sd_stats[D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES]);
    printf(",\"dm\":");
    print_retained_sd_store_json(
        &status.retained_sd_stats[D1L_RETAINED_BLOB_STORE_DM_MESSAGES]);
    printf(",\"routes\":");
    print_retained_sd_store_json(
        &status.retained_sd_stats[D1L_RETAINED_BLOB_STORE_ROUTES]);
    printf(",\"packets\":");
    print_retained_sd_store_json(
        &status.retained_sd_stats[D1L_RETAINED_BLOB_STORE_PACKET_LOG]);
    printf(",\"nodes\":");
    print_retained_sd_store_json(
        &status.retained_sd_stats[D1L_RETAINED_BLOB_STORE_NODES]);
    printf(",\"contacts\":");
    print_retained_sd_store_json(
        &status.retained_sd_stats[D1L_RETAINED_BLOB_STORE_CONTACTS]);
    printf(",\"read_state\":");
    print_retained_sd_store_json(
        &status.retained_sd_stats[D1L_RETAINED_BLOB_STORE_READ_STATE]);
    printf("}}");
    printf(",\"stores\":{\"settings\":\"nvs\",\"identity\":\"nvs\",\"messages\":");
    print_json_string(status.message_store_backend ? status.message_store_backend : "volatile");
    printf(",\"dm\":");
    print_json_string(status.dm_store_backend ? status.dm_store_backend : "volatile");
    printf(",\"packets\":");
    print_json_string(status.packet_log_backend ? status.packet_log_backend : "volatile");
    printf(",\"routes\":");
    print_json_string(status.route_store_backend ? status.route_store_backend : "volatile");
    printf(",\"nodes\":");
    print_json_string(status.node_store_backend ?
                          status.node_store_backend : "volatile");
    printf(",\"contacts\":");
    print_json_string(status.contact_store_backend ?
                          status.contact_store_backend : "volatile");
    printf(",\"read_state\":");
    print_json_string(status.read_state_backend ?
                          status.read_state_backend : "volatile");
    printf(",\"crashlog\":\"nvs\",\"map_tiles\":");
    print_json_string(
        map_available && status.map_tile_backend ?
            status.map_tile_backend : "unavailable");
    printf(",\"exports\":");
    print_json_string(status.export_backend ? status.export_backend : "serial");
    printf("},\"node_history_requires_sd\":true,"
           "\"live_mesh_without_sd\":[\"rf_tx\",\"rf_rx\",\"chat\"],"
           "\"setup_required\":%s,\"setup_supported\":%s,\"setup_action\":",
           bool_json(status.setup_required), bool_json(status.setup_supported));
    print_json_string(status.setup_action ? status.setup_action : "not_available");
    printf(",\"fallback\":\"per_store\",\"note\":");
    print_json_string(status.note ? status.note : "");
    printf("}\n");
}

static void cmd_storage_mount(void)
{
    esp_err_t ret = d1l_storage_status_mount(D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS);
    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);
    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":\"storage mount\",\"code\":\"%s\",\"sd\":{\"state\":",
           D1L_CONSOLE_SCHEMA,
           bool_json(ret == ESP_OK),
           esp_err_to_name(ret));
    print_json_string(status.sd_state ? status.sd_state : "unknown");
    printf(",\"interface\":");
    print_json_string(status.sd_interface ? status.sd_interface : "unknown");
    printf(",\"filesystem\":");
    print_json_string(status.sd_filesystem ? status.sd_filesystem : "unknown");
    printf(",\"rp2040_bridge_ready\":%s,\"rp2040_protocol_supported\":%s,\"present\":%s,\"mounted\":%s,\"data_root_ready\":%s,\"needs_fat32\":%s,\"capacity_kb\":%lu,\"free_kb\":%lu,\"file_ops\":%s,\"file_line_max\":%lu,\"file_chunk_max\":%lu,\"path_max\":%lu,\"atomic_rename\":%s,\"response_truncated\":%s,\"probe_power\":",
           bool_json(status.rp2040_bridge_ready),
           bool_json(status.rp2040_sd_protocol_supported),
           bool_json(status.sd_present),
           bool_json(status.sd_mounted),
           bool_json(status.sd_data_root_ready),
           bool_json(status.sd_needs_fat32),
           (unsigned long)status.capacity_kb,
           (unsigned long)status.free_kb,
           bool_json(status.file_ops_supported),
           (unsigned long)status.file_line_max,
           (unsigned long)status.file_chunk_max,
           (unsigned long)status.path_max,
           bool_json(status.atomic_rename_supported),
           bool_json(status.response_truncated));
    print_json_string(status.sd_probe_power[0] ? status.sd_probe_power : "unknown");
    printf(",\"probe_mode\":");
    print_json_string(status.sd_probe_mode[0] ? status.sd_probe_mode : "unknown");
    printf(",\"probe_error\":%lu,\"probe_data\":%lu,\"mount_error\":%lu,\"mount_data\":%lu,\"last_error\":\"%s\"},\"setup_required\":%s,\"setup_supported\":%s,\"setup_action\":",
           (unsigned long)status.sd_probe_error,
           (unsigned long)status.sd_probe_data,
           (unsigned long)status.sd_mount_error,
           (unsigned long)status.sd_mount_data,
           esp_err_to_name(status.last_error),
           bool_json(status.setup_required),
           bool_json(status.setup_supported));
    print_json_string(status.setup_action ? status.setup_action : "not_available");
    printf(",\"public_rf_tx\":false,\"formats_sd\":false,\"note\":");
    print_json_string(status.note ? status.note : "");
    printf("}\n");
}

static void print_storage_manager_result(const char *cmd, esp_err_t ret,
                                         bool retained_worker_quiesce_acquired)
{
    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);
    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":",
           D1L_CONSOLE_SCHEMA,
           bool_json(ret == ESP_OK));
    print_json_string(cmd);
    printf(",\"code\":\"%s\",\"retained_worker_quiesce_acquired\":%s,\"manager\":{\"running\":%s,\"state\":",
           esp_err_to_name(ret),
           bool_json(retained_worker_quiesce_acquired),
           bool_json(status.manager_running));
    print_json_string(status.manager_state ? status.manager_state : "BRIDGE_WAIT");
    printf(",\"attempt\":%lu,\"backoff_ms\":%lu,\"force_nvs\":%s,"
           "\"initial_ping_timeouts\":%lu,\"bridge_response_seen\":%s,"
           "\"auto_reset_pending\":%s,\"auto_reset_attempted\":%s},\"sd\":{\"state\":",
           (unsigned long)status.manager_attempt,
           (unsigned long)status.manager_backoff_ms,
           bool_json(status.force_nvs),
           (unsigned long)status.manager_initial_ping_timeouts,
           bool_json(status.manager_bridge_response_seen),
           bool_json(status.manager_auto_reset_pending),
           bool_json(status.manager_auto_reset_attempted));
    print_json_string(status.sd_state ? status.sd_state : "unknown");
    printf(",\"rp2040_bridge_ready\":%s,\"rp2040_protocol_supported\":%s,\"present\":%s,\"presence_stale\":%s,\"mounted\":%s,\"data_root_ready\":%s,\"status_stale\":%s,\"refresh_failures\":%lu},\"setup_action\":",
           bool_json(status.rp2040_bridge_ready),
           bool_json(status.rp2040_sd_protocol_supported),
           bool_json(status.sd_present),
           bool_json(status.sd_presence_stale),
           bool_json(status.sd_mounted),
           bool_json(status.sd_data_root_ready),
           bool_json(status.bridge_status_stale),
           (unsigned long)status.bridge_status_refresh_failures);
    print_json_string(status.setup_action ? status.setup_action : "not_available");
    printf(",\"public_rf_tx\":false,\"formats_sd\":false,\"note\":");
    print_json_string(status.note ? status.note : "");
    printf("}\n");
}

static void cmd_storage_remount(void)
{
    bool retained_worker_quiesce_acquired = false;
    esp_err_t ret =
        d1l_storage_status_remount_blocking(
            D1L_STORAGE_REMOUNT_TIMEOUT_MS,
            &retained_worker_quiesce_acquired);
    print_storage_manager_result("storage remount", ret,
                                 retained_worker_quiesce_acquired);
}

static void cmd_storage_reset_bridge(void)
{
    esp_err_t ret = d1l_storage_manager_reset_bridge();
    print_storage_manager_result("storage reset-bridge", ret, false);
}

static void cmd_storage_force_nvs(const char *line)
{
    bool force = true;
    if (strcmp(line, "storage force-nvs off") == 0) {
        force = false;
    } else if (strcmp(line, "storage force-nvs") != 0 &&
               strcmp(line, "storage force-nvs on") != 0) {
        err_result("storage force-nvs", "INVALID_MODE",
                   "usage: storage force-nvs [on|off]");
        return;
    }
    d1l_storage_manager_force_nvs(force);
    esp_err_t ret = d1l_storage_manager_start();
    print_storage_manager_result(force ? "storage force-nvs" : "storage force-nvs off",
                                 ret, false);
}

static void print_storage_diag_probe(const char *name,
                                     bool present,
                                     uint32_t error,
                                     uint32_t data,
                                     uint32_t capacity_kb)
{
    printf(",\"");
    printf("%s", name);
    printf("\":{\"present\":%s,\"error\":%lu,\"data\":%lu,\"capacity_kb\":%lu}",
           bool_json(present),
           (unsigned long)error,
           (unsigned long)data,
           (unsigned long)capacity_kb);
}

/*
 * The console owns large, cold command snapshots that do not participate in
 * ISR or cache-disabled paths. Keep them in PSRAM so optional Wi-Fi can retain
 * the internal-DMA heap it needs when it is enabled after the full UI starts.
 */
static d1l_rp2040_sd_diag_t s_console_storage_diag EXT_RAM_BSS_ATTR;

static void cmd_storage_diag(void)
{
    d1l_rp2040_sd_diag_t *diag = &s_console_storage_diag;
    esp_err_t ret = d1l_rp2040_bridge_sd_diag(diag, D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS);
    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":\"storage diag\",\"code\":\"%s\",\"bridge_ready\":%s,\"diag_supported\":%s,\"response_truncated\":%s,\"pins\":",
            D1L_CONSOLE_SCHEMA,
            bool_json(ret == ESP_OK),
            esp_err_to_name(ret),
            bool_json(diag->bridge_ready),
            bool_json(diag->protocol_supported),
            bool_json(diag->response_truncated));
    print_json_string(diag->pins[0] ? diag->pins : "unknown");
    printf(",\"spi_hz\":%lu,\"selected_power\":",
            (unsigned long)diag->spi_hz);
    print_json_string(diag->selected_power[0] ? diag->selected_power : "unknown");
    printf(",\"selected_mode\":");
    print_json_string(diag->selected_mode[0] ? diag->selected_mode : "unknown");
    printf(",\"mount_selected\":%s,\"probes\":{",
            bool_json(diag->mount_selected));
    printf("\"high_dedicated\":{\"present\":%s,\"error\":%lu,\"data\":%lu,\"capacity_kb\":%lu}",
            bool_json(diag->high_dedicated_present),
            (unsigned long)diag->high_dedicated_error,
            (unsigned long)diag->high_dedicated_data,
            (unsigned long)diag->high_dedicated_capacity_kb);
    print_storage_diag_probe("high_shared", diag->high_shared_present,
                             diag->high_shared_error, diag->high_shared_data,
                             diag->high_shared_capacity_kb);
    print_storage_diag_probe("low_dedicated", diag->low_dedicated_present,
                             diag->low_dedicated_error, diag->low_dedicated_data,
                             diag->low_dedicated_capacity_kb);
    print_storage_diag_probe("low_shared", diag->low_shared_present,
                             diag->low_shared_error, diag->low_shared_data,
                             diag->low_shared_capacity_kb);
    printf("},\"formats_sd\":false,\"public_rf_tx\":false,\"note\":");
    print_json_string(diag->note[0] ? diag->note : "");
    printf("}\n");
}

static void cmd_storage_diag_raw(void)
{
    d1l_rp2040_sd_diag_t *diag = &s_console_storage_diag;
    esp_err_t ret = d1l_rp2040_bridge_sd_diag(diag, D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS);
    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":\"storage diag raw\",\"code\":\"%s\",\"bridge_ready\":%s,\"diag_supported\":%s,\"response_truncated\":%s,\"raw_line\":",
            D1L_CONSOLE_SCHEMA,
            bool_json(ret == ESP_OK),
            esp_err_to_name(ret),
            bool_json(diag->bridge_ready),
            bool_json(diag->protocol_supported),
            bool_json(diag->response_truncated));
    print_json_string(diag->raw_line[0] ? diag->raw_line : "");
    printf(",\"formats_sd\":false,\"public_rf_tx\":false,\"note\":");
    print_json_string(diag->note[0] ? diag->note : "");
    printf("}\n");
}

static void cmd_storage_map_policy(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP)) {
        release_unavailable_status(
            "storage map-policy", D1L_RELEASE_FEATURE_MAP);
        return;
    }
    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);
    d1l_connectivity_status_t connectivity = {0};
    d1l_connectivity_status(&connectivity);
    char sample_path[D1L_RP2040_FILE_PATH_MAX + 1U] = {0};
    const bool sample_path_ok = d1l_map_tile_store_path(0U, 0U, 0U,
                                                        sample_path,
                                                        sizeof(sample_path));
    const bool cache_ready = d1l_map_tile_store_sd_ready(&status);

    ok_begin("storage map-policy");
    printf(",\"kind\":\"map_tile_policy\",\"map_page_supported\":true,\"tile_cache_ready\":%s,\"tile_cache_backend\":",
           bool_json(cache_ready));
    print_json_string(status.map_tile_backend ? status.map_tile_backend : "unavailable");
    printf(",\"cache_policy\":");
    print_json_string(D1L_MAP_TILE_CACHE_POLICY);
    printf(",\"cache_path_template\":");
    print_json_string(D1L_MAP_TILE_CACHE_PATH_TEMPLATE);
    printf(",\"sample_path\":");
    print_json_string(sample_path_ok ? sample_path : "");
    const bool download_supported = connectivity.wifi_build_enabled && cache_ready;
    const bool live_network_download = download_supported && connectivity.wifi_connected;
    const char *download_state = !cache_ready ? "sd_cache_required" :
                                 !connectivity.wifi_connected ? "wifi_required" :
                                 D1L_MAP_TILE_DOWNLOAD_STATE;
    printf(",\"source\":");
    print_json_string(D1L_MAP_TILE_SOURCE_ID);
    printf(",\"url_template\":");
    print_json_string(D1L_MAP_TILE_SOURCE_URL_TEMPLATE);
    printf(",\"attribution\":");
    print_json_string(D1L_MAP_TILE_ATTRIBUTION);
    printf(",\"license_url\":");
    print_json_string(D1L_MAP_TILE_LICENSE_URL);
    printf(",\"built_in\":true,\"current_view_only\":true,\"max_current_view_tiles\":%u,\"min_cache_days\":%u,\"zoom\":%u,\"default_zoom\":%u,\"min_zoom\":%u,\"max_zoom\":%u,\"sideload_supported\":false",
           (unsigned)D1L_MAP_VIEW_MAX_TILES,
           (unsigned)D1L_MAP_TILE_MIN_CACHE_DAYS,
           (unsigned)D1L_MAP_VIEW_DEFAULT_ZOOM,
           (unsigned)D1L_MAP_VIEW_DEFAULT_ZOOM,
           (unsigned)D1L_MAP_VIEW_MIN_ZOOM,
           (unsigned)D1L_MAP_VIEW_MAX_ZOOM);
#if D1L_ENABLE_QUALIFICATION_HOOKS
    printf(",\"canary_command\":\"storage map-tile-canary <token>\"");
#endif
    printf(",\"download_supported\":%s,\"live_network_download\":%s,\"download_state\":",
           bool_json(download_supported),
           bool_json(live_network_download));
    print_json_string(download_state);
    printf(",\"download_requires\":");
    print_json_string(D1L_MAP_TILE_DOWNLOAD_REQUIRES);
    printf(",\"policy\":");
    print_json_string(D1L_MAP_TILE_PROVIDER_POLICY);
    printf(",\"wifi_state\":");
    print_json_string(connectivity.wifi_state ? connectivity.wifi_state : "off");
    printf(",\"wifi_build_enabled\":%s,\"public_rf_tx\":false,\"formats_sd\":false,\"note\":\"Set a location and enable Wi-Fi; the built-in OpenStreetMap source fetches only the visible current view into the persistent SD cache\"}\n",
           bool_json(connectivity.wifi_build_enabled));
}

static void cmd_map_tiles_status(void)
{
    d1l_map_view_status_t status = {0};
    d1l_map_view_service_status(&status);
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP)) {
        ok_begin("map tiles status");
        printf(",\"available\":false");
        print_release_profile_fields();
        printf(",\"feature\":\"map\",\"initialized\":%s,\"visible\":%s,"
               "\"worker_running\":%s,\"network_requests\":%u,"
               "\"runtime_policy_violation\":%s,"
               "\"mutation_allowed\":false,"
               "\"reason\":\"unavailable_in_release_profile\","
               "\"public_rf_tx\":false,\"formats_sd\":false}\n",
               bool_json(status.initialized), bool_json(status.visible),
               bool_json(status.worker_running),
               (unsigned)status.network_requests,
               bool_json(
                   status.initialized || status.visible ||
                   status.worker_running || status.network_requests > 0U));
        return;
    }
    ok_begin("map tiles status");
    printf(",\"source\":");
    print_json_string(status.source_id[0] ?
                      status.source_id : D1L_MAP_TILE_SOURCE_ID);
    printf(",\"render_style\":");
    print_json_string(D1L_MAP_RENDER_STYLE_ID);
    printf(",\"attribution\":");
    print_json_string(status.attribution[0] ?
                      status.attribution : D1L_MAP_TILE_ATTRIBUTION);
    printf(",\"provider_configured\":%s,"
           "\"background_prefetch_permitted\":%s,"
           "\"provider_max_zoom\":%u,"
           "\"initialized\":%s,\"visible\":%s,\"worker_running\":%s,\"frame_ready\":%s,\"sd_cache_ready\":%s,\"wifi_connected\":%s,\"rate_limited\":%s,\"current_view_only\":%s,\"generation\":%lu,\"frame_revision\":%lu,\"retry_after_sec\":%lu,\"decode_total_us\":%lu,\"decode_max_us\":%lu,\"decode_samples\":%u,\"worker_stack_bytes\":%lu,\"worker_stack_free_bytes\":%lu,\"lat_e7\":%ld,\"lon_e7\":%ld,\"width\":%u,\"height\":%u,\"zoom\":%u,\"planned_tiles\":%u,\"attempted_tiles\":%u,\"cache_hits\":%u,\"network_requests\":%u,\"downloaded_tiles\":%u,\"rendered_tiles\":%u,\"failed_tiles\":%u,\"phase\":",
           bool_json(status.provider_configured),
           bool_json(status.background_prefetch_permitted),
           (unsigned)status.provider_max_zoom,
           bool_json(status.initialized), bool_json(status.visible),
           bool_json(status.worker_running), bool_json(status.frame_ready),
           bool_json(status.sd_cache_ready), bool_json(status.wifi_connected),
           bool_json(status.rate_limited), bool_json(status.current_view_only),
           (unsigned long)status.generation, (unsigned long)status.frame_revision,
           (unsigned long)status.retry_after_sec,
           (unsigned long)status.decode_total_us,
           (unsigned long)status.decode_max_us,
           (unsigned)status.decode_samples,
           (unsigned long)status.worker_stack_bytes,
           (unsigned long)status.worker_stack_free_bytes,
           (long)status.lat_e7,
           (long)status.lon_e7, (unsigned)status.width, (unsigned)status.height,
           (unsigned)status.zoom, (unsigned)status.planned_tiles,
           (unsigned)status.attempted_tiles, (unsigned)status.cache_hits,
           (unsigned)status.network_requests, (unsigned)status.downloaded_tiles,
           (unsigned)status.rendered_tiles, (unsigned)status.failed_tiles);
    print_json_string(status.phase);
    printf(",\"message\":");
    print_json_string(status.message);
    printf(",\"public_rf_tx\":false,\"formats_sd\":false}\n");
}

static void cmd_map_provider_status(void)
{
    d1l_settings_t settings = {0};
    d1l_connectivity_status_t connectivity = {0};
    d1l_storage_status_t storage = {0};
    d1l_map_prefetch_status_t prefetch = {0};
    d1l_map_tile_provider_t provider = {0};
    (void)d1l_settings_public_snapshot(&settings);
    d1l_connectivity_status(&connectivity);
    d1l_storage_status(&storage);
    const bool sd_ready = d1l_map_tile_store_sd_ready(&storage);
    const esp_err_t provider_refresh = sd_ready ?
        d1l_map_tile_provider_refresh(&storage) : ESP_ERR_NOT_SUPPORTED;
    d1l_map_tile_provider_snapshot(&provider);
    d1l_map_prefetch_service_status(&prefetch);

    const bool provider_https =
        strncmp(provider.url_template, "https://", 8U) == 0;
    const bool osm_standard =
        d1l_map_tile_provider_uses_osm_standard(&provider);
    const bool authorized_provider =
        provider_refresh == ESP_OK && provider.configured &&
        provider.network_fetch_allowed && provider_https &&
        provider.offline_storage_permitted &&
        provider.background_prefetch_permitted && !osm_standard;
    const bool nodes_accounted =
        prefetch.nodes_included + prefetch.nodes_outside_radius ==
        prefetch.nodes_seen;

    ok_begin("map provider status");
    printf(",\"configured\":%s,\"authorized_provider\":%s,"
           "\"provider_refresh_ok\":%s,\"provider_refresh_code\":",
           bool_json(provider.configured),
           bool_json(authorized_provider),
           bool_json(provider_refresh == ESP_OK));
    print_json_string(esp_err_to_name(provider_refresh));
    printf(",\"source_id\":");
    print_json_string(provider.source_id);
    printf(",\"network_url_redacted\":true,"
           "\"https\":%s,\"network_fetch_allowed\":%s,"
           "\"offline_storage_permitted\":%s,"
           "\"background_prefetch_permitted\":%s,"
           "\"osm_standard_endpoint\":%s,\"attribution\":",
           bool_json(provider_https),
           bool_json(provider.network_fetch_allowed),
           bool_json(provider.offline_storage_permitted),
           bool_json(provider.background_prefetch_permitted),
           bool_json(osm_standard));
    print_json_string(provider.attribution);
    printf(",\"license_url\":");
    print_json_string(provider.license_url);
    printf(",\"provider_max_zoom\":%u,"
           "\"minimum_request_interval_ms\":%lu,"
           "\"average_tile_bytes\":%lu,"
           "\"cache_budget_mb\":%lu,\"device_location\":{"
           "\"set\":%s,\"lat_e7\":%ld,\"lon_e7\":%ld,\"source\":",
           (unsigned)provider.max_zoom,
           (unsigned long)provider.minimum_request_interval_ms,
           (unsigned long)provider.average_tile_bytes,
           (unsigned long)provider.cache_budget_mb,
           bool_json(settings.map_location_set),
           (long)settings.map_lat_e7, (long)settings.map_lon_e7);
    print_json_string(map_location_source_name(&settings));
    printf("},\"wifi\":{\"setting_enabled\":%s,\"profile_saved\":%s,"
           "\"connected\":%s,\"state\":",
           bool_json(connectivity.wifi_enabled_setting),
           bool_json(connectivity.wifi_profile_saved),
           bool_json(connectivity.wifi_connected));
    print_json_string(connectivity.wifi_state ?
                      connectivity.wifi_state : "off");
    printf("},\"sd\":{\"ready\":%s,\"capacity_kb\":%lu,"
           "\"free_kb\":%lu,\"backend\":",
           bool_json(sd_ready), (unsigned long)storage.capacity_kb,
           (unsigned long)storage.free_kb);
    print_json_string(storage.map_tile_backend ?
                      storage.map_tile_backend : "unavailable");
    printf("},\"node_radius_km\":%.0f,\"nodes_accounted\":%s,"
           "\"node_markers\":{\"generation\":%lu,\"seen\":%u,"
           "\"included\":%u,\"outside_radius\":%u},"
           "\"prefetch\":{\"initialized\":%s,\"running\":%s,"
           "\"eligible\":%s,\"complete\":%s,"
           "\"paused_for_visible_map\":%s,\"location_set\":%s,"
           "\"wifi_connected\":%s,\"sd_ready\":%s,"
           "\"provider_configured\":%s,"
           "\"background_prefetch_permitted\":%s,"
           "\"storage_reserve_reached\":%s,"
           "\"selected_max_zoom\":%u,\"total_tiles\":%" PRIu64 ","
           "\"visited_tiles\":%" PRIu64 ",\"cached_tiles\":%" PRIu64 ","
           "\"network_requests\":%" PRIu64 ","
           "\"downloaded_tiles\":%" PRIu64 ",\"failed_tiles\":%" PRIu64 ","
           "\"evicted_tiles\":%" PRIu64 ","
           "\"downloaded_bytes\":%" PRIu64 ","
           "\"cache_used_bytes\":%" PRIu64 ","
           "\"estimated_bytes\":%" PRIu64 ","
           "\"allocation_bytes\":%" PRIu64 ","
           "\"cache_budget_mb\":%lu,"
           "\"worker_stack_bytes\":%lu,"
           "\"worker_stack_free_bytes\":%lu,\"source_id\":",
           D1L_MAP_PREFETCH_NODE_RADIUS_KM,
           bool_json(nodes_accounted),
           (unsigned long)prefetch.marker_generation,
           (unsigned)prefetch.nodes_seen,
           (unsigned)prefetch.nodes_included,
           (unsigned)prefetch.nodes_outside_radius,
           bool_json(prefetch.initialized), bool_json(prefetch.running),
           bool_json(prefetch.eligible), bool_json(prefetch.complete),
           bool_json(prefetch.paused_for_visible_map),
           bool_json(prefetch.location_set),
           bool_json(prefetch.wifi_connected),
           bool_json(prefetch.sd_ready),
           bool_json(prefetch.provider_configured),
           bool_json(prefetch.background_prefetch_permitted),
           bool_json(prefetch.storage_reserve_reached),
           (unsigned)prefetch.selected_max_zoom,
           prefetch.total_tiles, prefetch.visited_tiles,
           prefetch.cached_tiles, prefetch.network_requests,
           prefetch.downloaded_tiles, prefetch.failed_tiles,
           prefetch.evicted_tiles, prefetch.downloaded_bytes,
           prefetch.cache_used_bytes, prefetch.estimated_bytes,
           prefetch.allocation_bytes,
           (unsigned long)prefetch.cache_budget_mb,
           (unsigned long)prefetch.worker_stack_bytes,
           (unsigned long)prefetch.worker_stack_free_bytes);
    print_json_string(prefetch.source_id);
    printf(",\"phase\":");
    print_json_string(prefetch.phase);
    printf(",\"message\":");
    print_json_string(prefetch.message);
    printf("},\"network_requests\":%" PRIu64 ","
           "\"downloaded_tiles\":%" PRIu64 ","
           "\"public_rf_tx\":false,\"formats_sd\":false}\n",
           prefetch.network_requests, prefetch.downloaded_tiles);
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void cmd_map_acceptance_open(void)
{
    const esp_err_t ret = d1l_ui_phase1_request_map_acceptance();
    if (ret != ESP_OK) {
        err_result("map acceptance open", esp_err_to_name(ret),
                   "close overlays and complete onboarding before Map acceptance");
        return;
    }
    ok_begin("map acceptance open");
    printf(",\"requested_tab\":\"map\",\"pending\":true,"
           "\"configured_current_view_only\":true,"
           "\"configured_device_center\":true,"
           "\"forced_sd_reload\":true,"
           "\"arbitrary_url_accepted\":false,"
           "\"arbitrary_location_accepted\":false,"
           "\"public_rf_tx\":false,\"formats_sd\":false}\n");
}
#endif

static void print_storage_setup_payload(const d1l_storage_status_t *status)
{
    printf(",\"available\":%s,\"setup_required\":%s,\"setup_supported\":%s,\"setup_action\":",
           bool_json(status->rp2040_sd_protocol_supported || status->sd_present),
           bool_json(status->setup_required),
           bool_json(status->setup_supported));
    print_json_string(status->setup_action ? status->setup_action : "not_available");
    printf(",\"needs_fat32\":%s,\"will_format\":false,\"format_requested\":false,\"format_performed\":false,\"policy\":\"no_device_format\",\"data_backend\":",
           bool_json(status->sd_needs_fat32));
    print_json_string(status->data_backend ? status->data_backend : "volatile");
    printf(",\"fallback\":\"volatile\",\"note\":");
    print_json_string(status->note ? status->note : "");
    printf("}\n");
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void print_storage_filecanary_error(const char *step,
                                           esp_err_t ret,
                                           const d1l_storage_status_t *status,
                                           const d1l_rp2040_file_result_t *file,
                                           const char *hint)
{
    printf("{\"schema\":%d,\"ok\":false,\"cmd\":\"storage filecanary\",\"code\":\"%s\",\"step\":",
           D1L_CONSOLE_SCHEMA, esp_err_to_name(ret));
    print_json_string(step ? step : "unknown");
    printf(",\"sd_state\":");
    print_json_string(status && status->sd_state ? status->sd_state : "unknown");
    printf(",\"data_backend\":");
    print_json_string(status && status->data_backend ? status->data_backend : "volatile");
    printf(",\"packet_log_backend\":");
    print_json_string(status && status->packet_log_backend ? status->packet_log_backend : "volatile");
    printf(",\"file_op\":");
    print_json_string(file && file->op[0] ? file->op : "");
    printf(",\"file_error\":");
    print_json_string(file && file->err[0] ? file->err : "");
    printf(",\"file_note\":");
    print_json_string(file && file->note[0] ? file->note : "");
    printf(",\"hint\":");
    print_json_string(hint ? hint : "SD file-operation canary failed; no format was performed");
    printf("}\n");
}

static void print_storage_filecanary_error_and_resume(const char *step,
                                                      esp_err_t ret,
                                                      const d1l_storage_status_t *status,
                                                      const d1l_rp2040_file_result_t *file,
                                                      const char *hint)
{
    d1l_storage_manager_resume();
    print_storage_filecanary_error(step, ret, status, file, hint);
}

static bool storage_filecanary_ready(const d1l_storage_status_t *status)
{
    return status &&
           status->file_ops_supported &&
           status->atomic_rename_supported &&
           status->file_line_max >= D1L_RP2040_FILE_LINE_MAX &&
           status->file_chunk_max >= D1L_RP2040_FILE_CHUNK_MAX &&
           status->path_max >= D1L_RP2040_FILE_PATH_MAX;
}

static void cmd_storage_filecanary(void)
{
    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);
    if (!storage_filecanary_ready(&status)) {
        (void)d1l_storage_status_mount(D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS);
        d1l_storage_status(&status);
    }

    if (!storage_filecanary_ready(&status)) {
        print_storage_filecanary_error(
            "preflight",
            ESP_ERR_NOT_SUPPORTED,
            &status,
            NULL,
            "requires a ready RP2040 SD bridge with file_ops and atomic rename; retained-store migration is tested by storage retained-canary");
        return;
    }

    static const uint8_t payload[] = "DeskOS SD file canary v1";
    const size_t payload_len = sizeof(payload) - 1U;
    const uint32_t token = esp_random();
    const uint32_t tick = (uint32_t)xTaskGetTickCount();
    char tmp_path[D1L_RP2040_FILE_PATH_MAX + 1U];
    char final_path[D1L_RP2040_FILE_PATH_MAX + 1U];
    uint8_t read_buf[D1L_RP2040_FILE_CHUNK_MAX];
    d1l_rp2040_file_result_t file = {0};

    int path_len = snprintf(tmp_path, sizeof(tmp_path), "canary/fc-%08lx-%08lx.tmp",
                            (unsigned long)token, (unsigned long)tick);
    int final_len = snprintf(final_path, sizeof(final_path), "canary/fc-%08lx-%08lx.bin",
                             (unsigned long)token, (unsigned long)tick);
    if (path_len <= 0 || (size_t)path_len >= sizeof(tmp_path) ||
        final_len <= 0 || (size_t)final_len >= sizeof(final_path)) {
        print_storage_filecanary_error(
            "path",
            ESP_ERR_INVALID_SIZE,
            &status,
            NULL,
            "could not build unique file canary paths");
        return;
    }

    d1l_storage_manager_pause(D1L_STORAGE_FILE_CANARY_MANAGER_PAUSE_MS);

    esp_err_t ret = d1l_rp2040_bridge_file_write(tmp_path, 0U, payload, payload_len, true,
                                                 &file, D1L_STORAGE_FILE_CANARY_OP_TIMEOUT_MS);
    if (ret != ESP_OK || file.length != payload_len || file.size != payload_len) {
        print_storage_filecanary_error_and_resume("write_tmp", ret == ESP_OK ? ESP_FAIL : ret,
                                                  &status, &file, "temp write failed");
        return;
    }

    memset(read_buf, 0, sizeof(read_buf));
    ret = d1l_rp2040_bridge_file_read(tmp_path, 0U, read_buf, payload_len,
                                      &file, D1L_STORAGE_FILE_CANARY_OP_TIMEOUT_MS);
    if (ret != ESP_OK || file.length != payload_len ||
        memcmp(read_buf, payload, payload_len) != 0) {
        print_storage_filecanary_error_and_resume("read_tmp", ret == ESP_OK ? ESP_FAIL : ret,
                                                  &status, &file, "temp read-back did not match");
        return;
    }

    ret = d1l_rp2040_bridge_file_rename(tmp_path, final_path, true, &file,
                                        D1L_STORAGE_FILE_CANARY_OP_TIMEOUT_MS);
    if (ret != ESP_OK) {
        print_storage_filecanary_error_and_resume("rename_final", ret, &status, &file,
                                                  "rename replace commit failed");
        return;
    }

    ret = d1l_rp2040_bridge_file_stat(final_path, &file,
                                      D1L_STORAGE_FILE_CANARY_OP_TIMEOUT_MS);
    if (ret != ESP_OK || !file.exists || file.is_directory || file.size != payload_len) {
        print_storage_filecanary_error_and_resume("stat_final", ret == ESP_OK ? ESP_FAIL : ret,
                                                  &status, &file, "final canary stat did not match");
        return;
    }

    memset(read_buf, 0, sizeof(read_buf));
    ret = d1l_rp2040_bridge_file_read(final_path, 0U, read_buf, payload_len,
                                      &file, D1L_STORAGE_FILE_CANARY_OP_TIMEOUT_MS);
    if (ret != ESP_OK || file.length != payload_len ||
        memcmp(read_buf, payload, payload_len) != 0) {
        print_storage_filecanary_error_and_resume("read_final", ret == ESP_OK ? ESP_FAIL : ret,
                                                  &status, &file, "final read-back did not match");
        return;
    }

    ret = d1l_rp2040_bridge_file_delete(final_path, &file,
                                        D1L_STORAGE_FILE_CANARY_OP_TIMEOUT_MS);
    if (ret != ESP_OK) {
        print_storage_filecanary_error_and_resume("delete_final", ret, &status, &file,
                                                  "could not delete final canary file");
        return;
    }

    ret = d1l_rp2040_bridge_file_stat(final_path, &file,
                                      D1L_STORAGE_FILE_CANARY_OP_TIMEOUT_MS);
    if (ret != ESP_OK || file.exists) {
        print_storage_filecanary_error_and_resume("stat_deleted", ret == ESP_OK ? ESP_FAIL : ret,
                                                  &status, &file, "final canary file still exists after delete");
        return;
    }

    d1l_storage_manager_resume();
    ok_begin("storage filecanary");
    printf(",\"path\":");
    print_json_string(final_path);
    printf(",\"tmp_path\":");
    print_json_string(tmp_path);
    printf(",\"bytes\":%u,\"write_tmp\":true,\"read_tmp\":true,\"rename_replace\":true,\"stat_final\":true,\"read_final\":true,\"delete_final\":true,\"stat_deleted\":true,\"data_backend\":",
           (unsigned)payload_len);
    print_json_string(status.data_backend ? status.data_backend : "volatile");
    printf(",\"packet_log_backend\":");
    print_json_string(status.packet_log_backend ? status.packet_log_backend : "volatile");
    printf(",\"node_store_backend\":");
    print_json_string(status.node_store_backend ?
                          status.node_store_backend : "volatile");
    printf(",\"contact_store_backend\":");
    print_json_string(status.contact_store_backend ?
                          status.contact_store_backend : "volatile");
    printf(",\"read_state_backend\":");
    print_json_string(status.read_state_backend ?
                          status.read_state_backend : "volatile");
    printf(",\"note\":\"Serial-only RP2040 SD file-operation canary passed; no Public RF or format command was used\"}\n");
}
#endif

#if D1L_ENABLE_QUALIFICATION_HOOKS
static bool text_equals(const char *lhs, const char *rhs)
{
    return lhs && rhs && strcmp(lhs, rhs) == 0;
}

static bool storage_retained_history_sd_ready(const d1l_storage_status_t *status)
{
    return status &&
           status->data_enabled &&
           text_equals(status->data_backend, "sd") &&
           text_equals(status->message_store_backend, "sd") &&
           text_equals(status->dm_store_backend, "sd") &&
           text_equals(status->route_store_backend, "sd") &&
           text_equals(status->packet_log_backend, "sd") &&
           text_equals(status->node_store_backend, "sd") &&
           text_equals(status->contact_store_backend, "sd") &&
           text_equals(status->read_state_backend, "sd") &&
           status->sd_present &&
           status->sd_mounted &&
           status->sd_data_root_ready &&
           status->rp2040_sd_protocol_supported &&
           status->file_ops_supported &&
           status->atomic_rename_supported &&
           status->file_line_max >= D1L_RP2040_FILE_LINE_MAX &&
           status->file_chunk_max >= D1L_RP2040_FILE_CHUNK_MAX &&
           status->path_max >= D1L_RP2040_FILE_PATH_MAX;
}

static bool storage_retained_history_volatile_no_card_ready(
    const d1l_storage_status_t *status)
{
    return status &&
           status->manager_running &&
           text_equals(status->manager_state, "NO_CARD") &&
           text_equals(status->sd_state, "no_card") &&
           status->rp2040_bridge_ready &&
           status->rp2040_sd_protocol_supported &&
           !status->sd_present &&
           !status->sd_presence_stale &&
           !status->sd_mounted &&
           !status->sd_data_root_ready &&
           !status->bridge_status_stale &&
           text_equals(status->data_backend, "volatile") &&
           text_equals(status->message_store_backend, "volatile") &&
           text_equals(status->dm_store_backend, "volatile") &&
           text_equals(status->route_store_backend, "volatile") &&
           text_equals(status->packet_log_backend, "volatile") &&
           text_equals(status->node_store_backend, "volatile") &&
           text_equals(status->contact_store_backend, "volatile") &&
           text_equals(status->read_state_backend, "volatile");
}

static bool retained_canary_backend_generations(
    bool expect_sd,
    uint32_t generations[D1L_RETAINED_BLOB_STORE_COUNT])
{
    if (!generations) {
        return false;
    }
    for (size_t i = 0; i < D1L_RETAINED_BLOB_STORE_COUNT; ++i) {
        d1l_retained_blob_store_backend_state_t state = {0};
        if (!d1l_retained_blob_store_backend_state(
                (d1l_retained_blob_store_id_t)i, &state) ||
            state.enabled != expect_sd || state.generation == 0U) {
            return false;
        }
        generations[i] = state.generation;
    }
    return true;
}
#endif

static bool copy_storage_token(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0 || !src) {
        return false;
    }
    while (*src == ' ') {
        src++;
    }
    size_t out = 0;
    while (*src && out + 1U < dest_size) {
        const unsigned char ch = (unsigned char)*src++;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            dest[out++] = (char)ch;
        } else if (isspace(ch)) {
            break;
        } else {
            return false;
        }
    }
    dest[out] = '\0';
    return out > 0 && *src == '\0';
}

static bool read_storage_word(const char **cursor, char *dest, size_t dest_size)
{
    if (!cursor || !*cursor || !dest || dest_size == 0) {
        return false;
    }
    while (**cursor == ' ') {
        (*cursor)++;
    }
    size_t out = 0;
    while (**cursor != '\0' && **cursor != ' ' && out + 1U < dest_size) {
        const unsigned char ch = (unsigned char)**cursor;
        if (ch < 32 || ch > 126 || ch == '"' || ch == '\\') {
            return false;
        }
        dest[out++] = (char)ch;
        (*cursor)++;
    }
    dest[out] = '\0';
    while (**cursor == ' ') {
        (*cursor)++;
    }
    return out > 0;
}

static bool parse_u32_word(const char *word, uint32_t *out_value)
{
    if (!word || !out_value || word[0] == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(word, &end, 10);
    if (!end || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

static bool parse_next_u32_arg(const char **cursor, uint32_t *out_value)
{
    if (!cursor || !*cursor || !out_value) {
        return false;
    }
    while (**cursor == ' ') {
        (*cursor)++;
    }
    if (**cursor == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(*cursor, &end, 10);
    if (end == *cursor || !end || (*end != '\0' && *end != ' ') || value > UINT32_MAX) {
        return false;
    }
    *out_value = (uint32_t)value;
    *cursor = end;
    while (**cursor == ' ') {
        (*cursor)++;
    }
    return true;
}

static bool parse_channel_id_hex(const char *text, uint64_t *out_value)
{
    if (!text || !out_value) {
        return false;
    }
    const size_t length = strlen(text);
    if (length == 0U || length > 16U) {
        return false;
    }
    uint64_t value = 0U;
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char ch = (unsigned char)text[index];
        uint8_t digit;
        if (ch >= '0' && ch <= '9') {
            digit = (uint8_t)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = (uint8_t)(ch - 'a' + 10U);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = (uint8_t)(ch - 'A' + 10U);
        } else {
            return false;
        }
        value = (value << 4U) | digit;
    }
    if (value == 0U) {
        return false;
    }
    *out_value = value;
    return true;
}

static bool parse_message_offset_clause(const char *arg, size_t *out_offset)
{
    if (!arg || !out_offset) {
        return false;
    }
    while (*arg == ' ') {
        arg++;
    }
    if (strncmp(arg, "offset", 6) != 0 ||
        (arg[6] != ' ' && arg[6] != '\0')) {
        return false;
    }
    arg += 6;
    while (*arg == ' ') {
        arg++;
    }
    if (*arg == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(arg, &end, 10);
    if (end == arg) {
        return false;
    }
    while (end && *end == ' ') {
        end++;
    }
    if (!end || *end != '\0') {
        return false;
    }
    *out_offset = (size_t)value;
    return true;
}

static bool strip_message_offset_suffix(char *text, size_t *out_offset)
{
    if (!text || !out_offset) {
        return false;
    }
    char *candidate = NULL;
    for (char *p = strstr(text, " offset "); p; p = strstr(p + 1, " offset ")) {
        candidate = p;
    }
    if (!candidate) {
        return true;
    }
    if (!parse_message_offset_clause(candidate + 1, out_offset)) {
        return false;
    }
    *candidate = '\0';
    trim_line(text);
    return true;
}

static void print_storage_export_error(const char *cmd,
                                       const char *step,
                                       esp_err_t ret,
                                       const d1l_storage_status_t *status,
                                       const d1l_export_canary_result_t *canary,
                                       const char *hint)
{
    printf("{\"schema\":%d,\"ok\":false,\"cmd\":", D1L_CONSOLE_SCHEMA);
    print_json_string(cmd ? cmd : "storage export");
    printf(",\"code\":\"%s\",\"step\":", esp_err_to_name(ret));
    print_json_string(step ? step : "unknown");
    printf(",\"sd_state\":");
    print_json_string(status && status->sd_state ? status->sd_state : "unknown");
    printf(",\"export_backend\":");
    print_json_string(status && status->export_backend ? status->export_backend : "serial");
    printf(",\"path\":");
    print_json_string(canary ? canary->path : "");
    printf(",\"tmp_path\":");
    print_json_string(canary ? canary->tmp_path : "");
    printf(",\"file_op\":");
    print_json_string(canary && canary->file.op[0] ? canary->file.op : "");
    printf(",\"file_error\":");
    print_json_string(canary && canary->file.err[0] ? canary->file.err : "");
    printf(",\"public_rf_tx\":false,\"formats_sd\":false,\"hint\":");
    print_json_string(hint ? hint : "requires a ready RP2040 SD bridge with file_ops and atomic rename; no Public RF or format command was used");
    printf("}\n");
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static void print_storage_map_tile_error(const char *cmd,
                                         const char *step,
                                         esp_err_t ret,
                                         const d1l_storage_status_t *status,
                                         const d1l_map_tile_canary_result_t *canary,
                                         const char *hint)
{
    printf("{\"schema\":%d,\"ok\":false,\"cmd\":", D1L_CONSOLE_SCHEMA);
    print_json_string(cmd ? cmd : "storage map-tile-canary");
    printf(",\"code\":\"%s\",\"step\":", esp_err_to_name(ret));
    print_json_string(step ? step : "unknown");
    printf(",\"sd_state\":");
    print_json_string(status && status->sd_state ? status->sd_state : "unknown");
    printf(",\"map_tile_backend\":");
    print_json_string(status && status->map_tile_backend ? status->map_tile_backend : "unavailable");
    printf(",\"path\":");
    print_json_string(canary ? canary->path : "");
    printf(",\"tmp_path\":");
    print_json_string(canary ? canary->tmp_path : "");
    printf(",\"file_op\":");
    print_json_string(canary && canary->file.op[0] ? canary->file.op : "");
    printf(",\"file_error\":");
    print_json_string(canary && canary->file.err[0] ? canary->file.err : "");
    printf(",\"public_rf_tx\":false,\"formats_sd\":false,\"hint\":");
    print_json_string(hint ? hint : "requires a ready RP2040 SD bridge with file_ops and atomic rename; no Public RF or format command was used");
    printf("}\n");
}

static void cmd_storage_map_tile_canary(const char *line)
{
    static const char prefix[] = "storage map-tile-canary ";
    char token[D1L_MAP_TILE_CANARY_TOKEN_MAX + 1U] = {0};
    if (strncmp(line, prefix, strlen(prefix)) != 0 ||
        !copy_storage_token(token, sizeof(token), line + strlen(prefix))) {
        err_result("storage map-tile-canary", "INVALID_TOKEN",
                   "usage: storage map-tile-canary <token-with-a-z-0-9-dot-dash-underscore>");
        return;
    }

    (void)d1l_storage_status_refresh(D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS);
    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);

    d1l_map_tile_canary_result_t canary = {0};
    esp_err_t ret = d1l_map_tile_store_write_canary(token, &status, &canary);
    if (ret != ESP_OK) {
        print_storage_map_tile_error(
            "storage map-tile-canary", canary.step, ret, &status, &canary,
            "requires a ready RP2040 SD bridge with file_ops and atomic rename; no Public RF or format command was used");
        return;
    }

    d1l_storage_status(&status);
    ok_begin("storage map-tile-canary");
    printf(",\"token\":");
    print_json_string(canary.token);
    printf(",\"path\":");
    print_json_string(canary.path);
    printf(",\"tmp_path\":");
    print_json_string(canary.tmp_path);
    printf(",\"z\":%u,\"x\":%lu,\"y\":%lu,\"bytes\":%u,\"write_tmp\":%s,\"read_tmp\":%s,\"rename_replace\":%s,\"stat_final\":%s,\"read_final\":%s,\"public_rf_tx\":false,\"formats_sd\":false,\"map_tile_backend\":",
           (unsigned)canary.z,
           (unsigned long)canary.x,
           (unsigned long)canary.y,
           (unsigned)canary.bytes,
           bool_json(canary.write_tmp),
           bool_json(canary.read_tmp),
           bool_json(canary.rename_replace),
           bool_json(canary.stat_final),
           bool_json(canary.read_final));
    print_json_string(status.map_tile_backend ? status.map_tile_backend : "unavailable");
    printf(",\"note\":\"Map tile SD cache canary committed by temp write and atomic rename; no Public RF or format command was used\"}\n");
}

static void cmd_storage_map_tile_check(const char *line)
{
    static const char prefix[] = "storage map-tile-check ";
    char token[D1L_MAP_TILE_CANARY_TOKEN_MAX + 1U] = {0};
    if (strncmp(line, prefix, strlen(prefix)) != 0 ||
        !copy_storage_token(token, sizeof(token), line + strlen(prefix))) {
        err_result("storage map-tile-check", "INVALID_TOKEN",
                   "usage: storage map-tile-check <token-with-a-z-0-9-dot-dash-underscore>");
        return;
    }

    (void)d1l_storage_status_refresh(D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS);
    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);

    d1l_map_tile_canary_result_t canary = {0};
    esp_err_t ret = d1l_map_tile_store_check_canary(token, &status, &canary);
    if (ret != ESP_OK) {
        print_storage_map_tile_error(
            "storage map-tile-check", canary.step, ret, &status, &canary,
            "requires the previous map tile canary to survive SD reboot/remount; no Public RF or format command was used");
        return;
    }

    d1l_storage_status(&status);
    ok_begin("storage map-tile-check");
    printf(",\"token\":");
    print_json_string(canary.token);
    printf(",\"path\":");
    print_json_string(canary.path);
    printf(",\"z\":%u,\"x\":%lu,\"y\":%lu,\"bytes\":%u,\"stat_final\":%s,\"read_final\":%s,\"public_rf_tx\":false,\"formats_sd\":false,\"map_tile_backend\":",
           (unsigned)canary.z,
           (unsigned long)canary.x,
           (unsigned long)canary.y,
           (unsigned)canary.bytes,
           bool_json(canary.stat_final),
           bool_json(canary.read_final));
    print_json_string(status.map_tile_backend ? status.map_tile_backend : "unavailable");
    printf(",\"note\":\"Map tile SD cache canary verified read-only after remount; no Public RF or format command was used\"}\n");
}

static void cmd_storage_export_canary(const char *line)
{
    static const char prefix[] = "storage export-canary ";
    char token[D1L_EXPORT_CANARY_TOKEN_MAX + 1U] = {0};
    if (strncmp(line, prefix, strlen(prefix)) != 0 ||
        !copy_storage_token(token, sizeof(token), line + strlen(prefix))) {
        err_result("storage export-canary", "INVALID_TOKEN",
                   "usage: storage export-canary <token-with-a-z-0-9-dot-dash-underscore>");
        return;
    }

    (void)d1l_storage_status_refresh(D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS);
    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);

    d1l_export_canary_result_t canary = {0};
    esp_err_t ret = d1l_export_store_write_canary(token, &status, &canary);
    if (ret != ESP_OK) {
        print_storage_export_error("storage export-canary", canary.step, ret, &status,
                                   &canary,
                                   "requires a ready RP2040 SD bridge with file_ops and atomic rename; no Public RF or format command was used");
        return;
    }

    d1l_storage_status(&status);
    ok_begin("storage export-canary");
    printf(",\"token\":");
    print_json_string(canary.token);
    printf(",\"path\":");
    print_json_string(canary.path);
    printf(",\"tmp_path\":");
    print_json_string(canary.tmp_path);
    printf(",\"bytes\":%u,\"write_tmp\":%s,\"read_tmp\":%s,\"rename_replace\":%s,\"stat_final\":%s,\"read_final\":%s,\"public_rf_tx\":false,\"formats_sd\":false,\"export_backend\":",
           (unsigned)canary.bytes,
           bool_json(canary.write_tmp),
           bool_json(canary.read_tmp),
           bool_json(canary.rename_replace),
           bool_json(canary.stat_final),
           bool_json(canary.read_final));
    print_json_string(status.export_backend ? status.export_backend : "serial");
    printf(",\"note\":\"Diagnostic export SD canary committed by temp write and atomic rename; no Public RF or format command was used\"}\n");
}
#endif

static bool append_export_json(char *dest, size_t dest_size, size_t *used,
                               const char *fmt, ...)
{
    if (!dest || !used || !fmt || *used >= dest_size) {
        return false;
    }
    va_list args;
    va_start(args, fmt);
    const int ret = vsnprintf(dest + *used, dest_size - *used, fmt, args);
    va_end(args);
    if (ret < 0 || (size_t)ret >= dest_size - *used) {
        return false;
    }
    *used += (size_t)ret;
    return true;
}

static bool append_export_json_string(char *dest, size_t dest_size, size_t *used,
                                      const char *text)
{
    if (!dest || !used || *used >= dest_size) {
        return false;
    }
    if (!append_export_json(dest, dest_size, used, "\"")) {
        return false;
    }
    if (text) {
        for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
            switch (*p) {
            case '"':
                if (!append_export_json(dest, dest_size, used, "\\\"")) {
                    return false;
                }
                break;
            case '\\':
                if (!append_export_json(dest, dest_size, used, "\\\\")) {
                    return false;
                }
                break;
            case '\b':
                if (!append_export_json(dest, dest_size, used, "\\b")) {
                    return false;
                }
                break;
            case '\f':
                if (!append_export_json(dest, dest_size, used, "\\f")) {
                    return false;
                }
                break;
            case '\n':
                if (!append_export_json(dest, dest_size, used, "\\n")) {
                    return false;
                }
                break;
            case '\r':
                if (!append_export_json(dest, dest_size, used, "\\r")) {
                    return false;
                }
                break;
            case '\t':
                if (!append_export_json(dest, dest_size, used, "\\t")) {
                    return false;
                }
                break;
            default:
                if (*p < 0x20U) {
                    if (!append_export_json(dest, dest_size, used, "\\u%04x",
                                            (unsigned)*p)) {
                        return false;
                    }
                } else {
                    if (!append_export_json(dest, dest_size, used, "%c", (int)*p)) {
                        return false;
                    }
                }
                break;
            }
        }
    }
    return append_export_json(dest, dest_size, used, "\"");
}

static bool append_export_json_string_field(char *dest, size_t dest_size, size_t *used,
                                            const char *key, const char *value)
{
    return append_export_json(dest, dest_size, used, "\"%s\":", key) &&
           append_export_json_string(dest, dest_size, used, value);
}

static bool build_diagnostic_export_payload(const char *token,
                                            const d1l_storage_status_t *status,
                                            char *dest,
                                            size_t dest_size,
                                            size_t *out_len)
{
    if (!token || !status || !dest || !out_len || dest_size == 0) {
        return false;
    }
    size_t used = 0;
    d1l_health_snapshot_t h = d1l_health_snapshot();
    d1l_crash_log_stats_t crash_stats = d1l_crash_log_stats();
    d1l_crash_log_entry_t crash_entries[D1L_CRASH_LOG_CAPACITY];
    const size_t copied = d1l_crash_log_copy_recent(crash_entries, D1L_CRASH_LOG_CAPACITY);

    if (!append_export_json(dest, dest_size, &used,
                            "{\"schema\":1,\"kind\":\"diagnostic_export\",\"token\":\"%s\","
                            "\"public_rf_tx\":false,\"formats_sd\":false,"
                            "\"storage\":{\"sd_state\":\"%s\",\"data_backend\":\"%s\","
                            "\"export_backend\":\"%s\",\"message_store_backend\":\"%s\","
                            "\"dm_store_backend\":\"%s\",\"route_store_backend\":\"%s\","
                            "\"packet_log_backend\":\"%s\",\"node_store_backend\":\"%s\","
                            "\"contact_store_backend\":\"%s\",\"read_state_backend\":\"%s\","
                            "\"map_tile_backend\":\"%s\","
                            "\"file_ops\":%s,\"atomic_rename\":%s},",
                            token,
                            status->sd_state ? status->sd_state : "unknown",
                            status->data_backend ? status->data_backend : "volatile",
                            status->export_backend ? status->export_backend : "serial",
                            status->message_store_backend ? status->message_store_backend : "volatile",
                            status->dm_store_backend ? status->dm_store_backend : "volatile",
                            status->route_store_backend ? status->route_store_backend : "volatile",
                            status->packet_log_backend ? status->packet_log_backend : "volatile",
                            status->node_store_backend ?
                                status->node_store_backend : "volatile",
                            status->contact_store_backend ?
                                status->contact_store_backend : "volatile",
                            status->read_state_backend ?
                                status->read_state_backend : "volatile",
                            status->map_tile_backend ? status->map_tile_backend : "unavailable",
                            bool_json(status->file_ops_supported),
                            bool_json(status->atomic_rename_supported))) {
        return false;
    }
    if (!append_export_json(dest, dest_size, &used,
                            "\"health\":{\"boot_nonce\":%lu,\"uptime_ms\":%lu,\"heap_free\":%lu,"
                            "\"heap_min_free\":%lu,\"heap_largest_free\":%lu,"
                            "\"internal_heap_free\":%lu,\"internal_heap_min_free\":%lu,"
                            "\"internal_heap_largest_free\":%lu,"
                            "\"dma_heap_free\":%lu,\"dma_heap_largest_free\":%lu,"
                            "\"psram_free\":%lu,\"psram_min_free\":%lu,"
                            "\"psram_largest_free\":%lu,"
                            "\"current_task_stack_free_words\":%lu,"
                            "\"ui_task_stack_free_words\":%lu,"
                            "\"retained_task_stack_free_bytes\":%lu,"
                            "\"lvgl_free_bytes\":%lu,\"lvgl_largest_free_bytes\":%lu,"
                            "\"lvgl_used_pct\":%u,\"nvs_ready\":%s,\"nvs_error\":\"%s\",\"reset_reason\":\"%s\"},",
                            (unsigned long)h.boot_nonce,
                            (unsigned long)h.uptime_ms,
                            (unsigned long)h.heap_free,
                            (unsigned long)h.heap_min_free,
                            (unsigned long)h.heap_largest_free,
                            (unsigned long)h.internal_heap_free,
                            (unsigned long)h.internal_heap_min_free,
                            (unsigned long)h.internal_heap_largest_free,
                            (unsigned long)h.dma_heap_free,
                            (unsigned long)h.dma_heap_largest_free,
                            (unsigned long)h.psram_free,
                            (unsigned long)h.psram_min_free,
                            (unsigned long)h.psram_largest_free,
                            (unsigned long)h.current_task_stack_free_words,
                            (unsigned long)h.ui_task_stack_free_words,
                            (unsigned long)h.retained_task_stack_free_bytes,
                            (unsigned long)h.lvgl_free_bytes,
                            (unsigned long)h.lvgl_largest_free_bytes,
                            h.lvgl_used_pct,
                            bool_json(h.nvs_ready),
                            esp_err_to_name(h.nvs_error),
                            h.reset_reason ? h.reset_reason : "UNKNOWN")) {
        return false;
    }
    const bool map_tile_cache_ready = d1l_map_tile_store_sd_ready(status);
    if (!append_export_json(dest, dest_size, &used,
                            "\"limits\":{\"payload_max\":%u,\"file_chunk_max\":%u,"
                            "\"crashlog_capacity\":%u,\"sampled\":true},"
                            "\"map_tiles\":{\"exported\":false,\"cache_ready\":%s,"
                            "\"backend\":\"%s\",\"reason\":\"%s\"},"
                            "\"crashlog\":{\"count\":%u,\"capacity\":%u,"
                            "\"total_written\":%lu,\"dropped_oldest\":%lu,\"entries\":[",
                            (unsigned)D1L_EXPORT_DIAGNOSTIC_PAYLOAD_MAX,
                            (unsigned)D1L_RP2040_FILE_CHUNK_MAX,
                            (unsigned)D1L_CRASH_LOG_CAPACITY,
                            bool_json(map_tile_cache_ready),
                            status->map_tile_backend ? status->map_tile_backend : "unavailable",
                            map_tile_cache_ready ? "diagnostic_export_does_not_bundle_map_tiles" : "pending",
                            (unsigned)crash_stats.count,
                            (unsigned)crash_stats.capacity,
                            (unsigned long)crash_stats.total_written,
                            (unsigned long)crash_stats.dropped_oldest)) {
        return false;
    }
    for (size_t i = 0; i < copied; ++i) {
        const d1l_crash_log_entry_t *e = &crash_entries[i];
        if (!append_export_json(dest, dest_size, &used,
                                "%s{\"seq\":%lu,\"uptime_ms\":%lu,"
                                "\"reset_reason\":\"%s\",\"reset_reason_code\":%u,"
                                "\"crash_like\":%s,\"heap_free\":%lu,"
                                "\"heap_min_free\":%lu,\"psram_free\":%lu}",
                                i ? "," : "",
                                (unsigned long)e->seq,
                                (unsigned long)e->uptime_ms,
                                e->reset_reason,
                                e->reset_reason_code,
                                bool_json(e->crash_like),
                                (unsigned long)e->heap_free,
                                (unsigned long)e->heap_min_free,
                                (unsigned long)e->psram_free)) {
            return false;
        }
    }
    if (!append_export_json(dest, dest_size, &used, "]}}\n")) {
        return false;
    }
    *out_len = used;
    return true;
}

#define D1L_EXPORT_DATA_SAMPLE_MAX 4U

static bool append_data_export_message_entries(char *dest, size_t dest_size, size_t *used,
                                               const d1l_message_entry_t *entries,
                                               size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const d1l_message_entry_t *e = &entries[i];
        if (!append_export_json(dest, dest_size, used,
                                "%s{\"seq\":%lu,\"uptime_ms\":%lu,",
                                i ? "," : "",
                                (unsigned long)e->seq,
                                (unsigned long)e->uptime_ms) ||
            !append_export_json_string_field(dest, dest_size, used, "direction", e->direction) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "author", e->author) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "text", e->text) ||
            !append_export_json(dest, dest_size, used,
                                ",\"rssi_dbm\":%d,\"snr_tenths\":%d,"
                                "\"path_hash_bytes\":%u,\"path_hops\":%u,"
                                "\"delivered\":%s}",
                                e->rssi_dbm,
                                e->snr_tenths,
                                (unsigned)e->path_hash_bytes,
                                (unsigned)e->path_hops,
                                bool_json(e->delivered))) {
            return false;
        }
    }
    return true;
}

static bool append_data_export_dm_entries(char *dest, size_t dest_size, size_t *used,
                                          const d1l_dm_entry_t *entries,
                                          size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const d1l_dm_entry_t *e = &entries[i];
        if (!append_export_json(dest, dest_size, used,
                                "%s{\"seq\":%lu,\"uptime_ms\":%lu,",
                                i ? "," : "",
                                (unsigned long)e->seq,
                                (unsigned long)e->uptime_ms) ||
            !append_export_json_string_field(dest, dest_size, used, "contact_fingerprint", e->contact_fingerprint) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "contact_alias", e->contact_alias) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "direction", e->direction) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "text", e->text) ||
            !append_export_json(dest, dest_size, used,
                                ",\"rssi_dbm\":%d,\"snr_tenths\":%d,"
                                "\"path_hash_bytes\":%u,\"path_hops\":%u,"
                                "\"attempt\":%u,\"delivered\":%s,\"acked\":%s,"
                                "\"ack_hash\":%lu}",
                                e->rssi_dbm,
                                e->snr_tenths,
                                (unsigned)e->path_hash_bytes,
                                (unsigned)e->path_hops,
                                (unsigned)e->attempt,
                                bool_json(e->delivered),
                                bool_json(e->acked),
                                (unsigned long)e->ack_hash)) {
            return false;
        }
    }
    return true;
}

static bool append_data_export_route_entries(char *dest, size_t dest_size, size_t *used,
                                             const d1l_route_entry_t *entries,
                                             size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const d1l_route_entry_t *e = &entries[i];
        if (!append_export_json(dest, dest_size, used,
                                "%s{\"seq\":%lu,\"first_seen_ms\":%lu,"
                                "\"last_seen_ms\":%lu,\"seen_count\":%lu,",
                                i ? "," : "",
                                (unsigned long)e->seq,
                                (unsigned long)e->first_seen_ms,
                                (unsigned long)e->last_seen_ms,
                                (unsigned long)e->seen_count) ||
            !append_export_json_string_field(dest, dest_size, used, "target", e->target) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "label", e->label) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "kind", e->kind) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "route", e->route) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "direction", e->direction) ||
            !append_export_json(dest, dest_size, used,
                                ",\"last_rssi_dbm\":%d,\"last_snr_tenths\":%d,"
                                "\"path_hash_bytes\":%u,\"path_hops\":%u,"
                                "\"confidence\":%u,\"payload_len\":%u}",
                                e->last_rssi_dbm,
                                e->last_snr_tenths,
                                (unsigned)e->path_hash_bytes,
                                (unsigned)e->path_hops,
                                (unsigned)e->confidence,
                                (unsigned)e->payload_len)) {
            return false;
        }
    }
    return true;
}

static bool append_data_export_packet_entries(char *dest, size_t dest_size, size_t *used,
                                              const d1l_packet_log_entry_t *entries,
                                              size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const d1l_packet_log_entry_t *e = &entries[i];
        if (!append_export_json(dest, dest_size, used,
                                "%s{\"seq\":%lu,\"uptime_ms\":%lu,",
                                i ? "," : "",
                                (unsigned long)e->seq,
                                (unsigned long)e->uptime_ms) ||
            !append_export_json_string_field(dest, dest_size, used, "direction", e->direction) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "kind", e->kind) ||
            !append_export_json(dest, dest_size, used,
                                ",\"rssi_dbm\":%d,\"snr_tenths\":%d,"
                                "\"path_hash_bytes\":%u,\"path_hops\":%u,"
                                "\"payload_len\":%u,\"raw_len\":%u,"
                                "\"raw_truncated\":%s,",
                                e->rssi_dbm,
                                e->snr_tenths,
                                (unsigned)e->path_hash_bytes,
                                (unsigned)e->path_hops,
                                (unsigned)e->payload_len,
                                (unsigned)e->raw_len,
                                bool_json(e->raw_truncated)) ||
            !append_export_json_string_field(dest, dest_size, used, "note", e->note) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "raw_hex", e->raw_hex) ||
            !append_export_json(dest, dest_size, used, "}")) {
            return false;
        }
    }
    return true;
}

static bool append_data_export_contact_entries(char *dest, size_t dest_size, size_t *used,
                                               const d1l_contact_entry_t *entries,
                                               size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const d1l_contact_entry_t *e = &entries[i];
        if (!append_export_json(dest, dest_size, used,
                                "%s{\"seq\":%lu,\"created_ms\":%lu,"
                                "\"updated_ms\":%lu,\"out_path_updated_ms\":%lu,",
                                i ? "," : "",
                                (unsigned long)e->seq,
                                (unsigned long)e->created_ms,
                                (unsigned long)e->updated_ms,
                                (unsigned long)e->out_path_updated_ms) ||
            !append_export_json_string_field(dest, dest_size, used, "fingerprint", e->fingerprint) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "public_key_hex", e->public_key_hex) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "alias", e->alias) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "heard_name", e->heard_name) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "type", e->type) ||
            !append_export_json(dest, dest_size, used,
                                ",\"last_rssi_dbm\":%d,\"last_snr_tenths\":%d,"
                                "\"path_hash_bytes\":%u,\"path_hops\":%u,"
                                "\"out_path_valid\":%s,\"out_path_len\":%u,"
                                "\"favorite\":%s,\"muted\":%s}",
                                e->last_rssi_dbm,
                                e->last_snr_tenths,
                                (unsigned)e->path_hash_bytes,
                                (unsigned)e->path_hops,
                                bool_json(e->out_path_valid),
                                (unsigned)e->out_path_len,
                                bool_json(e->favorite),
                                bool_json(e->muted))) {
            return false;
        }
    }
    return true;
}

static bool append_data_export_node_entries(char *dest, size_t dest_size, size_t *used,
                                            const d1l_node_entry_t *entries,
                                            size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const d1l_node_entry_t *e = &entries[i];
        if (!append_export_json(dest, dest_size, used,
                                "%s{\"seq\":%lu,\"first_heard_ms\":%lu,"
                                "\"last_heard_ms\":%lu,\"advert_timestamp\":%lu,"
                                "\"heard_count\":%lu,",
                                i ? "," : "",
                                (unsigned long)e->seq,
                                (unsigned long)e->first_heard_ms,
                                (unsigned long)e->last_heard_ms,
                                (unsigned long)e->advert_timestamp,
                                (unsigned long)e->heard_count) ||
            !append_export_json_string_field(dest, dest_size, used, "fingerprint", e->fingerprint) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "public_key_hex", e->public_key_hex) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "name", e->name) ||
            !append_export_json(dest, dest_size, used, ",") ||
            !append_export_json_string_field(dest, dest_size, used, "type", e->type) ||
            !append_export_json(dest, dest_size, used,
                                ",\"rssi_dbm\":%d,\"snr_tenths\":%d,"
                                "\"path_hash_bytes\":%u,\"path_hops\":%u,"
                                "\"location_valid\":%s,\"lat_e6\":%ld,"
                                "\"lon_e6\":%ld,\"location_advert_timestamp\":%lu,"
                                "\"location_seq\":%lu}",
                                e->rssi_dbm,
                                e->snr_tenths,
                                (unsigned)e->path_hash_bytes,
                                (unsigned)e->path_hops,
                                bool_json(e->location_valid),
                                (long)e->lat_e6,
                                (long)e->lon_e6,
                                (unsigned long)e->location_advert_timestamp,
                                (unsigned long)e->location_seq)) {
            return false;
        }
    }
    return true;
}

static bool append_data_export_read_threads(char *dest, size_t dest_size, size_t *used,
                                            const d1l_read_state_dm_thread_t *threads,
                                            size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const d1l_read_state_dm_thread_t *t = &threads[i];
        if (!append_export_json(dest, dest_size, used,
                                "%s{\"last_read_seq\":%lu,\"newest_rx_seq\":%lu,"
                                "\"unread_count\":%lu,\"muted\":%s,",
                                i ? "," : "",
                                (unsigned long)t->last_read_seq,
                                (unsigned long)t->newest_rx_seq,
                                (unsigned long)t->unread_count,
                                bool_json(t->muted)) ||
            !append_export_json_string_field(dest, dest_size, used, "fingerprint", t->fingerprint) ||
            !append_export_json(dest, dest_size, used, "}")) {
            return false;
        }
    }
    return true;
}

static bool build_data_export_payload(const char *token,
                                      const d1l_storage_status_t *status,
                                      char *dest,
                                      size_t dest_size,
                                      size_t *out_len)
{
    if (!token || !status || !dest || !out_len || dest_size == 0) {
        return false;
    }

    static d1l_message_entry_t
        messages[D1L_EXPORT_DATA_SAMPLE_MAX] EXT_RAM_BSS_ATTR;
    static d1l_dm_entry_t
        dms[D1L_EXPORT_DATA_SAMPLE_MAX] EXT_RAM_BSS_ATTR;
    static d1l_route_entry_t
        routes[D1L_EXPORT_DATA_SAMPLE_MAX] EXT_RAM_BSS_ATTR;
    static d1l_packet_log_entry_t
        packets[D1L_EXPORT_DATA_SAMPLE_MAX] EXT_RAM_BSS_ATTR;
    static d1l_contact_entry_t
        contacts[D1L_EXPORT_DATA_SAMPLE_MAX] EXT_RAM_BSS_ATTR;
    static d1l_node_entry_t
        nodes[D1L_EXPORT_DATA_SAMPLE_MAX] EXT_RAM_BSS_ATTR;
    static d1l_read_state_dm_thread_t
        read_threads[D1L_EXPORT_DATA_SAMPLE_MAX] EXT_RAM_BSS_ATTR;

    d1l_settings_t settings_snapshot = {0};
    (void)d1l_settings_public_snapshot(&settings_snapshot);
    const d1l_settings_t *settings = &settings_snapshot;
    const d1l_message_store_stats_t message_stats = d1l_message_store_stats();
    const d1l_dm_store_stats_t dm_stats = d1l_dm_store_stats();
    const d1l_route_store_stats_t route_stats = d1l_route_store_stats();
    const d1l_packet_log_stats_t packet_stats = d1l_packet_log_stats();
    const d1l_contact_store_stats_t contact_stats = d1l_contact_store_stats();
    const d1l_node_store_stats_t node_stats = d1l_node_store_stats();
    const d1l_read_state_stats_t read_stats = d1l_read_state_stats();
    const size_t message_count = d1l_message_store_copy_recent(messages, D1L_EXPORT_DATA_SAMPLE_MAX);
    const size_t dm_count = d1l_dm_store_copy_recent(dms, D1L_EXPORT_DATA_SAMPLE_MAX);
    const size_t route_count = d1l_route_store_copy_recent(routes, D1L_EXPORT_DATA_SAMPLE_MAX);
    const size_t packet_count = d1l_packet_log_copy_recent(packets, D1L_EXPORT_DATA_SAMPLE_MAX);
    const size_t contact_count = d1l_contact_store_copy_recent(contacts, D1L_EXPORT_DATA_SAMPLE_MAX);
    const size_t node_count = d1l_node_store_copy_recent(nodes, D1L_EXPORT_DATA_SAMPLE_MAX);
    const size_t read_thread_count = d1l_read_state_copy_dm_threads(read_threads,
                                                                    D1L_EXPORT_DATA_SAMPLE_MAX);
    size_t used = 0;

    if (!append_export_json(dest, dest_size, &used,
                            "{\"schema\":1,\"kind\":\"data_export\",\"token\":\"%s\","
                            "\"sampled\":true,\"public_rf_tx\":false,\"formats_sd\":false,"
                            "\"private_identity_exported\":false,"
                            "\"storage\":{\"sd_state\":\"%s\",\"data_backend\":\"%s\","
                            "\"export_backend\":\"%s\",\"stores\":{\"messages\":\"%s\","
                            "\"dm\":\"%s\",\"routes\":\"%s\",\"packets\":\"%s\","
                            "\"contacts\":\"%s\",\"nodes\":\"%s\","
                            "\"read_state\":\"%s\",\"map_tiles\":\"%s\"},"
                            "\"file_ops\":%s,\"atomic_rename\":%s},"
                            "\"limits\":{\"payload_max\":%u,\"sample_max\":%u},",
                            token,
                            status->sd_state ? status->sd_state : "unknown",
                            status->data_backend ? status->data_backend : "volatile",
                            status->export_backend ? status->export_backend : "serial",
                            status->message_store_backend ? status->message_store_backend : "volatile",
                            status->dm_store_backend ? status->dm_store_backend : "volatile",
                            status->route_store_backend ? status->route_store_backend : "volatile",
                            status->packet_log_backend ? status->packet_log_backend : "volatile",
                            status->contact_store_backend ?
                                status->contact_store_backend : "volatile",
                            status->node_store_backend ?
                                status->node_store_backend : "volatile",
                            status->read_state_backend ?
                                status->read_state_backend : "volatile",
                            status->map_tile_backend ? status->map_tile_backend : "unavailable",
                            bool_json(status->file_ops_supported),
                            bool_json(status->atomic_rename_supported),
                            (unsigned)D1L_EXPORT_DATA_PAYLOAD_MAX,
                            (unsigned)D1L_EXPORT_DATA_SAMPLE_MAX)) {
        return false;
    }

    if (!append_export_json(dest, dest_size, &used,
                            "\"settings\":{\"role\":\"%s\",\"onboarding_complete\":%s,"
                            "\"region\":\"Canada/USA\",\"radio\":{\"frequency_hz\":%lu,"
                            "\"bandwidth_khz\":%.1f,\"sf\":%u,\"cr\":%u,"
                            "\"tx_power_dbm\":%d,\"rx_boost\":%s,\"tcxo\":\"%s\"},"
                            "\"map_location\":{\"set\":%s,\"source\":\"%s\","
                            "\"lat_e7\":%ld,\"lon_e7\":%ld},",
                            d1l_settings_role_name(settings->role),
                            bool_json(settings->onboarding_complete),
                            (unsigned long)settings->frequency_hz,
                            ((float)settings->bandwidth_tenths_khz) / 10.0f,
                            settings->spreading_factor,
                            settings->coding_rate,
                            settings->tx_power_dbm,
                            bool_json(settings->rx_boost),
                            d1l_settings_tcxo_name(settings->tcxo_mode),
                            bool_json(settings->map_location_set),
                            map_location_source_name(settings),
                            (long)(settings->map_location_set ? settings->map_lat_e7 : 0),
                            (long)(settings->map_location_set ? settings->map_lon_e7 : 0)) ||
        !append_export_json_string_field(dest, dest_size, &used, "node_name", settings->node_name) ||
        !append_export_json(dest, dest_size, &used, "},")) {
        return false;
    }

    if (!append_export_json(dest, dest_size, &used,
                            "\"messages\":{\"count\":%u,\"capacity\":%u,"
                            "\"total_written\":%lu,\"dropped_oldest\":%lu,"
                            "\"history_counters_scope\":\"shared_all_channels\","
                            "\"entries\":[",
                            (unsigned)message_stats.public_count,
                            (unsigned)message_stats.capacity,
                            (unsigned long)message_stats.total_written,
                            (unsigned long)message_stats.dropped_oldest) ||
        !append_data_export_message_entries(dest, dest_size, &used, messages, message_count) ||
        !append_export_json(dest, dest_size, &used, "]},")) {
        return false;
    }

    if (!append_export_json(dest, dest_size, &used,
                            "\"dm\":{\"count\":%u,\"capacity\":%u,"
                            "\"total_written\":%lu,\"dropped_oldest\":%lu,"
                            "\"entries\":[",
                            (unsigned)dm_stats.count,
                            (unsigned)dm_stats.capacity,
                            (unsigned long)dm_stats.total_written,
                            (unsigned long)dm_stats.dropped_oldest) ||
        !append_data_export_dm_entries(dest, dest_size, &used, dms, dm_count) ||
        !append_export_json(dest, dest_size, &used, "]},")) {
        return false;
    }

    if (!append_export_json(dest, dest_size, &used,
                            "\"routes\":{\"count\":%u,\"capacity\":%u,"
                            "\"total_written\":%lu,\"dropped_oldest\":%lu,"
                            "\"entries\":[",
                            (unsigned)route_stats.count,
                            (unsigned)route_stats.capacity,
                            (unsigned long)route_stats.total_written,
                            (unsigned long)route_stats.dropped_oldest) ||
        !append_data_export_route_entries(dest, dest_size, &used, routes, route_count) ||
        !append_export_json(dest, dest_size, &used, "]},")) {
        return false;
    }

    if (!append_export_json(dest, dest_size, &used,
                            "\"packets\":{\"count\":%u,\"capacity\":%u,"
                            "\"total_written\":%lu,\"dropped_oldest\":%lu,"
                            "\"entries\":[",
                            (unsigned)packet_stats.count,
                            (unsigned)packet_stats.capacity,
                            (unsigned long)packet_stats.total_written,
                            (unsigned long)packet_stats.dropped_oldest) ||
        !append_data_export_packet_entries(dest, dest_size, &used, packets, packet_count) ||
        !append_export_json(dest, dest_size, &used, "]},")) {
        return false;
    }

    if (!append_export_json(dest, dest_size, &used,
                            "\"contacts\":{\"count\":%u,\"capacity\":%u,"
                            "\"total_written\":%lu,\"dropped_oldest\":%lu,"
                            "\"entries\":[",
                            (unsigned)contact_stats.count,
                            (unsigned)contact_stats.capacity,
                            (unsigned long)contact_stats.total_written,
                            (unsigned long)contact_stats.dropped_oldest) ||
        !append_data_export_contact_entries(dest, dest_size, &used, contacts, contact_count) ||
        !append_export_json(dest, dest_size, &used, "]},")) {
        return false;
    }

    if (!append_export_json(dest, dest_size, &used,
                            "\"nodes\":{\"count\":%u,\"capacity\":%u,"
                            "\"total_written\":%lu,\"dropped_oldest\":%lu,"
                            "\"entries\":[",
                            (unsigned)node_stats.count,
                            (unsigned)node_stats.capacity,
                            (unsigned long)node_stats.total_written,
                            (unsigned long)node_stats.dropped_oldest) ||
        !append_data_export_node_entries(dest, dest_size, &used, nodes, node_count) ||
        !append_export_json(dest, dest_size, &used, "]},")) {
        return false;
    }

    if (!append_export_json(dest, dest_size, &used,
                            "\"read_state\":{\"last_public_read_seq\":%lu,"
                            "\"last_dm_read_seq\":%lu,\"newest_public_rx_seq\":%lu,"
                            "\"newest_dm_rx_seq\":%lu,\"public_unread_count\":%lu,"
                            "\"dm_unread_count\":%lu,\"muted_dm_unread_count\":%lu,"
                            "\"dm_thread_count\":%lu,\"mark_read_count\":%lu,"
                            "\"threads\":[",
                            (unsigned long)read_stats.last_public_read_seq,
                            (unsigned long)read_stats.last_dm_read_seq,
                            (unsigned long)read_stats.newest_public_rx_seq,
                            (unsigned long)read_stats.newest_dm_rx_seq,
                            (unsigned long)read_stats.public_unread_count,
                            (unsigned long)read_stats.dm_unread_count,
                            (unsigned long)read_stats.muted_dm_unread_count,
                            (unsigned long)read_stats.dm_thread_count,
                            (unsigned long)read_stats.mark_read_count) ||
        !append_data_export_read_threads(dest, dest_size, &used, read_threads, read_thread_count) ||
        !append_export_json(dest, dest_size, &used, "]}}\n")) {
        return false;
    }

    *out_len = used;
    return true;
}

static void cmd_storage_export_diagnostics(const char *line)
{
    static const char prefix[] = "storage export-diagnostics ";
    char token[D1L_EXPORT_CANARY_TOKEN_MAX + 1U] = {0};
    if (strncmp(line, prefix, strlen(prefix)) != 0 ||
        !copy_storage_token(token, sizeof(token), line + strlen(prefix))) {
        err_result("storage export-diagnostics", "INVALID_TOKEN",
                   "usage: storage export-diagnostics <token-with-a-z-0-9-dot-dash-underscore>");
        return;
    }

    (void)d1l_storage_status_refresh(D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS);
    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);

    static char payload[D1L_EXPORT_DIAGNOSTIC_PAYLOAD_MAX] EXT_RAM_BSS_ATTR;
    size_t payload_len = 0;
    if (!build_diagnostic_export_payload(token, &status, payload, sizeof(payload),
                                         &payload_len)) {
        d1l_export_canary_result_t failed = {0};
        print_storage_export_error(
            "storage export-diagnostics", "payload", ESP_ERR_INVALID_SIZE,
            &status, &failed,
            "diagnostic export payload exceeded its bounded buffer; no SD write was attempted");
        return;
    }

    d1l_export_canary_result_t export_result = {0};
    esp_err_t ret = d1l_export_store_write_diagnostics(
        token, (const uint8_t *)payload, payload_len, &status, &export_result);
    if (ret != ESP_OK) {
        print_storage_export_error(
            "storage export-diagnostics", export_result.step, ret, &status,
            &export_result,
            "requires a ready RP2040 SD bridge with file_ops and atomic rename; no Public RF or format command was used");
        return;
    }

    d1l_storage_status(&status);
    ok_begin("storage export-diagnostics");
    printf(",\"token\":");
    print_json_string(export_result.token);
    printf(",\"kind\":\"diagnostic_export\",\"path\":");
    print_json_string(export_result.path);
    printf(",\"tmp_path\":");
    print_json_string(export_result.tmp_path);
    printf(",\"bytes\":%u,\"chunks_written\":%lu,\"chunks_verified_tmp\":%lu,\"chunks_verified_final\":%lu,\"tmp_verified_bytes\":%u,\"final_verified_bytes\":%u,\"write_tmp\":%s,\"read_tmp\":%s,\"rename_replace\":%s,\"stat_final\":%s,\"read_final\":%s,\"public_rf_tx\":false,\"formats_sd\":false,\"export_backend\":",
           (unsigned)export_result.bytes,
           (unsigned long)export_result.chunks_written,
           (unsigned long)export_result.chunks_verified_tmp,
           (unsigned long)export_result.chunks_verified_final,
           (unsigned)export_result.tmp_verified_bytes,
           (unsigned)export_result.final_verified_bytes,
           bool_json(export_result.write_tmp),
           bool_json(export_result.read_tmp),
           bool_json(export_result.rename_replace),
           bool_json(export_result.stat_final),
           bool_json(export_result.read_final));
    print_json_string(status.export_backend ? status.export_backend : "serial");
    printf(",\"note\":\"Diagnostic export JSON committed to SD by chunked temp write and atomic rename; no Public RF or format command was used\"}\n");
}

static void cmd_storage_export_data(const char *line)
{
    static const char prefix[] = "storage export-data ";
    char token[D1L_EXPORT_CANARY_TOKEN_MAX + 1U] = {0};
    if (strncmp(line, prefix, strlen(prefix)) != 0 ||
        !copy_storage_token(token, sizeof(token), line + strlen(prefix))) {
        err_result("storage export-data", "INVALID_TOKEN",
                   "usage: storage export-data <token-with-a-z-0-9-dot-dash-underscore>");
        return;
    }

    (void)d1l_storage_status_refresh(D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS);
    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);

    static char payload[D1L_EXPORT_DATA_PAYLOAD_MAX] EXT_RAM_BSS_ATTR;
    size_t payload_len = 0;
    if (!build_data_export_payload(token, &status, payload, sizeof(payload),
                                   &payload_len)) {
        d1l_export_canary_result_t failed = {0};
        print_storage_export_error(
            "storage export-data", "payload", ESP_ERR_INVALID_SIZE,
            &status, &failed,
            "data export payload exceeded its bounded buffer; no SD write was attempted");
        return;
    }

    d1l_export_canary_result_t export_result = {0};
    esp_err_t ret = d1l_export_store_write_data(
        token, (const uint8_t *)payload, payload_len, &status, &export_result);
    if (ret != ESP_OK) {
        print_storage_export_error(
            "storage export-data", export_result.step, ret, &status,
            &export_result,
            "requires a ready RP2040 SD bridge with file_ops and atomic rename; no Public RF or format command was used");
        return;
    }

    d1l_storage_status(&status);
    ok_begin("storage export-data");
    printf(",\"token\":");
    print_json_string(export_result.token);
    printf(",\"kind\":\"data_export\",\"path\":");
    print_json_string(export_result.path);
    printf(",\"tmp_path\":");
    print_json_string(export_result.tmp_path);
    printf(",\"bytes\":%u,\"chunks_written\":%lu,\"chunks_verified_tmp\":%lu,\"chunks_verified_final\":%lu,\"tmp_verified_bytes\":%u,\"final_verified_bytes\":%u,\"write_tmp\":%s,\"read_tmp\":%s,\"rename_replace\":%s,\"stat_final\":%s,\"read_final\":%s,\"sampled\":true,\"private_identity_exported\":false,\"public_rf_tx\":false,\"formats_sd\":false,\"export_backend\":",
           (unsigned)export_result.bytes,
           (unsigned long)export_result.chunks_written,
           (unsigned long)export_result.chunks_verified_tmp,
           (unsigned long)export_result.chunks_verified_final,
           (unsigned)export_result.tmp_verified_bytes,
           (unsigned)export_result.final_verified_bytes,
           bool_json(export_result.write_tmp),
           bool_json(export_result.read_tmp),
           bool_json(export_result.rename_replace),
           bool_json(export_result.stat_final),
           bool_json(export_result.read_final));
    print_json_string(status.export_backend ? status.export_backend : "serial");
    printf(",\"note\":\"Sampled user data export JSON committed to SD by chunked temp write and atomic rename; no private identity material, Public RF, or format command was used\"}\n");
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static uint32_t retained_canary_hash(const char *token)
{
    uint32_t hash = 2166136261UL;
    for (const unsigned char *p = (const unsigned char *)token; p && *p; ++p) {
        hash ^= (uint32_t)*p;
        hash *= 16777619UL;
    }
    return hash ? hash : 1UL;
}

static void retained_canary_fingerprint(const char *token,
                                        char *fingerprint,
                                        size_t fingerprint_size)
{
    const uint32_t hash = retained_canary_hash(token);
    snprintf(fingerprint, fingerprint_size, "%08lx%08lx",
             (unsigned long)hash,
             (unsigned long)(hash ^ 0xA5A5F00DUL));
}

static void cmd_ui_data_canary(const char *line)
{
    static const char prefix[] = "ui data-canary ";
    char token[32] = {0};
    if (strncmp(line, prefix, strlen(prefix)) != 0 ||
        !copy_storage_token(token, sizeof(token), line + strlen(prefix))) {
        err_result("ui data-canary", "INVALID_TOKEN",
                   "usage: ui data-canary <token-with-a-z-0-9-dot-dash-underscore>");
        return;
    }

    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    char text[D1L_MESSAGE_TEXT_LEN] = {0};
    retained_canary_fingerprint(token, fingerprint, sizeof(fingerprint));
    snprintf(text, sizeof(text), "ui-data-canary %s", token);

    const uint32_t public_seq = d1l_message_store_stats().next_seq;
    esp_err_t ret = d1l_message_store_append_public_volatile("rx", "UI Canary", text,
                                                             0, 0, 1, 0, true);
    if (ret != ESP_OK) {
        err_result("ui data-canary", esp_err_to_name(ret),
                   "could not append public UI canary");
        return;
    }

    const uint32_t dm_seq = d1l_dm_store_stats().next_seq;
    ret = d1l_dm_store_append_volatile(fingerprint, "UI Canary", "rx", text,
                                       0, 0, 1, 0, 0, true, true,
                                       retained_canary_hash(token));
    if (ret != ESP_OK) {
        err_result("ui data-canary", esp_err_to_name(ret),
                   "could not append DM UI canary");
        return;
    }

    const uint32_t route_seq = d1l_route_store_stats().next_seq;
    ret = d1l_route_store_upsert_observation_volatile(fingerprint, "UI Canary",
                                                      "ui_canary", "local", "rx",
                                                      0, 0, 1, 0, (uint16_t)strlen(text));
    if (ret != ESP_OK) {
        err_result("ui data-canary", esp_err_to_name(ret),
                   "could not append route UI canary");
        return;
    }

    const uint32_t packet_seq = d1l_packet_log_stats().next_seq;
    char packet_note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    snprintf(packet_note, sizeof(packet_note), "ui-canary %s", token);
    d1l_packet_log_entry_t packet = {
        .direction = "rx",
        .kind = "ui_canary",
        .rssi_dbm = 0,
        .snr_tenths = 0,
        .path_hash_bytes = 1,
        .path_hops = 0,
        .payload_len = (uint16_t)strlen(text),
    };
    strncpy(packet.note, packet_note, sizeof(packet.note) - 1U);
    if (!d1l_packet_log_append_raw_volatile(&packet, (const uint8_t *)text, strlen(text))) {
        err_result("ui data-canary", "ESP_FAIL",
                   "could not append packet UI canary");
        return;
    }

    ok_begin("ui data-canary");
    printf(",\"token\":");
    print_json_string(token);
    printf(",\"fingerprint\":\"%s\",\"public_seq\":%lu,\"dm_seq\":%lu,\"route_seq\":%lu,\"packet_seq\":%lu",
           fingerprint,
           (unsigned long)public_seq,
           (unsigned long)dm_seq,
           (unsigned long)route_seq,
           (unsigned long)packet_seq);
    printf(",\"storage_required\":false,\"retention\":\"volatile\",\"public_rf_tx\":false,\"formats_sd\":false");
    printf(",\"note\":\"Synthetic UI refresh canary rows appended to RAM only, without Public RF, SD readiness, or format command\"}\n");
}

static void cmd_storage_retained_canary(const char *line)
{
    static const char prefix[] = "storage retained-canary ";
    char token[32] = {0};
    if (strncmp(line, prefix, strlen(prefix)) != 0 ||
        !copy_storage_token(token, sizeof(token), line + strlen(prefix))) {
        err_result("storage retained-canary", "INVALID_TOKEN",
                   "usage: storage retained-canary <token-with-a-z-0-9-dot-dash-underscore>");
        return;
    }

    const int64_t quiesce_started_us = esp_timer_get_time();
    uint32_t remaining_ms = deadline_remaining_ms(
        quiesce_started_us, D1L_RETAINED_CANARY_QUIESCE_TIMEOUT_MS);
    esp_err_t ret = remaining_ms > 0U ?
        d1l_storage_manager_quiesce_begin(remaining_ms) : ESP_ERR_TIMEOUT;
    if (ret != ESP_OK) {
        err_result("storage retained-canary", esp_err_to_name(ret),
                   "storage manager quiesce failed; retained canary cancelled");
        return;
    }

    remaining_ms = deadline_remaining_ms(
        quiesce_started_us, D1L_RETAINED_CANARY_QUIESCE_TIMEOUT_MS);
    ret = remaining_ms > 0U ?
        d1l_route_store_worker_quiesce_begin(remaining_ms) : ESP_ERR_TIMEOUT;
    if (ret != ESP_OK) {
        d1l_storage_manager_quiesce_end();
        err_result("storage retained-canary", esp_err_to_name(ret),
                   "retained worker quiesce failed; retained canary cancelled");
        return;
    }

    remaining_ms = deadline_remaining_ms(
        quiesce_started_us, D1L_RETAINED_CANARY_QUIESCE_TIMEOUT_MS);
    const uint32_t refresh_timeout_ms =
        remaining_ms < D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS ?
            remaining_ms : D1L_STORAGE_RP2040_SD_PROBE_TIMEOUT_MS;
    ret = refresh_timeout_ms > 0U ?
        d1l_storage_status_refresh(refresh_timeout_ms) : ESP_ERR_TIMEOUT;
    if (ret != ESP_OK) {
        d1l_route_store_worker_quiesce_end();
        d1l_storage_manager_quiesce_end();
        err_result("storage retained-canary", esp_err_to_name(ret),
                   "fresh storage status failed; retained canary cancelled");
        return;
    }

    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);
    const bool sd_backend_mode = storage_retained_history_sd_ready(&status);
    const bool volatile_no_card_backend_mode =
        storage_retained_history_volatile_no_card_ready(&status);
    uint32_t backend_generations[D1L_RETAINED_BLOB_STORE_COUNT] = {0};
    if ((!sd_backend_mode && !volatile_no_card_backend_mode) ||
        !retained_canary_backend_generations(
            sd_backend_mode, backend_generations)) {
        d1l_route_store_worker_quiesce_end();
        d1l_storage_manager_quiesce_end();
        err_result("storage retained-canary", "SD_RETAINED_HISTORY_NOT_READY",
                   "requires either ready RP2040 SD backends or an explicit fresh NO_CARD live-only state");
        return;
    }

    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    char text[D1L_MESSAGE_TEXT_LEN] = {0};
    retained_canary_fingerprint(token, fingerprint, sizeof(fingerprint));
    snprintf(text, sizeof(text), "sd-retained-canary %s", token);

    const uint32_t public_seq = d1l_message_store_stats().next_seq;
    ret = d1l_message_store_append_public("tx", "SD Canary", text,
                                          0, 0, 1, 0, true);
    if (ret != ESP_OK) {
        d1l_route_store_worker_quiesce_end();
        d1l_storage_manager_quiesce_end();
        err_result("storage retained-canary", esp_err_to_name(ret),
                   "could not append public retained-history canary");
        return;
    }

    const uint32_t dm_seq = d1l_dm_store_stats().next_seq;
    ret = d1l_dm_store_append(fingerprint, "SD Canary", "tx", text,
                              0, 0, 1, 0, 0, true, true,
                              retained_canary_hash(token));
    if (ret != ESP_OK) {
        d1l_route_store_worker_quiesce_end();
        d1l_storage_manager_quiesce_end();
        err_result("storage retained-canary", esp_err_to_name(ret),
                   "could not append DM retained-history canary");
        return;
    }

    const uint32_t route_seq = d1l_route_store_stats().next_seq;
    ret = d1l_route_store_upsert_observation(fingerprint, "SD Canary",
                                             "sd_canary", "local", "tx",
                                             0, 0, 1, 0, (uint16_t)strlen(text));
    if (ret != ESP_OK) {
        d1l_route_store_worker_quiesce_end();
        d1l_storage_manager_quiesce_end();
        err_result("storage retained-canary", esp_err_to_name(ret),
                   "could not append route retained-history canary");
        return;
    }

    uint32_t packet_seq = 0U;
    char packet_note[D1L_PACKET_LOG_NOTE_LEN] = {0};
    snprintf(packet_note, sizeof(packet_note), "sd-canary %s", token);
    d1l_packet_log_entry_t packet = {
        .direction = "tx",
        .kind = "sd_canary",
        .rssi_dbm = 0,
        .snr_tenths = 0,
        .path_hash_bytes = 1,
        .path_hops = 0,
        .payload_len = (uint16_t)strlen(text),
    };
    strncpy(packet.note, packet_note, sizeof(packet.note) - 1U);
    ret = d1l_packet_log_append_raw_checked(
        &packet, (const uint8_t *)text, strlen(text), &packet_seq);
    if (ret != ESP_OK) {
        d1l_route_store_worker_quiesce_end();
        d1l_storage_manager_quiesce_end();
        err_result("storage retained-canary", esp_err_to_name(ret),
                   "could not append packet retained-history canary");
        return;
    }

    d1l_route_store_worker_quiesce_end();
    d1l_storage_manager_quiesce_end();
    ok_begin("storage retained-canary");
    printf(",\"token\":");
    print_json_string(token);
    printf(",\"fingerprint\":\"%s\",\"public_seq\":%lu,\"dm_seq\":%lu,\"route_seq\":%lu,\"packet_seq\":%lu",
           fingerprint,
           (unsigned long)public_seq,
           (unsigned long)dm_seq,
           (unsigned long)route_seq,
           (unsigned long)packet_seq);
    printf(",\"storage_manager_quiesced\":true,\"retained_worker_quiesced\":true,\"backend_mode\":\"%s\",\"history_persisted\":%s,\"public_rf_tx\":false,\"dm_rf_tx\":false,\"formats_sd\":false,\"backends\":{\"messages\":\"%s\",\"dm\":\"%s\",\"routes\":\"%s\",\"packets\":\"%s\",\"nodes\":\"%s\",\"contacts\":\"%s\",\"read_state\":\"%s\"}",
           sd_backend_mode ? "sd" : "volatile_no_card",
           bool_json(sd_backend_mode),
           status.message_store_backend,
           status.dm_store_backend,
           status.route_store_backend,
           status.packet_log_backend,
           status.node_store_backend,
           status.contact_store_backend,
           status.read_state_backend);
    printf(",\"backend_generations\":{\"messages\":%lu,\"dm\":%lu,\"routes\":%lu,\"packets\":%lu,\"nodes\":%lu,\"contacts\":%lu,\"read_state\":%lu}",
           (unsigned long)backend_generations[D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES],
           (unsigned long)backend_generations[D1L_RETAINED_BLOB_STORE_DM_MESSAGES],
           (unsigned long)backend_generations[D1L_RETAINED_BLOB_STORE_ROUTES],
           (unsigned long)backend_generations[D1L_RETAINED_BLOB_STORE_PACKET_LOG],
           (unsigned long)backend_generations[D1L_RETAINED_BLOB_STORE_NODES],
           (unsigned long)backend_generations[D1L_RETAINED_BLOB_STORE_CONTACTS],
           (unsigned long)backend_generations[D1L_RETAINED_BLOB_STORE_READ_STATE]);
    printf(",\"note\":\"Synthetic retained-history canary rows appended without Public or DM RF or a format command\"}\n");
}
#endif

static void cmd_storage_setup(const char *line)
{
    (void)line;
    d1l_storage_status_t status = {0};
    d1l_storage_status(&status);

    ok_begin("storage setup");
    print_storage_setup_payload(&status);
}

static void print_packet_entry_json(const d1l_packet_log_entry_t *e)
{
    printf("{\"seq\":%lu,\"uptime_ms\":%lu,\"direction\":\"%s\",\"kind\":\"%s\",\"rssi_dbm\":%d,\"snr_tenths\":%d,\"path_hash_bytes\":%u,\"path_hops\":%u,\"payload_len\":%u,\"raw_len\":%u,\"raw_truncated\":%s,\"note\":\"%s\",\"raw_hex\":\"%s\"}",
           (unsigned long)e->seq, (unsigned long)e->uptime_ms,
           e->direction, e->kind, e->rssi_dbm, e->snr_tenths,
           e->path_hash_bytes, e->path_hops, e->payload_len, e->raw_len,
           bool_json(e->raw_truncated), e->note, e->raw_hex);
}

static void print_packet_entries_json(const char *cmd, const char *direction, const char *kind,
                                      const char *search_text,
                                      const d1l_packet_log_entry_t *entries, size_t copied,
                                      const d1l_packet_log_stats_t *stats)
{
    ok_begin(cmd);
    printf(",\"count\":%u,\"capacity\":%u,\"total_written\":%lu,\"dropped_oldest\":%lu",
           (unsigned)stats->count, (unsigned)stats->capacity,
           (unsigned long)stats->total_written, (unsigned long)stats->dropped_oldest);
    printf(",\"sd_history\":{\"enabled\":%s,\"records\":%lu,\"capacity\":%u,\"failed_writes\":%lu}",
           bool_json(stats->sd_history_enabled),
           (unsigned long)stats->sd_history_records,
           (unsigned)stats->sd_capacity,
           (unsigned long)stats->sd_history_failed_writes);
    if (direction || kind || search_text) {
        printf(",\"filter\":{\"direction\":\"%s\",\"kind\":\"%s\",\"search\":\"%s\"}",
               direction ? direction : "any", kind ? kind : "any", search_text ? search_text : "");
    }
    printf(",\"persistence\":{\"loaded\":%s,\"dirty\":%s,\"revision\":%llu,\"commits\":%lu,\"failures\":%lu,\"reconcile\":{\"pending\":%s,\"count\":%lu,\"failures\":%lu,\"last_error\":\"%s\"},\"sd\":{\"required\":%s,\"generation\":%lu,\"dirty\":%s,\"commits\":%lu,\"failures\":%lu,\"last_error\":\"%s\"},\"nvs\":{\"dirty\":%s,\"commits\":%lu,\"failures\":%lu,\"last_error\":\"%s\"},\"journal\":{\"dirty\":%s,\"commits\":%lu,\"failures\":%lu,\"last_error\":\"%s\"}}",
           bool_json(stats->loaded),
           bool_json(stats->persistence_dirty),
           (unsigned long long)stats->persistence_revision,
           (unsigned long)stats->persistence_commit_count,
           (unsigned long)stats->persistence_fail_count,
           bool_json(stats->sd_primary_reconcile_pending),
           (unsigned long)stats->sd_reconcile_count,
           (unsigned long)stats->sd_reconcile_fail_count,
           esp_err_to_name(stats->sd_reconcile_last_error),
           bool_json(stats->sd_history_enabled),
           (unsigned long)stats->sd_backend_generation,
           bool_json(stats->sd_primary_dirty),
           (unsigned long)stats->sd_primary_commit_count,
           (unsigned long)stats->sd_primary_fail_count,
           esp_err_to_name(stats->sd_primary_last_error),
           bool_json(stats->nvs_fallback_dirty),
           (unsigned long)stats->nvs_fallback_commit_count,
           (unsigned long)stats->nvs_fallback_fail_count,
           esp_err_to_name(stats->nvs_fallback_last_error),
           bool_json(stats->journal_dirty),
           (unsigned long)stats->journal_commit_count,
           (unsigned long)stats->journal_fail_count,
           esp_err_to_name(stats->journal_last_error));
    printf(",\"entries\":[");
    for (size_t i = 0; i < copied; ++i) {
        printf("%s", i ? "," : "");
        print_packet_entry_json(&entries[i]);
    }
    printf("],\"persisted\":%s,\"note\":\"Packet log records recent MeshCore RF TX/RX evidence\"}\n",
           bool_json(stats->loaded && !stats->persistence_dirty));
}

static bool read_packet_token(const char **cursor, char *dest, size_t dest_size)
{
    if (!cursor || !*cursor || !dest || dest_size == 0) {
        return false;
    }
    while (**cursor == ' ') {
        (*cursor)++;
    }
    size_t out = 0;
    while (**cursor != '\0' && **cursor != ' ' && out + 1U < dest_size) {
        unsigned char c = (unsigned char)**cursor;
        if (c < 32 || c > 126 || c == '"' || c == '\\') {
            c = '_';
        }
        dest[out++] = (char)c;
        (*cursor)++;
    }
    dest[out] = '\0';
    while (**cursor == ' ') {
        (*cursor)++;
    }
    return out > 0;
}

static void copy_packet_search(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    size_t out = 0;
    while (src && *src == ' ') {
        src++;
    }
    while (src && src[0] && out + 1U < dest_size) {
        unsigned char c = (unsigned char)*src++;
        if (c < 32 || c > 126 || c == '"' || c == '\\') {
            c = '_';
        }
        dest[out++] = (char)c;
    }
    while (out > 0 && dest[out - 1U] == ' ') {
        out--;
    }
    dest[out] = '\0';
}

static void cmd_packets(void)
{
    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    static d1l_packet_log_entry_t entries[8] EXT_RAM_BSS_ATTR;
    size_t copied = d1l_packet_log_copy_recent(entries, 8);
    print_packet_entries_json("packets", NULL, NULL, NULL, entries, copied, &stats);
}

static void cmd_packets_filter(const char *line)
{
    const char *arg = line + strlen("packets filter ");
    char direction[8] = "any";
    char kind[16] = "any";
    if (!read_packet_token(&arg, direction, sizeof(direction)) ||
        !read_packet_token(&arg, kind, sizeof(kind)) ||
        arg[0] != '\0') {
        err_result("packets filter", "INVALID_FILTER", "usage: packets filter <any|rx|tx> <any|text|kind>");
        return;
    }

    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    static d1l_packet_log_entry_t entries[8] EXT_RAM_BSS_ATTR;
    size_t copied = d1l_packet_log_query(entries, 8, direction, kind, NULL);
    print_packet_entries_json("packets filter", direction, kind, NULL, entries, copied, &stats);
}

static void cmd_packets_search(const char *line)
{
    char search[D1L_PACKET_LOG_QUERY_TEXT_LEN] = {0};
    copy_packet_search(search, sizeof(search), line + strlen("packets search "));
    if (search[0] == '\0') {
        err_result("packets search", "INVALID_QUERY", "usage: packets search <text>");
        return;
    }

    d1l_packet_log_stats_t stats = d1l_packet_log_stats();
    static d1l_packet_log_entry_t entries[8] EXT_RAM_BSS_ATTR;
    size_t copied = d1l_packet_log_query(entries, 8, "any", "any", search);
    print_packet_entries_json("packets search", "any", "any", search, entries, copied, &stats);
}

static void cmd_packets_detail(const char *line)
{
    const char *arg = line + strlen("packets detail ");
    while (*arg == ' ') {
        arg++;
    }
    char *end = NULL;
    unsigned long seq = strtoul(arg, &end, 10);
    while (end && *end == ' ') {
        end++;
    }
    if (arg[0] == '\0' || end == arg || (end && *end != '\0') || seq == 0 || seq > UINT32_MAX) {
        err_result("packets detail", "INVALID_SEQ", "usage: packets detail <seq>");
        return;
    }

    d1l_packet_log_entry_t entry = {0};
    esp_err_t ret = d1l_packet_log_find_by_seq((uint32_t)seq, &entry);
    if (ret != ESP_OK) {
        err_result("packets detail", esp_err_to_name(ret), "packet sequence not found");
        return;
    }

    ok_begin("packets detail");
    printf(",\"entry\":");
    print_packet_entry_json(&entry);
    printf("}\n");
}

static void cmd_packets_raw(const char *line)
{
    const char *arg = line + strlen("packets raw ");
    while (*arg == ' ') {
        arg++;
    }
    char *end = NULL;
    unsigned long seq = strtoul(arg, &end, 10);
    while (end && *end == ' ') {
        end++;
    }
    if (arg[0] == '\0' || end == arg || (end && *end != '\0') || seq == 0 || seq > UINT32_MAX) {
        err_result("packets raw", "INVALID_SEQ", "usage: packets raw <seq>");
        return;
    }

    d1l_packet_log_entry_t entry = {0};
    esp_err_t ret = d1l_packet_log_find_by_seq((uint32_t)seq, &entry);
    if (ret != ESP_OK) {
        err_result("packets raw", esp_err_to_name(ret), "packet sequence not found");
        return;
    }

    ok_begin("packets raw");
    printf(",\"seq\":%lu,\"raw_len\":%u,\"raw_preview_bytes\":%u,\"raw_truncated\":%s,\"raw_hex\":\"%s\",\"entry\":",
           (unsigned long)entry.seq, entry.raw_len, D1L_PACKET_LOG_RAW_PREVIEW_BYTES,
           bool_json(entry.raw_truncated), entry.raw_hex);
    print_packet_entry_json(&entry);
    printf("}\n");
}

static void cmd_packets_clear(void)
{
    esp_err_t ret = d1l_packet_log_clear();
    if (ret != ESP_OK) {
        err_result("packets clear", esp_err_to_name(ret), "could not clear packet log");
        return;
    }
    ok_begin("packets clear");
    printf(",\"persisted\":true,\"count\":0}\n");
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static bool message_store_stats_snapshot_equal(
    const d1l_message_store_stats_t *left,
    const d1l_message_store_stats_t *right)
{
    return left && right &&
        left->next_seq == right->next_seq &&
        left->total_written == right->total_written &&
        left->dropped_oldest == right->dropped_oldest &&
        left->epoch == right->epoch &&
        left->content_revision == right->content_revision &&
        left->clear_lineage == right->clear_lineage &&
        left->persistence_commit_count == right->persistence_commit_count &&
        left->persistence_fail_count == right->persistence_fail_count &&
        left->persistence_stale_snapshot_count ==
            right->persistence_stale_snapshot_count &&
        left->sd_primary_commit_count == right->sd_primary_commit_count &&
        left->sd_primary_fail_count == right->sd_primary_fail_count &&
        left->sd_backend_generation == right->sd_backend_generation &&
        left->sd_primary_last_error == right->sd_primary_last_error &&
        left->nvs_fallback_commit_count == right->nvs_fallback_commit_count &&
        left->nvs_fallback_fail_count == right->nvs_fallback_fail_count &&
        left->nvs_fallback_last_error == right->nvs_fallback_last_error &&
        left->persistence_revision == right->persistence_revision &&
        left->count == right->count &&
        left->public_count == right->public_count &&
        left->capacity == right->capacity &&
        left->loaded == right->loaded &&
        left->persistence_dirty == right->persistence_dirty &&
        left->sd_primary_required == right->sd_primary_required &&
        left->sd_primary_dirty == right->sd_primary_dirty &&
        left->sd_primary_reconcile_pending ==
            right->sd_primary_reconcile_pending &&
        left->nvs_fallback_dirty == right->nvs_fallback_dirty;
}

static bool dm_store_stats_snapshot_equal(
    const d1l_dm_store_stats_t *left,
    const d1l_dm_store_stats_t *right)
{
    return left && right &&
        left->next_seq == right->next_seq &&
        left->total_written == right->total_written &&
        left->dropped_oldest == right->dropped_oldest &&
        left->epoch == right->epoch &&
        left->content_revision == right->content_revision &&
        left->clear_lineage == right->clear_lineage &&
        left->persistence_commit_count == right->persistence_commit_count &&
        left->persistence_fail_count == right->persistence_fail_count &&
        left->persistence_stale_snapshot_count ==
            right->persistence_stale_snapshot_count &&
        left->sd_primary_commit_count == right->sd_primary_commit_count &&
        left->sd_primary_fail_count == right->sd_primary_fail_count &&
        left->sd_backend_generation == right->sd_backend_generation &&
        left->sd_primary_last_error == right->sd_primary_last_error &&
        left->nvs_fallback_commit_count == right->nvs_fallback_commit_count &&
        left->nvs_fallback_fail_count == right->nvs_fallback_fail_count &&
        left->nvs_fallback_last_error == right->nvs_fallback_last_error &&
        left->persistence_revision == right->persistence_revision &&
        left->count == right->count &&
        left->capacity == right->capacity &&
        left->loaded == right->loaded &&
        left->persistence_dirty == right->persistence_dirty &&
        left->sd_primary_required == right->sd_primary_required &&
        left->sd_primary_dirty == right->sd_primary_dirty &&
        left->sd_primary_reconcile_pending ==
            right->sd_primary_reconcile_pending &&
        left->nvs_fallback_dirty == right->nvs_fallback_dirty;
}
#endif

static bool contact_store_stats_snapshot_equal(
    const d1l_contact_store_stats_t *left,
    const d1l_contact_store_stats_t *right)
{
    return left && right &&
        left->next_seq == right->next_seq &&
        left->total_written == right->total_written &&
        left->dropped_oldest == right->dropped_oldest &&
        left->persistence_commit_count == right->persistence_commit_count &&
        left->persistence_coalesced_count ==
            right->persistence_coalesced_count &&
        left->persistence_fail_count == right->persistence_fail_count &&
        left->persistence_revision == right->persistence_revision &&
        left->persistence_last_error == right->persistence_last_error &&
        left->count == right->count &&
        left->capacity == right->capacity &&
        left->persistence_dirty == right->persistence_dirty;
}

static void print_public_message_entry_json(const d1l_message_entry_t *e,
                                            bool retained)
{
    printf("{\"seq\":%lu,\"uptime_ms\":%lu,\"direction\":",
           (unsigned long)e->seq, (unsigned long)e->uptime_ms);
    print_json_string(e->direction);
    printf(",\"author\":");
    print_json_string(e->author);
    printf(",\"text\":");
    print_json_string(e->text);
    printf(",\"rssi_dbm\":%d,\"snr_tenths\":%d,\"path_hash_bytes\":%u,\"path_hops\":%u,\"delivered\":%s,\"retained\":%s,\"volatile_preview\":%s}",
           e->rssi_dbm, e->snr_tenths, e->path_hash_bytes, e->path_hops,
           bool_json(e->delivered), bool_json(retained), bool_json(!retained));
}

static void cmd_messages_public(const char *line)
{
    static d1l_message_entry_t entries[D1L_MESSAGE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
    char search[D1L_MESSAGE_TEXT_LEN] = {0};
    bool filtered = false;
    size_t offset = 0;

    const char *args = line + strlen("messages public");
    while (*args == ' ') {
        args++;
    }
    if (*args == '\0') {
        /* Newest page of retained history. */
    } else if (strncmp(args, "offset", 6) == 0) {
        if (!parse_message_offset_clause(args, &offset)) {
            err_result("messages public", "INVALID_OFFSET",
                       "usage: messages public [offset <n>] | messages public search <text> [offset <n>]");
            return;
        }
    } else if (strncmp(args, "search ", 7) == 0) {
        copy_packet_search(search, sizeof(search), args + 7);
        if (!strip_message_offset_suffix(search, &offset)) {
            err_result("messages public", "INVALID_OFFSET",
                       "usage: messages public [offset <n>] | messages public search <text> [offset <n>]");
            return;
        }
        if (search[0] == '\0') {
            err_result("messages public", "INVALID_QUERY",
                       "usage: messages public [offset <n>] | messages public search <text> [offset <n>]");
            return;
        }
        filtered = true;
    } else {
        err_result("messages public", "INVALID_TARGET",
                   "usage: messages public [offset <n>] | messages public search <text> [offset <n>]");
        return;
    }

    size_t total_matches = 0;
    d1l_message_store_stats_t stats = {0};
    uint32_t volatile_preview_seq = 0U;
    const size_t copied = d1l_message_store_query_page_snapshot(
        entries, D1L_CONSOLE_MESSAGE_PAGE_SIZE, offset, search,
        &total_matches, &stats, &volatile_preview_seq);
    const bool has_older = total_matches > offset + copied;
    const size_t next_offset = has_older ? offset + copied : offset;
    ok_begin("messages public");
    const bool volatile_preview_present = volatile_preview_seq != 0U;
    printf(",\"count\":%u,\"capacity\":%u,\"retained_store_count\":%u,\"retained_public_count\":%u,\"volatile_preview_present\":%s,\"volatile_preview_seq\":%lu,\"total_written\":%lu,\"dropped_oldest\":%lu,\"history_counters_scope\":\"shared_all_channels\",\"filtered\":%s,\"offset\":%u,\"page_size\":%u,\"page_count\":%u,\"total_matches\":%u,\"has_older\":%s,\"next_offset\":%u",
           (unsigned)stats.public_count, (unsigned)stats.capacity,
           (unsigned)stats.count, (unsigned)stats.public_count,
           bool_json(volatile_preview_present),
           (unsigned long)volatile_preview_seq,
           (unsigned long)stats.total_written, (unsigned long)stats.dropped_oldest,
           bool_json(filtered), (unsigned)offset,
           (unsigned)D1L_CONSOLE_MESSAGE_PAGE_SIZE, (unsigned)copied,
           (unsigned)total_matches, bool_json(has_older), (unsigned)next_offset);
    if (filtered) {
        printf(",\"search\":");
        print_json_string(search);
    }
    printf(",\"retained_epoch\":%lu,\"content_revision\":%lu",
           (unsigned long)stats.epoch,
           (unsigned long)stats.content_revision);
    printf(",\"persistence\":{\"loaded\":%s,\"dirty\":%s,\"revision\":%llu,\"commits\":%lu,\"failures\":%lu,\"stale_snapshots\":%lu,\"sd\":{\"required\":%s,\"generation\":%lu,\"dirty\":%s,\"reconcile_pending\":%s,\"commits\":%lu,\"failures\":%lu,\"last_error\":\"%s\"},\"nvs\":{\"dirty\":%s,\"commits\":%lu,\"failures\":%lu,\"last_error\":\"%s\"}}",
           bool_json(stats.loaded),
           bool_json(stats.persistence_dirty),
           (unsigned long long)stats.persistence_revision,
           (unsigned long)stats.persistence_commit_count,
           (unsigned long)stats.persistence_fail_count,
           (unsigned long)stats.persistence_stale_snapshot_count,
           bool_json(stats.sd_primary_required),
           (unsigned long)stats.sd_backend_generation,
           bool_json(stats.sd_primary_dirty),
           bool_json(stats.sd_primary_reconcile_pending),
           (unsigned long)stats.sd_primary_commit_count,
           (unsigned long)stats.sd_primary_fail_count,
           esp_err_to_name(stats.sd_primary_last_error),
           bool_json(stats.nvs_fallback_dirty),
           (unsigned long)stats.nvs_fallback_commit_count,
           (unsigned long)stats.nvs_fallback_fail_count,
           esp_err_to_name(stats.nvs_fallback_last_error));
    printf(",\"entries\":[");
    for (size_t i = 0; i < copied; ++i) {
        printf("%s", i ? "," : "");
        print_public_message_entry_json(
            &entries[i],
            entries[i].seq != volatile_preview_seq);
    }
    printf("],\"persisted\":%s,\"note\":\"Public messages are kept in bounded retained storage; optional search filters retained rows and offset pages older rows\"}\n",
           bool_json(stats.loaded && !stats.persistence_dirty));
}

static void cmd_messages_clear(void)
{
    esp_err_t ret = d1l_message_store_clear_channel(
        D1L_CHANNEL_PUBLIC_ID);
    if (ret != ESP_OK) {
        err_result("messages clear", esp_err_to_name(ret), "could not clear public message store");
        return;
    }
    ret = d1l_app_model_mark_public_read();
    if (ret != ESP_OK) {
        err_result("messages clear", esp_err_to_name(ret), "public messages cleared but read state did not persist");
        return;
    }
    ok_begin("messages clear");
    printf(",\"persisted\":true,\"count\":0}\n");
}

static void print_dm_entry_json(const d1l_dm_entry_t *e, bool retained)
{
    printf("{\"seq\":%lu,\"uptime_ms\":%lu,\"fingerprint\":",
           (unsigned long)e->seq, (unsigned long)e->uptime_ms);
    print_json_string(e->contact_fingerprint);
    printf(",\"alias\":");
    print_json_string(e->contact_alias);
    printf(",\"direction\":");
    print_json_string(e->direction);
    printf(",\"text\":");
    print_json_string(e->text);
    printf(",\"rssi_dbm\":%d,\"snr_tenths\":%d,\"path_hash_bytes\":%u,"
           "\"path_hops\":%u,\"attempt\":%u,\"delivered\":%s,\"acked\":%s,"
           "\"ack_hash\":%lu,\"ack_response\":{\"identity_valid\":%s,\"state\":",
           e->rssi_dbm, e->snr_tenths, e->path_hash_bytes, e->path_hops,
           e->attempt, bool_json(e->delivered), bool_json(e->acked),
           (unsigned long)e->ack_hash, bool_json(e->identity_digest_valid));
    print_json_string(d1l_dm_ack_state_name(e->ack_state));
    printf(",\"dispatch_count\":%u,\"last_kind\":", e->ack_dispatch_count);
    print_json_string(d1l_dm_ack_dispatch_kind_name(e->ack_dispatch_kind));
    printf(",\"last_error\":");
    print_json_string(esp_err_to_name(e->ack_last_error));
    printf("},\"retained\":%s,\"volatile_preview\":%s}",
           bool_json(retained), bool_json(!retained));
}

static void cmd_messages_dm(const char *line)
{
    static d1l_dm_entry_t
        entries[D1L_DM_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
    char thread_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    bool filtered = false;
    size_t offset = 0;
    size_t total_matches = 0;
    size_t copied = 0;
    d1l_dm_store_stats_t stats = {0};
    uint32_t volatile_preview_seq = 0U;

    const char *args = line + strlen("messages dm");
    while (*args == ' ') {
        args++;
    }
    if (*args == '\0') {
        copied = d1l_dm_store_copy_recent_page_snapshot(
            entries, D1L_CONSOLE_MESSAGE_PAGE_SIZE, offset, &total_matches,
            &stats, &volatile_preview_seq);
    } else if (strncmp(args, "offset", 6) == 0) {
        if (!parse_message_offset_clause(args, &offset)) {
            err_result("messages dm", "INVALID_OFFSET",
                       "usage: messages dm [offset <n>] [fingerprint [offset <n>]]");
            return;
        }
        copied = d1l_dm_store_copy_recent_page_snapshot(
            entries, D1L_CONSOLE_MESSAGE_PAGE_SIZE, offset, &total_matches,
            &stats, &volatile_preview_seq);
    } else if (parse_fingerprint_token(args, thread_fingerprint,
                                     sizeof(thread_fingerprint))) {
        filtered = true;
        args += D1L_NODE_FINGERPRINT_LEN - 1U;
        while (*args == ' ') {
            args++;
        }
        if (*args != '\0' && !parse_message_offset_clause(args, &offset)) {
            err_result("messages dm", "INVALID_OFFSET",
                       "usage: messages dm [offset <n>] [fingerprint [offset <n>]]");
            return;
        }
        copied = d1l_dm_store_copy_thread_page_snapshot(
            thread_fingerprint, entries, D1L_CONSOLE_MESSAGE_PAGE_SIZE,
            offset, &total_matches, &stats, &volatile_preview_seq);
    } else {
        err_result("messages dm", "INVALID_TARGET",
                   "usage: messages dm [offset <n>] [fingerprint [offset <n>]]");
        return;
    }

    const bool has_older = total_matches > offset + copied;
    const size_t next_offset = has_older ? offset + copied : offset;
    ok_begin("messages dm");
    const bool volatile_preview_present = volatile_preview_seq != 0U;
    printf(",\"count\":%u,\"capacity\":%u,\"retained_count\":%u,\"volatile_preview_present\":%s,\"volatile_preview_seq\":%lu,\"total_written\":%lu,\"dropped_oldest\":%lu,\"filtered\":%s,\"offset\":%u,\"page_size\":%u,\"page_count\":%u,\"total_matches\":%u,\"has_older\":%s,\"next_offset\":%u",
           (unsigned)stats.count, (unsigned)stats.capacity,
           (unsigned)stats.count, bool_json(volatile_preview_present),
           (unsigned long)volatile_preview_seq,
           (unsigned long)stats.total_written, (unsigned long)stats.dropped_oldest,
           bool_json(filtered), (unsigned)offset,
           (unsigned)D1L_CONSOLE_MESSAGE_PAGE_SIZE, (unsigned)copied,
           (unsigned)total_matches, bool_json(has_older), (unsigned)next_offset);
    if (filtered) {
        printf(",\"fingerprint\":\"%s\",\"thread_count\":%u",
               thread_fingerprint, (unsigned)total_matches);
    }
    printf(",\"retained_epoch\":%lu,\"content_revision\":%lu",
           (unsigned long)stats.epoch,
           (unsigned long)stats.content_revision);
    printf(",\"persistence\":{\"loaded\":%s,\"dirty\":%s,\"revision\":%llu,\"commits\":%lu,\"failures\":%lu,\"stale_snapshots\":%lu,\"sd\":{\"required\":%s,\"generation\":%lu,\"dirty\":%s,\"reconcile_pending\":%s,\"commits\":%lu,\"failures\":%lu,\"last_error\":\"%s\"},\"nvs\":{\"dirty\":%s,\"commits\":%lu,\"failures\":%lu,\"last_error\":\"%s\"}}",
           bool_json(stats.loaded),
           bool_json(stats.persistence_dirty),
           (unsigned long long)stats.persistence_revision,
           (unsigned long)stats.persistence_commit_count,
           (unsigned long)stats.persistence_fail_count,
           (unsigned long)stats.persistence_stale_snapshot_count,
           bool_json(stats.sd_primary_required),
           (unsigned long)stats.sd_backend_generation,
           bool_json(stats.sd_primary_dirty),
           bool_json(stats.sd_primary_reconcile_pending),
           (unsigned long)stats.sd_primary_commit_count,
           (unsigned long)stats.sd_primary_fail_count,
           esp_err_to_name(stats.sd_primary_last_error),
           bool_json(stats.nvs_fallback_dirty),
           (unsigned long)stats.nvs_fallback_commit_count,
           (unsigned long)stats.nvs_fallback_fail_count,
           esp_err_to_name(stats.nvs_fallback_last_error));
    printf(",\"entries\":[");
    for (size_t i = 0; i < copied; ++i) {
        printf("%s", i ? "," : "");
        print_dm_entry_json(
            &entries[i],
            entries[i].seq != volatile_preview_seq);
    }
    printf("],\"persisted\":%s,\"note\":\"MeshCore direct-message rows are kept in bounded retained storage; optional fingerprint filters one retained thread and offset pages older rows\"}\n",
           bool_json(stats.loaded && !stats.persistence_dirty));
}

static void cmd_messages_dm_clear(void)
{
    esp_err_t ret = d1l_dm_store_clear();
    if (ret != ESP_OK) {
        err_result("messages dm clear", esp_err_to_name(ret), "could not clear DM store");
        return;
    }
    ret = d1l_read_state_mark_dm_read();
    if (ret != ESP_OK) {
        err_result("messages dm clear", esp_err_to_name(ret), "DM rows cleared but read state did not persist");
        return;
    }
    ok_begin("messages dm clear");
    printf(",\"persisted\":true,\"count\":0}\n");
}

static void cmd_messages_unread(void)
{
    d1l_read_state_stats_t stats = d1l_read_state_stats();
    static d1l_read_state_dm_thread_t threads[8] EXT_RAM_BSS_ATTR;
    size_t thread_count = d1l_read_state_copy_dm_threads(threads, 8);
    ok_begin("messages unread");
    printf(",\"public_unread\":%lu,\"dm_unread\":%lu,\"muted_dm_unread\":%lu,\"dm_thread_count\":%lu,\"last_public_read_seq\":%lu,\"last_dm_read_seq\":%lu,\"newest_public_rx_seq\":%lu,\"newest_dm_rx_seq\":%lu,\"mark_read_count\":%lu,\"dm_threads\":[",
           (unsigned long)stats.public_unread_count,
           (unsigned long)stats.dm_unread_count,
           (unsigned long)stats.muted_dm_unread_count,
           (unsigned long)stats.dm_thread_count,
           (unsigned long)stats.last_public_read_seq,
           (unsigned long)stats.last_dm_read_seq,
           (unsigned long)stats.newest_public_rx_seq,
           (unsigned long)stats.newest_dm_rx_seq,
           (unsigned long)stats.mark_read_count);
    for (size_t i = 0; i < thread_count; ++i) {
        const d1l_read_state_dm_thread_t *thread = &threads[i];
        printf("%s{\"fingerprint\":\"%s\",\"last_read_seq\":%lu,\"newest_rx_seq\":%lu,\"unread\":%lu,\"muted\":%s}",
               i ? "," : "", thread->fingerprint,
               (unsigned long)thread->last_read_seq,
               (unsigned long)thread->newest_rx_seq,
               (unsigned long)thread->unread_count,
               bool_json(thread->muted));
    }
    printf("],\"persisted\":true,\"note\":\"Unread counters are derived from persisted RX rows; DM cursors are tracked per thread\"}\n");
}

static void cmd_messages_read(const char *line)
{
    const char *arg = line + strlen("messages read ");
    while (*arg == ' ') {
        arg++;
    }

    esp_err_t ret = ESP_ERR_INVALID_ARG;
    const char *target = "invalid";
    char thread_fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (strcmp(arg, "public") == 0) {
        target = "public";
        ret = d1l_app_model_mark_public_read();
    } else if (strcmp(arg, "dm") == 0) {
        target = "dm";
        ret = d1l_read_state_mark_dm_read();
    } else if (strncmp(arg, "dm ", 3) == 0) {
        const char *fingerprint = arg + 3;
        while (*fingerprint == ' ') {
            fingerprint++;
        }
        if (parse_fingerprint_token(fingerprint, thread_fingerprint, sizeof(thread_fingerprint))) {
            target = "dm_thread";
            ret = d1l_read_state_mark_dm_thread_read(thread_fingerprint);
        }
    } else if (strcmp(arg, "all") == 0) {
        target = "all";
        ret = d1l_read_state_mark_all_read();
    }
    if (ret != ESP_OK) {
        err_result("messages read",
                   ret == ESP_ERR_INVALID_ARG ? "INVALID_TARGET" : esp_err_to_name(ret),
                   "usage: messages read <public|dm|dm <fingerprint>|all>");
        return;
    }

    d1l_read_state_stats_t stats = d1l_read_state_stats();
    ok_begin("messages read");
    printf(",\"target\":\"%s\"", target);
    if (thread_fingerprint[0] != '\0') {
        printf(",\"fingerprint\":\"%s\"", thread_fingerprint);
    }
    printf(",\"persisted\":true,\"public_unread\":%lu,\"dm_unread\":%lu,\"muted_dm_unread\":%lu,\"dm_thread_count\":%lu,\"last_public_read_seq\":%lu,\"last_dm_read_seq\":%lu}\n",
           (unsigned long)stats.public_unread_count,
           (unsigned long)stats.dm_unread_count,
           (unsigned long)stats.muted_dm_unread_count,
           (unsigned long)stats.dm_thread_count,
           (unsigned long)stats.last_public_read_seq,
           (unsigned long)stats.last_dm_read_seq);
}

static void cmd_nodes(void)
{
    d1l_node_store_stats_t stats = d1l_node_store_stats();
    const bool include_location = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_LOCATION);
    const uint32_t marker_generation = include_location ?
        d1l_node_store_marker_generation() : 0U;
    static d1l_node_view_t entries[8] EXT_RAM_BSS_ATTR;
    const d1l_node_query_t query = {
        .filter = D1L_NODE_FILTER_ALL,
        .sort = D1L_NODE_SORT_LAST_HEARD,
        .text = NULL,
        .keyed_only = false,
        .reachable_only = false,
    };
    size_t copied = d1l_node_store_query(&query, entries, 8);
    ok_begin("nodes");
    printf(",\"count\":%u,\"capacity\":%u,\"active_capacity\":%u,\"sd_history_capacity\":%u,\"total_written\":%lu,\"dropped_oldest\":%lu",
           (unsigned)stats.count, (unsigned)stats.capacity,
           (unsigned)D1L_NODE_RAM_ACTIVE_CAPACITY,
           (unsigned)D1L_NODE_SD_HISTORY_CAPACITY,
           (unsigned long)stats.total_written,
           (unsigned long)stats.dropped_oldest);
    if (include_location) {
        printf(",\"marker_generation\":%lu",
               (unsigned long)marker_generation);
    }
    printf(",\"filter\":\"all\",\"sort\":\"last_heard\",\"entries\":[");
    for (size_t i = 0; i < copied; ++i) {
        const d1l_node_view_t *view = &entries[i];
        const d1l_node_entry_t *e = &view->node;
        printf("%s{\"seq\":%lu,\"first_heard_ms\":%lu,\"last_heard_ms\":%lu,\"advert_timestamp\":%lu,\"heard_count\":%lu,\"fingerprint\":\"%s\",\"public_key\":\"%s\",\"name\":\"%s\",\"display_name\":\"%s\",\"type\":\"%s\",\"role\":\"%s\",\"favorite\":%s,\"muted\":%s,\"keyed\":%s,\"reachable\":%s,\"rssi_dbm\":%d,\"snr_tenths\":%d,\"path_hash_bytes\":%u,\"path_hops\":%u",
               i ? "," : "", (unsigned long)e->seq, (unsigned long)e->first_heard_ms,
               (unsigned long)e->last_heard_ms, (unsigned long)e->advert_timestamp,
               (unsigned long)e->heard_count, e->fingerprint, e->public_key_hex,
               e->name, view->display_name, e->type, view->role,
               bool_json(view->favorite), bool_json(view->muted),
               bool_json(view->keyed), bool_json(view->reachable),
               e->rssi_dbm, e->snr_tenths, e->path_hash_bytes, e->path_hops);
        if (include_location) {
            printf(",\"location_valid\":%s,\"lat_e6\":%ld,\"lon_e6\":%ld,\"location_advert_timestamp\":%lu,\"location_seq\":%lu",
                   bool_json(e->location_valid), (long)e->lat_e6,
                   (long)e->lon_e6,
                   (unsigned long)e->location_advert_timestamp,
                   (unsigned long)e->location_seq);
        }
        printf("}");
    }
    if (include_location) {
        printf("],\"persisted\":true,\"note\":\"Verified MeshCore adverts populate this bounded heard-node store; enriched query rows expose role, favorite, keyed, and reachability state\"}\n");
    } else {
        printf("],\"persisted\":true,\"note\":\"Core node projection exposes verified identity, role, favorite, keyed, reachability, route, and signal fields\"}\n");
    }
}

static void cmd_nodes_clear(void)
{
    esp_err_t ret = d1l_node_store_clear();
    if (ret != ESP_OK) {
        err_result("nodes clear", esp_err_to_name(ret), "could not clear heard-node store");
        return;
    }
    ok_begin("nodes clear");
    printf(",\"persisted\":true,\"count\":0}\n");
}

static const char *channel_source_name(uint8_t source)
{
    switch ((d1l_channel_source_t)source) {
    case D1L_CHANNEL_SOURCE_BUILTIN:
        return "builtin";
    case D1L_CHANNEL_SOURCE_MANUAL:
        return "manual";
    case D1L_CHANNEL_SOURCE_URI_IMPORT:
        return "uri_import";
    case D1L_CHANNEL_SOURCE_MIGRATED:
        return "migrated";
    default:
        return "unknown";
    }
}

static void print_channel_metadata_json(const d1l_channel_info_t *entry,
                                        uint64_t active_channel_id,
                                        bool leading_comma)
{
    printf("%s{\"channel_id\":\"%016" PRIx64 "\",\"name\":",
           leading_comma ? "," : "", entry->channel_id);
    print_json_string(entry->name);
    printf(",\"source\":");
    print_json_string(channel_source_name(entry->source));
    printf(",\"enabled\":%s,\"selected\":%s,\"unread\":%lu,"
           "\"newest_message_seq\":%lu,\"read_through_seq\":%lu}",
           bool_json(entry->enabled),
           bool_json(entry->channel_id == active_channel_id),
           (unsigned long)entry->unread_count,
           (unsigned long)entry->newest_message_seq,
           (unsigned long)entry->read_through_seq);
}

static void cmd_channels(void)
{
    if (!d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT)) {
        release_unavailable_status(
            "channels", D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT);
        return;
    }
    static d1l_channel_info_t
        entries[D1L_CHANNEL_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
    size_t count = 0U;
    uint64_t active_channel_id = 0U;
    d1l_channel_store_stats_t stats = {0};
    const esp_err_t ret = d1l_app_model_copy_channels(
        entries, D1L_CHANNEL_STORE_CAPACITY, &count, &active_channel_id,
        &stats);
    if (ret != ESP_OK) {
        err_result("channels", esp_err_to_name(ret),
                   "channel metadata is not available");
        return;
    }

    ok_begin("channels");
    printf(",\"count\":%u,\"capacity\":%u,\"revision\":%lu,"
           "\"active_channel_id\":\"%016" PRIx64 "\",\"entries\":[",
           (unsigned)count, (unsigned)D1L_CHANNEL_STORE_CAPACITY,
           (unsigned long)stats.revision, active_channel_id);
    for (size_t i = 0; i < count; ++i) {
        print_channel_metadata_json(&entries[i], active_channel_id, i > 0U);
    }
    printf("],\"persisted\":true,\"secret_material_redacted\":true,"
           "\"public_rf_tx\":false,\"formats_sd\":false}\n");
}

static void cmd_channels_select(const char *line)
{
    const char *arg = line + strlen("channels select ");
    uint64_t channel_id = 0U;
    if (!parse_channel_id_hex(arg, &channel_id)) {
        err_result("channels select", "INVALID_CHANNEL_ID",
                   "usage: channels select <1-16-hex-channel-id>");
        return;
    }

    d1l_channel_info_t selected = {0};
    const esp_err_t ret = d1l_app_model_select_channel(channel_id, &selected);
    if (ret != ESP_OK) {
        err_result("channels select", esp_err_to_name(ret),
                   ret == ESP_ERR_NOT_FOUND ? "channel does not exist" :
                   (ret == ESP_ERR_INVALID_STATE ?
                        "channel is disabled or the store is unavailable" :
                        "channel selection was not persisted"));
        return;
    }

    ok_begin("channels select");
    printf(",\"selected\":");
    print_channel_metadata_json(&selected, selected.channel_id, false);
    printf(",\"persisted\":true,\"secret_material_redacted\":true,"
           "\"public_rf_tx\":false,\"formats_sd\":false}\n");
}

static void cmd_contacts(void)
{
    const d1l_contact_store_stats_t stats_before =
        d1l_contact_store_stats();
    static d1l_contact_entry_t
        entries[D1L_CONTACT_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
    size_t copied = d1l_contact_store_copy_recent(
        entries, D1L_CONTACT_STORE_CAPACITY);
    const d1l_contact_store_stats_t stats = d1l_contact_store_stats();
    if (!contact_store_stats_snapshot_equal(&stats_before, &stats) ||
        copied != stats.count) {
        err_result("contacts", "RETAINED_SNAPSHOT_CHANGED",
                   "contact retained state changed during the bounded snapshot; retry");
        return;
    }
    ok_begin("contacts");
    printf(",\"count\":%u,\"capacity\":%u,\"total_written\":%lu,\"dropped_oldest\":%lu,\"persistence_revision\":%llu,\"persistence_dirty\":%s,\"persistence_last_error\":\"%s\",\"entries\":[",
           (unsigned)stats.count, (unsigned)stats.capacity,
           (unsigned long)stats.total_written, (unsigned long)stats.dropped_oldest,
           (unsigned long long)stats.persistence_revision,
           bool_json(stats.persistence_dirty),
           esp_err_to_name(stats.persistence_last_error));
    for (size_t i = 0; i < copied; ++i) {
        const d1l_contact_entry_t *e = &entries[i];
        printf("%s{\"seq\":%lu,\"created_ms\":%lu,\"updated_ms\":%lu,\"fingerprint\":",
               i ? "," : "", (unsigned long)e->seq, (unsigned long)e->created_ms,
               (unsigned long)e->updated_ms);
        print_json_string(e->fingerprint);
        printf(",\"public_key\":");
        print_json_string(e->public_key_hex);
        printf(",\"alias\":");
        print_json_string(e->alias);
        printf(",\"heard_name\":");
        print_json_string(e->heard_name);
        printf(",\"type\":");
        print_json_string(e->type);
        printf(",\"verification_source\":");
        print_json_string(
            d1l_contact_store_verification_source_name(e->verification_source));
        printf(",\"verified_at_ms\":%lu,\"signed_advert_timestamp\":%lu,\"last_heard_ms\":%lu,\"canonical\":%s,\"can_dm\":%s,\"can_admin\":%s,\"last_rssi_dbm\":%d,\"last_snr_tenths\":%d,\"path_hash_bytes\":%u,\"path_hops\":%u,\"out_path_known\":%s,\"out_path_len\":%u,\"out_path_updated_ms\":%lu,\"favorite\":%s,\"muted\":%s}",
               (unsigned long)e->verified_at_ms,
               (unsigned long)e->signed_advert_timestamp,
               (unsigned long)e->last_heard_ms,
                bool_json(d1l_contact_store_is_canonical(e)),
                bool_json(d1l_contact_store_can_dm(e)),
                bool_json(
                    d1l_release_feature_available(
                        D1L_RELEASE_FEATURE_ADMIN) &&
                    d1l_contact_store_can_admin(e)),
                e->last_rssi_dbm,
               e->last_snr_tenths, e->path_hash_bytes, e->path_hops,
               bool_json(e->out_path_valid), e->out_path_len,
               (unsigned long)e->out_path_updated_ms, bool_json(e->favorite),
               bool_json(e->muted));
    }
    printf("],\"persisted\":%s,\"note\":\"Canonical contacts require a full-key signed advert or validated MeshCore URI import; heard-only placeholders cannot DM\"}\n",
           bool_json(!stats.persistence_dirty &&
                     stats.persistence_last_error == ESP_OK));
}

static const char *contact_import_result_name(d1l_contact_import_result_t result)
{
    switch (result) {
    case D1L_CONTACT_IMPORT_CREATED:
        return "created";
    case D1L_CONTACT_IMPORT_UPDATED:
        return "updated";
    case D1L_CONTACT_IMPORT_PROMOTED_PLACEHOLDER:
        return "promoted";
    case D1L_CONTACT_IMPORT_COLLISION:
        return "collision";
    case D1L_CONTACT_IMPORT_ROLE_CONFLICT:
        return "role_conflict";
    case D1L_CONTACT_IMPORT_FULL:
        return "full";
    case D1L_CONTACT_IMPORT_NONE:
    default:
        return "none";
    }
}

static void print_contact_export_json(const d1l_contact_entry_t *e, bool leading_comma)
{
    char uri[D1L_CONTACT_EXPORT_URI_LEN] = {0};
    esp_err_t ret = d1l_contact_store_export_uri(e, uri, sizeof(uri));
    printf("%s{\"seq\":%lu,\"fingerprint\":",
           leading_comma ? "," : "", (unsigned long)e->seq);
    print_json_string(e->fingerprint);
    printf(",\"alias\":");
    print_json_string(e->alias);
    printf(",\"type\":");
    print_json_string(e->type);
    printf(",\"type_id\":%u,\"public_key\":",
           (unsigned)d1l_contact_store_meshcore_type_id(e->type));
    print_json_string(e->public_key_hex);
    printf(",\"shareable\":%s,\"meshcore_uri\":", bool_json(ret == ESP_OK));
    print_json_string(ret == ESP_OK ? uri : "");
    printf("}");
}

static void cmd_contacts_export(const char *line)
{
    const char *arg = line + strlen("contacts export");
    while (*arg == ' ') {
        arg++;
    }

    if (*arg == '\0') {
        d1l_contact_store_stats_t stats = d1l_contact_store_stats();
        static d1l_contact_entry_t entries[8] EXT_RAM_BSS_ATTR;
        size_t copied = d1l_contact_store_copy_recent(entries, 8);
        ok_begin("contacts export");
        printf(",\"count\":%u,\"capacity\":%u,\"entries\":[",
               (unsigned)stats.count, (unsigned)stats.capacity);
        for (size_t i = 0; i < copied; ++i) {
            print_contact_export_json(&entries[i], i > 0);
        }
        printf("],\"format\":\"meshcore://contact/add\",\"note\":\"QR-compatible contact export requires a retained 64-hex public key\"}\n");
        return;
    }

    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(arg, fingerprint, sizeof(fingerprint))) {
        err_result("contacts export", "INVALID_FINGERPRINT",
                   "usage: contacts export [16-hex-fingerprint]");
        return;
    }

    d1l_contact_entry_t contact = {0};
    if (!d1l_contact_store_find_by_fingerprint(fingerprint, &contact)) {
        err_result("contacts export", "ESP_ERR_NOT_FOUND", "contact is not promoted");
        return;
    }

    char uri[D1L_CONTACT_EXPORT_URI_LEN] = {0};
    esp_err_t ret = d1l_contact_store_export_uri(&contact, uri, sizeof(uri));
    if (ret != ESP_OK) {
        err_result("contacts export", "NO_PUBLIC_KEY",
                   "contact export requires a retained 64-hex public key from a signed advert");
        return;
    }

    ok_begin("contacts export");
    printf(",\"fingerprint\":");
    print_json_string(contact.fingerprint);
    printf(",\"alias\":");
    print_json_string(contact.alias);
    printf(",\"type\":");
    print_json_string(contact.type);
    printf(",\"type_id\":%u,\"public_key\":",
           (unsigned)d1l_contact_store_meshcore_type_id(contact.type));
    print_json_string(contact.public_key_hex);
    printf(",\"meshcore_uri\":");
    print_json_string(uri);
    printf(",\"format\":\"meshcore://contact/add\",\"qr_compatible\":true}\n");
}

static void cmd_contacts_import(const char *line)
{
    const char *uri = line + strlen("contacts import ");
    while (*uri == ' ') {
        uri++;
    }
    const size_t uri_len = strlen(uri);
    if (uri_len == 0U || uri_len > D1L_CONTACT_EXPORT_URI_LEN - 1U) {
        err_result("contacts import", "INVALID_URI",
                   "usage: contacts import <meshcore://contact/add?...>");
        return;
    }

    d1l_contact_import_result_t result = D1L_CONTACT_IMPORT_NONE;
    d1l_contact_entry_t contact = {0};
    const esp_err_t ret = d1l_contact_store_import_uri(
        uri, uri_len, &result, &contact);
    if (ret != ESP_OK) {
        const char *code = ret == ESP_ERR_INVALID_ARG ? "INVALID_URI" :
                           (result == D1L_CONTACT_IMPORT_COLLISION ?
                                "KEY_COLLISION" :
                            (result == D1L_CONTACT_IMPORT_ROLE_CONFLICT ?
                                 "ROLE_CONFLICT" :
                             (result == D1L_CONTACT_IMPORT_FULL ?
                                  "CONTACT_STORE_FULL" :
                                  esp_err_to_name(ret))));
        err_result("contacts import", code,
                   "URI must contain one bounded name, 64-hex public_key, and type 1..4");
        return;
    }

    ok_begin("contacts import");
    printf(",\"persisted\":true,\"result\":");
    print_json_string(contact_import_result_name(result));
    printf(",\"verification_source\":");
    print_json_string(
        d1l_contact_store_verification_source_name(contact.verification_source));
    printf(",\"fingerprint\":");
    print_json_string(contact.fingerprint);
    printf(",\"public_key\":");
    print_json_string(contact.public_key_hex);
    printf(",\"alias\":");
    print_json_string(contact.alias);
    printf(",\"type\":");
    print_json_string(contact.type);
    printf(",\"canonical\":%s,\"can_dm\":%s,\"can_admin\":%s}\n",
           bool_json(d1l_contact_store_is_canonical(&contact)),
           bool_json(d1l_contact_store_can_dm(&contact)),
           bool_json(d1l_contact_store_can_admin(&contact)));
}

#if D1L_ENABLE_QUALIFICATION_HOOKS
static bool core_retained_witness_nvs_only(void)
{
    for (size_t i = 0U; i < D1L_RETAINED_BLOB_STORE_COUNT; ++i) {
        d1l_retained_blob_store_backend_state_t state = {0};
        if (!d1l_retained_blob_store_backend_state(
                (d1l_retained_blob_store_id_t)i, &state) ||
            state.enabled) {
            return false;
        }
    }
    return true;
}

static bool core_retained_public_witness(
    const d1l_message_store_stats_t *expected,
    d1l_message_entry_t *out_entry)
{
    static d1l_message_entry_t
        entries[D1L_MESSAGE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
    if (!expected || !out_entry || expected->count != expected->capacity ||
        expected->public_count != expected->capacity || !expected->loaded ||
        expected->persistence_dirty || expected->nvs_fallback_dirty ||
        expected->nvs_fallback_last_error != ESP_OK ||
        expected->sd_primary_required || expected->sd_primary_dirty ||
        expected->sd_primary_reconcile_pending) {
        return false;
    }
    const size_t copied = d1l_message_store_copy_recent(
        entries, D1L_MESSAGE_STORE_CAPACITY);
    const d1l_message_store_stats_t observed = d1l_message_store_stats();
    if (copied != expected->count ||
        observed.next_seq != expected->next_seq ||
        observed.total_written != expected->total_written ||
        observed.dropped_oldest != expected->dropped_oldest ||
        observed.content_revision != expected->content_revision ||
        observed.persistence_revision != expected->persistence_revision ||
        observed.count != expected->count ||
        observed.public_count != expected->public_count ||
        observed.capacity != expected->capacity) {
        return false;
    }
    for (size_t i = copied; i > 0U; --i) {
        if (entries[i - 1U].seq != expected->next_seq) {
            *out_entry = entries[i - 1U];
            return true;
        }
    }
    return false;
}

static bool core_retained_dm_witness(
    const d1l_dm_store_stats_t *expected,
    d1l_dm_entry_t *out_entry)
{
    static d1l_dm_entry_t
        entries[D1L_DM_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
    if (!expected || !out_entry || expected->count != expected->capacity ||
        expected->count == 0U || !expected->loaded ||
        expected->persistence_dirty || expected->nvs_fallback_dirty ||
        expected->nvs_fallback_last_error != ESP_OK ||
        expected->sd_primary_required || expected->sd_primary_dirty ||
        expected->sd_primary_reconcile_pending) {
        return false;
    }
    const size_t copied = d1l_dm_store_copy_recent(
        entries, D1L_DM_STORE_CAPACITY);
    const d1l_dm_store_stats_t observed = d1l_dm_store_stats();
    if (copied != expected->count ||
        observed.next_seq != expected->next_seq ||
        observed.total_written != expected->total_written ||
        observed.dropped_oldest != expected->dropped_oldest ||
        observed.content_revision != expected->content_revision ||
        observed.persistence_revision != expected->persistence_revision ||
        observed.count != expected->count ||
        observed.capacity != expected->capacity) {
        return false;
    }
    for (size_t i = copied; i > 0U; --i) {
        if (entries[i - 1U].seq != expected->next_seq) {
            *out_entry = entries[i - 1U];
            return true;
        }
    }
    return false;
}

static bool core_retained_contact_witness(
    const d1l_contact_store_stats_t *expected,
    d1l_contact_entry_t *out_entry)
{
    static d1l_contact_entry_t
        entries[D1L_CONTACT_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
    if (!expected || !out_entry || expected->count != expected->capacity ||
        expected->count == 0U || expected->persistence_dirty ||
        expected->persistence_last_error != ESP_OK) {
        return false;
    }
    const size_t copied = d1l_contact_store_copy_recent(
        entries, D1L_CONTACT_STORE_CAPACITY);
    const d1l_contact_store_stats_t observed = d1l_contact_store_stats();
    if (copied != expected->count ||
        observed.next_seq != expected->next_seq ||
        observed.total_written != expected->total_written ||
        observed.dropped_oldest != expected->dropped_oldest ||
        observed.persistence_revision != expected->persistence_revision ||
        observed.count != expected->count ||
        observed.capacity != expected->capacity) {
        return false;
    }
    for (size_t i = 0U; i < copied; ++i) {
        if (d1l_contact_store_is_canonical(&entries[i]) &&
            d1l_contact_store_can_dm(&entries[i]) &&
            d1l_contact_store_has_export_key(&entries[i])) {
            *out_entry = entries[i];
            return true;
        }
    }
    return false;
}

/*
 * A release diagnostic must never consume or evict retained user capacity.
 * This exact-candidate fallback therefore accepts only three already-full,
 * clean NVS stores and selects one stable durable witness from each. It never
 * calls a retained mutation, global flush, MeshCore send, storage manager, SD
 * status refresh, or RP2040 bridge path.
 */
static void cmd_core_retained_witness(const char *line)
{
    static const char prefix[] = "core retained-witness ";
    char token[32] = {0};
    if (strncmp(line, prefix, strlen(prefix)) != 0 ||
        !copy_storage_token(
            token, sizeof(token), line + strlen(prefix))) {
        err_result("core retained-witness", "INVALID_TOKEN",
                   "usage: core retained-witness <token-with-a-z-0-9-dot-dash-underscore>");
        return;
    }

    if (!d1l_release_profile_is_core() ||
        d1l_release_sd_history_mode() != D1L_SD_HISTORY_MODE_DISABLED) {
        err_result("core retained-witness", "ESP_ERR_NOT_SUPPORTED",
                   "requires the Core 1.0 release profile with SD history disabled");
        return;
    }
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_RETAINED_NVS) ||
        !d1l_release_feature_available(D1L_RELEASE_FEATURE_PUBLIC_MESSAGES) ||
        !d1l_release_feature_available(D1L_RELEASE_FEATURE_DIRECT_MESSAGES) ||
        !d1l_release_feature_available(D1L_RELEASE_FEATURE_BASIC_CONTACTS)) {
        err_result("core retained-witness", "ESP_ERR_NOT_SUPPORTED",
                   "required retained Core capabilities are unavailable");
        return;
    }
    if (!d1l_retained_blob_store_nvs_ready() ||
        !d1l_retained_blob_store_nvs_marker_ready() ||
        !d1l_retained_blob_store_nvs_markers_complete() ||
        !d1l_retained_blob_store_nvs_anchor_ready() ||
        !d1l_retained_blob_store_nvs_sentinel_ready() ||
        !core_retained_witness_nvs_only()) {
        err_result("core retained-witness", "RETAINED_NVS_NOT_READY",
                   "all retained stores must be ready on NVS with SD backends disabled");
        return;
    }

    const d1l_contact_store_stats_t contact_before =
        d1l_contact_store_stats();
    const d1l_message_store_stats_t public_before =
        d1l_message_store_stats();
    const d1l_dm_store_stats_t dm_before = d1l_dm_store_stats();
    const bool contact_full =
        contact_before.count == contact_before.capacity;
    const bool public_full =
        public_before.count == public_before.capacity &&
        public_before.public_count == public_before.capacity;
    const bool dm_full = dm_before.count == dm_before.capacity;
    if (!contact_full || !public_full || !dm_full) {
        err_result("core retained-witness", "RETAINED_WITNESS_SET_NOT_FULL",
                   "safe Core proof requires every shared message row to be retained Public plus full DM and contact stores; no retained state was mutated");
        return;
    }

    static const char witness_mode[] = "existing_full_preserved";
    char public_key[D1L_NODE_PUBLIC_KEY_HEX_LEN] = {0};
    d1l_contact_entry_t contact = {0};
    d1l_message_entry_t public_witness = {0};
    d1l_dm_entry_t dm_witness = {0};

    /*
     * This exact-candidate fallback is deliberately witness-only. Resolve all
     * three immutable full-store witnesses without calling any retained-store
     * mutation or global flush path.
     */
    if (!core_retained_contact_witness(&contact_before, &contact)) {
        err_result("core retained-witness", "CONTACT_FULL_NO_DURABLE_WITNESS",
                   "full contact store has no stable canonical DM-capable witness");
        return;
    }
    strncpy(public_key, contact.public_key_hex, sizeof(public_key) - 1U);
    if (!core_retained_public_witness(&public_before, &public_witness)) {
        err_result("core retained-witness", "PUBLIC_FULL_NO_DURABLE_WITNESS",
                   "full Public store has no stable durable witness");
        wipe_console_bytes(public_key, sizeof(public_key));
        wipe_console_bytes(&contact, sizeof(contact));
        return;
    }
    if (!core_retained_dm_witness(&dm_before, &dm_witness)) {
        err_result("core retained-witness", "DM_FULL_NO_DURABLE_WITNESS",
                   "full DM store has no stable durable witness");
        wipe_console_bytes(public_key, sizeof(public_key));
        wipe_console_bytes(&contact, sizeof(contact));
        return;
    }

    char witness_label[D1L_MESSAGE_TEXT_LEN] = {0};
    snprintf(witness_label, sizeof(witness_label),
             "core-retained-witness %s", token);
    const uint32_t public_seq = public_witness.seq;
    const uint32_t dm_seq = dm_witness.seq;
    const d1l_message_store_stats_t public_after =
        d1l_message_store_stats();
    const d1l_dm_store_stats_t dm_after = d1l_dm_store_stats();
    const d1l_contact_store_stats_t contact_after =
        d1l_contact_store_stats();
    if (!message_store_stats_snapshot_equal(&public_before, &public_after) ||
        !dm_store_stats_snapshot_equal(&dm_before, &dm_after) ||
        !contact_store_stats_snapshot_equal(&contact_before, &contact_after) ||
        public_after.persistence_dirty ||
        public_after.nvs_fallback_dirty ||
        public_after.sd_primary_required ||
        dm_after.persistence_dirty || dm_after.nvs_fallback_dirty ||
        dm_after.sd_primary_required || contact_after.persistence_dirty ||
        !core_retained_witness_nvs_only()) {
        err_result("core retained-witness", "RETAINED_WITNESS_SET_CHANGED",
                   "full retained witness set changed during the zero-mutation proof");
        wipe_console_bytes(public_key, sizeof(public_key));
        wipe_console_bytes(&contact, sizeof(contact));
        return;
    }

    ok_begin("core retained-witness");
    printf(",\"token\":");
    print_json_string(token);
    printf(",\"witness_request_label\":");
    print_json_string(witness_label);
    printf(",\"fingerprint\":");
    print_json_string(contact.fingerprint);
    printf(",\"public_key\":");
    print_json_string(public_key);
    printf(",\"public_seq\":%lu,\"dm_seq\":%lu,\"contact_seq\":%lu,"
           "\"public_mode\":",
           (unsigned long)public_seq,
           (unsigned long)dm_seq,
           (unsigned long)contact.seq);
    print_json_string(witness_mode);
    printf(",\"dm_mode\":");
    print_json_string(witness_mode);
    printf(",\"contact_mode\":");
    print_json_string(witness_mode);
    printf(",\"contact_result\":");
    print_json_string(witness_mode);
    printf(",\"witness_only\":true,\"public_mutated\":false,"
           "\"dm_mutated\":false,\"contact_mutated\":false,"
           "\"public_evicted\":false,\"dm_evicted\":false,"
           "\"contact_evicted\":false");
    printf(",\"public_store_count_before\":%u,"
           "\"public_store_count_after\":%u,"
           "\"public_retained_count_before\":%u,"
           "\"public_retained_count_after\":%u,"
           "\"public_capacity\":%u,"
           "\"public_total_written_before\":%lu,"
           "\"public_total_written_after\":%lu,"
           "\"public_dropped_oldest_before\":%lu,"
           "\"public_dropped_oldest_after\":%lu,"
           "\"public_content_revision_before\":%lu,"
           "\"public_content_revision_after\":%lu",
           (unsigned)public_before.count, (unsigned)public_after.count,
           (unsigned)public_before.public_count,
           (unsigned)public_after.public_count,
           (unsigned)public_before.capacity,
           (unsigned long)public_before.total_written,
           (unsigned long)public_after.total_written,
           (unsigned long)public_before.dropped_oldest,
           (unsigned long)public_after.dropped_oldest,
           (unsigned long)public_before.content_revision,
           (unsigned long)public_after.content_revision);
    printf(",\"dm_count_before\":%u,\"dm_count_after\":%u,"
           "\"dm_capacity\":%u,\"dm_total_written_before\":%lu,"
           "\"dm_total_written_after\":%lu,"
           "\"dm_dropped_oldest_before\":%lu,"
           "\"dm_dropped_oldest_after\":%lu,"
           "\"dm_content_revision_before\":%lu,"
           "\"dm_content_revision_after\":%lu",
           (unsigned)dm_before.count, (unsigned)dm_after.count,
           (unsigned)dm_before.capacity,
           (unsigned long)dm_before.total_written,
           (unsigned long)dm_after.total_written,
           (unsigned long)dm_before.dropped_oldest,
           (unsigned long)dm_after.dropped_oldest,
           (unsigned long)dm_before.content_revision,
           (unsigned long)dm_after.content_revision);
    printf(",\"contact_count_before\":%u,\"contact_count_after\":%u,"
           "\"contact_capacity\":%u,\"contact_total_written_before\":%lu,"
           "\"contact_total_written_after\":%lu,"
           "\"contact_dropped_oldest_before\":%lu,"
           "\"contact_dropped_oldest_after\":%lu,"
           "\"contact_persistence_revision_before\":%llu,"
           "\"contact_persistence_revision_after\":%llu",
           (unsigned)contact_before.count, (unsigned)contact_after.count,
           (unsigned)contact_before.capacity,
           (unsigned long)contact_before.total_written,
           (unsigned long)contact_after.total_written,
           (unsigned long)contact_before.dropped_oldest,
           (unsigned long)contact_after.dropped_oldest,
           (unsigned long long)contact_before.persistence_revision,
           (unsigned long long)contact_after.persistence_revision);
    printf(",\"persisted\":true,\"retention\":\"nvs\","
           "\"backend_mode\":\"nvs_disabled\",\"synthetic_local\":false,"
           "\"retained_flush\":\"not_requested_zero_mutation\","
           "\"public_rf_tx\":false,"
           "\"dm_rf_tx\":false,\"sd_access\":false,"
           "\"rp2040_access\":false,\"formats_sd\":false,"
           "\"predecessor_evidence_used\":false}\n");
    wipe_console_bytes(public_key, sizeof(public_key));
    wipe_console_bytes(&contact, sizeof(contact));
}
#endif

static void cmd_contacts_clear(void)
{
    esp_err_t ret = d1l_contact_store_clear();
    if (ret != ESP_OK) {
        err_result("contacts clear", esp_err_to_name(ret), "could not clear contact store");
        return;
    }
    ok_begin("contacts clear");
    printf(",\"persisted\":true,\"count\":0}\n");
}

static void cmd_contacts_add(const char *line)
{
    const char *arg = line + strlen("contacts add ");
    while (*arg == ' ') {
        arg++;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(arg, fingerprint, sizeof(fingerprint))) {
        err_result("contacts add", "INVALID_FINGERPRINT", "usage: contacts add <16-hex-fingerprint> [alias]");
        return;
    }
    const char *alias = arg + strlen(fingerprint);
    while (*alias == ' ') {
        alias++;
    }

    d1l_node_entry_t heard = {0};
    bool heard_found = d1l_node_store_find_by_fingerprint(fingerprint, &heard);
    esp_err_t ret = d1l_contact_store_upsert_from_node(fingerprint, alias, heard_found ? &heard : NULL);
    if (ret != ESP_OK) {
        err_result("contacts add", esp_err_to_name(ret), "could not persist contact");
        return;
    }

    d1l_contact_entry_t contact = {0};
    d1l_contact_store_find_by_fingerprint(fingerprint, &contact);
    ok_begin("contacts add");
    printf(",\"persisted\":true,\"source\":");
    print_json_string(heard_found ? "heard_node" : "manual");
    printf(",\"fingerprint\":");
    print_json_string(contact.fingerprint);
    printf(",\"public_key\":");
    print_json_string(contact.public_key_hex);
    printf(",\"alias\":");
    print_json_string(contact.alias);
    printf(",\"heard_name\":");
    print_json_string(contact.heard_name);
    printf(",\"type\":");
    print_json_string(contact.type);
    printf("}\n");
}

static void cmd_contacts_set(const char *line)
{
    const char *arg = line + strlen("contacts set ");
    while (*arg == ' ') {
        arg++;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(arg, fingerprint, sizeof(fingerprint))) {
        err_result("contacts set", "INVALID_FINGERPRINT",
                   "usage: contacts set <fingerprint> <favorite|mute> <0|1>");
        return;
    }
    arg += strlen(fingerprint);
    while (*arg == ' ') {
        arg++;
    }

    char field[12] = {0};
    size_t field_len = 0;
    while (arg[field_len] != '\0' && !isspace((unsigned char)arg[field_len]) &&
           field_len + 1U < sizeof(field)) {
        field[field_len] = (char)tolower((unsigned char)arg[field_len]);
        field_len++;
    }
    field[field_len] = '\0';
    arg += field_len;
    while (*arg == ' ') {
        arg++;
    }

    char value_text[8] = {0};
    size_t value_len = 0;
    while (arg[value_len] != '\0' && !isspace((unsigned char)arg[value_len]) &&
           value_len + 1U < sizeof(value_text)) {
        value_text[value_len] = (char)tolower((unsigned char)arg[value_len]);
        value_len++;
    }
    value_text[value_len] = '\0';

    bool value = false;
    if (!parse_bool_token(value_text, &value)) {
        err_result("contacts set", "INVALID_VALUE",
                   "usage: contacts set <fingerprint> <favorite|mute> <0|1>");
        return;
    }

    d1l_contact_entry_t contact = {0};
    if (!d1l_contact_store_find_by_fingerprint(fingerprint, &contact)) {
        err_result("contacts set", "ESP_ERR_NOT_FOUND", "contact is not promoted");
        return;
    }
    if (strcmp(field, "favorite") == 0 || strcmp(field, "fav") == 0) {
        contact.favorite = value;
    } else if (strcmp(field, "mute") == 0 || strcmp(field, "muted") == 0) {
        contact.muted = value;
    } else {
        err_result("contacts set", "INVALID_FIELD",
                   "usage: contacts set <fingerprint> <favorite|mute> <0|1>");
        return;
    }

    esp_err_t ret = d1l_contact_store_set_flags(fingerprint, contact.favorite, contact.muted, &contact);
    if (ret != ESP_OK) {
        err_result("contacts set", esp_err_to_name(ret), "could not persist contact flags");
        return;
    }

    ok_begin("contacts set");
    printf(",\"persisted\":true,\"fingerprint\":");
    print_json_string(contact.fingerprint);
    printf(",\"favorite\":%s,\"muted\":%s}\n",
           bool_json(contact.favorite), bool_json(contact.muted));
}

static void cmd_contacts_rename(const char *line)
{
    const char *arg = line + strlen("contacts rename ");
    while (*arg == ' ') {
        arg++;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(arg, fingerprint, sizeof(fingerprint))) {
        err_result("contacts rename", "INVALID_FINGERPRINT",
                   "usage: contacts rename <fingerprint> <alias>");
        return;
    }
    const char *alias = arg + strlen(fingerprint);
    while (*alias == ' ') {
        alias++;
    }
    if (alias[0] == '\0') {
        err_result("contacts rename", "EMPTY_ALIAS",
                   "usage: contacts rename <fingerprint> <alias>");
        return;
    }

    d1l_contact_entry_t contact = {0};
    esp_err_t ret = d1l_contact_store_rename(fingerprint, alias, &contact);
    if (ret != ESP_OK) {
        err_result("contacts rename", esp_err_to_name(ret), "could not persist contact alias");
        return;
    }

    ok_begin("contacts rename");
    printf(",\"persisted\":true,\"fingerprint\":");
    print_json_string(contact.fingerprint);
    printf(",\"alias\":");
    print_json_string(contact.alias);
    printf(",\"updated_ms\":%lu}\n", (unsigned long)contact.updated_ms);
}

static void cmd_contacts_delete(const char *line)
{
    const char *arg = line + strlen("contacts delete ");
    while (*arg == ' ') {
        arg++;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(arg, fingerprint, sizeof(fingerprint))) {
        err_result("contacts delete", "INVALID_FINGERPRINT",
                   "usage: contacts delete <fingerprint>");
        return;
    }

    d1l_contact_entry_t contact = {0};
    esp_err_t ret = d1l_contact_store_delete(fingerprint, &contact);
    if (ret != ESP_OK) {
        err_result("contacts delete", esp_err_to_name(ret), "could not delete contact");
        return;
    }

    d1l_contact_store_stats_t stats = d1l_contact_store_stats();
    ok_begin("contacts delete");
    printf(",\"persisted\":true,\"fingerprint\":");
    print_json_string(contact.fingerprint);
    printf(",\"alias\":");
    print_json_string(contact.alias);
    printf(",\"count\":%u,\"history_retained\":true}\n",
           (unsigned)stats.count);
}

static void cmd_mesh_send_dm(const char *line)
{
    const char *arg = line + strlen("mesh send dm ");
    while (*arg == ' ') {
        arg++;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(arg, fingerprint, sizeof(fingerprint))) {
        err_result("mesh send dm", "INVALID_FINGERPRINT", "usage: mesh send dm <16-hex-fingerprint> <text>");
        return;
    }
    const char *text = arg + strlen(fingerprint);
    while (*text == ' ') {
        text++;
    }
    if (text[0] == '\0') {
        err_result("mesh send dm", "EMPTY_MESSAGE", "usage: mesh send dm <16-hex-fingerprint> <text>");
        return;
    }
    const d1l_user_text_info_t text_info = d1l_user_text_validate(text);
    if (text_info.result != D1L_USER_TEXT_OK) {
        err_result("mesh send dm",
                   text_info.result == D1L_USER_TEXT_TOO_LONG ?
                       "MESSAGE_TOO_LONG" : "INVALID_MESSAGE",
                   text_info.result == D1L_USER_TEXT_TOO_LONG ?
                       "max 138 UTF-8 bytes" :
                       "message must be valid printable UTF-8");
        return;
    }
    esp_err_t ret = d1l_meshcore_service_send_dm(fingerprint, text);
    if (ret != ESP_OK) {
        const char *hint =
            ret == ESP_ERR_INVALID_SIZE ? "max 138 UTF-8 bytes" :
            ret == ESP_ERR_NOT_FINISHED ?
                "DM storage is busy; message was not transmitted, retry" :
            ret == ESP_ERR_NOT_FOUND ?
                "DM contact not found; receive or import its signed advert" :
            ret == ESP_ERR_TIMEOUT ?
                "DM request timed out before radio admission; retry" :
            ret == ESP_ERR_INVALID_STATE ?
                "DM is unavailable or another delivery is active" :
                "DM could not be queued; message was not transmitted";
        err_result("mesh send dm",
                   ret == ESP_ERR_INVALID_SIZE ? "MESSAGE_TOO_LONG" : esp_err_to_name(ret),
                   hint);
        return;
    }
    ok_begin("mesh send dm");
    printf(",\"queued\":true,\"fingerprint\":\"%s\"}\n", fingerprint);
}

static void print_route_entry_json(const d1l_route_entry_t *e)
{
    printf("{\"seq\":%lu,\"first_seen_ms\":%lu,\"last_seen_ms\":%lu,\"seen_count\":%lu,\"target\":\"%s\",\"label\":\"%s\",\"kind\":\"%s\",\"route\":\"%s\",\"direction\":\"%s\",\"last_rssi_dbm\":%d,\"last_snr_tenths\":%d,\"path_hash_bytes\":%u,\"path_hops\":%u,\"confidence\":%u,\"payload_len\":%u}",
           (unsigned long)e->seq, (unsigned long)e->first_seen_ms,
           (unsigned long)e->last_seen_ms, (unsigned long)e->seen_count,
           e->target, e->label, e->kind, e->route, e->direction,
           e->last_rssi_dbm, e->last_snr_tenths, e->path_hash_bytes,
           e->path_hops, e->confidence, e->payload_len);
}

static void cmd_routes(void)
{
    d1l_route_store_stats_t stats = d1l_route_store_stats();
    static d1l_route_entry_t entries[8] EXT_RAM_BSS_ATTR;
    size_t copied = d1l_route_store_copy_recent(entries, 8);
    ok_begin("routes");
    printf(",\"count\":%u,\"capacity\":%u,\"total_written\":%lu,\"dropped_oldest\":%lu,"
           "\"persistence\":{\"revision\":%llu,\"commit_count\":%lu,"
           "\"coalesced_count\":%lu,\"fail_count\":%lu,"
           "\"stale_snapshot_count\":%lu,\"dirty\":%s,"
           "\"clear_failure_latched\":%s,\"clear_fail_count\":%lu,"
           "\"clear_last_error\":\"%s\","
           "\"sd_primary\":{\"required\":%s,\"backend_generation\":%lu,\"dirty\":%s,\"reconcile_pending\":%s,\"commit_count\":%lu,"
           "\"fail_count\":%lu,\"last_error\":\"%s\"},"
           "\"nvs_fallback\":{\"dirty\":%s,\"commit_count\":%lu,"
           "\"fail_count\":%lu,\"last_error\":\"%s\"}},\"entries\":[",
           (unsigned)stats.count, (unsigned)stats.capacity,
           (unsigned long)stats.total_written, (unsigned long)stats.dropped_oldest,
           (unsigned long long)stats.persistence_revision,
           (unsigned long)stats.persistence_commit_count,
           (unsigned long)stats.persistence_coalesced_count,
           (unsigned long)stats.persistence_fail_count,
           (unsigned long)stats.persistence_stale_snapshot_count,
           bool_json(stats.persistence_dirty),
           bool_json(stats.clear_failure_latched),
           (unsigned long)stats.clear_fail_count,
           esp_err_to_name(stats.clear_last_error),
           bool_json(stats.sd_primary_required),
           (unsigned long)stats.sd_backend_generation,
           bool_json(stats.sd_primary_dirty),
           bool_json(stats.sd_primary_reconcile_pending),
           (unsigned long)stats.sd_primary_commit_count,
           (unsigned long)stats.sd_primary_fail_count,
           esp_err_to_name(stats.sd_primary_last_error),
           bool_json(stats.nvs_fallback_dirty),
           (unsigned long)stats.nvs_fallback_commit_count,
           (unsigned long)stats.nvs_fallback_fail_count,
           esp_err_to_name(stats.nvs_fallback_last_error));
    for (size_t i = 0; i < copied; ++i) {
        printf("%s", i ? "," : "");
        print_route_entry_json(&entries[i]);
    }
    printf("],\"persisted\":%s,\"note\":\"Routes are learned from MeshCore path metadata on Public and advert packets\"}\n",
           bool_json(!stats.persistence_dirty));
}

static void cmd_routes_detail(const char *line)
{
    const char *arg = line + strlen("routes detail ");
    while (*arg == ' ') {
        arg++;
    }
    char *end = NULL;
    unsigned long seq = strtoul(arg, &end, 10);
    while (end && *end == ' ') {
        end++;
    }
    if (arg[0] == '\0' || end == arg || (end && *end != '\0') || seq == 0 || seq > UINT32_MAX) {
        err_result("routes detail", "INVALID_SEQ", "usage: routes detail <seq>");
        return;
    }

    d1l_route_entry_t entry = {0};
    esp_err_t ret = d1l_route_store_find_by_seq((uint32_t)seq, &entry);
    if (ret != ESP_OK) {
        err_result("routes detail", esp_err_to_name(ret), "route sequence not found");
        return;
    }

    ok_begin("routes detail");
    printf(",\"entry\":");
    print_route_entry_json(&entry);
    printf("}\n");
}

static bool route_trace_is_direct(const d1l_route_entry_t *entry)
{
    return entry && (entry->path_hops == 0 ||
                     strcmp(entry->route, "direct") == 0);
}

static size_t route_trace_best_index(const d1l_route_entry_t *entries, size_t count)
{
    size_t best = 0;
    bool best_set = false;
    for (size_t i = 0; i < count; ++i) {
        const bool candidate_direct = route_trace_is_direct(&entries[i]);
        const bool best_direct = best_set && route_trace_is_direct(&entries[best]);
        if (!best_set ||
            (candidate_direct && !best_direct) ||
            (candidate_direct == best_direct && entries[i].confidence > entries[best].confidence) ||
            (candidate_direct == best_direct && entries[i].confidence == entries[best].confidence &&
             entries[i].seq > entries[best].seq)) {
            best = i;
            best_set = true;
        }
    }
    return best;
}

static void cmd_routes_trace_contact(const char *line)
{
    const char *arg = line + strlen("routes trace contact ");
    while (*arg == ' ') {
        arg++;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(arg, fingerprint, sizeof(fingerprint))) {
        err_result("routes trace contact", "INVALID_FINGERPRINT",
                   "usage: routes trace contact <16-hex-fingerprint>");
        return;
    }
    const char *tail = arg + strlen(fingerprint);
    while (*tail == ' ') {
        tail++;
    }
    if (*tail != '\0') {
        err_result("routes trace contact", "INVALID_FINGERPRINT",
                   "usage: routes trace contact <16-hex-fingerprint>");
        return;
    }

    const esp_err_t ret =
        d1l_meshcore_service_send_trace_contact(fingerprint);
    if (ret != ESP_OK) {
        const char *code = esp_err_to_name(ret);
        const char *detail =
           ret == ESP_ERR_NOT_FOUND ?
                "exact canonical contact not found" :
            ret == ESP_ERR_NOT_SUPPORTED ?
                "contact route has no traceable loop" :
            ret == ESP_ERR_INVALID_SIZE ?
                "derived contact route loop is too long" :
            ret == ESP_ERR_NOT_FINISHED ?
                "one contact TRACE is already pending; poll routes trace status" :
            ret == ESP_ERR_NOT_ALLOWED ?
                "contact TRACE cooldown is active; poll routes trace status for remaining_ms" :
            ret == ESP_ERR_INVALID_STATE ?
                "contact/path is noncanonical, missing, expired, preboot, ambiguous, or another radio operation is active" :
                "could not queue the contact-derived TRACE";
        if (ret == ESP_ERR_NOT_FINISHED) {
            code = "TRACE_PENDING";
        } else if (ret == ESP_ERR_NOT_ALLOWED) {
            code = "TRACE_COOLDOWN";
        }
        err_result("routes trace contact", code, detail);
        return;
    }

    d1l_meshcore_trace_snapshot_t trace = {0};
    d1l_meshcore_service_trace_snapshot(&trace);
    const uint32_t tag = trace.pending ? trace.pending_tag : trace.last_tag;
    const uint8_t path_hops = trace.pending ?
        trace.pending_path_hops : trace.last_path_hops;
    const uint8_t path_hash_bytes = trace.pending ?
        trace.pending_path_hash_bytes : trace.last_path_hash_bytes;
    const size_t path_bytes = (size_t)path_hash_bytes * path_hops;
    const uint8_t *path_hashes = trace.pending ?
        trace.pending_path_hashes : trace.last_path_hashes;
    ok_begin("routes trace contact");
    printf(",\"fingerprint\":\"%s\",\"queued\":true,\"pending\":%s,\"tag\":%lu,\"tag_hex\":\"%08lX\",\"path_hash_bytes\":%u,\"path_hops\":%u,\"path_hashes_hex\":",
           fingerprint, bool_json(trace.pending), (unsigned long)tag,
           (unsigned long)tag, (unsigned)path_hash_bytes,
           (unsigned)path_hops);
    print_hex_bytes_json(path_hashes, path_bytes);
    printf(",\"route\":\"direct\",\"route_source\":\"current_boot_proven_contact_path\",\"flags\":%u,\"timeout_ms\":%u,\"cooldown_ms\":%u,\"contact_trace_supported\":true,\"operator_path_accepted\":false,\"public_rf_tx\":false,\"targeted_trace_rf_tx\":true,\"hardware_verified\":false,\"note\":\"Runtime derived the immutable loop from the exact canonical contact and current proven path; repeater/room routes include the contact hash while chat/sensor routes pivot at the farthest proven repeater\"}\n",
           path_hash_bytes == 2U ? 1U : 0U,
           D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS,
           D1L_MESHCORE_TRACE_COOLDOWN_MS);
}

static void cmd_trace_status(const char *command)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE)) {
        release_unavailable_status(
            command, D1L_RELEASE_FEATURE_USER_TRACE);
        return;
    }
    d1l_meshcore_service_status_t status = d1l_meshcore_service_status();
    d1l_meshcore_trace_snapshot_t trace = {0};
    d1l_meshcore_service_trace_snapshot(&trace);
    const char *retention_note =
        !trace.last_result_valid ?
            "No matched TRACE result is available" :
        !trace.last_retention_attempted ?
            "Matched TRACE retention outcome is pending" :
        trace.last_route_summary_accepted && trace.last_packet_preview_retained ?
            "Route summary accepted for deferred flush and packet preview retention succeeded" :
        trace.last_route_summary_accepted ?
            "Route summary accepted for deferred flush but packet preview retention failed" :
        trace.last_packet_preview_retained ?
            "Route summary store rejected the result but packet preview retention succeeded" :
            "Route summary and packet preview retention both failed";

    ok_begin(command);
    printf(",\"counters\":{\"tx_queued\":%lu,\"rx_matched\":%lu,\"rx_duplicates\":%lu,\"pending_expired\":%lu,\"no_response\":%lu,\"rx_expired\":%lu,\"rx_unmatched\":%lu,\"rx_correlation_code_mismatch\":%lu,\"rx_path_mismatch\":%lu,\"rx_malformed\":%lu,\"rx_source_ignored\":%lu,\"rx_in_flight_ignored\":%lu,\"rx_unsupported\":%lu},\"pending\":{\"active\":%s,\"expired\":%s,\"tag\":%lu,\"age_ms\":%lu,\"path_hash_bytes\":%u,\"path_hops\":%u,\"path_hashes_hex\":",
           (unsigned long)status.trace_tx_queued,
           (unsigned long)status.trace_rx_matched,
           (unsigned long)status.trace_rx_duplicates,
           (unsigned long)status.trace_pending_expired,
           (unsigned long)status.trace_pending_expired,
           (unsigned long)status.trace_rx_expired,
           (unsigned long)status.trace_rx_unmatched,
           (unsigned long)status.trace_rx_auth_mismatch,
           (unsigned long)status.trace_rx_path_mismatch,
           (unsigned long)status.trace_rx_malformed,
           (unsigned long)status.trace_rx_source_ignored,
           (unsigned long)status.trace_rx_in_flight_ignored,
           (unsigned long)status.trace_rx_unsupported,
           bool_json(trace.pending), bool_json(trace.pending_expired),
           (unsigned long)trace.pending_tag,
           (unsigned long)trace.pending_age_ms,
           trace.pending_path_hash_bytes,
           trace.pending_path_hops);
    print_hex_bytes_json(
        trace.pending_path_hashes,
        (size_t)trace.pending_path_hash_bytes * trace.pending_path_hops);
    printf("},\"last_attempt\":{\"valid\":%s,\"outcome\":\"%s\",\"no_response\":%s,\"tag\":%lu,\"age_ms\":%lu},\"cooldown\":{\"active\":%s,\"duration_ms\":%u,\"remaining_ms\":%lu},\"last_result\":{\"valid\":%s,\"tag\":%lu,\"age_ms\":%lu,\"path_hash_bytes\":%u,\"path_hops\":%u,\"path_hashes_hex\":",
           bool_json(trace.last_attempt_valid),
           d1l_meshcore_trace_outcome_name(trace.last_attempt_outcome),
           bool_json(
               trace.last_attempt_outcome ==
                   D1L_MESHCORE_TRACE_OUTCOME_NO_RESPONSE),
           (unsigned long)trace.last_attempt_tag,
           (unsigned long)trace.last_attempt_age_ms,
           bool_json(trace.cooldown_active),
           D1L_MESHCORE_TRACE_COOLDOWN_MS,
           (unsigned long)trace.cooldown_remaining_ms,
           bool_json(trace.last_result_valid), (unsigned long)trace.last_tag,
           (unsigned long)trace.last_age_ms, trace.last_path_hash_bytes,
           trace.last_path_hops);
    print_hex_bytes_json(
        trace.last_path_hashes,
        (size_t)trace.last_path_hash_bytes * trace.last_path_hops);
    printf(",\"path_snrs_quarter_db\":[");
    for (size_t i = 0; i < trace.last_path_hops; ++i) {
        printf("%s%d", i ? "," : "",
               (int)trace.last_path_snrs_quarter_db[i]);
    }
    printf("],\"radio_rssi_dbm\":%d,\"radio_snr_quarter_db\":%d},"
           "\"fingerprint\":\"%s\",\"zero_hop\":%s,\"matched\":%s,"
           "\"retention\":{\"attempted\":%s,\"route_summary_accepted\":%s,"
           "\"route_summary_durable_verified\":false,"
           "\"packet_preview_retained\":%s,\"per_hop_complete\":false,"
           "\"packet_preview_bytes\":%u},"
           "\"correlation_scope\":\"tag_opaque_correlation_code_immutable_contact_loop\","
           "\"last_detail_scope\":\"boot_local\",\"timeout_ms\":%u,"
           "\"flags_zero_direct_only\":false,"
           "\"trace_flags_supported\":[0,1,2,3],"
           "\"trace_wire_hash_bytes_supported\":[1,2,4,8],"
           "\"requires_current_boot_proven_contact_path\":true,"
           "\"contact_trace_supported\":true,"
           "\"operator_path_accepted\":false,\"one_byte_hash_only\":false,"
           "\"contact_trace_path_hash_bytes_supported\":[1,2],"
           "\"contact_route_hash_bytes_accepted\":[1,2,3],"
           "\"contact_route_hash_bytes_normalized_to_two\":[3],"
           "\"contact_route_hash_bytes_rejected\":[],"
           "\"hardware_verified\":false,"
           "\"note\":\"%s; timeout transitions to no_response under owner "
           "maintenance; cooldown is bounded after matched or no_response; "
           "full per-hop detail is boot-local; two-byte contact TRACE and "
           "wider wire parsing remain hardware-unverified\"}\n",
           trace.last_rssi_dbm, trace.last_radio_snr_quarter_db,
           trace.target_fingerprint, bool_json(trace.zero_hop_ping),
           bool_json(
               trace.last_result_valid &&
               trace.last_attempt_outcome == D1L_MESHCORE_TRACE_OUTCOME_MATCHED),
           bool_json(trace.last_retention_attempted),
           bool_json(trace.last_route_summary_accepted),
           bool_json(trace.last_packet_preview_retained),
           D1L_PACKET_LOG_RAW_PREVIEW_BYTES,
           D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS, retention_note);
}

static void cmd_routes_trace_status(void)
{
    cmd_trace_status("routes trace status");
}

static void cmd_repeater_ping(const char *line)
{
    const char *arg = line + strlen("repeater ping ");
    while (*arg == ' ') {
        arg++;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(arg, fingerprint, sizeof(fingerprint))) {
        err_result(
            "repeater ping", "INVALID_FINGERPRINT",
            "usage: repeater ping <16-hex-fingerprint>");
        return;
    }
    const char *tail = arg + strlen(fingerprint);
    while (*tail == ' ') {
        tail++;
    }
    if (*tail != '\0') {
        err_result(
            "repeater ping", "INVALID_FINGERPRINT",
            "usage: repeater ping <16-hex-fingerprint>");
        return;
    }

    const esp_err_t ret =
        d1l_meshcore_service_ping_repeater(fingerprint);
    if (ret != ESP_OK) {
        const char *code = esp_err_to_name(ret);
        const char *detail =
            ret == ESP_ERR_NOT_FOUND ?
                "exact canonical repeater contact not found" :
            ret == ESP_ERR_NOT_SUPPORTED ?
                "target is not a repeater or its identity width is unsupported" :
            ret == ESP_ERR_NOT_FINISHED ?
                "one TRACE or repeater Ping is already pending; poll repeater ping status" :
            ret == ESP_ERR_NOT_ALLOWED ?
                "repeater Ping cooldown is active; poll repeater ping status" :
            ret == ESP_ERR_INVALID_STATE ?
                "repeater contact, identity, radio, or trace state is not ready" :
                "could not queue the zero-hop repeater Ping";
        if (ret == ESP_ERR_NOT_FINISHED) {
            code = "PING_PENDING";
        } else if (ret == ESP_ERR_NOT_ALLOWED) {
            code = "PING_COOLDOWN";
        }
        err_result("repeater ping", code, detail);
        return;
    }

    d1l_meshcore_trace_snapshot_t trace = {0};
    d1l_meshcore_service_trace_snapshot(&trace);
    ok_begin("repeater ping");
    printf(
        ",\"fingerprint\":\"%s\",\"queued\":true,\"pending\":%s,"
        "\"tag\":%lu,\"zero_hop\":true,\"targeted_trace_rf_tx\":true,"
        "\"public_rf_tx\":false,\"status_command\":\"repeater ping status\"}\n",
        fingerprint, bool_json(trace.pending),
        (unsigned long)trace.pending_tag);
}

static void cmd_repeater_ping_status(void)
{
    cmd_trace_status("repeater ping status");
}

static void cmd_routes_trace(const char *line)
{
    const char *arg = line + strlen("routes trace ");
    while (*arg == ' ') {
        arg++;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(arg, fingerprint, sizeof(fingerprint))) {
        err_result("routes trace", "INVALID_FINGERPRINT", "usage: routes trace <16-hex-fingerprint>");
        return;
    }
    const char *tail = arg + strlen(fingerprint);
    while (*tail == ' ') {
        tail++;
    }
    if (*tail != '\0') {
        err_result("routes trace", "INVALID_FINGERPRINT", "usage: routes trace <16-hex-fingerprint>");
        return;
    }

    d1l_contact_entry_t contact = {0};
    const bool known_contact = d1l_contact_store_find_by_fingerprint(fingerprint, &contact);
    const uint8_t out_path_hash_bytes =
        known_contact && contact.out_path_valid ?
        d1l_meshcore_wire_path_hash_size(contact.out_path_len) : 0U;
    const uint8_t out_path_hops =
        known_contact && contact.out_path_valid ?
        d1l_meshcore_wire_path_hash_count(contact.out_path_len) : 0U;
    static d1l_route_entry_t entries[D1L_ROUTE_STORE_CAPACITY] EXT_RAM_BSS_ATTR;
    size_t copied = d1l_app_model_copy_route_trace(fingerprint, entries, D1L_ROUTE_STORE_CAPACITY);
    const size_t best = copied ? route_trace_best_index(entries, copied) : 0;

    ok_begin("routes trace");
    printf(",\"fingerprint\":\"%s\",\"known_contact\":%s,\"alias\":",
           fingerprint, bool_json(known_contact));
    print_json_string(known_contact && contact.alias[0] ? contact.alias : "");
    printf(",\"public_key_ready\":%s,\"contact_route\":{\"out_path_known\":%s,\"path_hash_bytes\":%u,\"path_hops\":%u,\"trace_hash_bytes\":%u,\"last_rssi_dbm\":%d,\"last_snr_tenths\":%d}",
           bool_json(known_contact && contact.public_key_hex[0]),
           bool_json(known_contact && contact.out_path_valid),
           (unsigned)out_path_hash_bytes,
           (unsigned)out_path_hops,
           (unsigned)d1l_meshcore_trace_contact_hash_width(
               out_path_hash_bytes),
           known_contact ? contact.last_rssi_dbm : 0,
           known_contact ? contact.last_snr_tenths : 0);
    printf(",\"route_count\":%u,\"best_route\":\"%s\",\"best_seq\":%lu,\"best_confidence\":%u,\"best_hops\":%u,\"entries\":[",
           (unsigned)copied,
           copied ? entries[best].route : "unknown",
           copied ? (unsigned long)entries[best].seq : 0UL,
           copied ? entries[best].confidence : 0,
           copied ? entries[best].path_hops : 0);
    for (size_t i = 0; i < copied; ++i) {
        printf("%s", i ? "," : "");
        print_route_entry_json(&entries[i]);
    }
    if (d1l_release_feature_available(D1L_RELEASE_FEATURE_USER_TRACE)) {
        printf("],\"path_discovery_probe_supported\":true,"
               "\"path_discovery_probe_command\":\"routes probe <fingerprint>\","
               "\"real_trace_contact_supported\":true,"
               "\"contact_trace_command\":\"routes trace contact <fingerprint>\","
               "\"contact_trace_requires_current_boot_proven_path\":true,"
               "\"contact_trace_path_hash_bytes_supported\":[1,2],"
               "\"contact_route_hash_bytes_accepted\":[1,2,3],"
               "\"contact_route_hash_bytes_normalized_to_two\":[3],"
               "\"operator_trace_path_accepted\":false,"
               "\"hardware_verified\":false,"
               "\"note\":\"This view combines retained route/contact evidence; "
               "routes probe sends a labelled DM/PATH discovery request while "
               "routes trace contact sends a real contact-derived TRACE packet\"}\n");
    } else {
        printf("],\"release_profile\":");
        print_json_string(d1l_release_profile_name());
        printf(",\"feature\":\"route_signal_read_only\","
               "\"available\":true,\"read_only\":true,"
               "\"path_discovery_probe_supported\":false,"
               "\"real_trace_contact_supported\":false,"
               "\"operator_trace_path_accepted\":false,"
               "\"user_trace_available\":false,\"hardware_verified\":false,"
               "\"note\":\"Retained route and contact evidence only; Core 1.0 "
               "does not expose user TRACE or PATH mutations\"}\n");
    }
}

static void cmd_routes_probe(const char *line)
{
    const char *arg = line + strlen("routes probe ");
    while (*arg == ' ') {
        arg++;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(arg, fingerprint, sizeof(fingerprint))) {
        err_result("routes probe", "INVALID_FINGERPRINT", "usage: routes probe <16-hex-fingerprint>");
        return;
    }
    const char *tail = arg + strlen(fingerprint);
    while (*tail == ' ') {
        tail++;
    }
    if (*tail != '\0') {
        err_result("routes probe", "INVALID_FINGERPRINT", "usage: routes probe <16-hex-fingerprint>");
        return;
    }

    char token[D1L_MESSAGE_TEXT_LEN] = {0};
    esp_err_t ret = d1l_app_model_request_path_discovery_probe(
        fingerprint, token, sizeof(token));
    if (ret != ESP_OK) {
        err_result("routes probe", esp_err_to_name(ret),
                   ret == ESP_ERR_INVALID_STATE ?
                   "path discovery probe is cooling down or radio is busy" :
                   "could not queue DM/PATH discovery probe");
        return;
    }

    ok_begin("routes probe");
    printf(",\"fingerprint\":\"%s\",\"queued\":true,\"token\":\"%s\",\"dm_rf_tx\":true,\"public_rf_tx\":false,\"path_discovery_probe_supported\":true,\"telemetry_requested\":true,\"real_trace_packet\":false,\"note\":\"Encrypted PATH discovery and base-telemetry request queued; correlated results appear in routes telemetry and the contact route screen\"}\n",
           fingerprint, token);
}

static void cmd_routes_telemetry(const char *line)
{
    const char *arg = line + strlen("routes telemetry ");
    while (*arg == ' ') {
        arg++;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(
            arg, fingerprint, sizeof(fingerprint))) {
        err_result(
            "routes telemetry", "INVALID_FINGERPRINT",
            "usage: routes telemetry <16-hex-fingerprint>");
        return;
    }
    const char *tail = arg + strlen(fingerprint);
    while (*tail == ' ') {
        tail++;
    }
    if (*tail != '\0') {
        err_result(
            "routes telemetry", "INVALID_FINGERPRINT",
            "usage: routes telemetry <16-hex-fingerprint>");
        return;
    }

    static d1l_meshcore_contact_telemetry_snapshot_t snapshot
        EXT_RAM_BSS_ATTR;
    d1l_app_model_contact_telemetry_snapshot(
        fingerprint, &snapshot);
    ok_begin("routes telemetry");
    printf(
        ",\"fingerprint\":\"%s\",\"state\":\"%s\","
        "\"pending\":%s,\"pending_tag\":%lu,"
        "\"pending_remaining_ms\":%lu,\"history_scope\":\"boot_local\","
        "\"history_count\":%u,\"history_capacity\":%u,\"entries\":[",
        fingerprint,
        d1l_meshcore_contact_telemetry_state_name(snapshot.state),
        bool_json(snapshot.pending),
        (unsigned long)snapshot.pending_tag,
        (unsigned long)snapshot.pending_remaining_ms,
        (unsigned)snapshot.history_count,
        (unsigned)D1L_MESHCORE_CONTACT_TELEMETRY_HISTORY_CAPACITY);
    for (size_t i = 0U; i < snapshot.history_count; ++i) {
        const d1l_meshcore_contact_telemetry_entry_t *entry =
            &snapshot.history[i];
        printf(
            "%s{\"sequence\":%lu,\"tag\":%lu,\"age_ms\":%lu,"
            "\"rssi_dbm\":%d,\"snr_tenths\":%d,\"path_hops\":%u,"
            "\"field_count\":%u,\"truncated\":%s,\"text\":",
            i ? "," : "", (unsigned long)entry->sequence,
            (unsigned long)entry->tag, (unsigned long)entry->age_ms,
            entry->last_rssi_dbm, entry->last_snr_tenths,
            entry->path_hops, (unsigned)entry->result.count,
            bool_json(entry->result.truncated));
        print_json_string(entry->result.text);
        printf("}");
    }
    printf("],\"public_rf_tx\":false}\n");
}

static void cmd_routes_reset(const char *line)
{
    const char *arg = line + strlen("routes reset ");
    while (*arg == ' ') {
        arg++;
    }
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    if (!parse_fingerprint_token(
            arg, fingerprint, sizeof(fingerprint))) {
        err_result(
            "routes reset", "INVALID_FINGERPRINT",
            "usage: routes reset <16-hex-fingerprint>");
        return;
    }
    const char *tail = arg + strlen(fingerprint);
    while (*tail == ' ') {
        tail++;
    }
    if (*tail != '\0') {
        err_result(
            "routes reset", "INVALID_FINGERPRINT",
            "usage: routes reset <16-hex-fingerprint>");
        return;
    }
    const esp_err_t ret =
        d1l_app_model_reset_contact_route(fingerprint);
    if (ret != ESP_OK) {
        err_result(
            "routes reset", esp_err_to_name(ret),
            "could not forget the selected contact route");
        return;
    }
    ok_begin("routes reset");
    printf(
        ",\"fingerprint\":\"%s\",\"persisted\":true,"
        "\"contact_only\":true,\"identity_preserved\":true,"
        "\"messages_preserved\":true}\n",
        fingerprint);
}

static void cmd_routes_clear(void)
{
    esp_err_t ret = d1l_route_store_clear();
    if (ret != ESP_OK) {
        err_result("routes clear", esp_err_to_name(ret), "could not clear route store");
        return;
    }
    ok_begin("routes clear");
    printf(",\"persisted\":true,\"count\":0}\n");
}

static void print_snr_json(int snr_tenths)
{
    const int snr_abs = snr_tenths < 0 ? -snr_tenths : snr_tenths;
    printf("%s%d.%d", snr_tenths < 0 ? "-" : "", snr_abs / 10, snr_abs % 10);
}

static void print_room_server_json(const d1l_mesh_room_server_t *e)
{
    printf("{\"fingerprint\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"rssi_dbm\":%d,\"snr_tenths\":%d,\"path_hops\":%u,\"heard_count\":%lu,\"last_heard_ms\":%lu}",
           e->fingerprint, e->name, e->type, e->rssi_dbm, e->snr_tenths,
           e->path_hops, (unsigned long)e->heard_count, (unsigned long)e->last_heard_ms);
}

static void print_repeater_candidate_json(const d1l_mesh_repeater_candidate_t *e)
{
    printf("{\"target\":\"%s\",\"label\":\"%s\",\"kind\":\"%s\",\"route\":\"%s\",\"source\":\"%s\",\"rssi_dbm\":%d,\"snr_tenths\":%d,\"path_hops\":%u,\"confidence\":%u,\"seen_count\":%lu,\"last_seen_ms\":%lu}",
           e->target, e->label, e->kind, e->route, e->source, e->rssi_dbm,
           e->snr_tenths, e->path_hops, e->confidence,
           (unsigned long)e->seen_count, (unsigned long)e->last_seen_ms);
}

static void cmd_signal(void)
{
    d1l_mesh_signal_summary_t summary = {0};
    d1l_mesh_inspector_signal_summary(&summary);
    ok_begin("signal");
    printf(",\"sample_count\":%lu,\"rx_packet_samples\":%lu,\"route_samples\":%lu,\"node_samples\":%lu,\"room_server_count\":%lu,\"repeater_candidate_count\":%lu",
           (unsigned long)summary.sample_count, (unsigned long)summary.rx_packet_samples,
           (unsigned long)summary.route_samples, (unsigned long)summary.node_samples,
           (unsigned long)summary.room_server_count,
           (unsigned long)summary.repeater_candidate_count);
    printf(",\"latest\":{\"label\":\"%s\",\"kind\":\"%s\",\"rssi_dbm\":%d,\"snr_tenths\":%d,\"snr_db\":",
           summary.latest_label, summary.latest_kind,
           summary.latest_rssi_dbm, summary.latest_snr_tenths);
    print_snr_json(summary.latest_snr_tenths);
    printf(",\"path_hops\":%u}", summary.latest_path_hops);
    printf(",\"strongest_rssi_dbm\":%d,\"weakest_rssi_dbm\":%d,\"best_snr_tenths\":%d,\"worst_snr_tenths\":%d,\"avg_rssi_dbm\":%d,\"avg_snr_tenths\":%d,\"note\":\"Signal is derived from recent packet, route, and heard-node evidence\"}\n",
           summary.strongest_rssi_dbm, summary.weakest_rssi_dbm,
           summary.best_snr_tenths, summary.worst_snr_tenths,
           summary.avg_rssi_dbm, summary.avg_snr_tenths);
}

static void cmd_roomservers(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN)) {
        release_unavailable_status(
            "roomservers", D1L_RELEASE_FEATURE_ADMIN);
        return;
    }
    d1l_mesh_signal_summary_t summary = {0};
    d1l_mesh_inspector_signal_summary(&summary);
    static d1l_mesh_room_server_t entries[8] EXT_RAM_BSS_ATTR;
    size_t copied = d1l_mesh_inspector_copy_room_servers(entries, 8);
    ok_begin("roomservers");
    printf(",\"count\":%u,\"total_known\":%lu,\"entries\":[", (unsigned)copied,
           (unsigned long)summary.room_server_count);
    for (size_t i = 0; i < copied; ++i) {
        printf("%s", i ? "," : "");
        print_room_server_json(&entries[i]);
    }
    printf("],\"persisted\":true,\"note\":\"Room servers are derived from signed heard-node adverts with room role\"}\n");
}

static void cmd_repeaters(void)
{
    if (!d1l_release_feature_available(D1L_RELEASE_FEATURE_ADMIN)) {
        release_unavailable_status(
            "repeaters", D1L_RELEASE_FEATURE_ADMIN);
        return;
    }
    d1l_mesh_signal_summary_t summary = {0};
    d1l_mesh_inspector_signal_summary(&summary);
    static d1l_mesh_repeater_candidate_t entries[8] EXT_RAM_BSS_ATTR;
    size_t copied = d1l_mesh_inspector_copy_repeater_candidates(entries, 8);
    ok_begin("repeaters");
    printf(",\"count\":%u,\"total_known\":%lu,\"entries\":[", (unsigned)copied,
           (unsigned long)summary.repeater_candidate_count);
    for (size_t i = 0; i < copied; ++i) {
        printf("%s", i ? "," : "");
        print_repeater_candidate_json(&entries[i]);
    }
    printf("],\"persisted\":true,\"note\":\"Repeater candidates are inferred from nonzero path-hop route and heard-node evidence\"}\n");
}

static const char *admin_state_name(d1l_meshcore_admin_state_t state)
{
    switch (state) {
    case D1L_MESHCORE_ADMIN_IDLE:
        return "idle";
    case D1L_MESHCORE_ADMIN_LOGIN_PENDING:
        return "login_pending";
    case D1L_MESHCORE_ADMIN_AUTHENTICATED:
        return "authenticated";
    case D1L_MESHCORE_ADMIN_STATUS_PENDING:
        return "status_pending";
    case D1L_MESHCORE_ADMIN_MUTATION_PENDING:
        return "mutation_pending";
    case D1L_MESHCORE_ADMIN_CLI_PENDING:
        return "cli_pending";
    case D1L_MESHCORE_ADMIN_QUERY_PENDING:
        return "query_pending";
    case D1L_MESHCORE_ADMIN_TIMED_OUT:
        return "timed_out";
    case D1L_MESHCORE_ADMIN_REJECTED_CREDENTIALS:
        return "rejected_credentials";
    case D1L_MESHCORE_ADMIN_DISCONNECTED:
        return "disconnected";
    case D1L_MESHCORE_ADMIN_UNSUPPORTED_PROTOCOL:
        return "unsupported_protocol";
    case D1L_MESHCORE_ADMIN_RADIO_BUSY:
        return "radio_busy";
    case D1L_MESHCORE_ADMIN_VOLATILE_REPLAY_REJECTED:
        return "volatile_replay_rejected";
    case D1L_MESHCORE_ADMIN_DURABLE_REPLAY_REJECTED:
        return "durable_replay_rejected";
    case D1L_MESHCORE_ADMIN_LOCAL_STORAGE_FAILED:
        return "local_storage_failed";
    default:
        return "invalid";
    }
}

static const char *admin_role_name(d1l_meshcore_admin_role_t role)
{
    switch (role) {
    case D1L_MESHCORE_ADMIN_ROLE_REPEATER:
        return "repeater";
    case D1L_MESHCORE_ADMIN_ROLE_ROOM:
        return "room";
    case D1L_MESHCORE_ADMIN_ROLE_NONE:
    default:
        return "none";
    }
}

static void cmd_admin_status(const char *command)
{
    d1l_meshcore_admin_snapshot_t status = {0};
    d1l_meshcore_service_admin_snapshot(&status);
    ok_begin(command);
    printf(",\"state\":\"%s\",\"role\":\"%s\",\"fingerprint\":\"%s\","
           "\"generation\":%lu,\"permissions\":%u,"
           "\"firmware_level\":%u,\"server_timestamp\":%lu,"
           "\"pending_tag\":%lu,\"pending_mutation\":\"%s\","
           "\"pending_query\":\"%s\",\"pending_query_offset\":%u,"
           "\"last_mutation\":\"%s\",\"last_mutation_success\":%s,"
           "\"cli_reply_valid\":%s,\"cli_reply_redacted\":%s,"
           "\"cli_reply_success\":%s,"
           "\"status_valid\":%s,"
           "\"session_secret_exposed\":false,\"credential_exposed\":false,"
           "\"login_tx_queued\":%lu,\"status_tx_queued\":%lu,"
           "\"query_tx_queued\":%lu,\"query_accepted\":%lu,"
           "\"query_rejected\":%lu,"
           "\"mutation_tx_queued\":%lu,\"mutation_accepted\":%lu,"
           "\"mutation_rejected\":%lu,"
           "\"cli_tx_queued\":%lu,\"cli_accepted\":%lu,"
           "\"cli_rejected\":%lu,"
           "\"response_accepted\":%lu,\"response_unmatched\":%lu,"
           "\"response_malformed\":%lu,\"response_expired\":%lu,"
           "\"response_replayed\":%lu,\"last_error\":\"%s\"",
           admin_state_name(status.state), admin_role_name(status.role),
           status.fingerprint, (unsigned long)status.generation,
           (unsigned)status.permissions, (unsigned)status.firmware_level,
           (unsigned long)status.server_timestamp,
           (unsigned long)status.pending_tag,
           d1l_meshcore_admin_mutation_name(status.pending_mutation),
           d1l_meshcore_admin_query_name(status.pending_query),
           (unsigned)status.pending_query_offset,
           d1l_meshcore_admin_mutation_name(status.last_mutation),
           bool_json(status.last_mutation_success),
           bool_json(status.cli_reply_valid),
           bool_json(status.cli_reply_redacted),
           bool_json(status.cli_reply_success),
           bool_json(status.status_valid),
           (unsigned long)status.login_tx_queued,
           (unsigned long)status.status_tx_queued,
           (unsigned long)status.query_tx_queued,
           (unsigned long)status.query_accepted,
           (unsigned long)status.query_rejected,
           (unsigned long)status.mutation_tx_queued,
           (unsigned long)status.mutation_accepted,
           (unsigned long)status.mutation_rejected,
           (unsigned long)status.cli_tx_queued,
           (unsigned long)status.cli_accepted,
           (unsigned long)status.cli_rejected,
           (unsigned long)status.response_accepted,
           (unsigned long)status.response_unmatched,
           (unsigned long)status.response_malformed,
           (unsigned long)status.response_expired,
           (unsigned long)status.response_replayed,
           esp_err_to_name(status.last_error));
    printf(",\"cli_reply\":");
    print_json_string(status.cli_reply_valid ? status.cli_reply : "");
    printf(",\"query_result\":{\"valid\":%s,\"kind\":\"%s\","
           "\"truncated\":%s,\"offset\":%u,\"total\":%u,\"count\":%u,"
           "\"text\":",
           bool_json(status.query_result.valid),
           d1l_meshcore_admin_query_name(status.query_result.kind),
           bool_json(status.query_result.truncated),
           (unsigned)status.query_result.offset,
           (unsigned)status.query_result.total,
           (unsigned)status.query_result.count);
    print_json_string(
        status.query_result.valid ? status.query_result.text : "");
    printf("}");
    if (status.status_valid) {
        printf(",\"server_status\":{\"battery_millivolts\":%u,"
               "\"tx_queue_length\":%u,\"noise_floor_dbm\":%d,"
               "\"last_rssi_dbm\":%d,\"packets_received\":%lu,"
               "\"packets_sent\":%lu,\"uptime_seconds\":%lu,"
               "\"error_flags\":%u,\"posts_created\":%u,"
               "\"posts_pushed\":%u}",
               (unsigned)status.status.battery_millivolts,
               (unsigned)status.status.tx_queue_length,
               (int)status.status.noise_floor_dbm,
               (int)status.status.last_rssi_dbm,
               (unsigned long)status.status.packets_received,
               (unsigned long)status.status.packets_sent,
               (unsigned long)status.status.uptime_seconds,
               (unsigned)status.status.error_flags,
               (unsigned)status.status.posts_created,
               (unsigned)status.status.posts_pushed);
    } else {
        printf(",\"server_status\":null");
    }
    printf(",\"supported_roles\":[\"repeater\",\"room\"],"
           "\"allowlisted_mutations\":[\"clear_stats\","
           "\"advertise_zero_hop\"],"
           "\"mutating_remote_commands_require_local_confirmation\":true,"
           "\"authenticated_remote_cli\":true,"
           "\"sensitive_cli_input_requires_masking\":true}\n");
}

static void cmd_admin_login(const char *line)
{
    char fingerprint[D1L_NODE_FINGERPRINT_LEN] = {0};
    char password[D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES + 1U] = {0};
    int consumed = 0;
    if (!line ||
        sscanf(line, "admin login %16s %15s%n",
               fingerprint, password, &consumed) != 2 ||
        line[consumed] != '\0' ||
        strlen(fingerprint) != D1L_NODE_FINGERPRINT_LEN - 1U) {
        wipe_console_bytes(password, sizeof(password));
        err_result("admin login", "INVALID_ARGUMENT",
                   "usage: admin login <16-hex-fingerprint> "
                   "<password|<empty>>");
        return;
    }
    for (size_t i = 0U; fingerprint[i] != '\0'; ++i) {
        if (!isxdigit((unsigned char)fingerprint[i])) {
            wipe_console_bytes(password, sizeof(password));
            err_result("admin login", "INVALID_FINGERPRINT",
                       "fingerprint must be exactly 16 hexadecimal characters");
            return;
        }
        fingerprint[i] =
            (char)tolower((unsigned char)fingerprint[i]);
    }
    if (strcmp(password, "<empty>") == 0) {
        wipe_console_bytes(password, sizeof(password));
    }
    const esp_err_t ret =
        d1l_meshcore_service_admin_login(fingerprint, password);
    wipe_console_bytes(password, sizeof(password));
    if (ret != ESP_OK) {
        err_result("admin login", esp_err_to_name(ret),
                   "require a verified repeater or room contact, valid route, and idle admin session");
        return;
    }
    cmd_admin_status("admin login");
}

static void cmd_admin_refresh(void)
{
    const esp_err_t ret =
        d1l_meshcore_service_admin_request_status();
    if (ret != ESP_OK) {
        err_result("admin refresh", esp_err_to_name(ret),
                   "an authenticated idle admin session is required");
        return;
    }
    cmd_admin_status("admin refresh");
}

static void cmd_admin_query(const char *line)
{
    d1l_meshcore_admin_query_t query = D1L_MESHCORE_ADMIN_QUERY_NONE;
    uint16_t offset = 0U;
    const char *command = line;
    if (line && strcmp(line, "admin telemetry") == 0) {
        query = D1L_MESHCORE_ADMIN_QUERY_TELEMETRY;
    } else if (line && strcmp(line, "admin access-list") == 0) {
        query = D1L_MESHCORE_ADMIN_QUERY_ACCESS_LIST;
    } else if (line && strcmp(line, "admin neighbours") == 0) {
        query = D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS;
    } else if (line) {
        unsigned parsed_offset = 0U;
        int consumed = 0;
        if (sscanf(line, "admin neighbours %u%n",
                   &parsed_offset, &consumed) == 1 &&
            line[consumed] == '\0' && parsed_offset <= UINT16_MAX) {
            query = D1L_MESHCORE_ADMIN_QUERY_NEIGHBOURS;
            offset = (uint16_t)parsed_offset;
        }
    }
    if (query == D1L_MESHCORE_ADMIN_QUERY_NONE) {
        err_result(
            "admin query", "INVALID_ARGUMENT",
            "usage: admin <telemetry|access-list|neighbours [offset]>");
        return;
    }
    const esp_err_t ret =
        d1l_meshcore_service_admin_request_query(query, offset);
    if (ret != ESP_OK) {
        err_result(
            command, esp_err_to_name(ret),
            "requires an authenticated compatible server session, "
            "current verified route, permission, and no pending request");
        return;
    }
    cmd_admin_status(command);
}

static void cmd_admin_logout(void)
{
    const esp_err_t ret = d1l_meshcore_service_admin_logout();
    if (ret != ESP_OK) {
        err_result("admin logout", esp_err_to_name(ret),
                   "could not clear volatile admin authority");
        return;
    }
    cmd_admin_status("admin logout");
}

static void cmd_admin_mutation(const char *line)
{
    d1l_meshcore_admin_mutation_t mutation =
        D1L_MESHCORE_ADMIN_MUTATION_NONE;
    const char *command = "admin mutation";
    if (line && strcmp(
            line,
            "admin clear-stats CONFIRM-REMOTE-MUTATION") == 0) {
        mutation = D1L_MESHCORE_ADMIN_MUTATION_CLEAR_STATS;
        command = "admin clear-stats";
    } else if (line && strcmp(
                   line,
                   "admin advertise-zero-hop CONFIRM-REMOTE-MUTATION") ==
                   0) {
        mutation = D1L_MESHCORE_ADMIN_MUTATION_ADVERTISE_ZERO_HOP;
        command = "admin advertise-zero-hop";
    } else {
        err_result(
            command, "CONFIRMATION_REQUIRED",
            "usage: admin <clear-stats|advertise-zero-hop> "
            "CONFIRM-REMOTE-MUTATION");
        return;
    }
    const esp_err_t ret =
        d1l_meshcore_service_admin_request_mutation(mutation, true);
    if (ret != ESP_OK) {
        err_result(
            command, esp_err_to_name(ret),
            "requires an authenticated admin-capable room/repeater session, "
            "current verified route, and no pending request");
        return;
    }
    cmd_admin_status(command);
}

static void cmd_admin_cli(const char *line)
{
    static const char PREFIX[] = "admin cli ";
    static const char CONFIRMATION[] = " CONFIRM-REMOTE-MUTATION";
    char remote_command[D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES + 1U] = {0};
    const char *input =
        line && strncmp(line, PREFIX, sizeof(PREFIX) - 1U) == 0 ?
            line + sizeof(PREFIX) - 1U : NULL;
    size_t input_len = input ? strlen(input) : 0U;
    bool confirmed = false;
    const size_t confirmation_len = sizeof(CONFIRMATION) - 1U;
    if (input_len > confirmation_len &&
        strcmp(input + input_len - confirmation_len, CONFIRMATION) == 0) {
        confirmed = true;
        input_len -= confirmation_len;
    }
    if (!input || input_len == 0U ||
        input_len > D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES) {
        err_result(
            "admin cli", "INVALID_ARGUMENT",
            "usage: admin cli <documented-command> "
            "[CONFIRM-REMOTE-MUTATION]");
        return;
    }
    memcpy(remote_command, input, input_len);
    remote_command[input_len] = '\0';
    const d1l_meshcore_admin_cli_policy_t policy =
        d1l_meshcore_admin_cli_command_policy(remote_command);
    if (policy == D1L_MESHCORE_ADMIN_CLI_UNSUPPORTED) {
        wipe_console_bytes(remote_command, sizeof(remote_command));
        err_result(
            "admin cli", "NOT_SUPPORTED",
            "command is not in the pinned role-compatible remote allowlist");
        return;
    }
    if (policy == D1L_MESHCORE_ADMIN_CLI_SENSITIVE) {
        wipe_console_bytes(remote_command, sizeof(remote_command));
        err_result(
            "admin cli", "NOT_ALLOWED",
            "sensitive remote commands require on-device Secure Input");
        return;
    }
    if (policy != D1L_MESHCORE_ADMIN_CLI_READ_ONLY && !confirmed) {
        wipe_console_bytes(remote_command, sizeof(remote_command));
        err_result(
            "admin cli", "CONFIRMATION_REQUIRED",
            "append CONFIRM-REMOTE-MUTATION for a documented remote change");
        return;
    }
    const esp_err_t ret = d1l_meshcore_service_admin_request_cli(
        remote_command, confirmed);
    wipe_console_bytes(remote_command, sizeof(remote_command));
    if (ret != ESP_OK) {
        err_result(
            "admin cli", esp_err_to_name(ret),
            "requires an authenticated admin session, compatible server role, current route, and no pending request");
        return;
    }
    cmd_admin_status("admin cli");
}

static void cmd_admin_room_post(const char *line)
{
    static const char PREFIX[] = "admin room-post ";
    const char *text =
        line && strncmp(line, PREFIX, sizeof(PREFIX) - 1U) == 0 ?
            line + sizeof(PREFIX) - 1U : NULL;
    if (!text || text[0] == '\0') {
        err_result(
            "admin room-post", "INVALID_ARGUMENT",
            "usage: admin room-post <text>");
        return;
    }
    const esp_err_t ret =
        d1l_meshcore_service_admin_send_room_post(text);
    if (ret != ESP_OK) {
        err_result(
            "admin room-post", esp_err_to_name(ret),
            "requires an authenticated room session with write permission");
        return;
    }
    cmd_admin_status("admin room-post");
}

static void print_crash_log_entry_json(const d1l_crash_log_entry_t *e)
{
    printf("{\"seq\":%lu,\"uptime_ms\":%lu,\"reset_reason\":\"%s\",\"reset_reason_code\":%u,\"crash_like\":%s,\"heap_free\":%lu,\"heap_min_free\":%lu,\"psram_free\":%lu}",
           (unsigned long)e->seq, (unsigned long)e->uptime_ms, e->reset_reason,
           e->reset_reason_code, bool_json(e->crash_like),
           (unsigned long)e->heap_free, (unsigned long)e->heap_min_free,
           (unsigned long)e->psram_free);
}

static void cmd_crashlog(void)
{
    d1l_crash_log_stats_t stats = d1l_crash_log_stats();
    static d1l_crash_log_entry_t
        entries[D1L_CRASH_LOG_CAPACITY] EXT_RAM_BSS_ATTR;
    size_t copied = d1l_crash_log_copy_recent(entries, D1L_CRASH_LOG_CAPACITY);
    ok_begin("crashlog");
    printf(",\"count\":%u,\"capacity\":%u,\"total_written\":%lu,\"dropped_oldest\":%lu,\"entries\":[",
           (unsigned)stats.count, (unsigned)stats.capacity,
           (unsigned long)stats.total_written, (unsigned long)stats.dropped_oldest);
    for (size_t i = 0; i < copied; ++i) {
        printf("%s", i ? "," : "");
        print_crash_log_entry_json(&entries[i]);
    }
    printf("],\"persisted\":true,\"note\":\"Crashlog records recent boot reset reasons and early memory watermarks\"}\n");
}

static void cmd_crashlog_clear(void)
{
    esp_err_t ret = d1l_crash_log_clear();
    if (ret != ESP_OK) {
        err_result("crashlog clear", esp_err_to_name(ret), "could not clear crash/reset log");
        return;
    }
    ok_begin("crashlog clear");
    printf(",\"persisted\":true,\"count\":0}\n");
}

static void print_event_log_entry_json(const d1l_event_log_entry_t *entry)
{
    if (!entry) {
        printf("null");
        return;
    }
    printf("{\"sequence\":%lu,\"uptime_ms\":%lu,\"level\":\"%s\","
           "\"source\":",
           (unsigned long)entry->sequence,
           (unsigned long)entry->uptime_ms,
           d1l_event_log_level_name(entry->level));
    print_json_string(entry->source);
    printf(",\"kind\":");
    print_json_string(entry->kind);
    printf(",\"message\":");
    print_json_string(entry->message);
    printf("}");
}

static void cmd_logs(void)
{
    d1l_event_log_entry_t *entries = heap_caps_calloc(
        D1L_EVENT_LOG_CAPACITY,
        sizeof(*entries),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) {
        err_result("logs", "ESP_ERR_NO_MEM",
                   "event log snapshot buffer unavailable");
        return;
    }
    const size_t copied = d1l_event_log_copy_recent(
        entries, D1L_EVENT_LOG_CAPACITY);
    const d1l_event_log_status_t status = d1l_event_log_status();
    ok_begin("logs");
    printf(",\"redacted\":true,\"count\":%u,\"capacity\":%u,"
           "\"total_written\":%lu,\"dropped_oldest\":%lu,"
           "\"runtime_level\":\"%s\",\"entries\":[",
           (unsigned)copied, (unsigned)status.capacity,
           (unsigned long)status.total_written,
           (unsigned long)status.dropped_oldest,
           d1l_event_log_level_name(status.runtime_level));
    for (size_t i = 0U; i < copied; ++i) {
        printf("%s", i ? "," : "");
        print_event_log_entry_json(&entries[i]);
    }
    printf("],\"arbitrary_shell\":false,\"secrets_included\":false}\n");
    heap_caps_free(entries);
}

static void cmd_logs_clear(const char *line)
{
    if (!line ||
        strcmp(line, "logs clear CONFIRM-CLEAR-LOGS") != 0) {
        err_result("logs clear", "CONFIRMATION_REQUIRED",
                   "usage: logs clear CONFIRM-CLEAR-LOGS");
        return;
    }
    const esp_err_t ret = d1l_event_log_clear();
    if (ret != ESP_OK) {
        err_result("logs clear", esp_err_to_name(ret),
                   "could not clear local event history");
        return;
    }
    ok_begin("logs clear");
    printf(",\"cleared\":true,\"local_confirmation\":true,"
           "\"arbitrary_shell\":false}\n");
}

static void cmd_terminal_status(void)
{
    const d1l_event_log_status_t status = d1l_event_log_status();
    ok_begin("terminal status");
    printf(",\"mode\":\"allowlisted_application_commands\","
           "\"default_read_only\":true,\"arbitrary_shell\":false,"
           "\"rf_guard_bypass\":false,\"format_guard_bypass\":false,"
           "\"update_guard_bypass\":false,\"runtime_level\":\"%s\","
           "\"event_count\":%u,\"event_capacity\":%u,"
           "\"mutable_actions\":[\"terminal level error\","
           "\"terminal level warn\",\"terminal level info\","
           "\"terminal level debug\",\"logs clear CONFIRM-CLEAR-LOGS\"]}\n",
           d1l_event_log_level_name(status.runtime_level),
           (unsigned)status.count, (unsigned)status.capacity);
}

static void cmd_terminal_level(const char *line)
{
    const char *prefix = "terminal level ";
    const char *value = line ? line + strlen(prefix) : NULL;
    d1l_event_log_level_t level = D1L_EVENT_LOG_LEVEL_INFO;
    if (!line || strncmp(line, prefix, strlen(prefix)) != 0 ||
        !d1l_event_log_level_from_name(value, &level)) {
        err_result("terminal level", "INVALID_LEVEL",
                   "usage: terminal level <error|warn|info|debug>");
        return;
    }
    const esp_err_t ret = d1l_event_log_set_runtime_level(level);
    if (ret != ESP_OK) {
        err_result("terminal level", esp_err_to_name(ret),
                   "could not set runtime log level");
        return;
    }
    ok_begin("terminal level");
    printf(",\"runtime_level\":\"%s\",\"persisted\":false,"
           "\"arbitrary_shell\":false}\n",
           d1l_event_log_level_name(level));
}

static void print_observer_status(const char *command)
{
    d1l_observer_status_t status = {0};
    d1l_observer_status(&status);
    ok_begin(command);
    printf(",\"state\":\"%s\",\"initialized\":%s,\"configured\":%s,"
           "\"enabled\":%s,\"connected\":%s,\"include_location\":%s,"
           "\"broker_host\":",
           d1l_observer_state_name(status.state),
           bool_json(status.initialized), bool_json(status.configured),
           bool_json(status.enabled), bool_json(status.connected),
           bool_json(status.include_location));
    print_json_string(status.broker_host);
    printf(",\"topic\":");
    print_json_string(status.topic);
    printf(",\"queue\":{\"count\":%lu,\"capacity\":%lu,"
           "\"queued_total\":%lu,\"published_total\":%lu,"
           "\"acknowledged_total\":%lu,\"dropped_oldest\":%lu},"
           "\"reconnects\":%lu,\"last_message_id\":%lu,"
           "\"last_error\":\"%s\",\"tls_required\":true,"
           "\"broker_identity_verified\":true,\"credentials_redacted\":true,"
           "\"message_text_uploaded\":false,\"rf_forwarding\":false}\n",
           (unsigned long)status.queued,
           (unsigned long)status.queue_capacity,
           (unsigned long)status.queued_total,
           (unsigned long)status.published_total,
           (unsigned long)status.acknowledged_total,
           (unsigned long)status.dropped_oldest,
           (unsigned long)status.reconnects,
           (unsigned long)status.last_message_id,
           esp_err_to_name(status.last_error));
}

static void cmd_observer_configure(const char *line, bool authenticated,
                                   bool include_location)
{
    char uri[D1L_OBSERVER_URI_LEN] = {0};
    char topic[D1L_OBSERVER_TOPIC_LEN] = {0};
    char username[D1L_OBSERVER_USERNAME_LEN] = {0};
    char password[D1L_OBSERVER_PASSWORD_LEN] = {0};
    int consumed = -1;
    const int parsed = authenticated ?
        sscanf(line, "observer configure-auth %191s %95s %63s %95s %n",
               uri, topic, username, password, &consumed) :
        sscanf(line, include_location ?
               "observer configure-location %191s %95s %n" :
               "observer configure %191s %95s %n",
               uri, topic, &consumed);
    const int expected = authenticated ? 4 : 2;
    if (parsed != expected || consumed < 0 || line[consumed] != '\0') {
        wipe_console_bytes(username, sizeof(username));
        wipe_console_bytes(password, sizeof(password));
        err_result("observer configure", "INVALID_ARGUMENT",
                   authenticated ?
                   "usage: observer configure-auth <mqtts-uri> <topic> <username> <password>" :
                   "usage: observer configure[-location] <mqtts-uri> <topic>");
        return;
    }
    const esp_err_t ret = d1l_observer_configure(
        uri, topic, authenticated ? username : "",
        authenticated ? password : "", include_location);
    wipe_console_bytes(username, sizeof(username));
    wipe_console_bytes(password, sizeof(password));
    if (ret != ESP_OK) {
        err_result("observer configure", esp_err_to_name(ret),
                   "require mqtts:// URI, bounded topic, and printable credentials");
        return;
    }
    print_observer_status("observer configure");
}

static void cmd_observer_set_enabled(bool enabled)
{
    const esp_err_t ret = d1l_observer_set_enabled(enabled);
    if (ret != ESP_OK) {
        err_result(enabled ? "observer on" : "observer off",
                   esp_err_to_name(ret),
                   enabled ?
                   "save a TLS broker configuration before enabling uploads" :
                   "could not persist observer disabled state");
        return;
    }
    print_observer_status(enabled ? "observer on" : "observer off");
}

static void cmd_observer_clear(void)
{
    const esp_err_t ret = d1l_observer_clear_configuration();
    if (ret != ESP_OK) {
        err_result("observer clear", esp_err_to_name(ret),
                   "could not clear broker configuration");
        return;
    }
    print_observer_status("observer clear");
}

static void cmd_update_status(void)
{
    d1l_update_status_t status = {0};
    d1l_update_status(&status);
    ok_begin("update status");
    printf(",\"state\":\"%s\",\"initialized\":%s,"
           "\"install_requested\":%s,\"cancel_allowed\":%s,"
           "\"reboot_required\":%s,\"running_image_confirmed\":%s,"
           "\"rollback_enabled\":%s,\"progress_percent\":%u,"
           "\"image_size\":%lu,\"bytes_verified\":%lu,"
           "\"bytes_written\":%lu,\"security_sequence\":%lu,"
           "\"highest_security_sequence\":%lu,\"version\":",
           d1l_update_state_name(status.state),
           bool_json(status.initialized),
           bool_json(status.install_requested),
           bool_json(status.cancel_allowed),
           bool_json(status.reboot_required),
           bool_json(status.running_image_confirmed),
           bool_json(status.rollback_enabled),
           (unsigned)status.progress_percent,
           (unsigned long)status.image_size,
           (unsigned long)status.bytes_verified,
           (unsigned long)status.bytes_written,
           (unsigned long)status.security_sequence,
           (unsigned long)status.highest_security_sequence);
    print_json_string(status.version);
    printf(",\"source_sha\":");
    print_json_string(status.source_sha);
    printf(",\"signer_key_id\":");
    print_json_string(status.signer_key_id);
    printf(",\"running_partition\":");
    print_json_string(status.running_partition);
    printf(",\"target_partition\":");
    print_json_string(status.target_partition);
    printf(",\"last_error\":\"%s\",\"manifest_path\":\"%s\","
           "\"signature_path\":\"%s\",\"image_path\":\"%s\","
           "\"local_confirmation_required\":true,"
           "\"rf_trigger_allowed\":false,"
           "\"partition_table_replacement_in_firmware\":false}\n",
           esp_err_to_name(status.last_error),
           D1L_UPDATE_MANIFEST_PATH, D1L_UPDATE_SIGNATURE_PATH,
           D1L_UPDATE_IMAGE_PATH);
}

static void cmd_update_install(const char *line)
{
    if (!line ||
        strcmp(line,
               "update install CONFIRM-SIGNED-UPDATE") != 0) {
        err_result("update install", "CONFIRMATION_REQUIRED",
                   "usage: update install CONFIRM-SIGNED-UPDATE");
        return;
    }
    const esp_err_t ret = d1l_update_request_install();
    if (ret != ESP_OK) {
        err_result("update install", esp_err_to_name(ret),
                   "update service is busy or unavailable");
        return;
    }
    ok_begin("update install");
    printf(",\"accepted\":true,\"asynchronous\":true,"
           "\"local_confirmation\":true,\"rf_trigger\":false,"
           "\"cancel_before_write_only\":true}\n");
}

static void cmd_update_cancel(void)
{
    const esp_err_t ret = d1l_update_cancel();
    if (ret != ESP_OK) {
        err_result("update cancel", esp_err_to_name(ret),
                   "cancellation is allowed only before flash writing begins");
        return;
    }
    ok_begin("update cancel");
    printf(",\"cancel_requested\":true,\"before_irreversible_stage\":true}\n");
}

static void cmd_update_reboot(const char *line)
{
    if (!line ||
        strcmp(line, "update reboot CONFIRM-REBOOT-UPDATE") != 0) {
        err_result("update reboot", "CONFIRMATION_REQUIRED",
                   "usage: update reboot CONFIRM-REBOOT-UPDATE");
        return;
    }
    const esp_err_t ret = d1l_update_prepare_reboot();
    if (ret != ESP_OK) {
        err_result("update reboot", esp_err_to_name(ret),
                   "verified update was not ready or quiesce failed");
        return;
    }
    ok_begin("update reboot");
    printf(",\"rebooting\":true,\"local_confirmation\":true,"
           "\"target\":\"verified_inactive_ota_slot\"}\n");
    fflush(stdout);
    d1l_update_execute_prepared_reboot();
}

static void cmd_health(void)
{
    d1l_health_snapshot_t h = d1l_health_snapshot();
    ok_begin("health");
    print_release_profile_fields();
    printf(",\"boot_nonce\":%lu,\"uptime_ms\":%lu,\"heap_free\":%lu,\"heap_min_free\":%lu,\"heap_largest_free\":%lu,\"internal_heap_free\":%lu,\"internal_heap_min_free\":%lu,\"internal_heap_largest_free\":%lu,\"dma_heap_free\":%lu,\"dma_heap_largest_free\":%lu,\"psram_free\":%lu,\"psram_min_free\":%lu,\"psram_largest_free\":%lu,\"current_task_stack_free_words\":%lu,\"ui_task_stack_free_words\":%lu,\"retained_task_stack_free_bytes\":%lu,\"lvgl_free_bytes\":%lu,\"lvgl_largest_free_bytes\":%lu,\"lvgl_used_pct\":%u,\"nvs_ready\":%s,\"nvs_error\":\"%s\",\"reset_reason\":\"%s\",\"board_ready\":%s,\"ui_ready\":%s}\n",
           (unsigned long)h.boot_nonce,
           (unsigned long)h.uptime_ms,
           (unsigned long)h.heap_free, (unsigned long)h.heap_min_free,
           (unsigned long)h.heap_largest_free,
           (unsigned long)h.internal_heap_free,
           (unsigned long)h.internal_heap_min_free,
           (unsigned long)h.internal_heap_largest_free,
           (unsigned long)h.dma_heap_free,
           (unsigned long)h.dma_heap_largest_free,
           (unsigned long)h.psram_free, (unsigned long)h.psram_min_free,
           (unsigned long)h.psram_largest_free,
           (unsigned long)h.current_task_stack_free_words,
           (unsigned long)h.ui_task_stack_free_words,
           (unsigned long)h.retained_task_stack_free_bytes,
           (unsigned long)h.lvgl_free_bytes,
           (unsigned long)h.lvgl_largest_free_bytes,
           h.lvgl_used_pct, bool_json(h.nvs_ready), esp_err_to_name(h.nvs_error),
           h.reset_reason,
           d1l_app_model_get()->board_ready ? "true" : "false",
           d1l_app_model_get()->ui_ready ? "true" : "false");
}

static void print_wifi_status_result(const char *command)
{
    d1l_connectivity_status_t status = {0};
    d1l_connectivity_status(&status);
    const bool available = d1l_release_feature_available(
        D1L_RELEASE_FEATURE_WIFI_USER_CONTROL);
    const bool runtime_policy_violation =
        !available &&
        (status.wifi_stack_active || status.wifi_connected ||
         status.wifi_connecting);
    ok_begin(command);
    printf(",\"available\":%s", bool_json(available));
    print_release_profile_fields();
    printf(",\"feature\":\"wifi_user_control\","
           "\"mutation_allowed\":%s,\"setting_enabled\":%s,"
           "\"configured_setting_enabled\":%s,\"build_enabled\":%s,"
           "\"stack_active\":%s,\"connected\":%s,\"connecting\":%s,"
           "\"scan_supported\":%s,\"profile_saved\":%s,"
           "\"profile_count\":%u,\"active_profile\":%u,"
           "\"profile_capacity\":%u,"
           "\"password_saved\":%s,\"runtime_policy_violation\":%s,\"ssid\":",
            bool_json(available),
            bool_json(available && status.wifi_enabled_setting),
            bool_json(status.wifi_enabled_setting),
            bool_json(status.wifi_build_enabled),
            bool_json(status.wifi_stack_active), bool_json(status.wifi_connected),
            bool_json(status.wifi_connecting),
            bool_json(available && status.wifi_scan_supported),
            bool_json(available && status.wifi_profile_saved),
            available ? (unsigned)status.wifi_profile_count : 0U,
            available && status.wifi_profile_count > 0U ?
                (unsigned)status.wifi_active_profile + 1U : 0U,
            (unsigned)D1L_WIFI_PROFILE_CAPACITY,
            bool_json(available && status.wifi_password_saved),
            bool_json(runtime_policy_violation));
    print_json_string(
        available && status.wifi_profile_saved ? status.wifi_ssid : "");
    printf(",\"state\":");
    print_json_string(status.wifi_state ? status.wifi_state : "off");
    printf(",\"ip\":");
    print_json_string(status.wifi_connected && status.wifi_ip ? status.wifi_ip : "");
    printf(",\"rssi_dbm\":%d,\"channel\":%u,\"retry_scheduled\":%s,\"retry_attempt\":%u,\"retry_delay_ms\":%lu,\"user_cancelled\":%s,\"safe_mode\":%s,\"last_disconnect_reason\":%u,\"last_failure_class\":",
           status.wifi_rssi_dbm, (unsigned)status.wifi_channel,
           bool_json(status.wifi_retry_scheduled),
           (unsigned)status.wifi_retry_attempt,
           (unsigned long)status.wifi_retry_delay_ms,
           bool_json(status.wifi_user_cancelled),
           bool_json(status.wifi_safe_mode),
           (unsigned)status.wifi_last_disconnect_reason);
    print_json_string(status.wifi_last_failure_class ?
                      status.wifi_last_failure_class : "none");
    printf(",\"retry_task_stack_high_water_bytes\":%lu,\"boot_guard_ready\":%s,\"boot_guard_recovered\":%s,\"consecutive_crash_boots\":%u,\"crash_loop_detected\":%s,\"last_active_subsystem\":",
           (unsigned long)status.wifi_retry_task_stack_high_water_bytes,
           bool_json(status.wifi_boot_guard_ready),
           bool_json(status.wifi_boot_guard_recovered),
           (unsigned)status.wifi_consecutive_crash_boots,
           bool_json(status.wifi_crash_loop_detected));
    print_json_string(status.wifi_last_active_subsystem ?
                      status.wifi_last_active_subsystem : "none");
    printf(",\"boot_guard_error\":");
    print_json_string(status.wifi_boot_guard_error ?
                      status.wifi_boot_guard_error : "ESP_ERR_INVALID_STATE");
    printf(",\"last_error\":");
    print_json_string(status.wifi_last_error ? status.wifi_last_error : "none");
    printf(",\"policy\":\"%s\",\"live_network\":%s}\n",
           status.coexistence_policy, bool_json(status.wifi_stack_active));
}

static void cmd_wifi_status(void)
{
    print_wifi_status_result("wifi status");
}

static void cmd_wifi_profiles(void)
{
    d1l_wifi_profile_info_t profiles[D1L_WIFI_PROFILE_CAPACITY] = {0};
    uint8_t active_profile = 0U;
    const size_t count = d1l_connectivity_wifi_profiles(
        profiles, D1L_WIFI_PROFILE_CAPACITY, &active_profile);
    ok_begin("wifi profiles");
    printf(",\"count\":%u,\"active_profile\":%u,\"capacity\":%u,"
           "\"passwords_printed\":false,\"profiles\":[",
           (unsigned)count,
           count > 0U ? (unsigned)active_profile + 1U : 0U,
           (unsigned)D1L_WIFI_PROFILE_CAPACITY);
    for (size_t i = 0U; i < count; ++i) {
        if (i > 0U) {
            printf(",");
        }
        printf("{\"index\":%u,\"active\":%s,\"saved\":%s,"
               "\"password_saved\":%s,\"ssid\":",
               (unsigned)i + 1U,
               bool_json(i == active_profile),
               bool_json(profiles[i].saved),
               bool_json(profiles[i].password_saved));
        print_json_string(profiles[i].ssid);
        printf("}");
    }
    printf("]}\n");
}

static void cmd_wifi_off(void)
{
    esp_err_t ret = d1l_connectivity_set_wifi_enabled(false);
    if (ret != ESP_OK) {
        err_result("wifi off", esp_err_to_name(ret),
                   "could not persist disabled state or clear connectivity crash guard");
        return;
    }
    d1l_connectivity_status_t status = {0};
    d1l_connectivity_status(&status);
    ok_begin("wifi off");
    printf(",\"persisted\":true,\"setting_enabled\":%s,\"build_enabled\":%s,\"state\":\"%s\"}\n",
           bool_json(status.wifi_enabled_setting), bool_json(status.wifi_build_enabled),
           status.wifi_state);
}

static void cmd_wifi_on(void)
{
    esp_err_t ret = d1l_connectivity_set_wifi_enabled(true);
    if (ret != ESP_OK) {
        err_result("wifi on",
                   ret == ESP_ERR_NOT_SUPPORTED ?
                   "WIFI_BUILD_DISABLED" :
                   esp_err_to_name(ret),
                   "Wi-Fi runtime could not be enabled on this release build");
        return;
    }
    print_wifi_status_result("wifi on");
}

static void cmd_wifi_save(const char *line)
{
    const char *args = line + strlen("wifi save ");
    while (*args == ' ') {
        args++;
    }
    if (args[0] == '\0') {
        err_result("wifi save", "INVALID_ARG", "usage: wifi save <ssid> [password]");
        return;
    }
    char ssid[D1L_WIFI_SSID_LEN] = {0};
    char password[D1L_WIFI_PASSWORD_LEN] = {0};
    const char *space = strchr(args, ' ');
    const char *password_to_save = NULL;
    size_t ssid_len = space ? (size_t)(space - args) : strlen(args);
    if (ssid_len == 0 || ssid_len >= sizeof(ssid)) {
        err_result("wifi save", "INVALID_SSID", "SSID must be 1-32 printable characters without spaces");
        return;
    }
    memcpy(ssid, args, ssid_len);
    ssid[ssid_len] = '\0';
    if (space) {
        const char *password_arg = space + 1;
        while (*password_arg == ' ') {
            password_arg++;
        }
        if (strlen(password_arg) >= sizeof(password)) {
            err_result("wifi save", "INVALID_PASSWORD", "password must fit in 64 characters");
            return;
        }
        snprintf(password, sizeof(password), "%s", password_arg);
        password_to_save = password;
    }
    esp_err_t ret = d1l_connectivity_save_wifi_profile(ssid, password_to_save);
    wipe_console_bytes(password, sizeof(password));
    if (ret != ESP_OK) {
        err_result("wifi save", esp_err_to_name(ret),
                   "could not fully save or apply Wi-Fi profile; inspect wifi status");
        return;
    }
    d1l_connectivity_status_t status = {0};
    d1l_connectivity_status(&status);
    ok_begin("wifi save");
    printf(",\"persisted\":true,\"profile_saved\":%s,\"password_saved\":%s,\"ssid\":",
           bool_json(status.wifi_profile_saved), bool_json(status.wifi_password_saved));
    print_json_string(status.wifi_ssid);
    printf(",\"public_rf_tx\":false,\"note\":\"Wi-Fi profile saved for the measured runtime path; password is not printed\"}\n");
}

static bool wifi_parse_one_based_profile(
    const char *line, const char *prefix, uint8_t *out_profile_index)
{
    if (!line || !prefix || !out_profile_index) {
        return false;
    }
    const char *arg = line + strlen(prefix);
    if (*arg == '\0') {
        return false;
    }
    char *end = NULL;
    const unsigned long value = strtoul(arg, &end, 10);
    if (end == arg || *end != '\0' ||
        value == 0UL || value > D1L_WIFI_PROFILE_CAPACITY) {
        return false;
    }
    *out_profile_index = (uint8_t)(value - 1UL);
    return true;
}

static void cmd_wifi_select(const char *line)
{
    uint8_t profile_index = 0U;
    if (!wifi_parse_one_based_profile(
            line, "wifi select ", &profile_index)) {
        err_result(
            "wifi select", "INVALID_INDEX",
            "usage: wifi select <1-based-index>");
        return;
    }
    const esp_err_t ret =
        d1l_connectivity_select_wifi_profile(profile_index);
    if (ret != ESP_OK) {
        err_result(
            "wifi select", esp_err_to_name(ret),
            "profile does not exist or could not be persisted/applied");
        return;
    }
    d1l_connectivity_status_t status = {0};
    d1l_connectivity_status(&status);
    ok_begin("wifi select");
    printf(",\"persisted\":true,\"active_profile\":%u,"
           "\"profile_count\":%u,\"ssid\":",
           (unsigned)status.wifi_active_profile + 1U,
           (unsigned)status.wifi_profile_count);
    print_json_string(status.wifi_ssid);
    printf(",\"password_printed\":false,\"public_rf_tx\":false}\n");
}

static void cmd_wifi_delete(const char *line)
{
    uint8_t profile_index = 0U;
    if (!wifi_parse_one_based_profile(
            line, "wifi delete ", &profile_index)) {
        err_result(
            "wifi delete", "INVALID_INDEX",
            "usage: wifi delete <1-based-index>");
        return;
    }
    const esp_err_t ret =
        d1l_connectivity_delete_wifi_profile(profile_index);
    if (ret != ESP_OK) {
        err_result(
            "wifi delete", esp_err_to_name(ret),
            "profile does not exist or could not be deleted/applied");
        return;
    }
    d1l_connectivity_status_t status = {0};
    d1l_connectivity_status(&status);
    ok_begin("wifi delete");
    printf(",\"persisted\":true,\"deleted_profile\":%u,"
           "\"profile_count\":%u,\"active_profile\":%u,\"ssid\":",
           (unsigned)profile_index + 1U,
           (unsigned)status.wifi_profile_count,
           status.wifi_profile_count > 0U ?
               (unsigned)status.wifi_active_profile + 1U : 0U);
    print_json_string(
        status.wifi_profile_saved ? status.wifi_ssid : "");
    printf(",\"setting_enabled\":%s,\"password_printed\":false,"
           "\"public_rf_tx\":false}\n",
           bool_json(status.wifi_enabled_setting));
}

static void cmd_wifi_clear(void)
{
    esp_err_t ret = d1l_connectivity_clear_wifi_profile();
    if (ret != ESP_OK) {
        err_result("wifi clear", esp_err_to_name(ret),
                   "could not fully clear Wi-Fi profile, disconnect runtime, or connectivity crash guard");
        return;
    }
    d1l_connectivity_status_t status = {0};
    d1l_connectivity_status(&status);
    ok_begin("wifi clear");
    printf(",\"persisted\":true,\"profile_saved\":%s,\"setting_enabled\":%s,\"ssid\":\"\"}\n",
           bool_json(status.wifi_profile_saved), bool_json(status.wifi_enabled_setting));
}

static void cmd_wifi_scan(void)
{
    d1l_wifi_scan_result_t scan = {0};
    (void)d1l_connectivity_wifi_scan(&scan);
    d1l_connectivity_status_t status = {0};
    d1l_connectivity_status(&status);
    ok_begin("wifi scan");
    printf(",\"scan_started\":%s,\"setting_enabled\":%s,\"build_enabled\":%s,\"scan_supported\":%s,\"profile_saved\":%s,\"state\":",
           bool_json(scan.scan_started),
           bool_json(status.wifi_enabled_setting), bool_json(status.wifi_build_enabled),
           bool_json(status.wifi_scan_supported), bool_json(status.wifi_profile_saved));
    print_json_string(status.wifi_state ? status.wifi_state : "off");
    printf(",\"reason\":");
    print_json_string(scan.reason ? scan.reason : "unknown");
    printf(",\"total_count\":%u,\"returned_count\":%u,\"truncated\":%s,\"networks\":[",
           (unsigned)scan.total_count, (unsigned)scan.returned_count,
           bool_json(scan.truncated));
    for (uint16_t i = 0; i < scan.returned_count; ++i) {
        if (i > 0U) {
            printf(",");
        }
        printf("{\"ssid\":");
        print_json_string(scan.aps[i].ssid);
        printf(",\"rssi_dbm\":%d,\"channel\":%u,\"auth\":",
               scan.aps[i].rssi_dbm, (unsigned)scan.aps[i].channel);
        print_json_string(scan.aps[i].auth ? scan.aps[i].auth : "unknown");
        printf("}");
    }
    printf("],\"public_rf_tx\":false,\"password_printed\":false}\n");
}

static void cmd_wifi_connect(void)
{
    esp_err_t ret = d1l_connectivity_wifi_connect();
    if (ret != ESP_OK) {
        d1l_connectivity_status_t status = {0};
        d1l_connectivity_status(&status);
        const char *code = esp_err_to_name(ret);
        if (ret == ESP_ERR_INVALID_STATE && !status.wifi_enabled_setting) {
            code = "WIFI_DISABLED";
        } else if (ret == ESP_ERR_INVALID_STATE && !status.wifi_profile_saved) {
            code = "WIFI_PROFILE_REQUIRED";
        } else if (ret == ESP_ERR_NOT_SUPPORTED) {
            code = status.wifi_build_enabled ? "WIFI_RUNTIME_UNAVAILABLE" : "WIFI_BUILD_DISABLED";
        }
        err_result("wifi connect", code,
                   "enable Wi-Fi and save a profile before connecting");
        return;
    }
    d1l_connectivity_status_t status = {0};
    d1l_connectivity_status(&status);
    ok_begin("wifi connect");
    printf(",\"initiated\":true,\"setting_enabled\":%s,\"profile_saved\":%s,\"state\":",
           bool_json(status.wifi_enabled_setting),
           bool_json(status.wifi_profile_saved));
    print_json_string(status.wifi_state ? status.wifi_state : "connecting");
    printf(",\"ssid\":");
    print_json_string(status.wifi_profile_saved ? status.wifi_ssid : "");
    printf(",\"password_printed\":false,\"public_rf_tx\":false}\n");
}

static void cmd_ble_status(void)
{
    d1l_connectivity_status_t status = {0};
    d1l_connectivity_status(&status);
    const bool available =
        d1l_release_feature_available(D1L_RELEASE_FEATURE_BLE);
    ok_begin("ble status");
    printf(",\"available\":%s", bool_json(available));
    print_release_profile_fields();
    printf(",\"feature\":\"ble\",\"mutation_allowed\":%s,"
           "\"setting_enabled\":%s,\"configured_setting_enabled\":%s,"
           "\"build_enabled\":%s,\"stack_active\":%s,"
           "\"runtime_policy_violation\":%s,\"state\":\"%s\","
           "\"policy\":\"%s\"}\n",
           bool_json(available),
           bool_json(
               available && status.ble_companion_enabled_setting),
           bool_json(status.ble_companion_enabled_setting),
           bool_json(status.ble_build_enabled),
           bool_json(status.ble_stack_active),
           bool_json(!available && status.ble_stack_active),
           status.ble_state, status.coexistence_policy);
}

static void cmd_ble_off(void)
{
    esp_err_t ret = d1l_connectivity_set_ble_enabled(false);
    if (ret != ESP_OK) {
        err_result("ble off", esp_err_to_name(ret),
                   "could not persist BLE disabled setting");
        return;
    }
    d1l_connectivity_status_t status = {0};
    d1l_connectivity_status(&status);
    ok_begin("ble off");
    printf(",\"persisted\":true,\"setting_enabled\":%s,\"build_enabled\":%s,\"state\":\"%s\"}\n",
           bool_json(status.ble_companion_enabled_setting), bool_json(status.ble_build_enabled),
           status.ble_state);
}

static void cmd_ble_on(void)
{
    d1l_connectivity_status_t status = {0};
    d1l_connectivity_status(&status);
    esp_err_t ret = d1l_connectivity_set_ble_enabled(true);
    if (ret != ESP_OK) {
        err_result("ble on",
                   ret == ESP_ERR_NOT_SUPPORTED ?
                   (status.ble_build_enabled ? "BLE_RUNTIME_PENDING" : "BLE_BUILD_DISABLED") :
                   esp_err_to_name(ret),
                   "BLE enable/start is pending a later measured connectivity build");
        return;
    }
    cmd_ble_status();
}

static void cmd_help(void)
{
    ok_begin("help");
    if (d1l_release_profile_is_core() &&
        !d1l_release_feature_available(
            D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT)) {
        print_release_profile_fields();
        printf(",\"commands\":[\"help\",\"version\",\"board\","
               "\"settings get\",\"settings reset\","
               "\"time migration status\","
               "\"time migrate-legacy <expected-mesh-ts> "
               "<confirmed-upper-bound> "
               D1L_TIME_PROTOCOL_MIGRATION_CONFIRMATION "\","
               "\"settings set name <name>\","
               "\"settings set timezone <UTC|UTC+HH:MM|UTC-HH:MM>\","
               "\"settings onboarding status\","
               "\"settings onboarding complete <name>\","
               "\"settings onboarding reset\",\"identity status\",\"i2c\","
#if D1L_ENABLE_QUALIFICATION_HOOKS
               "\"display test\",\"touch test\","
#endif
               "\"touch raw\",\"button\","
               "\"backlight <0-100>\",\"radiohw\",\"radio get\","
               "\"radio set preset uscan\",\"radio set freq 910.525\","
               "\"radio set bw 62.5\",\"radio set sf 7\","
               "\"radio set cr 5\",\"radio set txpower 20\","
               "\"radio set rxboost <0|1>\",\"ui status\","
               "\"ui tab <home|messages|nodes|packets|settings>\","
#if D1L_ENABLE_QUALIFICATION_HOOKS
               "\"ui scroll-probe "
               "<home|public_messages|dm_thread|nodes|contact_detail|"
               "contact_options|contact_forget|packets|settings|storage>\","
               "\"ui compose-probe "
               "<public|public-long|dm|dm-long|public-search|dm-search|"
               "packet-search|contact-edit|onboarding>\","
               "\"ui data-canary <token>\",\"ui capture status\","
               "\"ui capture begin\",\"ui capture chunk <offset> <len>\","
               "\"ui capture end\","
#endif
               "\"mesh status\",\"companion status\","
               "\"storage status\",\"storage force-nvs [on]\","
#if D1L_ENABLE_QUALIFICATION_HOOKS
               "\"core retained-witness <token>\","
#endif
               "\"mesh advert zero\",\"mesh advert flood\","
               "\"mesh send public <text>\","
               "\"mesh send dm <fingerprint> <text>\","
               "\"messages public [offset <n>]\","
               "\"messages public search <text> [offset <n>]\","
               "\"messages dm [offset <n>]\","
               "\"messages dm <fingerprint> [offset <n>]\","
               "\"messages unread\","
               "\"messages read <public|dm|dm <fingerprint>|all>\","
               "\"messages clear\",\"messages dm clear\",\"nodes\",\"contacts\","
               "\"contacts export [fingerprint]\","
               "\"contacts import <meshcore-uri>\","
               "\"contacts add <fingerprint> [alias]\","
               "\"contacts rename <fingerprint> <alias>\","
               "\"contacts delete <fingerprint>\","
               "\"contacts set <fingerprint> <favorite|mute> <0|1>\","
               "\"contacts clear\",\"routes\",\"routes detail <seq>\","
               "\"routes trace <fingerprint>\",\"signal\",\"packets\","
               "\"packets filter <any|rx|tx> <any|text|kind>\","
               "\"packets search <text>\",\"packets detail <seq>\","
               "\"packets raw <seq>\",\"health\","
               "\"crashlog\",\"crashlog clear\",\"reboot\","
               "\"factory-reset-status\",\"factory-reset-confirm\"],"
               "\"unavailable_status_commands\":[\"wifi status\","
               "\"ble status\",\"map center\",\"map tiles status\","
               "\"map provider status\","
               "\"channels\",\"routes trace status\",\"roomservers\","
               "\"repeaters\",\"repeater ping status\"],"
               "\"unavailable_features\":[\"map\","
               "\"wifi_user_control\",\"ble\","
               "\"multi_channel_management\",\"admin\","
               "\"observer_mqtt\",\"signed_update\","
               "\"mutable_terminal\",\"location\","
               "\"advanced_qr_emoji\",\"user_trace\"],"
               "\"fixed_default_public\":true,"
               "\"public_rf_tx\":false,\"formats_sd\":false}\n");
        return;
    }
#if D1L_ENABLE_QUALIFICATION_HOOKS
    printf(",\"contact_probe_surfaces\":[\"contact_detail\",\"contact_options\","
           "\"contact_forget\",\"contact_route\"]");
    printf(",\"storage_probe_surfaces\":[\"storage\",\"storage_card\",\"storage_data\"]");
    printf(",\"map_probe_surfaces\":[\"map\",\"map_options\",\"map_location\",\"map_cache\"]");
    printf(",\"map_probes_read_only\":true");
#endif
    printf(",\"commands\":[\"help\",\"version\",\"board\",\"settings get\",\"settings reset\","
            "\"time migration status\","
            "\"time migrate-legacy <expected-mesh-ts> <confirmed-upper-bound> "
            D1L_TIME_PROTOCOL_MIGRATION_CONFIRMATION "\","
           "\"settings set name <name>\",\"settings set pathhash <1|2|3>\","
           "\"settings set timezone <UTC|UTC+HH:MM|UTC-HH:MM>\","
           "\"settings set location <lat> <lon>\",\"settings clear location\","
           "\"settings onboarding status\",\"settings onboarding complete <name>\","
           "\"settings onboarding reset\",\"identity status\",\"i2c\","
#if D1L_ENABLE_QUALIFICATION_HOOKS
           "\"display test\",\"touch test\","
#endif
           "\"touch raw\",\"button\",\"backlight <0-100>\",\"radiohw\","
           "\"radio get\",\"radio set preset uscan\",\"radio set freq 910.525\","
           "\"radio set bw 62.5\",\"radio set sf 7\",\"radio set cr 5\","
           "\"radio set txpower 20\",\"radio set rxboost <0|1>\",\"ui status\","
           "\"ui tab <home|messages|nodes|map|packets|settings>\","
#if D1L_ENABLE_QUALIFICATION_HOOKS
           "\"ui scroll-probe <home|public_messages|dm_thread|nodes|contact_detail|contact_options|contact_forget|contact_route|mesh_roles|mesh_rooms|mesh_repeaters|packets|settings|storage|storage_card|storage_data|wifi|map|map_options|map_location|map_cache>\","
           "\"ui compose-probe <public|public-long|dm|dm-long|public-search|dm-search|packet-search|contact-edit|onboarding|map-location|wifi-ssid|wifi-password>\","
           "\"ui data-canary <token>\",\"ui capture status\",\"ui capture begin\","
           "\"ui capture chunk <offset> <len>\",\"ui capture end\","
#endif
           "\"map center\","
           "\"map center set <lat> <lon>\",\"map center clear\","
           "\"map tiles status\",\"map provider status\","
#if D1L_ENABLE_QUALIFICATION_HOOKS
           "\"map acceptance open\","
#endif
           "\"mesh status\",\"companion status\",\"rp2040 status\","
           "\"rp2040 set-baud <baud>\",\"rp2040 baud-probe [timeout_ms]\","
           "\"rp2040 ping\",\"rp2040 bootloader\",\"rp2040 stock-probe\","
           "\"rp2040 double-reset [hold_ms gap_ms [settle_ms]]\",\"rp2040 reset\","
           "\"storage status\",\"storage mount\",\"storage remount\","
           "\"storage reset-bridge\",\"storage force-nvs [on|off]\",\"storage diag\","
           "\"storage diag raw\",\"storage map-policy\",\"storage setup\","
#if D1L_ENABLE_QUALIFICATION_HOOKS
           "\"storage filecanary\",\"storage map-tile-canary <token>\","
           "\"storage map-tile-check <token>\",\"storage export-canary <token>\","
           "\"storage retained-canary <token>\","
#endif
           "\"storage export-diagnostics <token>\",\"storage export-data <token>\","
           "\"mesh advert zero\",\"mesh advert flood\","
           "\"mesh send public <text>\",\"mesh send dm <fingerprint> <text>\","
           "\"messages public [offset <n>]\",\"messages public search <text> [offset <n>]\","
           "\"messages dm [offset <n>]\",\"messages dm <fingerprint> [offset <n>]\","
           "\"messages unread\",\"messages read <public|dm|dm <fingerprint>|all>\","
           "\"messages clear\",\"messages dm clear\",\"nodes\",\"nodes clear\","
           "\"channels\",\"channels select <channel-id-hex>\",\"contacts\","
           "\"contacts export [fingerprint]\",\"contacts import <meshcore-uri>\",\"contacts add <fingerprint> [alias]\","
           "\"contacts rename <fingerprint> <alias>\",\"contacts delete <fingerprint>\","
           "\"contacts set <fingerprint> <favorite|mute> <0|1>\",\"contacts clear\","
           "\"routes\",\"routes detail <seq>\",\"routes trace <fingerprint>\","
           "\"routes trace contact <fingerprint>\",\"routes trace status\","
           "\"routes probe <fingerprint>\","
           "\"routes telemetry <fingerprint>\","
           "\"routes reset <fingerprint>\",\"routes clear\",\"packets\","
           "\"packets filter <any|rx|tx> <any|text|kind>\",\"packets search <text>\","
           "\"packets detail <seq>\",\"packets raw <seq>\",\"packets clear\",\"signal\","
           "\"roomservers\",\"repeaters\","
           "\"repeater ping <fingerprint>\",\"repeater ping status\","
           "\"admin status\","
           "\"admin login <fingerprint> <password|<empty>>\",\"admin refresh\","
           "\"admin telemetry\",\"admin neighbours [offset]\","
           "\"admin access-list\","
           "\"admin clear-stats CONFIRM-REMOTE-MUTATION\","
           "\"admin advertise-zero-hop CONFIRM-REMOTE-MUTATION\","
           "\"admin cli <documented-command> [CONFIRM-REMOTE-MUTATION]\","
           "\"admin room-post <text>\","
           "\"admin logout\",\"health\",\"crashlog\",\"crashlog clear\","
           "\"wifi status\",\"wifi profiles\",\"wifi scan\","
           "\"wifi save <ssid> [password]\",\"wifi select <1-based-index>\","
           "\"wifi delete <1-based-index>\",\"wifi connect\",\"wifi clear\","
           "\"wifi on\",\"wifi off\",\"ble status\","
           "\"ble on\",\"ble off\",\"observer status\",\"observer configure "
           "<mqtts-uri> <topic>\",\"observer configure-location "
           "<mqtts-uri> <topic>\",\"observer configure-auth "
           "<mqtts-uri> <topic> <username> <password>\","
           "\"observer on\",\"observer off\",\"observer clear\","
           "\"logs\",\"logs clear CONFIRM-CLEAR-LOGS\","
           "\"terminal status\",\"terminal level <error|warn|info|debug>\","
           "\"update status\",\"update install CONFIRM-SIGNED-UPDATE\","
           "\"update cancel\",\"update reboot CONFIRM-REBOOT-UPDATE\","
           "\"reboot\",\"factory-reset-status\","
           "\"factory-reset-confirm\"]}\n");
}

static void release_factory_reset_quiesces(void)
{
    d1l_rp2040_bridge_quiesce_end();
    d1l_route_store_worker_quiesce_end();
    d1l_storage_manager_quiesce_end();
}

static void restart_with_factory_reset_quiesces(void)
{
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS));
    esp_rom_software_reset_system();
    for (;;) {
    }
}

static void cmd_factory_reset_status(void)
{
    d1l_factory_reset_status_t status = {0};
    const esp_err_t inspect_ret = d1l_factory_reset_inspect(&status);
    const esp_err_t effective_ret = inspect_ret == ESP_OK ?
        status.last_error : inspect_ret;
    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":\"factory-reset-status\","
           "\"phase\":\"%s\",\"journal_found\":%s,\"pending\":%s,"
           "\"complete\":%s,\"attempt\":%lu,\"domains_completed\":%lu,"
           "\"domains_total\":%lu,\"keys_erased\":%lu,"
           "\"keys_already_absent\":%lu,\"failed_domain\":\"%s\","
           "\"failed_domain_index\":",
           D1L_CONSOLE_SCHEMA, bool_json(effective_ret == ESP_OK),
           d1l_factory_reset_phase_name(status.phase),
           bool_json(status.journal_found), bool_json(status.reset_pending),
           bool_json(status.reset_complete),
           (unsigned long)status.attempt_count,
           (unsigned long)status.domains_completed,
           (unsigned long)status.domains_total,
           (unsigned long)status.keys_erased,
           (unsigned long)status.keys_already_absent,
           status.last_failed_domain);
    if (status.last_failed_domain_index == UINT32_MAX) {
        printf("null");
    } else {
        printf("%lu", (unsigned long)status.last_failed_domain_index);
    }
    printf(",\"error\":\"%s\",\"stored_error\":\"%s\","
           "\"inspection_error\":\"%s\",\"sd_lineage_generation\":%lu,"
           "\"sd_lineage_active_mask\":%lu,"
           "\"sd_media_marker_required\":true,\"global_atomic\":false,"
           "\"physical_flash_scrubbed\":false,\"sd_touched\":false,"
           "\"removable_sd_data_preserved\":true,"
           "\"unknown_nvs_keys_preserved\":true,"
           "\"protocol_high_water_preserved\":true,"
           "\"migration_evidence_preserved\":true,"
           "\"time_checkpoint_preserved\":true,"
           "\"crash_forensics_preserved\":true,"
           "\"retained_ownership_evidence_preserved\":true,"
           "\"inventory_entry_count\":%u,\"clear_nvs_key_count\":%u,"
           "\"preserved_raw_marker_slots\":%u}\n",
           esp_err_to_name(effective_ret), esp_err_to_name(status.last_error),
           esp_err_to_name(inspect_ret),
           (unsigned long)status.sd_lineage_generation,
           (unsigned long)status.sd_lineage_active_mask,
           (unsigned int)D1L_FACTORY_RESET_INVENTORY_COUNT,
           (unsigned int)D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT,
           (unsigned int)D1L_FACTORY_RESET_RAW_MARKER_COUNT);
}

static void cmd_factory_reset_confirm(void)
{
    const int64_t started_us = esp_timer_get_time();
    uint32_t remaining_ms = reboot_deadline_remaining_ms(started_us);
    esp_err_t ret = d1l_storage_manager_quiesce_begin(remaining_ms);
    if (ret != ESP_OK) {
        err_result("factory-reset-confirm", esp_err_to_name(ret),
                   "storage manager quiesce failed; no reset was started");
        return;
    }

    remaining_ms = reboot_deadline_remaining_ms(started_us);
    ret = remaining_ms > 0U ?
        d1l_route_store_worker_quiesce_begin(remaining_ms) : ESP_ERR_TIMEOUT;
    if (ret != ESP_OK) {
        d1l_storage_manager_quiesce_end();
        err_result("factory-reset-confirm", esp_err_to_name(ret),
                   "retained producer quiesce failed; no reset was started");
        return;
    }

    remaining_ms = reboot_deadline_remaining_ms(started_us);
    ret = remaining_ms > 0U ?
        d1l_rp2040_bridge_quiesce_begin(remaining_ms) : ESP_ERR_TIMEOUT;
    if (ret != ESP_OK) {
        d1l_route_store_worker_quiesce_end();
        d1l_storage_manager_quiesce_end();
        err_result("factory-reset-confirm", esp_err_to_name(ret),
                   "RP2040 bridge quiesce failed; no reset was started");
        return;
    }

    remaining_ms = reboot_deadline_remaining_ms(started_us);
    if (remaining_ms <= D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS +
                            D1L_REBOOT_RESTART_MARGIN_MS) {
        release_factory_reset_quiesces();
        err_result("factory-reset-confirm", esp_err_to_name(ESP_ERR_TIMEOUT),
                   "reset deadline lacks NVS and console drain headroom");
        return;
    }

    const esp_err_t connectivity_ret = d1l_connectivity_prepare_reboot();
    if (connectivity_ret != ESP_OK) {
        /* Connectivity preparation can fail after retry cancellation or a
         * partial stop. There is no proven in-place restoration boundary, so
         * keep every quiesce and recover the runtime through a clean boot. */
        printf("{\"schema\":%d,\"ok\":false,"
               "\"cmd\":\"factory-reset-confirm\","
               "\"phase\":\"connectivity_prepare_failed\","
               "\"code\":\"%s\",\"partial_reset\":false,"
               "\"reset_scheduled\":false,"
               "\"request_write_may_have_applied\":false,"
               "\"connectivity_prepare\":\"%s\","
               "\"connectivity_state_restored\":false,"
               "\"storage_manager_quiesced\":true,"
               "\"retained_worker_quiesced\":true,"
               "\"rp2040_bridge_quiesced\":true,"
               "\"quiesces_released\":false,"
               "\"recovery\":\"controlled_restart\","
               "\"rebooting\":true,\"console_drain_grace_ms\":%lu}\n",
               D1L_CONSOLE_SCHEMA, esp_err_to_name(connectivity_ret),
               esp_err_to_name(connectivity_ret),
               (unsigned long)D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS);
        restart_with_factory_reset_quiesces();
        return;
    }

    d1l_factory_reset_status_t status = {0};
    ret = d1l_factory_reset_request(&status);
    if (ret != ESP_OK) {
        if (!status.request_write_may_have_applied) {
            printf("{\"schema\":%d,\"ok\":false,"
                   "\"cmd\":\"factory-reset-confirm\",\"phase\":\"%s\","
                   "\"code\":\"%s\",\"attempt\":%lu,"
                   "\"domains_completed\":%lu,\"domains_total\":%lu,"
                   "\"failed_domain\":\"%s\",\"partial_reset\":false,"
                   "\"reset_scheduled\":false,\"retry_required\":true,"
                   "\"request_commit_attempted\":%s,"
                   "\"request_write_may_have_applied\":false,"
                   "\"request_outcome_ambiguous\":false,"
                   "\"failure_telemetry_persisted\":%s,"
                   "\"connectivity_prepare\":\"ESP_OK\","
                   "\"connectivity_state_restored\":false,"
                   "\"storage_manager_quiesced\":true,"
                   "\"retained_worker_quiesced\":true,"
                   "\"rp2040_bridge_quiesced\":true,"
                   "\"quiesces_released\":false,"
                   "\"recovery\":\"controlled_restart\","
                   "\"global_atomic\":false,"
                   "\"physical_flash_scrubbed\":false,"
                   "\"sd_touched\":false,"
                   "\"removable_sd_data_preserved\":true,"
                   "\"rebooting\":true,"
                   "\"console_drain_grace_ms\":%lu}\n",
                   D1L_CONSOLE_SCHEMA,
                   d1l_factory_reset_phase_name(status.phase),
                   esp_err_to_name(ret), (unsigned long)status.attempt_count,
                   (unsigned long)status.domains_completed,
                   (unsigned long)status.domains_total,
                   status.last_failed_domain,
                   bool_json(status.request_commit_attempted),
                   bool_json(status.failure_telemetry_persisted),
                   (unsigned long)D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS);
        } else {
            const bool active_readback = status.reset_pending &&
                status.phase == D1L_FACTORY_RESET_PHASE_ACTIVE;
            printf("{\"schema\":%d,\"ok\":false,"
                   "\"cmd\":\"factory-reset-confirm\",\"phase\":\"%s\","
                   "\"code\":\"%s\",\"attempt\":%lu,"
                   "\"domains_completed\":%lu,\"domains_total\":%lu,"
                   "\"failed_domain\":\"%s\",\"partial_reset\":false,"
                   "\"reset_schedule_state\":\"%s\","
                   "\"reset_scheduled\":%s,\"retry_required\":%s,"
                   "\"request_commit_attempted\":%s,"
                   "\"request_write_may_have_applied\":true,"
                   "\"request_readback_exact\":%s,"
                   "\"request_outcome_ambiguous\":%s,"
                   "\"connectivity_prepare\":\"ESP_OK\","
                   "\"connectivity_state_restored\":false,"
                   "\"storage_manager_quiesced\":true,"
                   "\"retained_worker_quiesced\":true,"
                   "\"rp2040_bridge_quiesced\":true,"
                   "\"quiesces_released\":false,"
                   "\"recovery\":\"controlled_restart\","
                   "\"global_atomic\":false,"
                   "\"physical_flash_scrubbed\":false,"
                   "\"sd_touched\":false,"
                   "\"removable_sd_data_preserved\":true,"
                   "\"rebooting\":true,"
                   "\"console_drain_grace_ms\":%lu}\n",
                   D1L_CONSOLE_SCHEMA,
                   d1l_factory_reset_phase_name(status.phase),
                   esp_err_to_name(ret), (unsigned long)status.attempt_count,
                   (unsigned long)status.domains_completed,
                   (unsigned long)status.domains_total,
                   status.last_failed_domain,
                   active_readback ? "active_readback" : "ambiguous",
                   active_readback ? "true" : "null",
                   bool_json(!status.request_readback_exact),
                   bool_json(status.request_commit_attempted),
                   bool_json(status.request_readback_exact),
                   bool_json(status.request_outcome_ambiguous),
                   (unsigned long)D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS);
        }
        /* A set/commit error may have armed early-boot deletion, and even a
         * definite no-write failure follows connectivity shutdown. Never
         * release producers into either altered runtime lifetime. */
        restart_with_factory_reset_quiesces();
        return;
    }

    ok_begin("factory-reset-confirm");
    printf(",\"factory_reset_requested\":true,"
           "\"factory_reset_complete\":false,"
           "\"reset_scope\":\"inventoried_onboard_nvs_user_keys\","
           "\"phase\":\"active\",\"reset_scheduled\":true,"
           "\"erase_boundary\":\"early_boot_before_producers\","
           "\"attempt\":%lu,"
           "\"domains_completed\":%lu,\"domains_total\":%lu,"
           "\"keys_erased\":%lu,\"keys_already_absent\":%lu,"
           "\"storage_manager_quiesced\":true,"
           "\"retained_worker_quiesced\":true,"
           "\"rp2040_bridge_quiesced\":true,"
           "\"connectivity_prepare\":\"ESP_OK\","
           "\"global_atomic\":false,\"physical_flash_scrubbed\":false,"
           "\"sd_touched\":false,\"removable_sd_data_preserved\":true,"
           "\"pre_reset_sd_history_fenced_on_boot\":true,"
           "\"sd_media_marker_required\":true,"
           "\"sd_lineage_generation\":%lu,"
           "\"unknown_nvs_keys_preserved\":true,"
           "\"protocol_high_water_preserved\":true,"
           "\"migration_evidence_preserved\":true,"
           "\"time_checkpoint_preserved\":true,"
           "\"crash_forensics_preserved\":true,"
           "\"retained_ownership_evidence_preserved\":true,"
           "\"inventory_entry_count\":%u,\"clear_nvs_key_count\":%u,"
           "\"preserved_raw_marker_slots\":%u,"
           "\"rebooting\":true,\"console_drain_grace_ms\":%lu}\n",
           (unsigned long)status.attempt_count,
           (unsigned long)status.domains_completed,
           (unsigned long)status.domains_total,
           (unsigned long)status.keys_erased,
           (unsigned long)status.keys_already_absent,
           (unsigned long)status.sd_lineage_generation,
           (unsigned int)D1L_FACTORY_RESET_INVENTORY_COUNT,
           (unsigned int)D1L_FACTORY_RESET_CLEAR_DOMAIN_COUNT,
           (unsigned int)D1L_FACTORY_RESET_RAW_MARKER_COUNT,
           (unsigned long)D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS);
    /* All producer/bridge quiesces remain held through system reset. The
     * durable intent is executed by early boot before any producer starts. */
    restart_with_factory_reset_quiesces();
}

static void handle_line(const d1l_usb_command_view_t *command)
{
    if (!command || !command->text || command->length == 0U) {
        err_result("console", "INVALID_COMMAND_BYTES",
                   "command admission did not produce a canonical view");
        return;
    }
    if (!enforce_release_command_policy(command)) {
        return;
    }
    const char *line = command->text;
    d1l_event_log_append(D1L_EVENT_LOG_LEVEL_INFO, "usb", "command",
                         "allowlisted command accepted");
    if (strcmp(line, "help") == 0) {
        cmd_help();
    } else if (strcmp(line, "version") == 0) {
        cmd_version();
    } else if (strcmp(line, "time migration status") == 0) {
        cmd_time_migration_status();
    } else if (strncmp(line, "time migrate-legacy ",
                       strlen("time migrate-legacy ")) == 0) {
        cmd_time_migrate_legacy(line);
    } else if (strcmp(line, "board") == 0) {
        cmd_board();
    } else if (strcmp(line, "settings get") == 0) {
        cmd_settings_get();
    } else if (strcmp(line, "settings reset") == 0) {
        cmd_settings_reset();
    } else if (strncmp(line, "settings set name ", 18) == 0) {
        cmd_settings_set_name(line);
    } else if (strncmp(line, "settings set pathhash ", 22) == 0) {
        cmd_settings_set_pathhash(line);
    } else if (strncmp(line, "settings set timezone ",
                       strlen("settings set timezone ")) == 0) {
        cmd_settings_set_timezone(line);
    } else if (strncmp(line, "settings set location ", strlen("settings set location ")) == 0) {
        cmd_settings_set_location(line);
    } else if (strcmp(line, "settings clear location") == 0) {
        cmd_settings_clear_location();
    } else if (strcmp(line, "ui status") == 0) {
        cmd_ui_status();
    } else if (strncmp(line, "ui tab ", strlen("ui tab ")) == 0) {
        cmd_ui_tab(line);
#if D1L_ENABLE_QUALIFICATION_HOOKS
    } else if (strncmp(line, "ui scroll-probe ", strlen("ui scroll-probe ")) == 0) {
        cmd_ui_scroll_probe(line);
    } else if (strncmp(line, "ui compose-probe ", strlen("ui compose-probe ")) == 0) {
        cmd_ui_compose_probe(line);
    } else if (strncmp(line, "ui data-canary ", strlen("ui data-canary ")) == 0) {
        cmd_ui_data_canary(line);
    } else if (strcmp(line, "ui capture status") == 0) {
        cmd_ui_capture_status();
    } else if (strcmp(line, "ui capture begin") == 0) {
        cmd_ui_capture_begin();
    } else if (strncmp(line, "ui capture chunk ", strlen("ui capture chunk ")) == 0) {
        cmd_ui_capture_chunk(line);
    } else if (strcmp(line, "ui capture end") == 0) {
        cmd_ui_capture_end();
#endif
    } else if (strcmp(line, "map center") == 0) {
        cmd_map_center();
    } else if (strncmp(line, "map center set ", strlen("map center set ")) == 0) {
        cmd_map_center_set(line);
    } else if (strcmp(line, "map center clear") == 0) {
        cmd_map_center_clear();
    } else if (strcmp(line, "map tiles status") == 0) {
        cmd_map_tiles_status();
    } else if (strcmp(line, "map provider status") == 0) {
        cmd_map_provider_status();
#if D1L_ENABLE_QUALIFICATION_HOOKS
    } else if (strcmp(line, "map acceptance open") == 0) {
        cmd_map_acceptance_open();
#endif
    } else if (strcmp(line, "settings onboarding status") == 0) {
        cmd_settings_onboarding_status();
    } else if (strncmp(line, "settings onboarding complete ", 29) == 0) {
        cmd_settings_onboarding_complete(line);
    } else if (strcmp(line, "settings onboarding reset") == 0) {
        cmd_settings_onboarding_reset();
    } else if (strcmp(line, "identity status") == 0) {
        cmd_identity_status();
    } else if (strcmp(line, "i2c") == 0) {
        cmd_i2c();
#if D1L_ENABLE_QUALIFICATION_HOOKS
    } else if (strcmp(line, "display test") == 0) {
        cmd_display_test();
    } else if (strcmp(line, "touch test") == 0) {
        cmd_touch_test();
#endif
    } else if (strcmp(line, "touch raw") == 0) {
        cmd_touch_raw();
    } else if (strcmp(line, "button") == 0) {
        cmd_button();
    } else if (strncmp(line, "backlight ", 10) == 0) {
        cmd_backlight(line);
    } else if (strcmp(line, "radiohw") == 0) {
        cmd_radiohw();
    } else if (strcmp(line, "radio get") == 0) {
        cmd_radio_get();
    } else if (strcmp(line, "radio set preset uscan") == 0) {
        cmd_radio_set_preset_uscan();
    } else if (strncmp(line, "radio set freq ", 15) == 0) {
        cmd_radio_set_freq(line);
    } else if (strncmp(line, "radio set bw ", 13) == 0) {
        cmd_radio_set_bw(line);
    } else if (strncmp(line, "radio set sf ", 13) == 0) {
        cmd_radio_set_sf(line);
    } else if (strncmp(line, "radio set cr ", 13) == 0) {
        cmd_radio_set_cr(line);
    } else if (strncmp(line, "radio set txpower ", 18) == 0) {
        cmd_radio_set_txpower(line);
    } else if (strncmp(line, "radio set rxboost ", 18) == 0) {
        cmd_radio_set_rxboost(line);
    } else if (strcmp(line, "mesh status") == 0) {
        cmd_mesh_status();
    } else if (strcmp(line, "companion status") == 0) {
        cmd_companion_status();
    } else if (strcmp(line, "rp2040 status") == 0) {
        cmd_rp2040_status();
    } else if (strncmp(line, "rp2040 set-baud ", strlen("rp2040 set-baud ")) == 0) {
        cmd_rp2040_set_baud(line);
    } else if (strncmp(line, "rp2040 baud-probe", strlen("rp2040 baud-probe")) == 0 &&
               (line[strlen("rp2040 baud-probe")] == '\0' ||
                line[strlen("rp2040 baud-probe")] == ' ')) {
        cmd_rp2040_baud_probe(line);
    } else if (strcmp(line, "rp2040 ping") == 0) {
        cmd_rp2040_ping();
    } else if (strcmp(line, "rp2040 bootloader") == 0) {
        cmd_rp2040_bootloader();
    } else if (strcmp(line, "rp2040 stock-probe") == 0) {
        cmd_rp2040_stock_probe();
    } else if (strncmp(line, "rp2040 double-reset", strlen("rp2040 double-reset")) == 0 &&
               (line[strlen("rp2040 double-reset")] == '\0' ||
                line[strlen("rp2040 double-reset")] == ' ')) {
        cmd_rp2040_double_reset(line);
    } else if (strcmp(line, "rp2040 reset") == 0) {
        cmd_rp2040_reset();
    } else if (strcmp(line, "storage status") == 0) {
        cmd_storage_status();
    } else if (strcmp(line, "storage mount") == 0) {
        cmd_storage_mount();
    } else if (strcmp(line, "storage remount") == 0) {
        cmd_storage_remount();
    } else if (strcmp(line, "storage reset-bridge") == 0) {
        cmd_storage_reset_bridge();
    } else if (strncmp(line, "storage force-nvs", strlen("storage force-nvs")) == 0) {
        cmd_storage_force_nvs(line);
    } else if (strcmp(line, "storage diag") == 0) {
        cmd_storage_diag();
    } else if (strcmp(line, "storage diag raw") == 0) {
        cmd_storage_diag_raw();
    } else if (strcmp(line, "storage map-policy") == 0) {
        cmd_storage_map_policy();
    } else if (strcmp(line, "storage setup") == 0) {
        cmd_storage_setup(line);
#if D1L_ENABLE_QUALIFICATION_HOOKS
    } else if (strcmp(line, "storage filecanary") == 0) {
        cmd_storage_filecanary();
    } else if (strncmp(line, "storage map-tile-canary ", strlen("storage map-tile-canary ")) == 0) {
        cmd_storage_map_tile_canary(line);
    } else if (strncmp(line, "storage map-tile-check ", strlen("storage map-tile-check ")) == 0) {
        cmd_storage_map_tile_check(line);
    } else if (strncmp(line, "storage export-canary ", strlen("storage export-canary ")) == 0) {
        cmd_storage_export_canary(line);
#endif
    } else if (strncmp(line, "storage export-diagnostics ", strlen("storage export-diagnostics ")) == 0) {
        cmd_storage_export_diagnostics(line);
    } else if (strncmp(line, "storage export-data ", strlen("storage export-data ")) == 0) {
        cmd_storage_export_data(line);
#if D1L_ENABLE_QUALIFICATION_HOOKS
    } else if (strncmp(line, "storage retained-canary ", strlen("storage retained-canary ")) == 0) {
        cmd_storage_retained_canary(line);
    } else if (strncmp(line, "core retained-witness ", strlen("core retained-witness ")) == 0) {
        cmd_core_retained_witness(line);
#endif
    } else if (strcmp(line, "packets") == 0) {
        cmd_packets();
    } else if (strncmp(line, "packets filter ", 15) == 0) {
        cmd_packets_filter(line);
    } else if (strncmp(line, "packets search ", 15) == 0) {
        cmd_packets_search(line);
    } else if (strncmp(line, "packets detail ", 15) == 0) {
        cmd_packets_detail(line);
    } else if (strncmp(line, "packets raw ", 12) == 0) {
        cmd_packets_raw(line);
    } else if (strcmp(line, "packets clear") == 0) {
        cmd_packets_clear();
    } else if (strcmp(line, "messages public") == 0 ||
               strncmp(line, "messages public ", 16) == 0) {
        cmd_messages_public(line);
    } else if (strcmp(line, "messages dm") == 0 ||
               strncmp(line, "messages dm ", 12) == 0) {
        if (strcmp(line, "messages dm clear") == 0) {
            cmd_messages_dm_clear();
        } else {
            cmd_messages_dm(line);
        }
    } else if (strcmp(line, "messages unread") == 0) {
        cmd_messages_unread();
    } else if (strncmp(line, "messages read ", 14) == 0) {
        cmd_messages_read(line);
    } else if (strcmp(line, "messages clear") == 0) {
        cmd_messages_clear();
    } else if (strcmp(line, "nodes") == 0) {
        cmd_nodes();
    } else if (strcmp(line, "nodes clear") == 0) {
        cmd_nodes_clear();
    } else if (strcmp(line, "channels") == 0) {
        cmd_channels();
    } else if (strncmp(line, "channels select ", 16) == 0) {
        cmd_channels_select(line);
    } else if (strcmp(line, "contacts") == 0) {
        cmd_contacts();
    } else if (strcmp(line, "contacts export") == 0 ||
               strncmp(line, "contacts export ", 16) == 0) {
        cmd_contacts_export(line);
    } else if (strncmp(line, "contacts import ", 16) == 0) {
        cmd_contacts_import(line);
    } else if (strcmp(line, "contacts clear") == 0) {
        cmd_contacts_clear();
    } else if (strncmp(line, "contacts add ", 13) == 0) {
        cmd_contacts_add(line);
    } else if (strncmp(line, "contacts rename ", 16) == 0) {
        cmd_contacts_rename(line);
    } else if (strncmp(line, "contacts delete ", 16) == 0) {
        cmd_contacts_delete(line);
    } else if (strncmp(line, "contacts set ", 13) == 0) {
        cmd_contacts_set(line);
    } else if (strcmp(line, "routes") == 0) {
        cmd_routes();
    } else if (strncmp(line, "routes detail ", 14) == 0) {
        cmd_routes_detail(line);
    } else if (strncmp(line, "routes trace contact ", 21) == 0) {
        cmd_routes_trace_contact(line);
    } else if (strcmp(line, "routes trace status") == 0) {
        cmd_routes_trace_status();
    } else if (strncmp(line, "routes trace ", 13) == 0) {
        cmd_routes_trace(line);
    } else if (strncmp(line, "routes probe ", 13) == 0) {
        cmd_routes_probe(line);
    } else if (strncmp(line, "routes telemetry ", 17) == 0) {
        cmd_routes_telemetry(line);
    } else if (strncmp(line, "routes reset ", 13) == 0) {
        cmd_routes_reset(line);
    } else if (strcmp(line, "routes clear") == 0) {
        cmd_routes_clear();
    } else if (strcmp(line, "signal") == 0) {
        cmd_signal();
    } else if (strcmp(line, "roomservers") == 0) {
        cmd_roomservers();
    } else if (strcmp(line, "repeaters") == 0) {
        cmd_repeaters();
    } else if (strcmp(line, "repeater ping status") == 0) {
        cmd_repeater_ping_status();
    } else if (strncmp(line, "repeater ping ",
                       strlen("repeater ping ")) == 0) {
        cmd_repeater_ping(line);
    } else if (strcmp(line, "admin status") == 0) {
        cmd_admin_status("admin status");
    } else if (strncmp(line, "admin login ",
                       strlen("admin login ")) == 0) {
        cmd_admin_login(line);
    } else if (strcmp(line, "admin refresh") == 0) {
        cmd_admin_refresh();
    } else if (strcmp(line, "admin telemetry") == 0 ||
               strcmp(line, "admin access-list") == 0 ||
               strcmp(line, "admin neighbours") == 0 ||
               strncmp(line, "admin neighbours ",
                       strlen("admin neighbours ")) == 0) {
        cmd_admin_query(line);
    } else if (strncmp(line, "admin clear-stats",
                       strlen("admin clear-stats")) == 0 ||
               strncmp(line, "admin advertise-zero-hop",
                       strlen("admin advertise-zero-hop")) == 0) {
        cmd_admin_mutation(line);
    } else if (strcmp(line, "admin cli") == 0 ||
               strncmp(line, "admin cli ",
                       strlen("admin cli ")) == 0) {
        cmd_admin_cli(line);
    } else if (strcmp(line, "admin room-post") == 0 ||
               strncmp(line, "admin room-post ",
                       strlen("admin room-post ")) == 0) {
        cmd_admin_room_post(line);
    } else if (strcmp(line, "admin logout") == 0) {
        cmd_admin_logout();
    } else if (strcmp(line, "health") == 0) {
        cmd_health();
    } else if (strcmp(line, "crashlog") == 0) {
        cmd_crashlog();
    } else if (strcmp(line, "crashlog clear") == 0) {
        cmd_crashlog_clear();
    } else if (strcmp(line, "logs") == 0) {
        cmd_logs();
    } else if (strncmp(line, "logs clear", strlen("logs clear")) == 0) {
        cmd_logs_clear(line);
    } else if (strcmp(line, "terminal status") == 0) {
        cmd_terminal_status();
    } else if (strncmp(line, "terminal level ",
                       strlen("terminal level ")) == 0) {
        cmd_terminal_level(line);
    } else if (strcmp(line, "wifi status") == 0) {
        cmd_wifi_status();
    } else if (strcmp(line, "wifi profiles") == 0) {
        cmd_wifi_profiles();
    } else if (strcmp(line, "wifi off") == 0) {
        cmd_wifi_off();
    } else if (strcmp(line, "wifi on") == 0) {
        cmd_wifi_on();
    } else if (strcmp(line, "wifi scan") == 0) {
        cmd_wifi_scan();
    } else if (strcmp(line, "wifi connect") == 0) {
        cmd_wifi_connect();
    } else if (strncmp(line, "wifi save ", strlen("wifi save ")) == 0) {
        cmd_wifi_save(line);
    } else if (strncmp(line, "wifi select ",
                       strlen("wifi select ")) == 0) {
        cmd_wifi_select(line);
    } else if (strncmp(line, "wifi delete ",
                       strlen("wifi delete ")) == 0) {
        cmd_wifi_delete(line);
    } else if (strcmp(line, "wifi clear") == 0) {
        cmd_wifi_clear();
    } else if (strcmp(line, "ble status") == 0) {
        cmd_ble_status();
    } else if (strcmp(line, "ble off") == 0) {
        cmd_ble_off();
    } else if (strcmp(line, "ble on") == 0) {
        cmd_ble_on();
    } else if (strcmp(line, "observer status") == 0) {
        print_observer_status("observer status");
    } else if (strncmp(line, "observer configure-auth ",
                       strlen("observer configure-auth ")) == 0) {
        cmd_observer_configure(line, true, false);
    } else if (strncmp(line, "observer configure-location ",
                       strlen("observer configure-location ")) == 0) {
        cmd_observer_configure(line, false, true);
    } else if (strncmp(line, "observer configure ",
                       strlen("observer configure ")) == 0) {
        cmd_observer_configure(line, false, false);
    } else if (strcmp(line, "observer on") == 0) {
        cmd_observer_set_enabled(true);
    } else if (strcmp(line, "observer off") == 0) {
        cmd_observer_set_enabled(false);
    } else if (strcmp(line, "observer clear") == 0) {
        cmd_observer_clear();
    } else if (strcmp(line, "update status") == 0) {
        cmd_update_status();
    } else if (strncmp(line, "update install",
                       strlen("update install")) == 0) {
        cmd_update_install(line);
    } else if (strcmp(line, "update cancel") == 0) {
        cmd_update_cancel();
    } else if (strncmp(line, "update reboot",
                       strlen("update reboot")) == 0) {
        cmd_update_reboot(line);
    } else if (strcmp(line, "mesh advert zero") == 0) {
        cmd_mesh_advert("mesh advert zero", false);
    } else if (strcmp(line, "mesh advert flood") == 0) {
        cmd_mesh_advert("mesh advert flood", true);
    } else if (strncmp(line, "mesh send public ", 17) == 0) {
        cmd_mesh_send_public(line);
    } else if (strncmp(line, "mesh send dm ", 13) == 0) {
        cmd_mesh_send_dm(line);
    } else if (strcmp(line, "factory-reset-status") == 0) {
        cmd_factory_reset_status();
    } else if (strcmp(line, "reboot") == 0) {
        const int64_t reboot_started_us = esp_timer_get_time();
        uint32_t remaining_ms = reboot_deadline_remaining_ms(reboot_started_us);
        esp_err_t storage_quiesce_ret =
            d1l_storage_manager_quiesce_begin(remaining_ms);
        if (storage_quiesce_ret != ESP_OK) {
            err_result("reboot", esp_err_to_name(storage_quiesce_ret),
                       "storage manager quiesce failed; reboot cancelled");
            return;
        }

        remaining_ms = reboot_deadline_remaining_ms(reboot_started_us);
        esp_err_t retained_flush_ret = remaining_ms > 0U ?
            d1l_route_store_worker_force_flush(remaining_ms) : ESP_ERR_TIMEOUT;
        if (retained_flush_ret != ESP_OK) {
            d1l_storage_manager_quiesce_end();
            err_result("reboot", esp_err_to_name(retained_flush_ret),
                       "retained storage flush failed; reboot cancelled");
            return;
        }

        remaining_ms = reboot_deadline_remaining_ms(reboot_started_us);
        esp_err_t retained_quiesce_ret = remaining_ms > 0U ?
            d1l_route_store_worker_quiesce_begin(remaining_ms) : ESP_ERR_TIMEOUT;
        if (retained_quiesce_ret != ESP_OK) {
            d1l_storage_manager_quiesce_end();
            err_result("reboot", esp_err_to_name(retained_quiesce_ret),
                       "retained worker quiesce failed; reboot cancelled");
            return;
        }

        remaining_ms = reboot_deadline_remaining_ms(reboot_started_us);
        esp_err_t bridge_quiesce_ret = remaining_ms > 0U ?
            d1l_rp2040_bridge_quiesce_begin(remaining_ms) : ESP_ERR_TIMEOUT;
        if (bridge_quiesce_ret != ESP_OK) {
            d1l_route_store_worker_quiesce_end();
            d1l_storage_manager_quiesce_end();
            err_result("reboot", esp_err_to_name(bridge_quiesce_ret),
                       "RP2040 bridge quiesce failed; reboot cancelled");
            return;
        }

        remaining_ms = reboot_deadline_remaining_ms(reboot_started_us);
        if (remaining_ms <= D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS +
                            D1L_REBOOT_RESTART_MARGIN_MS) {
            d1l_rp2040_bridge_quiesce_end();
            d1l_route_store_worker_quiesce_end();
            d1l_storage_manager_quiesce_end();
            err_result("reboot", esp_err_to_name(ESP_ERR_TIMEOUT),
                       "reboot deadline lacks console drain headroom; reboot cancelled");
            return;
        }
        esp_err_t connectivity_prepare_ret = d1l_connectivity_prepare_reboot();
        if (connectivity_prepare_ret != ESP_OK) {
            d1l_rp2040_bridge_quiesce_end();
            d1l_route_store_worker_quiesce_end();
            d1l_storage_manager_quiesce_end();
            err_result("reboot", esp_err_to_name(connectivity_prepare_ret),
                       "connectivity prepare failed; reboot cancelled");
            return;
        }
        ok_begin("reboot");
        printf(",\"rebooting\":true,\"reset_scope\":\"system\",\"storage_manager_quiesced\":true,\"retained_worker_quiesced\":true,\"rp2040_bridge_quiesced\":true,\"connectivity_prepare\":\"ESP_OK\",\"retained_flush\":\"ESP_OK\",\"route_flush\":\"ESP_OK\",\"console_drain_grace_ms\":%lu}\n",
               (unsigned long)D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS));
        /* Keep all three quiesce locks held so neither retained persistence nor
         * a new UART2 SD exchange can begin while the bounded console grace
         * drains UART0. A full ROM system reset avoids carrying esp_restart's
         * one-second RTC watchdog into the next boot. */
        esp_rom_software_reset_system();
        for (;;) {
        }
    } else if (strcmp(line, "factory-reset-confirm") == 0) {
        cmd_factory_reset_confirm();
    } else {
        err_result("unknown", "UNKNOWN_COMMAND", "send help for supported commands");
    }
}

void d1l_usb_console_run(void)
{
    static char line[256] EXT_RAM_BSS_ATTR;
    size_t used = 0;
    bool dropping_overlong = false;
    cmd_help();
    bool prompt_pending = true;
    while (true) {
        if (prompt_pending) {
            printf(D1L_USB_CONSOLE_PROMPT);
            fflush(stdout);
            prompt_pending = false;
        }

        int ch = fgetc(stdin);
        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (dropping_overlong) {
                dropping_overlong = false;
                wipe_console_bytes(line, sizeof(line));
                used = 0;
                prompt_pending = true;
                continue;
            }
            if (used == 0U) {
                continue;
            }
            const size_t received_length = used;
            used = 0;
            d1l_usb_command_view_t command = {0};
            const d1l_usb_command_admit_status_t admission =
                d1l_usb_command_admit_in_place(
                    line, received_length, sizeof(line), &command);
            if (admission == D1L_USB_COMMAND_ADMIT_EMPTY) {
                wipe_console_bytes(line, sizeof(line));
                prompt_pending = true;
                continue;
            }
            printf("\n");
            if (admission != D1L_USB_COMMAND_ADMIT_OK) {
                err_result("console",
                           d1l_usb_command_admit_status_code(admission),
                           "command contains invalid or hidden bytes");
                wipe_console_bytes(line, sizeof(line));
                prompt_pending = true;
                continue;
            }
            handle_line(&command);
            wipe_console_bytes(line, sizeof(line));
            prompt_pending = true;
            continue;
        }

        if (dropping_overlong) {
            continue;
        }

        if (used + 1U >= sizeof(line)) {
            wipe_console_bytes(line, sizeof(line));
            used = 0;
            dropping_overlong = true;
            printf("\n");
            err_result("console", "LINE_TOO_LONG", "USB console command exceeded the line buffer");
            prompt_pending = true;
            continue;
        }

        line[used++] = (char)ch;
    }
}

static void factory_reset_recovery_status(const char *cmd)
{
    d1l_factory_reset_status_t status = {0};
    const esp_err_t inspect_ret = d1l_factory_reset_inspect(&status);
    const esp_err_t effective_ret = inspect_ret == ESP_OK ?
        status.last_error : inspect_ret;
    printf("{\"schema\":%d,\"ok\":%s,\"cmd\":\"%s\","
           "\"phase\":\"%s\",\"journal_found\":%s,"
           "\"pending\":%s,\"complete\":%s,\"attempt\":%lu,"
           "\"domains_completed\":%lu,\"domains_total\":%lu,"
           "\"keys_erased\":%lu,\"keys_already_absent\":%lu,"
           "\"failed_domain\":\"%s\",\"failed_domain_index\":",
           D1L_CONSOLE_SCHEMA, bool_json(effective_ret == ESP_OK), cmd,
           d1l_factory_reset_phase_name(status.phase),
           bool_json(status.journal_found), bool_json(status.reset_pending),
           bool_json(status.reset_complete),
           (unsigned long)status.attempt_count,
           (unsigned long)status.domains_completed,
           (unsigned long)status.domains_total,
           (unsigned long)status.keys_erased,
           (unsigned long)status.keys_already_absent,
           status.last_failed_domain);
    if (status.last_failed_domain_index == UINT32_MAX) {
        printf("null");
    } else {
        printf("%lu", (unsigned long)status.last_failed_domain_index);
    }
    printf(",\"error\":\"%s\",\"stored_error\":\"%s\","
           "\"inspection_error\":\"%s\","
           "\"sd_lineage_generation\":%lu,"
           "\"sd_lineage_active_mask\":%lu,"
           "\"recovery_mode\":true,\"producers_started\":false,"
           "\"rf_started\":false,\"connectivity_started\":false,"
           "\"stores_started\":false,\"sd_touched\":false}\n",
           esp_err_to_name(effective_ret), esp_err_to_name(status.last_error),
           esp_err_to_name(inspect_ret),
           (unsigned long)status.sd_lineage_generation,
           (unsigned long)status.sd_lineage_active_mask);
}

static void factory_reset_recovery_export(void)
{
    factory_reset_recovery_status("factory-reset-recovery-export");
    for (size_t i = 0U; i < d1l_factory_reset_inventory_count(); ++i) {
        d1l_factory_reset_inventory_entry_t entry = {0};
        if (!d1l_factory_reset_inventory_entry(i, &entry)) {
            continue;
        }
        printf("{\"schema\":%d,\"ok\":true,"
               "\"cmd\":\"factory-reset-recovery-export\","
               "\"inventory_index\":%lu,\"name\":\"%s\","
               "\"partition\":\"%s\",\"namespace\":\"%s\","
               "\"key\":\"%s\",\"disposition\":\"%s\"}\n",
               D1L_CONSOLE_SCHEMA, (unsigned long)i, entry.name,
               entry.partition_label, entry.nvs_namespace, entry.key,
               d1l_factory_reset_disposition_name(entry.disposition));
    }
}

void d1l_usb_console_run_factory_reset_recovery(
    const d1l_factory_reset_status_t *boot_status)
{
    static char line[128] EXT_RAM_BSS_ATTR;
    char repair_token[9] = {0};
    int64_t repair_deadline_us = 0;
    size_t used = 0U;
    bool dropping_overlong = false;
    bool prompt_pending = true;
    const char *boot_phase = boot_status ?
        d1l_factory_reset_phase_name(boot_status->phase) : "storage_error";

    printf("{\"schema\":%d,\"ok\":true,"
           "\"event\":\"factory_reset_recovery_console\","
           "\"boot_phase\":\"%s\",\"recovery_mode\":true,"
           "\"commands\":[\"help\","
           "\"factory-reset-recovery-status\","
           "\"factory-reset-recovery-export\","
           "\"factory-reset-repair-arm\","
           "\"factory-reset-repair-confirm <token> "
           "ERASE_ONBOARD_USER_STATE\"],"
           "\"producers_started\":false,\"sd_touched\":false}\n",
           D1L_CONSOLE_SCHEMA, boot_phase);

    for (;;) {
        if (prompt_pending) {
            printf("d1l-reset-recovery> ");
            fflush(stdout);
            prompt_pending = false;
        }
        const int ch = fgetc(stdin);
        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ch != '\n' && ch != '\r') {
            if (dropping_overlong) {
                continue;
            }
            if (used + 1U >= sizeof(line)) {
                wipe_console_bytes(line, sizeof(line));
                used = 0U;
                dropping_overlong = true;
                printf("\n");
                err_result("factory-reset-recovery", "LINE_TOO_LONG",
                           "recovery command exceeded the bounded line buffer");
                prompt_pending = true;
                continue;
            }
            line[used++] = (char)ch;
            continue;
        }
        if (dropping_overlong) {
            dropping_overlong = false;
            wipe_console_bytes(line, sizeof(line));
            used = 0U;
            prompt_pending = true;
            continue;
        }
        if (used == 0U) {
            continue;
        }
        const size_t received_length = used;
        used = 0U;
        d1l_usb_command_view_t command = {0};
        const d1l_usb_command_admit_status_t admission =
            d1l_usb_command_admit_in_place(
                line, received_length, sizeof(line), &command);
        printf("\n");

        if (admission == D1L_USB_COMMAND_ADMIT_EMPTY) {
            wipe_console_bytes(line, sizeof(line));
            prompt_pending = true;
            continue;
        }
        if (admission != D1L_USB_COMMAND_ADMIT_OK) {
            err_result("factory-reset-recovery",
                       d1l_usb_command_admit_status_code(admission),
                       "recovery command contains invalid or hidden bytes");
            wipe_console_bytes(line, sizeof(line));
            prompt_pending = true;
            continue;
        }
        const char *canonical_line = command.text;

        if (d1l_usb_command_equals(
                &command, "help", sizeof("help") - 1U)) {
            printf("{\"schema\":%d,\"ok\":true,\"cmd\":\"help\","
                   "\"recovery_only\":true,"
                   "\"repair_requires_two_confirmations\":true}\n",
                   D1L_CONSOLE_SCHEMA);
        } else if (d1l_usb_command_equals(
                       &command, "factory-reset-recovery-status",
                       sizeof("factory-reset-recovery-status") - 1U)) {
            factory_reset_recovery_status(
                "factory-reset-recovery-status");
        } else if (d1l_usb_command_equals(
                       &command, "factory-reset-recovery-export",
                       sizeof("factory-reset-recovery-export") - 1U)) {
            factory_reset_recovery_export();
        } else if (d1l_usb_command_equals(
                       &command, "factory-reset-repair-arm",
                       sizeof("factory-reset-repair-arm") - 1U)) {
            const uint32_t token = esp_random();
            (void)snprintf(repair_token, sizeof(repair_token), "%08" PRIx32,
                           token);
            repair_deadline_us = esp_timer_get_time() + 60000000LL;
            printf("{\"schema\":%d,\"ok\":true,"
                   "\"cmd\":\"factory-reset-repair-arm\","
                   "\"confirmation\":1,\"token\":\"%s\","
                   "\"expires_ms\":60000,"
                   "\"next\":\"factory-reset-repair-confirm %s "
                   "ERASE_ONBOARD_USER_STATE\"}\n",
                   D1L_CONSOLE_SCHEMA, repair_token, repair_token);
        } else if (d1l_usb_command_has_argument(
                       &command, "factory-reset-repair-confirm ",
                       sizeof("factory-reset-repair-confirm ") - 1U)) {
            char supplied_token[9] = {0};
            char phrase[32] = {0};
            char extra = '\0';
            const int fields = sscanf(
                canonical_line + sizeof("factory-reset-repair-confirm ") - 1U,
                "%8s %31s %c", supplied_token, phrase, &extra);
            const bool armed = repair_token[0] != '\0' &&
                repair_deadline_us > esp_timer_get_time();
            const bool exact = fields == 2 && armed &&
                strcmp(supplied_token, repair_token) == 0 &&
                strcmp(phrase, "ERASE_ONBOARD_USER_STATE") == 0;
            wipe_console_bytes(repair_token, sizeof(repair_token));
            repair_deadline_us = 0;
            wipe_console_bytes(supplied_token, sizeof(supplied_token));
            wipe_console_bytes(phrase, sizeof(phrase));
            if (!exact) {
                err_result("factory-reset-repair-confirm",
                           "CONFIRMATION_REQUIRED",
                           "arm again, then submit the exact token and phrase");
            } else {
                d1l_factory_reset_status_t repaired = {0};
                const esp_err_t repair_ret =
                    d1l_factory_reset_repair_quarantined(&repaired);
                if (repair_ret != ESP_OK) {
                    d1l_factory_reset_status_t readback = {0};
                    const esp_err_t inspect_ret =
                        d1l_factory_reset_inspect(&readback);
                    if (inspect_ret == ESP_OK && readback.reset_pending &&
                        readback.phase == D1L_FACTORY_RESET_PHASE_ACTIVE) {
                        printf("{\"schema\":%d,\"ok\":false,"
                               "\"cmd\":\"factory-reset-repair-confirm\","
                               "\"code\":\"%s\","
                               "\"repair_write_may_have_applied\":true,"
                               "\"active_readback\":true,"
                               "\"rebooting\":true}\n",
                               D1L_CONSOLE_SCHEMA,
                               esp_err_to_name(repair_ret));
                        restart_with_factory_reset_quiesces();
                    } else {
                        err_result(
                            "factory-reset-repair-confirm",
                            esp_err_to_name(repair_ret),
                            "journal repair failed; recovery mode remains active");
                    }
                } else {
                    printf("{\"schema\":%d,\"ok\":true,"
                           "\"cmd\":\"factory-reset-repair-confirm\","
                           "\"confirmation\":2,"
                           "\"fresh_active_journal\":true,"
                           "\"next_domain\":0,"
                           "\"sd_lineage_generation\":%lu,"
                           "\"rebooting\":true}\n",
                           D1L_CONSOLE_SCHEMA,
                           (unsigned long)repaired.sd_lineage_generation);
                    restart_with_factory_reset_quiesces();
                }
            }
        } else {
            err_result("factory-reset-recovery", "UNKNOWN_COMMAND",
                       "only bounded recovery status, export, and repair are available");
        }
        wipe_console_bytes(line, sizeof(line));
        prompt_pending = true;
    }
}
