import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def test_dm_command_is_authorized_owned_and_persisted_before_radio_admission():
    source = read("main/mesh/meshcore_service.c")
    task = body(
        source,
        "static void meshcore_service_task(void *arg)",
        "static esp_err_t meshcore_service_start_task(void)",
    )
    handler = body(
        source,
        "static esp_err_t meshcore_service_handle_send_dm",
        "static void meshcore_service_reply",
    )
    adapter = body(
        source,
        "static esp_err_t meshcore_service_send_dm_command",
        "esp_err_t d1l_meshcore_service_send_dm",
    )

    assert "D1L_MESHCORE_SERVICE_CMD_SEND_DM" in source
    assert "case D1L_MESHCORE_SERVICE_CMD_SEND_DM:" in task
    assert "meshcore_service_handle_send_dm(&cmd)" in task
    assert ".type = D1L_MESHCORE_SERVICE_CMD_SEND_DM" in adapter
    assert "meshcore_service_send_command(" in adapter
    preflight_lookup = adapter.index("d1l_contact_store_find_by_fingerprint")
    preflight_gate = adapter.index("d1l_contact_store_can_dm(&contact)")
    queue_at = adapter.index("meshcore_service_send_command(")
    assert preflight_lookup < preflight_gate < queue_at
    for forbidden in (
        "d1l_dm_store_append_tx",
        "d1l_dm_store_transition_delivery",
        "build_dm_text_packet",
        "Radio.Send",
    ):
        assert forbidden not in adapter

    owner_lookup = handler.index("d1l_contact_store_find_by_fingerprint")
    owner_gate = handler.index("d1l_contact_store_can_dm(&contact)")
    identity_at = handler.index("d1l_meshcore_service_ensure_identity()")
    timestamp_at = handler.index("d1l_settings_next_mesh_timestamp")
    append_at = handler.index("d1l_dm_store_append_tx(")
    retry_at = handler.index(
        "meshcore_service_finish_dm_persistence_admission("
    )
    begin_at = handler.index("begin_pending_dm_tx(")
    waiting_at = handler.index("D1L_DM_DELIVERY_WAITING_RADIO", begin_at)
    active_at = handler.index("D1L_DM_DELIVERY_TX_ACTIVE", waiting_at)
    radio_at = handler.index("Radio.SendWithOrigin(")
    assert (
        owner_lookup
        < owner_gate
        < identity_at
        < timestamp_at
        < append_at
        < retry_at
        < begin_at
        < waiting_at
        < active_at
        < radio_at
    )
    assert "if (s_pending_dm_tx.delivery.active)" in handler
    assert handler.index("if (s_pending_dm_tx.delivery.active)") < append_at
    assert "if (s_tx_busy)" in handler
    assert handler.index("if (s_tx_busy)", append_at) > append_at
    assert "record_detached_dm_queue_failure(" in handler

    admission = body(
        source,
        "static esp_err_t meshcore_service_finish_dm_persistence_admission",
        "static esp_err_t meshcore_service_handle_send_dm",
    )
    assert "append_outcome->error != ESP_ERR_NOT_FINISHED" in admission
    assert "D1L_MESHCORE_DM_PERSIST_RETRY_TIMEOUT_MS" in admission
    assert "vTaskDelay(retry_delay)" in admission
    assert "d1l_dm_store_flush()" in admission
    assert "append_outcome->durable = ret == ESP_OK" in admission
    assert admission.index("d1l_dm_store_flush()") < admission.index(
        "append_outcome->durable = ret == ESP_OK"
    )
    assert "ret != ESP_ERR_NOT_FINISHED" in admission


