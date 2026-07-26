import json

from scripts import sd_file_canary_d1l


def ready_storage_status(manager_state: str | None = None) -> str:
    manager = ""
    if manager_state is not None:
        manager = f'"manager":{{"running":true,"state":"{manager_state}"}},'
    return (
        '{"schema":1,"ok":true,"cmd":"storage status",'
        f"{manager}"
        '"sd":{"state":"ready","filesystem":"fat32","present":true,"mounted":true,'
        '"data_root_ready":true,"rp2040_protocol_supported":true,'
        '"status_stale":false,"presence_stale":false,"refresh_failures":0,'
        '"file_ops":true,"atomic_rename":true,'
        '"file_line_max":512,"file_chunk_max":192,"path_max":96},'
        '"data_enabled":true,"data_backend":"mixed",'
        '"message_store_backend":"sd","dm_store_backend":"sd",'
        '"route_store_backend":"sd","packet_log_backend":"sd",'
        '"retained_nvs":{"partition":"d1l_retained","marker_ready":true,'
        '"markers_complete":true,'
        '"anchor_ready":true,'
        '"sentinel_ready":true,'
        '"external_init_required":false,'
        '"initialized_this_boot":false,"ready":true,"init_error":"ESP_OK",'
        '"migrated_keys":4,"migration_error":"ESP_OK"},'
        '"retained_sd":{"degraded":false,"backup_degraded":false,"stores":{'
        '"messages":{"nvs_mirror_last_error":"ESP_OK"},'
        '"dm":{"nvs_mirror_last_error":"ESP_OK"},'
        '"routes":{"nvs_mirror_last_error":"ESP_OK"},'
        '"packets":{"nvs_mirror_last_error":"ESP_OK"}}},'
        '"stores":{"messages":"sd","dm":"sd","routes":"sd","packets":"sd"}}\n'
    )


def test_sd_file_gate_rejects_stale_or_failed_status_refresh():
    fresh = json.loads(ready_storage_status())
    assert sd_file_canary_d1l.storage_file_gate_ready(fresh)

    stale = json.loads(ready_storage_status())
    stale["sd"]["status_stale"] = True
    assert not sd_file_canary_d1l.storage_file_gate_ready(stale)

    presence_stale = json.loads(ready_storage_status())
    presence_stale["sd"]["presence_stale"] = True
    assert not sd_file_canary_d1l.storage_file_gate_ready(presence_stale)

    failed = json.loads(ready_storage_status())
    failed["sd"]["refresh_failures"] = 1
    assert not sd_file_canary_d1l.storage_file_gate_ready(failed)

    wrong_filesystem = json.loads(ready_storage_status())
    wrong_filesystem["sd"]["filesystem"] = "exfat"
    assert not sd_file_canary_d1l.storage_file_gate_ready(wrong_filesystem)


def test_retained_history_backends_require_dedicated_anchor():
    ready = json.loads(ready_storage_status())
    assert sd_file_canary_d1l.retained_history_backends_ready(ready)

    incomplete = json.loads(ready_storage_status())
    incomplete["retained_nvs"]["markers_complete"] = False
    assert not sd_file_canary_d1l.retained_history_backends_ready(incomplete)

    ready["retained_nvs"]["anchor_ready"] = False
    assert not sd_file_canary_d1l.retained_history_backends_ready(ready)

    del ready["retained_nvs"]["anchor_ready"]
    assert not sd_file_canary_d1l.retained_history_backends_ready(ready)

    external = json.loads(ready_storage_status())
    external["retained_nvs"]["external_init_required"] = True
    assert not sd_file_canary_d1l.retained_history_backends_ready(external)


def test_retained_history_backends_accept_sd_first_and_legacy_mixed_labels():
    current = json.loads(ready_storage_status())
    current["data_backend"] = "sd"
    assert sd_file_canary_d1l.retained_history_backends_ready(current)

    legacy = json.loads(ready_storage_status())
    assert legacy["data_backend"] == "mixed"
    assert sd_file_canary_d1l.retained_history_backends_ready(legacy)

    unavailable = json.loads(ready_storage_status())
    unavailable["data_backend"] = "volatile"
    assert not sd_file_canary_d1l.retained_history_backends_ready(unavailable)


def canary_success() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage filecanary",'
        '"rename_replace":true,"read_final":true,'
        '"delete_final":true,"stat_deleted":true}\n'
    )


def canary_timeout() -> str:
    return (
        '{"schema":1,"ok":false,"cmd":"storage filecanary",'
        '"code":"ESP_ERR_TIMEOUT","step":"write_tmp","file_note":"timeout"}\n'
    )


