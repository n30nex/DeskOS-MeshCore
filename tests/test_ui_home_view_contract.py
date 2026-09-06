from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_home_truth_and_event_lifetime_are_owned_outside_phase1():
    cmake = read("main/CMakeLists.txt")
    phase1 = read("main/ui/ui_phase1.c")
    renderer = read("main/ui/ui_home.c")
    renderer_header = read("main/ui/ui_home.h")
    view = read("main/ui/ui_home_view.c")
    view_header = read("main/ui/ui_home_view.h")

    assert '"ui/ui_home_view.c"' in cmake
    assert "d1l_ui_home_view_input_t" in view_header
    assert "d1l_ui_home_view_model_t" in view_header
    assert "d1l_ui_home_view(&input, &s_home_controller.rendered)" in phase1
    assert "d1l_ui_home_controller_t s_home_controller" in phase1
    assert "d1l_ui_home_deactivate(&s_home_controller)" in phase1
    assert "d1l_ui_home_action_binding_t" in renderer_header
    assert "binding->controller->action_handler" in renderer
    assert "controller->rendered = *view_model" in renderer

    assert "lv_" not in view
    assert "d1l_app_model" not in view
    assert "freertos/" not in view
    assert "d1l_app_snapshot_t" not in renderer
    assert "snapshot->" not in renderer
    assert "s_action_handler" not in renderer
    assert "k_action_values" not in renderer


def test_home_view_preserves_truthful_storage_and_map_states():
    view = read("main/ui/ui_home_view.c")

    for state in (
        "mount_pending",
        "mount_required",
        "no_card",
        "not_fat32_or_unmountable",
        "creating_deskos_files",
        "deskos_manifest_invalid",
        "protocol_pending",
        "fat32_ready",
    ):
        assert f'"{state}"' in view
    assert 'return "needs FAT32";' in view
    assert 'map_status = "Offline mode";' in view
    assert 'map_status = "Set a location";' in view
