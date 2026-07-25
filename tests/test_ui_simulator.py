import json
from dataclasses import replace
from pathlib import Path

from PIL import Image

from tools import ui_simulator


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def boxes_overlap(left: list[int], right: list[int]) -> bool:
    return min(left[2], right[2]) > max(left[0], right[0]) and min(left[3], right[3]) > max(left[1], right[1])


def assert_pairwise_disjoint(boxes: list[list[int]]) -> None:
    for index, left in enumerate(boxes):
        for right in boxes[index + 1 :]:
            assert not boxes_overlap(left, right), (left, right)


def assert_box_inside(inner: list[int], outer: list[int]) -> None:
    assert outer[0] <= inner[0] < inner[2] <= outer[2], (inner, outer)
    assert outer[1] <= inner[1] < inner[3] <= outer[3], (inner, outer)


def test_ui_simulator_generates_checked_480x480_screens(tmp_path):
    report = ui_simulator.generate(tmp_path)

    assert report["ok"] is True
    assert report["scenario"] == "default"
    assert report["display"] == {"width": 480, "height": 480}
    assert report["snapshot_counts"]["heard"] == 3
    assert report["overflow_count"] == 0
    assert report["touch_target_issue_count"] == 0
    assert report["dock_invariant_issues"] == []
    assert report["required_labels_missing"] == []
    assert report["flow_report"]["ok"] is True
    assert report["flow_report"]["target_overlaps"] == []
    assert report["flow_report"]["format_actions"] == []

    report_path = tmp_path / "ui-sim-report.json"
    saved_report = json.loads(report_path.read_text(encoding="utf-8"))
    assert saved_report["ok"] is True
    assert saved_report["schema"] == 2

    views = {view["name"]: view for view in report["views"]}
    assert set(views) == set(ui_simulator.RENDERERS)
    assert len(views) == 87
    for name, view in views.items():
        image_path = Path(view["screenshot"])
        assert image_path.exists(), name
        with Image.open(image_path) as image:
            assert image.size == (480, 480), name
        assert view["text_count"] > 0
        assert view["overflow"] == []
        assert view["touch_target_issues"] == []
        assert view["dock_expected"] is (name in ui_simulator.DOCKED_VIEWS)
        assert view["dock_rendered"] is (name in ui_simulator.DOCKED_VIEWS)
        assert view["dock_target_count"] == (5 if name in ui_simulator.DOCKED_VIEWS else 0)
        assert view["dock_invariant_ok"] is True
        if name in ui_simulator.CONTACT_HIERARCHY_VIEWS:
            assert view["truncated_labels"] == [], name
        if name in ui_simulator.MESH_ROLE_VIEWS:
            assert view["truncated_labels"] == [], name
        if name in ui_simulator.STORAGE_HIERARCHY_VIEWS:
            assert view["truncated_labels"] == [], name
            assert view["sibling_text_overlaps"] == [], name
        if name in ui_simulator.MAP_HIERARCHY_VIEWS:
            assert view["truncated_labels"] == [], name
            assert view["sibling_text_overlaps"] == [], name


def test_ui_simulator_large_mesh_stress_is_bounded(tmp_path):
    report = ui_simulator.generate(tmp_path, scenario="large-mesh")

    assert report["ok"] is True
    assert report["scenario"] == "large-mesh"
    assert report["overflow_count"] == 0
    assert report["touch_target_issue_count"] == 0
    assert report["flow_report"]["ok"] is True
    assert report["incoming_event_report"]["ok"] is True
    assert report["required_labels_missing"] == []
    assert report["snapshot_counts"]["heard"] == 96
    assert report["snapshot_counts"]["public_messages"] == 48
    assert report["snapshot_counts"]["dm_messages"] == 32

    views = {view["name"]: view for view in report["views"]}
    for name in ui_simulator.CONTACT_HIERARCHY_VIEWS:
        assert views[name]["truncated_labels"] == [], name
    messages = views["messages"]["metrics"]
    assert messages["messages_mode"] == "root"
    assert messages["public_source_count"] == 48
    assert messages["public_rendered_count"] == 0
    assert messages["dm_source_count"] == 32
    assert messages["dm_conversation_count"] == 32
    assert messages["dm_rendered_count"] == 0
    messages_public = views["messages_public"]["metrics"]
    assert messages_public["messages_mode"] == "public"
    assert messages_public["public_source_count"] == 48
    assert messages_public["public_rendered_count"] <= 2
    assert messages_public["public_sticky_compose"] is True
    assert messages_public["public_directional_bubbles"] is True
    assert messages_public["public_navigation_rf_silent"] is True
    messages_dm = views["messages_dm"]["metrics"]
    assert messages_dm["messages_mode"] == "dms"
    assert messages_dm["dm_source_count"] == 32
    assert messages_dm["dm_conversation_count"] == 32
    assert messages_dm["dm_rendered_count"] <= 5
    for view_name in (
        "compose_incoming_public_refresh",
        "public_search_incoming_refresh",
        "dm_thread_incoming_refresh",
        "dm_search_incoming_refresh",
    ):
        metrics = views[view_name]["metrics"]
        assert metrics["incoming_event_content_refreshed"] is True
        assert metrics["incoming_event_mode_preserved"] is True
        assert metrics["incoming_event_overlay_preserved"] is True
        assert metrics["incoming_event_keyboard_focus_preserved"] is True
        assert metrics["incoming_event_transmitted_rf"] is False
        assert metrics["incoming_event_advanced_public_cursor"] is False
        assert metrics["incoming_event_advanced_dm_cursor"] is False
    nodes = views["nodes"]["metrics"]
    assert nodes["contacts_source_count"] == 18
    assert nodes["contacts_rendered_count"] <= 2
    assert nodes["heard_source_count"] == 96
    assert nodes["heard_query_count"] == 64
    assert nodes["node_role_query_capacity"] == 64
    assert nodes["heard_rendered_count"] <= 4

    packets = views["packets"]["metrics"]
    assert packets["packet_source_count"] == 128
    assert packets["packet_query_limit"] == 100
    assert packets["packet_total_matches"] == 128
    assert packets["packet_sd_history_page"] is True
    assert packets["packet_load_older_available"] is True
    assert "Load Older" in set(views["packets"]["labels"])

    dm_thread = views["dm_thread_sheet"]["metrics"]
    assert dm_thread["dm_thread_source_count"] == 1
    assert dm_thread["dm_thread_rendered_count"] <= 3
    assert dm_thread["dm_thread_page_limit"] == 5
    assert dm_thread["dm_thread_load_older_available"] is False
    assert dm_thread["dm_thread_count_line"] == "1 of 1 messages"
    assert dm_thread["dm_thread_marks_read_on_open"] is True
    assert dm_thread["dm_thread_sticky_reply"] is True
    assert "Load Older" not in set(views["dm_thread_sheet"]["labels"])
    dm_thread_details = views["dm_thread_details_sheet"]["metrics"]
    assert dm_thread_details["dm_thread_details_expanded"] is True
    assert dm_thread_details["dm_thread_details_single_row"] is True
    assert dm_thread_details["dm_thread_details_rendered_lines"] > 0
    assert dm_thread_details["dm_thread_sticky_reply"] is True
    assert dm_thread_details["dm_thread_navigation_rf_silent"] is True
    dm_search_results = views["dm_thread_search_results"]["metrics"]
    assert dm_search_results["dm_thread_search_active"] is True
    assert dm_search_results["dm_thread_total_matches"] > 0
    assert dm_search_results["dm_thread_search_read_only"] is True
    dm_search_empty = views["dm_thread_search_no_match"]["metrics"]
    assert dm_search_empty["dm_thread_search_no_match"] is True
    assert dm_search_empty["dm_thread_empty_history"] is False
    dm_history_empty = views["dm_thread_empty_sheet"]["metrics"]
    assert dm_history_empty["dm_thread_empty_history"] is True
    assert dm_history_empty["dm_thread_search_no_match"] is False
    trace = views["route_trace_sheet"]["metrics"]
    assert trace["route_trace_rendered_count"] <= 2
    assert trace["route_trace_real_packet"] is True
    assert trace["route_trace_requires_explicit_touch"] is True
    assert trace["route_trace_current_boot_proven_path_required"] is True
    assert trace["route_trace_one_byte_hash_only"] is False
    assert trace["route_trace_contact_path_hash_bytes_supported"] == [1, 2]
    assert trace["route_trace_wire_hash_bytes_supported"] == [1, 2, 4, 8]
    assert trace["route_trace_flags_supported"] == [0, 1, 2, 3]
    assert trace["route_trace_contact_route_hash_bytes_rejected"] == [3]
    assert trace["route_trace_hardware_verified"] is False
    assert trace["route_trace_public_rf_tx"] is False

    mesh_root = views["mesh_roles_sheet"]["metrics"]
    assert mesh_root["mesh_roles_rooms_source_count"] == 6
    assert mesh_root["mesh_roles_repeaters_source_count"] == 12
    mesh_rooms = views["mesh_rooms_page"]["metrics"]
    assert mesh_rooms["mesh_rooms_source_count"] == 6
    assert mesh_rooms["mesh_rooms_loaded_count"] == 6
    assert mesh_rooms["mesh_rooms_rendered_count"] == 4
    assert mesh_rooms["mesh_rooms_scrollable"] is True
    mesh_repeaters = views["mesh_repeaters_page"]["metrics"]
    assert mesh_repeaters["mesh_repeaters_source_count"] == 12
    assert mesh_repeaters["mesh_repeaters_loaded_count"] == 8
    assert mesh_repeaters["mesh_repeaters_rendered_count"] == 4
    assert mesh_repeaters["mesh_repeaters_scrollable"] is True
    for page_name in ("mesh_rooms_page", "mesh_repeaters_page"):
        page = views[page_name]
        assert page["truncated_labels"] == []
        px0, py0, px1, py1 = page["metrics"]["mesh_roles_list_panel"]
        row_boxes = page["metrics"]["mesh_roles_row_boxes"]
        assert len(row_boxes) == 4
        for x0, y0, x1, y1 in row_boxes:
            assert px0 <= x0 < x1 <= px1
            assert py0 <= y0 < y1 <= py1
        assert all(left[3] <= right[1] for left, right in zip(row_boxes, row_boxes[1:]))

    public_history = views["public_history_sheet"]["metrics"]
    assert public_history["public_history_source_count"] == 48
    assert public_history["public_history_rendered_count"] <= 3
    assert public_history["public_history_page_limit"] == 5
    assert public_history["public_history_load_older_available"] is True
    assert "Load Older" in set(views["public_history_sheet"]["labels"])
    assert "Sent over RF" in public_history["public_history_rendered_states"]


def test_nodes_role_summary_uses_exact_render_query_roles(tmp_path):
    expected_by_scenario = {
        "default": {
            "chat_companion": 1,
            "repeater": 1,
            "room_server": 1,
            "sensor": 0,
            "unknown": 0,
        },
        "nodes-empty": {
            "chat_companion": 0,
            "repeater": 0,
            "room_server": 0,
            "sensor": 0,
            "unknown": 0,
        },
        "nodes-mixed-roles": {
            "chat_companion": 1,
            "repeater": 1,
            "room_server": 1,
            "sensor": 1,
            "unknown": 1,
        },
        "large-mesh": {
            "chat_companion": 38,
            "repeater": 13,
            "room_server": 13,
            "sensor": 0,
            "unknown": 0,
        },
    }

    for scenario, expected in expected_by_scenario.items():
        report = ui_simulator.generate(
            tmp_path / scenario,
            views=("nodes",),
            scenario=scenario,
        )
        assert report["ok"] is True, scenario
        nodes = report["views"][0]
        metrics = nodes["metrics"]
        assert metrics["node_role_counts"] == expected, scenario
        assert metrics["node_role_count_sum"] == metrics["heard_query_count"], scenario
        assert metrics["node_role_counts_match_query"] is True, scenario
        assert metrics["node_role_source"] == "exact_render_query_role", scenario
        assert metrics["nodes_navigation_rf_silent"] is True, scenario
        assert metrics["nodes_formats_sd"] is False, scenario
        assert metrics["nodes_destructive_actions"] == 0, scenario
        assert nodes["touch_target_issues"] == [], scenario
        assert all(target["rf_tx"] is False for target in nodes["touch_targets"]), scenario
        assert all(target["formats_sd"] is False for target in nodes["touch_targets"]), scenario
        assert all(target["destructive"] is False for target in nodes["touch_targets"]), scenario


def test_node_role_counts_reject_noncanonical_case_spacing_and_aliases() -> None:
    nodes = tuple(
        ui_simulator.Node(f"Node {index}", f"{index:016X}", role, "", "")
        for index, role in enumerate(("Repeater", " repeater", "room server", "ROOM", ""))
    )
    assert ui_simulator.node_role_counts(nodes) == {
        "chat_companion": 0,
        "repeater": 0,
        "room_server": 0,
        "sensor": 0,
        "unknown": 5,
    }


def test_dm_simulator_projects_one_latest_row_per_conversation() -> None:
    snapshot = ui_simulator.sample_snapshot()
    summaries = ui_simulator.dm_conversation_summaries(snapshot.dm_messages)

    assert len(snapshot.dm_messages) == 2
    assert len(summaries) == 1
    assert summaries[0].source == "YKF Corebot"
    assert summaries[0].direction == "tx"
    assert summaries[0].unread is True

    surface = ui_simulator.Surface("messages_dm")
    ui_simulator.render_messages_dm(surface, snapshot)
    assert surface.metrics["dm_conversation_count"] == 1
    assert surface.metrics["dm_rendered_count"] == 1
    assert surface.metrics["dm_rendered_states"] == [
        "1 unread | Awaiting ACK"
    ]


