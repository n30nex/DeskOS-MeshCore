from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_packet_hash_matches_pinned_upstream_framing_and_bounds():
    header = read("main/mesh/meshcore_packet_hash.h")
    source = read("main/mesh/meshcore_packet_hash.c")

    assert "#define D1L_MESHCORE_PACKET_HASH_BYTES 8U" in header
    assert "#define D1L_MESHCORE_PACKET_HASH_CACHE_CAPACITY 160U" in header
    assert "uint8_t occupied[D1L_MESHCORE_PACKET_HASH_OCCUPANCY_BYTES]" in header
    assert "material[material_len++] = packet->type;" in source
    trace = source.split(
        "if (packet->type == D1L_MESHCORE_PAYLOAD_TRACE)", 1
    )[1].split("memcpy(&material[material_len]", 1)[0]
    assert "material[material_len++] = packet->path_len;" in trace
    assert "material[material_len++] = 0U;" in trace
    assert "mbedtls_md(md, material, material_len, digest)" in source
    assert "memcpy(out_hash, digest, D1L_MESHCORE_PACKET_HASH_BYTES)" in source
    assert "malloc" not in source


def receiver(service: str, name: str, next_name: str) -> str:
    return service.split(name, 1)[1].split(next_name, 1)[0]


def test_signed_advert_duplicate_probe_preserves_retryable_storage_failures():
    service = read("main/mesh/meshcore_service.c")
    advert_rx = service.split("static void parse_rx_advert_packet", 1)[1].split(
        "static void meshcore_service_handle_radio_tx_done", 1
    )[0]

    signature = advert_rx.index("verify_advert_signature(")
    self_check = advert_rx.index("memcmp(pub_key, settings->identity_public_key")
    app_parse = advert_rx.index("d1l_advert_data_parse(")
    hash_calculate = advert_rx.index("d1l_meshcore_packet_hash_calculate(")
    hash_probe = advert_rx.index("d1l_meshcore_packet_hash_cache_contains(")
    admission = advert_rx.index("d1l_meshcore_advert_admit_verified(")
    outcome_dispatch = advert_rx.index("switch (admission.outcome)")
    cache_policy = advert_rx.index(
        "d1l_meshcore_advert_admission_receipt_cacheable(&admission)"
    )
    remember = advert_rx.index("d1l_meshcore_packet_hash_cache_remember(")
    assert (
        signature
        < self_check
        < app_parse
        < hash_calculate
        < hash_probe
        < admission
        < outcome_dispatch
        < cache_policy
        < remember
    )
    assert '"advert_duplicate"' not in advert_rx
    assert "append_packet_log_deferred(" in advert_rx
    assert "route_ret == ESP_OK && packet_retained && packet_hash_ready" in advert_rx


