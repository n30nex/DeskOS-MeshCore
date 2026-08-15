import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def static_void_body(source: str, symbol: str) -> str:
    marker = f"static void {symbol}("
    start = source.rfind(marker)
    assert start >= 0, symbol
    end = source.find("\nstatic ", start + len(marker))
    return source[start : end if end >= 0 else len(source)]


def test_modal_module_is_registered_and_owns_visibility_helpers():
    cmake = read("main/CMakeLists.txt")
    modal_source = read("main/ui/ui_modal.c")
    modal_header = read("main/ui/ui_modal.h")

    assert '"ui/ui_modal.c"' in cmake
    assert '#include "ui_modal.h"' in modal_source
    for symbol in [
        "d1l_ui_modal_reset",
        "d1l_ui_modal_hide",
        "d1l_ui_modal_show",
        "d1l_ui_modal_visible",
        "d1l_ui_modal_has_active",
        "d1l_ui_modal_configure_scroll",
    ]:
        assert symbol in modal_header
        assert symbol in modal_source

    assert "lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN)" in modal_source
    assert "lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN)" in modal_source
    assert "lv_obj_move_foreground(obj)" in modal_source
    assert "static lv_obj_t *s_active_modal" in modal_source
    assert "lv_obj_is_valid(s_active_modal)" in modal_source

    show_body = modal_source.split("void d1l_ui_modal_show", 1)[1].split(
        "bool d1l_ui_modal_visible", 1
    )[0]
    assert "s_active_modal && s_active_modal != obj" in show_body
    assert show_body.index("lv_obj_add_flag(s_active_modal, LV_OBJ_FLAG_HIDDEN)") < show_body.index(
        "lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN)"
    )
    assert show_body.index("lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN)") < show_body.index(
        "s_active_modal = obj"
    )

    hide_body = modal_source.split("void d1l_ui_modal_hide", 1)[1].split(
        "void d1l_ui_modal_show", 1
    )[0]
    assert "if (!obj)" in hide_body
    assert "if (s_active_modal == obj)" in hide_body
    assert "s_active_modal = NULL" in hide_body

    reset_body = modal_source.split("void d1l_ui_modal_reset", 1)[1].split(
        "void d1l_ui_modal_hide", 1
    )[0]
    assert "s_active_modal = NULL" in reset_body
    active_body = modal_source.split("bool d1l_ui_modal_has_active", 1)[1].split(
        "void d1l_ui_modal_configure_scroll", 1
    )[0]
    assert "s_active_modal &&" in active_body
    assert "lv_obj_is_valid(s_active_modal)" in active_body
    assert "d1l_ui_modal_visible(s_active_modal)" in active_body
    assert active_body.index("s_active_modal &&") < active_body.index(
        "lv_obj_is_valid(s_active_modal)"
    )
    assert active_body.index("lv_obj_is_valid(s_active_modal)") < active_body.index(
        "d1l_ui_modal_visible(s_active_modal)"
    )
    assert "All modal lifecycle and query calls are UI-task-only." in modal_header
    assert "Semaphore" not in modal_source
    assert "Mutex" not in modal_source


def test_phase1_routes_sheet_visibility_through_modal_boundary():
    ui_source = read("main/ui/ui_phase1.c")

    assert '#include "ui_modal.h"' in ui_source
    assert "d1l_ui_modal_configure_scroll(sheet);" in ui_source
    assert "return d1l_ui_modal_visible(obj);" in ui_source
    assert ui_source.index("d1l_ui_modal_reset();") < ui_source.index(
        's_screen = create_screen_object("root screen")'
    )

    assert not re.search(
        r"lv_obj_add_flag\(s_(?:[a-z0-9_]+_)?sheet, LV_OBJ_FLAG_HIDDEN\)",
        ui_source,
    )
    assert not re.search(
        r"lv_obj_clear_flag\(s_(?:[a-z0-9_]+_)?sheet, LV_OBJ_FLAG_HIDDEN\)",
        ui_source,
    )
    assert not re.search(
        r"lv_obj_move_foreground\(s_(?:[a-z0-9_]+_)?sheet\)",
        ui_source,
    )

    hide_calls = re.findall(r"d1l_ui_modal_hide\(s_[a-z0-9_]+_sheet\)", ui_source)
    show_calls = re.findall(r"show_modal\(s_[a-z0-9_]+_sheet\)", ui_source)
    assert len(hide_calls) >= 20
    assert len(show_calls) >= 20
    assert ui_source.count("d1l_ui_modal_show(") == 2

    show_wrapper = ui_source.split("static void show_modal", 1)[1].split(
        "static void layout_content_for_active_tab", 1
    )[0]
    assert "set_dock_hidden(true);" in show_wrapper
    assert "d1l_ui_modal_show(obj);" in show_wrapper
    assert show_wrapper.index("set_dock_hidden(true);") < show_wrapper.index(
        "d1l_ui_modal_show(obj);"
    )
    assert "static void show_map_modal" in show_wrapper
    assert "d1l_ui_map_viewport_prepare_cover();" in show_wrapper

    restore = ui_source.split("static void restore_dock_for_active_tab", 1)[1].split(
        "static void show_modal", 1
    )[0]
    assert "d1l_ui_modal_has_active()" in restore
    assert "set_dock_hidden(true);" in restore
    assert restore.index("d1l_ui_modal_has_active()") < restore.index(
        "d1l_ui_chrome_layout_for_screen"
    )


