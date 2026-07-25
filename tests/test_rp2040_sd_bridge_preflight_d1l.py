import json
import sys

from scripts import rp2040_sd_bridge_preflight_d1l as preflight


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


class CommandAwareSerial:
    def __init__(self, responses_by_command):
        self.responses_by_command = {
            command: [line.encode("utf-8") for line in responses]
            for command, responses in responses_by_command.items()
        }
        self.current_command = None
        self.writes = []
        self.reset_count = 0

    def write(self, data):
        command = data.decode("utf-8")
        self.writes.append(command)
        self.current_command = command.strip()

    def readline(self):
        responses = self.responses_by_command.get(self.current_command, [])
        return responses.pop(0) if responses else b""

    def reset_input_buffer(self):
        self.reset_count += 1

    def open(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False


def protocol_pending_storage_line() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage status",'
        '"sd":{"state":"protocol_pending","rp2040_bridge_ready":true,'
        '"rp2040_protocol_supported":false,"last_error":"ESP_ERR_TIMEOUT"},'
        '"data_backend":"nvs","export_backend":"serial"}\n'
    )


def ready_storage_line(manager_state: str = "READY_SD") -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage status",'
        f'"manager":{{"running":true,"state":"{manager_state}"}},'
        '"sd":{"state":"ready","filesystem":"fat32","present":true,"mounted":true,'
        '"data_root_ready":true,"rp2040_bridge_ready":true,'
        '"rp2040_protocol_supported":true,"file_ops":true,'
        '"atomic_rename":true,"file_line_max":512,'
        '"file_chunk_max":192,"path_max":96,"status_stale":false,'
        '"presence_stale":false,"refresh_failures":0},'
        '"data_backend":"mixed","export_backend":"sd_diagnostic_exports_ready"}\n'
    )


def mount_required_storage_line() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage status",'
        '"sd":{"state":"mount_required","present":false,"mounted":false,'
        '"rp2040_bridge_ready":true,"rp2040_protocol_supported":true,'
        '"last_error":"ESP_OK"},'
        '"data_backend":"nvs","export_backend":"serial"}\n'
    )


def mount_pending_storage_line() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage status",'
        '"sd":{"state":"mount_pending","present":false,"mounted":false,'
        '"rp2040_bridge_ready":true,"rp2040_protocol_supported":true,'
        '"last_error":"ESP_OK"},'
        '"data_backend":"nvs","export_backend":"serial"}\n'
    )


def no_card_storage_line() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage status",'
        '"sd":{"state":"no_card","present":false,"mounted":false,'
        '"rp2040_bridge_ready":true,"rp2040_protocol_supported":true,'
        '"last_error":"ESP_OK"},'
        '"data_backend":"nvs","export_backend":"serial"}\n'
    )


def no_card_rejected_probe_storage_line() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage status",'
        '"sd":{"state":"no_card","present":false,"mounted":false,'
        '"rp2040_bridge_ready":true,"rp2040_protocol_supported":true,'
        '"probe_error":3,"probe_data":0,"last_error":"ESP_OK"},'
        '"data_backend":"nvs","export_backend":"serial"}\n'
    )


def raw_card_present_mount_failed_line() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage status",'
        '"sd":{"state":"not_fat32_or_unmountable","present":true,"mounted":false,'
        '"data_root_ready":false,"needs_fat32":true,'
        '"rp2040_bridge_ready":true,"rp2040_protocol_supported":true,'
        '"file_ops":false,"atomic_rename":false,'
        '"probe_error":0,"probe_data":192,"mount_error":23,"mount_data":8,'
        '"last_error":"ESP_OK"},'
        '"data_backend":"nvs","export_backend":"serial"}\n'
    )


def storage_mount_pending_line() -> str:
    return (
        '{"schema":1,"ok":false,"cmd":"storage mount","code":"ESP_ERR_TIMEOUT",'
        '"sd":{"state":"protocol_pending","rp2040_bridge_ready":true,'
        '"rp2040_protocol_supported":false,"last_error":"ESP_ERR_TIMEOUT"},'
        '"public_rf_tx":false,"formats_sd":false}\n'
    )