def test_dm_simulator_keeps_exact_ids_distinct_and_muted_unread_separate() -> None:
    snapshot = ui_simulator.sample_snapshot()
    base = snapshot.dm_messages[0]
    first = replace(
        base,
        source="Raw A",
        conversation="Shared Alias",
        conversation_id="AAAAAAAAAAAAAAAA",
        unread=True,
        muted=True,
        seq=1,
    )
    second = replace(
        base,
        source="Raw B",
        conversation="Shared Alias",
        conversation_id="BBBBBBBBBBBBBBBB",
        unread=False,
        muted=False,
        seq=2,
    )
    snapshot = replace(
        snapshot,
        dm_messages=(first, second),
        unread_dm=0,
        muted_unread_dm=1,
    )

    summaries = ui_simulator.dm_conversation_summaries(snapshot.dm_messages)
    assert [summary.conversation_id for summary in summaries] == [
        "BBBBBBBBBBBBBBBB",
        "AAAAAAAAAAAAAAAA",
    ]
    assert [summary.source for summary in summaries] == [
        "Shared Alias",
        "Shared Alias",
    ]

    surface = ui_simulator.Surface("messages_dm")
    ui_simulator.render_messages_dm(surface, snapshot)
    assert surface.metrics["dm_conversation_count"] == 2
    assert surface.metrics["dm_rendered_states"] == [
        "Received",
        "1 unread muted | New",
    ]


def test_dm_thread_search_excludes_matching_rows_from_other_conversations() -> None:
    snapshot = ui_simulator.sample_snapshot()
    base = snapshot.dm_messages[0]
    older_other = replace(
        base,
        source="Other contact",
        text="shared search token",
        conversation="Other contact",
        conversation_id="AAAAAAAAAAAAAAAA",
        seq=1,
    )
    selected = replace(
        base,
        source="Selected contact",
        text="shared search token",
        conversation="Selected contact",
        conversation_id="BBBBBBBBBBBBBBBB",
        seq=2,
    )
    snapshot = replace(snapshot, dm_messages=(older_other, selected))

    surface = ui_simulator.Surface("dm_thread_search_results")
    ui_simulator.render_dm_thread_state(surface, snapshot, query="shared")

    assert surface.metrics["dm_thread_selected_conversation_id"] == "BBBBBBBBBBBBBBBB"
    assert surface.metrics["dm_thread_source_count"] == 1
    assert surface.metrics["dm_thread_total_matches"] == 1
    assert "Other contact" not in surface.labels

    details = ui_simulator.Surface("dm_thread_details_sheet")
    ui_simulator.render_dm_thread_details_sheet(details, snapshot)
    assert details.metrics["dm_thread_details_conversation_id"] == "BBBBBBBBBBBBBBBB"
    assert "Other contact" not in details.labels


def test_ui_simulator_covers_current_touch_surfaces(tmp_path):
    report = ui_simulator.generate(tmp_path)
    views_by_name = {view["name"]: view for view in report["views"]}
    labels_by_view = {view["name"]: set(view["labels"]) for view in report["views"]}
    expected_docked_views = frozenset(
        {
            "messages",
            "messages_public",
            "messages_channel_private",
                "messages_dm",
                "messages_loading",
                "messages_public_storage_degraded",
                "messages_dm_storage_unavailable",
                "messages_dm_no_contact",
                "messages_dm_no_history",
                "messages_dm_retry",
                "messages_dm_failure",
            "nodes",
            "map",
            "packets",
            "settings",
            "settings_tools_expanded",
            "settings_connections_expanded",
            "settings_storage_maps_expanded",
            "settings_device_expanded",
            "settings_support_expanded",
            "settings_advanced_expanded",
        }
    )
    assert ui_simulator.DOCKED_VIEWS == expected_docked_views

    assert {
        "Messages",
        "Nodes",
        "Map",
        "Tools",
        "Mesh",
        "Wi-Fi",
        "BLE",
        "SD",
        "Attention",
    } <= labels_by_view["home"]
    assert "DeskOS" in labels_by_view["home"]
    assert "Mesh ready, listening" not in labels_by_view["home"]
    assert "D1L Desk" not in labels_by_view["home"]
    assert not any(label.startswith("--:--  Mesh") for label in labels_by_view["home"])
    assert ui_simulator.TOP_BAR_H == 56
    assert ui_simulator.HOME_TOP_BAR_H == 16
    assert ui_simulator.DOCK_Y == 428
    assert ui_simulator.DOCK_H == 52
    assert "RX" not in labels_by_view["home"]
    assert "TX" not in labels_by_view["home"]
    assert not any(target["kind"] == "dock_tab" for target in views_by_name["home"]["touch_targets"])
    for view_name in (
        "messages",
        "messages_public",
        "messages_channel_private",
        "messages_dm",
        "nodes",
        "map",
        "packets",
        "settings",
        "settings_tools_expanded",
        "settings_connections_expanded",
        "settings_storage_maps_expanded",
        "settings_device_expanded",
        "settings_support_expanded",
        "settings_advanced_expanded",
    ):
        assert any(target["kind"] == "dock_tab" for target in views_by_name[view_name]["touch_targets"])
    assert {
        "Messages",
        "Choose a conversation type",
        "Public",
        "Default channel conversation",
        "Direct messages",
        "Private contact conversations",
    } <= labels_by_view["messages"]
    assert {"Public", "Back", "Mark read", "History", "Compose"} <= labels_by_view["messages_public"]
    assert {"Direct messages", "Back"} <= labels_by_view["messages_dm"]
    public_metrics = views_by_name["messages_public"]["metrics"]
    assert public_metrics["public_outgoing_bubbles"] == 1
    assert public_metrics["public_incoming_bubbles"] == 1
    assert public_metrics["public_rendered_states"] == ["New", "Sent over RF"]
    assert public_metrics["public_time_validity_truthful"] is True
    assert public_metrics["public_sticky_compose"] is True
    assert {
        "Nodes",
        "Contacts",
        "Heard Nodes",
        "All Heard",
        "Chat",
        "Repeater",
        "Room",
        "Sensor",
        "Unknown",
        "DM",
        "CMP",
        "ROOM",
    } <= labels_by_view["nodes"]
    assert {"Map", "Options", "(c) OpenStreetMap contributors"} <= labels_by_view["map"]
    assert {
        "Map options",
        "Back to Map",
        "Set location",
        "Cache status",
        "Tiles download only while Map is open. Reopening the same area uses the saved copy.",
    } <= labels_by_view["map_options"]
    assert {
        "Set location",
        "Back",
        "Map center in decimal degrees",
        "Set the center used for the local map area.",
        "Latitude",
        "Longitude",
        "Save location",
    } <= labels_by_view["map_location"]
    assert {
        "Cache status",
        "Back",
            "Read-only readiness",
        "SD card",
        "Wi-Fi",
        "Location",
        "Map view",
        "OpenStreetMap is built in.",
        "(c) OpenStreetMap contributors",
    } <= labels_by_view["map_cache"]
    assert {"Packets", "live tail  rssi -41  snr 30  avg -46", "Mesh Roles", "All", "RX", "TX", "Text", "Search", "Pause", "Routes", "Packet Feed"} <= labels_by_view["packets"]
    assert {
        "Tools",
        "Settings and utilities",
        "Tools",
        "Packets and diagnostics",
        "Connections",
        "Storage & maps",
        "Device",
        "Support",
        "Advanced",
    } <= labels_by_view["settings"]
    assert {"Tools", "Packets and diagnostics", "Packets", "Diagnostics"} <= labels_by_view["settings_tools_expanded"]
    assert {"Tools", "Connections", "Wi-Fi", "Bluetooth", "Radio"} <= labels_by_view["settings_connections_expanded"]
    assert {"Tools", "Storage & maps", "SD Card", "Map options"} <= labels_by_view["settings_storage_maps_expanded"]
    assert {"Tools", "Device", "Display", "Identity"} <= labels_by_view["settings_device_expanded"]
    assert {"Tools", "Support", "About", "Version 1.0.0"} <= labels_by_view["settings_support_expanded"]
    assert {"Tools", "Advanced", "Mesh advertise", "Broadcast presence"} <= labels_by_view["settings_advanced_expanded"]
    assert {
        "Wi-Fi Setup",
        "Profile and state",
        "Network name",
        "Password",
        "SSID",
        "Optional",
        "Scan",
        "Connect",
        "Scan to list nearby 2.4 GHz networks",
        "Keyboard",
        "Save",
        "Clear",
        "Enable",
    } <= labels_by_view["wifi_setup_sheet"]
    assert {
        "BLE Setup",
        "Companion state",
        "BLE companion transport is unavailable in this release.",
        "No BLE pairing or transport artifact is present for public release.",
        "Enable unavailable",
        "Pair unavailable",
        "Forget unavailable",
    } <= labels_by_view["ble_setup_sheet"]
    assert {"Display", "Screen controls", "Brightness", "Night", "Contrast", "Timeout"} <= labels_by_view["display_settings_sheet"]
    assert {"Diagnostics", "Advanced health", "Health", "Crashlog", "Export", "Soak"} <= labels_by_view["diagnostics_sheet"]
    assert {"Compose Public", "Public message", "20 chars | 20/138 B", "Keyboard", "Send", "Clear", "Close"} <= labels_by_view["compose_sheet"]
    assert {"DM YKF Corebot", "Direct message", "reply to YKF Corebot", "20 chars | 20/138 B", "Send", "Clear", "Close"} <= labels_by_view["compose_dm_sheet"]
    assert {"Radio Settings", "Freq 910.525 MHz", "-25k", "+25k", "Cycle BW", "Save"} <= labels_by_view["radio_settings_sheet"]
    assert {
        "Storage",
        "Back",
        "Current storage",
        "Using internal storage",
        "Card status",
        "Data locations",
        ui_simulator.STORAGE_SAFETY_COPY,
    } <= labels_by_view["storage_setup_sheet"]
    assert {
        "Card status",
        "Read-only card details",
        "Back",
        "State",
        "Filesystem",
        "Capacity",
        "Free space",
        "Readiness",
        ui_simulator.STORAGE_SAFETY_COPY,
    } <= labels_by_view["storage_card_page"]
    assert {
        "Data locations",
        "Back",
        "Messages",
        "Direct messages",
        "Packets",
        "Routes",
        "Map tiles",
        ui_simulator.STORAGE_SAFETY_COPY,
    } <= labels_by_view["storage_data_page"]
    assert {row["title"] for row in views_by_name["storage_data_page"]["metrics"]["storage_data_rows"]} == {
        "Messages",
        "Direct messages",
        "Packets",
        "Routes",
        "Map tiles",
        "Exports",
    }
    assert {"Mesh Roles", "Back", "Rooms", "Repeaters", "Large meshes stay bounded"} <= labels_by_view["mesh_roles_sheet"]
    assert {"Rooms", "Back", "Room servers", "All shown"} <= labels_by_view["mesh_rooms_page"]
    assert {"Repeaters", "Back", "Repeater candidates", "All shown"} <= labels_by_view["mesh_repeaters_page"]
    assert {"Advert", "Zero-hop advert", "Zero Hop", "Flood advert", "Flood", "Close"} <= labels_by_view["advert_sheet"]
    assert {"Packet Search", "Search kind, note, raw hex", "Apply", "Clear", "Close"} <= labels_by_view["packet_search_sheet"]
    assert {"Public History", "Public scrollback", "Search", "Clear", "Close"} <= labels_by_view["public_history_sheet"]
    assert {"Public Search", "Search author or message", "Apply", "Clear", "Close"} <= labels_by_view["public_search_sheet"]
    assert {"Message Detail", "Back", "Sender", "Message", "Technical details", "Reply"} <= labels_by_view["message_detail_sheet"]
    assert {
        "DM unavailable [sender_name_unverified]",
        "Public sender names have no verified full key.",
        "DM sender",
    } <= labels_by_view["message_detail_sheet"]
    assert {
        "Message Detail",
        "Back",
        "Sender",
        "Message",
        "Technical details",
        "Signal",
        "Path",
        "Reply",
        ui_simulator.SAMPLE_LONG_PUBLIC_MESSAGE,
    } <= labels_by_view["message_detail_technical_page"]
    assert {"YKF Corebot", "Contact detail", "Back", "Fingerprint", "Signal", "Status", "Message", "Contact options"} <= labels_by_view["contact_detail_sheet"]
    assert {
        "DM unavailable [identity_incomplete]",
        "Identity has no complete verified full key.",
        "Contact options",
    } <= labels_by_view["contact_incomplete_detail_sheet"]
    assert "Message" not in labels_by_view["contact_incomplete_detail_sheet"]
    assert {
        "DM unavailable [contact_not_canonical]",
        "Contact is not verified by signed advert or import.",
        "Contact options",
    } <= labels_by_view["contact_noncanonical_detail_sheet"]
    assert "Message" not in labels_by_view["contact_noncanonical_detail_sheet"]
    assert {
        "Node Detail",
        "Heard-only Chat",
        "Why no DM?",
        "DM unavailable [heard_only]",
        "Heard node only; add or import a verified chat Contact.",
    } <= labels_by_view["heard_only_node_detail_sheet"]
    assert {
        "Node Detail",
        "YKF Room",
        "Why no DM?",
        "DM unavailable [role_not_dm_capable]",
        "This verified role does not support direct chat.",
        "Admin",
        "Verified server; local authenticated login required.",
    } <= labels_by_view["managed_node_detail_sheet"]
    assert {
        "Contact Options",
        "YKF Corebot",
        "Back",
        "Route",
        "Rename",
        "Favorite",
        "Mute",
        "Export",
        "Forget contact",
        "Requires confirmation",
    } <= labels_by_view["contact_options_page"]
    assert {"Node Detail", "Role", "Fingerprint", "Public key", "Signal", "Path", "Advert location", "Last heard", "Close"} <= labels_by_view["node_detail_sheet"]
    assert {"Rename Contact", "YKF Corebot", "Back", "Contact alias", "Keyboard", "Cancel", "Save name"} <= labels_by_view["contact_edit_sheet"]
    assert "Forget" not in labels_by_view["contact_edit_sheet"]
    assert {"Export Contact", "YKF Corebot", "Back", "MeshCore QR", "Fingerprint", "URI", "Ready to scan"} <= labels_by_view["contact_export_sheet"]
    assert {
        "Forget Contact",
        "YKF Corebot",
        "Back",
        "Remove this contact?",
        "This cannot be undone.",
        "Cancel",
        "Forget contact",
    } <= labels_by_view["forget_contact_confirm_page"]
    assert {"YKF Corebot", "2 of 2 messages", "Back", "Search", "Reply"} <= labels_by_view["dm_thread_sheet"]
    assert "Read" not in labels_by_view["dm_thread_sheet"]
    assert "DM Thread" not in labels_by_view["dm_thread_sheet"]
    assert "new" not in labels_by_view["dm_thread_sheet"]
    assert {"YKF Corebot", "Back", "Hide details", "Search", "Technical details", "Reply"} <= labels_by_view["dm_thread_details_sheet"]
    assert {"DM Search", "Search this conversation", "Apply", "Clear", "Close"} <= labels_by_view["dm_search_sheet"]
    assert "No retained messages match this search." in labels_by_view["dm_thread_search_no_match"]
    assert "No retained messages in this conversation." in labels_by_view["dm_thread_empty_sheet"]
    assert {"Route Trace", "YKF Corebot", "Back", "Fingerprint", "Contact Path", "Best Evidence", "Trace"} <= labels_by_view["route_trace_sheet"]
    assert {"Route Detail", "Packet Detail", "Advanced", "Raw Hex"} <= (labels_by_view["route_detail_sheet"] | labels_by_view["packet_detail_sheet"])
    assert {"First boot setup", "Node name", "Start", "Use Defaults"} <= labels_by_view["onboarding_sheet"]


