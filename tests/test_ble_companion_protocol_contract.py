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
        "CMD_SEND_TXT_MSG",
        "CMD_SEND_CHANNEL_TXT_MSG",
        "CMD_SET_CHANNEL",
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
        "PUSH_CODE_MSG_WAITING",
    ):
        assert response in protocol
    assert "d1l_app_model_send_dm_text" in protocol
    assert "d1l_app_model_send_channel_text" in protocol
    assert "d1l_time_service_set_companion_time" in protocol
    assert "d1l_app_model_set_companion_map_location" in protocol
    assert "bytes_all_zero(&payload[2], D1L_CHANNEL_NAME_LEN - 1U)" in protocol
    assert "d1l_app_model_remove_channel(" in protocol


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
    assert "Enter PIN %06lu" in ui
    assert "d1l_app_model_ble_begin_pairing" in phase1
    assert "d1l_app_model_ble_forget_peer" in phase1
    for line in transport.splitlines():
        if "LOG" in line or "printf" in line:
            assert "passkey" not in line.lower()
