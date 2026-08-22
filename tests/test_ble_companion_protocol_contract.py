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
        "CMD_ADD_UPDATE_CONTACT",
        "CMD_SYNC_NEXT_MESSAGE",
        "CMD_RESET_PATH",
        "CMD_SEND_TXT_MSG",
        "CMD_SEND_CHANNEL_TXT_MSG",
        "CMD_SEND_LOGIN",
        "CMD_SEND_STATUS_REQ",
        "CMD_HAS_CONNECTION",
        "CMD_LOGOUT",
        "CMD_SEND_TELEMETRY_REQ",
        "CMD_GET_ADVERT_PATH",
        "CMD_SEND_BINARY_REQ",
        "CMD_SET_CHANNEL",
        "CMD_SET_OTHER_PARAMS",
        "CMD_SET_TUNING_PARAMS",
        "CMD_GET_CUSTOM_VARS",
        "CMD_SET_CUSTOM_VAR",
        "CMD_GET_TUNING_PARAMS",
        "CMD_GET_STATS",
        "CMD_SET_FLOOD_SCOPE_KEY",
        "CMD_SET_AUTOADD_CONFIG",
        "CMD_GET_AUTOADD_CONFIG",
        "CMD_GET_ALLOWED_REPEAT_FREQ",
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
        "RESP_CODE_ADVERT_PATH",
        "RESP_CODE_CUSTOM_VARS",
        "RESP_CODE_TUNING_PARAMS",
        "RESP_CODE_STATS",
        "RESP_CODE_AUTOADD_CONFIG",
        "RESP_CODE_ALLOWED_REPEAT_FREQ",
        "PUSH_CODE_MSG_WAITING",
        "PUSH_CODE_ADVERT",
        "PUSH_CODE_NEW_ADVERT",
        "PUSH_CODE_PATH_UPDATED",
        "PUSH_CODE_CONTACT_DELETED",
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

    login_push = protocol.split("static void build_admin_login_push", 1)[1].split(
        "static size_t append_admin_status", 1
    )[0]
    assert "const bool expose_guest = s_admin_guest_requested" in login_push
    assert "D1L_MESHCORE_ADMIN_PERMISSION_GUEST" in login_push
    assert "s_pending_payload[offset++] = exposed_permissions" in login_push
    assert "s_admin_guest_requested = password_len == 0U" in login
    assert "s_admin_guest_fingerprint" in login
    assert "clear_admin_session_authorization()" in login

    auth_target = protocol.split(
        "static bool authenticated_admin_target", 1
    )[1].split("static bool capture_pending_admin_request", 1)[0]
    assert "admin_session_matches_or_restore(out_contact)" in auth_target

    reset = protocol.split("static void reset_session_state", 1)[1].split(
        "static uint32_t last_existing_seq", 1
    )[0]
    assert "clear_admin_session_authorization();" in reset
    assert "clear_admin_access_intent();" not in reset
    assert "Keep the requested access level across a transient BLE reconnect" in reset
    assert "clear_admin_access_intent();" in login

    connection = protocol.split(
        "static void has_connection_command", 1
    )[1].split("static void logout_command", 1)[0]
    assert "admin_state_active(s_admin_snapshot.state)" in connection
    assert "admin_session_matches_or_restore(&contact)" in connection

    access_list = protocol.split("case BINARY_REQ_ACCESS_LIST:", 1)[1].split(
        "case BINARY_REQ_NEIGHBOURS:", 1
    )[0]
    assert "admin_guest_session_matches" in access_list

    guest_session = protocol.split(
        "static bool admin_guest_session_matches", 1
    )[1].split("static void clear_admin_request", 1)[0]
    assert "s_admin_guest_requested" in guest_session
    assert "s_admin_guest_fingerprint" in guest_session
    assert "D1L_MESHCORE_ADMIN_PERMISSION_GUEST" in guest_session

    cli = protocol.split("static void send_admin_cli_command", 1)[1].split(
        "static void send_dm_command", 1
    )[0]
    assert "admin_guest_session_matches(contact)" in cli
    for suffix in ("'\\0'", "' '", "'\\t'", "'\\r'", "'\\n'"):
        assert f"payload[13U + text_len - 1U] == {suffix}" in cli
    assert cli.index("while (text_len > 0U") < cli.index(
        "memchr(&payload[13], '\\0', text_len)"
    )
    assert 'strcmp(command, "clock sync")' in cli
    assert '"time %lu"' in cli
    assert "anti-replay packet sequence may intentionally be ahead" in cli
    assert "d1l_meshcore_service_admin_request_cli(command, true)" in cli

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

    response = protocol.split(
        "static size_t build_contact_response", 1
    )[1].split("static void begin_contact_iteration", 1)[0]
    assert "D1L_BLE_PROTOCOL_OUT_PATH_UNKNOWN 0xFFU" in protocol
    assert "const bool path_known" in response
    assert "path_known ? contact->out_path_len" in response
    assert "D1L_BLE_PROTOCOL_OUT_PATH_UNKNOWN" in response
    assert "path_known && path_len > 0U" in response


