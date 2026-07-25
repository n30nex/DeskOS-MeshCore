#include "ui_storage_view.h"

#include <stdio.h>
#include <string.h>

#include "app/release_profile.h"

enum {
    COLOR_GREEN = 0xA7F3D0,
    COLOR_AMBER = 0xFBBF24,
    COLOR_RED = 0xFCA5A5,
    COLOR_BLUE = 0x93C5FD,
};

_Static_assert(sizeof(d1l_ui_storage_view_model_t) <=
                   D1L_UI_STORAGE_VIEW_MODEL_MAX_BYTES,
               "Storage view model exceeded its persistent-owner size budget");

static bool text_equals(const char *value, const char *expected)
{
    return value && expected && strcmp(value, expected) == 0;
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (!destination || capacity == 0U) {
        return;
    }
    snprintf(destination, capacity, "%s", source ? source : "");
}

static bool bounded_text_is_terminated(const char *text, size_t capacity)
{
    return text && capacity > 0U && memchr(text, '\0', capacity) != NULL;
}

static bool storage_needs_attention(const d1l_ui_storage_view_input_t *input)
{
    return input && (input->retained_sd_degraded ||
           text_equals(input->sd_state, "error") ||
           text_equals(input->sd_state, "bridge_reported") ||
           text_equals(input->setup_action,
                       "inspect_rp2040_sd_cmd0_firmware_path") ||
           text_equals(input->setup_action,
                       "inspect_rp2040_sd_mount_error_firmware_path"));
}

static const char *card_state(const d1l_ui_storage_view_input_t *input)
{
    if (storage_needs_attention(input)) {
        return "Card needs attention";
    }
    if (text_equals(input->setup_action, "wait_for_storage_reconnect")) {
        return "Card reader reconnecting";
    }
    if (text_equals(input->setup_action, "run_storage_mount") ||
        text_equals(input->setup_action, "wait_for_storage_mount")) {
        return "Checking card";
    }
    if (input->rp2040_bridge_required && !input->rp2040_bridge_ready) {
        return "Card reader unavailable";
    }
    if (input->rp2040_bridge_required &&
        !input->rp2040_sd_protocol_supported) {
        return "Card reader starting";
    }
    if (!input->sd_present) {
        return "No card inserted";
    }
    if (input->sd_needs_fat32 ||
        text_equals(input->sd_state, "not_fat32_or_unmountable")) {
        return "FAT32 card required";
    }
    if (text_equals(input->sd_state, "deskos_manifest_invalid")) {
        return "DeskOS files need attention";
    }
    if (text_equals(input->sd_state, "checking") ||
        text_equals(input->sd_state, "mount_pending")) {
        return "Checking card";
    }
    if (text_equals(input->sd_state, "creating_deskos_files") ||
        (input->sd_mounted && !input->sd_data_root_ready)) {
        return "Preparing DeskOS folders";
    }
    if (input->sd_mounted && input->sd_data_root_ready) {
        return "Ready";
    }
    return "Card detected, not ready";
}

static const char *filesystem(const d1l_ui_storage_view_input_t *input)
{
    if (!input->sd_present) {
        return "Not available";
    }
    if (input->sd_needs_fat32) {
        return "FAT32 required";
    }
    if (text_equals(input->sd_filesystem, "fat32") ||
        text_equals(input->sd_filesystem, "fatfs")) {
        return "FAT32";
    }
    if (text_equals(input->sd_filesystem, "exfat")) {
        return "exFAT (not supported)";
    }
    return input->sd_mounted ? "Detected" : "Not available";
}

static const char *readiness(const d1l_ui_storage_view_input_t *input)
{
    if (storage_needs_attention(input) ||
        text_equals(input->sd_state, "deskos_manifest_invalid")) {
        return "Needs attention";
    }
    if (text_equals(input->setup_action, "wait_for_storage_reconnect")) {
        return "Reconnecting";
    }
    if (text_equals(input->setup_action, "run_storage_mount") ||
        text_equals(input->setup_action, "wait_for_storage_mount")) {
        return "Checking";
    }
    if (text_equals(input->setup_action, "bridge_unavailable") ||
        text_equals(input->setup_action, "bridge_protocol_pending")) {
        return "Not available";
    }
    if (text_equals(input->setup_action, "insert_card") || !input->sd_present) {
        return "No card";
    }
    if (input->sd_needs_fat32) {
        return "Needs FAT32";
    }
    if (text_equals(input->sd_state, "checking") ||
        text_equals(input->sd_state, "mount_pending")) {
        return "Checking";
    }
    if (input->sd_mounted && input->sd_data_root_ready) {
        return "Ready";
    }
    if (input->sd_present || input->sd_mounted) {
        return "Setup incomplete";
    }
    return "Not ready";
}