def storage_mount_noop_line() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"storage mount",'
        '"sd":{"state":"protocol_pending","rp2040_bridge_ready":true,'
        '"rp2040_protocol_supported":false,"last_error":"ESP_OK"},'
        '"public_rf_tx":false,"formats_sd":false}\n'
    )


def rp2040_ping_line() -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"rp2040 ping","bridge_ready":true,'
        '"protocol_supported":true,"sd_touched":false,'
        '"file_line_max":512,"file_chunk_max":192,"path_max":96,'
        '"public_rf_tx":false,"formats_sd":false}\n'
    )


def rp2040_ping_failed_line() -> str:
    return (
        '{"schema":1,"ok":false,"cmd":"rp2040 ping","code":"ESP_ERR_TIMEOUT",'
        '"bridge_ready":true,"protocol_supported":false,"sd_touched":false,'
        '"public_rf_tx":false,"formats_sd":false}\n'
    )


def test_preflight_dry_run_is_non_destructive():
    report = preflight.dry_run_report("artifact-dir")

    assert report["ok"] is True
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert report["copies_uf2"] is False
    assert report["commands"] == [
        "rp2040 status",
        "rp2040 ping",
        "storage status",
        "storage mount",
        "storage status",
        "storage diag",
        "health",
    ]
    assert not any(command.startswith("mesh send public") for command in report["commands"])
    assert not any("setup confirm" in command for command in report["commands"])
    assert 'command_timeout = max(timeout, 15.0) if command in {"storage mount", "storage diag"} else timeout' in (
        preflight.Path(preflight.__file__).read_text(encoding="utf-8")
    )


def test_main_stdout_exposes_acceptance_and_settle_exhaustion(
    tmp_path, monkeypatch, capsys
):
    exhausted = {
        "schema": 1,
        "mode": "hardware-preflight",
        "ok": False,
        "serial_commands_ok": True,
        "ready_for_sd_acceptance": False,
        "storage_status_settle_exhausted": True,
    }
    monkeypatch.setattr(
        sys,
        "argv",
        ["rp2040_sd_bridge_preflight_d1l.py", "--port", "COM12"],
    )
    monkeypatch.setattr(preflight, "run_preflight", lambda **_kwargs: exhausted)
    monkeypatch.setattr(
        preflight,
        "write_report",
        lambda report, out_path: tmp_path / "preflight.json",
    )

    code = preflight.main()
    stdout = json.loads(capsys.readouterr().out)

    assert code == 1
    assert stdout["ok"] is False
    assert stdout["ready_for_sd_acceptance"] is False
    assert stdout["storage_status_settle_exhausted"] is True


def test_preflight_classifies_protocol_pending_without_uf2_volume():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {},
        {
            "ok": True,
            "cmd": "storage status",
            "manager": {"running": True, "state": "READY_SD"},
            "sd": {
                "state": "protocol_pending",
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": False,
                "last_error": "ESP_ERR_TIMEOUT",
            },
        },
        [],
        {"ok": True},
    )

    assert report["state"] == "rp2040_protocol_pending"
    assert report["next_action"] == "put_rp2040_in_uf2_bootloader"
    assert report["storage_file_gate_ready"] is False
    assert report["uf2_volume_available"] is False


def test_preflight_classifies_protocol_pending_with_uf2_volume():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {},
        {
            "ok": True,
            "cmd": "storage status",
            "sd": {
                "state": "protocol_pending",
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": False,
            },
        },
        [{"path": "R:\\"}],
        {"ok": True},
    )

    assert report["state"] == "rp2040_protocol_pending"
    assert report["next_action"] == "dry_run_then_copy_rp2040_uf2"
    assert report["uf2_volume_available"] is True


def test_preflight_classifies_ready_storage_gate():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {"ok": True, "cmd": "rp2040 ping", "protocol_supported": True, "sd_touched": False},
        {
            "ok": True,
            "cmd": "storage status",
            "manager": {"running": True, "state": "READY_SD"},
            "sd": {
                "state": "ready",
                "filesystem": "fat32",
                "present": True,
                "mounted": True,
                "data_root_ready": True,
                "rp2040_protocol_supported": True,
                "file_ops": True,
                "atomic_rename": True,
                "file_line_max": 512,
                    "file_chunk_max": 192,
                    "path_max": 96,
                    "status_stale": False,
                    "presence_stale": False,
                    "refresh_failures": 0,
            },
        },
        [],
        {"ok": True},
    )

    assert report["state"] == "sd_bridge_ready"
    assert report["next_action"] == "run_sd_file_and_export_acceptance"
    assert report["storage_file_gate_ready"] is True
    assert report["storage_manager_ready_sd"] is True
    assert report["ready_for_sd_acceptance"] is True


