import copy
import json
from pathlib import Path

import pytest

from scripts import wifi_resilience_d1l as wifi


COMMIT = "2dcd494b8f598cda74321d482aa69ab5cf40a7d5"
TARGET = "Mesh Lab 2.4"
SECRET_SENTINEL = "never-store-this-password"
ROOT = Path(__file__).resolve().parents[1]


def windows_target_snapshot():
    return wifi.resolve_core_target(
        "COM12",
        port_lister=lambda: [
            {
                "device": "COM12",
                "vid": 0x1A86,
                "pid": 0x7523,
                "serial_number": "D1L-TEST",
                "hwid": "USB VID:PID=1A86:7523",
                "location": "1-1",
            }
        ],
        platform_name="nt",
    )


def make_args(*extra: str):
    return wifi.parse_args(
        [
            "--port",
            "COM12",
            "--target-ssid",
            TARGET,
            "--expected-firmware-commit",
            COMMIT,
            "--wait-sec",
            "0.01",
            "--poll-sec",
            "0.01",
            "--open-settle-sec",
            "0",
            *extra,
        ]
    )


class FakeWifiDevice:
    def __init__(
        self,
        *,
        enabled=False,
        profile_ssid=TARGET,
        wrong_on_identity=False,
        scan_uses_aps=False,
        target_in_scan=True,
        flip_boot_nonce=False,
        current_stack=1024,
        retry_stack=2304,
    ):
        self.enabled = enabled
        self.connected = enabled
        self.profile_ssid = profile_ssid
        self.wrong_on_identity = wrong_on_identity
        self.scan_uses_aps = scan_uses_aps
        self.target_in_scan = target_in_scan
        self.flip_boot_nonce = flip_boot_nonce
        self.current_stack = current_stack
        self.retry_stack = retry_stack
        self.commands = []
        self.health_count = 0
        self.uptime = 1000

    def status(self, command="wifi status"):
        connected = self.enabled and self.connected
        return {
            "schema": 1,
            "ok": True,
            "cmd": command,
            "setting_enabled": self.enabled,
            "build_enabled": True,
            "stack_active": self.enabled,
            "connected": connected,
            "connecting": False,
            "scan_supported": True,
            "profile_saved": True,
            "password_saved": True,
            "ssid": self.profile_ssid,
            "state": "connected"
            if connected
            else ("starting" if self.enabled else "off"),
            "ip": "192.0.2.44" if connected else "",
            "rssi_dbm": -42 if connected else 0,
            "channel": 6 if connected else 0,
            "retry_scheduled": False,
            "retry_attempt": 0,
            "retry_delay_ms": 0,
            "user_cancelled": False,
            "safe_mode": False,
            "last_disconnect_reason": 0,
            "last_failure_class": "none",
            "retry_task_stack_high_water_bytes": self.retry_stack
            if self.enabled
            else 0,
            "boot_guard_ready": True,
            "boot_guard_recovered": False,
            "consecutive_crash_boots": 0,
            "crash_loop_detected": False,
            "last_active_subsystem": "wifi" if self.enabled else "none",
            "boot_guard_error": "ESP_OK",
            "last_error": "none",
            "policy": "offline_first_one_companion_radio",
            "live_network": self.enabled,
            "password": SECRET_SENTINEL,
        }

    def health(self):
        self.health_count += 1
        self.uptime += 10
        nonce = 201 if self.flip_boot_nonce and self.health_count > 1 else 101
        return {
            "schema": 1,
            "ok": True,
            "cmd": "health",
            "boot_nonce": nonce,
            "uptime_ms": self.uptime,
            "heap_free": 4_500_000,
            "internal_heap_free": 50_000,
            "dma_heap_free": 42_000,
            "psram_free": 4_400_000,
            "current_task_stack_free_words": self.current_stack,
            "ui_task_stack_free_words": 1200,
            "retained_task_stack_free_bytes": 7000,
            "nvs_ready": True,
            "nvs_error": "ESP_OK",
            "reset_reason": "POWERON",
            "board_ready": True,
            "ui_ready": True,
            "password": SECRET_SENTINEL,
        }

    def __call__(self, command, _timeout):
        self.commands.append(command)
        if command == "version":
            return {
                "schema": 1,
                "ok": True,
                "cmd": "version",
                "build_commit": COMMIT,
                "password": SECRET_SENTINEL,
            }
        if command == "health":
            return self.health()
        if command == "wifi status":
            return self.status()
        if command == "wifi on":
            self.enabled = True
            self.connected = True
            return self.status("wifi status" if self.wrong_on_identity else "wifi on")
        if command == "wifi off":
            self.enabled = False
            self.connected = False
            return {
                "schema": 1,
                "ok": True,
                "cmd": "wifi off",
                "persisted": True,
                "setting_enabled": False,
                "build_enabled": True,
                "state": "off",
                "password": SECRET_SENTINEL,
            }
        if command == "wifi connect":
            self.connected = True
            return {
                "schema": 1,
                "ok": True,
                "cmd": "wifi connect",
                "initiated": True,
                "setting_enabled": self.enabled,
                "profile_saved": True,
                "state": "connecting",
                "ssid": self.profile_ssid,
                "password_printed": False,
                "public_rf_tx": False,
                "password": SECRET_SENTINEL,
            }
        if command == "wifi scan":
            networks = [
                {"ssid": "Other", "rssi_dbm": -55, "channel": 1, "auth": "wpa2_psk"}
            ]
            if self.target_in_scan:
                networks.append(
                    {"ssid": TARGET, "rssi_dbm": -41, "channel": 6, "auth": "wpa2_psk"}
                )
            result = {
                "schema": 1,
                "ok": True,
                "cmd": "wifi scan",
                "scan_started": True,
                "setting_enabled": True,
                "build_enabled": True,
                "scan_supported": True,
                "profile_saved": True,
                "state": "connected",
                "reason": "ok",
                "total_count": len(networks),
                "returned_count": len(networks),
                "truncated": False,
                "public_rf_tx": False,
                "password_printed": False,
                "password": SECRET_SENTINEL,
            }
            result["aps" if self.scan_uses_aps else "networks"] = networks
            return result
        raise AssertionError(command)