def test_ui_simulator_reports_touch_targets_and_flows(tmp_path):
    report = ui_simulator.generate(tmp_path)
    views = {view["name"]: view for view in report["views"]}
    actions_by_view = {
        name: {target["action"]: target for target in view["touch_targets"]}
        for name, view in views.items()
    }

    assert ui_simulator.EXPECTED_FLOWS
    assert report["flow_report"]["skipped_flows"] == []
    assert report["flow_report"]["missing_steps"] == []
    assert report["flow_report"]["target_issues"] == []
    assert report["flow_report"]["target_overlaps"] == []
    assert report["flow_report"]["format_actions"] == []

    flow_names = {flow["name"] for flow in report["flow_report"]["flows"]}
    assert {
        "first_boot_onboarding",
        "lock_overlay_unlock",
        "home_launcher_navigation",
        "home_status_modals_return_to_active_tab",
        "messages_hierarchy_navigation",
        "public_compose_and_send",
        "public_history_search",
        "public_message_detail",
            "public_message_reply",
            "public_sender_dm_explanation",
        "dm_thread_open_and_reply",
            "node_detail_inspection",
            "heard_only_dm_explanation",
        "contact_detail_options_hierarchy",
        "contact_rename_and_forget_confirmation",
        "map_page_policy",
        "packet_filters_search_and_details",
        "mesh_roles_browser",
        "more_category_navigation",
        "more_tools_expanded_navigation",
        "more_connections_expanded_navigation",
        "more_storage_maps_expanded_navigation",
        "more_device_expanded_navigation",
        "more_support_expanded_navigation",
        "more_advanced_expanded_navigation",
        "settings_radio_storage_and_advert",
        "settings_display_and_diagnostics",
    } <= flow_names

    assert actions_by_view["messages"]["open_messages_public"]["destination"] == "messages_public"
    assert actions_by_view["messages"]["open_messages_dm"]["destination"] == "messages_dm"
    assert actions_by_view["messages_public"]["open_messages_root"]["destination"] == "messages"
    assert actions_by_view["messages_public"]["open_public_compose"]["destination"] == "compose_sheet"
    assert actions_by_view["messages_public"]["open_public_history"]["destination"] == "public_history_sheet"
    assert actions_by_view["messages_public"]["open_message_detail"]["destination"] == "message_detail_sheet"
    assert actions_by_view["messages_dm"]["open_messages_root"]["destination"] == "messages"
    assert actions_by_view["messages_dm"]["open_dm_thread"]["destination"] == "dm_thread_sheet"
    assert actions_by_view["messages_dm"]["open_dm_thread"]["marks_read"] is True
    assert actions_by_view["message_detail_sheet"]["close_message_detail"]["destination"] == "messages_public"
    assert actions_by_view["message_detail_sheet"]["toggle_message_detail_advanced"]["height"] >= ui_simulator.MIN_TOUCH_TARGET
    assert actions_by_view["message_detail_sheet"]["toggle_message_detail_advanced"]["destination"] == "message_detail_technical_page"
    assert actions_by_view["message_detail_technical_page"]["toggle_message_detail_advanced"]["destination"] == "message_detail_sheet"
    assert actions_by_view["message_detail_sheet"]["open_public_reply"]["destination"] == "compose_sheet"
    assert actions_by_view["message_detail_sheet"]["open_public_reply"]["visual_box"] == [16, 420, 232, 472]
    assert actions_by_view["message_detail_sheet"]["open_public_reply"]["height"] >= 48
    assert actions_by_view["message_detail_technical_page"]["open_public_reply"]["visual_box"] == [16, 420, 232, 472]
    sender_dm = actions_by_view["message_detail_sheet"]["explain_public_sender_dm"]
    assert sender_dm["visual_box"] == [248, 420, 464, 472]
    assert sender_dm["destination"] is None
    assert views["message_detail_sheet"]["metrics"]["sender_dm_reason_code"] == "sender_name_unverified"
    assert views["message_detail_sheet"]["metrics"]["sender_dm_opens_compose"] is False
    assert views["message_detail_sheet"]["metrics"]["sender_dm_rf_tx"] is False
    heard_only_dm = actions_by_view["heard_only_node_detail_sheet"]["explain_node_dm"]
    assert heard_only_dm["height"] >= ui_simulator.MIN_TOUCH_TARGET
    assert heard_only_dm["destination"] is None
    assert views["heard_only_node_detail_sheet"]["metrics"]["node_detail_dm_reason_code"] == "heard_only"
    assert views["heard_only_node_detail_sheet"]["metrics"]["node_detail_dm_opens_compose"] is False
    assert views["heard_only_node_detail_sheet"]["metrics"]["node_detail_dm_rf_tx"] is False
    assert views["node_detail_sheet"]["metrics"]["node_detail_dm_reason_code"] == "ready"
    assert views["node_detail_sheet"]["metrics"]["node_detail_dm_exact_full_key"] is True
    assert views["contact_detail_sheet"]["metrics"]["contact_dm_reason_code"] == "ready"
    assert views["contact_detail_sheet"]["metrics"]["contact_dm_exact_full_key"] is True
    assert views["contact_incomplete_detail_sheet"]["metrics"]["contact_dm_reason_code"] == "identity_incomplete"
    assert views["contact_incomplete_detail_sheet"]["metrics"]["contact_dm_opens_compose"] is False
    assert views["contact_noncanonical_detail_sheet"]["metrics"]["contact_dm_reason_code"] == "contact_not_canonical"
    assert views["contact_noncanonical_detail_sheet"]["metrics"]["contact_dm_opens_compose"] is False
    assert views["node_detail_sheet"]["metrics"]["node_detail_frame"] == [16, 60, 464, 476]
    assert views["node_detail_sheet"]["metrics"]["node_detail_content_clipped"] is False
    assert views["heard_only_node_detail_sheet"]["metrics"]["node_detail_content_clipped"] is False
    assert views["managed_node_detail_sheet"]["metrics"]["node_detail_frame"] == [16, 60, 464, 476]
    assert views["managed_node_detail_sheet"]["metrics"]["node_detail_content_bottom"] == 460
    assert views["managed_node_detail_sheet"]["metrics"]["node_detail_content_clipped"] is False
    assert views["managed_node_detail_sheet"]["metrics"]["node_detail_management_gated"] is True
    assert views["nodes"]["metrics"]["contact_dm_shortcut_min_height"] >= ui_simulator.MIN_TOUCH_TARGET
    assert views["nodes"]["metrics"]["node_dm_shortcut_min_height"] >= ui_simulator.MIN_TOUCH_TARGET
    assert views["message_detail_technical_page"]["metrics"]["message_text_complete"] is True
    assert views["message_detail_technical_page"]["metrics"]["message_wrapped_lines"] >= 3
    assert views["message_detail_technical_page"]["metrics"]["message_technical_details_expanded"] is True
    assert views["message_detail_technical_page"]["metrics"]["message_body_scrollable"] is True
    assert views["message_detail_technical_page"]["metrics"]["message_sticky_reply"] is True
    tx_detail = views["public_tx_detail_technical_page"]["metrics"]
    assert tx_detail["message_direction"] == "tx"
    assert tx_detail["message_delivery_state"] == "Sent over RF"
    assert tx_detail["message_signal_measured"] is False
    assert tx_detail["message_path_hops_measured"] is False
    assert tx_detail["message_reply_available"] is False
    assert tx_detail["message_navigation_rf_silent"] is True
    assert actions_by_view["dm_thread_sheet"]["close_dm_thread"]["destination"] == "messages_dm"
    assert actions_by_view["dm_thread_sheet"]["open_dm_reply"]["visual_box"] == [16, 420, 464, 472]
    assert actions_by_view["dm_thread_sheet"]["open_dm_reply"]["height"] >= 48
    assert actions_by_view["dm_thread_sheet"]["open_dm_reply"]["destination"] == "compose_dm_sheet"
    assert actions_by_view["dm_thread_sheet"]["toggle_dm_details"]["destination"] == "dm_thread_details_sheet"
    assert actions_by_view["dm_thread_sheet"]["open_dm_search"]["destination"] == "dm_search_sheet"
    assert actions_by_view["dm_thread_sheet"]["open_dm_search"]["rf_tx"] is False
    assert actions_by_view["dm_thread_details_sheet"]["toggle_dm_details"]["destination"] == "dm_thread_sheet"
    assert actions_by_view["dm_thread_details_sheet"]["open_dm_reply"]["visual_box"] == [16, 420, 464, 472]
    assert actions_by_view["dm_thread_details_sheet"]["open_dm_reply"]["height"] >= 48
    assert actions_by_view["dm_thread_details_sheet"]["open_dm_reply"]["destination"] == "compose_dm_sheet"
    assert views["dm_thread_details_sheet"]["metrics"]["dm_thread_details_expanded"] is True
    assert views["dm_thread_details_sheet"]["metrics"]["dm_thread_navigation_rf_silent"] is True
    assert actions_by_view["dm_search_sheet"]["apply_dm_search"]["destination"] == "dm_thread_search_results"
    assert actions_by_view["dm_search_sheet"]["clear_dm_search"]["destination"] == "dm_thread_sheet"
    assert views["dm_search_sheet"]["metrics"]["dm_search_advances_read_cursor"] is False
    for label in ("Apply", "Clear", "Close"):
        target = next(
            item for item in views["dm_search_sheet"]["touch_targets"]
            if item["label"] == label
        )
        assert target["height"] >= 44
    assert "mark_dm_thread_read" not in actions_by_view["dm_thread_sheet"]
    assert not any(target["kind"] == "dock_tab" for target in views["compose_sheet"]["touch_targets"])
    assert not any(target["kind"] == "dock_tab" for target in views["compose_dm_sheet"]["touch_targets"])
    assert not any(target["kind"] == "dock_tab" for target in views["home"]["touch_targets"])
    assert actions_by_view["home"]["open_messages_root"]["visual_box"] == [12, 16, 234, 156]
    assert actions_by_view["home"]["open_nodes"]["visual_box"] == [246, 16, 468, 156]
    assert actions_by_view["home"]["open_map"]["visual_box"] == [12, 164, 234, 304]
    assert actions_by_view["home"]["open_settings"]["visual_box"] == [246, 164, 468, 304]
    home_status_actions = (
        ("open_radio_settings", [14, 314, 102, 398], "radio_settings_sheet", "LV_SYMBOL_LOOP"),
        ("open_wifi_settings", [105, 314, 193, 398], "wifi_setup_sheet", "LV_SYMBOL_WIFI"),
        ("open_ble_settings", [196, 314, 284, 398], "ble_setup_sheet", "LV_SYMBOL_BLUETOOTH"),
        ("open_storage_setup", [287, 314, 375, 398], "storage_setup_sheet", "LV_SYMBOL_SD_CARD"),
        ("open_diagnostics", [378, 314, 466, 398], "diagnostics_sheet", "LV_SYMBOL_WARNING"),
    )
    for action, box, destination, icon in home_status_actions:
        target = actions_by_view["home"][action]
        assert target["visual_box"] == box
        assert target["kind"] == "home_status_item"
        assert target["width"] == 88
        assert target["height"] == 84
        assert target["destination"] == destination
        assert target["icon"] == icon
        assert ": " in target["semantic_label"]
        assert target["rf_tx"] is False
        assert target["formats_sd"] is False
        assert target["destructive"] is False
    modal_close_actions = {
        "radio_settings_sheet": "close_radio_settings",
        "wifi_setup_sheet": "close_wifi_setup",
        "ble_setup_sheet": "close_ble_setup",
        "storage_setup_sheet": "close_storage_setup",
        "diagnostics_sheet": "close_diagnostics",
    }
    for view_name, action in modal_close_actions.items():
        assert actions_by_view[view_name][action]["destination"] == "active_tab"
        assert views[view_name]["metrics"]["modal_return_policy"] == "active_tab"
    assert (
        actions_by_view["radio_settings_sheet"]["save_radio_profile"]["destination"]
        == "radio_settings_sheet"
    )
    assert views["home"]["metrics"]["home_status_strip_height"] == 88
    assert views["home"]["metrics"]["home_status_item_count"] == 5
    for docked_view in (
        "messages",
        "messages_public",
        "messages_dm",
        "nodes",
        "map",
        "packets",
        "settings",
        "settings_tools_expanded",
        "settings_connections_expanded",
        "settings_storage_maps_expanded",
        "settings_device_expanded",
        "settings_support_expanded",
        "settings_advanced_expanded",
    ):
        dock_targets = [target for target in views[docked_view]["touch_targets"] if target["kind"] == "dock_tab"]
        assert [target["label"] for target in dock_targets] == [
            "Home tab",
            "Messages tab",
            "Nodes tab",
            "Map tab",
            "Tools tab",
        ], docked_view
        assert all(target["width"] == 88 for target in dock_targets), docked_view
        assert all(target["height"] == 44 for target in dock_targets), docked_view
        assert [target["semantic_label"] for target in dock_targets] == [
            "Home",
            "Messages",
            "Nodes",
            "Map",
            "Tools",
        ], docked_view
        assert [target["icon"] for target in dock_targets] == [
            "LV_SYMBOL_HOME",
            "LV_SYMBOL_ENVELOPE",
            "LV_SYMBOL_LIST",
            "LV_SYMBOL_IMAGE",
            "LV_SYMBOL_SETTINGS",
        ], docked_view
        assert sum(bool(target["selected"]) for target in dock_targets) == 1, docked_view
        assert views[docked_view]["metrics"]["docked_content_height"] == 372, docked_view
    assert actions_by_view["home"]["open_messages_root"]["destination"] == "messages"
    assert actions_by_view["home"]["open_nodes"]["destination"] == "nodes"
    assert actions_by_view["home"]["open_map"]["destination"] == "map"
    assert actions_by_view["home"]["open_settings"]["destination"] == "settings"
    assert actions_by_view["wifi_setup_sheet"]["close_wifi_setup"]["destination"] == "active_tab"
    assert actions_by_view["wifi_setup_sheet"]["wifi_scan"]["destination"] is None
    assert actions_by_view["wifi_setup_sheet"]["wifi_connect"]["destination"] is None
    assert actions_by_view["wifi_setup_sheet"]["edit_wifi_ssid"]["kind"] == "text_field"
    assert actions_by_view["wifi_setup_sheet"]["edit_wifi_password"]["kind"] == "text_field"
    assert not any(target["kind"] == "dock_tab" for target in views["wifi_setup_sheet"]["touch_targets"])
    assert actions_by_view["ble_setup_sheet"]["close_ble_setup"]["destination"] == "active_tab"
    assert actions_by_view["map"]["open_map_options"]["destination"] == "map_options"
    assert set(actions_by_view["map_options"]) == {
        "close_map_options",
        "open_map_location",
        "open_map_cache",
    }
    assert actions_by_view["map_options"]["close_map_options"]["destination"] == "map"
    assert actions_by_view["map_options"]["open_map_location"]["destination"] == "map_location"
    assert actions_by_view["map_options"]["open_map_cache"]["destination"] == "map_cache"
    assert not any(target["kind"] == "dock_tab" for target in views["map_options"]["touch_targets"])
    assert set(actions_by_view["map_cache"]) == {"close_map_cache"}
    assert actions_by_view["map_cache"]["close_map_cache"]["destination"] == "map_options"
    assert actions_by_view["map_location"]["save_map_location"]["destination"] == "map"
    assert actions_by_view["map_location"]["edit_map_latitude"]["kind"] == "text_field"
    assert actions_by_view["map_location"]["edit_map_longitude"]["kind"] == "text_field"
    assert actions_by_view["map_location"]["close_map_location"]["destination"] == "map_options"
    assert actions_by_view["nodes"]["open_node_detail"]["destination"] == "node_detail_sheet"
    assert actions_by_view["nodes"]["open_dm_compose"]["destination"] == "compose_dm_sheet"
    assert actions_by_view["nodes"]["open_node_dm"]["destination"] == "compose_dm_sheet"
    assert actions_by_view["node_detail_sheet"]["open_node_dm"]["destination"] == "compose_dm_sheet"
    assert actions_by_view["node_detail_sheet"]["close_node_detail"]["destination"] == "nodes"
    assert set(actions_by_view["contact_detail_sheet"]) == {
        "close_contact_detail",
        "open_dm_compose",
        "open_contact_options",
    }
    assert actions_by_view["contact_detail_sheet"]["close_contact_detail"]["destination"] == "nodes"
    assert actions_by_view["contact_detail_sheet"]["open_dm_compose"]["destination"] == "compose_dm_sheet"
    assert actions_by_view["contact_detail_sheet"]["open_contact_options"]["destination"] == "contact_options_page"
    assert set(actions_by_view["contact_options_page"]) == {
        "close_contact_options",
        "open_route_trace",
        "open_contact_edit",
        "toggle_favorite",
        "toggle_mute",
        "open_contact_export",
        "open_forget_contact_confirm",
    }
    assert actions_by_view["contact_options_page"]["close_contact_options"]["destination"] == "contact_detail_sheet"
    assert actions_by_view["contact_options_page"]["open_route_trace"]["destination"] == "route_trace_sheet"
    assert actions_by_view["contact_options_page"]["open_contact_edit"]["destination"] == "contact_edit_sheet"
    assert actions_by_view["contact_options_page"]["toggle_favorite"]["destination"] is None
    assert actions_by_view["contact_options_page"]["toggle_mute"]["destination"] is None
    assert actions_by_view["contact_options_page"]["open_contact_export"]["destination"] == "contact_export_sheet"
    assert actions_by_view["contact_options_page"]["open_forget_contact_confirm"]["destination"] == "forget_contact_confirm_page"
    assert actions_by_view["contact_options_page"]["open_forget_contact_confirm"]["destructive"] is False
    assert set(actions_by_view["contact_edit_sheet"]) == {
        "close_contact_edit",
        "edit_contact_alias",
        "cancel_contact_edit",
        "save_contact_alias",
    }
    assert actions_by_view["contact_edit_sheet"]["close_contact_edit"]["destination"] == "contact_options_page"
    assert actions_by_view["contact_edit_sheet"]["cancel_contact_edit"]["destination"] == "contact_options_page"
    assert actions_by_view["contact_edit_sheet"]["save_contact_alias"]["destination"] == "contact_options_page"
    assert set(actions_by_view["contact_export_sheet"]) == {"close_contact_export"}
    assert actions_by_view["contact_export_sheet"]["close_contact_export"]["destination"] == "contact_options_page"
    assert set(actions_by_view["route_trace_sheet"]) == {"close_route_trace", "send_contact_trace"}
    assert actions_by_view["route_trace_sheet"]["close_route_trace"]["destination"] == "contact_options_page"
    trace_target = actions_by_view["route_trace_sheet"]["send_contact_trace"]
    assert trace_target["rf_tx"] is True
    assert trace_target["public_rf_tx"] is False
    assert trace_target["dm_tx"] is False
    assert set(actions_by_view["forget_contact_confirm_page"]) == {
        "close_forget_contact_confirm",
        "cancel_forget_contact",
        "confirm_forget_contact",
    }
    assert actions_by_view["forget_contact_confirm_page"]["close_forget_contact_confirm"]["destination"] == "contact_options_page"
    assert actions_by_view["forget_contact_confirm_page"]["cancel_forget_contact"]["destination"] == "contact_options_page"
    assert actions_by_view["forget_contact_confirm_page"]["confirm_forget_contact"]["destination"] == "nodes"
    assert actions_by_view["forget_contact_confirm_page"]["confirm_forget_contact"]["destructive"] is True
    assert report["flow_report"]["destructive_actions"] == [
        {
            "view": "forget_contact_confirm_page",
            "action": "confirm_forget_contact",
            "label": "Forget contact",
            "destination": "nodes",
        }
    ]
    assert {
        name: views[name]["metrics"]["contact_hierarchy_level"]
        for name in ui_simulator.CONTACT_HIERARCHY_VIEWS
    } == {
        "contact_detail_sheet": "detail",
        "contact_options_page": "options",
        "contact_edit_sheet": "rename",
        "contact_export_sheet": "export",
        "forget_contact_confirm_page": "forget_confirm",
        "route_trace_sheet": "route_trace",
    }
    assert views["contact_detail_sheet"]["metrics"]["contact_primary_action_count"] == 2
    assert views["contact_options_page"]["metrics"]["contact_option_count"] == 6
    assert views["forget_contact_confirm_page"]["metrics"]["contact_forget_confirmation"] is True
    assert views["forget_contact_confirm_page"]["metrics"]["contact_warning_complete"] is True
    assert all(views[name]["dock_rendered"] is False for name in ui_simulator.CONTACT_HIERARCHY_VIEWS)
    assert "send_public_test" not in actions_by_view["messages"]
    assert views["messages"]["metrics"]["messages_navigation_rf_silent"] is True
    assert views["messages_public"]["metrics"]["public_navigation_rf_silent"] is True
    assert actions_by_view["compose_sheet"]["send_channel_text"]["public_rf_tx"] is True
    assert views["compose_dm_sheet"]["metrics"]["compose_is_dm"] is True
    assert views["compose_dm_sheet"]["metrics"]["compose_send_enabled"] is True
    assert views["compose_dm_sheet"]["metrics"]["compose_eligibility_reason"] == "ready"
    assert actions_by_view["compose_dm_sheet"]["send_dm_text"]["enabled"] is True
    assert actions_by_view["compose_dm_sheet"]["send_dm_text"]["rf_tx"] is True
    assert actions_by_view["compose_dm_sheet"]["send_dm_text"]["public_rf_tx"] is False
    assert actions_by_view["compose_dm_sheet"]["send_dm_text"]["dm_tx"] is True
    assert actions_by_view["compose_dm_sheet"]["clear_dm_message"]["rf_tx"] is False
    assert actions_by_view["compose_dm_sheet"]["close_compose"]["destination"] == "messages_dm"
    assert views["compose_utf8_sheet"]["metrics"]["compose_validation"] == "valid_utf8"
    assert views["compose_utf8_sheet"]["metrics"]["compose_byte_count"] == 12
    assert views["compose_utf8_sheet"]["metrics"]["compose_character_count"] == 7
    assert views["compose_byte_limit_sheet"]["metrics"]["compose_byte_count"] == 138
    assert views["compose_byte_limit_sheet"]["metrics"]["compose_send_enabled"] is True
    for invalid_view in ("compose_oversize_sheet", "compose_invalid_sheet"):
        assert views[invalid_view]["metrics"]["compose_send_enabled"] is False
        target = actions_by_view[invalid_view]["send_channel_text"]
        assert target["enabled"] is False
        assert target["rf_tx"] is False
        assert target["public_rf_tx"] is False
    disabled_runtime_views = {
        "compose_offline_sheet": "radio_waiting",
        "compose_busy_sheet": "radio_busy",
        "compose_protocol_time_sheet": "protocol_time_unavailable",
        "compose_dm_no_contact_sheet": "contact_missing",
        "compose_dm_active_sheet": "dm_delivery_active",
    }
    for view_name, reason in disabled_runtime_views.items():
        metrics = views[view_name]["metrics"]
        assert metrics["compose_send_enabled"] is False
        assert metrics["compose_eligibility_reason"] == reason
        send_action = "send_dm_text" if metrics["compose_is_dm"] else "send_channel_text"
        target = actions_by_view[view_name][send_action]
        assert target["enabled"] is False
        assert target["rf_tx"] is False
        assert target["public_rf_tx"] is False
        assert target["dm_tx"] is False
    retry = views["compose_retry_sheet"]["metrics"]
    assert retry["compose_send_enabled"] is True
    assert retry["compose_retry_available"] is True
    assert retry["compose_eligibility_reason"] == "retry_timeout"
    retry_target = actions_by_view["compose_retry_sheet"]["send_channel_text"]
    assert retry_target["public_rf_tx"] is True
    assert retry_target["enabled"] is True
    for compose_view in (
        "compose_sheet",
        "compose_utf8_sheet",
        "compose_byte_limit_sheet",
        "compose_oversize_sheet",
        "compose_invalid_sheet",
        "compose_dm_sheet",
        *disabled_runtime_views,
        "compose_retry_sheet",
    ):
        for label in ("Send", "Clear", "Close"):
            target = next(
                item for item in views[compose_view]["touch_targets"]
                if item["label"] == label
            )
            assert target["visual_box"][3] - target["visual_box"][1] >= 44
    assert {
        "toggle_more_tools",
        "toggle_more_connections",
        "toggle_more_storage_maps",
        "toggle_more_device",
        "toggle_more_support",
        "toggle_more_advanced",
    } == set(actions_by_view["settings"]) - {"open_home", "open_messages", "open_nodes", "open_map", "open_settings"}
    assert all(
        actions_by_view["settings"][action]["kind"] == "menu_category"
        for action in (
            "toggle_more_tools",
            "toggle_more_connections",
            "toggle_more_storage_maps",
            "toggle_more_device",
            "toggle_more_support",
            "toggle_more_advanced",
        )
    )
    assert actions_by_view["settings"]["toggle_more_tools"]["visual_box"] == [18, 110, 462, 158]
    assert actions_by_view["settings"]["toggle_more_connections"]["visual_box"] == [18, 162, 462, 210]
    assert actions_by_view["settings"]["toggle_more_storage_maps"]["visual_box"] == [18, 214, 462, 262]
    assert actions_by_view["settings"]["toggle_more_device"]["visual_box"] == [18, 266, 462, 314]
    assert actions_by_view["settings"]["toggle_more_support"]["visual_box"] == [18, 318, 462, 366]
    assert actions_by_view["settings"]["toggle_more_advanced"]["visual_box"] == [18, 370, 462, 418]
    assert actions_by_view["settings_tools_expanded"]["open_packets"]["destination"] == "packets"
    assert actions_by_view["settings_tools_expanded"]["open_diagnostics"]["destination"] == "diagnostics_sheet"
    assert actions_by_view["settings_connections_expanded"]["open_wifi_settings"]["destination"] == "wifi_setup_sheet"
    assert actions_by_view["settings_connections_expanded"]["open_ble_settings"]["destination"] == "ble_setup_sheet"
    assert actions_by_view["settings_connections_expanded"]["open_radio_settings"]["destination"] == "radio_settings_sheet"
    assert actions_by_view["settings_storage_maps_expanded"]["open_storage_setup"]["destination"] == "storage_setup_sheet"
    assert actions_by_view["settings_storage_maps_expanded"]["open_map_options"]["destination"] == "map_options"
    assert actions_by_view["settings_device_expanded"]["open_display_settings"]["destination"] == "display_settings_sheet"
    assert actions_by_view["settings_advanced_expanded"]["open_advert_sheet"]["destination"] == "advert_sheet"
    assert not any(target["label"] == "Identity" for target in views["settings_device_expanded"]["touch_targets"])
    assert not any(target["label"] == "About" for target in views["settings_support_expanded"]["touch_targets"])
    for key, expected_leaves, expected_actions in (
        ("tools", 2, 2),
        ("connections", 3, 3),
        ("storage_maps", 2, 2),
        ("device", 2, 1),
        ("support", 1, 0),
        ("advanced", 1, 1),
    ):
        expanded_view = views[f"settings_{key}_expanded"]
        assert expanded_view["metrics"]["more_expanded_category"] == key
        assert expanded_view["metrics"]["more_leaf_count"] == expected_leaves
        assert expanded_view["metrics"]["more_actionable_leaf_count"] == expected_actions
        assert expanded_view["metrics"]["more_scroll_anchor_y"] == 110
    assert actions_by_view["display_settings_sheet"]["close_display_settings"]["destination"] == "settings"
    assert actions_by_view["diagnostics_sheet"]["close_diagnostics"]["destination"] == "active_tab"
    assert actions_by_view["packets"]["pause_packet_feed"]["height"] >= ui_simulator.MIN_TOUCH_TARGET
    assert actions_by_view["packet_detail_sheet"]["toggle_packet_detail_advanced"]["height"] >= ui_simulator.MIN_TOUCH_TARGET
    assert set(actions_by_view["mesh_roles_sheet"]) == {
        "close_mesh_roles",
        "open_mesh_rooms",
        "open_mesh_repeaters",
    }
    assert actions_by_view["mesh_roles_sheet"]["close_mesh_roles"]["destination"] == "packets"
    assert actions_by_view["mesh_roles_sheet"]["open_mesh_rooms"]["destination"] == "mesh_rooms_page"
    assert actions_by_view["mesh_roles_sheet"]["open_mesh_repeaters"]["destination"] == "mesh_repeaters_page"
    assert set(actions_by_view["mesh_rooms_page"]) == {"close_mesh_rooms"}
    assert actions_by_view["mesh_rooms_page"]["close_mesh_rooms"]["destination"] == "mesh_roles_sheet"
    assert set(actions_by_view["mesh_repeaters_page"]) == {"close_mesh_repeaters"}
    assert actions_by_view["mesh_repeaters_page"]["close_mesh_repeaters"]["destination"] == "mesh_roles_sheet"
    for page_name in ui_simulator.MESH_ROLE_VIEWS:
        for target in views[page_name]["touch_targets"]:
            x0, y0, x1, y1 = target["visual_box"]
            assert x1 - x0 >= ui_simulator.MIN_TOUCH_TARGET, (page_name, target["label"])
            assert y1 - y0 >= ui_simulator.MIN_TOUCH_TARGET, (page_name, target["label"])
    assert views["mesh_rooms_page"]["metrics"]["mesh_rooms_source_count"] == 1
    assert views["mesh_rooms_page"]["metrics"]["mesh_rooms_loaded_count"] == 1
    assert views["mesh_rooms_page"]["metrics"]["mesh_rooms_rendered_count"] == 1
    assert views["mesh_rooms_page"]["metrics"]["mesh_rooms_scrollable"] is False
    assert views["mesh_repeaters_page"]["metrics"]["mesh_repeaters_source_count"] == 1
    assert views["mesh_repeaters_page"]["metrics"]["mesh_repeaters_loaded_count"] == 1
    assert views["mesh_repeaters_page"]["metrics"]["mesh_repeaters_rendered_count"] == 1
    assert views["mesh_repeaters_page"]["metrics"]["mesh_repeaters_scrollable"] is False
    for page_name in ("mesh_rooms_page", "mesh_repeaters_page"):
        panel_top = views[page_name]["metrics"]["mesh_roles_list_panel"][1]
        back_bottom = next(
            target["visual_box"][3]
            for target in views[page_name]["touch_targets"]
            if target["label"] == "Back"
        )
        assert back_bottom < panel_top
    assert actions_by_view["advert_sheet"]["send_advert_zero"]["rf_tx"] is True
    assert actions_by_view["advert_sheet"]["send_advert_flood"]["rf_tx"] is True
    assert set(actions_by_view["storage_setup_sheet"]) == {
        "close_storage_setup",
        "open_storage_card",
        "open_storage_data",
    }
    assert actions_by_view["storage_setup_sheet"]["close_storage_setup"]["destination"] == "active_tab"
    assert actions_by_view["storage_setup_sheet"]["open_storage_card"]["destination"] == "storage_card_page"
    assert actions_by_view["storage_setup_sheet"]["open_storage_data"]["destination"] == "storage_data_page"
    assert set(actions_by_view["storage_card_page"]) == {"close_storage_card"}
    assert actions_by_view["storage_card_page"]["close_storage_card"]["destination"] == "storage_setup_sheet"
    assert set(actions_by_view["storage_data_page"]) == {"close_storage_data"}
    assert actions_by_view["storage_data_page"]["close_storage_data"]["destination"] == "storage_setup_sheet"
    for page_name in ui_simulator.STORAGE_HIERARCHY_VIEWS:
        assert views[page_name]["dock_rendered"] is False
        assert views[page_name]["sibling_text_overlaps"] == []
        assert all(target["formats_sd"] is False for target in views[page_name]["touch_targets"])
        assert all(target["destructive"] is False for target in views[page_name]["touch_targets"])
    assert actions_by_view["lock_overlay"]["unlock"]["destination"] == "home"

    for view in report["views"]:
        for target in view["touch_targets"]:
            assert target["width"] >= ui_simulator.MIN_TOUCH_TARGET, (view["name"], target["label"])
            assert target["height"] >= ui_simulator.MIN_TOUCH_TARGET, (view["name"], target["label"])
            assert target["offscreen"] is False, (view["name"], target["label"])
            assert target["top_bar_overlap"] is False, (view["name"], target["label"])
            assert target["dock_overlap"] is False, (view["name"], target["label"])
            assert target["unexpected_dock"] is False, (view["name"], target["label"])


