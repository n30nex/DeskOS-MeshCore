import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def canonical_sha256(relative: str) -> str:
    payload = (ROOT / relative).read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(payload).hexdigest()


def test_trace_helper_supports_bounded_widths_and_direct_terminal_only() -> None:
    trace = read("main/mesh/meshcore_trace.h")
    wire = read("main/mesh/meshcore_wire.h")

    assert "D1L_MESHCORE_PAYLOAD_TRACE 0x09U" in wire
    assert "D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS 30000U" in trace
    assert "D1L_MESHCORE_TRACE_COOLDOWN_MS 30000U" in trace
    assert "D1L_MESHCORE_TRACE_DUPLICATE_WINDOW_MS 60000U" in trace
    assert "D1L_MESHCORE_TRACE_MAX_HASH_BYTES 8U" in trace
    assert "D1L_MESHCORE_TRACE_MAX_CONTACT_HASH_BYTES 2U" in trace
    assert (
        "D1L_MESHCORE_MAX_PACKET_PAYLOAD - D1L_MESHCORE_TRACE_FIXED_BYTES"
        in trace
    )
    assert 'D1L_MESHCORE_TRACE_RETAINED_TARGET "trace_last"' in trace
    assert "D1L_MESHCORE_ROUTE_DIRECT" in trace
    assert "d1l_meshcore_trace_flags_for_hash_width(path_hash_bytes)" in trace
    assert "path_hash_bytes == 4U ? 2U : 3U" in trace
    assert "D1L_MESHCORE_TRACE_FRAME_SOURCE" in trace
    assert "D1L_MESHCORE_TRACE_FRAME_IN_FLIGHT" in trace
    assert "D1L_MESHCORE_TRACE_FRAME_UNSUPPORTED" in trace
    assert "D1L_MESHCORE_TRACE_FRAME_TERMINAL" in trace
    assert "packet.path_hops > explicit_hops" in trace
    assert "(flags & 0xfcU) != 0U" in trace
    assert "explicit_path_bytes > D1L_MESHCORE_TRACE_MAX_PATH_BYTES" in trace
    assert "explicit_path_bytes == 0U" in trace
    assert "return D1L_MESHCORE_TRACE_FRAME_UNSUPPORTED" in trace
    assert "d1l_meshcore_trace_classify(raw, raw_len, out_terminal) ==" in trace
    assert "D1L_MESHCORE_ROUTE_FLOOD" not in trace
    assert "D1L_MESHCORE_ROUTE_TRANSPORT" not in trace
    assert "*out_source = source" in trace
    assert "*out_terminal = terminal" in trace
    assert "d1l_meshcore_trace_plan_contact" in trace
    assert "d1l_meshcore_trace_contact_hash_width" in trace
    assert "D1L_MESHCORE_CONTACT_TRACE_PLAN_UNSUPPORTED_WIDTH" in trace
    assert "D1L_MESHCORE_CONTACT_TRACE_PLAN_EMPTY" in trace
    assert "D1L_MESHCORE_CONTACT_TRACE_PLAN_TOO_LONG" in trace
    assert "(size_t)path_hops * 2U + 1U" in trace
    assert "(size_t)path_hops * 2U - 1U" in trace
    assert "contact_hash, trace_hash_bytes" in trace
    assert "&out_path[(i - 1U) * route_hash_bytes]" in trace
    assert "route_hash_bytes == 2U || route_hash_bytes == 3U ? 2U" in trace


