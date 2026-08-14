from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    assert start in source, start
    assert end in source, end
    return source.split(start, 1)[1].split(end, 1)[0]


def test_device_sheets_have_one_small_persistent_owner():
    cmake = read("main/CMakeLists.txt")
    header = read("main/ui/ui_device_sheets.h")
    source = read("main/ui/ui_device_sheets.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert '"ui/ui_device_sheets.c"' in cmake
    assert '#include "ui_device_sheets.h"' in phase1
    assert "D1L_UI_DEVICE_SHEETS_CONTROLLER_MAX_BYTES 192U" in header
    assert "_Static_assert(sizeof(d1l_ui_device_sheets_controller_t)" in source
    assert "s_device_sheets_controller EXT_RAM_BSS_ATTR" in phase1
    assert "static lv_obj_t *s_display_sheet" not in phase1
    assert "static lv_obj_t *s_diagnostics_sheet" not in phase1
    assert "create_display_sheet" not in phase1
    assert "create_diagnostics_sheet" not in phase1


def test_device_sheet_create_replaces_both_and_rolls_back_partial_failure():
    source = read("main/ui/ui_device_sheets.c")
    delete = body(source, "static void delete_sheet", "static void destroy_sheets")
    destroy = body(source, "static void destroy_sheets", "bool d1l_ui_device_sheets_create")
    create = body(source, "bool d1l_ui_device_sheets_create", "static void invalidate_sheet")

    assert delete.index("d1l_ui_modal_hide(*sheet);") < delete.index(
        "lv_obj_del(*sheet);"
    )
    assert "delete_sheet(&controller->display_sheet);" in destroy
    assert "delete_sheet(&controller->diagnostics_sheet);" in destroy
    assert "memset(controller, 0, sizeof(*controller));" in destroy
    assert create.index("destroy_sheets(controller);") < create.index(
        "controller->display_sheet = create_sheet(parent, false);"
    )
    assert "controller->diagnostics_sheet = create_sheet(parent, true);" in create
    assert create.count("destroy_sheets(controller);") >= 3
    assert "lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);" in source
    assert "lv_obj_set_scrollbar_mode(sheet, LV_SCROLLBAR_MODE_AUTO);" in source


def test_device_sheet_actions_are_generation_guarded_and_app_free():
    header = read("main/ui/ui_device_sheets.h")
    source = read("main/ui/ui_device_sheets.c")
    phase1 = read("main/ui/ui_phase1.c")

    for action in ("CLOSE_DISPLAY", "CLOSE_DIAGNOSTICS"):
        assert f"D1L_UI_DEVICE_SHEETS_ACTION_{action}" in header
        assert f"D1L_UI_DEVICE_SHEETS_ACTION_{action}" in phase1
    assert "binding->generation == binding->controller->generation" in source
    assert "controller->generation++" in source
    assert "controller->generation == 0U" in source
    assert "controller->action_handler = NULL" in source
    assert "memset(controller->bindings, 0" in source
    assert "d1l_app_model_" not in source
    assert "request_content_refresh" not in source


def test_display_render_is_truthful_disabled_and_fails_closed():
    source = read("main/ui/ui_device_sheets.c")
    render = body(
        source,
        "bool d1l_ui_device_sheets_render_display",
        "static lv_obj_t *create_diagnostic_line",
    )

    for label in (
        "Brightness %u%%",
        "Night %s",
        "Contrast %s",
        "Timeout Off",
        "Timeout %u min",
        "Timeout %us",
    ):
        assert f'"{label}"' in render
    for action in (
        "BRIGHTNESS",
        "NIGHT_MODE",
        "HIGH_CONTRAST",
        "TIMEOUT",
        "TIMEZONE_MINUS",
        "TIMEZONE_PLUS",
    ):
        assert f"D1L_UI_DEVICE_SHEETS_ACTION_{action}" in render
    assert "const d1l_app_snapshot_t *snapshot" in render
    assert '"Local display time"' in render
    assert '"Use Time -1h/+1h to set the local clock.' in render
    assert '"Time -1h"' in render
    assert '"Time +1h"' in render
    assert "snapshot->timezone_settings_ready" in render
    assert "snapshot->time_available" in render
    assert "if (!complete)" in render
    assert "invalidate_sheet(controller, sheet);" in render


def test_display_locks_after_ten_minutes_and_top_button_wakes_safely():
    preferences_h = read("main/hal/display_preferences.h")
    preferences = read("main/hal/display_preferences.c")
    phase1 = read("main/ui/ui_phase1.c")
    gesture = read("main/ui/ui_button_gesture.h")

    assert "D1L_DISPLAY_TIMEOUT_DEFAULT_SECONDS 600U" in preferences_h
    valid = body(
        preferences,
        "bool d1l_display_timeout_valid",
        "static bool notification_mode_valid",
    )
    assert "seconds == 0U" in valid
    assert "D1L_DISPLAY_TIMEOUT_DEFAULT_SECONDS" in valid
    timeout_action = body(
        phase1,
        "case D1L_UI_DEVICE_SHEETS_ACTION_TIMEOUT:",
        "case D1L_UI_DEVICE_SHEETS_ACTION_CLOSE_DIAGNOSTICS:",
    )
    assert "D1L_DISPLAY_TIMEOUT_DEFAULT_SECONDS : 0U" in timeout_action

    timezone_action = body(
        phase1,
        "case D1L_UI_DEVICE_SHEETS_ACTION_TIMEZONE_MINUS:",
        "case D1L_UI_DEVICE_SHEETS_ACTION_CLOSE_DIAGNOSTICS:",
    )
    assert "d1l_app_model_set_timezone_offset_minutes(next)" in timezone_action
    assert "D1L_TIMEZONE_OFFSET_MINUTES_MIN" in timezone_action
    assert "D1L_TIMEZONE_OFFSET_MINUTES_MAX" in timezone_action

    lock = body(phase1, "static void lock_event_cb", "static uint8_t desired_backlight_percent")
    create = body(phase1, "static void create_lock_overlay", "static void ui_task")
    timer = body(phase1, "static void display_runtime_timer_cb", "static void map_viewport_timer_cb")
    assert "lv_obj_move_foreground(s_lock_overlay);" in lock
    assert "create_object(lv_layer_top(), \"lock overlay\")" in create
    assert "LV_OPA_COVER" in create
    assert "LV_OBJ_FLAG_CLICKABLE" in create
    assert "d1l_board_button_pressed()" in timer
    assert "D1L_UI_BUTTON_ACTION_WAKE" in timer
    assert "d1l_app_model_request_advert(false)" in timer
    assert "D1L_UI_BUTTON_DOUBLE_PRESS_MS 600U" in gesture
    assert "gesture->was_pressed" in gesture


def test_diagnostics_render_is_snapshot_only_and_read_only():
    source = read("main/ui/ui_device_sheets.c")
    render = body(
        source,
        "bool d1l_ui_device_sheets_render_diagnostics",
        "void d1l_ui_device_sheets_hide_display",
    )

    for line in (
        "reset %s  uptime %lus  mesh %s",
        "heap %luK free  min %luK  largest %luK",
        "LVGL free %lu  largest %lu  used %u%%",
        "ui stack %lu words  console stack %lu",
        "packets rx %lu tx %lu  rejected %lu",
    ):
        assert f'"{line}"' in render
    assert '"Crashlog  Events  Serial"' in render
    assert '"Terminal shows recent events and the current log level."' in render
    assert '"Open Terminal"' in render
    assert "D1L_UI_DEVICE_SHEETS_ACTION_OPEN_TERMINAL" in render
    assert "D1L_RELEASE_FEATURE_MUTABLE_TERMINAL" in render
    assert "if (!snapshot" in render
    assert "if (!complete)" in render
    assert "d1l_app_model_snapshot" not in source


def test_phase1_shows_only_successfully_rendered_owned_sheets():
    phase1 = read("main/ui/ui_phase1.c")

    assert "if (render_display_sheet())" in phase1
    assert "if (render_diagnostics_sheet())" in phase1
    assert "d1l_ui_device_sheets_display(&s_device_sheets_controller)" in phase1
    assert "d1l_ui_device_sheets_diagnostics(&s_device_sheets_controller)" in phase1
    assert "d1l_ui_device_sheets_hide_display(&s_device_sheets_controller)" in phase1
    assert "d1l_ui_device_sheets_hide_diagnostics(&s_device_sheets_controller)" in phase1
    assert "if (!d1l_ui_device_sheets_create(&s_device_sheets_controller, s_screen))" in phase1