def test_outbound_dm_bounds_retained_worker_handoff_before_radio():
    source = read("main/mesh/meshcore_service.c")
    handler = body(
        source,
        "static esp_err_t meshcore_service_handle_send_dm",
        "static void meshcore_service_reply",
    )

    assert '#include "mesh/route_store_worker.h"' in source
    quiesce_at = handler.index("d1l_route_store_worker_quiesce_begin(")
    append_at = handler.index("d1l_dm_store_append_tx(")
    success_release_at = handler.index(
        "d1l_route_store_worker_quiesce_end();", append_at
    )
    radio_at = handler.index("Radio.SendWithOrigin(")
    cleanup_label_at = handler.index("dm_persistence_release:")
    cleanup_release_at = handler.index(
        "d1l_route_store_worker_quiesce_end();", cleanup_label_at
    )

    assert (
        quiesce_at
        < append_at
        < success_release_at
        < radio_at
        < cleanup_label_at
        < cleanup_release_at
    )
    assert (
        "D1L_MESHCORE_DM_PERSIST_RETRY_TIMEOUT_MS"
        in handler[quiesce_at:append_at]
    )
    assert "ESP_ERR_NOT_FINISHED" in handler[quiesce_at:append_at]
    assert "return " not in handler[append_at:success_release_at]
    assert handler[append_at:success_release_at].count(
        "goto dm_persistence_release;"
    ) == 7
    assert handler.count("d1l_route_store_worker_quiesce_end();") == 2
    assert handler[success_release_at:radio_at].strip() == (
        "d1l_route_store_worker_quiesce_end();"
    )


def test_dm_console_reports_transient_storage_admission_truthfully():
    console = read("main/comms/usb_console.c")
    command = body(
        console,
        "static void cmd_mesh_send_dm",
        "static void print_route_entry_json",
    )
    assert "ret == ESP_ERR_NOT_FINISHED" in command
    assert "DM storage is busy; message was not transmitted, retry" in command
    assert "DM requires a promoted contact with a retained public key" not in command


def test_dm_contact_provenance_gate_has_zero_unproven_side_effects():
    service = read("main/mesh/meshcore_service.c")
    contact_store = read("main/mesh/contact_store.c")
    native = read("tests/native/store_behavior_test.c")
    handler = body(
        service,
        "static esp_err_t meshcore_service_handle_send_dm",
        "static void meshcore_service_reply",
    )
    adapter = body(
        service,
        "static esp_err_t meshcore_service_send_dm_command",
        "esp_err_t d1l_meshcore_service_send_dm",
    )
    can_dm = body(
        contact_store,
        "bool d1l_contact_store_can_dm",
        "bool d1l_contact_store_can_admin",
    )

    # Both the caller boundary and the sole runtime owner fail closed. The
    # owner re-check precedes identity creation, timestamp consumption,
    # retained-message mutation, and physical radio admission.
    assert adapter.index("d1l_contact_store_can_dm(&contact)") < adapter.index(
        "meshcore_service_send_command("
    )
    gate = handler.index("d1l_contact_store_can_dm(&contact)")
    for side_effect in (
        "d1l_meshcore_service_ensure_identity()",
        "d1l_settings_next_mesh_timestamp",
        "d1l_dm_store_append_tx(",
        "ensure_radio_started()",
        "Radio.SendWithOrigin(",
    ):
        assert gate < handler.index(side_effect)
    assert "d1l_contact_store_is_canonical(entry)" in can_dm
    assert 'strcmp(entry->type, "chat") == 0' in can_dm

    # Native contact fixtures exercise each provenance class feeding this
    # exact predicate: passive/unproven is denied; signed and URI-imported
    # canonical chat contacts are admitted.
    unproven = body(
        native,
        "static void test_full_key_chat_without_provenance_is_not_dm_capable",
        "static void test_contact_export_role_mapping",
    )
    imported = body(
        native,
        "static void test_uri_import_promotes_placeholder_and_preserves_preferences",
        "static void test_uri_import_conflicts_and_nvs_failure_are_non_mutating",
    )
    verified = body(
        native,
        "static void test_verified_contact_create_reload_update_preserves_preferences",
        "static void test_verified_contact_promotes_placeholder_without_losing_preferences",
    )
    assert "D1L_CONTACT_VERIFICATION_NONE" in unproven
    assert "!d1l_contact_store_can_dm(&contact)" in unproven
    assert "D1L_CONTACT_VERIFICATION_URI_IMPORT" in imported
    assert "d1l_contact_store_can_dm(&contact)" in imported
    assert "D1L_CONTACT_VERIFICATION_SIGNED_ADVERT" in verified
    assert "d1l_contact_store_can_dm(&contact)" in verified


