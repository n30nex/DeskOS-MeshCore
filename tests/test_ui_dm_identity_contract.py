from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_dm_identity_helper_is_built_and_exposes_stable_reasons() -> None:
    cmake = read("main/CMakeLists.txt")
    header = read("main/ui/ui_dm_identity.h")
    source = read("main/ui/ui_dm_identity.c")

    assert '"ui/ui_dm_identity.c"' in cmake
    for reason in (
        "sender_name_unverified",
        "identity_incomplete",
        "heard_only",
        "contact_missing",
        "contact_not_canonical",
        "identity_mismatch",
        "role_not_dm_capable",
    ):
        assert f'"{reason}"' in source
    assert "D1L_UI_DM_IDENTITY_SOURCE_PUBLIC_SENDER_LABEL" in header
    assert "d1l_contact_store_is_canonical(contact)" in source
    assert "d1l_contact_store_can_dm(contact)" in source
    public_gate = source.split(
        "if (input->source == D1L_UI_DM_IDENTITY_SOURCE_PUBLIC_SENDER_LABEL)", 1
    )[1].split("const bool source_identity_complete", 1)[0]
    assert "D1L_UI_DM_IDENTITY_SENDER_NAME_UNVERIFIED" in public_gate
    assert "resolved_contact" not in public_gate


def test_production_paths_use_full_key_resolution_and_never_alias_match_public_sender() -> None:
    app_header = read("main/app/app_model.h")
    app_source = read("main/app/app_model.c")
    ui = read("main/ui/ui_phase1.c")

    assert "d1l_app_model_find_contact_by_public_key" in app_header
    exact_lookup = app_source.split(
        "esp_err_t d1l_app_model_find_contact_by_public_key", 1
    )[1].split("esp_err_t d1l_app_model_set_contact_flags", 1)[0]
    assert "d1l_contact_store_find_by_public_key(public_key_hex, out_contact)" in exact_lookup

    contact_projection = ui.split(
        "static d1l_ui_dm_identity_eligibility_t dm_identity_for_contact", 1
    )[1].split("static d1l_ui_dm_identity_eligibility_t dm_identity_for_node", 1)[0]
    node_projection = ui.split(
        "static d1l_ui_dm_identity_eligibility_t dm_identity_for_node", 1
    )[1].split("static void show_dm_identity_reason", 1)[0]
    assert "d1l_app_model_find_contact_by_public_key" in contact_projection
    assert "d1l_app_model_find_contact_by_public_key" in node_projection
    assert "find_contact(" not in contact_projection
    assert "find_contact(" not in node_projection

    public_action = ui.split(
        "static void explain_public_sender_dm_event_cb", 1
    )[1].split("static lv_obj_t *create_nested_page_body", 1)[0]
    assert "show_dm_identity_reason(public_sender_dm_eligibility())" in public_action
    assert "open_dm_compose_for_contact" not in public_action
    assert "d1l_app_model_send_dm_text" not in public_action


def test_invalid_identity_taps_explain_without_compose_or_rf_and_controls_are_touch_sized() -> None:
    ui = read("main/ui/ui_phase1.c")
    node_detail = read("main/ui/ui_node_detail.c")

    node_reason_action = ui.split(
        "static void handle_node_detail_action", 1
    )[1].split("static void show_node_detail_view", 1)[0]
    assert "show_dm_identity_reason" in node_reason_action
    assert "open_dm_compose_for_contact" not in node_reason_action
    assert "d1l_app_model_send_dm_text" not in node_reason_action

    render_node = node_detail.split("bool d1l_ui_node_detail_render", 1)[1].split(
        "void d1l_ui_node_detail_deactivate", 1
    )[0]
    assert '"Why no DM?", 174, 0, 120, 44' in render_node
    assert '"DM", 208, 0, 54, 44' in render_node
    assert '"Close", 316, 0, 76, 44' in render_node
    assert '"Admin", 8, 352, 104, 44' in render_node
    assert '"Verified server; local authenticated login required."' in render_node

    render_message = ui.split("static void render_message_detail_sheet(void)", 1)[1].split(
        "static void show_message_detail_for", 1
    )[0]
    assert '"Reply", 16, 360, 216, 52' in render_message
    assert '"DM sender", 248, 360, 216, 52' in render_message
    assert '"Private message unavailable: %s"' in render_message


