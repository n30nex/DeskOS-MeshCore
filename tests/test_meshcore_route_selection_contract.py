import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def canonical_sha256(relative: str) -> str:
    payload = (ROOT / relative).read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(payload).hexdigest()


def test_production_dm_send_consumes_one_boot_proven_fail_closed_route_plan():
    service = read("main/mesh/meshcore_service.c")
    selector = read("main/mesh/meshcore_route_selection.h")
    send_dm = service.split(
        "static esp_err_t meshcore_service_handle_send_dm", 1
    )[1].split("static void meshcore_service_reply", 1)[0]
    finalize = service.split(
        "static void finalize_pending_dm_radio_result", 1
    )[1].split("static esp_err_t fail_pending_dm_before_radio", 1)[0]
    wrapper = service.split("esp_err_t d1l_meshcore_service_send_dm", 1)[1].split(
        "esp_err_t d1l_meshcore_service_request_path_discovery_probe", 1
    )[0]
    path_rx = service.split("static void parse_rx_path_packet", 1)[1].split(
        "static bool verify_advert_signature", 1
    )[0]
    initialization = service.split("void d1l_meshcore_service_init", 1)[1].split(
        "esp_err_t d1l_meshcore_service_ensure_identity", 1
    )[0]

    assert '#include "mesh/meshcore_route_selection.h"' in service
    path_state = read("main/mesh/meshcore_path_state.h")
    assert "D1L_MESHCORE_DIRECT_PATH_MAX_AGE_MS (30U * 60U * 1000U)" in path_state
    assert "D1L_MESHCORE_DIRECT_PATH_FAILURE_THRESHOLD 2U" in path_state
    assert "bool learned_this_boot" in selector
    assert "D1L_MESHCORE_ROUTE_SELECTION_FLOOD_PREBOOT_PATH" in selector
    assert "d1l_meshcore_wire_path_len_valid(learned_path_len)" in selector
    assert "selection.path_age_ms >" in selector
    assert "*out_selection = selection;" in selector
    assert "lookup_boot_route(contact.fingerprint, contact.out_path" in send_dm
    assert "contact.out_path_updated_ms" not in send_dm
    assert "route_learned_this_boot" in send_dm
    assert "d1l_contact_store_prepare_path_route(" in send_dm
    assert "contact.out_path_state.generation" in send_dm
    assert "d1l_meshcore_route_select_canonical(" in send_dm
    assert "build_dm_text_packet(settings, &contact, cmd->dm_text, &selection" in send_dm
    assert "0U, tx_timestamp" in send_dm
    assert "const bool use_direct = contact.out_path_valid" not in send_dm
    assert "route_name(s_pending_dm_tx.selection.route)" in finalize
    assert '"dm_text"' in finalize
    assert "record_dm_route_selection(&selection)" in send_dm
    assert "d1l_meshcore_route_selection_reason_name(" in finalize
    assert send_dm.index("record_dm_route_selection(&selection)") > send_dm.index(
        "Radio.SendWithOrigin("
    )
    assert "meshcore_service_send_dm_command(fingerprint, text, false)" in wrapper

    assert "d1l_contact_store_update_path_from_source(" in path_rx
    assert "d1l_meshcore_path_plain_decode(" in path_rx
    assert "decoded.source" in path_rx
    assert "remember_boot_route(" in path_rx
    assert path_rx.index("remember_boot_route(") > path_rx.index(
        "d1l_contact_store_update_path_from_source("
    )
    assert "clear_boot_routes();" in initialization