def test_trace_tracker_correlates_immutable_tag_auth_path_and_bounds_age() -> None:
    trace = read("main/mesh/meshcore_trace.h")

    assert "tracker->pending_tag = tag" in trace
    assert "tracker->pending_auth_code = auth_code" in trace
    assert "tracker->pending_path_hash_bytes = path_hash_bytes" in trace
    assert "memcpy(tracker->pending_path_hashes, path_hashes, path_bytes)" in trace
    assert "terminal->path_hash_bytes == expected_hash_bytes" in trace
    assert "D1L_MESHCORE_TRACE_CORRELATION_AUTH_MISMATCH" in trace
    assert "D1L_MESHCORE_TRACE_CORRELATION_PATH_MISMATCH" in trace
    assert "D1L_MESHCORE_TRACE_CORRELATION_DUPLICATE" in trace
    assert "D1L_MESHCORE_TRACE_CORRELATION_EXPIRED" in trace
    assert "tracker->last_result = *terminal" in trace
    assert "D1L_MESHCORE_TRACE_OUTCOME_PENDING" in trace
    assert "D1L_MESHCORE_TRACE_OUTCOME_MATCHED" in trace
    assert "D1L_MESHCORE_TRACE_OUTCOME_NO_RESPONSE" in trace
    assert "d1l_meshcore_trace_tracker_cooldown_remaining_ms" in trace
    assert "tracker->attempt_outcome_at_ms = now_ms" in trace
    assert "tracker->expired_attempt_valid = true" in trace
    assert "tracker->expired_correlation_code = tracker->pending_auth_code" in trace
    assert "terminal->auth_code == tracker->expired_correlation_code" in trace
    assert "tracker->expired_path_hashes" in trace
    assert "(uint32_t)(now_ms - tracker->pending_started_ms)" in trace
    assert "(uint32_t)(now_ms - tracker->completed_at_ms)" in trace
    assert "(uint32_t)(now_ms - tracker->attempt_outcome_at_ms)" in trace


def test_service_retains_only_a_matched_trace_result() -> None:
    service = read("main/mesh/meshcore_service.c")
    receive = service.split("static void parse_rx_trace_packet", 1)[1].split(
        "static bool verify_advert_signature", 1
    )[0]

    assert "d1l_meshcore_trace_classify" in receive
    assert "d1l_meshcore_trace_tracker_consume" in receive
    assert "s_status.trace_rx_malformed++" in receive
    assert "s_status.trace_rx_source_ignored++" in receive
    assert "s_status.trace_rx_in_flight_ignored++" in receive
    assert "s_status.trace_rx_unsupported++" in receive
    assert "meshcore_service_expire_trace_if_due(now_ms)" in receive
    assert "s_status.trace_rx_duplicates++" in receive
    assert "s_status.trace_rx_expired++" in receive
    assert "s_status.trace_rx_unmatched++" in receive
    assert "s_status.trace_rx_auth_mismatch++" in receive
    assert "s_status.trace_rx_path_mismatch++" in receive
    matched_guard = (
        "if (correlation != D1L_MESHCORE_TRACE_CORRELATION_MATCHED)"
    )
    assert matched_guard in receive
    assert receive.index(matched_guard) < receive.index(
        "d1l_route_store_upsert_observation("
    )
    assert receive.index(matched_guard) < receive.index(
        "const bool packet_retained = !packet_retention_needed"
    )
    terminal_guard = "if (frame_kind != D1L_MESHCORE_TRACE_FRAME_TERMINAL)"
    assert terminal_guard in receive
    assert receive.index(terminal_guard) < receive.index(
        "d1l_meshcore_trace_tracker_consume"
    )
    assert receive.index(terminal_guard) < receive.index(
        "d1l_route_store_upsert_observation("
    )
    assert "s_trace_last_retention_attempted = true" in receive
    assert "s_trace_last_route_summary_accepted || route_ret == ESP_OK" in receive
    assert "s_trace_last_packet_preview_retained || packet_retained" in receive
    assert "d1l_meshcore_trace_retained_target_for_tag(terminal.tag)" in receive
    assert '"trace_%08lX"' not in receive
    assert '"trace_reply"' in receive
    assert '"Explicit TRACE"' in receive


