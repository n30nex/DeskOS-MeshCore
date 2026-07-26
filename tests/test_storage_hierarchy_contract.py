from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_slice(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start + len(signature))
    return source[start:end]


def test_storage_probe_surfaces_are_canonical_settings_destinations():
    navigation = read("main/ui/ui_navigation.c")
    console = read("main/comms/usb_console.c")
    probe = read("scripts/scroll_probe_d1l.py")

    mapping = function_slice(
        navigation,
        "bool d1l_ui_scroll_surface_from_name",
        "void d1l_ui_navigation_request",
    )
    for surface in ("storage", "storage_card", "storage_data"):
        branch = mapping.split(f'strcmp(normalized, "{surface}")', 1)[1].split(
            "} else", 1
        )[0]
        assert f'surface = "{surface}";' in branch
        assert "screen = D1L_UI_SCREEN_SETTINGS;" in branch

    assert '\\"storage_probe_surfaces\\":[\\"storage\\",\\"storage_card\\",\\"storage_data\\"]' in console
    assert 'SCROLL_MOVEMENT_OPTIONAL = {"home", "storage", "storage_card", *MAP_READ_ONLY_SURFACES}' in probe
    for surface in ("home", "storage", "storage_card"):
        assert f'"{surface}"' in probe
    assert '"storage_data": {"tab": "settings", "label": "Data locations"}' in probe


def test_storage_touch_hierarchy_is_full_height_read_only_and_plain_language():
    source = read("main/ui/ui_phase1.c")

    create = function_slice(source, "static void create_storage_sheet", "static void create_route_detail_sheet")
    assert "lv_obj_set_size(s_storage_sheet, 480, 424);" in create
    assert "lv_obj_set_pos(s_storage_sheet, 0, 56);" in create
    assert "lv_obj_clear_flag(s_storage_sheet, LV_OBJ_FLAG_SCROLLABLE);" in create

    root = function_slice(source, "static void render_storage_root", "static void render_storage_card_status")
    assert root.count("create_button") == 2
    assert "open_storage_card_status_event_cb" in root
    assert "open_storage_data_locations_event_cb" in root

    rendered_pages = function_slice(
        source,
        "static void render_storage_root",
        "static void open_storage_sheet_event_cb",
    )
    for raw_slug in (
        "prepare_fat32_on_computer",
        "inspect_rp2040_sd",
        "sd_pending_store_migration",
        "not_fat32_or_unmountable",
    ):
        assert raw_slug not in rendered_pages
    for mutator in (
        "d1l_storage_status_mount",
        "d1l_storage_status_remount",
        "d1l_storage_manager_reset_bridge",
        "d1l_storage_manager_force_nvs",
        "confirm_format",
        "delete_storage",
    ):
        assert mutator not in rendered_pages


def test_storage_retained_sd_degradation_is_copied_into_touch_snapshot():
    header = read("main/app/app_model.h")
    source = read("main/app/app_model.c")

    snapshot_struct = header.split("typedef struct {", 6)[-1].split(
        "} d1l_app_snapshot_t;", 1
    )[0]
    snapshot_copy = function_slice(
        source,
        "void d1l_app_model_snapshot",
        "esp_err_t d1l_app_model_send_public_text",
    )

    assert "bool storage_retained_sd_degraded;" in snapshot_struct
    assert "bool storage_retained_backup_degraded;" in snapshot_struct
    assert (
        "snapshot->storage_retained_sd_degraded = storage.retained_sd_degraded;"
        in snapshot_copy
    )
    assert (
        "snapshot->storage_retained_backup_degraded = storage.retained_backup_degraded;"
        in snapshot_copy
    )
    assert "const char *node_store_backend;" in snapshot_struct
    assert "snapshot->node_store_backend = storage.node_store_backend;" in snapshot_copy
    assert "const char *contact_store_backend;" in snapshot_struct
    assert "snapshot->contact_store_backend = storage.contact_store_backend;" in snapshot_copy
    assert "const char *read_state_backend;" in snapshot_struct
    assert "snapshot->read_state_backend = storage.read_state_backend;" in snapshot_copy