def test_ui_simulator_storage_state_scenarios_fit(tmp_path):
    internal = {
        "Messages": "Internal",
        "Direct messages": "Internal",
        "Packets": "Internal",
        "Routes": "Internal",
        "Map tiles": "Unavailable",
        "Exports": "USB only",
    }
    retained_sd = {
        "Messages": "SD + internal backup",
        "Direct messages": "SD + internal backup",
        "Packets": "SD + internal backup",
        "Routes": "SD + internal backup",
        "Map tiles": "Pending",
        "Exports": "USB only",
    }
    scenarios = {
        "default": (
            "Using internal storage",
            "Your data stays available internally.",
            "Card reader starting",
            "Not available",
            internal,
        ),
        "storage-no-card": (
            "No SD card",
            "Insert a FAT32 card when you want more space.",
            "No card inserted",
            "No card",
            internal,
        ),
        "storage-needs-fat32": (
            "Card needs FAT32",
            "Prepare the card as FAT32 on a computer, then reinsert it.",
            "FAT32 card required",
            "Needs FAT32",
            internal,
        ),
        "storage-mount-error": (
            "Card needs attention",
            "Technical details are available over USB.",
            "Card needs attention",
            "Needs attention",
            internal,
        ),
        "storage-probe-error": (
            "Card needs attention",
            "Technical details are available over USB.",
            "Card needs attention",
            "Needs attention",
            internal,
        ),
        "storage-root-missing": (
            "Card setup incomplete",
            "Reinsert the card to finish creating DeskOS folders.",
            "Preparing DeskOS folders",
            "Setup incomplete",
            internal,
        ),
        "storage-ready-pending-migration": (
            "SD card ready",
            "The card is ready while saved data stays internal.",
            "Ready",
            "Ready",
            {**internal, "Map tiles": "Pending"},
        ),
        "storage-ready-packet-log-sd": (
            "SD card ready",
            "Saved data stays mirrored internally.",
            "Ready",
            "Ready",
            {**internal, "Packets": "SD + internal backup", "Map tiles": "Pending"},
        ),
        "storage-ready-retained-history-sd": (
            "SD card ready",
            "Saved data stays mirrored internally.",
            "Ready",
            "Ready",
            retained_sd,
        ),
        "storage-ready-map-tiles-sd": (
            "SD card ready",
            "Saved data stays mirrored internally.",
            "Ready",
            "Ready",
            {**retained_sd, "Map tiles": "SD card", "Exports": "SD card"},
        ),
        "storage-ready-map-only-sd": (
            "SD card ready",
            "The card is ready while saved data stays internal.",
            "Ready",
            "Ready",
            {**internal, "Map tiles": "SD card"},
        ),
        "storage-ready-export-only-sd": (
            "SD card ready",
            "The card is ready while saved data stays internal.",
            "Ready",
            "Ready",
            {**internal, "Map tiles": "Pending", "Exports": "SD card"},
        ),
        "storage-degraded": (
            "SD needs attention",
            "Saved data remains available.",
            "Card needs attention",
            "Needs attention",
            retained_sd,
        ),
        "storage-media-error": (
            "Card needs attention",
            "Technical details are available over USB.",
            "Card needs attention",
            "Needs attention",
            internal,
        ),
        "storage-bridge-reported": (
            "Card needs attention",
            "Technical details are available over USB.",
            "Card needs attention",
            "Needs attention",
            internal,
        ),
    }
    raw_tokens = {
        "bridge_protocol_pending",
        "insert_card",
        "prepare_fat32_on_computer",
        "inspect_rp2040_sd_mount_error_firmware_path",
        "inspect_rp2040_sd_cmd0_firmware_path",
        "retry_storage_mount",
        "store_migration_pending",
        "packet_log_canary_enabled",
        "retained_history_sd_enabled",
        "NVS fallback",
        "Backends",
        "RP2040",
        "CMD0",
        "SdFat",
        "bridge_reported",
        "not_available",
    }

    for scenario, (state, guidance, card_state, readiness, expected_data) in scenarios.items():
        report = ui_simulator.generate(
            tmp_path / scenario,
            views=("storage_setup_sheet", "storage_card_page", "storage_data_page"),
            scenario=scenario,
        )
        views = {view["name"]: view for view in report["views"]}
        labels = {name: set(view["labels"]) for name, view in views.items()}

        assert report["ok"] is True, scenario
        assert report["overflow_count"] == 0, scenario
        assert report["touch_target_issue_count"] == 0, scenario
        assert report["flow_report"]["format_actions"] == [], scenario
        assert report["flow_report"]["destructive_actions"] == [], scenario
        assert report["required_labels_missing"] == [], scenario
        assert {state, guidance, "Card status", "Data locations", ui_simulator.STORAGE_SAFETY_COPY} <= labels[
            "storage_setup_sheet"
        ]
        card_rows = {row["title"]: row["value"] for row in views["storage_card_page"]["metrics"]["storage_card_rows"]}
        assert card_rows["State"] == card_state
        assert card_rows["Readiness"] == readiness
        assert set(card_rows) == {"State", "Filesystem", "Capacity", "Free space", "Readiness"}

        all_storage_labels = set().union(*(labels[name] for name in ui_simulator.STORAGE_HIERARCHY_VIEWS))
        for raw in raw_tokens:
            assert all(raw not in label for label in all_storage_labels), (scenario, raw)

        root = views["storage_setup_sheet"]
        card = views["storage_card_page"]
        data = views["storage_data_page"]
        assert root["metrics"]["storage_hierarchy_level"] == "root"
        assert card["metrics"]["storage_hierarchy_level"] == "card"
        assert data["metrics"]["storage_hierarchy_level"] == "data"
        assert root["metrics"]["storage_setup_action_hidden"] is True
        assert root["metrics"]["storage_root_action_count"] == 2
        expected_root_summary = (
            "SD + internal"
            if any(value in ("SD + internal backup", "SD card") for value in expected_data.values())
            else "Internal"
        )
        assert root["metrics"]["storage_root_location_summary"] == expected_root_summary
        assert expected_root_summary in labels["storage_setup_sheet"]
        assert card["metrics"]["storage_card_field_count"] == 5
        assert data["metrics"]["storage_data_row_count"] == 6
        assert data["metrics"]["storage_data_visible_row_count"] == 5
        assert data["metrics"]["storage_data_scroll_enabled"] is True
        assert data["metrics"]["storage_data_content_fits"] is False
        assert data["metrics"]["storage_data_content_height"] > data["metrics"]["storage_data_viewport_height"]
        assert data["metrics"]["storage_data_scroll_range_px"] > 0
        assert {row["title"]: row["value"] for row in data["metrics"]["storage_data_rows"]} == expected_data

        assert_pairwise_disjoint(root["metrics"]["storage_root_regions"])
        assert_pairwise_disjoint(card["metrics"]["storage_card_row_boxes"])
        for row_box in card["metrics"]["storage_card_row_boxes"]:
            assert_box_inside(row_box, card["metrics"]["storage_card_panel"])
        assert_pairwise_disjoint(data["metrics"]["storage_data_virtual_row_boxes"])
        for row_box in data["metrics"]["storage_data_visible_row_boxes"]:
            assert_box_inside(row_box, data["metrics"]["storage_data_viewport"])
        safety_boxes = {tuple(views[name]["metrics"]["storage_safety_box"]) for name in ui_simulator.STORAGE_HIERARCHY_VIEWS}
        assert safety_boxes == {(16, 424, 464, 468)}
        for name in ui_simulator.STORAGE_HIERARCHY_VIEWS:
            view = views[name]
            assert view["overflow"] == [], (scenario, name)
            assert view["truncated_labels"] == [], (scenario, name)
            assert view["sibling_text_overlaps"] == [], (scenario, name)
            assert all(target["width"] >= ui_simulator.MIN_TOUCH_TARGET for target in view["touch_targets"])
            assert all(target["height"] >= ui_simulator.MIN_TOUCH_TARGET for target in view["touch_targets"])
            assert all(target["formats_sd"] is False for target in view["touch_targets"])

        if scenario in ("storage-probe-error", "storage-media-error", "storage-bridge-reported"):
            assert any(label.endswith("SD Needs attention") for label in labels["storage_setup_sheet"])
            parity_report = ui_simulator.generate(
                tmp_path / f"{scenario}-menu-parity",
                views=("home", "settings_storage_maps_expanded"),
                scenario=scenario,
            )
            parity_labels = {view["name"]: set(view["labels"]) for view in parity_report["views"]}
            assert "Check" in parity_labels["home"]
            home_metrics = next(
                view["metrics"] for view in parity_report["views"]
                if view["name"] == "home"
            )
            assert "SD: Check" in home_metrics["home_status_semantic_labels"]
            assert "Attention: Check" in home_metrics["home_status_semantic_labels"]
            assert "Needs attention" in parity_labels["settings_storage_maps_expanded"]
            assert any(
                label.endswith("SD Needs attention")
                for label in parity_labels["settings_storage_maps_expanded"]
            )

        if scenario == "storage-ready-map-tiles-sd":
            map_report = ui_simulator.generate(tmp_path / f"{scenario}-map", views=("map",), scenario=scenario)
            map_view = map_report["views"][0]
            assert map_report["ok"] is True
            assert map_view["metrics"]["map_tile_cache_ready"] is True
            assert map_view["metrics"]["map_tile_download_supported"] is False
            assert map_view["metrics"]["map_tile_render_supported"] is False
            assert "Set a location" in set(map_view["labels"])
            assert "(c) OpenStreetMap contributors" in set(map_view["labels"])
            assert "sd_map_tiles_ready" not in set(map_view["labels"])


