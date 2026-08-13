import json

import pytest

from scripts import smoke_d1l
from scripts.smoke_d1l import SMOKE_COMMANDS, dry_run_report, expected_command_name, parse_jsonl, send_console_command


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


def health_response(boot_nonce: int | None, reset_reason: str = "SW") -> str:
    nonce = f',"boot_nonce":{boot_nonce}' if boot_nonce is not None else ""
    return f'{{"schema":1,"ok":true,"cmd":"health","board_ready":true,"ui_ready":true,"reset_reason":"{reset_reason}"{nonce}}}\n'


def settings_response(name: str, path_hash: int) -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"settings get",'
        f'"node_name":"{name}","path_hash_bytes":{path_hash},'
        '"wifi_enabled":true,"ble_companion_enabled":false}\n'
    )


def wifi_status_response(ssid: str = "Toddmas2.4") -> str:
    return (
        '{"schema":1,"ok":true,"cmd":"wifi status","setting_enabled":true,'
        f'"profile_saved":true,"password_saved":true,"ssid":"{ssid}"}}\n'
    )


def persistence_responses(
    first_post_nonce: int | None = 22,
    cleanup_post_nonce: int | None = 33,
    cleanup_ssid: str = "Toddmas2.4",
    first_post_reset_reason: str = "SW",
    cleanup_post_reset_reason: str = "SW",
) -> list[str]:
    return [
        settings_response("Krab Desk", 2),
        wifi_status_response(),
        '{"schema":1,"ok":true,"cmd":"settings set name","node_name":"D1L Smoke Persist"}\n',
        '{"schema":1,"ok":true,"cmd":"settings set pathhash","path_hash_bytes":3}\n',
        settings_response("D1L Smoke Persist", 3),
        health_response(11),
        '{"schema":1,"ok":true,"cmd":"reboot","rebooting":true,"reset_scope":"system","storage_manager_quiesced":true,"retained_worker_quiesced":true,"rp2040_bridge_quiesced":true,"connectivity_prepare":"ESP_OK","retained_flush":"ESP_OK","route_flush":"ESP_OK"}\n',
        health_response(first_post_nonce, first_post_reset_reason),
        settings_response("D1L Smoke Persist", 3),
        wifi_status_response(),
        '{"schema":1,"ok":true,"cmd":"settings set name","node_name":"Krab Desk"}\n',
        '{"schema":1,"ok":true,"cmd":"settings set pathhash","path_hash_bytes":2}\n',
        settings_response("Krab Desk", 2),
        health_response(first_post_nonce),
        '{"schema":1,"ok":true,"cmd":"reboot","rebooting":true,"reset_scope":"system","storage_manager_quiesced":true,"retained_worker_quiesced":true,"rp2040_bridge_quiesced":true,"connectivity_prepare":"ESP_OK","retained_flush":"ESP_OK","route_flush":"ESP_OK"}\n',
        health_response(cleanup_post_nonce, cleanup_post_reset_reason),
        settings_response("Krab Desk", 2),
        wifi_status_response(cleanup_ssid),
    ]


def test_parse_jsonl_ignores_logs():
    lines = [
        "I (123) boot: log line\n",
        '{"schema":1,"ok":true,"cmd":"version"}\n',
        "d1l> ",
        '{"schema":1,"ok":false,"cmd":"radiohw","code":"SPI_NOT_READY"}\n',
    ]
    parsed = parse_jsonl(lines)
    assert [item["cmd"] for item in parsed] == ["version", "radiohw"]


def test_command_result_preserves_ignored_json_before_expected_reply():
    ser = FakeSerial(
        [
            '{"schema":1,"ok":true,"cmd":"help"}\n',
            '{"schema":1,"ok":true,"cmd":"storage status","state":"ready"}\n',
        ]
    )

    result = smoke_d1l.read_command_result(ser, "storage status", 1.0)

    assert result["ok"] is True
    assert result["state"] == "ready"
    assert result["ignored_json_count"] == 1
    assert result["ignored_json"] == [{"cmd": "help", "ok": True}]
    assert result["ignored_boot_help_seen"] is True


