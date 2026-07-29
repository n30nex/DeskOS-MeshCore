from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def test_first_start_module_is_in_the_production_firmware_component():
    cmake = read("main/CMakeLists.txt")
    header = read("main/ui/ui_first_start.h")
    source = read("main/ui/ui_first_start.c")

    assert '"ui/ui_first_start.c"' in cmake
    assert "D1L_UI_FIRST_START_READINESS" in header
    assert "D1L_UI_FIRST_START_NAME" in header
    assert "D1L_UI_FIRST_START_CHANNELS" in header
    assert "d1l_ui_first_start_create" in source
    assert "lv_obj_set_size(controller->overlay, 480, 480)" in source
    assert "lv_obj_move_foreground(controller->overlay)" in source
    assert "D1L_FIRST_START_READY_HOLD_MS 650U" in source


def test_every_boot_readiness_overlay_gates_on_five_truthful_essential_rows():
    source = read("main/ui/ui_first_start.c")
    ready = body(
        source,
        "bool d1l_ui_first_start_essential_ready",
        "static void update_readiness_rows",
    )
    rows = body(
        source,
        "static void update_readiness_rows",
        "static void update_storage_map_page",
    )

    for field in (
        "snapshot->board_ready",
        "snapshot->identity_ready",
        "snapshot->radio_ready",
        "snapshot->radio_applied",
        "!snapshot->radio_apply_pending",
        "snapshot->storage_rp2040_bridge_ready",
        "snapshot->onboarding_complete",
        "d1l_ui_first_start_map_prepared(snapshot)",
        "snapshot->ui_ready",
    ):
        assert field in ready
    for label in ('"Display"', '"Identity"', '"Radio"', '"UI"'):
        assert label in rows
    assert (
        'snapshot->onboarding_complete ? "Storage & maps" : "Storage service"'
        in rows
    )
    assert "snapshot->onboarding_complete" in rows
    assert "d1l_ui_first_start_map_prepared(snapshot)" in rows
    assert '"%u of 5 essential systems ready"' in rows
    assert '"Prepared SD: %s   NRCan maps: %s"' in rows
    assert '"Needs FAT32"' in rows
    assert '"Not ready"' in rows
    assert "d1l_ui_first_start_sd_prepared(snapshot)" in rows
    assert "d1l_ui_first_start_map_prepared(snapshot)" in rows


def test_identity_is_ready_before_overlay_without_transmitting_from_fresh_boot():
    app_main = read("main/app_main.c")
    init = app_main.index("d1l_meshcore_service_init();")
    identity = app_main.index("d1l_meshcore_service_ensure_identity();", init)
    rx = app_main.index("d1l_meshcore_service_start_rx_async()", identity)
    onboarded_guard = app_main.index("else if (onboarding_complete)", rx)
    advert = app_main.index(
        "d1l_meshcore_service_request_boot_advert(true)", onboarded_guard
    )

    assert init < identity < rx < onboarded_guard < advert
    assert "performs no RF transmission" in app_main[init:rx]
    assert "factory-fresh unit remains silent" in app_main[init:rx]


def test_name_is_explicit_and_coordinates_are_manual_decimal_values():
    source = read("main/ui/ui_first_start.c")
    name = body(source, "static void render_name", "static void render_location")
    location = body(
        source, "static void render_location", "static void render_wifi"
    )
    parser = body(
        source,
        "bool d1l_ui_first_start_parse_coordinate_e7",
        "bool d1l_ui_first_start_sd_prepared",
    )
    save = body(
        source,
        "static void save_name_and_advance",
        "static void save_location_and_advance",
    )

    assert '"Required: 1-31 characters"' in name
    assert "controller->node_name" in name
    assert "D1L_NODE_NAME_FACTORY_DEFAULT" in save
    assert "d1l_settings_node_name_valid" in save
    assert '"Latitude (-90 to 90)"' in location
    assert '"Longitude (-180 to 180)"' in location
    assert "No coordinates " in location
    assert "10000000LL" in parser
    assert "d1l_app_model_set_map_location" in source
    for forbidden in ("Toronto", "Ottawa", "Vancouver", "43."):
        assert forbidden not in source


def test_wifi_is_optional_masked_and_wiped_before_leaving_the_page():
    source = read("main/ui/ui_first_start.c")
    wifi = body(source, "static void render_wifi", "static void render_radio")
    save = body(
        source,
        "static void save_wifi_and_advance",
        "static void confirm_radio_and_advance",
    )
    skip = body(source, "static void handle_skip", "static void handle_next")

    assert "d1l_app_model_clear_map_location()" in skip
    assert '"Wi-Fi password"' in wifi
    assert "D1L_WIFI_PASSWORD_LEN - 1U, true" in wifi
    assert '"Skip keeps Wi-Fi off.' in wifi
    assert "d1l_app_model_save_wifi_profile" in save
    assert "clear_sensitive_input(controller)" in save
    assert "d1l_app_model_set_wifi_enabled(true)" in save
    assert "d1l_app_model_set_wifi_enabled(false)" in skip
    assert "clear_sensitive_input(controller)" in skip
    assert "lv_textarea_set_password_mode(textarea, password)" in source
    assert 'lv_textarea_set_text(controller->secondary_textarea, "")' in source