@pytest.mark.parametrize("number", ["8", "11", "15", "29"])
def test_forbidden_ports_are_rejected(number):
    with pytest.raises(ValueError, match="forbidden port"):
        wifi.enforce_port_guard("COM" + number)


def test_runner_accepts_only_the_two_authorized_d1l_targets():
    assert wifi.enforce_port_guard(" com12 ") == "COM12"
    assert wifi.enforce_port_guard(wifi.D1L_POSIX_PORT) == wifi.D1L_POSIX_PORT
    with pytest.raises(ValueError, match="requires COM12"):
        wifi.enforce_port_guard("COM7")
    with pytest.raises(ValueError, match="requires COM12"):
        wifi.enforce_port_guard("/dev/ttyUSB2")


@pytest.mark.parametrize(
    "command",
    [
        "wifi save MeshLab secret",
        "wifi clear",
        "storage sd format-confirm",
        "rp2040 uf2",
        "mesh send public test",
    ],
)
def test_command_allowlist_refuses_credentials_rf_sd_and_rp2040(command):
    with pytest.raises(ValueError, match="outside Wi-Fi acceptance allowlist"):
        wifi.enforce_safe_command(command)


def test_dry_run_is_truthful_non_hardware_and_has_no_password_surface():
    args = make_args("--dry-run")
    called = False

    def should_not_run(_command, _timeout):
        nonlocal called
        called = True
        raise AssertionError("dry-run touched a command sender")

    report = wifi.run_acceptance(args, command_sender=should_not_run)

    assert report["ok"] is True
    assert report["classification"] == "dry_run_only"
    assert report["truth"] == {
        "physical_observed": False,
        "simulated": False,
        "dry_run": True,
        "source_inspection": False,
        "exact_commit_verified": False,
    }
    assert report["acceptance_passed"] is False
    assert report["feature_evidence_eligible"] is False
    assert report["release_gate_eligible"] is False
    assert report["commands"] == []
    assert called is False
    assert wifi.validate_completed_report(report) is False
    assert "password" not in vars(args)
    assert all(
        step["command"] in wifi.SAFE_COMMANDS for step in report["planned_commands"]
    )


