import re
from pathlib import Path

from scripts.smoke_d1l import SMOKE_COMMANDS


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def static_void_body(source: str, symbol: str) -> str:
    marker = f"static void {symbol}("
    start = source.rfind(marker)
    assert start >= 0, symbol
    end = source.find("\nstatic ", start + len(marker))
    return source[start : end if end >= 0 else len(source)]


def concrete_button_dimensions(body: str, parent: str) -> list[tuple[int, int]]:
    return [
        (int(width), int(height))
        for width, height in re.findall(
            rf"create_button\(\s*{re.escape(parent)}\s*,\s*[^,]+,\s*-?\d+\s*,\s*-?\d+\s*,\s*(\d+)\s*,\s*(\d+)\s*,",
            body,
        )
    ]


def test_mesh_inspector_is_bounded_and_read_only():
    header = read("main/mesh/mesh_inspector.h")
    source = read("main/mesh/mesh_inspector.c")
    cmake = read("main/CMakeLists.txt")
    assert "D1L_MESH_INSPECTOR_LABEL_LEN 24U" in header
    assert "D1L_REPEATER_PREVIEW_CAPACITY 8U" in header
    assert "D1L_ROOM_SERVER_PREVIEW_CAPACITY 8U" in header
    assert "d1l_mesh_signal_summary_t" in header
    assert "d1l_mesh_room_server_t" in header
    assert "d1l_mesh_repeater_candidate_t" in header
    assert "d1l_mesh_inspector_signal_summary" in header
    assert "static d1l_packet_log_entry_t s_packet_scratch[8]" in source
    assert "static d1l_route_entry_t s_route_scratch[D1L_ROUTE_STORE_CAPACITY]" in source
    assert "static d1l_node_entry_t s_node_scratch[D1L_NODE_STORE_CAPACITY]" in source
    assert "d1l_packet_log_copy_recent(s_packet_scratch, 8)" in source
    assert "d1l_route_store_copy_recent(s_route_scratch, D1L_ROUTE_STORE_CAPACITY)" in source
    assert "d1l_node_store_copy_recent(s_node_scratch, D1L_NODE_STORE_CAPACITY)" in source
    assert 'strcmp(node->type, "room") == 0' in source
    assert 'strcmp(route->target, "public") != 0' in source
    assert "nvs_" not in source
    assert '"mesh/mesh_inspector.c"' in cmake


def test_console_exposes_signal_rooms_and_repeaters():
    console = read("main/comms/usb_console.c")
    assert '#include "mesh/mesh_inspector.h"' in console
    assert 'ok_begin("signal")' in console
    assert 'ok_begin("roomservers")' in console
    assert 'ok_begin("repeaters")' in console
    assert 'strcmp(line, "signal")' in console
    assert 'strcmp(line, "roomservers")' in console
    assert 'strcmp(line, "repeaters")' in console
    assert "Signal is derived from recent packet, route, and heard-node evidence" in console
    assert "Room servers are derived from signed heard-node adverts with room role" in console
    assert "Repeater candidates are inferred from nonzero path-hop route" in console
    assert "signal" in SMOKE_COMMANDS
    assert "roomservers" in SMOKE_COMMANDS
    assert "repeaters" in SMOKE_COMMANDS
    usage = "ui scroll-probe <home|public_messages|dm_thread|nodes|contact_detail|contact_options|contact_forget|contact_route|mesh_roles|mesh_rooms|mesh_repeaters|packets|settings|storage|storage_card|storage_data|wifi|map|map_options|map_location|map_cache>"
    assert console.count(usage) == 3