def test_tx_callbacks_advance_the_exact_session_and_never_claim_delivery():
    source = read("main/mesh/meshcore_service.c")
    done = body(
        source,
        "static void meshcore_service_handle_radio_tx_done",
        "static void meshcore_service_handle_radio_tx_timeout",
    )
    timeout = body(
        source,
        "static void meshcore_service_handle_radio_tx_timeout",
        "static void meshcore_service_handle_radio_rx_done",
    )
    transition = body(
        source,
        "static esp_err_t transition_pending_dm_tx",
        "typedef enum {\n    D1L_PENDING_DM_ACK_TRANSITION_RETRYABLE",
    )
    ack_transition = body(
        source,
        "static d1l_pending_dm_ack_transition_result_t transition_pending_dm_ack",
        "static esp_err_t transition_pending_dm_retry",
    )

    assert done.index("d1l_meshcore_start_rx();") < done.index(
        "D1L_DM_DELIVERY_TX_DONE"
    )
    assert done.index("D1L_DM_DELIVERY_TX_DONE") < done.index(
        "D1L_DM_DELIVERY_AWAITING_ACK"
    )
    assert "D1L_DM_DELIVERY_ACKNOWLEDGED" not in done
    assert "D1L_DM_DELIVERY_FAILED_RADIO" in timeout
    assert "D1L_DM_DELIVERY_REASON_RADIO_ERROR" in timeout

    assert "s_pending_dm_tx.delivery.session_id" in transition
    assert "s_pending_dm_tx.delivery.revision" in transition
    assert "s_pending_dm_tx.delivery.state" in transition
    assert "outcome.row_seq" not in transition
    assert "d1l_dm_delivery_owner_apply(" in transition
    assert "next_state == D1L_DM_DELIVERY_ACKNOWLEDGED" in transition
    assert (
        "const bool publish_to_owner = "
        "outcome.changed || outcome.persistence_retry;"
    ) in transition
    assert "outcome.changed || outcome.persistence_retry" in ack_transition
    durable_gate = ack_transition.index("if (!outcome.durable)")
    owner_apply = ack_transition.index("d1l_dm_delivery_owner_apply(")
    assert durable_gate < owner_apply
    assert "d1l_meshcore_ack_completion_observe_cas(" in ack_transition


def test_terminal_events_must_match_the_exact_active_operation_and_dm_revision():
    source = read("main/mesh/meshcore_service.c")
    matcher = body(
        source,
        "static bool meshcore_radio_terminal_matches",
        "static void runtime_note_queue_drop",
    )
    done = body(
        source,
        "static void meshcore_service_handle_radio_tx_done",
        "static void meshcore_service_handle_radio_tx_timeout",
    )
    timeout = body(
        source,
        "static void meshcore_service_handle_radio_tx_timeout",
        "static void meshcore_service_handle_radio_rx_done",
    )

    assert "d1l_mesh_tx_operation_identity_equal(" in matcher
    assert "event->tx_operation" in matcher
    assert "s_active_radio_tx" in matcher
    assert "s_pending_dm_tx.delivery.session_id" in matcher
    assert "event->tx_operation.dm_session_id" in matcher
    assert "s_pending_dm_tx.delivery.revision" in matcher
    assert "event->tx_operation.dm_revision" in matcher
    assert "D1L_DM_DELIVERY_TX_ACTIVE" in matcher

    for handler in (done, timeout):
        match_at = handler.index("meshcore_radio_terminal_matches(event)")
        clear_at = handler.index("meshcore_radio_tx_operation_clear()")
        busy_at = handler.index("s_tx_busy = false")
        rx_at = handler.index("d1l_meshcore_start_rx();")
        assert match_at < clear_at < busy_at < rx_at