def test_command_result_keeps_boot_marker_after_ignored_tail_is_truncated():
    responses = ['{"schema":1,"ok":true,"cmd":"help"}\n']
    responses.extend(
        f'{{"schema":1,"ok":true,"cmd":"noise-{index}"}}\n'
        for index in range(6)
    )
    responses.append(
        '{"schema":1,"ok":true,"cmd":"storage status","state":"ready"}\n'
    )
    ser = FakeSerial(responses)

    result = smoke_d1l.read_command_result(ser, "storage status", 1.0)

    assert result["ok"] is True
    assert result["ignored_json_count"] == 7
    assert all(row["cmd"] != "help" for row in result["ignored_json"])
    assert result["ignored_boot_help_seen"] is True


def test_command_result_strips_firmware_spoofed_host_console_evidence():
    ser = FakeSerial(
        [
            '{"schema":1,"ok":true,"cmd":"storage status",'
            '"ignored_json_count":1,"ignored_json":[{"cmd":"help","ok":true}],'
            '"ignored_boot_help_seen":true,"expected_boot_help_after_reboot":true}\n'
        ]
    )

    result = smoke_d1l.read_command_result(ser, "storage status", 1.0)

    assert result["ok"] is True
    assert not smoke_d1l.HOST_CONSOLE_EVIDENCE_FIELDS.intersection(result)


def test_dry_run_lists_phase1_commands():
    report = dry_run_report()
    assert report["ok"] is True
    assert "radiohw" in report["commands"]
    assert "touch test" in report["commands"]
    assert report["commands"] == SMOKE_COMMANDS
    assert "storage status" in report["commands"]
    assert "storage setup" in report["commands"]
    assert not any(command.startswith("mesh send public") for command in report["commands"])


def test_production_smoke_uses_raw_touch_and_skips_qualification_hooks():
    commands = smoke_d1l.commands_for_release_profile("full_feature")

    assert "touch raw" in commands
    assert "display test" not in commands
    assert "touch test" not in commands
    assert smoke_d1l.commands_for_release_profile("development") == SMOKE_COMMANDS


def test_sd_packet_search_gets_bounded_startup_timeout():
    assert smoke_d1l.smoke_command_timeout("packets search test", 8.0) == 20.0
    assert smoke_d1l.smoke_command_timeout("health", 8.0) == 8.0


def test_firmware_identity_requires_exact_device_build_commit():
    commit = "0123456789abcdef0123456789abcdef01234567"
    version = {
        "schema": 1,
        "ok": True,
        "cmd": "version",
        "build_commit": commit.upper(),
    }

    assert smoke_d1l.firmware_identity_matches(version, commit) is True
    assert smoke_d1l.firmware_identity_matches(version, "f" * 40) is False
    assert smoke_d1l.firmware_identity_matches(
        {**version, "build_commit": commit[:7]}, commit
    ) is False
    assert smoke_d1l.firmware_identity_matches(
        {**version, "build_commit": "unknown"}, commit
    ) is False


def test_dry_run_records_expected_firmware_identity_gate():
    commit = "0123456789abcdef0123456789abcdef01234567"
    report = dry_run_report(commit)

    assert report["expected_firmware_commit"] == commit
    assert report["firmware_identity_required"] is True


def test_storage_utility_commands_map_to_console_responses():
    assert expected_command_name("storage map-tile-check remount1") == "storage map-tile-check"
    assert expected_command_name("storage export-canary exportTest1") == "storage export-canary"
    assert expected_command_name("storage export-diagnostics diagTest1") == "storage export-diagnostics"
    assert expected_command_name("storage export-data dataTest1") == "storage export-data"


def test_public_history_commands_map_to_console_response():
    assert expected_command_name("messages public") == "messages public"
    assert expected_command_name("messages public offset 8") == "messages public"
    assert (
        expected_command_name("messages public search weather offset 8")
        == "messages public"
    )