def test_app_snapshot_and_ui_surface_mesh_visibility():
    header = read("main/app/app_model.h")
    source = read("main/app/app_model.c")
    ui = read("main/ui/ui_phase1.c")
    nodes_ui = read("main/ui/ui_nodes.c")
    assert "D1L_APP_SNAPSHOT_ROOM_PREVIEW D1L_ROOM_SERVER_PREVIEW_CAPACITY" in header
    assert "D1L_APP_SNAPSHOT_REPEATER_PREVIEW D1L_REPEATER_PREVIEW_CAPACITY" in header
    assert "signal_summary" in header
    assert "recent_rooms" in header
    assert "recent_repeaters" in header
    assert "d1l_mesh_inspector_signal_summary(&snapshot->signal_summary)" in source
    assert "d1l_mesh_inspector_copy_room_servers" in source
    assert "d1l_mesh_inspector_copy_repeater_candidates" in source
    assert '"Signal"' in ui
    assert '"Mesh Roles"' in ui
    assert "static lv_obj_t *s_mesh_roles_sheet" in ui
    assert "render_mesh_roles_sheet" in ui
    assert "render_room_server_role_row" in ui
    assert "render_repeater_role_row" in ui
    assert "open_mesh_roles_event_cb" in ui
    assert 'create_button(content, "Mesh Roles"' in ui
    assert "create_mesh_roles_sheet(s_screen)" in ui
    assert "D1L_MESH_ROLES_PAGE_ROOT" in ui
    assert "D1L_MESH_ROLES_PAGE_ROOM_SERVERS" in ui
    assert "D1L_MESH_ROLES_PAGE_REPEATER_CANDIDATES" in ui
    assert '"Read-only lists. Each role scrolls separately."' in ui
    assert "nodes_render_header" in nodes_ui
    assert "nodes_render_role_avatar" in nodes_ui
    assert "nodes_role_badge_text(view->role)" in nodes_ui
    assert "nodes_node_route_label(view, route, sizeof(route))" in nodes_ui
    for role_label in ('"Chat"', '"Repeater"', '"Room"', '"Sensor"', '"Node"'):
        assert role_label in nodes_ui
    assert "format_snr_tenths" in ui


def test_mesh_roles_is_full_height_hierarchical_bounded_and_read_only():
    ui = read("main/ui/ui_phase1.c")

    create_sheet = static_void_body(ui, "create_mesh_roles_sheet")
    assert "lv_obj_set_size(s_mesh_roles_sheet, 480, 424);" in create_sheet
    assert "lv_obj_set_pos(s_mesh_roles_sheet, 0, 56);" in create_sheet
    assert "lv_obj_clear_flag(s_mesh_roles_sheet, LV_OBJ_FLAG_SCROLLABLE);" in create_sheet

    header = static_void_body(ui, "render_mesh_roles_header")
    root = static_void_body(ui, "render_mesh_roles_root")
    rooms = static_void_body(ui, "render_mesh_roles_room_servers")
    repeaters = static_void_body(ui, "render_mesh_roles_repeater_candidates")
    assert 'render_mesh_roles_header("Mesh Roles", close_mesh_roles_event_cb);' in root
    assert '"Rooms"' in root
    assert '"Repeaters"' in root
    assert "s_snapshot.signal_summary.room_server_count" in root
    assert "s_snapshot.signal_summary.repeater_candidate_count" in root
    assert "open_mesh_room_servers_event_cb" in root
    assert "open_mesh_repeater_candidates_event_cb" in root
    for row_renderer in ("render_room_server_role_row", "render_repeater_role_row"):
        assert row_renderer not in root
    assert "create_mesh_roles_list" not in root

    assert 'render_mesh_roles_header("Rooms", mesh_roles_subpage_back_event_cb);' in rooms
    assert 'render_mesh_roles_header("Repeaters", mesh_roles_subpage_back_event_cb);' in repeaters
    assert "s_snapshot.signal_summary.room_server_count" in rooms
    assert "s_snapshot.signal_summary.repeater_candidate_count" in repeaters
    assert "render_room_server_role_row(list," in rooms
    assert "render_repeater_role_row(list," in repeaters
    assert 'create_label(list, "No room servers heard yet."' in rooms
    assert 'create_label(list, "No path-hop candidates yet."' in repeaters

    list_start = ui.rfind("static lv_obj_t *create_mesh_roles_list(")
    assert list_start >= 0
    list_end = ui.find("\nstatic ", list_start + 1)
    list_body = ui[list_start:list_end]
    assert "create_panel(s_mesh_roles_sheet, 16, 64, 448, 352)" in list_body
    assert "lv_obj_set_size(list, 424, 256);" in list_body
    assert "lv_obj_set_pos(list, 12, 44);" in list_body
    assert "lv_obj_set_style_pad_all(list, 0, 0);" in list_body
    assert "lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);" in list_body
    assert "lv_obj_set_scroll_dir(list, LV_DIR_VER);" in list_body
    assert "lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);" in list_body
    assert '"showing %u/%u"' in list_body
    assert '"Newest %u of %lu loaded - scroll"' in list_body
    assert '"Scroll for %u more"' in list_body
    assert '"All shown"' in list_body

    room_row = static_void_body(ui, "render_room_server_role_row")
    repeater_row = static_void_body(ui, "render_repeater_role_row")
    for row in (room_row, repeater_row):
        assert "create_panel(parent, 0, y, 424, 56)" in row
        assert "create_button" not in row
        assert "lv_obj_add_event_cb" not in row
        assert "d1l_app_model_request" not in row
        assert "d1l_meshcore" not in row

    friendly_route = ui.split(
        "static const char *friendly_repeater_route", 1
    )[1].split("\nstatic ", 1)[0]
    assert 'strcmp(route, "heard_path") == 0' in friendly_route
    assert 'return "heard path";' in friendly_route
    assert 'strcmp(route, "flood") == 0' in friendly_route
    assert 'return "flood path";' in friendly_route
    assert "strchr(route, '_')" in friendly_route
    assert 'return "route evidence";' in friendly_route
    assert "friendly_repeater_route(entry)" in repeater_row

    dimensions = (
        concrete_button_dimensions(header, "s_mesh_roles_sheet")
        + concrete_button_dimensions(root, "s_mesh_roles_sheet")
    )
    assert dimensions == [(76, 44), (448, 84), (448, 84)]
    assert all(width >= 44 and height >= 44 for width, height in dimensions)


