from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_protocol_worker_owns_dispatch_outside_nimble_callbacks():
    transport = read("main/comms/ble_companion.c")
    protocol = read("main/comms/ble_companion_protocol.c")
    cmake = read("main/CMakeLists.txt")

    assert '"comms/ble_companion_protocol.c"' in cmake
    assert "d1l_ble_companion_protocol_start()" in transport
    assert "d1l_ble_companion_protocol_stop()" in transport
    assert "d1l_ble_companion_take_rx_frame" in protocol
    assert "d1l_ble_companion_queue_tx_frame" in protocol
    assert "xTaskCreate(" in protocol
    assert "d1l_app_model_" not in transport
    assert "mesh/" not in transport


def test_official_initial_sync_and_messaging_commands_are_real():
    protocol = read("main/comms/ble_companion_protocol.c")

    for command in (
        "CMD_APP_START",
        "CMD_DEVICE_QUERY",
        "CMD_GET_CONTACTS",
        "CMD_GET_CHANNEL",
        "CMD_GET_DEVICE_TIME",
        "CMD_SET_DEVICE_TIME",
        "CMD_SYNC_NEXT_MESSAGE",
        "CMD_RESET_PATH",
        "CMD_SEND_TXT_MSG",
        "CMD_SEND_CHANNEL_TXT_MSG",
        "CMD_SEND_LOGIN",
        "CMD_SEND_STATUS_REQ",
        "CMD_HAS_CONNECTION",
        "CMD_LOGOUT",
        "CMD_SEND_TELEMETRY_REQ",
        "CMD_SEND_BINARY_REQ",
        "CMD_SET_CHANNEL",
        "CMD_GET_STATS",
        "CMD_SET_FLOOD_SCOPE_KEY",
    ):
        assert f"case {command}:" in protocol
    for response in (
        "RESP_CODE_SELF_INFO",
        "RESP_CODE_DEVICE_INFO",
        "RESP_CODE_CONTACTS_START",
        "RESP_CODE_CONTACT",
        "RESP_CODE_END_OF_CONTACTS",
        "RESP_CODE_CHANNEL_INFO",
        "RESP_CODE_CONTACT_MSG_RECV_V3",
        "RESP_CODE_CHANNEL_MSG_RECV_V3",
        "RESP_CODE_STATS",
        "PUSH_CODE_MSG_WAITING",
        "PUSH_CODE_LOGIN_SUCCESS",
        "PUSH_CODE_LOGIN_FAIL",
        "PUSH_CODE_STATUS_RESPONSE",
        "PUSH_CODE_TELEMETRY_RESPONSE",
        "PUSH_CODE_BINARY_RESPONSE",
    ):
        assert response in protocol
    assert "d1l_app_model_send_dm_text" in protocol
    assert "d1l_app_model_send_channel_text" in protocol
    assert "d1l_time_service_set_companion_time" in protocol
    assert "d1l_app_model_set_companion_map_location" in protocol
    assert "bytes_all_zero(&payload[2], D1L_CHANNEL_NAME_LEN - 1U)" in protocol
    assert "d1l_app_model_remove_channel(" in protocol
    assert "static d1l_channel_store_stats_t s_channel_stats" in protocol
    assert protocol.count("&s_channel_stats") == 3


def test_repeater_login_and_management_use_the_existing_admin_runtime():
    protocol = read("main/comms/ble_companion_protocol.c")
    dispatch_h = read("main/mesh/meshcore_admin_dispatch.h")
    runtime_h = read("main/mesh/meshcore_admin_runtime.h")
    runtime = read("main/mesh/meshcore_admin_runtime.c")

    login = protocol.split("static void send_login_command", 1)[1].split(
        "static void begin_admin_status_command", 1
    )[0]
    assert "d1l_meshcore_service_admin_logout()" in login
    assert "d1l_meshcore_service_admin_login(" in login
    assert "D1L_BLE_ADMIN_REQUEST_LOGIN" in login
    assert "set_admin_sent_response(true" in login

    for call in (
        "d1l_meshcore_service_admin_request_status()",
        "d1l_meshcore_service_admin_request_query(query, offset)",
        "d1l_meshcore_service_admin_request_cli(command, true)",
    ):
        assert call in protocol
    assert "payload[1] != 0U && payload[1] != 1U" in protocol
    assert "s_pending_payload[offset++] = 1U" in protocol
    assert "maybe_queue_admin_response();" in protocol
    assert "s_admin_snapshot.last_completed_tag == s_admin_request_tag" in protocol

    assert "D1L_MESHCORE_ADMIN_MAX_QUERY_WIRE_BYTES 251U" in dispatch_h
    assert "uint16_t query_wire_len;" in dispatch_h
    assert "uint8_t query_wire[D1L_MESHCORE_ADMIN_MAX_QUERY_WIRE_BYTES];" in dispatch_h
    assert "uint32_t last_completed_tag;" in runtime_h
    assert "snapshot.last_completed_tag = s_session.last_completed_tag;" in runtime
    assert "memcpy(snapshot.query_wire, s_session.query_wire" in runtime


def test_repeater_path_reset_uses_the_full_contact_key_and_safe_route_reset():
    protocol = read("main/comms/ble_companion_protocol.c")

    reset = protocol.split(
        "static void reset_contact_path_command", 1
    )[1].split("static void get_contact_command", 1)[0]
    assert "length != 33U" in reset
    assert "contact_for_public_key(&payload[1], &contact)" in reset
    assert "d1l_meshcore_service_reset_contact_route(contact.fingerprint)" in reset
    assert "case CMD_RESET_PATH:" in protocol