def test_storage_card_media_state_filesystem_and_readiness_mappings():
    base = ui_simulator.sample_snapshot()
    assert ui_simulator.storage_card_state(base) == "Card reader starting"
    assert ui_simulator.storage_card_readiness(base) == "Not available"
    assert ui_simulator.storage_filesystem_label(base) == "Not available"

    unavailable = replace(base, storage_setup_action="bridge_unavailable")
    assert ui_simulator.storage_card_state(unavailable) == "Card reader unavailable"
    assert ui_simulator.storage_card_readiness(unavailable) == "Not available"

    no_card = ui_simulator.storage_no_card_snapshot()
    assert ui_simulator.storage_card_state(no_card) == "No card inserted"
    assert ui_simulator.storage_card_readiness(no_card) == "No card"

    fat32_required = ui_simulator.storage_needs_fat32_snapshot()
    assert ui_simulator.storage_card_state(fat32_required) == "FAT32 card required"
    assert ui_simulator.storage_filesystem_label(fat32_required) == "FAT32 required"
    assert ui_simulator.storage_card_readiness(fat32_required) == "Needs FAT32"
    backup_reformat = replace(fat32_required, storage_setup_action="backup_reformat_fat32_on_computer")
    assert ui_simulator.storage_friendly_state(backup_reformat)[2] == "Prepare as FAT32, then reinsert the card."

    checking = replace(
        fat32_required,
        storage_setup_action="wait_for_storage_mount",
    )
    assert ui_simulator.storage_card_state(checking) == "Checking card"
    assert ui_simulator.storage_card_readiness(checking) == "Checking"

    preparing = ui_simulator.storage_root_missing_snapshot()
    assert ui_simulator.storage_card_state(preparing) == "Preparing DeskOS folders"
    assert ui_simulator.storage_card_readiness(preparing) == "Setup incomplete"

    ready = ui_simulator.storage_ready_pending_migration_snapshot()
    assert ui_simulator.storage_card_state(ready) == "Ready"
    assert ui_simulator.storage_card_readiness(ready) == "Ready"
    assert ui_simulator.storage_filesystem_label(ready) == "FAT32"
    assert ui_simulator.storage_filesystem_label(replace(ready, storage_sd_filesystem="fatfs")) == "FAT32"
    assert ui_simulator.storage_filesystem_label(
        replace(ready, storage_sd_mounted=False, storage_sd_data_root_ready=False, storage_sd_filesystem="exfat")
    ) == "exFAT (not supported)"
    assert ui_simulator.storage_filesystem_label(
        replace(ready, storage_sd_data_root_ready=False, storage_sd_filesystem="unknown")
    ) == "Detected"

    detected_not_ready = replace(
        base,
        storage_sd_present=True,
        storage_setup_action="not_available",
    )
    assert ui_simulator.storage_card_state(detected_not_ready) == "Detected - not ready"
    assert ui_simulator.storage_card_readiness(detected_not_ready) == "Not ready"
    assert ui_simulator.storage_filesystem_label(detected_not_ready) == "Not available"
    assert ui_simulator.storage_card_readiness(replace(base, storage_setup_action="not_available")) == "Not ready"

    mount_error = ui_simulator.storage_mount_error_snapshot()
    probe_error = ui_simulator.storage_probe_error_snapshot()
    assert ui_simulator.storage_menu_status(mount_error) == "Needs attention"
    assert ui_simulator.storage_menu_status(probe_error) == "Needs attention"
    assert ui_simulator.storage_menu_status(
        replace(
            ui_simulator.storage_ready_pending_migration_snapshot(),
            storage_sd_state="error",
        )
    ) == "Needs attention"
    assert ui_simulator.storage_menu_status(
        replace(base, storage_setup_action="not_available", storage_sd_state="bridge_reported")
    ) == "Needs attention"

    map_only = ui_simulator.storage_ready_map_only_sd_snapshot()
    export_only = ui_simulator.storage_ready_export_only_sd_snapshot()
    assert map_only.storage_data_enabled is False
    assert export_only.storage_data_enabled is False
    assert map_only.storage_sd_data_root_ready is True
    assert export_only.storage_sd_data_root_ready is True
    assert ui_simulator.storage_menu_status(map_only) == "Ready"
    assert ui_simulator.storage_menu_status(export_only) == "Ready"
    assert ui_simulator.storage_menu_status(
        replace(map_only, storage_retained_sd_degraded=True)
    ) == "Needs attention"

    for media_error in (
        ui_simulator.storage_media_error_snapshot(),
        ui_simulator.storage_bridge_reported_snapshot(),
    ):
        assert media_error.storage_setup_action == "not_available"
        assert ui_simulator.storage_needs_attention(media_error) is True
        assert ui_simulator.storage_menu_status(media_error) == "Needs attention"
        assert ui_simulator.storage_friendly_state(media_error)[0] == "Card needs attention"
        assert ui_simulator.storage_card_menu_status(media_error) == "Needs attention"
        assert ui_simulator.storage_card_readiness(media_error) == "Needs attention"
        assert ui_simulator.storage_card_state(media_error) == "Card needs attention"
    assert ui_simulator.storage_needs_attention(ui_simulator.storage_ready_pending_migration_snapshot()) is False