static uint32_t card_value_accent(const char *value)
{
    if (text_equals(value, "Ready")) {
        return COLOR_GREEN;
    }
    if (text_equals(value, "Needs attention") ||
        text_equals(value, "Card needs attention") ||
        text_equals(value, "DeskOS files need attention")) {
        return COLOR_RED;
    }
    if (text_equals(value, "Not available") ||
        text_equals(value, "No card") ||
        text_equals(value, "No card inserted") ||
        text_equals(value, "Status unavailable") ||
        text_equals(value, "Card reader unavailable")) {
        return COLOR_BLUE;
    }
    return COLOR_AMBER;
}

static void format_kb(char *destination, size_t capacity, uint32_t kb, bool known)
{
    if (!destination || capacity == 0U) {
        return;
    }
    if (!known) {
        copy_text(destination, capacity, "Not reported");
    } else if (kb >= 1024U * 1024U) {
        const uint32_t whole = kb / (1024U * 1024U);
        const uint32_t tenth =
            ((kb % (1024U * 1024U)) * 10U) / (1024U * 1024U);
        snprintf(destination, capacity, "%lu.%lu GB",
                 (unsigned long)whole, (unsigned long)tenth);
    } else if (kb >= 1024U) {
        snprintf(destination, capacity, "%lu MB", (unsigned long)(kb / 1024U));
    } else {
        snprintf(destination, capacity, "%lu KB", (unsigned long)kb);
    }
}

static bool backend_uses_sd(const char *backend)
{
    return text_equals(backend, "sd") || text_equals(backend, "mixed") ||
           text_equals(backend, "sd_map_tiles_ready") ||
           text_equals(backend, "sd_diagnostic_exports_ready");
}

static const char *retained_backend(
    const d1l_ui_storage_view_input_t *input, const char *backend)
{
    if (text_equals(backend, "sd") || text_equals(backend, "mixed")) {
        return input->retained_backup_degraded ?
            "SD; backup degraded" : "SD + internal backup";
    }
    if (text_equals(backend, "nvs")) {
        return input->retained_backup_degraded ? "Internal issue" : "Internal";
    }
    return "Unavailable";
}

static const char *map_backend(const char *backend)
{
    if (text_equals(backend, "sd_map_tiles_ready")) {
        return "SD card";
    }
    if (text_equals(backend, "sd_pending_store_migration")) {
        return "Pending";
    }
    return "Unavailable";
}

static const char *export_backend(const char *backend)
{
    return text_equals(backend, "sd_diagnostic_exports_ready") ?
        "SD card" : "USB only";
}

static uint32_t backend_accent(const char *backend)
{
    if (backend_uses_sd(backend)) {
        return COLOR_GREEN;
    }
    if (text_equals(backend, "sd_pending_store_migration")) {
        return COLOR_AMBER;
    }
    return COLOR_BLUE;
}

static const char *data_summary(const d1l_ui_storage_view_input_t *input)
{
    const bool uses_sd = backend_uses_sd(input->message_store_backend) ||
        backend_uses_sd(input->dm_store_backend) ||
        backend_uses_sd(input->packet_log_backend) ||
        backend_uses_sd(input->route_store_backend) ||
        backend_uses_sd(input->map_tile_backend) ||
        backend_uses_sd(input->export_backend);
    if (input->retained_backup_degraded) {
        return uses_sd ? "SD; backup issue" : "Storage issue";
    }
    return uses_sd ? "SD + internal" : "Internal";
}

