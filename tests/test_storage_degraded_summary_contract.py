from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def test_home_and_top_bar_prioritize_retained_sd_degradation_over_ready_state():
    source = read("main/ui/ui_home_view.c")
    shell = read("main/ui/ui_phase1.c")
    state_body = function_body(
        source,
        "const char *d1l_ui_home_sd_state",
        "static const char *map_storage_state",
    )

    degraded = "if (storage_needs_attention(input))"
    ready = "if (input->storage_data_enabled || input->storage_sd_data_root_ready)"
    assert degraded in state_body
    assert state_body.index(degraded) < state_body.index(ready)
    assert 'return "Needs attention";' in state_body

    color_table = source.split("out_view->sd_value_color =", 1)[1].split(";", 1)[0]
    assert "out_view->storage_needs_attention ? 0xF87171U" in color_table
    assert color_table.index("storage_needs_attention") < color_table.index(
        "input->storage_data_enabled || input->storage_sd_data_root_ready"
    )

    top_bar = function_body(shell, "static void update_chrome", "static lv_obj_t *render_metric_card")
    assert 'label_set_fmt(s_identity_label, "Wi-Fi %s  BLE %s  SD %s"' in top_bar
    assert "d1l_ui_home_sd_state(&home_input)" in top_bar


def test_home_and_top_bar_never_render_raw_storage_state_slugs():
    source = read("main/ui/ui_home_view.c")
    state_body = function_body(
        source,
        "const char *d1l_ui_home_sd_state",
        "static const char *map_storage_state",
    )

    assert "return state;" not in state_body
    assert 'return "internal";' in state_body
    for raw_state in (
        "pending_bridge",
        "rp2040_unavailable",
        "unsupported",
    ):
        assert f'strcmp(state, "{raw_state}") == 0' in state_body


def test_more_sd_card_summary_uses_attention_status_and_warning_style_when_degraded():
    source = read("main/ui/ui_more_view.c")
    renderer = read("main/ui/ui_settings.c")
    render_body = source.split("bool d1l_ui_more_view(", 1)[1]
    status = render_body.split("const char *storage_status", 1)[1].split(";", 1)[0]

    assert "storage_attention" in status
    assert '"Needs attention"' in status
    assert "storage_ready" in status
    assert status.index("storage_attention") < status.index("storage_ready")

    readiness = render_body.split("const bool storage_ready", 1)[1].split(";", 1)[0]
    assert "input->storage_data_enabled" in readiness
    assert "input->storage_sd_data_root_ready" in readiness

    storage_rows = render_body.split(
        "D1L_UI_MORE_CATEGORY_STORAGE_MAPS", 1
    )[1].split("D1L_UI_MORE_CATEGORY_DEVICE", 1)[0]
    assert "storage_attention ? COLOR_RED" in storage_rows
    assert "storage_ready ? COLOR_GREEN" in storage_rows
    assert "D1L_UI_SETTINGS_ACTION_STORAGE, storage_attention" in storage_rows

    category_summary = render_body.split(
        "const char *storage_summary", 1
    )[1].split(";", 1)[0]
    assert "input->storage_retained_backup_degraded" in category_summary
    assert "sd_attention" in category_summary
    assert '"Storage needs attention"' in category_summary
    assert '"Backup needs attention"' in category_summary
    assert '"SD needs attention"' in category_summary

    assert "category->warning ? 0xFCA5A5 : 0xF4F7FB" in renderer
    assert "settings_create_row(parent, 444, 54, item->warning)" in renderer


def test_probe_and_reported_sd_errors_use_friendly_attention_summary_before_ready():
    home = read("main/ui/ui_home_view.c")
    more = read("main/ui/ui_more_view.c")
    settings = read("main/ui/ui_settings.c")
    raw_faults = (
        '"error"',
        '"bridge_reported"',
        '"inspect_rp2040_sd_cmd0_firmware_path"',
        '"inspect_rp2040_sd_mount_error_firmware_path"',
    )

    home_classifier = function_body(
        home,
        "static bool storage_needs_attention",
        "const char *d1l_ui_home_sd_state",
    )
    more_classifier = function_body(
        more,
        "static bool sd_needs_attention",
        "static const char *map_storage_state",
    )
    for raw_fault in raw_faults:
        assert raw_fault in home_classifier
        assert raw_fault in more_classifier

    home_summary = function_body(
        home,
        "const char *d1l_ui_home_sd_state",
        "static const char *map_storage_state",
    )
    assert home_summary.index("storage_needs_attention(input)") < home_summary.index(
        "input->storage_data_enabled || input->storage_sd_data_root_ready"
    )
    assert 'return "Needs attention";' in home_summary

    more_render = more.split("bool d1l_ui_more_view(", 1)[1]
    assert more_render.index("const bool sd_attention") < more_render.index(
        "const bool storage_ready"
    )
    more_readiness = more_render.split("const bool storage_ready", 1)[1].split(";", 1)[0]
    assert "input->storage_data_enabled" in more_readiness
    assert "input->storage_sd_data_root_ready" in more_readiness
    assert 'storage_attention ? "Needs attention"' in more_render

    for raw_fault in raw_faults:
        assert raw_fault not in home_summary
        assert raw_fault not in settings
    assert "snapshot->" not in settings
