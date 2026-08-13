from pathlib import Path

from tools import ui_simulator


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    assert start in source, start
    assert end in source, end
    return source.split(start, 1)[1].split(end, 1)[0]


def test_node_detail_has_one_bounded_owner_and_immutable_input_copy() -> None:
    cmake = read("main/CMakeLists.txt")
    header = read("main/ui/ui_node_detail.h")
    source = read("main/ui/ui_node_detail.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert '"ui/ui_node_detail.c"' in cmake
    assert '#include "ui_node_detail.h"' in phase1
    assert "D1L_UI_NODE_DETAIL_CONTROLLER_MAX_BYTES 512U" in header
    assert "d1l_node_view_t node;" in header
    assert "const d1l_node_view_t *node;" not in body(
        header,
        "typedef struct {\n    d1l_node_view_t node;",
        "} d1l_ui_node_detail_view_model_t;",
    )
    assert "controller->rendered = *view_model;" in source
    assert "view_model_is_valid" in source
    assert "bounded_text_is_terminated" in source
    assert "_Static_assert(sizeof(d1l_ui_node_detail_controller_t)" in source
    assert "s_node_detail_controller EXT_RAM_BSS_ATTR" in phase1
    assert "static lv_obj_t *s_node_detail_sheet" not in phase1
    assert "static d1l_node_view_t s_node_detail_node" not in phase1
    assert "static void render_node_detail_sheet" not in phase1


def test_node_detail_callbacks_are_generation_safe_and_cleanup_fail_closed() -> None:
    header = read("main/ui/ui_node_detail.h")
    source = read("main/ui/ui_node_detail.c")

    for action in ("CLOSE", "OPEN_DM", "EXPLAIN_DM"):
        assert f"D1L_UI_NODE_DETAIL_ACTION_{action}" in header
    assert "binding->generation == binding->controller->generation" in source
    assert "controller->generation++" in source
    assert "controller->generation == 0U" in source
    assert "controller->action_handler = NULL" in source
    assert "memset(controller->bindings, 0" in source
    invalidate = body(
        source, "static void invalidate_render", "static bool binding_is_current"
    )
    assert invalidate.index("deactivate_actions(controller);") < invalidate.index(
        "memset(&controller->rendered, 0"
    )
    assert invalidate.index("d1l_ui_modal_hide(controller->sheet);") < invalidate.index(
        "lv_obj_clean(controller->sheet);"
    )
    assert "d1l_app_model_" not in source
    assert "d1l_ui_map_viewport_reacquire" not in source


def test_node_detail_preserves_truthful_fields_geometry_and_actions() -> None:
    source = read("main/ui/ui_node_detail.c")
    phase1 = read("main/ui/ui_phase1.c")
    simulator = read("tools/ui_simulator.py")

    for exact in (
        "Contact",
        "%s route  |  %u hop%s  |  %s",
        "Last signal %d dBm  |  SNR %s%d.%d",
        "Shared location %s, %s",
        "Location not shared",
        "Heard on this boot  |  %lu sighting%s",
        "Messaging %s: %s",
        "Advanced identity  %.16s  |  key %s",
        "Admin",
        "Verified server; local authenticated login required.",
    ):
        assert f'"{exact}"' in source
    assert "GPS" not in source
    assert "lv_obj_set_size(controller->sheet, 480, 480);" in source
    assert "lv_obj_set_pos(controller->sheet, 0, 0);" in source
    assert "lv_obj_set_style_pad_all(controller->sheet, 12, 0);" in source
    assert '"Why no DM?", 174, 0, 120, 44' in source
    assert '"DM", 208, 0, 54, 44' in source
    assert '"Close", 316, 0, 76, 44' in source
    assert '"Admin", 8, 352, 104, 44' in source
    assert "lv_obj_set_pos(managed_reason, 120, 360);" in source
    assert 12 + 360 + 40 <= 480

    handler = body(
        phase1, "static void handle_node_detail_action", "static void show_node_detail_view"
    )
    assert "open_node_dm_for(event->node);" in handler
    assert "dm_identity_for_node(event->node, NULL)" in handler
    assert "event->return_to_map" in handler
    assert "d1l_ui_map_viewport_reacquire()" in handler
    assert "request_content_refresh();" in handler
    assert "d1l_app_model_send_dm_text" not in handler

    # The simulator remains the visual/flow oracle for this extracted renderer.
    for view_name in (
        '"node_detail_sheet"',
        '"heard_only_node_detail_sheet"',
        '"managed_node_detail_sheet"',
    ):
        assert view_name in simulator
    assert '"node_detail_content_clipped"' in simulator
    assert '"node_detail_return_destination"' in simulator


def test_node_detail_focused_simulator_views_remain_truthful(tmp_path: Path) -> None:
    report = ui_simulator.generate(
        tmp_path,
        views=(
            "node_detail_sheet",
            "heard_only_node_detail_sheet",
            "managed_node_detail_sheet",
        ),
    )
    assert report["ok"] is True
    views = {view["name"]: view for view in report["views"]}
    ready = views["node_detail_sheet"]
    heard_only = views["heard_only_node_detail_sheet"]
    managed = views["managed_node_detail_sheet"]

    assert ready["metrics"]["node_detail_dm_reason_code"] == "ready"
    assert ready["metrics"]["node_detail_dm_exact_full_key"] is True
    assert ready["metrics"]["node_detail_content_clipped"] is False
    assert heard_only["metrics"]["node_detail_dm_reason_code"] == "heard_only"
    assert heard_only["metrics"]["node_detail_dm_opens_compose"] is False
    assert heard_only["metrics"]["node_detail_dm_rf_tx"] is False
    assert managed["metrics"]["node_detail_management_gated"] is True
    assert managed["metrics"]["node_detail_content_bottom"] == 400
    assert managed["metrics"]["node_detail_content_clipped"] is False
    assert "Verified server; local authenticated login required." in managed["labels"]