def test_saved_profile_cycle_uses_networks_restores_off_and_redacts_unknown_secret():
    device = FakeWifiDevice(enabled=False)

    report = wifi.run_acceptance(
        make_args(), command_sender=device, sleep=lambda _seconds: None
    )

    assert report["ok"] is True
    assert report["mode"] == "simulation"
    assert report["simulated"] is True
    assert report["physical_observed"] is False
    assert report["acceptance_passed"] is False
    assert report["cycles_completed"] == 1
    assert report["checks"]["networks_schema_consumed"] is True
    assert report["checks"]["bounded_scan_target_seen"] is True
    assert report["profile_preserved"] is True
    assert report["enable_intent_restored"] is True
    assert device.enabled is False
    assert "wifi scan" in report["commands"]
    assert "wifi connect" in report["commands"]
    assert "wifi off" in report["commands"]
    assert set(report["commands"]) <= wifi.SAFE_COMMANDS
    assert SECRET_SENTINEL not in json.dumps(report, sort_keys=True)
    assert not any(command.startswith("wifi save") for command in report["commands"])
    assert "wifi clear" not in report["commands"]


def test_enabled_intent_is_restored_connected_after_disable_cycle():
    device = FakeWifiDevice(enabled=True)

    report = wifi.run_acceptance(
        make_args(), command_sender=device, sleep=lambda _seconds: None
    )

    assert report["ok"] is True
    assert report["original_enable_intent"] is True
    assert report["profile_before"] == report["profile_after"]
    assert report["profile_after"]["setting_enabled"] is True
    assert device.enabled is True
    assert device.connected is True
    assert device.commands.count("wifi off") == 1
    assert device.commands.count("wifi on") == 1


def test_wifi_on_response_identity_mismatch_fails_but_restores_original_off_intent():
    device = FakeWifiDevice(enabled=False, wrong_on_identity=True)

    report = wifi.run_acceptance(
        make_args(), command_sender=device, sleep=lambda _seconds: None
    )

    assert report["ok"] is False
    assert report["classification"] == "response_identity_mismatch"
    assert report["failure"]["code"] == "response_identity_mismatch"
    assert device.enabled is False
    assert "wifi off" in device.commands
    assert report["enable_intent_restored"] is True


def test_scan_requires_networks_and_does_not_accept_legacy_aps_alias():
    device = FakeWifiDevice(enabled=False, scan_uses_aps=True)

    report = wifi.run_acceptance(
        make_args(), command_sender=device, sleep=lambda _seconds: None
    )

    assert report["ok"] is False
    assert report["classification"] == "wifi_scan_schema_invalid"
    assert device.enabled is False
    assert report["enable_intent_restored"] is True


def test_scan_fails_closed_when_exact_target_is_not_seen():
    device = FakeWifiDevice(enabled=False, target_in_scan=False)

    report = wifi.run_acceptance(
        make_args(), command_sender=device, sleep=lambda _seconds: None
    )

    assert report["ok"] is False
    assert report["classification"] == "target_ssid_not_seen"
    assert device.enabled is False


def test_saved_profile_target_mismatch_is_rejected_before_mutation():
    device = FakeWifiDevice(enabled=False, profile_ssid="Different network")

    report = wifi.run_acceptance(
        make_args(), command_sender=device, sleep=lambda _seconds: None
    )

    assert report["ok"] is False
    assert report["classification"] == "target_ssid_mismatch"
    assert report["mutation_attempted"] is False
    assert "wifi on" not in device.commands
    assert "wifi off" not in device.commands


def test_boot_nonce_change_fails_closed_and_restores_original_intent():
    device = FakeWifiDevice(enabled=False, flip_boot_nonce=True)

    report = wifi.run_acceptance(
        make_args(), command_sender=device, sleep=lambda _seconds: None
    )

    assert report["ok"] is False
    assert report["classification"] == "boot_nonce_changed"
    assert device.enabled is False
    assert "wifi off" in device.commands


def test_health_stack_threshold_is_fail_closed_before_wifi_mutation():
    device = FakeWifiDevice(enabled=False, current_stack=100)

    report = wifi.run_acceptance(
        make_args(), command_sender=device, sleep=lambda _seconds: None
    )

    assert report["ok"] is False
    assert report["classification"] == "current_stack_low"
    assert "wifi on" not in device.commands
    assert "wifi off" not in device.commands