def test_path_probe_is_a_real_correlated_flood_request():
    service = read("main/mesh/meshcore_service.c")
    contact_header = read("main/mesh/contact_store.h")
    ui = read("main/ui/ui_phase1.c")
    builder = service.split(
        "static esp_err_t build_path_discovery_request", 1
    )[1].split("static esp_err_t build_dm_ack_response", 1)[0]
    trace = service.split(
        "esp_err_t d1l_meshcore_service_request_path_discovery_probe", 1
    )[1].split("esp_err_t d1l_meshcore_service_send_trace_contact", 1)[0]
    dm_command = service.split(
        "static esp_err_t meshcore_service_send_dm_command", 1
    )[1].split("esp_err_t d1l_meshcore_service_send_dm", 1)[0]
    repeater_controls = ui.split(
        'if (strcmp(contact->type, "repeater") == 0)', 1
    )[1].split("} else {", 1)[0]

    assert "d1l_contact_store_can_path_probe" in contact_header
    assert "d1l_contact_store_can_path_probe(contact)" in builder
    assert "d1l_contact_store_can_dm(contact)" not in builder
    assert "d1l_contact_store_prepare_path_route(" in trace
    assert "d1l_contact_store_can_path_probe(&contact)" in trace
    assert "d1l_contact_store_can_dm(&contact)" not in trace
    assert "build_path_discovery_request(" in trace
    assert "s_path_response_expectation.tag = tag" in trace
    assert "s_path_response_fingerprint" in trace
    assert "meshcore_service_send_raw(" in trace
    assert '"path_discovery_req"' in trace
    assert "meshcore_service_send_dm_command(" not in trace
    assert "d1l_contact_store_can_dm(&contact)" in dm_command
    assert "d1l_contact_store_can_path_probe(&contact)" not in dm_command
    assert '"Probe"' in repeater_controls
    assert "route_probe_request_event_cb" in repeater_controls


def test_inbound_dm_ack_consumes_the_same_immutable_route_selection():
    service = read("main/mesh/meshcore_service.c")
    dm_parse = service.split("static bool parse_rx_dm_packet", 1)[1].split(
        "static d1l_rx_ack_result_t record_dm_ack", 1
    )[0]
    ack_planning = dm_parse.split("uint8_t ack_hash_bytes[4]", 1)[1].split(
        "ack_build_ret = planned", 1
    )[0]

    assert "d1l_contact_store_prepare_path_route(" in dm_parse
    assert "d1l_contact_store_can_dm(&ack_contact)" in dm_parse
    assert "lookup_boot_route(" in ack_planning
    assert "ack_contact.out_path_state.generation" in ack_planning
    assert "d1l_meshcore_route_select_canonical(" in ack_planning
    assert "d1l_meshcore_ack_dispatch_plan(" in ack_planning
    assert "ack_route_selection.route == D1L_MESHCORE_ROUTE_DIRECT" in ack_planning
    assert "ack_direct ? ack_route_selection.path : NULL" in ack_planning
    assert "ack_direct ? ack_route_selection.path_len : 0U" in ack_planning
    assert ack_planning.index("d1l_meshcore_route_select_canonical(") < ack_planning.index(
        "d1l_meshcore_ack_dispatch_plan("
    )
    planner_call = ack_planning.split("d1l_meshcore_ack_dispatch_plan(", 1)[1]
    assert "contact->out_path_valid" not in planner_call
    assert "contact->out_path, contact->out_path_len" not in planner_call


def test_direct_results_mutate_retained_path_only_in_runtime_owner():
    service = read("main/mesh/meshcore_service.c")
    contact_header = read("main/mesh/contact_store.h")
    contact_source = read("main/mesh/contact_store.c")
    timeout = service.split(
        "static void meshcore_service_handle_radio_tx_timeout", 1
    )[1].split("static void meshcore_service_handle_radio_tx_watchdog", 1)[0]
    ack = service.split("static d1l_rx_ack_result_t record_dm_ack", 1)[1].split(
        "static void parse_rx_ack_packet", 1
    )[0]
    ack_finalize = service.split(
        "static bool finalize_pending_dm_ack_completion(void)\n{", 1
    )[1].split("static void clear_pending_ack_tx", 1)[0]
    callback = service.split("static void on_tx_done", 1)[1].split(
        "static void on_tx_timeout", 1
    )[0]

    assert "d1l_meshcore_path_state_t out_path_state" in contact_header
    assert "D1L_CONTACT_STORE_SCHEMA 8U" in contact_source
    assert "D1L_CONTACT_STORE_SCHEMA_V7 7U" in contact_source
    assert "D1L_CONTACT_STORE_SCHEMA_V6 6U" in contact_source
    assert "migrate_v6_blob" in contact_source
    assert "finalize_migrated_path_state" in contact_source
    assert "retained_path_record_is_valid" in contact_source
    retry = service.split("static esp_err_t retry_pending_dm_as_flood(uint64_t now_us)\n{", 1)[1].split(
        "static esp_err_t meshcore_service_handle_send_dm", 1
    )[0]
    assert "record_pending_direct_path_result(false);" not in timeout
    assert "record_pending_direct_path_result(false);" in retry
    assert "record_pending_direct_path_result(true);" not in ack
    assert "record_pending_direct_path_result(true);" in ack_finalize
    assert ack_finalize.index(
        "d1l_meshcore_ack_completion_take_terminal_effects("
    ) < ack_finalize.index("record_pending_direct_path_result(true);")
    assert "d1l_contact_store_note_path_result(" not in callback
    assert "retained worker owns all serialization and NVS I/O" in service