class FakeSerial:
    def __init__(self, responses):
        self.responses = [line.encode("utf-8") for line in responses]
        self.writes = []
        self.reset_count = 0

    def write(self, data):
        self.writes.append(data.decode("utf-8"))

    def readline(self):
        return self.responses.pop(0) if self.responses else b""

    def reset_input_buffer(self):
        self.reset_count += 1

    def open(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False


def host_timeout(command: str) -> str:
    return json.dumps(
        {"schema": 1, "ok": False, "cmd": command, "code": "TIMEOUT"}
    ) + "\n"


def test_sd_file_canary_dry_run_is_serial_only():
    report = sd_file_canary_d1l.dry_run_report()

    assert report["ok"] is True
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert report["commands"] == [
        "storage status",
        "storage mount",
        "storage status",
        "storage filecanary",
        "storage status",
        "packets",
        "health",
    ]


def test_preflight_host_timeout_stops_filecanary_sequence(monkeypatch):
    ser = FakeSerial([host_timeout("storage status")])

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    report = sd_file_canary_d1l.run_canary(
        "COM12", 115200, 1.0, allow_unavailable=False
    )

    assert ser.writes == ["storage status\n"]
    assert report["sequence_completed"] is False
    assert report["timed_out_command"] == "storage status"
    assert report["canary"]["code"] == "SKIPPED_AFTER_TIMEOUT"
    assert report["ok"] is False


def test_preflight_boot_marker_stops_filecanary_sequence(monkeypatch):
    ser = FakeSerial(
        [
            '{"schema":1,"ok":true,"cmd":"help"}\n',
            ready_storage_status("READY_SD"),
        ]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    report = sd_file_canary_d1l.run_canary(
        "COM12", 115200, 1.0, allow_unavailable=False
    )

    assert ser.writes == ["storage status\n"]
    assert report["sequence_completed"] is False
    assert report["unexpected_console_restart"] is True
    assert report["canary"]["code"] == "SKIPPED_AFTER_TIMEOUT"
    assert report["ok"] is False


def test_filecanary_host_timeout_stops_status_packet_health_followups(monkeypatch):
    ser = FakeSerial(
        [ready_storage_status("READY_SD"), host_timeout("storage filecanary")]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    report = sd_file_canary_d1l.run_canary(
        "COM12", 115200, 1.0, allow_unavailable=False
    )

    assert ser.writes == ["storage status\n", "storage filecanary\n"]
    assert report["sequence_completed"] is False
    assert report["timed_out_command"] == "storage filecanary"
    assert report["storage_after"]["code"] == "SKIPPED_AFTER_TIMEOUT"
    assert report["packets"]["code"] == "SKIPPED_AFTER_TIMEOUT"
    assert report["health"]["code"] == "SKIPPED_AFTER_TIMEOUT"
    assert report["ok"] is False
    assert not any(command.startswith("mesh send public") for command in report["commands"])
    assert not any("confirm" in command for command in report["commands"])


def test_filecanary_uses_long_command_timeout(monkeypatch):
    ser = FakeSerial(
        [
            ready_storage_status("READY_SD"),
            canary_success(),
            ready_storage_status("READY_SD"),
            '{"schema":1,"ok":true,"cmd":"packets","count":0,"persisted":true}\n',
            '{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n',
        ]
    )
    calls = []
    original_send = sd_file_canary_d1l.send_console_command

    def recording_send(serial_port, command, timeout):
        calls.append((command, timeout))
        return original_send(serial_port, command, timeout)

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(sd_file_canary_d1l, "send_console_command", recording_send)
    report = sd_file_canary_d1l.run_canary(
        "COM12", 115200, 1.0, allow_unavailable=False
    )

    assert report["ok"] is True
    assert ("storage filecanary", sd_file_canary_d1l.FILE_CANARY_MIN_TIMEOUT_SECONDS) in calls


def test_sd_file_canary_allows_pre_flash_unavailable_state(monkeypatch):
    ser = FakeSerial(
        [
            '{"schema":1,"ok":true,"cmd":"storage status","sd":{"state":"protocol_pending"},"data_backend":"nvs","packet_log_backend":"nvs"}\n',
            '{"schema":1,"ok":false,"cmd":"storage filecanary","code":"ESP_ERR_NOT_SUPPORTED","step":"preflight"}\n',
            '{"schema":1,"ok":true,"cmd":"storage status","sd":{"state":"protocol_pending"},"data_backend":"nvs","packet_log_backend":"nvs"}\n',
            '{"schema":1,"ok":true,"cmd":"packets","count":0,"persisted":true}\n',
            '{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n',
        ]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    report = sd_file_canary_d1l.run_canary("COM12", 115200, 1.0, allow_unavailable=True)

    assert report["ok"] is True
    assert report["canary_passed"] is False
    assert report["canary_unavailable_ok"] is True
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert ser.reset_count == 1
    assert ser.writes == [
        "storage status\n",
        "storage filecanary\n",
        "storage status\n",
        "packets\n",
        "health\n",
    ]


def test_sd_file_canary_success_requires_canary_ok(monkeypatch):
    ser = FakeSerial(
        [
            ready_storage_status(),
            canary_success(),
            ready_storage_status(),
            '{"schema":1,"ok":true,"cmd":"packets","count":1,"persisted":true}\n',
            '{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n',
        ]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    report = sd_file_canary_d1l.run_canary("COM12", 115200, 1.0, allow_unavailable=False)

    assert report["ok"] is True
    assert report["canary_passed"] is True
    assert report["canary_unavailable_ok"] is False
    assert report["canary_retried"] is False
    assert report["storage_file_gate_ready_before"] is True
    assert report["storage_file_gate_ready_after"] is True
    assert report["retained_history_sd_ready_before"] is True
    assert report["retained_history_sd_ready_after"] is True


def test_sd_file_canary_retries_single_transient_timeout_when_storage_stays_ready(monkeypatch):
    ser = FakeSerial(
        [
            ready_storage_status(),
            canary_timeout(),
            ready_storage_status(),
            canary_success(),
            ready_storage_status(),
            '{"schema":1,"ok":true,"cmd":"packets","count":1,"persisted":true}\n',
            '{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n',
        ]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(sd_file_canary_d1l.time, "sleep", lambda _seconds: None)
    report = sd_file_canary_d1l.run_canary("COM12", 115200, 1.0, allow_unavailable=False)

    assert report["ok"] is True
    assert report["canary_passed"] is True
    assert report["canary_retried"] is True
    assert report["canary_retry_ready"] is True
    assert report["initial_canary"]["step"] == "write_tmp"
    assert report["canary"]["ok"] is True
    assert ser.writes == [
        "storage status\n",
        "storage filecanary\n",
        "storage status\n",
        "storage filecanary\n",
        "storage status\n",
        "packets\n",
        "health\n",
    ]


def test_sd_file_canary_waits_for_storage_manager_ready_sd(monkeypatch):
    ser = FakeSerial(
        [
            ready_storage_status("BRIDGE_WAIT"),
            ready_storage_status("STATUS"),
            ready_storage_status("READY_SD"),
            canary_success(),
            ready_storage_status("READY_SD"),
            '{"schema":1,"ok":true,"cmd":"packets","count":1,"persisted":true}\n',
            '{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n',
        ]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(sd_file_canary_d1l.time, "sleep", lambda _seconds: None)
    report = sd_file_canary_d1l.run_canary(
        "COM12",
        115200,
        1.0,
        allow_unavailable=False,
        mount_wait_sec=5.0,
    )

    assert report["ok"] is True
    assert report["storage_ready_waited"] is True
    assert report["storage_ready_poll_count"] == 3
    assert report["storage_file_gate_ready_before"] is True
    assert report["storage_manager_ready_before"] is True
    assert ser.writes == [
        "storage status\n",
        "storage status\n",
        "storage status\n",
        "storage filecanary\n",
        "storage status\n",
        "packets\n",
        "health\n",
    ]


def test_sd_file_canary_waits_for_mount_pending_to_become_ready(monkeypatch):
    ser = FakeSerial(
        [
            '{"schema":1,"ok":true,"cmd":"storage status","sd":{"state":"mount_pending"},"data_backend":"nvs","packet_log_backend":"nvs"}\n',
            '{"schema":1,"ok":true,"cmd":"storage mount","sd":{"state":"mount_pending"}}\n',
            '{"schema":1,"ok":true,"cmd":"storage status","sd":{"state":"mount_pending"},"data_backend":"nvs","packet_log_backend":"nvs"}\n',
            ready_storage_status(),
            canary_success(),
            ready_storage_status(),
            '{"schema":1,"ok":true,"cmd":"packets","count":1,"persisted":true}\n',
            '{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n',
        ]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(sd_file_canary_d1l.time, "sleep", lambda _seconds: None)
    report = sd_file_canary_d1l.run_canary(
        "COM12",
        115200,
        1.0,
        allow_unavailable=False,
        mount_wait_sec=5.0,
    )

    assert report["ok"] is True
    assert report["storage_ready_waited"] is True
    assert report["storage_ready_poll_count"] == 3
    assert report["storage_file_gate_ready_before"] is True
    assert ser.writes == [
        "storage status\n",
        "storage mount\n",
        "storage status\n",
        "storage status\n",
        "storage filecanary\n",
        "storage status\n",
        "packets\n",
        "health\n",
    ]


def test_sd_file_canary_success_is_independent_of_retained_history_backends(monkeypatch):
    partial_status = ready_storage_status().replace('"dm_store_backend":"sd"', '"dm_store_backend":"nvs"')
    ser = FakeSerial(
        [
            partial_status,
            canary_success(),
            partial_status,
            '{"schema":1,"ok":true,"cmd":"packets","count":1,"persisted":true}\n',
            '{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n',
        ]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    report = sd_file_canary_d1l.run_canary("COM12", 115200, 1.0, allow_unavailable=False)

    assert report["ok"] is True
    assert report["canary_passed"] is True
    assert report["retained_history_sd_ready_before"] is False
    assert report["retained_history_sd_ready_after"] is False