def test_request_timeout_and_completion_are_correlated_to_one_slot_generation():
    source = read("main/mesh/meshcore_service.c")
    guard = read("main/mesh/meshcore_runtime_guard.h")
    command_guard = read("main/mesh/meshcore_command_guard.h")
    task = body(
        source,
        "static void meshcore_service_task(void *arg)",
        "static esp_err_t meshcore_service_start_task(void)",
    )
    send = body(
        source,
        "static esp_err_t meshcore_service_send_command",
        "esp_err_t d1l_meshcore_service_start_rx_async",
    )
    send_claimed = body(
        source,
        "static esp_err_t meshcore_service_send_claimed_command",
        "static esp_err_t meshcore_service_send_command",
    )
    enqueue = body(
        source,
        "static bool meshcore_service_enqueue_claimed",
        "static esp_err_t meshcore_service_send_claimed_command",
    )
    wait = body(
        source,
        "static esp_err_t meshcore_request_wait",
        "static void meshcore_request_abort_unqueued",
    )
    admit = body(
        source,
        "static bool meshcore_request_admit",
        "static void meshcore_request_complete",
    )
    complete = body(
        source,
        "static void meshcore_request_complete",
        "static void publish_callback_tx_operation",
    )

    assert "D1L_MESHCORE_REQUEST_SLOT_COUNT" in source
    assert "StaticSemaphore_t completion_storage" in source
    assert "s_request_slots[D1L_MESHCORE_REQUEST_SLOT_COUNT]" in source
    assert "request_id" in source
    assert "request_deadline_us" in source
    assert "volatile uint32_t word;" in guard
    assert "volatile uint32_t state;" not in guard
    assert "volatile uint32_t request_id;" not in guard
    assert "&guard->word" in guard
    assert "D1L_MESH_REQUEST_ID_MAX" in source
    for forbidden in (
        "reply_task",
        "xTaskNotifyWait",
        "xTaskNotifyStateClear",
        "eSetValueWithOverwrite",
    ):
        assert forbidden not in source

    assert send.index("meshcore_request_claim(cmd, timeout_ms)") < send.index(
        "meshcore_service_send_claimed_command(cmd, timeout_ms)"
    )
    assert "d1l_mesh_command_request_enqueue" in send_claimed
    assert "xQueueSend(enqueue_context->queue, command" in enqueue
    assert "meshcore_request_abort_unqueued(cmd)" in send_claimed
    assert "meshcore_request_wait(cmd, timeout_ticks)" in send_claimed
    assert task.index("meshcore_request_admit(&cmd)") < task.index(
        "switch (cmd.type)"
    )

    assert "d1l_mesh_command_request_cancel_and_release" in wait
    assert "D1L_MESH_REQUEST_PENDING" in command_guard
    assert "D1L_MESH_REQUEST_CALLER_CANCELLED" in wait
    assert "D1L_MESH_REQUEST_ADMITTED" in wait
    assert "xSemaphoreTake(slot->completion, portMAX_DELAY)" in wait
    assert admit.index("request_deadline_us") < admit.index(
        "d1l_mesh_command_request_admit"
    )
    assert "d1l_mesh_command_request_expire" in admit
    assert "d1l_mesh_request_guard_release(" not in admit
    assert "d1l_mesh_command_request_complete" in complete
    assert "cmd->request_id" in complete
    assert "D1L_MESH_REQUEST_ADMITTED" in command_guard
    assert "D1L_MESH_REQUEST_COMPLETED" in command_guard


def test_callback_terminal_snapshot_is_immutable_and_monotonically_identified():
    source = read("main/mesh/meshcore_service.c")
    begin = body(
        source,
        "static bool meshcore_radio_tx_operation_begin",
        "static void meshcore_radio_tx_operation_clear",
    )
    callbacks = source.split(
        "/* SX1262 callbacks are deliberately bounded", 1
    )[1].split("static RadioEvents_t s_radio_events", 1)[0]
    take_terminal = body(
        source,
        "static bool meshcore_service_take_latched_terminal",
        "static void status_lock",
    )

    assert ".operation_id = ++s_next_radio_tx_operation_id" in begin
    assert "identity.dm_session_id = dm_delivery->session_id" in begin
    assert "identity.dm_revision = dm_delivery->revision" in begin
    assert "publish_callback_tx_operation(&identity)" in begin
    assert callbacks.count(
        "capture_callback_tx_operation(origin, &identity)"
    ) == 2
    assert "out_event->tx_operation = best_identity" in take_terminal