def test_contact_mutation_commands_map_to_console_responses():
    assert expected_command_name("contacts add A1B2C3D4E5F60789 CodexTemp") == "contacts add"
    assert expected_command_name("contacts import meshcore://contact/add?name=A&public_key=00&type=1") == "contacts import"
    assert expected_command_name("contacts rename A1B2C3D4E5F60789 CodexTemp2") == "contacts rename"
    assert expected_command_name("contacts delete A1B2C3D4E5F60789") == "contacts delete"


def test_protocol_acceptance_commands_map_to_exact_console_responses():
    fingerprint = "A1B2C3D4E5F60789"
    assert expected_command_name(
        f"routes telemetry {fingerprint}"
    ) == "routes telemetry"
    assert expected_command_name(
        f"routes trace contact {fingerprint}"
    ) == "routes trace contact"
    assert expected_command_name("routes trace status") == "routes trace status"
    assert expected_command_name(
        f"repeater ping {fingerprint}"
    ) == "repeater ping"
    assert expected_command_name(
        "repeater ping status"
    ) == "repeater ping status"
    assert expected_command_name(
        f"admin login {fingerprint} secret"
    ) == "admin login"


def test_rp2040_argument_commands_map_to_console_responses():
    assert expected_command_name("rp2040 baud-probe 700") == "rp2040 baud-probe"
    assert expected_command_name("rp2040 set-baud 115200") == "rp2040 set-baud"


def test_persistence_check_reboots_and_restores_original_fields(monkeypatch):
    monkeypatch.setattr(smoke_d1l.time, "sleep", lambda _seconds: None)
    calls = []
    real_send = smoke_d1l.send_console_command

    def tracked_send(ser, command, timeout):
        calls.append((command, timeout))
        return real_send(ser, command, timeout)

    monkeypatch.setattr(smoke_d1l, "send_console_command", tracked_send)
    ser = FakeSerial(persistence_responses())

    report = smoke_d1l.run_persistence_check(ser, timeout=1)

    assert report["ok"] is True
    assert report["reboot_command_passed"] is True
    assert report["reboot_route_flush"] == "ESP_OK"
    assert report["reboot_proven"] is True
    assert report["post_reboot_reset_reason"] == "SW"
    assert report["pre_reboot_boot_nonce"] == 11
    assert report["post_reboot_boot_nonce"] == 22
    assert report["cleanup_reboot_command_passed"] is True
    assert report["cleanup_reboot_route_flush"] == "ESP_OK"
    assert report["cleanup_reboot_proven"] is True
    assert report["cleanup_post_reboot_reset_reason"] == "SW"
    assert report["cleanup_pre_reboot_boot_nonce"] == 22
    assert report["cleanup_post_reboot_boot_nonce"] == 33
    assert report["settings_snapshot_valid"] is True
    assert report["original_node_name"] == "Krab Desk"
    assert report["original_path_hash_bytes"] == 2
    assert report["wifi_profile_before"] == {
        "setting_enabled": True,
        "profile_saved": True,
        "password_saved": True,
        "ssid": "Toddmas2.4",
    }
    assert report["after_reboot_ok"] is True
    assert report["cleanup_ok"] is True
    assert ser.writes == [
        "settings get\n",
        "wifi status\n",
        "settings set name D1L Smoke Persist\n",
        "settings set pathhash 3\n",
        "settings get\n",
        "health\n",
        "reboot\n",
        "health\n",
        "settings get\n",
        "wifi status\n",
        "settings set name Krab Desk\n",
        "settings set pathhash 2\n",
        "settings get\n",
        "health\n",
        "reboot\n",
        "health\n",
        "settings get\n",
        "wifi status\n",
    ]
    assert not any(
        command in ser.writes
        for command in ("settings reset\n", "wifi off\n", "ble off\n")
    )
    assert ser.reset_count == 2
    assert [timeout for command, timeout in calls if command == "reboot"] == [75.0, 75.0]
    assert [timeout for command, timeout in calls if command == "settings get"] == [1] * 5


