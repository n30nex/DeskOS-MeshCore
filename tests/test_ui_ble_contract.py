from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    assert start in source, start
    assert end in source, end
    return source.split(start, 1)[1].split(end, 1)[0]


def test_ble_sheet_has_one_bounded_persistent_owner():
    cmake = read("main/CMakeLists.txt")
    header = read("main/ui/ui_ble.h")
    source = read("main/ui/ui_ble.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert '"ui/ui_ble.c"' in cmake
    assert '#include "ui_ble.h"' in phase1
    assert "D1L_UI_BLE_CONTROLLER_MAX_BYTES 640U" in header
    assert "d1l_ui_ble_controller_t" in header
    assert "_Static_assert(sizeof(d1l_ui_ble_controller_t)" in source
    assert "s_ble_controller EXT_RAM_BSS_ATTR" in phase1
    assert "static lv_obj_t *s_ble_sheet" not in phase1
    assert "create_ble_sheet" not in phase1


def test_ble_callbacks_reject_stale_generation_and_keep_app_actions_in_phase1():
    header = read("main/ui/ui_ble.h")
    source = read("main/ui/ui_ble.c")
    phase1 = read("main/ui/ui_phase1.c")

    for action in ("CLOSE", "TOGGLE", "PAIR", "FORGET"):
        assert f"D1L_UI_BLE_ACTION_{action}" in header
        assert f"D1L_UI_BLE_ACTION_{action}" in phase1
    assert "binding->generation == binding->controller->generation" in source
    assert "controller->generation++" in source
    assert "controller->generation == 0U" in source
    assert "controller->action_handler = NULL" in source
    assert "memset(controller->bindings, 0" in source
    assert "d1l_app_model_" not in source
    assert "request_content_refresh" not in source
    assert "d1l_app_model_set_ble_enabled" in phase1


def test_ble_recreate_and_invalid_render_fail_closed():
    source = read("main/ui/ui_ble.c")
    phase1 = read("main/ui/ui_phase1.c")

    create = body(source, "bool d1l_ui_ble_create", "bool d1l_ui_ble_render")
    assert "if (controller->sheet)" in create
    assert "lv_obj_is_valid(controller->sheet)" in create
    assert create.index("d1l_ui_modal_hide(controller->sheet);") < create.index(
        "lv_obj_del(controller->sheet);"
    )
    assert "memset(controller, 0, sizeof(*controller))" in create
    assert "!parent || !lv_obj_is_valid(parent)" in create

    invalidate = body(source, "static void invalidate_render", "static bool binding_is_current")
    assert "deactivate_actions(controller);" in invalidate
    assert "memset(&controller->rendered, 0" in invalidate
    assert invalidate.index("d1l_ui_modal_hide(controller->sheet);") < invalidate.index(
        "lv_obj_clean(controller->sheet);"
    )

    render = body(source, "bool d1l_ui_ble_render", "void d1l_ui_ble_deactivate")
    assert render.count("invalidate_render(controller);") >= 2
    assert "if (!action_handler || !view_model_is_valid(view_model))" in render
    assert "if (!complete)" in render
    assert "static bool render_ble_sheet(void)" in phase1
    assert "if (render_ble_sheet())" in phase1
    assert "if (!render_ble_sheet())" in phase1
    assert "hide_ble_sheet();" in phase1


def test_ble_view_text_is_owned_bounded_and_validated_before_render():
    connectivity_header = read("main/ui/ui_connectivity.h")
    connectivity = read("main/ui/ui_connectivity.c")
    source = read("main/ui/ui_ble.c")

    for field in ("purpose", "runtime_note", "toggle_label", "production_note"):
        assert f"const char *{field};" not in connectivity_header
        assert f"out_view->{field}" in connectivity
        assert f"sizeof(view_model->{field})" in source
    assert "bounded_text_is_terminated" in source
    assert "view_model_is_valid" in source
    assert "controller->rendered = *view_model" in source


def test_ble_renderer_preserves_unavailable_truth_and_enables_real_actions():
    source = read("main/ui/ui_ble.c")
    connectivity = read("main/ui/ui_connectivity.c")

    assert '"BLE Setup"' in source
    assert '"Enable unavailable"' in source
    assert '"Pair unavailable"' in source
    assert '"Forget unavailable"' in source
    assert '"Pair"' in source
    assert '"Forget"' in source
    assert source.count("lv_obj_add_state(") >= 2
    assert "LV_STATE_DISABLED" in source
    assert "D1L_UI_BLE_ACTION_PAIR" in source
    assert "D1L_UI_BLE_ACTION_FORGET" in source
    assert "pairing_available" in source
    assert "forget_available" in source
    assert '"BLE companion transport is unavailable in this release."' in connectivity
    assert '"No BLE pairing or transport runtime is present."' in connectivity
    assert '"USB remains available for recovery and diagnostics."' in connectivity