def test_ack_deadline_retry_and_contact_revalidation_are_fail_closed():
    service = read("main/mesh/meshcore_service.c")
    planner = read("main/mesh/meshcore_dm_retry.h")
    send_dm = service.split(
        "static esp_err_t meshcore_service_handle_send_dm", 1
    )[1].split("static void meshcore_service_reply", 1)[0]
    inbound = service.split("static bool parse_rx_dm_packet", 1)[1].split(
        "static d1l_rx_ack_result_t record_dm_ack", 1
    )[0]
    maintenance = service.split(
        "static void meshcore_service_run_owner_maintenance", 1
    )[1].split("static void meshcore_service_handle_radio_rx_done", 1)[0]
    tx_done = service.split(
        "static void meshcore_service_handle_radio_tx_done", 1
    )[1].split("static void meshcore_service_handle_radio_tx_timeout", 1)[0]
    terminal = service.split(
        "static void fail_pending_dm_ack_timeout(esp_err_t error)\n{", 1
    )[1].split("static esp_err_t retry_pending_dm_as_flood", 1)[0]
    retry = service.split("static esp_err_t retry_pending_dm_as_flood(uint64_t now_us)\n{", 1)[1].split(
        "static esp_err_t meshcore_service_handle_send_dm", 1
    )[0]

    assert "D1L_MESHCORE_DM_ACK_DEADLINE_RETRY_FLOOD" in planner
    assert "D1L_MESHCORE_DM_ACK_DEADLINE_FAIL_TIMEOUT" in planner
    assert "d1l_meshcore_dm_ack_deadline_take_due_when_idle(" in maintenance
    assert "s_tx_busy" in maintenance
    assert "d1l_meshcore_dm_ack_deadline_arm(" in tx_done
    owner_truth = tx_done.index("const bool awaiting_ack_owned")
    persistence_log = tx_done.index("if (ret != ESP_OK)", owner_truth)
    deadline_arm = tx_done.index("d1l_meshcore_dm_ack_deadline_arm(", persistence_log)
    assert owner_truth < persistence_log < deadline_arm
    assert "awaiting_ack_owned &&" in tx_done[persistence_log:deadline_arm]
    assert "ret == ESP_OK" not in tx_done[persistence_log:deadline_arm]
    assert "retry_pending_dm_as_flood(now_us)" in maintenance
    assert "fail_pending_dm_ack_timeout(ESP_ERR_TIMEOUT)" in maintenance
    assert "d1l_dm_store_transition_delivery_retry(" in service
    assert "retry_ack_hash" in retry
    assert "retry_attempt = 1U" in retry
    assert "D1L_MESHCORE_ROUTE_SELECTION_FLOOD_DIRECT_RETRY" in retry
    assert "D1L_DM_DELIVERY_FAILED_TIMEOUT" in terminal
    assert "D1L_DM_DELIVERY_REASON_ACK_TIMEOUT" in terminal
    assert "clear_pending_dm_tx();" in terminal

    prepare = send_dm.index("d1l_contact_store_prepare_path_route(")
    recheck = send_dm.index("d1l_contact_store_can_dm(&contact)", prepare)
    append = send_dm.index("d1l_dm_store_append_tx(")
    radio = send_dm.index("Radio.SendWithOrigin(")
    assert prepare < recheck < append < radio
    assert "return prepare_ret;" in send_dm[prepare:recheck]

    inbound_prepare = inbound.index("d1l_contact_store_prepare_path_route(")
    inbound_return = inbound.index("return false;", inbound_prepare)
    inbound_append = inbound.index("d1l_dm_store_append_rx_identity_deferred(")
    inbound_radio = inbound.index("dispatch_bounded_dm_ack(")
    assert inbound_prepare < inbound_return < inbound_append
    assert inbound_prepare < inbound_return < inbound_radio
    assert "d1l_dm_store_append_rx_identity(" not in inbound