def test_runtime_owner_derives_contact_trace_from_one_current_proven_path() -> None:
    service = read("main/mesh/meshcore_service.c")
    header = read("main/mesh/meshcore_service.h")
    resolve = service.split(
        "static esp_err_t meshcore_service_resolve_trace_contact", 1
    )[1].split(
        "static esp_err_t meshcore_service_handle_send_trace_contact", 1
    )[0]
    send = service.split(
        "static esp_err_t meshcore_service_handle_send_trace_contact", 1
    )[1].split("static void meshcore_service_reply", 1)[0]
    queue = service.split(
        "static esp_err_t meshcore_service_send_trace_command", 1
    )[1].split(
        "esp_err_t d1l_meshcore_service_send_trace_contact", 1
    )[0]
    wrappers = service.split(
        "esp_err_t d1l_meshcore_service_send_trace_contact", 1
    )[1].split("const char *d1l_meshcore_service_state_name", 1)[0]
    helper = service.split(
        "static bool meshcore_service_expire_trace_if_due", 1
    )[1].split("static void parse_rx_trace_packet", 1)[0]
    maintenance = service.split(
        "static void meshcore_service_run_owner_maintenance", 1
    )[1].split("static void meshcore_service_handle_radio_rx_done", 1)[0]

    assert "d1l_contact_store_copy_recent" in resolve
    assert "meshcore_service_fingerprint_equal(" in resolve
    assert "matches != 1U" in resolve
    assert "d1l_contact_store_is_canonical(&unique)" in resolve
    assert "d1l_contact_store_prepare_path_route" in resolve
    assert "unique.fingerprint, now_ms" in resolve
    assert "prepared.public_key_hex, unique.public_key_hex" in resolve
    assert "lookup_boot_route(contact.fingerprint, contact.out_path" in send
    assert "contact.out_path_state.generation" in send
    assert "d1l_meshcore_route_select_canonical" in send
    assert "selection.route != D1L_MESHCORE_ROUTE_DIRECT" in send
    assert "selection.reason != D1L_MESHCORE_ROUTE_SELECTION_DIRECT_PROVEN" in send
    assert 'strcmp(contact.type, "repeater") == 0' in send
    assert 'strcmp(contact.type, "room") == 0' in send
    assert "d1l_meshcore_trace_plan_contact" in send
    assert "D1L_MESHCORE_CONTACT_TRACE_PLAN_UNSUPPORTED_WIDTH" in send
    assert "contact_public_key, &plan" in send
    assert "d1l_meshcore_trace_build_source" in send
    assert "tag, auth_code, plan.path_hash_bytes, plan.path_hashes" in send
    assert "meshcore_service_expire_trace_if_due(now_ms)" in send
    assert "d1l_meshcore_trace_tracker_cooldown_remaining_ms" in send
    assert "return ESP_ERR_NOT_FINISHED;" in send
    assert "return ESP_ERR_NOT_ALLOWED;" in send
    assert "d1l_meshcore_trace_tracker_begin" in send
    assert "d1l_meshcore_trace_tracker_cancel" in send
    assert "meshcore_service_handle_send_raw(&raw_cmd)" in send
    assert "d1l_route_store_upsert_observation_volatile" in send
    assert '"trace_request"' in send
    assert "D1L_MESHCORE_ROUTE_FLOOD" not in send
    assert send.index("d1l_meshcore_trace_tracker_begin") < send.index(
        "meshcore_service_handle_send_raw(&raw_cmd)"
    )
    assert send.index("meshcore_service_handle_send_raw(&raw_cmd)") < send.index(
        "s_status.trace_tx_queued++"
    )
    assert "D1L_MESHCORE_SERVICE_CMD_SEND_TRACE_CONTACT" in queue
    assert "trace_fingerprint" in queue
    assert "meshcore_service_lower_hex(fingerprint[i])" in queue
    assert "ESP_ERR_INVALID_ARG" in queue
    assert "path_hash" not in queue
    assert "meshcore_service_send_trace_command(fingerprint, false)" in wrappers
    assert "meshcore_service_send_trace_command(fingerprint, true)" in wrappers
    assert "d1l_meshcore_service_send_trace_loop" not in service
    assert "d1l_meshcore_service_send_trace_loop" not in header
    assert "ESP_ERR_NOT_FINISHED means one TRACE is pending" in header
    assert "ESP_ERR_NOT_ALLOWED means" in header
    assert "d1l_meshcore_trace_tracker_expire_pending" in helper
    assert "s_status.trace_pending_expired++" in helper
    assert "meshcore_service_expire_trace_if_due" in maintenance
    assert maintenance.index("meshcore_service_expire_trace_if_due") < (
        maintenance.index("meshcore_service_handle_radio_tx_watchdog")
    )


