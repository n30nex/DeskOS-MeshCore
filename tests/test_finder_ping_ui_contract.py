from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_zero_hop_discovery_is_correlated_bounded_and_not_promoted() -> None:
    wire = read("main/mesh/meshcore_wire.h")
    semantics = read("main/mesh/meshcore_packet_semantics.c")
    service = read("main/mesh/meshcore_service.c")

    assert "D1L_MESHCORE_PAYLOAD_CONTROL 0x0BU" in wire
    assert "D1L_MESHCORE_PACKET_SEMANTIC_CONTROL" in semantics
    assert "packet->route != D1L_MESHCORE_ROUTE_DIRECT" in semantics
    assert "packet->path_hops != 0U" in semantics
    assert "d1l_meshcore_discovery_build_request(tag, request)" in service
    assert "D1L_MESHCORE_DISCOVERY_FILTER_ALL" in read(
        "main/mesh/meshcore_discovery.h"
    )
    assert "s_discovery_tag" in service
    assert "discovery_session_active_locked" in service
    assert "d1l_meshcore_discovery_parse_response(" in service
    assert "D1L_MESHCORE_DISCOVERY_MAX_RESULTS" in service
    assert '"node_discover_req"' in service
    assert '"node_discover_resp"' in service
    receive = service.split("static void parse_rx_discovery_packet", 1)[1].split(
        "static void parse_rx_trace_packet", 1
    )[0]
    assert "d1l_contact_store_upsert" not in receive
    assert "d1l_node_store_upsert" not in receive


def test_nodes_expose_finder_and_confirmed_retained_clear() -> None:
    nodes_header = read("main/ui/ui_nodes.h")
    nodes = read("main/ui/ui_nodes.c")
    phase1 = read("main/ui/ui_phase1.c")
    app = read("main/app/app_model.c")

    assert "D1L_UI_NODES_ACTION_FIND_NEARBY" in nodes_header
    assert "D1L_UI_NODES_ACTION_CLEAR_HEARD" in nodes_header
    assert 'nodes_create_button(summary, "Find"' in nodes
    assert 'nodes_create_button(summary, "Clear"' in nodes
    assert '"Find Nearby"' in phase1
    assert "Zero-hop RF only." in phase1
    assert "Discovery keys are unverified until a signed advert is received." in phase1
    assert "d1l_app_model_find_contact_by_public_key(" in phase1
    assert "d1l_app_model_discover_nearby()" in phase1
    assert "d1l_app_model_clear_nodes(true)" in phase1
    assert "Tap Clear again to erase heard nodes" in phase1
    assert "if (!confirmed)" in app
    assert "d1l_node_store_clear()" in app
    assert "d1l_meshcore_service_clear_discovery_results()" in app


def test_repeater_ping_and_trace_results_are_visible() -> None:
    service = read("main/mesh/meshcore_service.c")
    phase1 = read("main/ui/ui_phase1.c")
    console = read("main/comms/usb_console.c")

    ping = service.split(
        "static esp_err_t meshcore_service_handle_send_trace_contact", 1
    )[1].split("static esp_err_t meshcore_service_handle_admin_login", 1)[0]
    assert "cmd->trace_zero_hop_ping" in ping
    assert 'strcmp(contact.type, "repeater") != 0' in ping
    assert "plan.path_hops = 1U" in ping
    assert "memcpy(plan.path_hashes, contact_public_key" in ping
    assert '"ping_request"' in ping
    assert '"Ping"' in phase1
    assert "d1l_app_model_ping_repeater(" in phase1
    assert '"Zero-hop Ping"' in phase1
    assert '"%s pending  %lus elapsed"' in phase1
    assert '"%s reply  RTT %lums  RSSI %d  back SNR %s%d.%02d"' in phase1
    assert '"Hop SNR:"' in phase1
    assert "render_second != s_route_trace_last_render_second" in phase1
    assert "static void cmd_repeater_ping(const char *line)" in console
    assert "d1l_meshcore_service_ping_repeater(fingerprint)" in console
    assert '\\"targeted_trace_rf_tx\\":true' in console
    assert "cmd_trace_status(\"repeater ping status\")" in console
