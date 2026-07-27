from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def c_function(source: str, signature: str) -> str:
    start = source.index(signature)
    while True:
        brace = source.index("{", start)
        semicolon = source.find(";", start, brace)
        if semicolon < 0:
            break
        start = source.index(signature, start + len(signature))
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def test_v5_history_identity_and_public_compatibility_are_exact():
    header = read("main/mesh/message_store.h")
    source = read("main/mesh/message_store.c")
    app = read("main/app/app_model.c")

    assert "D1L_MESSAGE_STORE_SCHEMA 5U" in source
    assert "D1L_MESSAGE_STORE_SCHEMA_V4 4U" in source
    assert "uint8_t reserved[5]" in header
    assert "offsetof(d1l_message_entry_t, channel_id) == 192U" in source
    assert "sizeof(d1l_message_entry_t) == 200U" in source
    assert "sizeof(d1l_message_store_blob_t) == 3240U" in source
    assert "current->channel_id = D1L_CHANNEL_PUBLIC_ID" in source

    public_copy = c_function(source, "size_t d1l_message_store_copy_recent(")
    public_page = c_function(source, "size_t d1l_message_store_query_page(")
    assert "D1L_CHANNEL_PUBLIC_ID" in public_copy
    assert "d1l_message_store_copy_channel_recent" in public_copy
    assert "D1L_CHANNEL_PUBLIC_ID" in public_page
    assert "d1l_message_store_query_channel_page" in public_page
    channel_page = c_function(
        source, "size_t d1l_message_store_query_channel_page("
    )
    assert "entry->channel_id != channel_id" in channel_page
    assert "s_volatile_entry.channel_id == channel_id" in channel_page

    assert "snapshot->message_count = messages.public_count" in app
    assert "d1l_message_store_copy_recent(snapshot->recent_messages" in app


def test_reconciliation_is_owned_bounded_and_ui_snapshots_are_read_only():
    coordinator = read("main/mesh/channel_message_coordinator.c")
    coordinator_header = read("main/mesh/channel_message_coordinator.h")
    channel_store = read("main/mesh/channel_store.c")
    service = read("main/mesh/meshcore_service.c")
    app = read("main/app/app_model.c")
    app_main = read("main/app_main.c")

    assert "D1L_CHANNEL_MESSAGE_RECONCILE_ATTEMPTS 3U" in coordinator
    assert "D1L_CHANNEL_MESSAGE_RECONCILE_RETRY_MS 5000U" in coordinator_header
    assert "d1l_message_store_snapshot_retained(" in coordinator
    assert "d1l_channel_store_reconcile_retained_rows(" in coordinator
    assert "persistence_revision ==" in coordinator
    assert "s_reconcile_pending = true" in coordinator
    assert "d1l_channel_message_reconcile_if_due" in coordinator

    reconcile = c_function(
        channel_store,
        "esp_err_t d1l_channel_store_reconcile_retained_rows(",
    )
    assert "retained_unread" in reconcile
    assert "rows[i].received" in reconcile
    assert "persist_store_or_rollback(&s_rollback_scratch)" in reconcile
    assert "read_through = unread == 0U" in reconcile

    copy_channels = c_function(app, "esp_err_t d1l_app_model_copy_channels(")
    assert "d1l_channel_message_reconcile(" not in copy_channels
    assert "d1l_channel_store_snapshot(" in copy_channels
    assert "read_state.public_unread_count" in copy_channels
    assert "channel_message_reconcile_pending" in app

    assert app_main.index("d1l_channel_store_init()") < app_main.index(
        "d1l_message_store_init()"
    )
    assert "d1l_channel_message_reconcile()" in app_main
    maintenance = c_function(
        service, "static void meshcore_service_run_owner_maintenance("
    )
    assert "d1l_channel_message_reconcile_if_due" in maintenance