def test_exclusive_modal_parent_returns_and_onboarding_suppression_are_explicit():
    ui_source = read("main/ui/ui_phase1.c")
    contact_source = read("main/ui/ui_contact_sheets.c")

    public_close = ui_source.split("static void close_public_search_event_cb", 1)[1].split(
        "static void public_history_load_older_event_cb", 1
    )[0]
    assert "hide_public_search_sheet();" in public_close
    assert "show_public_history_sheet();" in public_close

    public_keyboard = ui_source.split("static void public_search_keyboard_event_cb", 1)[1].split(
        "static void open_public_search_event_cb", 1
    )[0]
    assert "close_public_search_event_cb(event);" in public_keyboard

    contact_options_close = ui_source.split(
        "case D1L_UI_CONTACT_ACTION_CLOSE_OPTIONS:", 1
    )[1].split("case D1L_UI_CONTACT_ACTION_ROUTE_TRACE:", 1)[0]
    assert "hide_contact_options_sheet();" in contact_options_close
    assert "show_contact_detail_sheet();" in contact_options_close

    contact_returns = {
        "CANCEL_EDIT": "hide_contact_edit_sheet();",
        "CLOSE_EXPORT": "hide_contact_export_sheet();",
        "CANCEL_FORGET": "hide_contact_forget_sheet();",
    }
    delete_call = "d1l_app_model_delete_contact(contact->fingerprint"
    for action, hide_call in contact_returns.items():
        callback = ui_source.split(
            f"case D1L_UI_CONTACT_ACTION_{action}:", 1
        )[1].split("case D1L_UI_CONTACT_ACTION_", 1)[0]
        assert hide_call in callback, action
        assert "show_contact_options_sheet();" in callback, action
        assert delete_call not in callback, action

    save_contact = ui_source.split(
        "case D1L_UI_CONTACT_ACTION_SAVE_EDIT:", 1
    )[1].split("case D1L_UI_CONTACT_ACTION_CANCEL_EDIT:", 1)[0]
    assert "hide_contact_edit_sheet();" in save_contact
    assert "show_contact_options_sheet();" in save_contact
    assert "show_contact_detail_sheet();" not in save_contact
    assert delete_call not in save_contact

    rename_keyboard = contact_source.split("static void keyboard_event_cb", 1)[1].split(
        "static lv_obj_t *create_label", 1
    )[0]
    assert "code == LV_EVENT_CANCEL" in rename_keyboard
    assert "D1L_UI_CONTACT_ACTION_CANCEL_EDIT" in rename_keyboard
    assert "D1L_UI_CONTACT_ACTION_SAVE_EDIT" in rename_keyboard
    assert delete_call not in rename_keyboard

    forget_page = contact_source.split(
        "bool d1l_ui_contact_sheets_render_forget", 1
    )[1].split("bool d1l_ui_contact_sheets_render_export", 1)[0]
    assert forget_page.count("D1L_UI_CONTACT_ACTION_CANCEL_FORGET") == 2
    assert forget_page.count("D1L_UI_CONTACT_ACTION_CONFIRM_FORGET") == 1

    confirm_forget = ui_source.split(
        "case D1L_UI_CONTACT_ACTION_CONFIRM_FORGET:", 1
    )[1].split("case D1L_UI_CONTACT_ACTION_CLOSE_EXPORT:", 1)[0]
    assert ui_source.count(delete_call) == 1
    assert delete_call in confirm_forget

    onboarding = ui_source.split("static void update_onboarding_visibility", 1)[1].split(
        "static void set_object_hidden", 1
    )[0]
    assert "if (s_onboarding_probe_suppressed)" in onboarding
    assert "s_onboarding_probe_suppressed && object_is_visible" not in onboarding

    compose_probe = ui_source.split("static void open_compose_probe_on_ui_task", 1)[1].split(
        "static void open_keyboard_probe_on_ui_task", 1
    )[0]
    compose_after_switch = compose_probe.split("process_pending_tab_switch();", 1)[1]
    assert "s_onboarding_probe_suppressed = true;" in compose_after_switch

    keyboard_probe = ui_source.split("static void open_keyboard_probe_on_ui_task", 1)[1].split(
        "static void run_compose_probe_on_ui_task", 1
    )[0]
    keyboard_after_switch = keyboard_probe.split("process_pending_tab_switch();", 1)[1]
    assert (
        "s_onboarding_probe_suppressed = !d1l_ui_keyboard_probe_target_is_onboarding(target);"
        in keyboard_after_switch
    )

    assert "lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN)" in ui_source
    assert "lv_obj_clear_flag(s_lock_overlay, LV_OBJ_FLAG_HIDDEN)" in ui_source