def test_preflight_classifies_ping_ok_but_status_pending_separately():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {"ok": True, "cmd": "rp2040 ping", "protocol_supported": True, "sd_touched": False},
        {
            "ok": True,
            "cmd": "storage status",
            "sd": {
                "state": "protocol_pending",
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": False,
            },
        },
        [],
        {"ok": True},
    )

    assert report["state"] == "sd_status_pending"
    assert report["next_action"] == "inspect_storage_status_and_run_storage_diag"
    assert report["rp2040_ping_supported"] is True
    assert report["rp2040_ping_sd_touched"] is False


def test_preflight_classifies_safe_status_needing_explicit_mount():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {"ok": True, "cmd": "rp2040 ping", "protocol_supported": True, "sd_touched": False},
        {
            "ok": True,
            "cmd": "storage status",
            "sd": {
                "state": "mount_required",
                "present": False,
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": True,
            },
        },
        [],
        {"ok": True},
    )

    assert report["state"] == "sd_mount_required"
    assert report["next_action"] == "run_storage_mount"


def test_preflight_classifies_explicit_mount_timeout():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {"ok": True, "cmd": "rp2040 ping", "protocol_supported": True, "sd_touched": False},
        {
            "ok": True,
            "cmd": "storage status",
            "sd": {
                "state": "mount_required",
                "present": False,
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": True,
            },
        },
        [],
        {"ok": True},
        None,
        {"ok": False, "cmd": "storage mount", "code": "ESP_ERR_TIMEOUT"},
    )

    assert report["state"] == "sd_mount_pending"
    assert report["next_action"] == "inspect_rp2040_sd_mount_timeout_and_reset_bridge"
    assert report["storage_mount_ok"] is False


def test_preflight_classifies_async_mount_pending():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {"ok": True, "cmd": "rp2040 ping", "protocol_supported": True, "sd_touched": False},
        {
            "ok": True,
            "cmd": "storage status",
            "sd": {
                "state": "mount_pending",
                "present": False,
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": True,
            },
        },
        [],
        {"ok": True},
        None,
        {"ok": True, "cmd": "storage mount"},
    )

    assert report["state"] == "sd_mount_pending"
    assert report["next_action"] == "wait_for_storage_mount_or_reset_rp2040_bridge"
    assert report["storage_mount_ok"] is True


def test_preflight_classifies_no_card_with_diag_pending_as_bridge_flash_needed():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {},
        {
            "ok": True,
            "cmd": "storage status",
            "sd": {
                "state": "no_card",
                "present": False,
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": True,
            },
        },
        [],
        {"ok": True},
        {"ok": False, "cmd": "storage diag", "diag_supported": False},
    )

    assert report["state"] == "sd_card_not_present_diag_pending"
    assert report["next_action"] == "put_rp2040_in_uf2_bootloader"
    assert report["rp2040_diag_supported"] is False


def test_preflight_classifies_no_card_with_cmd0_rejection_as_firmware_path():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {"ok": True, "cmd": "rp2040 ping", "protocol_supported": True, "sd_touched": False},
        {
            "ok": True,
            "cmd": "storage status",
            "sd": {
                "state": "no_card",
                "present": False,
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": True,
                "probe_error": 3,
                "probe_data": 0,
            },
        },
        [],
        {"ok": True},
    )

    assert report["state"] == "sd_probe_rejected_card"
    assert report["next_action"] == "inspect_rp2040_sd_cmd0_firmware_path"


def test_preflight_classifies_no_card_with_cmd8_echo_rejection_as_firmware_path():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {"ok": True, "cmd": "rp2040 ping", "protocol_supported": True, "sd_touched": False},
        {
            "ok": True,
            "cmd": "storage status",
            "sd": {
                "state": "no_card",
                "present": False,
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": True,
                "probe_error": 4,
                "probe_data": 0,
            },
        },
        [],
        {"ok": True},
    )

    assert report["state"] == "sd_probe_rejected_card"
    assert report["next_action"] == "inspect_rp2040_sd_cmd0_firmware_path"