def test_serial_open_failure_returns_a_redacted_receipt_without_commands():
    class BrokenSerialModule:
        @staticmethod
        def Serial(**_kwargs):
            raise OSError(SECRET_SENTINEL)

    report = wifi.run_acceptance(
        make_args(),
        serial_module=BrokenSerialModule(),
        sleep=lambda _seconds: None,
        port_lister=lambda: [
            {
                "device": "COM12",
                "vid": 0x1A86,
                "pid": 0x7523,
                "serial_number": "D1L-TEST",
                "hwid": "USB VID:PID=1A86:7523",
                "location": "1-1",
            }
        ],
        platform_name="nt",
    )

    assert report["ok"] is False
    assert report["classification"] == "serial_open_failed"
    assert report["failure"]["message"] == "OSError"
    assert SECRET_SENTINEL not in json.dumps(report, sort_keys=True)
    assert report["commands"] == []


def test_completed_validator_rejects_simulation_and_derives_physical_fixture():
    report = wifi.run_acceptance(
        make_args(), command_sender=FakeWifiDevice(), sleep=lambda _seconds: None
    )
    assert wifi.validate_completed_report(report) is False

    physical = copy.deepcopy(report)
    physical["mode"] = "hardware"
    physical["truth"]["physical_observed"] = True
    physical["truth"]["simulated"] = False
    physical["physical_observed"] = True
    physical["simulated"] = False
    physical["acceptance_passed"] = True
    physical["commit"] = COMMIT
    physical["git"] = {"dirty": False}
    physical["release_gate_eligible"] = False
    assert wifi.validate_completed_report(physical) is False

    physical["hardware_transport"] = {
        "opened_by_runner": True,
        "port": "COM12",
        "baud": 115200,
        "closed_by_runner": True,
    }
    physical["d1l_target"] = windows_target_snapshot()
    physical["d1l_target_after"] = windows_target_snapshot()
    assert wifi.validate_completed_report(physical) is True

    for missing_label in (
        "cycle-1-enable",
        "cycle-1-reconnect",
        "cycle-1-disable",
    ):
        missing_action = copy.deepcopy(physical)
        step_index = next(
            index
            for index, step in enumerate(missing_action["steps"])
            if step["label"] == missing_label
        )
        del missing_action["steps"][step_index]
        del missing_action["commands"][step_index]
        for index, step in enumerate(missing_action["steps"], start=1):
            step["index"] = index
        assert wifi.validate_completed_report(missing_action) is False

    for field, value in (
        ("setting_enabled", False),
        ("build_enabled", False),
        ("profile_saved", False),
        ("ssid", "Different network"),
    ):
        invalid_enable = copy.deepcopy(physical)
        enable_response = next(
            step["response"]
            for step in invalid_enable["steps"]
            if step["label"] == "cycle-1-enable"
        )
        enable_response[field] = value
        assert wifi.validate_completed_report(invalid_enable) is False

    physical["steps"][0]["response"]["build_commit"] = "f" * 40
    assert wifi.validate_completed_report(physical) is False
    physical["steps"][0]["response"]["build_commit"] = COMMIT

    physical["cycles"][0]["connected"]["ip"] = "0.0.0.0"
    assert wifi.validate_completed_report(physical) is False
    physical["cycles"][0]["connected"]["ip"] = "192.0.2.44"

    physical["release_gate_eligible"] = True
    assert wifi.validate_completed_report(physical) is False
    physical["release_gate_eligible"] = False
    physical["truth"]["dry_run"] = True
    assert wifi.validate_completed_report(physical) is False


def test_default_receipt_name_uses_exact_sha_and_d1l_port():
    report = {
        "mode": "hardware",
        "expected_firmware_commit": COMMIT,
        "port": "COM12",
    }
    path = wifi.default_out_path(report)
    assert path.name == f"wifi_saved_profile_resilience_{COMMIT}_COM12.json"
    assert path.parent.name == "com12"


