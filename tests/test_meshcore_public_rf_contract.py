from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


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


def test_meshcore_service_builds_public_group_text_packets():
    source = read("main/mesh/meshcore_service.c")
    crypto = read("main/mesh/meshcore_crypto.c")
    channel_store = read("main/mesh/channel_store.c")
    wire = read("main/mesh/meshcore_wire.h")
    assert "D1L_MESHCORE_HEADER_GROUP_TEXT_FLOOD" in source
    assert "D1L_MESHCORE_PAYLOAD_GROUP_TEXT 0x05U" in wire
    assert "0x8b, 0x33, 0x87, 0xe9" in channel_store
    assert "s_public_secret" not in source
    assert "d1l_channel_store_copy_protocol_key" in source
    assert "d1l_meshcore_crypto_encrypt_then_mac(" in source
    assert "d1l_meshcore_crypto_decrypt_after_mac(" in source
    assert "mbedtls_aes_crypt_ecb" in crypto
    assert "mbedtls_md_hmac" in crypto
    assert "meshcore_service_send_raw_kind(" in source
    assert "Radio.SendWithOrigin(" in source
    assert "cmd->raw, cmd->raw_len," in source


def test_meshcore_service_rejects_139_byte_or_invalid_utf8_text_without_truncation():
    source = read("main/mesh/meshcore_service.c")
    console = read("main/comms/usb_console.c")
    ui = read("main/ui/ui_phase1.c")

    assert "#define D1L_MESHCORE_USER_TEXT_MAX D1L_MESSAGE_MAX_CHARS" in source
    assert "D1L_MESHCORE_USER_TEXT_MAX == 138U" in source
    assert "return ESP_ERR_INVALID_SIZE;" in source
    assert "validate_user_text(text)" in source

    public_builder = c_function(source, "static esp_err_t build_channel_text_packet(")
    dm_builder = c_function(source, "static esp_err_t build_dm_text_packet(")
    public_sender = c_function(
        source, "static esp_err_t meshcore_service_send_channel_owned("
    )
    dm_sender = c_function(
        source, "static esp_err_t meshcore_service_handle_send_dm("
    )

    assert "validate_user_text(text)" in public_builder
    assert "validate_user_text(text)" in dm_builder
    assert 'snprintf((char *)&plain[5]' not in public_builder
    assert 'snprintf((char *)&plain[5]' not in dm_builder
    assert "strnlen(sender_name, D1L_NODE_NAME_LEN)" in public_builder
    assert "memcpy(&plain[5], sender_name, sender_name_len)" in public_builder
    assert "plain[5U + sender_name_len] = ':'" in public_builder
    assert "plain[6U + sender_name_len] = ' '" in public_builder
    assert "memcpy(&plain[message_offset], text, message_len)" in public_builder
    assert "memcpy(&plain[5], text, message_len)" in dm_builder
    assert "validate_user_text(text)" in public_sender
    assert public_sender.index("validate_user_text(text)") < public_sender.index(
        "meshcore_service_queue_public_raw("
    )
    assert "D1L_MESHCORE_SERVICE_CMD_START_RX" not in public_sender
    assert "meshcore_service_queue_public_raw(" in public_sender
    assert "meshcore_service_send_raw_kind(" not in public_sender
    assert "append_packet_log_deferred(" not in public_sender
    assert "append_packet_log(\"tx\"" not in public_sender
    assert "if (s_tx_busy)" not in public_sender
    assert "validate_user_text(cmd->dm_text)" in dm_sender
    assert dm_sender.index("validate_user_text(cmd->dm_text)") < dm_sender.index(
        "d1l_meshcore_service_ensure_identity()"
    )

    assert "MESSAGE_TOO_LONG" in console
    assert "max 138 UTF-8 bytes" in console
    assert '"mesh/user_text.c"' in read("main/CMakeLists.txt")
    assert "d1l_user_text_validate(text)" in source
    assert "d1l_user_text_copy(" in source
    assert "s_pending_channel_text, sizeof(s_pending_channel_text), message" in source
    assert "s_pending_dm_tx.text," in source
    assert "lv_textarea_set_max_length(s_compose_textarea, D1L_MESSAGE_MAX_CHARS)" in ui