def test_tx_and_rx_authenticate_every_configured_channel_before_side_effects():
    service = read("main/mesh/meshcore_service.c")
    service_header = read("main/mesh/meshcore_service.h")

    assert "s_public_secret" not in service
    assert "s_public_channel_hash" not in service
    assert "d1l_meshcore_service_send_channel" in service_header
    assert "d1l_meshcore_service_send_active_channel" in service_header

    append_rx = c_function(
        service,
        "static d1l_channel_rx_store_result_t append_channel_message_store_rx(",
    )
    assert "sanitize_note(author, sizeof(author)," in append_rx
    assert 'channel_name && channel_name[0] ? channel_name : "Channel"' in append_rx
    assert "snprintf(author" not in append_rx
    assert ".admitted = message_seq != 0U" in append_rx
    sanitizer = c_function(service, "static void sanitize_note(")
    assert "out + 1U < dest_size" in sanitizer
    assert "c < 32 || c > 126" in sanitizer

    send = c_function(
        service, "static esp_err_t meshcore_service_send_channel_owned("
    )
    ready_index = send.index("channel_message_generation_ready()")
    copy_index = send.index("d1l_channel_store_copy_protocol_key")
    timestamp_index = send.index("d1l_time_service_preflight_protocol_timestamp")
    hash_index = send.index("d1l_meshcore_packet_hash_calculate")
    pending_index = send.index("remember_pending_channel_tx(channel_id, text,")
    radio_index = send.index("meshcore_service_send_raw")
    assert ready_index < copy_index < timestamp_index < hash_index
    assert hash_index < pending_index < radio_index
    assert "D1L_MESHCORE_SERVICE_CMD_START_RX" not in send
    assert "D1L_MESHCORE_CHANNEL_COMMAND_TIMEOUT_MS" in send
    assert "d1l_channel_store_find_unique_hash" not in send
    assert "secure_zero_channel_key(&channel_key)" in send

    build = c_function(service, "static esp_err_t build_channel_text_packet(")
    assert "raw[i++] = channel->channel_hash" in build
    assert "channel->secret" in build
    assert "sender_name" in build
    assert "message_offset = 5U + sender_name_len + 2U" in build
    assert "settings->node_name, text" in send

    active = c_function(
        service, "esp_err_t d1l_meshcore_service_send_active_channel("
    )
    public = c_function(service, "esp_err_t d1l_meshcore_service_send_public(")
    assert "d1l_channel_store_find_default" in active
    assert "active.channel_id" in active
    assert "D1L_CHANNEL_PUBLIC_ID" in public

    receive = c_function(service, "static void parse_rx_channel_packet(")
    matches_index = receive.index("d1l_channel_store_copy_rx_hash_matches")
    decrypt_index = receive.index("meshcore_decrypt_after_mac")
    finish_index = receive.index("d1l_meshcore_channel_dispatch_finish")
    current_key_index = receive.index("d1l_channel_store_copy_protocol_key")
    plaintext_index = receive.index("d1l_meshcore_text_plaintext_view(")
    duplicate_index = receive.index("d1l_meshcore_packet_hash_cache_contains")
    ready_index = receive.index("channel_message_generation_ready()")
    append_index = receive.index("append_channel_message_store_rx(")
    assert matches_index < decrypt_index < finish_index < current_key_index
    assert current_key_index < plaintext_index < duplicate_index < ready_index
    assert ready_index < append_index
    assert "D1L_CHANNEL_STORE_CAPACITY" in receive
    assert "d1l_channel_store_copy_hash_matches" not in receive
    assert "d1l_channel_store_find_unique_hash" not in receive
    assert "channel_protocol_keys_equal" in receive
    assert "channel_rx_unknown_hash++" in receive
    assert "channel_rx_hash_collision++" in receive
    assert "secure_zero_bytes(candidates, sizeof(candidates))" in receive
    assert "channel_id, channel.name" in receive

    gate = c_function(
        service, "static bool channel_message_generation_ready("
    )
    assert "d1l_channel_message_reconcile_pending()" in gate
    assert "d1l_channel_message_reconcile()" in gate
    assert "return false" in gate


def test_rejected_channel_rx_is_retained_side_effect_free_in_production_order():
    service = read("main/mesh/meshcore_service.c")
    receive = c_function(service, "static void parse_rx_channel_packet(")

    ready_index = receive.index("channel_message_generation_ready()")
    append_index = receive.index("append_channel_message_store_rx(")
    route_index = receive.index("d1l_route_store_upsert_observation(")
    packet_log_index = receive.index("append_packet_log_deferred(")
    remember_index = receive.index("d1l_meshcore_packet_hash_cache_remember(")
    assert ready_index < append_index < remember_index < route_index < packet_log_index

    adversarial_rejections = (
        (
            "if (!d1l_meshcore_channel_dispatch_begin(&dispatch, candidate_count))",
            "d1l_meshcore_channel_dispatch_observe(",
        ),
        (
            "if (!d1l_meshcore_channel_dispatch_observe(",
            "secure_zero_bytes(candidates, sizeof(candidates));\n\n    size_t selected_index",
        ),
        (
            "if (dispatch_outcome != D1L_MESHCORE_CHANNEL_DISPATCH_ACCEPTED)",
            "d1l_channel_protocol_key_t current_key",
        ),
        ("if (!selected_current ||", "if (plain_len < 6U"),
        (
            "if (plain_len < 6U || (plain[4] >> 2) != 0)",
            "d1l_meshcore_text_plaintext_view_t text_view",
        ),
        (
            "if (!d1l_meshcore_text_plaintext_view(",
            "plain[5U + text_view.text_length]",
        ),
        (
            "if (packet_hash_ready && d1l_meshcore_packet_hash_cache_contains(",
            "if (!channel_message_generation_ready())",
        ),
    )
    retained_mutations = (
        "channel_message_generation_ready()",
        "append_channel_message_store_rx(",
        "d1l_meshcore_packet_hash_cache_remember(",
        "d1l_route_store_upsert_observation(",
        "append_packet_log_deferred(",
    )
    for start_marker, end_marker in adversarial_rejections:
        start = receive.index(start_marker)
        end = receive.index(end_marker, start)
        rejection = receive[start:end]
        assert "return;" in rejection
        assert end <= ready_index
        for mutation in retained_mutations:
            assert mutation not in rejection

    reconcile_gate = receive[
        ready_index : receive.index("char route_target[17]", ready_index)
    ]
    assert "secure_zero_bytes(plain, sizeof(plain))" in reconcile_gate
    assert "channel_rx_reconcile_blocked++" in reconcile_gate
    assert "return;" in reconcile_gate