def test_mesh_roles_back_paths_and_page_probes_are_non_destructive():
    ui = read("main/ui/ui_phase1.c")

    close_root = static_void_body(ui, "close_mesh_roles_event_cb")
    subpage_back = static_void_body(ui, "mesh_roles_subpage_back_event_cb")
    open_rooms = static_void_body(ui, "open_mesh_room_servers_event_cb")
    open_repeaters = static_void_body(ui, "open_mesh_repeater_candidates_event_cb")
    assert "hide_mesh_roles_sheet();" in close_root
    assert "s_mesh_roles_page = D1L_MESH_ROLES_PAGE_ROOT;" in subpage_back
    assert "render_mesh_roles_sheet();" in subpage_back
    assert "show_modal(s_mesh_roles_sheet);" in subpage_back
    assert "s_mesh_roles_page = D1L_MESH_ROLES_PAGE_ROOM_SERVERS;" in open_rooms
    assert "s_mesh_roles_page = D1L_MESH_ROLES_PAGE_REPEATER_CANDIDATES;" in open_repeaters

    probe_start = ui.index("static lv_obj_t *scroll_probe_open_mesh_surface")
    probe_end = ui.index("\nstatic void measure_scroll_probe_target", probe_start)
    probe = ui[probe_start:probe_end]
    for surface in ("mesh_roles", "mesh_rooms", "mesh_repeaters"):
        assert f'"{surface}"' in probe
    assert "open_mesh_roles_event_cb(NULL);" in probe
    assert "open_mesh_room_servers_event_cb(NULL);" in probe
    assert "open_mesh_repeater_candidates_event_cb(NULL);" in probe
    for forbidden in (
        "d1l_app_model_request",
        "d1l_meshcore",
        "route_path_probe_event_cb",
        "confirm_forget_contact_event_cb",
    ):
        assert forbidden not in probe

    page_classifier_start = ui.index("static void run_scroll_probe_on_ui_task")
    page_classifier_end = ui.index("\nstatic uint32_t begin_pending_scroll_probe", page_classifier_start)
    page_classifier = ui[page_classifier_start:page_classifier_end]
    for surface in ("mesh_roles", "mesh_rooms", "mesh_repeaters"):
        assert f'strcmp(canonical, "{surface}") == 0' in page_classifier
    assert "const bool static_page" in page_classifier
    assert "result->target_found = target != NULL;" in page_classifier
    assert page_classifier.index("force_ui_layout_repaint();") < page_classifier.index("if (static_page)")
    assert "return;" in page_classifier
    assert "result->ok = result->surface_supported && result->target_found;" in page_classifier

    assert "D1L_UI_SCROLL_PROBE_TIMEOUT_MS = 5000U" in ui