def test_storage_touch_ui_prioritizes_all_attention_states():
    source = read("main/ui/ui_storage_view.c")
    attention = function_slice(
        source,
        "static bool storage_needs_attention",
        "static const char *card_state",
    )
    state = function_slice(
        source,
        "static const char *card_state",
        "static const char *filesystem",
    )
    readiness = function_slice(
        source,
        "static const char *readiness",
        "static uint32_t card_value_accent",
    )
    hero = function_slice(
        source,
        "static void set_hero",
        "static void set_location",
    )

    for attention_signal in (
        "input->retained_sd_degraded",
        'input->sd_state, "error"',
        'input->sd_state, "bridge_reported"',
        'input->setup_action,\n                       "inspect_rp2040_sd_cmd0_firmware_path"',
        'input->setup_action,\n                       "inspect_rp2040_sd_mount_error_firmware_path"',
    ):
        assert attention_signal in attention
    assert state.index("storage_needs_attention(input)") < state.index(
        "input->sd_mounted && input->sd_data_root_ready"
    )
    assert readiness.index("storage_needs_attention(input)") < readiness.index(
        "input->sd_mounted && input->sd_data_root_ready"
    )
    assert hero.index("input->retained_backup_degraded") < hero.index(
        "if (input->retained_sd_degraded)",
        hero.index("input->retained_sd_degraded") + 1,
    )
    assert hero.index('text_equals(input->setup_action, "wait_for_storage_reconnect")') < hero.rindex(
        "} else if (input->data_enabled)"
    )
    assert 'state = "Card reader reconnecting";' in hero
    assert '"Last confirmed SD remains active briefly."' in hero
    assert '"History becomes live-only if status retries fail."' in hero
    assert "accent = COLOR_RED;" in hero


def test_storage_hero_distinguishes_live_only_mode_from_card_errors():
    source = read("main/ui/ui_storage_view.c")
    hero = function_slice(
        source,
        "static void set_hero",
        "static void set_location",
    )

    backup = hero.index("input->retained_backup_degraded")
    retained = hero.index(
        "if (input->retained_sd_degraded)",
        hero.index("input->retained_sd_degraded") + 1,
    )
    general_attention = hero.index("storage_needs_attention(input)")
    ready = hero.rindex("} else if (input->data_enabled)")
    assert backup < retained < general_attention < ready

    backup_branch = hero[backup:retained]
    assert 'state = "Saved storage needs attention";' in backup_branch
    assert 'detail = "SD saved-data access reported an error.";' in backup_branch
    assert 'guidance = "See USB diagnostics before relying on saved history.";' in backup_branch

    retained_branch = hero[retained:general_attention]
    assert 'state = "SD needs attention";' in retained_branch
    assert 'detail = "History is live only until SD recovers.";' in retained_branch
    assert 'guidance = "RF transmit, receive, and chat remain available.";' in retained_branch

    media_branch = hero[general_attention:hero.index('if (text_equals(input->setup_action, "bridge_unavailable"))')]
    assert 'state = "Card needs attention";' in media_branch
    assert 'detail = "History is live only.";' in media_branch
    assert 'guidance = "Technical details are available over USB.";' in media_branch


def test_storage_backup_degradation_redraws_and_avoids_false_internal_claims():
    source = read("main/ui/ui_phase1.c")
    view = read("main/ui/ui_storage_view.c")
    generation = function_slice(
        source,
        "static d1l_ui_content_generation_t content_generation_from_snapshot",
        "static bool content_generation_text_equal",
    )
    equality = function_slice(
        source,
        "static bool content_generation_equal",
        "static void remember_rendered_content_generation",
    )
    friendly = function_slice(
        view,
        "static const char *retained_backend",
        "static const char *map_backend",
    )

    assert "snapshot->storage_retained_backup_degraded" in generation
    assert "storage_retained_backup_degraded" in equality
    assert '"SD issue"' in friendly
    assert '"Live only - not saved"' in friendly
    assert '"Legacy internal"' in friendly


def test_storage_root_summary_counts_only_genuinely_sd_ready_backends():
    source = read("main/ui/ui_storage_view.c")
    uses_sd = function_slice(
        source,
        "static bool backend_uses_sd",
        "static const char *retained_backend",
    )
    summary = function_slice(
        source,
        "static const char *data_summary",
        "static void set_hero",
    )

    for backend in (
        "message_store_backend",
        "dm_store_backend",
        "packet_log_backend",
        "route_store_backend",
        "node_store_backend",
        "contact_store_backend",
        "read_state_backend",
        "map_tile_backend",
        "export_backend",
    ):
        assert f"input->{backend}" in summary
    assert summary.count("backend_uses_sd(") == 9
    assert 'return uses_sd ? "SD card" : "Live only";' in summary
    assert '"Live only; history not saved"' in summary
    assert "sd_pending_store_migration" not in uses_sd