def test_storage_root_summary_uses_all_six_child_backends():
    pending = ui_simulator.storage_ready_pending_migration_snapshot()
    assert ui_simulator.storage_root_location_summary(pending) == "Internal"
    assert ui_simulator.map_storage_label(pending.map_tile_backend) == "Pending"
    assert ui_simulator.storage_root_location_summary(replace(pending, storage_backend="mixed")) == "Internal"

    retained_only = replace(pending, message_store_backend="sd")
    assert ui_simulator.storage_root_location_summary(retained_only) == "SD + internal"
    assert (
        ui_simulator.retained_storage_label(
            retained_only, retained_only.message_store_backend
        )
        == "SD + internal backup"
    )

    map_only = ui_simulator.storage_ready_map_only_sd_snapshot()
    assert ui_simulator.storage_root_location_summary(map_only) == "SD + internal"
    assert ui_simulator.map_storage_label(map_only.map_tile_backend) == "SD card"
    assert ui_simulator.export_storage_label(map_only.export_backend) == "USB only"

    export_only = ui_simulator.storage_ready_export_only_sd_snapshot()
    assert ui_simulator.storage_root_location_summary(export_only) == "SD + internal"
    assert ui_simulator.map_storage_label(export_only.map_tile_backend) == "Pending"
    assert ui_simulator.export_storage_label(export_only.export_backend) == "SD card"


def test_backup_degradation_is_truthful_without_disabling_healthy_sd_map():
    sd_primary = ui_simulator.storage_backup_degraded_sd_snapshot()
    state, detail, guidance, color = ui_simulator.storage_friendly_state(sd_primary)

    assert state == "SD card ready"
    assert detail == "Saved data is using SD."
    assert guidance == "Internal backup needs attention."
    assert color == ui_simulator.AMBER
    assert ui_simulator.storage_menu_status(sd_primary) == "Needs attention"
    assert ui_simulator.home_storage_status(sd_primary) == "backup issue"
    assert ui_simulator.storage_card_menu_status(sd_primary) == "Ready"
    assert ui_simulator.storage_card_readiness(sd_primary) == "Ready"
    assert ui_simulator.map_storage_summary(sd_primary) == "SD starting"
    assert ui_simulator.storage_root_location_summary(sd_primary) == "SD; backup issue"
    assert (
        ui_simulator.retained_storage_label(
            sd_primary, sd_primary.message_store_backend
        )
        == "SD; backup degraded"
    )
    storage_category = next(
        category
        for category in ui_simulator.more_category_specs(sd_primary)
        if category["key"] == "storage_maps"
    )
    assert storage_category["summary"] == "Backup needs attention"
    assert storage_category["leaves"][0][1] == "Ready"
    assert storage_category["leaves"][0][5] is False

    internal = ui_simulator.storage_backup_degraded_internal_snapshot()
    state, detail, guidance, color = ui_simulator.storage_friendly_state(internal)
    assert state == "Storage needs attention"
    assert detail == "Internal saved-data storage is unavailable."
    assert "USB diagnostics" in guidance
    assert color == ui_simulator.WARNING_TEXT
    assert ui_simulator.storage_root_location_summary(internal) == "Storage issue"
    assert (
        ui_simulator.retained_storage_label(
            internal, internal.message_store_backend
        )
        == "Unavailable"
    )

    combined = replace(sd_primary, storage_retained_sd_degraded=True)
    assert ui_simulator.home_storage_status(combined) == "storage issue"
    combined_category = next(
        category
        for category in ui_simulator.more_category_specs(combined)
        if category["key"] == "storage_maps"
    )
    assert combined_category["summary"] == "Storage needs attention"


def test_storage_warning_propagates_to_collapsed_and_expanded_more_rows(tmp_path):
    snapshot = ui_simulator.storage_degraded_snapshot()
    storage = next(
        category
        for category in ui_simulator.more_category_specs(snapshot)
        if category["key"] == "storage_maps"
    )
    sd_leaf = storage["leaves"][0]

    assert storage["summary"] == "SD needs attention"
    assert storage["warning"] is True
    assert storage["color"] == ui_simulator.WARNING_TEXT
    assert sd_leaf[1] == "Needs attention"
    assert sd_leaf[2] == ui_simulator.RED
    assert sd_leaf[5] is True

    report = ui_simulator.generate(
        tmp_path,
        views=("settings", "settings_storage_maps_expanded"),
        scenario="storage-degraded",
    )
    labels = {view["name"]: set(view["labels"]) for view in report["views"]}
    assert report["ok"] is True
    assert "SD needs attention" in labels["settings"]
    assert "SD needs attention" in labels["settings_storage_maps_expanded"]


def test_ui_simulator_manual_location_scenario_fits(tmp_path):
    report = ui_simulator.generate(tmp_path / "manual-location", views=("map",), scenario="manual-location")
    view = report["views"][0]
    labels = set(view["labels"])

    assert report["ok"] is True
    assert report["overflow_count"] == 0
    assert report["touch_target_issue_count"] == 0
    assert report["required_labels_missing"] == []
    assert {"Waiting for storage", "Options", "(c) OpenStreetMap contributors"} <= labels
    assert view["metrics"]["map_location_set"] is True
    assert view["metrics"]["map_progress_bar_supported"] is True
    assert view["metrics"]["map_progress_bar_visible"] is False
    assert view["metrics"]["map_tile_style"] == "local_dark_osm_standard"
    assert view["metrics"]["map_center_source"] == "manual"
    assert view["metrics"]["map_center_lat_e7"] == 436532000
    assert view["metrics"]["map_center_lon_e7"] == -793832000


def test_ui_simulator_home_map_card_reports_honest_setup_state(tmp_path):
    expected = {
        "default": "Set a location",
        "manual-location": "SD starting",
        "map-location-wifi-off": "Needs Wi-Fi",
        "map-ready": "Ready to open",
    }
    for scenario, status in expected.items():
        report = ui_simulator.generate(
            tmp_path / f"home-{scenario}", views=("home",), scenario=scenario
        )
        labels = set(report["views"][0]["labels"])
        assert report["ok"] is True, scenario
        assert status in labels, scenario
        assert "Map cache ready" not in labels, scenario
        assert "Set up Map" not in labels, scenario