def test_pending_history_is_bound_to_exact_channel_radio_operation():
    service = read("main/mesh/meshcore_service.c")

    admission = c_function(
        service, "esp_err_t d1l_meshcore_service_send_channel("
    )
    assert "__atomic_compare_exchange_n" in admission
    assert "meshcore_service_send_channel_owned" in admission
    assert "__atomic_store_n(&s_channel_send_admission, 0U" in admission

    begin = c_function(
        service, "static esp_err_t meshcore_service_handle_send_raw("
    )
    assert "operation_kind =\n        cmd->requested_tx_kind" in begin
    assert "else if (s_pending_channel_tx)" not in begin
    assert "operation_kind == D1L_MESH_TX_OPERATION_PUBLIC" in begin
    assert "s_pending_channel_operation_id" in begin
    assert "s_active_radio_tx.operation_id" in begin

    channel_send = c_function(
        service, "static esp_err_t meshcore_service_send_channel_owned("
    )
    assert "meshcore_service_send_raw_kind(" in channel_send
    assert "D1L_MESH_TX_OPERATION_PUBLIC" in channel_send
    generic_send = c_function(
        service, "static esp_err_t meshcore_service_send_raw("
    )
    assert "D1L_MESH_TX_OPERATION_GENERIC" in generic_send
    reciprocal_send = c_function(
        service, "static esp_err_t meshcore_service_queue_raw_response("
    )
    assert ".requested_tx_kind = D1L_MESH_TX_OPERATION_GENERIC" in reciprocal_send
    assert "s_pending_channel_tx" not in reciprocal_send

    flush = c_function(service, "static void flush_pending_channel_tx(")
    assert "operation->kind != D1L_MESH_TX_OPERATION_PUBLIC" in flush
    assert "operation->operation_id != s_pending_channel_operation_id" in flush
    assert "append_channel_message_store_tx" in flush
    assert flush.index("d1l_meshcore_packet_hash_cache_remember") < flush.index(
        "append_channel_message_store_tx"
    )
    assert "s_pending_channel_packet_hash_ready" in flush

    done = c_function(
        service, "static void meshcore_service_handle_radio_tx_done("
    )
    timeout = c_function(
        service, "static void meshcore_service_handle_radio_tx_timeout("
    )
    assert done.index("meshcore_radio_terminal_matches") < done.index(
        "flush_pending_channel_tx"
    )
    assert "event->tx_operation.kind == D1L_MESH_TX_OPERATION_PUBLIC" in done
    assert "flush_pending_channel_tx" not in done.split(
        "D1L_MESH_TX_OPERATION_PUBLIC", 1
    )[0]
    assert "s_pending_channel_operation_id ==" in timeout
    assert "event->tx_operation.operation_id" in timeout
    assert "clear_pending_channel_tx" in timeout


def test_public_clear_is_scoped_and_mark_order_preserves_retry_affordance():
    source = read("main/mesh/message_store.c")
    console = read("main/comms/usb_console.c")
    app = read("main/app/app_model.c")

    clear_channel = c_function(
        source, "esp_err_t d1l_message_store_clear_channel("
    )
    assert "entry->channel_id != channel_id" in clear_channel
    assert "s_next_seq =" not in clear_channel
    assert "s_clear_lineage = new_clear_lineage()" in clear_channel
    clear_command = c_function(console, "static void cmd_messages_clear(")
    assert "d1l_message_store_clear_channel(" in clear_command
    assert "D1L_CHANNEL_PUBLIC_ID" in clear_command
    assert "d1l_message_store_clear()" not in clear_command

    mark_public = c_function(app, "esp_err_t d1l_app_model_mark_public_read(")
    reconcile_index = mark_public.index("d1l_channel_message_reconcile")
    channel_index = mark_public.index("d1l_channel_store_mark_all_read")
    primary_index = mark_public.index("d1l_read_state_mark_public_read")
    assert reconcile_index < channel_index < primary_index
