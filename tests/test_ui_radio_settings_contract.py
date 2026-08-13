from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    assert start in source, start
    assert end in source, end
    return source.split(start, 1)[1].split(end, 1)[0]


def test_radio_settings_has_one_bounded_persistent_owner():
    cmake = read("main/CMakeLists.txt")
    header = read("main/ui/ui_radio_settings.h")
    source = read("main/ui/ui_radio_settings.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert '"ui/ui_radio_settings.c"' in cmake
    assert '#include "ui_radio_settings.h"' in phase1
    assert "D1L_UI_RADIO_SETTINGS_CONTROLLER_MAX_BYTES 320U" in header
    assert "D1L_UI_RADIO_SETTINGS_BINDING_COUNT 12U" in header
    assert "_Static_assert(sizeof(d1l_ui_radio_settings_controller_t)" in source
    assert "s_radio_settings_controller EXT_RAM_BSS_ATTR" in phase1
    assert "static lv_obj_t *s_radio_settings_sheet" not in phase1
    assert "static d1l_app_radio_profile_edit_t s_radio_edit" not in phase1
    assert "create_radio_settings_sheet" not in phase1


def test_radio_callbacks_are_generation_guarded_and_app_free():
    header = read("main/ui/ui_radio_settings.h")
    source = read("main/ui/ui_radio_settings.c")
    phase1 = read("main/ui/ui_phase1.c")

    for action in (
        "FREQ_DOWN",
        "FREQ_UP",
        "BANDWIDTH",
        "SF_DOWN",
        "SF_UP",
        "CODING_RATE",
        "TX_DOWN",
        "TX_UP",
        "RX_BOOST",
        "DEFAULTS",
        "SAVE",
        "CLOSE",
    ):
        assert f"D1L_UI_RADIO_SETTINGS_ACTION_{action}" in header
    assert "binding->generation == binding->controller->generation" in source
    assert "binding->generation != 0U" in source
    assert "controller->generation++" in source
    assert "controller->generation == 0U" in source
    assert "controller->action_handler = NULL" in source
    assert "memset(controller->bindings, 0" in source
    assert "d1l_app_model_" not in source
    assert "request_content_refresh" not in source
    assert "d1l_app_model_save_radio_profile(edit)" in phase1
    assert "d1l_app_model_default_radio_profile(&defaults)" in phase1


def test_radio_adjustments_preserve_safe_bounds_and_cycle_policy():
    source = read("main/ui/ui_radio_settings.c")
    adjust = body(source, "static void apply_adjustment", "static bool action_adjusts_edit")

    assert "frequency_hz >= 902025000UL" in adjust
    assert "frequency_hz -= 25000UL" in adjust
    assert "frequency_hz <= 927975000UL" in adjust
    assert "frequency_hz += 25000UL" in adjust
    assert "spreading_factor > 5U" in adjust
    assert "spreading_factor < 12U" in adjust
    assert "coding_rate >= 8U" in adjust
    assert "5U : (uint8_t)(controller->edit.coding_rate + 1U)" in adjust
    assert "tx_power_dbm > -9" in adjust
    assert "tx_power_dbm < D1L_RADIO_TX_POWER_DBM" in adjust
    assert "controller->edit.rx_boost = !controller->edit.rx_boost" in adjust
    assert "{625U, 1250U, 2500U, 5000U}" in source


def test_radio_recreate_and_incomplete_render_fail_closed():
    source = read("main/ui/ui_radio_settings.c")
    phase1 = read("main/ui/ui_phase1.c")

    create = body(
        source,
        "bool d1l_ui_radio_settings_create",
        "bool d1l_ui_radio_settings_set_edit",
    )
    assert "controller->sheet && lv_obj_is_valid(controller->sheet)" in create
    assert create.index("d1l_ui_modal_hide(controller->sheet);") < create.index(
        "lv_obj_del(controller->sheet);"
    )
    assert "memset(controller, 0, sizeof(*controller));" in create
    assert "!parent || !lv_obj_is_valid(parent)" in create

    invalidate = body(
        source,
        "static void invalidate_render",
        "bool d1l_ui_radio_settings_create",
    )
    assert "deactivate_actions(controller);" in invalidate
    assert invalidate.index("d1l_ui_modal_hide(controller->sheet);") < invalidate.index(
        "lv_obj_clean(controller->sheet);"
    )

    render = body(
        source,
        "bool d1l_ui_radio_settings_render",
        "void d1l_ui_radio_settings_deactivate",
    )
    assert render.count("invalidate_render(controller);") >= 2
    assert "!lv_obj_is_valid(controller->sheet) || !action_handler" in render
    assert "if (!complete)" in render
    assert "static bool render_radio_settings_sheet(void)" in phase1
    assert "if (render_radio_settings_sheet())" in phase1
    assert "if (!render_radio_settings_sheet())" in phase1
    assert "hide_radio_settings_sheet();" in phase1


def test_radio_renderer_preserves_layout_labels_and_all_touch_bindings():
    source = read("main/ui/ui_radio_settings.c")

    for label in (
        "Radio",
        "Canada preset",
        "Radio matches saved settings",
        "Saved settings apply when the radio restarts",
        "Radio status unavailable",
        "Frequency %.3f MHz",
        "-25k",
        "+25k",
        "Bandwidth %.1f kHz",
        "Change bandwidth",
        "Spread %u",
        "Coding %u",
        "Change",
        "Power %d dBm",
        "Receive boost: On",
        "Receive boost: Off",
        "Restore Canada",
        "Save",
        "Close",
    ):
        assert f'"{label}"' in source
    assert "lv_obj_set_size(controller->sheet, 480, 480);" in source
    assert "lv_obj_set_pos(controller->sheet, 0, 0);" in source
    assert source.count("BINDING_") >= 25
    assert "BINDING_TOP_CLOSE = 0" in source
    assert "BINDING_BOTTOM_CLOSE" not in source
    assert "lv_obj_add_event_cb(button, action_event_cb, LV_EVENT_CLICKED, binding);" in source
