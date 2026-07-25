#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_storage_view.h"

static d1l_ui_storage_view_input_t base_input(void)
{
    return (d1l_ui_storage_view_input_t) {
        .rp2040_bridge_required = true,
        .rp2040_bridge_ready = true,
        .rp2040_sd_protocol_supported = true,
        .sd_state = "no_card",
        .sd_filesystem = "unknown",
        .setup_action = "insert_card",
        .message_store_backend = "nvs",
        .dm_store_backend = "nvs",
        .packet_log_backend = "nvs",
        .route_store_backend = "nvs",
        .map_tile_backend = "unavailable",
        .export_backend = "usb",
    };
}

static void test_no_card_truth(void)
{
    d1l_ui_storage_view_input_t input = base_input();
    d1l_ui_storage_view_model_t view;
    assert(d1l_ui_storage_view(&input, &view));
    assert(d1l_ui_storage_view_model_is_valid(&view));
    assert(strcmp(view.hero.state, "No SD card") == 0);
    assert(strcmp(view.hero.detail, "Internal storage is active.") == 0);
    assert(strcmp(view.card_summary, "No card inserted") == 0);
    assert(strcmp(view.data_summary, "Internal") == 0);
    assert(strcmp(view.card.filesystem, "Not available") == 0);
    assert(strcmp(view.card.capacity, "Not reported") == 0);
    assert(strcmp(view.card.free_space, "Not reported") == 0);
    assert(strcmp(view.card.readiness, "No card") == 0);
    assert(view.location_count == D1L_UI_STORAGE_LOCATION_COUNT);
    assert(strcmp(view.locations[D1L_UI_STORAGE_LOCATION_MESSAGES].value,
                  "Internal") == 0);
    assert(strcmp(view.locations[D1L_UI_STORAGE_LOCATION_MAP_TILES].value,
                  "Unavailable") == 0);
    assert(strcmp(view.locations[D1L_UI_STORAGE_LOCATION_EXPORTS].value,
                  "USB only") == 0);
    assert(!view.needs_attention);
}

static void test_ready_sd_truth(void)
{
    d1l_ui_storage_view_input_t input = base_input();
    input.sd_present = true;
    input.sd_mounted = true;
    input.sd_data_root_ready = true;
    input.data_enabled = true;
    input.capacity_kb = 32U * 1024U * 1024U;
    input.free_kb = 1536U;
    input.sd_state = "ready";
    input.sd_filesystem = "fatfs";
    input.setup_action = "none";
    input.message_store_backend = "mixed";
    input.dm_store_backend = "sd";
    input.packet_log_backend = "mixed";
    input.route_store_backend = "sd";
    input.map_tile_backend = "sd_map_tiles_ready";
    input.export_backend = "sd_diagnostic_exports_ready";

    d1l_ui_storage_view_model_t view;
    assert(d1l_ui_storage_view(&input, &view));
    assert(strcmp(view.hero.state, "SD card ready") == 0);
    assert(strcmp(view.hero.guidance,
                  "Saved data stays mirrored internally.") == 0);
    assert(strcmp(view.card.state, "Ready") == 0);
    assert(strcmp(view.card.filesystem, "FAT32") == 0);
    assert(strcmp(view.card.capacity, "32.0 GB") == 0);
    assert(strcmp(view.card.free_space, "1 MB") == 0);
    assert(strcmp(view.card.readiness, "Ready") == 0);
    assert(strcmp(view.data_summary, "SD + internal") == 0);
    assert(strcmp(view.locations[D1L_UI_STORAGE_LOCATION_MESSAGES].value,
                  "SD + internal backup") == 0);
    assert(strcmp(view.locations[D1L_UI_STORAGE_LOCATION_MAP_TILES].value,
                  "SD card") == 0);
    assert(strcmp(view.locations[D1L_UI_STORAGE_LOCATION_EXPORTS].value,
                  "SD card") == 0);
    assert(!view.needs_attention);
}