def test_persistence_check_rejects_cancelled_reboot(monkeypatch):
    monkeypatch.setattr(smoke_d1l.time, "sleep", lambda _seconds: None)
    ser = FakeSerial(
        [
            settings_response("Krab Desk", 2),
            wifi_status_response(),
            '{"schema":1,"ok":true,"cmd":"settings set name","node_name":"D1L Smoke Persist"}\n',
            '{"schema":1,"ok":true,"cmd":"settings set pathhash","path_hash_bytes":3}\n',
            settings_response("D1L Smoke Persist", 3),
            health_response(11),
            '{"schema":1,"ok":false,"cmd":"reboot","code":"ESP_ERR_NVS_NOT_ENOUGH_SPACE"}\n',
            '{"schema":1,"ok":true,"cmd":"settings set name","node_name":"Krab Desk"}\n',
            '{"schema":1,"ok":true,"cmd":"settings set pathhash","path_hash_bytes":2}\n',
            settings_response("Krab Desk", 2),
            wifi_status_response(),
        ]
    )

    report = smoke_d1l.run_persistence_check(ser, timeout=1)

    assert report["reboot_command_passed"] is False
    assert report["reboot_proven"] is False
    assert report["after_reboot_ok"] is False
    assert report["cleanup_reboot_command_passed"] is False
    assert report["cleanup_ok"] is False
    assert report["cancelled_reboot_cleanup_attempted"] is True
    assert report["cancelled_reboot_cleanup_ok"] is True
    assert report["ok"] is False
    assert ser.reset_count == 0
    assert ser.writes[-5:] == [
        "reboot\n",
        "settings set name Krab Desk\n",
        "settings set pathhash 2\n",
        "settings get\n",
        "wifi status\n",
    ]
    assert "settings reset\n" not in ser.writes


@pytest.mark.parametrize("post_nonce", [11, None])
def test_successful_reboot_requires_changed_valid_boot_nonce(monkeypatch, post_nonce):
    monkeypatch.setattr(smoke_d1l.time, "sleep", lambda _seconds: None)
    ser = FakeSerial(persistence_responses(first_post_nonce=post_nonce))

    report = smoke_d1l.run_persistence_check(ser, timeout=1)

    assert report["reboot_command_passed"] is True
    assert report["reboot_proven"] is False
    assert report["after_reboot_ok"] is False
    assert report["ok"] is False


def test_successful_reboot_rejects_watchdog_reset(monkeypatch):
    monkeypatch.setattr(smoke_d1l.time, "sleep", lambda _seconds: None)
    ser = FakeSerial(persistence_responses(first_post_reset_reason="WDT"))

    report = smoke_d1l.run_persistence_check(ser, timeout=1)

    assert report["reboot_command_passed"] is True
    assert report["post_reboot_reset_reason"] == "WDT"
    assert report["reboot_proven"] is False
    assert report["after_reboot_ok"] is False
    assert report["ok"] is False


@pytest.mark.parametrize("cleanup_post_nonce", [22, None])
def test_cleanup_reboot_requires_changed_valid_boot_nonce(monkeypatch, cleanup_post_nonce):
    monkeypatch.setattr(smoke_d1l.time, "sleep", lambda _seconds: None)
    ser = FakeSerial(persistence_responses(cleanup_post_nonce=cleanup_post_nonce))

    report = smoke_d1l.run_persistence_check(ser, timeout=1)

    assert report["reboot_proven"] is True
    assert report["cleanup_reboot_command_passed"] is True
    assert report["cleanup_reboot_proven"] is False
    assert report["cleanup_ok"] is False
    assert report["ok"] is False


def test_cleanup_reboot_rejects_watchdog_reset(monkeypatch):
    monkeypatch.setattr(smoke_d1l.time, "sleep", lambda _seconds: None)
    ser = FakeSerial(persistence_responses(cleanup_post_reset_reason="WDT"))

    report = smoke_d1l.run_persistence_check(ser, timeout=1)

    assert report["reboot_proven"] is True
    assert report["cleanup_reboot_command_passed"] is True
    assert report["cleanup_post_reboot_reset_reason"] == "WDT"
    assert report["cleanup_reboot_proven"] is False
    assert report["cleanup_ok"] is False
    assert report["ok"] is False