def test_only_the_expected_ack_closes_the_awaiting_owner_session():
    source = read("main/mesh/meshcore_service.c")
    ack = source.rsplit("static d1l_rx_ack_result_t record_dm_ack", 1)[1].split(
        "static void parse_rx_ack_packet", 1
    )[0]

    matches_at = ack.index("d1l_dm_delivery_owner_ack_matches(")
    retain_at = ack.index("retain_pending_dm_ack_receipt(")
    transition_at = ack.index("transition_pending_dm_ack(")
    finalize_at = ack.index("finalize_pending_dm_ack_completion()")
    assert matches_at < retain_at < transition_at < finalize_at
    assert "d1l_meshcore_ack_completion_suppresses_rf_retry(" in ack
    assert "d1l_dm_store_mark_acked" not in ack
    assert '"dm_ack_persistence_pending"' in ack
    assert "return D1L_RX_ACK_RETRYABLE;" in ack[matches_at:transition_at]
    assert "D1L_RX_ACK_ACCEPTED_PENDING" in ack
    assert "bank_pending_dm_ack_packet_hash(packet_hash)" in ack
    assert '"dm_ack_unmatched"' in ack


def test_ack_receipt_is_bound_to_the_exact_retry_revision_and_hash():
    source = read("main/mesh/meshcore_service.c")
    header = read("main/mesh/meshcore_ack_completion.h")
    retain = body(
        source,
        "static bool retain_pending_dm_ack_receipt",
        "static void clear_pending_dm_tx",
    )
    transition = body(
        source,
        "static esp_err_t transition_pending_dm_tx",
        "typedef enum {\n    D1L_PENDING_DM_ACK_TRANSITION_RETRYABLE",
    )
    ack = source.rsplit("static d1l_rx_ack_result_t record_dm_ack", 1)[1].split(
        "static void parse_rx_ack_packet", 1
    )[0]
    finalize = body(
        source,
        "static bool finalize_pending_dm_ack_completion",
        "static void clear_pending_ack_tx",
    )

    assert "d1l_meshcore_ack_receipt_binding_t binding;" in source
    assert "binding->base_revision == base_revision" in header
    assert "binding->ack_hash == ack_hash" in header
    assert "completion->base_revision != owner->revision" in retain
    assert "d1l_meshcore_ack_receipt_binding_begin(" in retain
    awaiting = transition.index("next_state == D1L_DM_DELIVERY_AWAITING_ACK")
    rearm_clear = transition.index("memset(&s_pending_dm_tx.ack_receipt", awaiting)
    completion_begin = transition.index("d1l_meshcore_ack_completion_begin(", awaiting)
    assert rearm_clear < completion_begin
    retryable = ack.index("D1L_PENDING_DM_ACK_TRANSITION_RETRYABLE")
    receipt_clear = ack.index("memset(&s_pending_dm_tx.ack_receipt", retryable)
    persistence_log = ack.index('"dm_ack_persistence_pending"', retryable)
    assert retryable < receipt_clear < persistence_log
    assert "s_pending_dm_tx.ack_completion.base_revision" in finalize
    assert "s_pending_dm_tx.ack_hash" in finalize


def test_ack_store_masks_failed_persistence_until_same_revision_retry_is_durable():
    store = read("main/mesh/dm_store.c")
    header = read("main/mesh/dm_store.h")
    transition = body(
        store,
        "static esp_err_t transition_delivery_internal",
        "esp_err_t d1l_dm_store_transition_delivery",
    )

    assert "bool persistence_retry;" in header
    assert "reserve_ack_persistence_pending_locked(entry)" in transition
    assert "outcome->persistence_retry = true" in transition
    assert "outcome->durable = ret == ESP_OK" in transition
    assert "expected_delivery_revision + 1U" in transition
    assert "find_ack_persistence_pending_locked(" in store
    assert "project_ack_persistence_truth_locked" in store
    assert "entry->acked = false" in store
    assert "entry->delivered = false" in store