def test_every_rx_family_uses_semantic_authority_before_hash_suppression():
    service = read("main/mesh/meshcore_service.c")

    channel = receiver(
        service, "static void parse_rx_channel_packet", "static bool parse_rx_dm_packet"
    )
    assert channel.index("d1l_channel_store_copy_rx_hash_matches(") < channel.index(
        "meshcore_decrypt_after_mac("
    ) < channel.index("d1l_meshcore_channel_dispatch_finish(") < channel.index(
        "d1l_meshcore_text_plaintext_view("
    ) < channel.index(
        "channel_metadata("
    ) < channel.index("d1l_meshcore_packet_hash_cache_contains(")
    assert channel.index("append_channel_message_store_rx(") < channel.index(
        "d1l_meshcore_packet_hash_cache_remember("
    ) < channel.index("d1l_route_store_upsert_observation(")
    assert "message_result.admitted" in channel
    assert "append_packet_log_deferred(" in channel
    assert "d1l_channel_store_find_unique_hash(" not in channel

    dm = receiver(
        service, "static bool parse_rx_dm_packet", "typedef enum {\n    D1L_RX_ACK_UNMATCHED"
    )
    plain_marker = "const bool room_post ="
    dm_plain = dm.split(plain_marker, 1)[1]
    assert dm_plain.index("d1l_meshcore_text_plaintext_view(") < dm_plain.index(
        "d1l_dm_store_find_rx_identity_loaded("
    ) < dm_plain.index(
        "d1l_meshcore_packet_hash_cache_contains("
    )
    assert dm.index("meshcore_decrypt_after_mac(") < dm.index(plain_marker)
    assert "D1L_MESHCORE_TXT_TYPE_SIGNED_PLAIN" in dm_plain
    assert "d1l_meshcore_admin_runtime_note_room_activity(" in dm_plain
    assert "room_post ? settings->identity_public_key : sender_pub" in dm_plain
    assert "room_post ? 4U : D1L_MESHCORE_DM_ACK_WIRE_BYTES" in dm_plain
    assert 'room_post ? "room_post" : "dm_text"' in dm_plain
    hit = dm_plain.split("d1l_meshcore_packet_hash_cache_contains(", 1)[1].split(
        "if (duplicate)", 1
    )[0]
    assert "duplicate &&" in dm_plain[dm_plain.index("packet_hash_ready"):dm_plain.index(
        "d1l_meshcore_packet_hash_cache_contains("
    )]
    assert "dispatch_bounded_dm_ack(" in hit
    assert dm_plain.index(
        "d1l_dm_store_append_rx_identity_deferred("
    ) < dm_plain.index("store_outcome.inserted") < dm_plain.rindex(
        "d1l_meshcore_packet_hash_cache_remember("
    )
    assert "d1l_dm_store_append_rx_identity(" not in dm_plain
    assert "append_packet_log_deferred(" in dm_plain
    assert dm.index("d1l_meshcore_peer_dispatch_classify(") < dm.index(
        "derive_local_identity_shared_secret("
    )

    ack = receiver(
        service, "typedef enum {\n    D1L_RX_ACK_UNMATCHED", "static d1l_meshcore_path_response_result_t"
    )
    record = ack.split("static d1l_rx_ack_result_t record_dm_ack", 1)[1].split(
        "static void parse_rx_ack_packet", 1
    )[0]
    assert record.index("d1l_dm_delivery_owner_ack_matches(") < record.index(
        "transition_pending_dm_ack("
    ) < record.index("finalize_pending_dm_ack_completion()")
    finalizer = service.split(
        "static bool finalize_pending_dm_ack_completion(void)", 2
    )[-1].split("static void clear_pending_ack_tx", 1)[0]
    assert finalizer.index("d1l_meshcore_ack_completion_take_terminal_effects(") < finalizer.index(
        "record_pending_direct_path_result(true)"
    )
    assert "d1l_meshcore_ack_completion_suppresses_rf_retry(" in record
    assert "D1L_RX_ACK_ALREADY_ACCEPTED" in record
    parser = ack.split("static void parse_rx_ack_packet", 1)[1]
    assert "hash_packet.payload = &packet.payload[1]" in parser
    assert "owner_needs_packet_retry" in parser
    assert "record_dm_ack(" in parser
    assert "bank_pending_dm_ack_packet_hash(packet_hash)" in record
    assert finalizer.index("d1l_meshcore_ack_completion_take_terminal_effects(") < finalizer.index(
        "remember_pending_dm_ack_packet_hashes()"
    )

    path = receiver(
        service, "static void parse_rx_path_packet", "static void parse_rx_trace_packet"
    )
    assert path.index("meshcore_decrypt_after_mac(") < path.index(
        "d1l_meshcore_path_plain_decode("
    ) < path.index("d1l_meshcore_packet_hash_cache_contains(")
    assert path.index("d1l_meshcore_path_replay_take(") < path.index(
        "d1l_contact_store_update_path_from_source("
    ) < path.rindex("d1l_meshcore_packet_hash_cache_remember(")
    assert '"path_ack_retry"' in path
    assert path.count("d1l_meshcore_path_replay_forget(") == 1
    assert path.index("d1l_meshcore_path_replay_forget(") < path.index(
        "d1l_route_store_upsert_observation("
    )
    assert "append_packet_log_deferred(" in path

    trace = receiver(
        service, "static void parse_rx_trace_packet", "static bool verify_advert_signature"
    )
    assert trace.index("d1l_meshcore_trace_classify(") < trace.index(
        "d1l_meshcore_packet_hash_cache_contains("
    ) < trace.index("d1l_meshcore_trace_tracker_consume(")
    assert "packet_hash_ready && !trace_pending" in trace
    assert "retention_retry" in trace
    assert "append_packet_log_deferred(" in trace
    assert trace.index("d1l_meshcore_trace_tracker_consume(") < trace.rindex(
        "d1l_meshcore_packet_hash_cache_remember("
    )


def test_admission_receipt_cache_policy_is_terminal_and_explicit():
    source = read("main/mesh/meshcore_advert_admission.c")
    policy = source.split(
        "bool d1l_meshcore_advert_admission_receipt_cacheable", 1
    )[1].split("esp_err_t d1l_meshcore_advert_admit_verified", 1)[0]

    assert "D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED" in policy
    assert "receipt->contact_store_error == ESP_OK" in policy
    assert "D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_SUCCEEDED" in policy
    assert "D1L_MESHCORE_ADVERT_ADMISSION_REPLAY_REJECTED" in policy
    for outcome in (
        "D1L_MESHCORE_ADVERT_ADMISSION_INVALID",
        "D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_FAILED",
        "D1L_MESHCORE_ADVERT_ADMISSION_KEY_COLLISION",
        "D1L_MESHCORE_ADVERT_ADMISSION_NODE_STORE_ERROR",
    ):
        assert outcome in policy


def test_packet_hash_cache_is_boot_local_and_firmware_linked():
    service = read("main/mesh/meshcore_service.c")
    init = service.split("void d1l_meshcore_service_init", 1)[1]
    first_init = init.split("if (s_service_initialized)", 1)[1]
    assert (
        "d1l_meshcore_packet_hash_cache_reset(&s_rx_packet_hash_cache);"
        in first_init
    )
    assert '"mesh/meshcore_packet_hash.c"' in read("main/CMakeLists.txt")