def test_home_status_strip_covers_ready_connecting_absent_and_error_states(tmp_path):
    expected = {
        "home-status-ready": {
            "Mesh: Ready",
            "Wi-Fi: Connected",
            "BLE: On",
            "SD: Ready",
            "Attention: OK",
        },
        "home-status-connecting": {
            "Mesh: Applying",
            "Wi-Fi: Connecting",
            "BLE: Off",
            "SD: Setup",
            "Attention: Notice",
        },
        "home-status-no-card": {
            "Mesh: Ready",
            "Wi-Fi: Off",
            "BLE: Unavailable",
            "SD: No card",
            "Attention: Notice",
        },
        "home-status-error": {
            "Mesh: Error",
            "Wi-Fi: Off",
            "BLE: Unavailable",
            "SD: Check",
            "Attention: Check",
        },
        "home-status-reconnecting": {
            "Mesh: Ready",
            "Wi-Fi: Connected",
            "BLE: On",
            "SD: Reconnect",
            "Attention: Notice",
        },
        "home-status-busy": {
            "Mesh: Busy",
            "Wi-Fi: Connected",
            "BLE: On",
            "SD: Ready",
            "Attention: OK",
        },
    }
    for scenario, semantic_labels in expected.items():
        report = ui_simulator.generate(
            tmp_path / scenario,
            views=("home",),
            scenario=scenario,
        )
        view = report["views"][0]
        status_targets = [
            target for target in view["touch_targets"]
            if target["kind"] == "home_status_item"
        ]
        assert report["ok"] is True, scenario
        assert report["overflow_count"] == 0, scenario
        assert report["touch_target_issue_count"] == 0, scenario
        assert len(status_targets) == 5, scenario
        assert all(target["width"] == 88 for target in status_targets), scenario
        assert all(target["height"] == 84 for target in status_targets), scenario
        assert set(view["metrics"]["home_status_semantic_labels"]) == semantic_labels
        assert {target["semantic_label"] for target in status_targets} == semantic_labels
        assert view["metrics"]["home_status_attention_required"] is (
            scenario == "home-status-error"
        )


def test_home_mesh_status_unknown_ready_substrings_fail_closed():
    base = replace(
        ui_simulator.sample_snapshot(),
        radio_ready=True,
        radio_applied=True,
    )
    for state in ("not_ready", "unready", "listening", "READY"):
        label, color, attention = ui_simulator.home_mesh_status(
            replace(base, mesh_state=state)
        )
        assert label == "Starting", state
        assert color == ui_simulator.AMBER, state
        assert attention is False, state
    assert ui_simulator.home_mesh_status(
        replace(base, mesh_state="waiting_for_radio")
    ) == ("Offline", ui_simulator.MUTED, False)
    assert ui_simulator.home_mesh_status(
        replace(base, mesh_state="tx_busy")
    ) == ("Busy", ui_simulator.GREEN, False)
    for radio_ready, radio_applied in ((False, False), (False, True), (True, False)):
        assert ui_simulator.home_mesh_status(
            replace(
                base,
                mesh_state="tx_busy",
                radio_ready=radio_ready,
                radio_applied=radio_applied,
            )
        ) == ("Starting", ui_simulator.AMBER, False)


def test_map_storage_copy_state_table_distinguishes_starting_absent_fat32_and_error():
    base = ui_simulator.manual_location_snapshot()
    cases = (
        (
            replace(base, storage_setup_action="bridge_protocol_pending"),
            "SD starting",
            "Waiting for storage",
        ),
        (
            replace(base, storage_setup_action="wait_for_storage_reconnect"),
            "SD starting",
            "Waiting for storage",
        ),
        (
            replace(base, storage_setup_action="insert_card"),
            "Insert SD",
            "SD card required",
        ),
        (
            replace(
                base,
                storage_setup_action="prepare_fat32_on_computer",
                storage_sd_present=True,
                storage_sd_needs_fat32=True,
            ),
            "Needs FAT32",
            "FAT32 card required",
        ),
        (
            replace(base, storage_sd_state="error"),
            "Check SD",
            "Map unavailable",
        ),
    )
    for snapshot, summary, viewport_title in cases:
        assert ui_simulator.map_storage_summary(snapshot) == summary
        assert ui_simulator.map_view_status(snapshot)[0] == viewport_title

    reconnecting = replace(
        base,
        storage_setup_action="wait_for_storage_reconnect",
        storage_data_enabled=True,
    )
    assert ui_simulator.storage_menu_status(reconnecting) == "Reconnecting"
    assert ui_simulator.storage_card_menu_status(reconnecting) == "Reconnecting"
    assert ui_simulator.storage_card_readiness(reconnecting) == "Reconnecting"
    assert ui_simulator.storage_card_state(reconnecting) == "Card reader reconnecting"
    assert ui_simulator.storage_friendly_state(reconnecting)[1:3] == (
        "Last confirmed SD remains active briefly.",
        "Internal fallback takes over if status retries fail.",
    )
    reconnecting_fallback = replace(reconnecting, storage_data_enabled=False)
    assert ui_simulator.storage_friendly_state(reconnecting_fallback)[1:3] == (
        "Internal storage is active.",
        "SD access resumes after a valid status reply.",
    )


def test_settings_map_status_matches_location_storage_wifi_and_renderer_priority():
    base = ui_simulator.manual_location_snapshot()
    cases = (
        (replace(base, map_location_set=False), "Set location"),
        (replace(base, storage_setup_action="bridge_protocol_pending"), "SD starting"),
        (replace(base, storage_setup_action="insert_card"), "Insert SD"),
        (
            replace(
                base,
                storage_setup_action="prepare_fat32_on_computer",
                storage_sd_needs_fat32=True,
            ),
            "Needs FAT32",
        ),
        (replace(base, storage_sd_state="error"), "Check SD"),
        (
            replace(base, map_tile_cache_ready=True, wifi_connected=False),
            "Needs Wi-Fi",
        ),
        (
            replace(
                base,
                map_tile_cache_ready=True,
                wifi_connected=True,
                map_tile_render_supported=False,
            ),
            "Loading",
        ),
        (
            replace(
                base,
                map_tile_cache_ready=True,
                wifi_connected=True,
                map_tile_render_supported=True,
            ),
            "Ready",
        ),
    )
    for snapshot, expected in cases:
        assert ui_simulator.settings_map_status(snapshot) == expected
        storage_category = next(
            category
            for category in ui_simulator.more_category_specs(snapshot)
            if category["key"] == "storage_maps"
        )
        assert storage_category["leaves"][1][1] == expected


def test_ui_simulator_map_hierarchy_is_simple_friendly_and_bounded(tmp_path):
    hierarchy_views = ("map", "map_options", "map_location", "map_cache")
    report = ui_simulator.generate(
        tmp_path / "ready-map", views=hierarchy_views, scenario="map-ready"
    )
    views = {view["name"]: view for view in report["views"]}
    labels = {name: set(view["labels"]) for name, view in views.items()}
    actions = {
        name: {target["action"]: target for target in view["touch_targets"]}
        for name, view in views.items()
    }

    assert report["ok"] is True
    assert report["overflow_count"] == 0
    assert report["touch_target_issue_count"] == 0
    assert report["required_labels_missing"] == []
    assert views["map"]["metrics"]["map_hierarchy_level"] == "actual_view"
    assert views["map"]["metrics"]["map_actual_view"] is True
    assert views["map"]["metrics"]["map_landing_action_count"] == 1
    assert views["map"]["metrics"]["map_view_control_count"] == 3
    assert views["map"]["metrics"]["map_pan_gesture"] is True
    assert views["map"]["metrics"]["map_full_bleed_content"] is True
    assert views["map"]["metrics"]["map_local_header_height"] == 0
    assert views["map"]["metrics"]["map_controls_overlay_canvas"] is True
    assert views["map"]["metrics"]["map_min_control_target"] == 48
    assert views["map"]["metrics"]["map_viewport"] == [0, ui_simulator.TOP_BAR_H, 480, ui_simulator.DOCK_Y]
    assert views["map"]["metrics"]["map_default_zoom"] == 10
    assert views["map"]["metrics"]["map_min_zoom"] == 8
    assert views["map"]["metrics"]["map_max_zoom"] == 14
    assert views["map"]["metrics"]["map_same_view_frame_reuse"] is True
    assert views["map"]["metrics"]["map_visible_tile_limit"] == 9
    assert views["map"]["metrics"]["map_zoom_batch_count"] == 1
    assert views["map"]["metrics"]["map_cached_tile_count"] == 9
    assert views["map"]["metrics"]["map_interactive_request_eligible"] is True
    assert views["map"]["metrics"]["map_attribution_visible"] is True
    assert views["map_options"]["metrics"]["map_hierarchy_level"] == "options"
    assert views["map_options"]["metrics"]["map_options_action_count"] == 2
    assert "SD ready" in labels["map_options"]
    assert "Local area saved" not in labels["map_options"]
    assert views["map_cache"]["metrics"]["map_cache_read_only"] is True

    assert {"map_recenter", "map_zoom_out", "map_zoom_in"}.issubset(actions["map"])
    for action in ("map_recenter", "map_zoom_out", "map_zoom_in"):
        target = actions["map"][action]
        assert target["width"] >= 48
        assert target["height"] >= 48
        assert target["rf_tx"] is False
        assert target["formats_sd"] is False
    assert {"Center", "Drag to pan", "z10", "-", "+"}.issubset(labels["map"])

    assert_pairwise_disjoint(views["map_options"]["metrics"]["map_options_regions"])
    assert_pairwise_disjoint(views["map_cache"]["metrics"]["map_cache_row_boxes"])
    for row_box in views["map_cache"]["metrics"]["map_cache_row_boxes"]:
        assert_box_inside(row_box, views["map_cache"]["metrics"]["map_cache_panel"])

    all_labels = set().union(*(labels[name] for name in hierarchy_views))
    for raw_token in (
        "provider",
        "sd_map_tiles_ready",
        "offline",
        "Tile source",
        "Remove source",
        "tile_render_pending",
        "map/tiles/z{z}/x{x}/y{y}.tile",
        "serial canaries",
    ):
        assert all(raw_token not in label for label in all_labels), raw_token
    assert not any("download" in action for view_actions in actions.values() for action in view_actions)
    assert not any(target["formats_sd"] for view in views.values() for target in view["touch_targets"])
    assert not any(target["rf_tx"] for view in views.values() for target in view["touch_targets"])

    for page_name in ui_simulator.MAP_HIERARCHY_VIEWS:
        page = views[page_name]
        assert page["dock_rendered"] is False
        assert page["truncated_labels"] == []
        assert page["sibling_text_overlaps"] == []
        for target in page["touch_targets"]:
            x0, y0, x1, y1 = target["visual_box"]
            assert x1 - x0 >= ui_simulator.MIN_TOUCH_TARGET, (page_name, target["label"])
            assert y1 - y0 >= ui_simulator.MIN_TOUCH_TARGET, (page_name, target["label"])

    assert actions["map"]["open_map_options"]["destination"] == "map_options"
    assert actions["map_options"]["open_map_location"]["destination"] == "map_location"
    assert actions["map_location"]["close_map_location"]["destination"] == "map_options"
    assert actions["map_options"]["open_map_cache"]["destination"] == "map_cache"
    assert actions["map_cache"]["close_map_cache"]["destination"] == "map_options"


def test_ui_simulator_built_in_map_states_are_deterministic_and_policy_bounded(tmp_path):
    hierarchy_views = ("map", "map_options", "map_location", "map_cache")
    first = ui_simulator.generate(
        tmp_path / "map-ready-first",
        views=hierarchy_views,
        scenario="map-ready",
    )
    second = ui_simulator.generate(
        tmp_path / "map-ready-second",
        views=hierarchy_views,
        scenario="map-ready",
    )
    first_views = {view["name"]: view for view in first["views"]}
    second_views = {view["name"]: view for view in second["views"]}

    assert first["ok"] is True
    assert second["ok"] is True
    for name in hierarchy_views:
        assert Path(first_views[name]["screenshot"]).read_bytes() == Path(second_views[name]["screenshot"]).read_bytes(), name

    cache_rows = {row["title"]: row["value"] for row in first_views["map_cache"]["metrics"]["map_cache_rows"]}
    assert cache_rows == {
        "Wi-Fi": "Connected",
        "SD card": "Ready",
        "Location": "Saved",
        "Map view": "Local area saved",
    }
    for view in first_views.values():
        assert view["metrics"].get("map_probe_network_allowed") is False
        assert view["metrics"].get("map_background_download") is False
        assert view["metrics"].get("map_area_download") is False