def test_public_tx_history_retries_nonblocking_exact_terminal_stages_before_release():
    source = read("main/mesh/meshcore_service.c")
    remember = c_function(source, "static esp_err_t remember_pending_channel_tx(")
    terminal = c_function(source, "static void mark_pending_channel_tx_done(")
    history = c_function(
        source, "static bool maintain_pending_channel_history("
    )
    sender = c_function(
        source, "static esp_err_t meshcore_service_send_channel_owned("
    )
    message = c_function(
        source, "static bool append_channel_message_store_tx("
    )
    degraded_handler = c_function(
        source, "static void fail_pending_channel_history_degraded("
    )
    maintenance = c_function(
        source, "static void meshcore_service_run_owner_maintenance("
    )
    timeout = c_function(
        source, "static void meshcore_service_handle_radio_tx_timeout("
    )

    assert "memcpy(s_pending_channel_raw, raw, raw_len);" in remember
    assert "s_pending_channel_raw_len = raw_len;" in remember
    assert "s_pending_channel_path_hash_bytes = path_hash_bytes;" in remember
    assert "s_pending_channel_name" in remember
    assert "s_pending_channel_author" in remember
    assert "s_pending_channel_tx_completed = false;" in remember

    completion = terminal.index("s_pending_channel_tx_completed = true;")
    packet_hash = terminal.index(
        "d1l_meshcore_packet_hash_cache_remember(", completion
    )
    packet = history.index("append_packet_log_deferred(")
    visible = history.index("append_channel_message_store_tx(", packet)
    route = history.index(
        "d1l_route_store_upsert_observation_deferred(", visible
    )
    all_stages = history.index(
        "s_pending_channel_packet_history_admitted &&", route
    )
    release = history.index("clear_pending_channel_tx();", all_stages)
    assert completion < packet_hash
    assert "s_pending_channel_history_attempted = false;" in terminal
    assert packet < visible < route < all_stages < release
    assert "append_packet_log_deferred(" not in terminal
    assert "append_channel_message_store_tx(" not in terminal
    assert "s_pending_channel_raw" in history
    assert "s_pending_channel_text" in history
    assert "s_pending_channel_packet_history_admitted =" in history
    assert "s_pending_channel_message_history_admitted =" in history
    assert "s_pending_channel_route_history_admitted =" in history
    assert "D1L_MESHCORE_PUBLIC_HISTORY_RETRY_INTERVAL_MS" in history
    assert "D1L_MESHCORE_PUBLIC_HISTORY_RETRY_TIMEOUT_MS" in source
    assert "d1l_message_store_append_channel_deferred(" in message
    assert "d1l_message_store_append_channel(" not in message
    assert "reconcile_channel_messages(" not in message
    assert "public_tx_history_failures" in degraded_handler
    assert "clear_pending_channel_tx();" in degraded_handler
    assert "d1l_route_store_upsert_observation(" not in sender
    assert "if (s_tx_busy)" in maintenance
    assert "pending_channel_history_deadline_expired(now_ms)" in maintenance
    assert "maintain_pending_channel_history(now_ms)" in maintenance
    assert "clear_pending_channel_tx();" in timeout


def test_authenticated_inbound_text_is_validated_before_visible_or_ack_side_effects():
    source = read("main/mesh/meshcore_service.c")
    public_rx = c_function(source, "static void parse_rx_channel_packet(")
    dm_rx = c_function(source, "static bool parse_rx_dm_packet(")
    public_validate = public_rx.index("d1l_meshcore_text_plaintext_view(")
    assert public_validate < public_rx.index("s_status.rx_packets++")
    assert public_validate < public_rx.index("d1l_route_store_upsert_observation")
    assert public_validate < public_rx.index("append_channel_message_store_rx")
    dm_validate = dm_rx.index("d1l_meshcore_text_plaintext_view(")
    for side_effect in [
        "calc_dm_ack_hash",
        "calc_dm_identity_digest",
        "d1l_dm_store_append_rx_identity",
        "dispatch_bounded_dm_ack",
    ]:
        assert dm_validate < dm_rx.index(side_effect)