def test_recent_advert_paths_are_available_to_the_phone_without_persistence():
    protocol = read("main/comms/ble_companion_protocol.c")
    service = read("main/mesh/meshcore_service.c")
    service_h = read("main/mesh/meshcore_service.h")

    advert_path = protocol.split(
        "static void get_advert_path_command", 1
    )[1].split("static void send_channel_command", 1)[0]
    assert "length < 34U || payload[1] != 0U" in advert_path
    assert "contact_for_public_key(&payload[2], &contact)" in advert_path
    assert "d1l_meshcore_service_advert_path_snapshot" in advert_path
    assert "RESP_CODE_ADVERT_PATH" in advert_path
    assert "d1l_meshcore_wire_path_byte_len(path.path_len)" in advert_path
    assert "if (path_bytes > 0U)" in advert_path
    assert "case CMD_GET_ADVERT_PATH:" in protocol

    assert "d1l_meshcore_advert_path_snapshot_t" in service_h
    assert "s_advert_paths[D1L_CONTACT_STORE_CAPACITY] EXT_RAM_BSS_ATTR" in service
    assert "remember_advert_path(pub_prefix, received_epoch, packet.path" in service
    remember = service.split("static void remember_advert_path", 1)[1].split(
        "bool d1l_meshcore_service_advert_path_snapshot", 1
    )[0]
    assert "(path_len > 0U && !path)" in remember
    assert "if (path_bytes > 0U)" in remember
    assert "clear_advert_paths();" in service


def test_phone_contact_updates_use_the_official_wire_shape_and_notify_paths():
    protocol = read("main/comms/ble_companion_protocol.c")

    update = protocol.split(
        "static void add_update_contact_command", 1
    )[1].split("static void reset_contact_path_command", 1)[0]
    assert "1U + 32U + 3U +" in update
    assert "D1L_BLE_PROTOCOL_CONTACT_PATH_BYTES + 32U + 4U" in update
    assert "imported.type_id = payload[33U]" in update
    assert "const uint8_t flags = payload[34U]" in update
    assert "const uint8_t path_len = payload[35U]" in update
    assert "const uint8_t *path = &payload[36U]" in update
    assert "d1l_contact_store_import_uri(" in update
    assert "d1l_contact_store_rename(" in update
    assert "d1l_contact_store_set_flags(" in update
    assert "d1l_contact_store_reset_path(" in update
    assert "d1l_contact_store_update_path_from_source(" in update
    assert "case CMD_ADD_UPDATE_CONTACT:" in protocol

    changes = protocol.split(
        "static void maybe_queue_contact_change", 1
    )[1].split("static void build_self_info", 1)[0]
    assert "PUSH_CODE_PATH_UPDATED" in changes
    assert "PUSH_CODE_ADVERT" in changes
    new_contact = changes.split("if (previous_index < 0)", 1)[1].split(
        "d1l_contact_entry_t *previous", 1
    )[0]
    assert "PUSH_CODE_NEW_ADVERT" in new_contact
    assert "build_contact_response" in new_contact


def test_admin_and_phone_commands_preempt_bulk_contact_sync():
    protocol = read("main/comms/ble_companion_protocol.c")
    worker = protocol.split("static void protocol_task", 1)[1].split(
        "esp_err_t d1l_ble_companion_protocol_start", 1
    )[0]

    admin = worker.index("maybe_queue_admin_response();")
    receive = worker.index("d1l_ble_companion_take_rx_frame(")
    contacts = worker.index("prepare_next_contact_response();")
    assert admin < receive < contacts


def test_reconnect_preserves_watermarks_and_rechecks_unsynced_messages():
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
    assert "s_force_message_notification_check = true" in reset
    assert "last_existing_seq(dm.next_seq)" in initialize
    assert "last_existing_seq(messages.next_seq)" in initialize
    assert worker.index("initialize_message_sync_watermarks();") < worker.index(
        "reset_session_state();"
    )


def test_phone_message_sync_covers_all_channels_and_keeps_real_timestamps():
    protocol = read("main/comms/ble_companion_protocol.c")
    sync = protocol.split(
        "static bool build_next_channel_message", 1
    )[1].split("static void build_next_message", 1)[0]
    waiting = protocol.split(
        "static void maybe_queue_message_waiting", 1
    )[1].split("static void protocol_task", 1)[0]

    assert "d1l_message_store_snapshot_retained(" in sync
    assert "d1l_message_store_copy_recent(" not in sync
    assert "d1l_message_entry_display_timestamp(entry)" in sync
    assert '"%s: %s"' in sync
    assert "d1l_message_store_snapshot_retained(" in waiting
    assert "s_force_message_notification_check" in waiting


