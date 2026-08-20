from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_ble_gatt_contract_matches_pinned_meshcore_and_stays_bounded():
    header = read("main/comms/ble_companion.h")
    source = read("main/comms/ble_companion.c")
    queue = read("main/comms/ble_companion_queue.h")

    for uuid in (
        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
        "6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
        "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
    ):
        assert uuid in header
    assert "D1L_BLE_COMPANION_QUEUE_DEPTH 4U" in queue
    assert "D1L_COMPANION3_MAX_FRAME_SIZE" in queue
    assert "BLE_GATT_CHR_F_WRITE_NO_RSP" in source
    assert "BLE_GATT_CHR_F_READ" in source
    assert "BLE_GATT_CHR_F_NOTIFY" in source
    assert ".access_cb = tx_access" in source
    assert "empty secure" in source
    assert "BLE_GAP_EVENT_SUBSCRIBE" in source
    assert "BLE_GAP_EVENT_MTU" in source
    assert "ble_gatts_notify_custom" in source
    assert "payload_len > notification_capacity" in source
    assert "ble_att_set_preferred_mtu" in source
    assert "D1L_BLE_COMPANION_PREFERRED_ATT_MTU 517U" in header
    assert "D1L_BLE_COMPANION_TX_MIN_INTERVAL_US 60000LL" in source
    assert "d1l_ble_companion_poll();" in read(
        "main/comms/ble_companion_protocol.c"
    )


def test_ble_transport_fails_closed_on_security_and_preserves_wire_contract():
    source = read("main/comms/ble_companion.c")
    header = read("main/comms/ble_companion.h")

    for security in (
        "desc.sec_state.encrypted",
        "desc.sec_state.authenticated",
        "desc.sec_state.bonded",
        "ble_hs_cfg.sm_bonding = 1",
        "ble_hs_cfg.sm_mitm = 1",
        "ble_hs_cfg.sm_sc = 1",
        "BLE_ATT_ERR_INSUFFICIENT_AUTHEN",
        "BLE_GAP_REPEAT_PAIRING_RETRY",
    ):
        assert security in source
    assert "ble_gap_security_initiate" in source
    assert "ble_store_util_delete_peer" in source
    assert "secure_connections_mitm_bonded" in source
    assert "D1L_COMPANION3_APP_TO_RADIO" in source
    assert "D1L_COMPANION3_RADIO_TO_APP" in source
    assert "d1l_companion3_encode" in source
    assert "raw_gatt_internal_meshcore_3byte" in source
    assert "raw MeshCore BLE characteristic values" in header
    assert "meshcore_service" not in source
    assert "mesh/" not in source


def test_ble_pin_matches_meshcore_companion_and_is_never_logged():
    source = read("main/comms/ble_companion.c")
    app_header = read("main/app/app_model.h")
    settings = read("main/app/settings_model.c")

    assert "D1L_BLE_COMPANION_STATIC_PASSKEY 123456U" in source
    assert "ble_sm_configure_static_passkey" in source
    assert "ble_sm_inject_io" in source
    for line in source.splitlines():
        if "LOG" in line or "printf" in line:
            assert "PASSKEY" not in line.upper()
            assert "pairing_passkey" not in line
    assert "D1L_BLE_COMPANION_TRANSPORT_SUPPORTED true" in app_header
    assert "settings->ble_companion_enabled = false" in settings


def test_ble_build_configuration_is_nimble_only_and_memory_bounded():
    defaults = read("sdkconfig.defaults")
    cmake = read("main/CMakeLists.txt")
    manager = read("main/comms/connectivity_manager.c")

    for setting in (
        "CONFIG_BT_ENABLED=y",
        "CONFIG_BT_NIMBLE_ENABLED=y",
        "CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y",
        "# CONFIG_BT_NIMBLE_ROLE_CENTRAL is not set",
        "# CONFIG_BT_NIMBLE_ROLE_OBSERVER is not set",
        "CONFIG_BT_NIMBLE_GATT_SERVER=y",
        "# CONFIG_BT_NIMBLE_GATT_CLIENT is not set",
        "CONFIG_BT_NIMBLE_SECURITY_ENABLE=y",
        "CONFIG_BT_NIMBLE_SM_SC=y",
        "CONFIG_BT_NIMBLE_SM_SC_ONLY=1",
        "CONFIG_BT_NIMBLE_NVS_PERSIST=y",
        "CONFIG_BT_NIMBLE_MAX_BONDS=2",
        "CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1",
        "CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517",
        "CONFIG_BT_NIMBLE_ATT_MAX_PREP_ENTRIES=4",
        "CONFIG_BT_NIMBLE_GATT_MAX_PROCS=2",
        "CONFIG_BT_NIMBLE_MAX_CCCDS=2",
        "CONFIG_BT_NIMBLE_WHITELIST_SIZE=2",
        "CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096",
        "CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=12",
        "CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=12",
        "CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT=8",
        "CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT=15",
        "CONFIG_BT_NIMBLE_LOW_SPEED_MODE=y",
        "CONFIG_BT_CTRL_BLE_MAX_ACT=2",
        "# CONFIG_BT_CTRL_BLE_SCAN is not set",
    ):
        assert setting in defaults
    assert "# CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC is not set" in defaults
    assert "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y" in defaults
    assert "# CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC is not set" in defaults
    for service in (
        "PROX",
        "ANS",
        "CTS",
        "HTP",
        "IPSS",
        "TPS",
        "IAS",
        "LLS",
        "SPS",
        "HR",
        "BAS",
        "DIS",
    ):
        assert f"# CONFIG_BT_NIMBLE_{service}_SERVICE is not set" in defaults
    assert '"comms/ble_companion.c"' in cmake
    assert '"comms/ble_companion_protocol.c"' in cmake
    assert '"comms/ble_companion_queue.c"' in cmake
    assert "bt" in cmake
    assert "d1l_ble_companion_start()" in manager
    assert "d1l_ble_companion_stop()" in manager
    assert "d1l_ble_companion_prepare_reboot()" in manager


def test_ble_callbacks_keep_frame_scratch_off_the_host_task_stack():
    source = read("main/comms/ble_companion.c")
    rx_access = source.split("static int rx_access", 1)[1].split(
        "static int tx_access", 1
    )[0]
    pump_tx = source.split("static void pump_tx(void)\n{", 1)[1].split(
        "static void update_security", 1
    )[0]

    for buffer in ("s_rx_payload", "s_rx_frame", "s_tx_frame"):
        declaration = source.split(f"{buffer}[", 1)[1].split(";", 1)[0]
        assert "EXT_RAM_BSS_ATTR" in declaration
    assert "uint8_t payload[" not in rx_access
    assert "uint8_t frame[" not in rx_access
    assert "uint8_t frame[" not in pump_tx