static void set_hero(const d1l_ui_storage_view_input_t *input,
                     d1l_ui_storage_hero_view_t *hero)
{
    const char *state = "Storage starting";
    const char *detail = "Checking saved-data storage.";
    const char *guidance = "Status updates automatically.";
    uint32_t accent = COLOR_AMBER;

    if (input->retained_backup_degraded) {
        if (input->retained_sd_degraded) {
            state = "Saved storage needs attention";
            detail = "SD and internal backup reported errors.";
            guidance = "See USB diagnostics before relying on saved history.";
            accent = COLOR_RED;
        } else if (input->data_enabled) {
            state = "SD card ready";
            detail = "Saved data is using SD.";
            guidance = "Internal backup needs attention.";
        } else {
            state = "Storage needs attention";
            detail = "Internal saved-data storage is unavailable.";
            guidance = "See USB diagnostics before relying on saved history.";
            accent = COLOR_RED;
        }
    } else if (input->retained_sd_degraded) {
        state = "SD needs attention";
        detail = "Internal storage is active.";
        guidance = "Saved data remains available.";
        accent = COLOR_RED;
    } else if (storage_needs_attention(input)) {
        state = "Card needs attention";
        detail = "Internal storage is active.";
        guidance = "Technical details are available over USB.";
        accent = COLOR_RED;
    } else if (text_equals(input->setup_action, "wait_for_storage_reconnect")) {
        state = "Card reader reconnecting";
        detail = input->data_enabled ?
            "Last confirmed SD remains active briefly." :
            "Internal storage is active.";
        guidance = input->data_enabled ?
            "Internal fallback takes over if status retries fail." :
            "SD access resumes after a valid status reply.";
    } else if (text_equals(input->setup_action, "bridge_unavailable")) {
        detail = "SD support is unavailable.";
        guidance = "Internal storage remains active.";
    } else if (text_equals(input->setup_action, "bridge_protocol_pending")) {
        detail = "SD support is starting.";
    } else if (text_equals(input->setup_action, "run_storage_mount") ||
               text_equals(input->setup_action, "wait_for_storage_mount")) {
        state = "Checking SD card";
        detail = "Using internal storage for now.";
        guidance = "The card check finishes automatically.";
    } else if (text_equals(input->setup_action, "insert_card")) {
        state = "No SD card";
        detail = "Internal storage is active.";
        guidance = "Insert a FAT32 card for more space.";
    } else if (text_equals(input->setup_action, "prepare_fat32_on_computer") ||
               text_equals(input->setup_action,
                           "backup_reformat_fat32_on_computer")) {
        state = "Card needs FAT32";
        detail = "Prepare it on a computer.";
        guidance = "Prepare as FAT32, then reinsert the card.";
    } else if (text_equals(input->setup_action, "retry_storage_mount") ||
               text_equals(input->setup_action, "use_nvs_fallback")) {
        state = "Card setup incomplete";
        detail = "Internal storage is active.";
        guidance = "Reinsert the card to finish DeskOS folders.";
    } else if (text_equals(input->setup_action, "forced_nvs")) {
        state = "Internal storage only";
        detail = "SD storage is paused.";
        guidance = "Internal storage remains active.";
    } else if (input->data_enabled) {
        state = "SD card ready";
        detail = "SD is used with internal backup.";
        guidance = "Saved data stays mirrored internally.";
        accent = COLOR_GREEN;
    } else if (input->sd_data_root_ready) {
        state = "SD card ready";
        detail = "Using internal storage.";
        guidance = "SD is ready; saved data stays internal.";
        accent = COLOR_GREEN;
    }

    copy_text(hero->state, sizeof(hero->state), state);
    copy_text(hero->detail, sizeof(hero->detail), detail);
    copy_text(hero->guidance, sizeof(hero->guidance), guidance);
    hero->accent = accent;
}

static void set_location(d1l_ui_storage_location_view_t *location,
                         d1l_ui_storage_location_t kind,
                         const char *name,
                         const char *value,
                         uint32_t accent)
{
    location->location = kind;
    copy_text(location->name, sizeof(location->name), name);
    copy_text(location->value, sizeof(location->value), value);
    location->accent = accent;
}

bool d1l_ui_storage_location_available(
    d1l_ui_storage_location_t location)
{
    if (location < D1L_UI_STORAGE_LOCATION_MESSAGES ||
        location > D1L_UI_STORAGE_LOCATION_EXPORTS) {
        return false;
    }
    return location != D1L_UI_STORAGE_LOCATION_MAP_TILES ||
        d1l_release_feature_available(D1L_RELEASE_FEATURE_MAP);
}