static void test_reconnect_and_degraded_truth(void)
{
    d1l_ui_storage_view_input_t input = base_input();
    input.sd_present = true;
    input.sd_mounted = true;
    input.sd_data_root_ready = true;
    input.data_enabled = true;
    input.setup_action = "wait_for_storage_reconnect";
    input.sd_filesystem = "fat32";

    d1l_ui_storage_view_model_t view;
    assert(d1l_ui_storage_view(&input, &view));
    assert(strcmp(view.hero.state, "Card reader reconnecting") == 0);
    assert(strcmp(view.hero.detail,
                  "Last confirmed SD remains active briefly.") == 0);
    assert(strcmp(view.card.readiness, "Reconnecting") == 0);

    input.retained_sd_degraded = true;
    input.retained_backup_degraded = true;
    input.message_store_backend = "mixed";
    assert(d1l_ui_storage_view(&input, &view));
    assert(strcmp(view.hero.state, "Saved storage needs attention") == 0);
    assert(strcmp(view.card.state, "Card needs attention") == 0);
    assert(strcmp(view.data_summary, "SD; backup issue") == 0);
    assert(strcmp(view.locations[D1L_UI_STORAGE_LOCATION_MESSAGES].value,
                  "SD; backup degraded") == 0);
    assert(view.needs_attention);
}

static void test_fail_closed_card_truth(void)
{
    d1l_ui_storage_view_input_t input = base_input();
    d1l_ui_storage_view_model_t view;

    input.sd_present = true;
    input.sd_needs_fat32 = true;
    input.sd_filesystem = "exfat";
    input.setup_action = "prepare_fat32_on_computer";
    assert(d1l_ui_storage_view(&input, &view));
    assert(strcmp(view.hero.state, "Card needs FAT32") == 0);
    assert(strcmp(view.card.state, "FAT32 card required") == 0);
    assert(strcmp(view.card.filesystem, "FAT32 required") == 0);
    assert(strcmp(view.card.readiness, "Needs FAT32") == 0);

    input.sd_needs_fat32 = false;
    input.sd_state = "bridge_reported";
    input.setup_action = "inspect_rp2040_sd_mount_error_firmware_path";
    assert(d1l_ui_storage_view(&input, &view));
    assert(strcmp(view.hero.state, "Card needs attention") == 0);
    assert(strcmp(view.card.readiness, "Needs attention") == 0);
    assert(view.needs_attention);
}

static void test_validation_rejects_corruption(void)
{
    d1l_ui_storage_view_input_t input = base_input();
    d1l_ui_storage_view_model_t view;
    assert(!d1l_ui_storage_view(NULL, &view));
    assert(!d1l_ui_storage_view(&input, NULL));
    assert(d1l_ui_storage_view(&input, &view));

    memset(view.hero.state, 'X', sizeof(view.hero.state));
    assert(!d1l_ui_storage_view_model_is_valid(&view));
    assert(d1l_ui_storage_view(&input, &view));
    view.location_count = D1L_UI_STORAGE_LOCATION_COUNT - 1U;
    assert(!d1l_ui_storage_view_model_is_valid(&view));
    assert(d1l_ui_storage_view(&input, &view));
    view.locations[0].location = D1L_UI_STORAGE_LOCATION_EXPORTS;
    assert(!d1l_ui_storage_view_model_is_valid(&view));
}

static void test_release_profile_filters_only_unavailable_map_location(void)
{
    assert(d1l_ui_storage_location_available(
        D1L_UI_STORAGE_LOCATION_MESSAGES));
    assert(d1l_ui_storage_location_available(
        D1L_UI_STORAGE_LOCATION_DIRECT_MESSAGES));
    assert(d1l_ui_storage_location_available(
        D1L_UI_STORAGE_LOCATION_PACKETS));
    assert(d1l_ui_storage_location_available(
        D1L_UI_STORAGE_LOCATION_ROUTES));
    assert(d1l_ui_storage_location_available(
        D1L_UI_STORAGE_LOCATION_EXPORTS));
    assert(d1l_ui_storage_location_available(
               D1L_UI_STORAGE_LOCATION_MAP_TILES) == !EXPECT_CORE);
    assert(!d1l_ui_storage_location_available(
        (d1l_ui_storage_location_t)D1L_UI_STORAGE_LOCATION_COUNT));
}

int main(void)
{
    test_no_card_truth();
    test_ready_sd_truth();
    test_reconnect_and_degraded_truth();
    test_fail_closed_card_truth();
    test_validation_rejects_corruption();
    test_release_profile_filters_only_unavailable_map_location();
    puts("native UI Storage view model: ok");
    return 0;
}