def test_ui_simulator_map_markers_are_named_bounded_and_open_node_detail(tmp_path):
    report = ui_simulator.generate(
        tmp_path / "map-markers", views=("map", "node_detail_sheet"), scenario="map-ready"
    )
    views = {view["name"]: view for view in report["views"]}
    map_view = views["map"]
    detail = views["node_detail_sheet"]

    assert report["ok"] is True
    assert map_view["metrics"]["map_marker_query_limit"] == 32
    assert map_view["metrics"]["map_marker_display_limit"] == 8
    assert map_view["metrics"]["map_marker_query_count"] == 3
    assert map_view["metrics"]["map_marker_displayed_count"] == 3
    assert set(map_view["metrics"]["map_marker_full_names"]) == {
        "YKF Corebot",
        "YKF Room",
        "Krabs Lagoon Repeater",
    }
    assert set(map_view["metrics"]["map_marker_names"]) == {
        "YKF Corebot",
        "YKF Room",
        "Krabs Lagoo...",
    }
    assert map_view["metrics"]["map_marker_name_max_chars"] == 14
    assert map_view["metrics"]["map_marker_truncated_count"] == 1
    assert map_view["metrics"]["map_marker_labels_below"] is True
    assert map_view["metrics"]["map_marker_overlay_separate"] is True
    assert map_view["metrics"]["map_marker_refresh_rebuilds_tiles"] is False
    assert map_view["metrics"]["map_marker_hit_diameter_px"] == 44
    assert map_view["metrics"]["map_marker_source"] == "signed_advert_location"
    assert map_view["metrics"]["map_marker_provenance_verified"] is True
    assert map_view["metrics"]["map_marker_age_verified"] is True
    assert map_view["metrics"]["map_marker_role_labels"] == [
        "Companion",
        "Room Server",
        "Repeater",
    ]
    assert map_view["metrics"]["map_marker_age_seconds"] == [60, 120, 300]
    assert map_view["metrics"]["map_marker_age_labels"] == ["1m", "2m", "5m"]
    assert map_view["metrics"]["map_marker_precision_claim"] == "accuracy_unknown"
    assert map_view["metrics"]["map_center_source"] == "manual"
    assert map_view["metrics"]["map_center_provenance_explicit"] is True
    assert map_view["metrics"]["map_center_trust_required_for_initial_acquire"] is True
    assert map_view["metrics"]["map_center_trust_required_for_interactive_acquire"] is True
    assert map_view["metrics"]["map_center_trust_required_for_reacquire"] is True
    assert map_view["metrics"]["map_trust_loss_invalidates_retained_view"] is True
    assert map_view["metrics"]["map_backward_time_rechecks_future_pins"] is True
    assert map_view["metrics"]["map_pin_truth_legend"] == (
        "Signed advert E6\nage verified\naccuracy unknown"
    )
    assert map_view["metrics"]["map_saved_center_pin"] == "omitted"
    assert len(set(map_view["metrics"]["map_marker_colors"])) == 3
    assert "#F87171" not in map_view["metrics"]["map_marker_colors"]
    assert set(map_view["metrics"]["map_marker_colors"]) == {
        "#2DD4BF",
        "#FACC15",
        "#C084FC",
    }
    assert_pairwise_disjoint(map_view["metrics"]["map_marker_bounds"])
    for marker_box in map_view["metrics"]["map_marker_bounds"]:
        for exclusion_box in map_view["metrics"]["map_marker_exclusion_boxes"]:
            assert boxes_overlap(marker_box, exclusion_box) is False
    marker_targets = [
        target for target in map_view["touch_targets"]
        if target["kind"] == "map_marker_hit"
    ]
    assert len(marker_targets) == 3
    assert all(target["width"] == 44 and target["height"] == 44 for target in marker_targets)
    assert all(target["action"] == "open_map_node_detail" for target in marker_targets)
    assert all(target["destination"] == "node_detail_sheet" for target in marker_targets)
    assert all(target["rf_tx"] is False and target["formats_sd"] is False for target in marker_targets)
    assert "Krabs Lagoo..." in map_view["labels"]
    assert "Advert location" in detail["labels"]
    assert detail["metrics"]["node_detail_location_provenance"] == "advert"
    assert detail["metrics"]["node_detail_advert_location"] == "43.675000, -79.440000"
    assert detail["metrics"]["node_detail_dm_available"] is True
    assert detail["metrics"]["node_detail_management_gated"] is False
    dm_targets = [
        target for target in detail["touch_targets"]
        if target["action"] == "open_node_dm"
    ]
    assert len(dm_targets) == 1
    assert dm_targets[0]["destination"] == "compose_dm_sheet"
    assert detail["metrics"]["node_detail_return_destination"] == "map"
    assert detail["metrics"]["node_detail_return_reuses_map_view"] is True


def test_ui_simulator_map_markers_fail_closed_without_verified_age():
    surface = ui_simulator.Surface("map")
    snapshot = replace(
        ui_simulator.map_ready_snapshot(),
        map_marker_age_reference_valid=False,
        map_marker_reference_timestamp=0,
    )
    ui_simulator.render_map(surface, snapshot)

    assert surface.metrics["map_marker_query_count"] == 3
    assert surface.metrics["map_marker_displayed_count"] == 0
    assert surface.metrics["map_marker_age_verified"] is False
    assert surface.metrics["map_pin_truth_legend"] == (
        "Signed advert E6\npins hidden\nage unverified"
    )


def test_ui_simulator_map_markers_fail_closed_for_unknown_location_provenance():
    surface = ui_simulator.Surface("map")
    ready = ui_simulator.map_ready_snapshot()
    snapshot = replace(
        ready,
        heard=tuple(
            replace(node, location_provenance="unknown")
            for node in ready.heard
        ),
    )
    ui_simulator.render_map(surface, snapshot)

    assert surface.metrics["map_marker_query_count"] == 3
    assert surface.metrics["map_marker_displayed_count"] == 0
    assert surface.metrics["map_marker_provenance_verified"] is False
    assert surface.metrics["map_marker_age_verified"] is True


def test_ui_simulator_backward_trusted_time_rechecks_and_removes_future_pins():
    surface = ui_simulator.Surface("map")
    snapshot = replace(
        ui_simulator.map_ready_snapshot(),
        map_marker_reference_timestamp=1_800,
    )
    ui_simulator.render_map(surface, snapshot)

    assert surface.metrics["map_marker_query_count"] == 3
    assert surface.metrics["map_marker_displayed_count"] == 1
    assert surface.metrics["map_marker_full_names"] == ["Krabs Lagoon Repeater"]
    assert surface.metrics["map_marker_age_seconds"] == [100]
    assert surface.metrics["map_marker_age_labels"] == ["1m"]


def test_ui_simulator_unknown_center_provenance_invalidates_all_acquire_paths():
    surface = ui_simulator.Surface("map")
    snapshot = replace(
        ui_simulator.map_ready_snapshot(),
        map_center_source="unknown",
    )
    ui_simulator.render_map(surface, snapshot)

    assert "Center unavailable" in surface.labels
    assert surface.metrics["map_frame_ready"] is False
    assert surface.metrics["map_view_control_count"] == 0
    assert surface.metrics["map_pan_gesture"] is False
    assert surface.metrics["map_interactive_request_eligible"] is False
    assert surface.metrics["map_center_provenance_explicit"] is False
    assert surface.metrics["map_marker_displayed_count"] == 0


def test_ui_simulator_map_downloading_shows_determinate_progress(tmp_path):
    report = ui_simulator.generate(
        tmp_path / "map-downloading", views=("map",), scenario="map-downloading"
    )
    view = report["views"][0]

    assert report["ok"] is True
    assert "Downloading 3/9" in view["labels"]
    assert view["metrics"]["map_frame_ready"] is False
    assert view["metrics"]["map_progress_bar_supported"] is True
    assert view["metrics"]["map_progress_bar_visible"] is True
    assert view["metrics"]["map_progress_completed"] == 3
    assert view["metrics"]["map_progress_total"] == 9
    assert view["metrics"]["map_marker_displayed_count"] == 0


def test_ui_simulator_map_setup_connection_live_and_cached_states(tmp_path):
    expected = {
        "default": ("Set a location", False, 0, False),
        "map-location-wifi-off": ("Wi-Fi needed", False, 0, False),
        "map-wifi-connecting": ("Connecting to Wi-Fi", False, 0, False),
        "map-ready": (None, True, 9, True),
        "map-cached-revisit": (None, False, 9, True),
    }
    for scenario, (status, eligible, cached, frame_ready) in expected.items():
        report = ui_simulator.generate(
            tmp_path / scenario, views=("map",), scenario=scenario
        )
        view = report["views"][0]
        assert report["ok"] is True, scenario
        if status is not None:
            assert status in set(view["labels"]), scenario
        assert ("Live view" in set(view["labels"])) is False, scenario
        assert ("Cached view" in set(view["labels"])) is False, scenario
        assert ("Z12" in set(view["labels"])) is False, scenario
        assert ("9/9 cached" in set(view["labels"])) is False, scenario
        assert view["metrics"]["map_frame_ready"] is frame_ready
        assert view["metrics"]["map_interactive_request_eligible"] is eligible
        assert view["metrics"]["map_cached_tile_count"] == cached
        assert view["metrics"]["map_visible_tile_limit"] == 9
        assert view["metrics"]["map_zoom_batch_count"] == 1
        assert view["metrics"]["map_attribution"] == "(c) OpenStreetMap contributors"


def test_ui_simulator_unsaved_map_location_fields_start_blank(tmp_path):
    report = ui_simulator.generate(
        tmp_path / "unsaved-location", views=("map_location",), scenario="default"
    )
    view = report["views"][0]

    assert report["ok"] is True
    assert view["metrics"]["map_location_latitude_value"] == ""
    assert view["metrics"]["map_location_longitude_value"] == ""
    assert view["metrics"]["map_location_examples_are_placeholders_only"] is True
    assert "e.g. 43.6532000" in view["labels"]
    assert "e.g. -79.3832000" in view["labels"]


def test_channel_surfaces_partition_rows_and_fail_closed_exactly():
    snap = ui_simulator.sample_snapshot()

    public = ui_simulator.Surface("messages_public")
    ui_simulator.render_messages_public(public, snap)
    private = ui_simulator.Surface("messages_channel_private")
    ui_simulator.render_messages_channel_private(private, snap)

    assert public.metrics["active_channel_id"] == 1
    assert public.metrics["active_channel_source_count"] == 3
    assert private.metrics["active_channel_id"] == 2
    assert private.metrics["active_channel_source_count"] == 2
    assert "private channel row stays isolated" not in public.labels
    assert "ops-only acknowledgement" not in public.labels
    assert "Public test reply received" not in private.labels
    assert "test ack on Public" not in private.labels
    assert next(
        target for target in private.touch_targets
        if target["action"] == "mark_channel_read"
    )["rf_tx"] is False

    selector = ui_simulator.Surface("channel_selector_sheet")
    ui_simulator.render_channel_selector_sheet(selector, snap)
    assert selector.metrics["channel_selector_capacity"] == 8
    assert selector.metrics["channel_selector_row_height"] == 44
    assert selector.metrics["channel_selector_displays_secret"] is False
    assert selector.metrics["channel_selector_navigation_rf_silent"] is True
    assert [row["channel_id"] for row in selector.metrics["channel_selector_channels"]] == [1, 2, 3]
    selector_rows = [
        target for target in selector.touch_targets
        if target["kind"] == "channel_row"
    ]
    assert all(target["height"] >= 44 for target in selector_rows)
    disabled = next(
        target for target in selector_rows
        if target["action"] == "select_channel_3"
    )
    assert disabled["enabled"] is False
    assert disabled["destination"] is None
    assert disabled["rf_tx"] is False

    private_selector = ui_simulator.Surface("channel_selector_private_sheet")
    ui_simulator.render_channel_selector_private_sheet(private_selector, snap)
    close_selector = next(
        target for target in private_selector.touch_targets
        if target["action"] == "close_channel_selector"
    )
    assert close_selector["destination"] == "messages_channel_private"

    history = ui_simulator.Surface("channel_history_private_sheet")
    ui_simulator.render_channel_history_private_sheet(history, snap)
    assert history.metrics["channel_history_channel_id"] == 2
    assert history.metrics["channel_history_source_count"] == 2
    assert history.metrics["public_history_source_count"] == 3
    close_history = next(
        target for target in history.touch_targets
        if target["action"] == "close_public_history"
    )
    assert close_history["destination"] == "messages_channel_private"

    search = ui_simulator.Surface("channel_search_private_sheet")
    ui_simulator.render_channel_search_private_sheet(search, snap)
    assert search.metrics["channel_search_channel_id"] == 2
    assert search.metrics["channel_search_source_count"] == 2

    compose = ui_simulator.Surface("compose_channel_private_sheet")
    ui_simulator.render_compose_channel_private_sheet(compose, snap)
    send = next(
        target for target in compose.touch_targets
        if target["action"] == "send_channel_text"
    )
    assert compose.metrics["compose_channel_id"] == 2
    assert compose.metrics["compose_send_enabled"] is True
    assert send["enabled"] is True
    assert send["rf_tx"] is True

    disabled_compose = ui_simulator.Surface("compose_channel_disabled_sheet")
    ui_simulator.render_compose_channel_disabled_sheet(disabled_compose, snap)
    disabled_send = next(
        target for target in disabled_compose.touch_targets
        if target["action"] == "send_channel_text"
    )
    assert disabled_compose.metrics["compose_channel_id"] == 3
    assert disabled_compose.metrics["compose_eligibility_reason"] == "channel_not_sendable"
    assert disabled_compose.metrics["compose_send_enabled"] is False
    assert disabled_send["enabled"] is False
    assert disabled_send["rf_tx"] is False


def test_ui_simulator_is_documented_and_run_in_ci():
    workflow = read(".github/workflows/d1l-ci.yml")
    host_lock = read("requirements/ci-host-windows.txt")
    build_inputs = json.loads(read(".github/d1l-build-inputs.json"))
    test_plan = read("docs/TEST_PLAN_D1L.md")
    roadmap = read("docs/ROADMAP.md")
    checklist = read("docs/RELEASE_CHECKLIST.md")

    assert "Pillow==12.3.0" in host_lock
    assert "--require-hashes" in host_lock
    assert build_inputs["host_python"]["requirements"]["path"] == (
        "requirements/ci-host-windows.txt"
    )
    assert ".github/d1l-build-inputs.json" in workflow
    assert "python -m pip install --disable-pip-version-check --require-hashes" in workflow
    assert "python ./tools/ui_simulator.py --out artifacts/ui-sim" in workflow
    assert "python ./tools/ui_simulator.py --scenario large-mesh --out artifacts/ui-sim-large" in workflow
    assert "python ./tools/ui_simulator.py --scenario storage-states --out artifacts/ui-sim-storage" in workflow
    assert "python ./tools/ui_simulator.py --scenario map-ready --view map --view map_options --view map_location --view map_cache --out artifacts/ui-sim-map-ready" in workflow
    assert "python .\\tools\\ui_simulator.py --out artifacts\\ui-sim" in test_plan
    assert "python .\\tools\\ui_simulator.py --scenario large-mesh --out artifacts\\ui-sim-large" in test_plan
    assert "python .\\tools\\ui_simulator.py --scenario map-ready --view map --view map_options --view map_location --view map_cache --out artifacts\\ui-sim-map-ready" in test_plan
    assert "tools/ui_simulator.py" in roadmap
    assert "large-mesh" in roadmap
    assert "Simulator screenshots captured" in checklist
    assert "Large simulated mesh UI stress passes" in checklist
