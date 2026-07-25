#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D1L_UI_STORAGE_TEXT_LEN 80U
#define D1L_UI_STORAGE_LOCATION_COUNT 6U
#define D1L_UI_STORAGE_VIEW_MODEL_MAX_BYTES 2048U

typedef enum {
    D1L_UI_STORAGE_LOCATION_MESSAGES = 0,
    D1L_UI_STORAGE_LOCATION_DIRECT_MESSAGES,
    D1L_UI_STORAGE_LOCATION_PACKETS,
    D1L_UI_STORAGE_LOCATION_ROUTES,
    D1L_UI_STORAGE_LOCATION_MAP_TILES,
    D1L_UI_STORAGE_LOCATION_EXPORTS,
} d1l_ui_storage_location_t;

typedef struct {
    bool rp2040_bridge_required;
    bool rp2040_bridge_ready;
    bool rp2040_sd_protocol_supported;
    bool sd_present;
    bool sd_mounted;
    bool sd_data_root_ready;
    bool sd_needs_fat32;
    bool data_enabled;
    bool retained_sd_degraded;
    bool retained_backup_degraded;
    uint32_t capacity_kb;
    uint32_t free_kb;
    const char *sd_state;
    const char *sd_filesystem;
    const char *setup_action;
    const char *message_store_backend;
    const char *dm_store_backend;
    const char *packet_log_backend;
    const char *route_store_backend;
    const char *map_tile_backend;
    const char *export_backend;
} d1l_ui_storage_view_input_t;

typedef struct {
    char state[D1L_UI_STORAGE_TEXT_LEN];
    char detail[D1L_UI_STORAGE_TEXT_LEN];
    char guidance[D1L_UI_STORAGE_TEXT_LEN];
    uint32_t accent;
} d1l_ui_storage_hero_view_t;

typedef struct {
    char state[D1L_UI_STORAGE_TEXT_LEN];
    char filesystem[D1L_UI_STORAGE_TEXT_LEN];
    char capacity[D1L_UI_STORAGE_TEXT_LEN];
    char free_space[D1L_UI_STORAGE_TEXT_LEN];
    char readiness[D1L_UI_STORAGE_TEXT_LEN];
    uint32_t state_accent;
    uint32_t readiness_accent;
} d1l_ui_storage_card_view_t;

typedef struct {
    d1l_ui_storage_location_t location;
    char name[D1L_UI_STORAGE_TEXT_LEN];
    char value[D1L_UI_STORAGE_TEXT_LEN];
    uint32_t accent;
} d1l_ui_storage_location_view_t;

typedef struct {
    d1l_ui_storage_hero_view_t hero;
    char card_summary[D1L_UI_STORAGE_TEXT_LEN];
    char data_summary[D1L_UI_STORAGE_TEXT_LEN];
    d1l_ui_storage_card_view_t card;
    size_t location_count;
    d1l_ui_storage_location_view_t locations[D1L_UI_STORAGE_LOCATION_COUNT];
    bool needs_attention;
} d1l_ui_storage_view_model_t;

bool d1l_ui_storage_view(const d1l_ui_storage_view_input_t *input,
                         d1l_ui_storage_view_model_t *out_view);
bool d1l_ui_storage_location_available(
    d1l_ui_storage_location_t location);
bool d1l_ui_storage_view_model_is_valid(
    const d1l_ui_storage_view_model_t *view_model);