def test_preflight_classifies_error_with_cmd8_echo_rejection_as_firmware_path():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {"ok": True, "cmd": "rp2040 ping", "protocol_supported": True, "sd_touched": False},
        {
            "ok": True,
            "cmd": "storage status",
            "sd": {
                "state": "error",
                "present": False,
                "note": "sd_probe_rejected_card",
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": True,
                "probe_error": 4,
                "probe_data": 0,
            },
        },
        [],
        {"ok": True},
    )

    assert report["state"] == "sd_probe_rejected_card"
    assert report["next_action"] == "inspect_rp2040_sd_cmd0_firmware_path"


def test_preflight_prioritizes_present_rejected_probe_over_fat32_guidance():
    report = preflight.classify_preflight(
        {"ok": True, "cmd": "rp2040 status", "uart_ready": True},
        {"ok": True, "cmd": "rp2040 ping", "protocol_supported": True, "sd_touched": False},
        {
            "ok": True,
            "cmd": "storage status",
            "sd": {
                "state": "error",
                "present": True,
                "mounted": False,
                "needs_fat32": True,
                "note": "sd_probe_rejected_card",
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": True,
                "probe_error": 3,
                "probe_data": 0,
            },
        },
        [],
        {"ok": True},
    )

    assert report["state"] == "sd_probe_rejected_card"
    assert report["next_action"] == "inspect_rp2040_sd_cmd0_firmware_path"


