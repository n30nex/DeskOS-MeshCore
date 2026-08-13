from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_nodes_root_has_owned_bounded_view_model_and_action_lifetime():
    cmake = read("main/CMakeLists.txt")
    phase1 = read("main/ui/ui_phase1.c")
    header = read("main/ui/ui_nodes.h")
    source = read("main/ui/ui_nodes.c")

    assert '"ui/ui_nodes.c"' in cmake
    assert '"ui/ui_nodes_summary.c"' in cmake
    assert '#include "ui_nodes.h"' in phase1
    assert "d1l_ui_nodes_view_model_t" in header
    assert "d1l_ui_nodes_action_event_t" in header
    assert "d1l_ui_nodes_controller_t" in header
    assert "d1l_ui_nodes_render" in header
    assert "d1l_ui_nodes_deactivate" in header
    assert "d1l_contact_entry_t contact_rows[D1L_CONTACT_STORE_CAPACITY]" in header
    assert "d1l_node_view_t node_rows[D1L_NODE_STORE_CAPACITY]" in header
    assert "bool contact_can_dm[D1L_CONTACT_STORE_CAPACITY]" in header
    assert "bool node_can_dm[D1L_NODE_STORE_CAPACITY]" in header
    assert "d1l_ui_node_role_counts_t role_counts" in header

    assert "view_model != &controller->rendered" in source
    assert "controller->rendered = *view_model;" in source
    assert "controller->rendered.contact_row_count >" in source
    assert "D1L_CONTACT_STORE_CAPACITY" in source
    assert "node_row_count > D1L_NODE_STORE_CAPACITY" in source
    assert "binding->row_index >= controller->rendered.contact_row_count" in source
    assert "binding->row_index >= controller->rendered.node_row_count" in source
    assert "!controller->rendered.contact_can_dm[binding->row_index]" in source
    assert "!controller->rendered.node_can_dm[binding->row_index]" in source
    assert "&controller->rendered.contact_rows[binding->row_index]" in source
    assert "&controller->rendered.node_rows[binding->row_index]" in source
    assert "memset(&controller->rendered, 0, sizeof(controller->rendered));" in source
    assert "controller->action_handler = NULL;" in source

    assert "nodes_view_model_from_snapshot(snapshot, &s_nodes_controller.rendered);" in phase1
    assert "snapshot->recent_contact_count" in phase1
    assert "D1L_CONTACT_STORE_CAPACITY" in phase1
    assert "d1l_app_model_query_nodes(" in phase1
    assert "D1L_NODE_STORE_CAPACITY" in phase1
    assert "d1l_ui_nodes_summarize_roles(" in phase1
    assert "view_model->node_count" not in phase1
    assert "contact_can_dm(&view_model->contact_rows[i])" in phase1
    assert "node_view_can_dm(&view_model->node_rows[i])" in phase1
    assert "handle_nodes_action" in phase1
    assert "show_contact_detail_for(event->contact);" in phase1
    assert "open_dm_compose_for_contact(event->contact);" in phase1
    assert "show_node_detail_view(event->node, false);" in phase1
    assert "open_node_dm_for(event->node);" in phase1
    assert "d1l_ui_nodes_deactivate(&s_nodes_controller);" in phase1
    assert "static d1l_ui_nodes_controller_t s_nodes_controller EXT_RAM_BSS_ATTR;" in phase1
    assert "static d1l_node_view_t s_map_node_rows[D1L_NODE_STORE_CAPACITY] " in phase1


def test_nodes_root_module_matches_mobile_contacts_sections_and_boundaries():
    source = read("main/ui/ui_nodes.c")

    for label in (
        '"Contacts"',
        '"Find"',
        '"Clear"',
        '"Search contacts"',
        '"Sort: Recent"',
        '"Saved contacts"',
        '"Nearby"',
        '"No contacts yet"',
        '"Find nearby"',
        '"No saved contacts yet"',
        '"No other nearby nodes"',
        '"Manage"',
        '"Chat"',
        '"Repeater"',
        '"Room"',
        '"Sensor"',
        '"Node"',
    ):
        assert label in source
    assert "nodes_render_header" in source
    assert "nodes_render_contact_row" in source
    assert "nodes_render_node_row" in source
    assert "nodes_render_role_avatar" in source
    assert "nodes_node_matches_contact" in source
    assert "nodes_nearby_count" in source
    assert "nodes_render_empty_state" in source
    assert "static const char *nodes_role_badge_text" in source
    assert "static uint32_t nodes_role_color" in source
    assert "NODES_ROW_HEIGHT 58" in source
    assert "NODES_MIN_TOUCH_HEIGHT 44" in source
    assert 'nodes_create_button(row, "Chat", 340, 7, 84, 44' in source
    assert "entry->public_key_hex" not in source
    assert '"Fingerprint' not in source
    contact_row = source.split("static void nodes_render_contact_row", 1)[1].split(
        "static void nodes_render_node_row", 1
    )[0]
    node_row = source.split("static void nodes_render_node_row", 1)[1].split(
        "static void nodes_render_empty_state", 1
    )[0]
    for primary_row in (contact_row, node_row):
        assert "dBm" not in primary_row
        assert "rssi" not in primary_row.lower()
        assert "snr" not in primary_row.lower()
    assert "%.8s" not in source
    assert "Scroll proof" not in source
    assert "D1L_UI_NODES_ACTION_OPEN_CONTACT_DM" in source
    assert "D1L_UI_NODES_ACTION_OPEN_NODE_DM" in source
    assert "D1L_UI_NODES_ACTION_OPEN_SEARCH" in source
    assert "D1L_UI_NODES_ACTION_CYCLE_SORT" in source

    assert "static lv_obj_t *s_" not in source
    assert "lv_timer_" not in source
    assert "d1l_app_model_" not in source
    assert "d1l_meshcore_service_" not in source
    assert "d1l_storage_" not in source
    assert "freertos/" not in source


def test_nodes_summary_counts_only_exact_canonical_role_labels():
    summary = read("main/ui/ui_nodes_summary.c")

    for role in ("companion", "chat", "repeater", "room", "sensor"):
        assert f'role_equals(role, "{role}")' in summary
    assert "roles[i]" in summary
    assert "node_roles[i] = view_model->node_rows[i].role;" in read("main/ui/ui_phase1.c")
    assert "path_hops" not in summary
    assert "rssi" not in summary
    assert "snr" not in summary
    assert "++out_counts->unknown;" in summary
    assert "++out_counts->total;" in summary
