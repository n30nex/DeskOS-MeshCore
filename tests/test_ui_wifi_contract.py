from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_wifi_sheet_has_one_bounded_persistent_owner():
    cmake = read("main/CMakeLists.txt")
    header = read("main/ui/ui_wifi.h")
    source = read("main/ui/ui_wifi.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert '"ui/ui_wifi.c"' in cmake
    assert '#include "ui_wifi.h"' in phase1
    assert "d1l_ui_wifi_controller_t" in header
    assert "D1L_UI_WIFI_CONTROLLER_MAX_BYTES 1024U" in header
    assert "_Static_assert(sizeof(d1l_ui_wifi_controller_t)" in source
    assert "s_wifi_controller EXT_RAM_BSS_ATTR" in phase1
    assert "static lv_obj_t *s_wifi_sheet" not in phase1
    assert "static lv_obj_t *s_wifi_ssid_textarea" not in phase1
    assert "static lv_obj_t *s_wifi_password_textarea" not in phase1
    assert "static lv_obj_t *s_wifi_keyboard" not in phase1


def test_wifi_callbacks_reject_stale_generation_and_route_owned_actions():
    header = read("main/ui/ui_wifi.h")
    connectivity_header = read("main/ui/ui_connectivity.h")
    source = read("main/ui/ui_wifi.c")
    phase1 = read("main/ui/ui_phase1.c")

    for action in ("CLOSE", "SAVE", "CLEAR", "SCAN", "CONNECT", "TOGGLE"):
        assert f"D1L_UI_WIFI_ACTION_{action}" in header
        assert f"D1L_UI_WIFI_ACTION_{action}" in phase1
    assert "binding->generation == binding->controller->generation" in source
    assert "controller->generation++" in source
    assert "controller->generation == 0U" in source
    assert "view_model_is_valid" in source
    assert "bounded_text_is_terminated" in source
    assert "sizeof(view_model->toggle_label)" in source
    assert "sizeof(view_model->password_placeholder)" in source
    assert "char toggle_label[D1L_UI_CONNECTIVITY_LABEL_LEN]" in connectivity_header
    assert "char password_placeholder[D1L_UI_CONNECTIVITY_LABEL_LEN]" in connectivity_header
    assert "wifi_action_handler" in phase1
    assert "d1l_app_model_" not in source
    assert "request_content_refresh" not in source


def test_wifi_credentials_are_bounded_redacted_and_cleared_on_hide():
    header = read("main/ui/ui_wifi.h")
    source = read("main/ui/ui_wifi.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "D1L_WIFI_SSID_LEN - 1U" in source
    assert "D1L_WIFI_PASSWORD_LEN - 1U" in source
    assert "lv_textarea_set_password_mode(textarea, password)" in source
    assert 'lv_textarea_set_text(controller->password_textarea, "")' in source
    assert "d1l_ui_keyboard_clear_textarea(controller->keyboard)" in source
    render = source.split("bool d1l_ui_wifi_render", 1)[1].split(
        "void d1l_ui_wifi_deactivate", 1
    )[0]
    assert render.index("clear_sensitive_input(controller);") < render.index(
        "lv_obj_clean(controller->sheet);"
    )
    assert "char password[" not in header
    assert "fprintf(" not in source
    assert "puts(" not in source
    assert "ESP_LOG" not in source
    assert "d1l_ui_wifi_deactivate(&s_wifi_controller)" in phase1


def test_wifi_recreate_and_invalid_render_fail_closed():
    source = read("main/ui/ui_wifi.c")
    phase1 = read("main/ui/ui_phase1.c")

    create = source.split("bool d1l_ui_wifi_create", 1)[1].split(
        "bool d1l_ui_wifi_render", 1
    )[0]
    assert "if (controller->sheet)" in create
    assert "lv_obj_is_valid(controller->sheet)" in create
    assert "d1l_ui_modal_hide(controller->sheet)" in create
    assert "lv_obj_del(controller->sheet)" in create
    assert "memset(controller, 0, sizeof(*controller))" in create

    render = source.split("bool d1l_ui_wifi_render", 1)[1].split(
        "void d1l_ui_wifi_deactivate", 1
    )[0]
    invalid = render.split("if (!action_handler || !view_model_is_valid(view_model))", 1)[1]
    assert invalid.index("invalidate_render(controller);") < invalid.index(
        "return false;"
    )
    invalidate = source.split("static void invalidate_render", 1)[1].split(
        "bool d1l_ui_wifi_create", 1
    )[0]
    assert "controller->action_handler = NULL" in invalidate
    assert "memset(controller->bindings, 0" in invalidate
    assert "d1l_ui_modal_hide(controller->sheet)" in invalidate
    assert "lv_obj_clean(controller->sheet)" in invalidate
    assert "static bool render_wifi_sheet(void)" in phase1
    assert "const bool wifi_rendered = render_wifi_sheet();" in phase1
    assert "if (wifi_rendered && wifi_sheet)" in phase1
    assert "if (!render_wifi_sheet())" in phase1
    assert "hide_wifi_sheet();" in phase1


def test_wifi_renderer_preserves_available_and_unavailable_surfaces():
    source = read("main/ui/ui_wifi.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert '"Wi-Fi Setup"' in source
    assert '"Profile and state"' in source
    assert '"Network name"' in source
    assert '"Password"' in source
    for label in ("Close", "Save", "Delete", "Next", "Scan", "Connect"):
        assert f'"{label}"' in source
    assert "if (!controller->rendered.controls_available)" in source
    assert "controller->rendered.scan_line" in source
    assert "d1l_ui_wifi_sheet(&s_wifi_controller)" in phase1
    assert "d1l_ui_wifi_ssid_textarea(&s_wifi_controller)" in phase1
    assert "d1l_ui_wifi_password_textarea(&s_wifi_controller)" in phase1
    assert "d1l_ui_wifi_keyboard(&s_wifi_controller)" in phase1