def test_invalid_settings_snapshot_aborts_without_mutation():
    ser = FakeSerial(
        [
            settings_response("Krab Desk", 0),
            wifi_status_response(),
        ]
    )

    report = smoke_d1l.run_persistence_check(ser, timeout=1)

    assert report["settings_snapshot_valid"] is False
    assert report["mutation_started"] is False
    assert report["ok"] is False
    assert ser.writes == ["settings get\n", "wifi status\n"]


def test_failed_initial_mutation_never_reboots():
    ser = FakeSerial(
        [
            settings_response("Krab Desk", 2),
            wifi_status_response(),
            '{"schema":1,"ok":true,"cmd":"settings set name","node_name":"D1L Smoke Persist"}\n',
            '{"schema":1,"ok":true,"cmd":"settings set pathhash","path_hash_bytes":3}\n',
            settings_response("D1L Smoke Persist", 2),
            '{"schema":1,"ok":true,"cmd":"settings set name","node_name":"Krab Desk"}\n',
            '{"schema":1,"ok":true,"cmd":"settings set pathhash","path_hash_bytes":2}\n',
            settings_response("Krab Desk", 2),
            wifi_status_response(),
        ]
    )

    report = smoke_d1l.run_persistence_check(ser, timeout=1)

    assert report["mutation_verified"] is False
    assert report["reboot_attempted"] is False
    assert report["cancelled_reboot_cleanup_ok"] is True
    assert report["ok"] is False
    assert "health\n" not in ser.writes
    assert "reboot\n" not in ser.writes


def test_failed_initial_mutation_command_never_reboots_even_if_readback_matches():
    ser = FakeSerial(
        [
            settings_response("Krab Desk", 2),
            wifi_status_response(),
            '{"schema":1,"ok":false,"cmd":"settings set name","code":"ESP_FAIL"}\n',
            '{"schema":1,"ok":true,"cmd":"settings set pathhash","path_hash_bytes":3}\n',
            settings_response("D1L Smoke Persist", 3),
            '{"schema":1,"ok":true,"cmd":"settings set name","node_name":"Krab Desk"}\n',
            '{"schema":1,"ok":true,"cmd":"settings set pathhash","path_hash_bytes":2}\n',
            settings_response("Krab Desk", 2),
            wifi_status_response(),
        ]
    )

    report = smoke_d1l.run_persistence_check(ser, timeout=1)

    assert report["mutation_commands_ok"] is False
    assert report["mutation_verified"] is False
    assert report["reboot_attempted"] is False
    assert report["cancelled_reboot_cleanup_ok"] is True
    assert report["ok"] is False
    assert "health\n" not in ser.writes
    assert "reboot\n" not in ser.writes


def test_failed_restore_never_issues_cleanup_reboot(monkeypatch):
    monkeypatch.setattr(smoke_d1l.time, "sleep", lambda _seconds: None)
    responses = persistence_responses()[:10]
    responses.extend(
        [
            '{"schema":1,"ok":false,"cmd":"settings set name","code":"ESP_FAIL"}\n',
            '{"schema":1,"ok":true,"cmd":"settings set pathhash","path_hash_bytes":2}\n',
            settings_response("D1L Smoke Persist", 2),
            '{"schema":1,"ok":false,"cmd":"settings set name","code":"ESP_FAIL"}\n',
            '{"schema":1,"ok":true,"cmd":"settings set pathhash","path_hash_bytes":2}\n',
            settings_response("D1L Smoke Persist", 2),
            settings_response("D1L Smoke Persist", 2),
            wifi_status_response(),
        ]
    )
    ser = FakeSerial(responses)

    report = smoke_d1l.run_persistence_check(ser, timeout=1)

    assert report["reboot_proven"] is True
    assert report["restore_before_cleanup_reboot_ok"] is False
    assert report["cleanup_reboot_command_passed"] is False
    assert report["ok"] is False
    assert ser.writes.count("reboot\n") == 1
    assert ser.writes[-2:] == ["settings get\n", "wifi status\n"]


