from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def test_advert_is_a_bounded_mesh_runtime_command():
    source = read("main/mesh/meshcore_service.c")
    task = body(
        source,
        "static void meshcore_service_task(void *arg)",
        "static esp_err_t meshcore_service_start_task(void)",
    )
    handler = body(
        source,
        "static esp_err_t meshcore_service_handle_send_advert",
        "static void meshcore_service_reply",
    )
    adapter = body(
        source,
        "static esp_err_t meshcore_service_request_advert",
        "static esp_err_t meshcore_service_send_channel_owned",
    )

    assert "D1L_MESHCORE_SERVICE_CMD_SEND_ADVERT" in source
    assert "bool flood;" in source
    assert "bool boot_advert;" in source
    assert "case D1L_MESHCORE_SERVICE_CMD_SEND_ADVERT:" in task
    assert "meshcore_service_handle_send_advert(&cmd)" in task
    assert "s_status.rejected_commands++" in task

    for owned_step in (
        "d1l_meshcore_service_ensure_identity()",
        "ensure_radio_started()",
        "d1l_settings_next_mesh_timestamp(&tx_timestamp)",
        "build_advert_packet(settings, cmd->flood",
        "meshcore_service_handle_send_raw(cmd)",
    ):
        assert owned_step in handler
    assert handler.index("ensure_radio_started()") < handler.index(
        "d1l_settings_next_mesh_timestamp(&tx_timestamp)"
    )
    assert "meshcore_service_send_command" not in handler

    finalize = body(
        source,
        "static void meshcore_service_finalize_send_advert",
        "static void meshcore_service_reply",
    )
    assert "d1l_route_store_upsert_observation" in finalize
    assert "append_packet_log_internal(" in finalize
    assert "note, true" in finalize
    assert task.index("meshcore_service_reply(&cmd, ret)") < task.index(
        "meshcore_service_finalize_send_advert(&cmd)"
    )

    assert ".type = D1L_MESHCORE_SERVICE_CMD_SEND_ADVERT" in adapter
    assert ".flood = flood" in adapter
    assert ".boot_advert = boot_advert" in adapter
    assert "meshcore_service_request_advert(flood, false)" in adapter
    assert "meshcore_service_request_advert(flood, true)" in adapter
    assert "meshcore_service_send_command(" in adapter
    for forbidden in (
        "d1l_meshcore_service_ensure_identity",
        "d1l_settings_next_mesh_timestamp",
        "build_advert_packet",
        "meshcore_service_send_raw",
        "s_tx_busy",
        "s_status",
        "d1l_route_store_",
        "append_packet_log",
    ):
        assert forbidden not in adapter


def test_boot_announces_signed_identity_after_rx_is_queued():
    source = read("main/app_main.c")
    rx = source.index("d1l_meshcore_service_start_rx_async()")
    success = source.index("if (mesh_rx_ret != ESP_OK)", rx)
    advert = source.index(
        "d1l_meshcore_service_request_boot_advert(true)", success
    )

    assert rx < success < advert
    assert "MeshCore boot advert failed" in source[advert:]


def test_boot_advert_status_is_machine_observable():
    header = read("main/mesh/meshcore_service.h")
    service = read("main/mesh/meshcore_service.c")
    console = read("main/comms/usb_console.c")

    for field in (
        "boot_advert_tx_queued",
        "boot_advert_tx_done",
        "boot_advert_tx_failed",
        "boot_advert_public_key_prefix",
        "boot_advert_node_name",
        "boot_advert_flood",
    ):
        assert field in header
        assert field in service
    assert '\\"boot_queued\\"' in console
    assert '\\"boot_done\\"' in console
    assert '\\"boot_failed\\"' in console
    assert '\\"boot_public_key_prefix\\"' in console
    assert '\\"boot_node_name\\"' in console


def test_deferred_packet_log_admission_never_waits_on_storage_owner():
    header = read("main/mesh/packet_log.h")
    packet_log = read("main/mesh/packet_log.c")
    store_lock = read("main/mesh/store_lock.h")

    assert "d1l_packet_log_append_raw_deferred" in header
    assert "d1l_store_lock_try_take" in store_lock
    deferred_admission = body(
        packet_log,
        "if (defer_flush) {",
        "} else {",
    )
    assert "d1l_store_lock_try_take(&s_append_clear_lock)" in deferred_admission
    assert "d1l_store_lock_try_take(&s_store_lock)" in deferred_admission
    assert "d1l_store_lock_take" not in deferred_admission