def test_trace_snapshot_is_passive_and_does_not_expose_auth_code() -> None:
    service = read("main/mesh/meshcore_service.c")
    header = read("main/mesh/meshcore_service.h")
    snapshot = service.split(
        "void d1l_meshcore_service_trace_snapshot", 1
    )[1].split("esp_err_t d1l_meshcore_service_request_advert", 1)[0]

    assert "d1l_meshcore_trace_tracker_expire_pending" not in snapshot
    assert "snapshot.pending_expired" in snapshot
    assert "D1L_MESHCORE_TRACE_PENDING_TIMEOUT_MS" in snapshot
    assert "snapshot.pending_path_hash_bytes" in snapshot
    assert "snapshot.last_attempt_outcome" in snapshot
    assert "snapshot.cooldown_remaining_ms" in snapshot
    assert "d1l_meshcore_trace_tracker_cooldown_remaining_ms" in snapshot
    assert "snapshot.last_path_hash_bytes" in snapshot
    assert "snapshot.last_retention_attempted" in snapshot
    assert "snapshot.last_route_summary_accepted" in snapshot
    assert "snapshot.last_packet_preview_retained" in snapshot
    snapshot_type = header.split("typedef struct {", 2)[2].split(
        "} d1l_meshcore_trace_snapshot_t", 1
    )[0]
    assert "auth_code" not in snapshot_type


