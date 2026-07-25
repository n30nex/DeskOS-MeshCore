from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def c_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def test_core_rx_filters_private_keys_before_decrypt_and_retained_effects():
    service = read("main/mesh/meshcore_service.c")
    receive = c_function(service, "static void parse_rx_channel_packet(")

    candidate = receive.index("d1l_channel_store_copy_rx_hash_matches(")
    decrypt = receive.index("meshcore_decrypt_after_mac(")
    profile = receive.index("channel_available_in_release_profile(channel_id)")
    message = receive.index("append_channel_message_store_rx(")
    route = receive.index("d1l_route_store_upsert_observation(")
    packet = receive.index("append_packet_log_deferred(")
    assert candidate < decrypt < profile < message < route < packet
    assert "d1l_channel_store_copy_hash_matches(" not in receive

    store = read("main/mesh/channel_store.c")
    lookup = c_function(
        store, "size_t d1l_channel_store_copy_rx_hash_matches("
    )
    assert lookup.index("channel_available_in_release_profile(") < lookup.index(
        "copy_protocol_key("
    )


def test_core_tx_rejects_private_channel_before_reconcile_or_key_access():
    service = read("main/mesh/meshcore_service.c")
    send = c_function(
        service, "static esp_err_t meshcore_service_send_channel_owned("
    )
    profile = send.index("channel_available_in_release_profile(channel_id)")
    reconcile = send.index("channel_message_generation_ready()")
    key = send.index("d1l_channel_store_copy_protocol_key(")
    assert profile < reconcile < key
    assert "return ESP_ERR_NOT_SUPPORTED;" in send[profile:reconcile]

    active = c_function(
        service, "esp_err_t d1l_meshcore_service_send_active_channel("
    )
    public = active.index("d1l_meshcore_service_send_public(text)")
    retained_default = active.index("d1l_channel_store_find_default(")
    assert public < retained_default


def test_core_reconciliation_compacts_public_rows_and_never_mutates_private_entries():
    app_main = read("main/app_main.c")
    app_model = read("main/app/app_model.c")
    coordinator = read("main/mesh/channel_message_coordinator.c")
    reconcile = c_function(
        coordinator, "esp_err_t d1l_channel_message_reconcile("
    )
    assert "d1l_channel_message_reconcile();" in app_main
    assert app_model.count("d1l_channel_message_reconcile();") >= 2
    profile = reconcile.index("d1l_release_profile_is_core()")
    append = reconcile.index("s_channel_rows[admitted_row_count++]")
    handoff = reconcile.index("d1l_channel_store_reconcile_retained_rows(")
    assert profile < append < handoff
    assert "s_channel_rows, admitted_row_count" in reconcile[handoff:]

    store = read("main/mesh/channel_store.c")
    retained = c_function(
        store, "esp_err_t d1l_channel_store_reconcile_retained_rows("
    )
    assert retained.count("channel_available_in_release_profile(") >= 6
    first_profile = retained.index("channel_available_in_release_profile(")
    first_index_lookup = retained.index("index_by_id(rows[i].channel_id)")
    assert first_profile < first_index_lookup
    mutation = retained.index("entry->newest_message_seq = newest;")
    final_profile = retained.rindex("channel_available_in_release_profile(")
    assert final_profile < mutation