def test_meshcore_service_decodes_verified_adverts():
    source = read("main/mesh/meshcore_service.c")
    admission = read("main/mesh/meshcore_advert_admission.c")
    wire = read("main/mesh/meshcore_wire.h")
    cmake = read("main/CMakeLists.txt")
    header = read("main/mesh/meshcore_service.h")
    assert "D1L_MESHCORE_PAYLOAD_ADVERT 0x04U" in wire
    assert "D1L_MESHCORE_ADVERT_MIN_PAYLOAD" in source
    assert "ed25519_verify" in source
    assert "d1l_meshcore_advert_admit_verified" in source
    assert "d1l_node_store_upsert_advert" in admission
    assert '"mesh/meshcore_advert_admission.c"' in cmake
    assert 'append_packet_log_deferred(\n        "rx", "advert"' in source
    assert "parse_rx_advert_packet(payload, size, rssi, snr)" in source
    assert "../third_party/MeshCore/lib/ed25519/verify.c" in cmake
    assert "uint32_t rx_adverts;" in header


def test_meshcore_service_generates_identity_and_signed_adverts():
    source = read("main/mesh/meshcore_service.c")
    cmake = read("main/CMakeLists.txt")
    assert "ed25519_create_keypair" in source
    assert "ed25519_sign" in source
    assert "d1l_secure_random_fill" in source
    assert "d1l_settings_next_mesh_timestamp" in source
    assert "build_advert_packet" in source
    assert '"tx", "advert", 0, 0' in source
    assert "d1l_packet_log_append_raw_deferred" in source
    assert "../third_party/MeshCore/lib/ed25519/keypair.c" in cmake
    assert "../third_party/MeshCore/lib/ed25519/sign.c" in cmake


def test_meshcore_identity_generation_preserves_inconsistent_persisted_material():
    source = read("main/mesh/meshcore_service.c")
    body = source.split("esp_err_t d1l_meshcore_service_ensure_identity(void)", 1)[
        1
    ].split("d1l_meshcore_service_status_t d1l_meshcore_service_status", 1)[0]

    load_status = body.index("d1l_settings_load_status()")
    current = body.index("d1l_settings_identity_secret_snapshot(&identity)")
    classify = body.index("d1l_identity_state_classify(")
    reject = body.index("persisted_state == D1L_IDENTITY_STATE_INCONSISTENT")
    random = body.index("d1l_secure_random_fill")
    save = body.index("d1l_settings_save_identity_if_absent")
    assert load_status < current < classify < reject < random < save

    unreadable = body.split("if (load_status != ESP_OK)", 1)[1].split(
        "d1l_settings_identity_secret_t identity", 1
    )[0]
    assert "s_status.identity_ready = false;" in unreadable
    assert "return load_status;" in unreadable
    assert "d1l_settings_identity_secret_snapshot" not in unreadable
    assert "d1l_settings_save" not in unreadable

    inconsistent = body.split(
        "persisted_state == D1L_IDENTITY_STATE_INCONSISTENT", 1
    )[1].split("uint8_t seed", 1)[0]
    assert "s_status.identity_ready = false;" in inconsistent
    assert "return ESP_ERR_INVALID_STATE;" in inconsistent
    assert "d1l_settings_save" not in inconsistent
    assert "d1l_secure_random_fill" not in inconsistent

    generation = body.split("uint8_t seed", 1)[1]
    assert "d1l_identity_state_classify(" in generation
    assert "d1l_settings_identity_secret_wipe(&identity)" in generation
    assert "D1L_IDENTITY_STATE_CONSISTENT" in generation
    assert "identity_public_key[0]" not in generation


def test_meshcore_service_uses_meshcore_narrow_radio_profile():
    source = read("main/mesh/meshcore_service.c")
    assert "D1L_MESHCORE_BW_INDEX_62K5" in source
    assert "case 625:" in source
    assert "Radio.SetPublicNetwork(false)" in source
    assert "D1L_MESHCORE_PREAMBLE_LOW_SF 32U" in source
    assert "LORA_BW_062" in source
    assert "SX126xSetModulationParams(&SX126x.ModulationParams)" in source