def test_feature_receipt_can_never_be_promoted_to_full_wp13_release_gate(
    tmp_path, monkeypatch
):
    report = wifi.run_acceptance(
        make_args(), command_sender=FakeWifiDevice(), sleep=lambda _seconds: None
    )
    report["mode"] = "hardware"
    report["truth"]["physical_observed"] = True
    report["truth"]["simulated"] = False
    report["physical_observed"] = True
    report["simulated"] = False
    report["acceptance_passed"] = True
    report["hardware_transport"] = {
        "opened_by_runner": True,
        "port": "COM12",
        "baud": 115200,
        "closed_by_runner": True,
    }
    report["d1l_target"] = windows_target_snapshot()
    report["d1l_target_after"] = windows_target_snapshot()

    def exact_stamp(payload, _root):
        payload["commit"] = COMMIT
        payload["git"] = {"dirty": False}

    monkeypatch.setattr(wifi, "stamp_report", exact_stamp)
    out = wifi.write_report(report, tmp_path / "receipt.json")
    written = json.loads(out.read_text(encoding="ascii"))

    assert written["feature_evidence_eligible"] is True
    assert written["release_gate_eligible"] is False
    assert written["full_wp13_matrix_covered"] is False
    assert written["uncovered_release_scenarios"] == list(
        wifi.FULL_WP13_UNCOVERED_SCENARIOS
    )


def test_connected_status_requires_one_kib_retry_stack_margin():
    low = FakeWifiDevice(enabled=True, retry_stack=1023).status()
    exact = FakeWifiDevice(enabled=True, retry_stack=1024).status()

    assert wifi.connected_status_ok(wifi.wifi_status_snapshot(low), TARGET) is False
    assert wifi.connected_status_ok(wifi.wifi_status_snapshot(exact), TARGET) is True


def test_health_summary_allows_expected_active_allocation_after_recovery():
    first = wifi.health_snapshot(FakeWifiDevice().health())
    transient = copy.deepcopy(first)
    transient["heap_free"] -= wifi.MAX_HEAP_LOSS_BYTES + 1
    recovered = copy.deepcopy(first)

    summary = wifi._health_summary([first, transient, recovered])

    assert (
        summary["peak_heap_loss_bytes"]["heap_free"]
        == wifi.MAX_HEAP_LOSS_BYTES + 1
    )
    assert summary["heap_loss_bytes"]["heap_free"] == 0
    assert summary["ok"] is True


def test_health_summary_rejects_unrecovered_heap_loss():
    first = wifi.health_snapshot(FakeWifiDevice().health())
    leaked = copy.deepcopy(first)
    leaked["heap_free"] -= wifi.MAX_HEAP_LOSS_BYTES + 1

    summary = wifi._health_summary([first, leaked])

    assert summary["heap_loss_bytes"]["heap_free"] == (
        wifi.MAX_HEAP_LOSS_BYTES + 1
    )
    assert summary["ok"] is False


def test_health_summary_rejects_transient_internal_heap_exhaustion():
    first = wifi.health_snapshot(FakeWifiDevice().health())
    transient = copy.deepcopy(first)
    transient["internal_heap_free"] = wifi.MIN_INTERNAL_HEAP_FREE_BYTES - 1
    recovered = copy.deepcopy(first)

    summary = wifi._health_summary([first, transient, recovered])

    assert summary["minimum_free_bytes"]["internal_heap_free"] == (
        wifi.MIN_INTERNAL_HEAP_FREE_BYTES - 1
    )
    assert summary["ok"] is False


def test_console_wifi_on_preserves_its_machine_readable_identity():
    console = (ROOT / "main/comms/usb_console.c").read_text(encoding="utf-8")
    printer = console.split("static void print_wifi_status_result", 1)[1].split(
        "static void cmd_wifi_status", 1
    )[0]
    status = console.split("static void cmd_wifi_status", 1)[1].split(
        "static void cmd_wifi_off", 1
    )[0]
    enabled = console.split("static void cmd_wifi_on", 1)[1].split(
        "static void cmd_wifi_save", 1
    )[0]

    assert "ok_begin(command);" in printer
    assert 'print_wifi_status_result("wifi status")' in status
    assert 'print_wifi_status_result("wifi on")' in enabled
    assert "cmd_wifi_status();" not in enabled