def test_owner_automatically_reconciles_background_durable_ack():
    source = read("main/mesh/meshcore_service.c")
    header = read("main/mesh/dm_store.h")
    transition = body(
        source,
        "static d1l_pending_dm_ack_transition_result_t transition_pending_dm_ack",
        "static void reconcile_pending_dm_ack_persistence",
    )
    reconcile = body(
        source,
        "static void reconcile_pending_dm_ack_persistence",
        "static esp_err_t record_detached_dm_queue_failure",
    )
    task = body(
        source,
        "static void meshcore_service_task(void *arg)",
        "static esp_err_t meshcore_service_start_task(void)",
    )

    assert "bool ack_persistence_pending;" in source
    assert "if (!outcome.durable)" in transition
    assert "d1l_meshcore_ack_completion_observe_cas(" in transition
    assert "s_pending_dm_tx.ack_persistence_pending = true" in transition
    assert "d1l_dm_store_find_delivery_session" in header
    query = reconcile.index("d1l_dm_store_find_delivery_session")
    state = reconcile.index("D1L_DM_DELIVERY_ACKNOWLEDGED", query)
    revision = reconcile.index("durable.delivery_revision != owner->revision + 1U")
    delivered = reconcile.index("!durable.acked || !durable.delivered")
    reconcile_cas = reconcile.index("d1l_meshcore_ack_completion_mark_reconciled(")
    publish = reconcile.index("d1l_dm_delivery_owner_apply(")
    finalize = reconcile.rindex("finalize_pending_dm_ack_completion()")
    assert query < state < revision < delivered < reconcile_cas < publish < finalize
    assert "transition_pending_dm_tx(" not in reconcile
    assert task.count("meshcore_service_run_owner_maintenance();") >= 3
    assert "pdMS_TO_TICKS(D1L_MESHCORE_OWNER_POLL_MS)" in task


def test_actual_radio_callbacks_remain_storage_free_and_bounded():
    source = read("main/mesh/meshcore_service.c")
    callbacks = source.split(
        "/* SX1262 callbacks are deliberately bounded", 1
    )[1].split("static RadioEvents_t s_radio_events", 1)[0]

    assert callbacks.count("enqueue_radio_event(") == 5
    for forbidden in (
        "d1l_dm_store_",
        "transition_pending_dm_tx",
        "d1l_packet_log_",
        "d1l_route_store_",
        "Radio.",
    ):
        assert forbidden not in callbacks


def test_delivery_owner_service_keeps_the_exact_oracle_binding_pin():
    relative = "main/mesh/meshcore_service.c"
    payload = (ROOT / relative).read_bytes().replace(b"\r\n", b"\n")
    manifest = json.loads(read("tests/meshcore_oracle/manifest.json"))
    assert manifest["production_binding_sources"][relative] == hashlib.sha256(
        payload
    ).hexdigest()


def test_ack_completion_is_armed_from_the_actual_awaiting_revision():
    source = read("main/mesh/meshcore_service.c")
    begin = source.index("static bool begin_pending_dm_tx(")
    transition = source.index("static esp_err_t transition_pending_dm_tx(")
    ack_transition = source.index(
        "static d1l_pending_dm_ack_transition_result_t transition_pending_dm_ack("
    )

    begin_body = source[begin:transition]
    transition_body = source[transition:ack_transition]
    assert "d1l_meshcore_ack_completion_begin(" not in begin_body
    awaiting = transition_body.index(
        "next_state == D1L_DM_DELIVERY_AWAITING_ACK"
    )
    owner_publish = transition_body.index("d1l_dm_delivery_owner_apply(")
    completion_arm = transition_body.index("d1l_meshcore_ack_completion_begin(")
    assert owner_publish < awaiting < completion_arm
    assert "s_pending_dm_tx.delivery.revision" in transition_body[
        completion_arm:completion_arm + 400
    ]
