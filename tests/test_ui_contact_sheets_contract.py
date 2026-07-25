from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    assert start in source, start
    assert end in source, end
    return source.split(start, 1)[1].split(end, 1)[0]


def test_contact_hierarchy_has_one_bounded_persistent_owner():
    cmake = read("main/CMakeLists.txt")
    header = read("main/ui/ui_contact_sheets.h")
    source = read("main/ui/ui_contact_sheets.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert '"ui/ui_contact_sheets.c"' in cmake
    assert '#include "ui_contact_sheets.h"' in phase1
    assert "D1L_UI_CONTACT_SHEETS_CONTROLLER_MAX_BYTES 1536U" in header
    assert "D1L_UI_CONTACT_SHEETS_BINDING_COUNT 17U" in header
    assert "_Static_assert(sizeof(d1l_ui_contact_sheets_controller_t)" in source
    assert "s_contact_sheets_controller EXT_RAM_BSS_ATTR" in phase1
    for raw_owner in (
        "static lv_obj_t *s_contact_detail_sheet",
        "static lv_obj_t *s_contact_options_sheet",
        "static lv_obj_t *s_contact_forget_sheet",
        "static lv_obj_t *s_contact_edit_sheet",
        "static lv_obj_t *s_contact_export_sheet",
        "static d1l_contact_entry_t s_contact_detail_contact",
        "static d1l_contact_entry_t s_contact_export_contact",
    ):
        assert raw_owner not in phase1


def test_contact_create_is_atomic_and_edit_sheet_remains_lazy():
    source = read("main/ui/ui_contact_sheets.c")
    create = body(
        source,
        "bool d1l_ui_contact_sheets_create",
        "bool d1l_ui_contact_sheets_set_contact",
    )
    destroy = body(source, "static void destroy_sheets", "static lv_obj_t *create_sheet")

    assert create.index("destroy_sheets(controller);") < create.index(
        "controller->parent = parent;"
    )
    for sheet in ("detail_sheet", "options_sheet", "forget_sheet", "export_sheet"):
        assert f"controller->{sheet} = create_sheet(parent);" in create
    assert "controller->edit_sheet = create_sheet(parent);" not in create
    assert create.count("destroy_sheets(controller);") >= 5
    assert "detach_edit_keyboard(controller, true);" in destroy
    for sheet in (
        "detail_sheet",
        "options_sheet",
        "forget_sheet",
        "edit_sheet",
        "export_sheet",
    ):
        assert f"delete_sheet(&controller->{sheet});" in destroy
    assert "memset(controller, 0, sizeof(*controller));" in destroy


def test_contact_callbacks_reject_stale_generation_and_keep_app_actions_in_phase1():
    header = read("main/ui/ui_contact_sheets.h")
    source = read("main/ui/ui_contact_sheets.c")
    phase1 = read("main/ui/ui_phase1.c")

    for action in (
        "CLOSE_DETAIL",
        "MESSAGE",
        "OPEN_OPTIONS",
        "CLOSE_OPTIONS",
        "ROUTE_TRACE",
        "RENAME",
        "TOGGLE_FAVORITE",
        "TOGGLE_MUTE",
        "EXPORT",
        "OPEN_FORGET",
        "CANCEL_FORGET",
        "CONFIRM_FORGET",
        "CLOSE_EXPORT",
        "SAVE_EDIT",
        "CANCEL_EDIT",
    ):
        assert f"D1L_UI_CONTACT_ACTION_{action}" in header
    assert "binding->generation == binding->controller->generation" in source
    assert "binding->generation != 0U" in source
    assert "controller->generation++" in source
    assert "controller->generation == 0U" in source
    assert "controller->action_handler = NULL" in source
    assert "memset(controller->bindings, 0" in source
    assert "d1l_app_model_" not in source
    assert "d1l_contact_store_" not in source
    for action in (
        "set_contact_flags",
        "rename_contact",
        "delete_contact",
        "export_contact_uri",
    ):
        assert f"d1l_app_model_{action}" in phase1


def test_edit_keyboard_is_detached_before_clean_or_delete():
    source = read("main/ui/ui_contact_sheets.c")
    detach = body(source, "static void detach_edit_keyboard", "static void delete_sheet")
    destroy = body(source, "static void destroy_sheets", "static lv_obj_t *create_sheet")
    invalidate = body(source, "static void invalidate_render", "static bool finish_render")
    render_edit = body(
        source,
        "bool d1l_ui_contact_sheets_render_edit",
        "static void hide_sheet",
    )

    assert "lv_keyboard_set_textarea(controller->edit_keyboard, NULL);" in detach
    assert destroy.index("detach_edit_keyboard(controller, true);") < destroy.index(
        "delete_sheet(&controller->edit_sheet);"
    )
    assert invalidate.index("detach_edit_keyboard(controller, false);") < invalidate.index(
        "lv_obj_clean(sheet);"
    )
    assert render_edit.index("detach_edit_keyboard(controller, false);") < render_edit.index(
        "begin_render(controller, controller->edit_sheet"
    )
    assert "LV_EVENT_READY" in render_edit
    assert "LV_EVENT_CANCEL" in render_edit


def test_all_contact_renderers_fail_closed_on_partial_construction():
    source = read("main/ui/ui_contact_sheets.c")
    for renderer, next_marker in (
        ("detail", "bool d1l_ui_contact_sheets_render_options"),
        ("options", "bool d1l_ui_contact_sheets_render_forget"),
        ("forget", "bool d1l_ui_contact_sheets_render_export"),
        ("export", "bool d1l_ui_contact_sheets_render_edit"),
        ("edit", "static void hide_sheet"),
    ):
        render = body(
            source,
            f"bool d1l_ui_contact_sheets_render_{renderer}",
            next_marker,
        )
        assert "finish_render(controller, sheet, complete)" in render
    invalidate = body(source, "static void invalidate_render", "static bool finish_render")
    assert "deactivate_actions(controller);" in invalidate
    assert invalidate.index("d1l_ui_modal_hide(sheet);") < invalidate.index(
        "lv_obj_clean(sheet);"
    )


def test_contact_progressive_disclosure_layout_and_truth_are_preserved():
    source = read("main/ui/ui_contact_sheets.c")
    detail = body(
        source,
        "bool d1l_ui_contact_sheets_render_detail",
        "bool d1l_ui_contact_sheets_render_options",
    )
    options = body(
        source,
        "bool d1l_ui_contact_sheets_render_options",
        "bool d1l_ui_contact_sheets_render_forget",
    )

    for label in (
        "Back",
        "Message",
        "Contact options",
    ):
        assert f'"{label}"' in detail
    assert '"DM unavailable [%s]"' in detail
    assert "d1l_ui_dm_identity_reason_code(" in detail
    assert "d1l_ui_dm_identity_reason_text(" in detail
    assert "controller->rendered.dm_identity_reason" in detail
    for hidden in ("Route trace", "Rename", "Export QR", "Forget contact"):
        assert f'"{hidden}"' not in detail
    for label in (
        "Route trace",
        "Rename",
        "Remove from favorites",
        "Add to favorites",
        "Include in unread count",
        "Exclude from unread count",
        "Export QR",
        "Export unavailable",
        "Forget contact",
    ):
        assert f'"{label}"' in options
    for label in (
        "This removes the saved contact and its routing preferences.",
        "Message history remains on this device.",
        "Alias only; retained history remains",
        "No retained public key for QR export",
        "Scan with a MeshCore client or copy from serial",
    ):
        assert f'"{label}"' in source
    assert "lv_obj_set_size(sheet, 480, 424);" in source
    assert "lv_obj_set_pos(sheet, 0, 56);" in source
    assert "d1l_ui_keyboard_configure_input(" in source
    assert "16, 148, 448, 200" in source