def test_console_and_ui_disclose_trace_boundaries_truthfully() -> None:
    console = read("main/comms/usb_console.c")
    app_header = read("main/app/app_model.h")
    trace_contact = console.split(
        "static void cmd_routes_trace_contact", 1
    )[1].split("static void cmd_routes_trace_status", 1)[0]
    trace_contact_failure = trace_contact.split(
        "if (ret != ESP_OK)", 1
    )[1].split("d1l_meshcore_trace_snapshot_t", 1)[0]
    trace_status = console.split("static void cmd_trace_status", 1)[1].split(
        "static void cmd_routes_trace_status", 1
    )[0]
    trace_status_wrapper = console.split(
        "static void cmd_routes_trace_status", 1
    )[1].split("static void cmd_repeater_ping", 1)[0]
    app = read("main/app/app_model.c")
    ui = read("main/ui/ui_phase1.c")
    test_plan = read("docs/TEST_PLAN_D1L.md")

    assert 'ok_begin("routes trace contact")' in console
    assert "ok_begin(command)" in trace_status
    assert 'cmd_trace_status("routes trace status")' in trace_status_wrapper
    assert "routes trace contact <fingerprint>" in console
    assert "routes trace status" in console
    assert "requires_current_boot_proven_contact_path" in console
    assert "contact_trace_supported" in console
    assert "operator_path_accepted" in console
    assert "operator_trace_path_accepted" in console
    assert "current_boot_proven_contact_path" in console
    assert '"one_byte_hash_only\\":false' in console
    assert '"trace_wire_hash_bytes_supported\\":[1,2,4,8]' in console
    assert '"contact_trace_path_hash_bytes_supported\\":[1,2]' in console
    assert '"contact_route_hash_bytes_accepted\\":[1,2,3]' in console
    assert '"contact_route_hash_bytes_normalized_to_two\\":[3]' in console
    assert '"contact_route_hash_bytes_rejected\\":[]' in console
    assert '"trace_flags_supported\\":[0,1,2,3]' in console
    assert "hardware_verified" in console
    assert "per_hop_complete" in console
    assert "route_summary_durable_verified" in console
    assert "packet_preview_retained" in console
    assert "trace_rx_source_ignored" in console
    assert "trace_rx_in_flight_ignored" in console
    assert "trace_rx_unsupported" in console
    assert "trace_pending_expired" in console
    assert "rx_correlation_code_mismatch" in console
    assert '"rx_auth_mismatch\\":' not in console
    assert "TRACE_PENDING" in console
    assert "TRACE_COOLDOWN" in console
    assert "ret == ESP_ERR_NOT_FINISHED" in trace_contact
    assert "ret == ESP_ERR_NOT_ALLOWED" in trace_contact
    assert 'code = "TRACE_PENDING"' in trace_contact
    assert 'code = "TRACE_COOLDOWN"' in trace_contact
    assert "d1l_meshcore_service_trace_snapshot" not in trace_contact_failure
    assert '"no_response\\":%s' in console
    assert '"cooldown\\":{' in console
    assert "D1L_MESHCORE_TRACE_COOLDOWN_MS" in console
    assert 'retained_summary\\\":true' not in console
    assert "boot_local" in console
    assert "tag_opaque_correlation_code_immutable_contact_loop" in console
    assert "tag_auth_immutable_contact_loop" not in console
    assert "pending_auth" not in console
    assert "routes trace send <loop-path-hex>" not in console
    assert "parse_trace_loop_path" not in console
    assert console.index('strncmp(line, "routes trace contact ", 21)') < console.index(
        'strncmp(line, "routes trace ", 13)'
    )
    assert console.index('strcmp(line, "routes trace status")') < console.index(
        'strncmp(line, "routes trace ", 13)'
    )
    assert "d1l_app_model_request_path_discovery_probe" in console
    assert "real_trace_packet" in console
    assert "Encrypted PATH discovery and base-telemetry request queued" in console
    assert '\\"telemetry_requested\\":true' in console
    assert "d1l_meshcore_service_request_path_discovery_probe" in app
    assert "d1l_app_model_send_trace_contact" in app_header
    assert "d1l_meshcore_service_send_trace_contact(fingerprint)" in app
    assert "d1l_app_model_send_trace_contact(" in ui
    assert "d1l_app_model_request_path_discovery_probe(" in ui
    assert '"Probe and TRACE are contact-only RF; Reset forgets only this contact; no Public RF"' in ui
    assert "\"Authenticated TRACE" not in ui
    assert "routes trace send <loop-path-hex>" not in test_plan
    assert "contact_trace_supported=false" not in test_plan
    assert "real_trace_contact_supported=false" not in test_plan
    assert "routes trace contact <fingerprint>" in test_plan
    assert "operator_path_accepted=false" in test_plan
    assert "one_byte_hash_only=false" in test_plan
    assert "trace_wire_hash_bytes_supported=[1,2,4,8]" in test_plan
    assert "contact_trace_path_hash_bytes_supported=[1,2]" in test_plan
    regression = test_plan.split(
        "For the current contact-targeted TRACE software regression", 1
    )[1].split("\n\n", 1)[0]
    physical = test_plan.split(
        "`routes probe` remains a DM token/PATH helper", 1
    )[1].split("\n\n", 1)[0]
    assert "opaque correlation code" in regression
    assert "opaque authentication" not in regression
    assert "one- and two-byte hash widths with request flags 0 and 1" in physical
    assert (
        "flags 0 through 3, representing 1-, 2-, 4-, and 8-byte hashes"
        in physical
    )
    assert "three-byte contact route must normalize to two-byte prefixes" in physical
    assert "opaque correlation code" in physical
    assert "opaque authentication" not in physical
    assert "controlled multi-hop official-peer RF" in physical
    assert "exact-candidate hardware acceptance" in physical


def test_trace_helper_and_service_are_exact_production_binding_sources() -> None:
    manifest = json.loads(read("tests/meshcore_oracle/manifest.json"))
    runner = read("scripts/meshcore_conformance_d1l.py")
    allowlist = runner.split(
        "EXPECTED_ORACLE_PRODUCTION_BINDING_SOURCE_PATHS = {", 1
    )[1].split("}", 1)[0]

    for relative in (
        "main/comms/usb_console.c",
        "main/mesh/meshcore_command_guard.h",
        "main/mesh/meshcore_runtime_guard.h",
        "main/mesh/meshcore_trace.h",
        "main/mesh/meshcore_service.c",
        "main/mesh/meshcore_service.h",
    ):
        assert relative in manifest["production_binding_sources"]
        assert manifest["production_binding_sources"][relative] == canonical_sha256(
            relative
        )
        assert f'"{relative}"' in allowlist