def test_storage_touch_navigation_resets_to_root_consistently():
    source = read("main/ui/ui_phase1.c")
    hide = function_slice(
        source,
        "static void hide_storage_sheet",
        "static void hide_wifi_sheet",
    )
    back = function_slice(
        source,
        "static void storage_subpage_back_event_cb",
        "static void open_storage_card_status_event_cb",
    )
    open_sheet = function_slice(
        source,
        "static void open_storage_sheet_event_cb",
        "static void wifi_refresh_sheet",
    )

    for implementation in (hide, back, open_sheet):
        assert "s_storage_page = D1L_STORAGE_PAGE_ROOT;" in implementation
    assert back.index("s_storage_page = D1L_STORAGE_PAGE_ROOT;") < back.index(
        "render_storage_sheet();"
    )
    assert open_sheet.index("s_storage_page = D1L_STORAGE_PAGE_ROOT;") < open_sheet.index(
        "render_storage_sheet();"
    )


def test_storage_data_scrolls_behind_one_fixed_no_format_footer():
    source = read("main/ui/ui_phase1.c")
    data_page = function_slice(source, "static void render_storage_data_locations", "static void render_storage_sheet")
    render_sheet = function_slice(source, "static void render_storage_sheet", "static void open_storage_sheet_event_cb")
    safety = function_slice(source, "static void render_storage_safety_copy", "static void render_storage_detail_row")

    assert 'create_object(s_storage_sheet, "storage locations list")' in data_page
    assert "lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);" in data_page
    assert "lv_obj_set_scroll_dir(list, LV_DIR_VER);" in data_page
    assert "lv_obj_set_size(list, 448, 288);" in data_page
    assert "create_panel(s_storage_sheet, 16, 368, 448, 44)" in safety
    assert "This device never formats cards." in safety
    assert render_sheet.index("switch (s_storage_page)") < render_sheet.index(
        "render_storage_safety_copy();"
    )


def test_storage_data_probe_records_scroll_then_restores_top_for_capture():
    source = read("main/ui/ui_phase1.c")
    measure = function_slice(
        source,
        "static void measure_scroll_probe_target",
        "static lv_obj_t *scroll_probe_open_surface",
    )
    run = function_slice(
        source,
        "static void run_scroll_probe_on_ui_task",
        "static uint32_t begin_pending_scroll_probe",
    )

    assert measure.index(
        "lv_obj_scroll_to_y(target, LV_COORD_MAX, LV_ANIM_OFF);"
    ) < measure.index("result->after_y")
    assert measure.index("result->after_y") < measure.index("result->moved")

    measured = run.index("measure_scroll_probe_target(target, result);")
    scoped_restore = run.index('strcmp(canonical, "storage_data") == 0', measured)
    restore = run.index("lv_obj_scroll_to_y(target, 0, LV_ANIM_OFF);", scoped_restore)
    repaint = run.index("force_ui_layout_repaint();", restore)
    assert measured < scoped_restore < restore < repaint
    assert "result->" not in run[restore:]


def test_storage_simulator_declares_matching_safe_pages_and_scroll_contract():
    simulator = read("tools/ui_simulator.py")
    assert 'STORAGE_HIERARCHY_VIEWS = frozenset({"storage_setup_sheet", "storage_card_page", "storage_data_page"})' in simulator
    assert 'STORAGE_SAFETY_COPY = "FAT32 only - This device never formats cards."' in simulator
    assert '"storage_root_action_count": 2' in simulator
    assert '"storage_setup_action_hidden": True' in simulator
    assert '"storage_data_scroll_enabled": True' in simulator
    for action in ("open_storage_card", "open_storage_data", "close_storage_card", "close_storage_data"):
        assert f'"{action}"' in simulator


def test_release_docs_define_current_storage_and_map_acceptance():
    readme = read("README.md")
    guide = read("docs/USER_GUIDE_D1L.md")
    checklist = read("docs/RELEASE_CHECKLIST.md")
    test_plan = read("docs/TEST_PLAN_D1L.md").split("## Historical", 1)[0]

    for doc in (readme, guide, checklist, test_plan):
        assert "core_1_0" in doc
        assert "conditional" in doc
        assert "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0" in doc

    assert "SD as the primary store" in guide
    assert "Degraded mode keeps basic live RF" in guide
    assert "SD-primary retained data" in checklist
    assert "authorized Map download/offline revisit" in checklist
    assert "one consolidated physical" in checklist
    assert "No soak is required" in checklist
