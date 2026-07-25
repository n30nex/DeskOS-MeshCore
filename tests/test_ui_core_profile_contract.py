from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def test_core_dock_and_home_expose_only_the_five_contract_destinations():
    phase1 = read("main/ui/ui_phase1.c")
    home = read("main/ui/ui_home.c")

    core_dock = body(
        phase1,
        "static const d1l_ui_dock_item_t k_core_dock_items[]",
        "_Static_assert",
    )
    assert core_dock.count("{D1L_UI_TAB_") == 5
    for tab in ("HOME", "MESSAGES", "NODES", "PACKETS", "SETTINGS"):
        assert f"D1L_UI_TAB_{tab}" in core_dock
    assert "D1L_UI_TAB_MAP" not in core_dock
    assert "active_dock_items" in phase1
    assert 'map_available ? "Map" : "Packets"' in home
    assert 'map_available ? "Tools" : "Settings"' in home


def test_core_deep_links_are_denied_before_probe_or_screen_work():
    phase1 = read("main/ui/ui_phase1.c")
    navigation = read("main/ui/ui_navigation.c")

    request = body(
        phase1,
        "esp_err_t d1l_ui_phase1_request_tab",
        "esp_err_t d1l_ui_phase1_scroll_probe",
    )
    assert request.index("d1l_ui_screen_available(tab)") < request.index(
        "set_map_interactive_touch_authorized(false)"
    )
    assert "return ESP_ERR_NOT_SUPPORTED;" in request

    scroll = body(
        phase1,
        "esp_err_t d1l_ui_phase1_scroll_probe",
        "esp_err_t d1l_ui_phase1_compose_probe",
    )
    assert scroll.index("d1l_ui_scroll_surface_available(canonical, tab)") < (
        scroll.index("probe_gate_reserve_admission")
    )
    compose = body(
        phase1,
        "esp_err_t d1l_ui_phase1_compose_probe",
        "const char *d1l_ui_phase1_active_tab_name",
    )
    assert compose.index("compose_probe_target_available(canonical)") < (
        compose.index("probe_gate_reserve_admission")
    )
    assert "if (!d1l_ui_screen_available(screen))" in navigation


def test_core_does_not_create_excluded_controllers_or_map_timer():
    phase1 = read("main/ui/ui_phase1.c")
    startup = body(
        phase1,
        "esp_err_t d1l_ui_phase1_show_home",
        "esp_err_t d1l_ui_phase1_start",
    )

    for feature, call in (
        ("D1L_RELEASE_FEATURE_WIFI_USER_CONTROL", "d1l_ui_wifi_create"),
        ("D1L_RELEASE_FEATURE_BLE", "d1l_ui_ble_create"),
        ("D1L_RELEASE_FEATURE_MAP", "d1l_ui_map_sheets_create"),
        (
            "D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT",
            "d1l_ui_channel_sheets_create",
        ),
        ("D1L_RELEASE_FEATURE_USER_TRACE", "create_route_trace_sheet"),
        ("D1L_RELEASE_FEATURE_ADMIN", "create_mesh_roles_sheet"),
    ):
        guard = startup.split(call, 1)[0].rsplit(
            "d1l_release_feature_available", 1
        )[1]
        assert feature in guard
    timer_guard = startup.split("lv_timer_create(map_viewport_timer_cb", 1)[
        0
    ].rsplit("d1l_release_feature_available", 1)[1]
    assert "D1L_RELEASE_FEATURE_MAP" in timer_guard


def test_core_messages_contacts_settings_and_node_detail_hide_excluded_paths():
    phase1 = read("main/ui/ui_phase1.c")
    messages = read("main/ui/ui_messages.c")
    contacts = read("main/ui/ui_contact_sheets.c")
    settings = read("main/ui/ui_settings.c")
    node_detail = read("main/ui/ui_node_detail.c")

    messages_projection = body(
        phase1,
        "static void messages_view_model_from_snapshot",
        "static bool show_channel_selector_sheet",
    )
    assert "d1l_ui_messages_projected_channel_id(" in messages_projection
    assert "view_model->active_channel_id" in messages_projection
    assert "D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT" in messages
    assert "D1L_RELEASE_FEATURE_USER_TRACE" in contacts
    assert "D1L_RELEASE_FEATURE_ADVANCED_QR_EMOJI" in contacts
    assert "D1L_RELEASE_FEATURE_WIFI_USER_CONTROL" in settings
    assert "D1L_RELEASE_FEATURE_MUTABLE_TERMINAL" in settings
    assert "D1L_RELEASE_FEATURE_LOCATION" in node_detail
    assert "D1L_RELEASE_FEATURE_ADMIN" in node_detail


def test_core_storage_is_internal_until_sd_is_exactly_qualified():
    phase1 = read("main/ui/ui_phase1.c")
    home = read("main/ui/ui_home.c")
    settings = read("main/ui/ui_settings.c")

    assert '"Internal NVS"' in phase1
    assert '"Retained internal storage"' in phase1
    assert "D1L_RELEASE_FEATURE_SD_HISTORY" in phase1
    assert 'sd_history_available ? "SD" : "Storage"' in home
    assert 'sd_history_available ?' in home
    assert '"Internal NVS"' in settings
    assert "d1l_ui_home_view_apply_release_profile(out_input)" in phase1
    assert "d1l_ui_more_view_apply_release_profile(out_input)" in phase1
    assert "d1l_ui_storage_location_available(location->location)" in phase1


def test_core_copy_describes_only_reachable_conversations_and_unread_behavior():
    home = read("main/ui/ui_home.c")
    contacts = read("main/ui/ui_contact_sheets.c")
    messages = read("main/ui/ui_messages.c")

    assert '"Public and direct conversations"' in home
    assert "D1L_RELEASE_FEATURE_MULTI_CHANNEL_MANAGEMENT" in home
    assert '"Public, direct, and room conversations"' in home
    assert '"Include in unread count"' in contacts
    assert '"Exclude from unread count"' in contacts
    assert "Mute notifications" not in contacts
    assert "Unmute notifications" not in contacts
    assert "notification" not in messages.lower()