def test_meshcore_status_getter_is_passive_and_radio_owned_by_service_task():
    source = read("main/mesh/meshcore_service.c")
    header = read("main/mesh/meshcore_service.h")
    app_main = read("main/app_main.c")
    status_body = source.split("d1l_meshcore_service_status_t d1l_meshcore_service_status", 1)[
        1
    ].split("void d1l_meshcore_service_admin_snapshot", 1)[0]

    forbidden = [
        "d1l_meshcore_service_ensure_identity",
        "ensure_radio_started",
        "d1l_meshcore_start_rx",
        "Radio.",
        "d1l_settings_save",
        "d1l_secure_random_fill",
        "ed25519_create_keypair",
    ]
    for token in forbidden:
        assert token not in status_body

    assert "d1l_meshcore_service_cmd_t" in source
    assert "xQueueCreate(D1L_MESHCORE_SERVICE_QUEUE_LEN" in source
    assert "meshcore_service_task" in source
    assert "meshcore_service_handle_start_rx" in source
    assert "meshcore_service_handle_send_raw" in source
    assert "meshcore_service_request_rx_async" not in source
    assert "SemaphoreHandle_t s_status_mutex" in source
    assert "status_lock()" in status_body
    assert "d1l_meshcore_service_status_t snapshot = s_status" in status_body
    assert "radio_apply_pending" in source
    assert "radio_apply_error" in source
    assert "radio_profile_strings_match(lhs->tcxo, rhs->tcxo)" in source
    assert "mark_radio_apply_result(&profile, ret)" in source
    assert "radio_profiles_match(&applied_profile, &current_profile)" in status_body
    assert "esp_err_t d1l_meshcore_service_start_rx_async(void);" in header
    assert "esp_err_t d1l_meshcore_service_start_rx_async(void)" in source
    start_async = source.split("esp_err_t d1l_meshcore_service_start_rx_async(void)", 1)[1].split(
        "static esp_err_t meshcore_service_send_raw", 1
    )[0]
    assert "meshcore_service_start_task()" in start_async
    assert ".type = D1L_MESHCORE_SERVICE_CMD_START_RX" in start_async
    assert "xQueueSend(s_service_queue, &cmd, 0)" in start_async
    assert "Radio." not in start_async
    assert 'ESP_LOGW(TAG, "asynchronous MeshCore RX start failed: %s"' in source
    ensure_radio = source.split("static esp_err_t ensure_radio_started(void)", 1)[1].split(
        "static void d1l_meshcore_start_rx(void)", 1
    )[0]
    assert "d1l_settings_public_snapshot(&settings_snapshot)" in ensure_radio
    assert "s_status.identity_ready = settings_snapshot.identity_ready;" in ensure_radio
    assert app_main.index("d1l_board_init()") < app_main.index(
        "d1l_meshcore_service_start_rx_async()"
    ) < app_main.index("d1l_connectivity_init()")


def test_meshcore_service_reinitialization_preserves_live_radio_and_pending_work():
    source = read("main/mesh/meshcore_service.c")
    assert "static bool s_service_initialized;" in source
    init_body = source.split("void d1l_meshcore_service_init(void)", 1)[1].split(
        "esp_err_t d1l_meshcore_service_ensure_identity", 1
    )[0]
    repeated_init = init_body.split("if (s_service_initialized)", 1)[1].split(
        "memset(&s_status", 1
    )[0]
    assert "s_status.path_hash_bytes = settings->path_hash_bytes;" in repeated_init
    assert "s_status.identity_ready = settings->identity_ready;" in repeated_init
    assert "status_unlock();" in repeated_init
    assert "return;" in repeated_init
    for destructive_reset in [
        "s_radio_started = false",
        "s_tx_busy = false",
        "s_pending_public_tx = false",
        "memset(&s_pending_dm_tx",
    ]:
        assert destructive_reset not in repeated_init
    assert "s_service_initialized = task_ret == ESP_OK;" in init_body


def test_console_reports_phase2_public_rf_evidence():
    console = read("main/comms/usb_console.c")
    assert 'phase2_public_rf' in console
    assert 'signed advert TX/RX enabled' in console
    assert '\\"rx_adverts\\":%lu' in console
    assert '\\"note\\":\\"%s\\"' in console