def test_node_detail_geometry_contains_all_reason_and_management_copy() -> None:
    node_detail = read("main/ui/ui_node_detail.c")
    create = node_detail.split("bool d1l_ui_node_detail_create", 1)[1].split(
        "bool d1l_ui_node_detail_render", 1
    )[0]
    render = node_detail.split("bool d1l_ui_node_detail_render", 1)[1].split(
        "void d1l_ui_node_detail_deactivate", 1
    )[0]

    assert "lv_obj_set_size(controller->sheet, 480, 480);" in create
    assert "lv_obj_set_pos(controller->sheet, 0, 0);" in create
    assert "lv_obj_set_style_pad_all(controller->sheet, 12, 0);" in create
    assert "lv_obj_set_pos(managed_reason, 120, 360);" in render
    assert "lv_obj_set_size(managed_reason, 276, 40);" in render
    # 12 px top padding + local y 360 + h 40 stays inside 480 px.
    assert 12 + 360 + 40 <= 480


def test_direct_contact_and_node_dm_shortcuts_have_unclipped_44px_targets() -> None:
    nodes = read("main/ui/ui_nodes.c")
    contact_row = nodes.split("static void nodes_render_contact_row", 1)[1].split(
        "static void nodes_render_node_row", 1
    )[0]
    node_row = nodes.split("static void nodes_render_node_row", 1)[1].split(
        "void d1l_ui_nodes_render", 1
    )[0]

    assert "NODES_ROW_WIDTH 448" in nodes
    assert "NODES_ROW_HEIGHT 58" in nodes
    assert "parent, NODES_ROW_X, y, NODES_ROW_WIDTH, NODES_ROW_HEIGHT" in contact_row
    assert 'nodes_create_button(row, "Chat", 340, 7, 84, 44' in contact_row
    assert 7 + 44 <= 58

    assert "parent, NODES_ROW_X, y, NODES_ROW_WIDTH, NODES_ROW_HEIGHT" in node_row
    assert 'nodes_create_button(row, "Chat", 340, 7, 84, 44' in node_row
    assert 7 + 44 <= 58


def test_contact_sheet_carries_exact_identity_reason_instead_of_generic_role_copy() -> None:
    header = read("main/ui/ui_contact_sheets.h")
    contact_ui = read("main/ui/ui_contact_sheets.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "d1l_ui_dm_identity_reason_t dm_identity_reason" in header
    assert "d1l_ui_dm_identity_reason_t dm_identity_reason," in header
    setter = contact_ui.split("bool d1l_ui_contact_sheets_set_contact", 1)[1].split(
        "bool d1l_ui_contact_sheets_set_export_uri", 1
    )[0]
    assert "controller->rendered.dm_identity_reason = dm_identity_reason" in setter
    assert "dm_identity_reason == D1L_UI_DM_IDENTITY_READY" in setter

    detail = contact_ui.split("bool d1l_ui_contact_sheets_render_detail", 1)[1].split(
        "bool d1l_ui_contact_sheets_render_options", 1
    )[0]
    assert '"Messaging unavailable"' in detail
    assert "d1l_ui_dm_identity_reason_code(" not in detail
    assert "d1l_ui_dm_identity_reason_text(" in detail
    assert "Messaging unavailable for this role" not in detail
    selected = phase1.split("static bool set_selected_contact", 1)[1].split(
        "static void contact_sheets_action_handler", 1
    )[0]
    assert "dm_identity_for_contact(contact, NULL)" in selected
    assert "eligibility.reason" in selected


def test_user_guide_documents_sender_identity_and_usb_import_boundary() -> None:
    guide = read("docs/USER_GUIDE_D1L.md")
    parity = read("docs/DESKOS_MESHCORE_FEATURE_PARITY.md")

    assert "sender_name_unverified" in guide
    assert "never alias-matches" in guide
    assert "complete public key" in guide
    assert "remain read-only" in guide
    assert "Contacts can be imported from the USB console" in guide
    assert "`contacts import <meshcore-uri>`" in guide
    assert "Export QR" in guide
    assert "temporary payload after rendering" in guide
    assert "privacy-safe contact/channel QR" in parity