def test_changed_wifi_profile_fails_preservation_evidence(monkeypatch):
    monkeypatch.setattr(smoke_d1l.time, "sleep", lambda _seconds: None)
    ser = FakeSerial(persistence_responses(cleanup_ssid="OtherNetwork"))

    report = smoke_d1l.run_persistence_check(ser, timeout=1)

    assert report["cleanup_reboot_proven"] is True
    assert report["cleanup_ok"] is False
    assert report["ok"] is False


def test_reboot_timeout_floor_does_not_change_other_commands():
    assert smoke_d1l.timeout_for_reboot_command("reboot", 5.0) == 75.0
    assert smoke_d1l.timeout_for_reboot_command("reboot", 25.0) == 75.0
    assert smoke_d1l.timeout_for_reboot_command("reboot", 90.0) == 90.0
    assert smoke_d1l.timeout_for_reboot_command("health", 5.0) == 5.0


def test_reboot_command_requires_system_reset_and_all_quiesce_receipts():
    valid = {
        "ok": True,
        "rebooting": True,
        "reset_scope": "system",
        "storage_manager_quiesced": True,
        "retained_worker_quiesced": True,
        "rp2040_bridge_quiesced": True,
        "connectivity_prepare": "ESP_OK",
        "retained_flush": "ESP_OK",
        "route_flush": "ESP_OK",
    }
    assert smoke_d1l.reboot_command_passed(valid) is True
    for field in ("reset_scope", "connectivity_prepare"):
        missing = dict(valid)
        del missing[field]
        assert smoke_d1l.reboot_command_passed(missing) is False
    cpu_reset = dict(valid, reset_scope="cpu")
    assert smoke_d1l.reboot_command_passed(cpu_reset) is False
    connectivity_failure = dict(valid, connectivity_prepare="ESP_FAIL")
    assert smoke_d1l.reboot_command_passed(connectivity_failure) is False
    assert smoke_d1l.reboot_command_passed(
        {
            "ok": True,
            "rebooting": True,
            "storage_manager_quiesced": True,
            "rp2040_bridge_quiesced": True,
            "retained_flush": "ESP_OK",
            "route_flush": "ESP_OK",
        }
    ) is False
    assert smoke_d1l.reboot_command_passed(
        {
            "ok": True,
            "rebooting": True,
        }
    ) is False
    assert smoke_d1l.reboot_command_passed(
        {
            "ok": True,
            "rebooting": True,
            "route_flush": "ESP_FAIL",
        }
    ) is False


def test_boot_nonce_parser_rejects_missing_zero_and_bool_values():
    assert smoke_d1l.health_boot_nonce({"ok": True}) is None
    assert smoke_d1l.health_boot_nonce({"ok": True, "boot_nonce": 0}) is None
    assert smoke_d1l.health_boot_nonce({"ok": True, "boot_nonce": True}) is None
    assert smoke_d1l.health_boot_nonce({"ok": True, "boot_nonce": 7}) == 7


def test_new_crash_like_entries_only_returns_post_baseline_crashes():
    before = {"ok": True, "total_written": 24}
    after = {
        "ok": True,
        "total_written": 27,
        "entries": [
            {"seq": 23, "crash_like": True},
            {"seq": 25, "crash_like": False},
            {"seq": 26, "crash_like": True, "reset_reason": "PANIC"},
            {"seq": 27, "crash_like": True, "reset_reason": "WDT"},
        ],
    }

    assert smoke_d1l.new_crash_like_entries(before, after) == after["entries"][2:]


@pytest.mark.parametrize(
    "result",
    [
        None,
        {},
        {"ok": False, "total_written": 2},
        {"ok": True, "total_written": -1},
        {"ok": True, "total_written": True},
    ],
)
def test_crashlog_total_written_rejects_unusable_values(result):
    assert smoke_d1l.crashlog_total_written(result) is None