bool d1l_ui_storage_view_model_is_valid(
    const d1l_ui_storage_view_model_t *view_model)
{
    if (!view_model ||
        view_model->location_count != D1L_UI_STORAGE_LOCATION_COUNT) {
        return false;
    }
    const char *required[] = {
        view_model->hero.state,
        view_model->hero.detail,
        view_model->hero.guidance,
        view_model->card_summary,
        view_model->data_summary,
        view_model->card.state,
        view_model->card.filesystem,
        view_model->card.capacity,
        view_model->card.free_space,
        view_model->card.readiness,
    };
    for (size_t index = 0U; index < sizeof(required) / sizeof(required[0]); ++index) {
        if (!bounded_text_is_terminated(required[index],
                                        D1L_UI_STORAGE_TEXT_LEN) ||
            required[index][0] == '\0') {
            return false;
        }
    }
    for (size_t index = 0U; index < view_model->location_count; ++index) {
        const d1l_ui_storage_location_view_t *location =
            &view_model->locations[index];
        if (location->location != (d1l_ui_storage_location_t)index ||
            !bounded_text_is_terminated(location->name,
                                        sizeof(location->name)) ||
            !bounded_text_is_terminated(location->value,
                                        sizeof(location->value)) ||
            location->name[0] == '\0' || location->value[0] == '\0') {
            return false;
        }
    }
    return true;
}

bool d1l_ui_storage_view(const d1l_ui_storage_view_input_t *input,
                         d1l_ui_storage_view_model_t *out_view)
{
    if (!input || !out_view) {
        return false;
    }
    memset(out_view, 0, sizeof(*out_view));
    out_view->needs_attention = storage_needs_attention(input) ||
        input->retained_backup_degraded;
    set_hero(input, &out_view->hero);

    const char *state = card_state(input);
    const char *ready = readiness(input);
    copy_text(out_view->card_summary, sizeof(out_view->card_summary), state);
    copy_text(out_view->data_summary, sizeof(out_view->data_summary),
              data_summary(input));
    copy_text(out_view->card.state, sizeof(out_view->card.state), state);
    copy_text(out_view->card.filesystem, sizeof(out_view->card.filesystem),
              filesystem(input));
    const bool capacity_known = input->sd_present && input->capacity_kb > 0U;
    format_kb(out_view->card.capacity, sizeof(out_view->card.capacity),
              input->capacity_kb, capacity_known);
    format_kb(out_view->card.free_space, sizeof(out_view->card.free_space),
              input->free_kb, capacity_known);
    copy_text(out_view->card.readiness, sizeof(out_view->card.readiness), ready);
    out_view->card.state_accent = card_value_accent(state);
    out_view->card.readiness_accent = card_value_accent(ready);

    out_view->location_count = D1L_UI_STORAGE_LOCATION_COUNT;
    set_location(&out_view->locations[D1L_UI_STORAGE_LOCATION_MESSAGES],
                 D1L_UI_STORAGE_LOCATION_MESSAGES, "Messages",
                 retained_backend(input, input->message_store_backend),
                 backend_accent(input->message_store_backend));
    set_location(&out_view->locations[D1L_UI_STORAGE_LOCATION_DIRECT_MESSAGES],
                 D1L_UI_STORAGE_LOCATION_DIRECT_MESSAGES, "Direct messages",
                 retained_backend(input, input->dm_store_backend),
                 backend_accent(input->dm_store_backend));
    set_location(&out_view->locations[D1L_UI_STORAGE_LOCATION_PACKETS],
                 D1L_UI_STORAGE_LOCATION_PACKETS, "Packets",
                 retained_backend(input, input->packet_log_backend),
                 backend_accent(input->packet_log_backend));
    set_location(&out_view->locations[D1L_UI_STORAGE_LOCATION_ROUTES],
                 D1L_UI_STORAGE_LOCATION_ROUTES, "Routes",
                 retained_backend(input, input->route_store_backend),
                 backend_accent(input->route_store_backend));
    set_location(&out_view->locations[D1L_UI_STORAGE_LOCATION_MAP_TILES],
                 D1L_UI_STORAGE_LOCATION_MAP_TILES, "Map tiles",
                 map_backend(input->map_tile_backend),
                 backend_accent(input->map_tile_backend));
    set_location(&out_view->locations[D1L_UI_STORAGE_LOCATION_EXPORTS],
                 D1L_UI_STORAGE_LOCATION_EXPORTS, "Exports",
                 export_backend(input->export_backend),
                 backend_accent(input->export_backend));

    return d1l_ui_storage_view_model_is_valid(out_view);
}