def test_phone_advert_uses_the_nonblocking_radio_owner_queue():
    protocol = read("main/comms/ble_companion_protocol.c")
    advert = protocol.split(
        "static void queue_self_advert_command", 1
    )[1].split("static void dispatch_command", 1)[0]

    assert "d1l_app_model_queue_advert(" in advert
    assert "payload[1] > 1U" in advert
    assert "queue_self_advert_command(payload, length);" in protocol


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
    assert "s_status.last_response_error_code = 0U" in protocol
    assert "s_status.last_response_error_code = code" in protocol

    flood_scope = protocol.split(
        "static void set_flood_scope_command", 1
    )[1].split("static void dispatch_command", 1)[0]
    assert "length < 2U" in flood_scope
    assert "payload[1] > 1U" in flood_scope
    assert "payload[1] == 0U && length == 2U" in flood_scope
    assert "payload[1] == 1U && length == 2U" in flood_scope
    assert "RESP_CODE_DISABLED" in flood_scope
    assert "case CMD_SET_FLOOD_SCOPE_KEY:" in protocol

    transport = read("main/comms/ble_companion.c")
    console = read("main/comms/usb_console.c")
    assert "protocol.last_unsupported_command" in transport
    assert "protocol_last_unsupported_command" in console
    assert "protocol.last_response_error_code" in transport
    assert "protocol_last_response_error_code" in console


def test_phone_startup_uses_protocol_owned_storage_and_official_contact_count():
    protocol = read("main/comms/ble_companion_protocol.c")

    battery = protocol.split("static void build_battery_storage(void)", 1)[1].split(
        "static uint32_t message_epoch", 1
    )[0]
    assert "d1l_storage_status(&s_storage_status);" in battery
    assert "d1l_app_model_snapshot" not in battery
    assert "D1L_BLE_PROTOCOL_WIRED_MILLIVOLTS" in battery
    assert "write_u16_le(&response[1], 0U)" not in battery

    contacts = protocol.split("static void begin_contact_iteration", 1)[1].split(
        "static void prepare_next_contact_response", 1
    )[0]
    assert "write_u32_le(&response[1], s_contact_count);" in contacts
    assert "filtered_count" not in contacts

    self_info = protocol.split("static void build_self_info(void)", 1)[1].split(
        "static void build_device_info", 1
    )[0]
    assert "D1L_BLE_PROTOCOL_SELF_ADV_TYPE 1U" in protocol
    assert "D1L_BLE_PROTOCOL_MANUAL_ADD_CONTACTS 0U" in protocol
    assert (
        self_info.index("D1L_BLE_PROTOCOL_SELF_ADV_TYPE")
        < self_info.index("settings.identity_public_key")
        < self_info.index("D1L_BLE_PROTOCOL_MANUAL_ADD_CONTACTS")
        < self_info.index("settings.frequency_hz")
    )
    assert "Every verified signed advert is retained as a contact" in self_info


def test_v10_phone_settings_are_truthful_and_bounded():
    protocol = read("main/comms/ble_companion_protocol.c")

    assert "D1L_BLE_PROTOCOL_AUTOADD_CONFIG 0x1EU" in protocol
    assert "D1L_BLE_PROTOCOL_AUTOADD_MAX_HOPS 0U" in protocol
    assert "static void set_other_params_command" in protocol
    assert "static void set_tuning_params_command" in protocol
    assert "static void build_tuning_params" in protocol
    assert "static void set_autoadd_config_command" in protocol
    assert "static void build_autoadd_config" in protocol
    assert "case CMD_SET_DEVICE_PIN:" in protocol
    assert "case CMD_SET_TUNING_PARAMS:" in protocol

    disabled = protocol.split("case CMD_SET_DEVICE_PIN:", 1)[1].split(
        "default:", 1
    )[0]
    assert "RESP_CODE_DISABLED" in disabled


def test_phone_contacts_use_lastmod_and_live_official_pushes():
    protocol = read("main/comms/ble_companion_protocol.c")

    response = protocol.split(
        "static size_t build_contact_response", 1
    )[1].split("static void begin_contact_iteration", 1)[0]
    assert "contact_lastmod(contact)" in response
    assert "copy_contact_name" in response
    assert response.index("contact->signed_advert_timestamp") < response.index(
        "contact_lastmod(contact)"
    )

    iterator = protocol.split(
        "static void prepare_next_contact_response", 1
    )[1].split("static size_t capture_contact_sync_snapshot", 1)[0]
    assert "const uint32_t lastmod = contact_lastmod(contact);" in iterator
    assert "lastmod <= s_contact_since" in iterator
    assert "s_contact_most_recent = lastmod" in iterator

    live_sync = protocol.split(
        "static void maybe_queue_contact_change", 1
    )[1].split("static void build_self_info", 1)[0]
    assert "PUSH_CODE_CONTACT_DELETED" in live_sync
    assert "PUSH_CODE_ADVERT" in live_sync
    assert "PUSH_CODE_PATH_UPDATED" in live_sync
    assert "previous->seq == current->seq" in live_sync
    assert "maybe_queue_contact_change();" in protocol


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