def test_radio_storage_map_and_channels_are_production_truth_not_test_gates():
    source = read("main/ui/ui_first_start.c")
    radio = body(
        source, "static void render_radio", "static void render_storage_map"
    )
    storage = body(
        source, "static void render_storage_map", "static void render_channels"
    )
    channels = body(
        source, "static void render_channels", "static void render_finishing"
    )
    confirm = body(
        source,
        "static void confirm_radio_and_advance",
        "static void finish_onboarding",
    )

    for value in (
        "910.525 MHz",
        "Bandwidth 62.5 kHz",
        "Spreading factor 7",
        "Coding rate 5",
    ):
        assert value in radio
    assert "d1l_app_model_default_radio_profile" in confirm
    assert "d1l_app_model_save_radio_profile" in confirm
    assert "910525000UL" in source
    assert "625U" in source
    assert "DeskOS firmware never formats cards." in storage
    assert '"STEP 5 OF 6 - REQUIRED"' in storage
    assert '"1.0 requires a prepared FAT32 SD card.' in storage
    assert "map/offline-provider.json" in storage
    assert "Natural Resources Canada provider manifest." in storage
    assert "controller->next_button, LV_STATE_DISABLED" in storage
    assert "d1l_storage_status_mount" not in source
    assert "d1l_storage_manager_request_remount" not in source
    for channel in ("Public   selected by default", "#bot", "#test"):
        assert channel in channels
    assert "test packet" not in channels.lower()


def test_required_prepared_media_gates_channel_and_finish_steps():
    source = read("main/ui/ui_first_start.c")
    update = body(
        source,
        "static void update_storage_map_page",
        "static void save_name_and_advance",
    )
    next_action = body(
        source,
        "static void handle_next",
        "static void handle_action",
    )

    assert "controller->media_ready = sd_ready && map_ready;" in update
    assert "lv_obj_clear_state(" in update
    assert "lv_obj_add_state(" in update
    assert "if (!controller->media_ready)" in next_action
    assert "Insert the prepared FAT32 card with its NRCan provider manifest." in next_action


def test_finish_uses_canonical_onboarding_and_existing_users_bypass_wizard():
    source = read("main/ui/ui_first_start.c")
    model = read("main/app/app_model.c")
    finish = body(
        source,
        "static void finish_onboarding",
        "static void handle_back",
    )
    update = body(
        source,
        "void d1l_ui_first_start_update",
        "void d1l_ui_first_start_deactivate",
    )

    assert "d1l_app_model_complete_onboarding(controller->node_name)" in finish
    assert "d1l_settings_complete_onboarding" not in source
    assert "node_name, before.wifi_enabled" in model
    assert "before.ble_companion_enabled, before.observer_enabled" in model
    assert "node_name, false, false, false" not in model
    assert "d1l_channel_store_seed_onboarding_defaults" not in source
    assert "if (snapshot->onboarding_complete)" in update
    assert "d1l_ui_first_start_deactivate(controller)" in update
    assert "render_name(controller)" in update


def test_snapshot_exposes_actual_provider_configuration_for_truthful_map_status():
    header = read("main/app/app_model.h")
    model = read("main/app/app_model.c")
    source = read("main/ui/ui_first_start.c")

    assert "bool map_tile_provider_configured;" in header
    assert "d1l_map_tile_provider_snapshot(&map_provider)" in model
    assert (
        "snapshot->map_tile_provider_configured = map_provider.configured;"
        in model
    )
    assert "snapshot->map_tile_provider_configured" in source


def test_wizard_touch_targets_are_at_least_44_pixels():
    source = read("main/ui/ui_first_start.c")

    assert "lv_obj_set_size(button, width, 48)" in source
    assert "lv_obj_set_size(textarea, 448, 44)" in source
    assert "lv_obj_set_size(controller->overlay, 480, 480)" in source


def test_wizard_input_actions_fail_closed_if_a_control_could_not_allocate():
    source = read("main/ui/ui_first_start.c")

    helper = body(
        source, "static const char *textarea_text_or_empty",
        "static void copy_text"
    )
    assert "object_valid(textarea)" in helper
    assert 'lv_textarea_get_text(textarea) : ""' in helper
    assert source.count("lv_textarea_get_text(") == 1