def test_run_preflight_queries_only_safe_serial_commands(monkeypatch):
    ser = FakeSerial(
        [
            '{"schema":1,"ok":true,"cmd":"rp2040 status","uart_ready":true}\n',
            rp2040_ping_line(),
            protocol_pending_storage_line(),
            storage_mount_noop_line(),
            protocol_pending_storage_line(),
            '{"schema":1,"ok":false,"cmd":"storage diag","code":"ESP_ERR_TIMEOUT","diag_supported":false}\n',
            '{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n',
        ]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(preflight.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(preflight, "verify_optional_artifact", lambda *_args: {"ok": True})
    monkeypatch.setattr(preflight, "candidate_volumes", lambda: [])

    report = preflight.run_preflight(
        port="COM12",
        baud=115200,
        timeout=1.0,
        artifact_dir="artifact",
        expected_sha256=None,
    )

    assert report["ok"] is True
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert report["copies_uf2"] is False
    assert report["classification"]["state"] == "sd_status_pending"
    assert ser.reset_count == 1
    assert ser.writes == [
        "rp2040 status\n",
        "rp2040 ping\n",
        "storage status\n",
        "storage mount\n",
        "storage status\n",
        "storage diag\n",
        "health\n",
    ]


def test_run_preflight_skips_diag_when_mount_is_still_pending(monkeypatch):
    statuses = [mount_required_storage_line()]
    statuses.extend(mount_pending_storage_line() for _ in range(preflight.MOUNT_POLL_ATTEMPTS + 1))
    ser = CommandAwareSerial(
        {
            "rp2040 status": ['{"schema":1,"ok":true,"cmd":"rp2040 status","uart_ready":true}\n'],
            "rp2040 ping": [rp2040_ping_line()],
            "storage status": statuses,
            "storage mount": [
                '{"schema":1,"ok":true,"cmd":"storage mount","public_rf_tx":false,"formats_sd":false,'
                '"sd":{"state":"mount_pending","rp2040_protocol_supported":true}}\n'
            ],
            "health": ['{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n'],
        }
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(preflight.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(preflight, "verify_optional_artifact", lambda *_args: {"ok": True})
    monkeypatch.setattr(preflight, "candidate_volumes", lambda: [])

    report = preflight.run_preflight(
        port="COM12",
        baud=115200,
        timeout=1.0,
        artifact_dir="artifact",
        expected_sha256=None,
    )

    assert report["ok"] is False
    assert report["serial_commands_ok"] is True
    assert report["ready_for_sd_acceptance"] is False
    assert report["storage_mount_poll_count"] == preflight.MOUNT_POLL_ATTEMPTS
    assert report["storage_status_poll_count"] == preflight.MOUNT_POLL_ATTEMPTS
    assert report["storage_status_settle_exhausted"] is True
    assert report["classification"]["state"] == "sd_mount_pending"
    assert report["storage_diag"]["skipped"] is True
    assert report["storage_diag"]["reason"] == "storage_mount_pending"
    assert "storage diag\n" not in ser.writes


def test_run_preflight_tolerates_rp2040_status_timeout_when_storage_proves_bridge(monkeypatch):
    ser = CommandAwareSerial(
        {
            "rp2040 ping": [rp2040_ping_line()],
            "storage status": [protocol_pending_storage_line(), protocol_pending_storage_line()],
            "storage mount": [storage_mount_noop_line()],
            "storage diag": [
                '{"schema":1,"ok":false,"cmd":"storage diag","code":"ESP_ERR_TIMEOUT","diag_supported":false}\n'
            ],
            "health": ['{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n'],
        }
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(preflight.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(preflight, "verify_optional_artifact", lambda *_args: {"ok": True})
    monkeypatch.setattr(preflight, "candidate_volumes", lambda: [])

    report = preflight.run_preflight(
        port="COM12",
        baud=115200,
        timeout=0.01,
        artifact_dir="artifact",
        expected_sha256=None,
    )

    assert report["ok"] is True
    assert report["rp2040_status_ok"] is False
    assert report["rp2040_ping_ok"] is True
    assert report["rp2040_status_optional_ok"] is True
    assert report["classification"]["rp2040_uart_ready"] is True
    assert report["classification"]["state"] == "sd_status_pending"


def test_run_preflight_skips_storage_mount_and_diag_after_ping_failure(monkeypatch):
    ser = CommandAwareSerial(
        {
            "rp2040 status": ['{"schema":1,"ok":true,"cmd":"rp2040 status","uart_ready":true}\n'],
            "rp2040 ping": [rp2040_ping_failed_line()],
            "health": ['{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n'],
        }
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(preflight.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(preflight, "verify_optional_artifact", lambda *_args: {"ok": True})
    monkeypatch.setattr(preflight, "candidate_volumes", lambda: [])

    report = preflight.run_preflight(
        port="COM12",
        baud=115200,
        timeout=1.0,
        artifact_dir="artifact",
        expected_sha256=None,
    )

    assert report["ok"] is False
    assert report["commands"] == ["rp2040 status", "rp2040 ping", "health"]
    assert report["classification"]["state"] == "rp2040_protocol_pending"
    assert report["storage_mount"]["skipped"] is True
    assert report["storage_diag"]["skipped"] is True
    assert ser.writes == ["rp2040 status\n", "rp2040 ping\n", "health\n"]


def test_run_preflight_reports_ready_for_sd_acceptance(monkeypatch):
    ser = FakeSerial(
        [
            '{"schema":1,"ok":true,"cmd":"rp2040 status","uart_ready":true}\n',
            rp2040_ping_line(),
            ready_storage_line(),
            '{"schema":1,"ok":true,"cmd":"storage mount","public_rf_tx":false,"formats_sd":false,'
            '"sd":{"state":"ready","filesystem":"fat32","present":true,"mounted":true,"data_root_ready":true,'
            '"rp2040_protocol_supported":true,"file_ops":true,"atomic_rename":true,'
            '"file_line_max":512,"file_chunk_max":192,"path_max":96}}\n',
            ready_storage_line(),
            '{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n',
        ]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(preflight.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(preflight, "verify_optional_artifact", lambda *_args: {"ok": True})
    monkeypatch.setattr(preflight, "candidate_volumes", lambda: [])

    report = preflight.run_preflight(
        port="COM12",
        baud=115200,
        timeout=1.0,
        artifact_dir="artifact",
        expected_sha256=None,
    )

    assert report["ok"] is True
    assert report["storage_diag_ok"] is True
    assert report["storage_diag"]["skipped"] is True
    assert report["storage_diag"]["reason"] == "storage_file_gate_ready"
    assert report["ready_for_sd_acceptance"] is True
    assert report["classification"]["state"] == "sd_bridge_ready"
    assert ser.writes == [
        "rp2040 status\n",
        "rp2040 ping\n",
        "storage status\n",
        "storage mount\n",
        "storage status\n",
        "health\n",
    ]


def test_run_preflight_polls_manager_ping_until_ready_sd(monkeypatch):
    ser = CommandAwareSerial(
        {
            "rp2040 status": [
                '{"schema":1,"ok":true,"cmd":"rp2040 status","uart_ready":true}\n'
            ],
            "rp2040 ping": [rp2040_ping_line()],
            "storage status": [
                ready_storage_line("PING"),
                ready_storage_line("PING"),
                ready_storage_line("READY_SD"),
            ],
            "storage mount": [
                '{"schema":1,"ok":true,"cmd":"storage mount",'
                '"public_rf_tx":false,"formats_sd":false}\n'
            ],
            "health": [
                '{"schema":1,"ok":true,"cmd":"health",'
                '"board_ready":true,"ui_ready":true}\n'
            ],
        }
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(preflight.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(preflight, "verify_optional_artifact", lambda *_args: {"ok": True})
    monkeypatch.setattr(preflight, "candidate_volumes", lambda: [])

    report = preflight.run_preflight(
        port="COM12",
        baud=115200,
        timeout=1.0,
        artifact_dir="artifact",
        expected_sha256=None,
    )

    assert report["ok"] is True
    assert report["ready_for_sd_acceptance"] is True
    assert report["classification"]["storage_file_gate_ready"] is True
    assert report["classification"]["storage_manager_ready_sd"] is True
    assert report["storage_manager_initial_state"] == "PING"
    assert report["storage_manager_final_state"] == "READY_SD"
    assert report["storage_manager_transition_poll_count"] == 1
    assert report["storage_mount_poll_count"] == 0
    assert report["storage_status_poll_count"] == 1
    assert report["storage_status_settle_exhausted"] is False
    assert ser.writes.count("storage status\n") == 3


def test_run_preflight_reports_exhausted_transitional_manager_truthfully(monkeypatch):
    ser = CommandAwareSerial(
        {
            "rp2040 status": [
                '{"schema":1,"ok":true,"cmd":"rp2040 status","uart_ready":true}\n'
            ],
            "rp2040 ping": [rp2040_ping_line()],
            "storage status": [
                ready_storage_line("PING")
                for _ in range(preflight.MOUNT_POLL_ATTEMPTS + 2)
            ],
            "storage mount": [
                '{"schema":1,"ok":true,"cmd":"storage mount",'
                '"public_rf_tx":false,"formats_sd":false}\n'
            ],
            "health": [
                '{"schema":1,"ok":true,"cmd":"health",'
                '"board_ready":true,"ui_ready":true}\n'
            ],
        }
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(preflight.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(preflight, "verify_optional_artifact", lambda *_args: {"ok": True})
    monkeypatch.setattr(preflight, "candidate_volumes", lambda: [])

    report = preflight.run_preflight(
        port="COM12",
        baud=115200,
        timeout=1.0,
        artifact_dir="artifact",
        expected_sha256=None,
    )

    assert report["ok"] is False
    assert report["serial_commands_ok"] is True
    assert report["ready_for_sd_acceptance"] is False
    assert report["classification"]["storage_file_gate_ready"] is True
    assert report["classification"]["storage_manager_ready_sd"] is False
    assert report["classification"]["state"] == "storage_manager_transition_pending"
    assert report["classification"]["next_action"] == "wait_for_storage_manager_ready_sd"
    assert report["storage_manager_final_state"] == "PING"
    assert report["storage_manager_transition_poll_count"] == preflight.MOUNT_POLL_ATTEMPTS
    assert report["storage_mount_poll_count"] == 0
    assert report["storage_status_poll_count"] == preflight.MOUNT_POLL_ATTEMPTS
    assert report["storage_manager_transition_exhausted"] is True
    assert report["storage_status_settle_exhausted"] is True
    assert report["storage_diag"]["reason"] == "storage_file_gate_ready"


def test_run_preflight_classifies_raw_present_mount_failed_card(monkeypatch):
    ser = FakeSerial(
        [
            '{"schema":1,"ok":true,"cmd":"rp2040 status","uart_ready":true}\n',
            rp2040_ping_line(),
            raw_card_present_mount_failed_line(),
            storage_mount_noop_line(),
            raw_card_present_mount_failed_line(),
            '{"schema":1,"ok":true,"cmd":"storage diag","diag_supported":true,'
            '"mount_selected":false,"note":"raw_card_present_mount_failed",'
            '"public_rf_tx":false,"formats_sd":false}\n',
            '{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n',
        ]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(preflight.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(preflight, "verify_optional_artifact", lambda *_args: {"ok": True})
    monkeypatch.setattr(preflight, "candidate_volumes", lambda: [])

    report = preflight.run_preflight(
        port="COM12",
        baud=115200,
        timeout=1.0,
        artifact_dir="artifact",
        expected_sha256=None,
    )

    assert report["ok"] is True
    assert report["ready_for_sd_acceptance"] is False
    assert report["classification"]["state"] == "raw_card_present_mount_failed"
    assert report["classification"]["next_action"] == "inspect_rp2040_sd_mount_error_firmware_path"
    assert report["classification"]["sd_mount_error"] == 23
    assert report["classification"]["sd_mount_data"] == 8


def test_posix_preflight_waits_for_console_before_bridge_commands(monkeypatch):
    ser = FakeSerial(
        [
            '{"schema":1,"ok":true,"cmd":"rp2040 status","uart_ready":true}\n',
            rp2040_ping_line(),
            ready_storage_line(),
            storage_mount_noop_line(),
            ready_storage_line(),
            '{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n',
        ]
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    startup_health = {
        "schema": 1,
        "ok": True,
        "cmd": "health",
        "boot_nonce": 123,
    }
    waits = []
    monkeypatch.setitem(sys.modules, "serial", FakeSerialModule())
    monkeypatch.setattr(preflight.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(
        preflight,
        "wait_for_console_ready",
        lambda handle, timeout, attempts: waits.append(
            (handle, timeout, attempts)
        ) or startup_health,
    )
    monkeypatch.setattr(preflight, "verify_optional_artifact", lambda *_args: {"ok": True})
    monkeypatch.setattr(preflight, "candidate_volumes", lambda: [])

    report = preflight.run_preflight(
        port=preflight.POSIX_D1L_TARGET,
        baud=115200,
        timeout=1.0,
        artifact_dir="artifact",
        expected_sha256=None,
    )

    assert report["ok"] is True
    assert report["serial_startup_health"] == startup_health
    assert waits == [
        (
            ser,
            preflight.POSIX_SERIAL_READY_TIMEOUT_SECONDS,
            3,
        )
    ]
    assert ser.writes[0] == "rp2040 status\n"


def test_run_preflight_polls_safe_status_while_mount_pending(monkeypatch):
    ser = CommandAwareSerial(
        {
            "rp2040 status": ['{"schema":1,"ok":true,"cmd":"rp2040 status","uart_ready":true}\n'],
            "rp2040 ping": [rp2040_ping_line()],
            "storage status": [
                mount_required_storage_line(),
                mount_pending_storage_line(),
                mount_pending_storage_line(),
                no_card_storage_line(),
            ],
            "storage mount": [
                '{"schema":1,"ok":true,"cmd":"storage mount","public_rf_tx":false,"formats_sd":false,'
                '"sd":{"state":"mount_pending","rp2040_protocol_supported":true}}\n'
            ],
            "storage diag": [
                '{"schema":1,"ok":true,"cmd":"storage diag","diag_supported":true,'
                '"mount_selected":false,"public_rf_tx":false,"formats_sd":false}\n'
            ],
            "health": ['{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true}\n'],
        }
    )

    class FakeSerialModule:
        Serial = lambda self, **_kwargs: ser

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(preflight.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(preflight, "verify_optional_artifact", lambda *_args: {"ok": True})
    monkeypatch.setattr(preflight, "candidate_volumes", lambda: [])

    report = preflight.run_preflight(
        port="COM12",
        baud=115200,
        timeout=1.0,
        artifact_dir="artifact",
        expected_sha256=None,
    )

    assert report["ok"] is True
    assert report["storage_mount_poll_count"] == 2
    assert report["storage_status"]["sd"]["state"] == "no_card"
    assert report["classification"]["state"] == "sd_card_not_present"
    assert ser.writes == [
        "rp2040 status\n",
        "rp2040 ping\n",
        "storage status\n",
        "storage mount\n",
        "storage status\n",
        "storage status\n",
        "storage status\n",
        "storage diag\n",
        "health\n",
    ]