def test_path_dispatch_masks_subtype_and_queues_only_accepted_flood_reciprocal():
    service = read("main/mesh/meshcore_service.c")
    dispatch = read("main/mesh/meshcore_path_dispatch.h")
    path_rx = service.split("static void parse_rx_path_packet", 1)[1].split(
        "static void parse_rx_trace_packet", 1
    )[0]

    assert "decoded.raw_extra_type & 0x0fU" in dispatch
    assert "D1L_MESHCORE_PATH_SOURCE_OBSERVED" in dispatch
    assert "D1L_MESHCORE_PATH_SOURCE_ACK_PATH" in dispatch
    assert "D1L_MESHCORE_PATH_SOURCE_PATH_RESPONSE" in dispatch
    assert "dispatch_path_response(" in path_rx
    assert "d1l_meshcore_path_response_take(" in service
    assert "d1l_meshcore_reciprocal_path_take(" in path_rx
    assert "build_reciprocal_path_packet(" in path_rx
    assert "meshcore_service_queue_raw_response(" in path_rx
    replay_take = path_rx.index("d1l_meshcore_path_replay_take(")
    path_mutation = path_rx.index("d1l_contact_store_update_path_from_source(")
    reciprocal_rf = path_rx.index("meshcore_service_queue_raw_response(")
    assert replay_take < path_mutation < reciprocal_rf
    assert "d1l_meshcore_path_replay_forget(" in path_rx[path_mutation:reciprocal_rf]
    assert "D1L_MESHCORE_PATH_REPLAY_CAPACITY 8U" in dispatch
    assert "memcmp(cache->identities[i], identity" in dispatch


def test_route_selection_telemetry_distinguishes_every_fallback_reason():
    service = read("main/mesh/meshcore_service.c")
    header = read("main/mesh/meshcore_service.h")
    console = read("main/comms/usb_console.c")

    for field in (
        "dm_route_direct_selected",
        "dm_route_flood_selected",
        "dm_route_missing_fallback",
        "dm_route_preboot_fallback",
        "dm_route_stale_fallback",
        "dm_route_malformed_fallback",
        "dm_route_expired_fallback",
        "dm_route_failed_fallback",
        "dm_route_direct_retry_fallback",
        "dm_route_last_path_age_ms",
        "dm_route_last_reason",
    ):
        assert field in header
        assert f"s_status.{field}" in service

    assert r'\"dm_route\":{' in console
    assert r'\"direct_selected\":%lu' in console
    assert r'\"flood_selected\":%lu' in console
    assert r'\"missing_fallback\":%lu' in console
    assert r'\"preboot_fallback\":%lu' in console
    assert r'\"stale_fallback\":%lu' in console
    assert r'\"malformed_fallback\":%lu' in console
    assert r'\"expired_fallback\":%lu' in console
    assert r'\"failed_fallback\":%lu' in console
    assert r'\"direct_retry_fallback\":%lu' in console
    assert r'\"last_reason\":\"%s\"' in console
    assert r'\"last_path_age_ms\":%lu' in console


def test_route_selector_is_an_exact_pinned_production_binding_source():
    manifest = json.loads(read("tests/meshcore_oracle/manifest.json"))
    runner = read("scripts/meshcore_conformance_d1l.py")
    relative = "main/mesh/meshcore_route_selection.h"

    assert relative in manifest["production_binding_sources"]
    assert manifest["production_binding_sources"][relative] == canonical_sha256(relative)
    allowlist = runner.split(
        "EXPECTED_ORACLE_PRODUCTION_BINDING_SOURCE_PATHS = {", 1
    )[1].split("}", 1)[0]
    assert f'"{relative}"' in allowlist

    for path_state_relative in (
        "main/mesh/contact_store.c",
        "main/mesh/contact_store.h",
        "main/mesh/dm_store.c",
        "main/mesh/dm_store.h",
        "main/mesh/meshcore_dm_retry.h",
        "main/mesh/meshcore_path_dispatch.h",
        "main/mesh/meshcore_path_state.c",
        "main/mesh/meshcore_path_state.h",
        "main/mesh/meshcore_service.h",
        "main/mesh/meshcore_wire.h",
    ):
        assert path_state_relative in manifest["production_binding_sources"]
        assert manifest["production_binding_sources"][path_state_relative] == (
            canonical_sha256(path_state_relative)
        )
        assert f'"{path_state_relative}"' in allowlist