def test_admin_and_phone_commands_preempt_bulk_contact_sync():
    protocol = read("main/comms/ble_companion_protocol.c")
    worker = protocol.split("static void protocol_task", 1)[1].split(
        "esp_err_t d1l_ble_companion_protocol_start", 1
    )[0]

    admin = worker.index("maybe_queue_admin_response();")
    receive = worker.index("d1l_ble_companion_take_rx_frame(")
    contacts = worker.index("prepare_next_contact_response();")
    assert admin < receive < contacts


def test_reconnect_preserves_message_watermarks_without_replaying_history():
    protocol = read("main/comms/ble_companion_protocol.c")
    reset = protocol.split("static void reset_session_state", 1)[1].split(
        "static uint32_t last_existing_seq", 1
    )[0]
    initialize = protocol.split(
        "static void initialize_message_sync_watermarks", 1
    )[1].split("static void maybe_queue_message_waiting", 1)[0]
    worker = protocol.split("static void protocol_task", 1)[1].split(
        "esp_err_t d1l_ble_companion_protocol_start", 1
    )[0]

    assert "s_last_synced_dm_seq" not in reset
    assert "s_last_synced_message_seq" not in reset
    assert "s_seen_dm_revision" not in reset
    assert "s_seen_message_revision" not in reset
    assert "last_existing_seq(dm.next_seq)" in initialize
    assert "last_existing_seq(messages.next_seq)" in initialize
    assert worker.index("initialize_message_sync_watermarks();") < worker.index(
        "reset_session_state();"
    )


def test_current_phone_stats_and_multibyte_paths_follow_official_wire_shape():
    protocol = read("main/comms/ble_companion_protocol.c")

    assert "case CMD_GET_STATS:" in protocol
    assert "case STATS_TYPE_CORE:" in protocol
    assert "case STATS_TYPE_RADIO:" in protocol
    assert "case STATS_TYPE_PACKETS:" in protocol
    assert "s_pending_len = 11U" in protocol
    assert "s_pending_len = 14U" in protocol
    assert "s_pending_len = 30U" in protocol
    assert "d1l_meshcore_wire_path_byte_len(path_len)" in protocol
    assert "dest[offset++] = path_len" in protocol

    path_mode = protocol.split(
        "static void set_path_hash_command", 1
    )[1].split("static void dispatch_command", 1)[0]
    assert "length < 3U" in path_mode
    assert "payload[1] != 0U" in path_mode
    assert "payload[2] > 2U" in path_mode
    assert "payload[2] + 1U" in path_mode
    assert "last_unsupported_command = s_status.last_command" in protocol

    flood_scope = protocol.split(
        "static void set_flood_scope_command", 1
    )[1].split("static void dispatch_command", 1)[0]
    assert "length != 2U" in flood_scope
    assert "payload[1] > 1U" in flood_scope
    assert "set_result_response(ESP_OK)" in flood_scope
    assert "case CMD_SET_FLOOD_SCOPE_KEY:" in protocol

    transport = read("main/comms/ble_companion.c")
    console = read("main/comms/usb_console.c")
    assert "protocol.last_unsupported_command" in transport
    assert "protocol_last_unsupported_command" in console


def test_phone_startup_uses_protocol_owned_storage_and_official_contact_count():
    protocol = read("main/comms/ble_companion_protocol.c")

    battery = protocol.split("static void build_battery_storage(void)", 1)[1].split(
        "static uint32_t message_epoch", 1
    )[0]
    assert "d1l_storage_status(&s_storage_status);" in battery
    assert "d1l_app_model_snapshot" not in battery

    contacts = protocol.split("static void begin_contact_iteration", 1)[1].split(
        "static void prepare_next_contact_response", 1
    )[0]
    assert "write_u32_le(&response[1], s_contact_count);" in contacts
    assert "filtered_count" not in contacts


def test_protocol_fails_closed_for_privileged_and_malformed_commands():
    protocol = read("main/comms/ble_companion_protocol.c")

    disabled = protocol.split("case CMD_REBOOT:", 1)[1].split(
        "default:", 1
    )[0]
    assert "CMD_FACTORY_RESET" in disabled
    assert "CMD_EXPORT_PRIVATE_KEY" in disabled
    assert "CMD_IMPORT_PRIVATE_KEY" in disabled
    assert "RESP_CODE_DISABLED" in disabled
    assert "ERR_CODE_UNSUPPORTED_CMD" in protocol
    assert "ERR_CODE_ILLEGAL_ARG" in protocol
    assert "payload_len == 0U" in protocol
    assert "frame_len != D1L_COMPANION3_HEADER_SIZE + payload_len" in protocol


def test_pairing_is_interoperable_bonded_and_locally_manageable():
    transport = read("main/comms/ble_companion.c")
    ui = read("main/ui/ui_connectivity.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "D1L_BLE_COMPANION_STATIC_PASSKEY 123456U" in transport
    assert "desc.sec_state.encrypted" in transport
    assert "desc.sec_state.authenticated" in transport
    assert "desc.sec_state.bonded" in transport
    assert "ble_store_util_delete_peer" in transport
    assert "d1l_ble_companion_begin_pairing" in transport
    assert "Enter this PIN in the MeshCore app." in ui
    assert "s_pairing_passkey = D1L_BLE_COMPANION_STATIC_PASSKEY" in transport
    assert "d1l_app_model_ble_begin_pairing" in phase1
    assert "d1l_app_model_ble_forget_peer" in phase1
    for line in transport.splitlines():
        if "LOG" in line or "printf" in line:
            assert "passkey" not in line.lower()
