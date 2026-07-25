#!/usr/bin/env python3
"""Fail-closed saved-profile Wi-Fi resilience acceptance for DeskOS D1L.

The runner is deliberately limited to the configured D1L ESP32 console. It never
accepts a Wi-Fi password, never rewrites or clears the saved profile, never
opens the RP2040, never formats storage, and never sends Mesh RF traffic.
Provision credentials separately before using this acceptance runner. On
``neopi5`` it opens only the stable D1L by-id selector after binding its USB
identity; the resolved ``/dev/ttyUSB*`` path is evidence and is never opened.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import math
import os
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

try:
    from artifact_metadata import stamp_report
    from core_smoke_d1l import enforce_core_port, resolve_core_target
    from d1l_serial_target import POSIX_D1L_TARGET, safe_slug, validate_snapshot
    from smoke_d1l import exact_commit, open_d1l_serial, send_console_command
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import stamp_report
    from scripts.core_smoke_d1l import enforce_core_port, resolve_core_target
    from scripts.d1l_serial_target import (
        POSIX_D1L_TARGET,
        safe_slug,
        validate_snapshot,
    )
    from scripts.smoke_d1l import exact_commit, open_d1l_serial, send_console_command


ROOT = Path(__file__).resolve().parents[1]
D1L_PORT_NUMBER = 12
D1L_PORT = f"COM{D1L_PORT_NUMBER}"
D1L_POSIX_PORT = POSIX_D1L_TARGET
D1L_CONSOLE_BAUD = 115200
FORBIDDEN_PORTS = {"COM" + number for number in ("8", "11", "15", "29")}
SAFE_COMMANDS = frozenset(
    {
        "version",
        "health",
        "wifi status",
        "wifi on",
        "wifi off",
        "wifi scan",
        "wifi connect",
    }
)
WIFI_SCAN_MAX = 8
RETAINED_STACK_MIN_BYTES = 4096
CURRENT_STACK_MIN_WORDS = 256
UI_STACK_MIN_WORDS = 512
WIFI_RETRY_STACK_MIN_BYTES = 1024
MAX_HEAP_LOSS_BYTES = 65536
MAX_INTERNAL_HEAP_LOSS_BYTES = 32768
MAX_DMA_HEAP_LOSS_BYTES = 32768
MAX_PSRAM_LOSS_BYTES = 262144
FEATURE_CLAIM_SCOPE = "saved_profile_scan_connect_toggle_subset"
FULL_WP13_UNCOVERED_SCENARIOS = (
    "credential_at_rest_threat_model_and_nvs_encryption_decision",
    "wifi_ble_coexistence",
    "access_point_reboot_recovery",
    "wrong_password_recovery",
    "weak_signal_recovery",
    "saved_profile_device_reboot",
    "safe_mode_recovery",
    "one_hundred_live_reconnect_cycles",
    "map_visible_only_resume",
    "current_idf_exact_device_qualification",
)


class AcceptanceFailure(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def normalize_port(value: str) -> str:
    port = str(value or "").strip()
    return D1L_POSIX_PORT if port == D1L_POSIX_PORT else port.upper()


def enforce_port_guard(value: str) -> str:
    port = normalize_port(value)
    if port in FORBIDDEN_PORTS:
        raise ValueError(f"refusing forbidden port {port}")
    try:
        return enforce_core_port(port)
    except ValueError as exc:
        raise ValueError(
            f"this D1L acceptance runner requires {D1L_PORT} or the exact "
            f"{D1L_POSIX_PORT} by-id target"
        ) from exc


def enforce_safe_command(command: str) -> None:
    if command not in SAFE_COMMANDS:
        raise ValueError(
            f"refusing command outside Wi-Fi acceptance allowlist: {command}"
        )


def _integer(value: object, *, minimum: int | None = None) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    if minimum is not None and value < minimum:
        return None
    return value


def _safe_code(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    value = value[:64]
    return value if re.fullmatch(r"[A-Za-z0-9_.-]+", value) else None


def _require_command_result(result: object, command: str) -> dict:
    if not isinstance(result, dict) or result.get("schema") != 1:
        raise AcceptanceFailure(
            "response_schema_invalid", "machine-readable response is missing"
        )
    if result.get("cmd") != command:
        raise AcceptanceFailure(
            "response_identity_mismatch",
            f"{command} did not return its own command identity",
        )
    if result.get("ok") is not True:
        code = _safe_code(result.get("code")) or "DEVICE_ERROR"
        raise AcceptanceFailure("command_failed", f"{command} failed with {code}")
    return result


def profile_metadata(status: dict) -> dict:
    return {
        "setting_enabled": status["setting_enabled"],
        "profile_saved": status["profile_saved"],
        "password_saved": status["password_saved"],
        "ssid": status["ssid"],
    }


def wifi_status_snapshot(result: object, command: str = "wifi status") -> dict:
    row = _require_command_result(result, command)
    bool_fields = (
        "setting_enabled",
        "build_enabled",
        "stack_active",
        "connected",
        "connecting",
        "scan_supported",
        "profile_saved",
        "password_saved",
        "retry_scheduled",
        "user_cancelled",
        "safe_mode",
        "boot_guard_ready",
        "boot_guard_recovered",
        "crash_loop_detected",
        "live_network",
    )
    text_fields = (
        "ssid",
        "state",
        "ip",
        "last_failure_class",
        "last_active_subsystem",
        "boot_guard_error",
        "last_error",
        "policy",
    )
    int_fields = (
        "rssi_dbm",
        "channel",
        "retry_attempt",
        "retry_delay_ms",
        "last_disconnect_reason",
        "retry_task_stack_high_water_bytes",
        "consecutive_crash_boots",
    )
    if any(not isinstance(row.get(field), bool) for field in bool_fields):
        raise AcceptanceFailure(
            "wifi_status_schema_invalid", "Wi-Fi status boolean field is invalid"
        )
    if any(not isinstance(row.get(field), str) for field in text_fields):
        raise AcceptanceFailure(
            "wifi_status_schema_invalid", "Wi-Fi status text field is invalid"
        )
    if any(_integer(row.get(field)) is None for field in int_fields):
        raise AcceptanceFailure(
            "wifi_status_schema_invalid", "Wi-Fi status numeric field is invalid"
        )
    return {field: row[field] for field in (*bool_fields, *text_fields, *int_fields)}


def _require_saved_target(status: dict, target_ssid: str) -> None:
    if status.get("profile_saved") is not True or not status.get("ssid"):
        raise AcceptanceFailure(
            "saved_profile_required",
            "a pre-provisioned saved Wi-Fi profile is required",
        )
    if status.get("ssid") != target_ssid:
        raise AcceptanceFailure(
            "target_ssid_mismatch",
            "saved profile does not exactly match the requested target SSID",
        )


def _valid_ip(value: object) -> bool:
    if not isinstance(value, str) or not value:
        return False
    try:
        address = ipaddress.ip_address(value)
    except ValueError:
        return False
    return address.version == 4 and not address.is_unspecified


def connected_status_ok(status: dict, target_ssid: str) -> bool:
    _require_saved_target(status, target_ssid)
    return (
        status["setting_enabled"] is True
        and status["build_enabled"] is True
        and status["stack_active"] is True
        and status["connected"] is True
        and status["connecting"] is False
        and status["scan_supported"] is True
        and status["state"] == "connected"
        and status["live_network"] is True
        and _valid_ip(status["ip"])
        and -127 <= status["rssi_dbm"] < 0
        and 1 <= status["channel"] <= 14
        and status["retry_task_stack_high_water_bytes"] >= WIFI_RETRY_STACK_MIN_BYTES
        and status["safe_mode"] is False
        and status["crash_loop_detected"] is False
        and status["boot_guard_ready"] is True
    )


def disabled_status_ok(status: dict, target_ssid: str) -> bool:
    _require_saved_target(status, target_ssid)
    return (
        status["setting_enabled"] is False
        and status["stack_active"] is False
        and status["connected"] is False
        and status["connecting"] is False
        and status["state"] == "off"
        and status["ip"] == ""
        and status["live_network"] is False
    )


def health_snapshot(result: object) -> dict:
    row = _require_command_result(result, "health")
    bool_fields = ("nvs_ready", "board_ready", "ui_ready")
    int_fields = (
        "boot_nonce",
        "uptime_ms",
        "heap_free",
        "internal_heap_free",
        "dma_heap_free",
        "psram_free",
        "current_task_stack_free_words",
        "ui_task_stack_free_words",
        "retained_task_stack_free_bytes",
    )
    if any(not isinstance(row.get(field), bool) for field in bool_fields):
        raise AcceptanceFailure(
            "health_schema_invalid", "health readiness field is invalid"
        )
    if any(_integer(row.get(field), minimum=1) is None for field in int_fields):
        raise AcceptanceFailure(
            "health_schema_invalid", "health numeric field is invalid"
        )
    if not all(row[field] for field in bool_fields) or row.get("nvs_error") != "ESP_OK":
        raise AcceptanceFailure(
            "health_not_ready", "board, UI, or NVS health is not ready"
        )
    if row["current_task_stack_free_words"] < CURRENT_STACK_MIN_WORDS:
        raise AcceptanceFailure(
            "current_stack_low", "console task stack margin is below threshold"
        )
    if row["ui_task_stack_free_words"] < UI_STACK_MIN_WORDS:
        raise AcceptanceFailure(
            "ui_stack_low", "UI task stack margin is below threshold"
        )
    if row["retained_task_stack_free_bytes"] < RETAINED_STACK_MIN_BYTES:
        raise AcceptanceFailure(
            "retained_stack_low", "retained worker stack margin is below threshold"
        )
    return {
        **{field: row[field] for field in bool_fields},
        **{field: row[field] for field in int_fields},
        "nvs_error": row["nvs_error"],
        "reset_reason": row.get("reset_reason")
        if isinstance(row.get("reset_reason"), str)
        else "unknown",
    }


def scan_snapshot(result: object, target_ssid: str) -> dict:
    row = _require_command_result(result, "wifi scan")
    networks = row.get("networks")
    if not isinstance(networks, list):
        raise AcceptanceFailure(
            "wifi_scan_schema_invalid",
            "wifi scan must expose the bounded networks array",
        )
    total = _integer(row.get("total_count"), minimum=0)
    returned = _integer(row.get("returned_count"), minimum=0)
    if (
        row.get("scan_started") is not True
        or row.get("setting_enabled") is not True
        or row.get("build_enabled") is not True
        or row.get("scan_supported") is not True
        or row.get("profile_saved") is not True
        or row.get("reason") != "ok"
        or total is None
        or returned is None
        or returned != len(networks)
        or returned > WIFI_SCAN_MAX
        or total < returned
        or not isinstance(row.get("truncated"), bool)
        or row.get("public_rf_tx") is not False
        or row.get("password_printed") is not False
    ):
        raise AcceptanceFailure("wifi_scan_invalid", "bounded Wi-Fi scan checks failed")
    clean: list[dict] = []
    for network in networks:
        if not isinstance(network, dict):
            raise AcceptanceFailure(
                "wifi_scan_schema_invalid", "Wi-Fi scan network entry is invalid"
            )
        ssid = network.get("ssid")
        rssi = _integer(network.get("rssi_dbm"))
        channel = _integer(network.get("channel"))
        auth = network.get("auth")
        if (
            not isinstance(ssid, str)
            or rssi is None
            or not -127 <= rssi <= 0
            or channel is None
            or not 1 <= channel <= 14
            or not isinstance(auth, str)
        ):
            raise AcceptanceFailure(
                "wifi_scan_schema_invalid", "Wi-Fi scan network field is invalid"
            )
        clean.append({"ssid": ssid, "rssi_dbm": rssi, "channel": channel, "auth": auth})
    matches = [network for network in clean if network["ssid"] == target_ssid]
    if not matches:
        raise AcceptanceFailure(
            "target_ssid_not_seen", "target SSID was absent from bounded scan"
        )
    return {
        "total_count": total,
        "returned_count": returned,
        "truncated": row["truncated"],
        "networks": clean,
        "target_matches": matches,
    }


def _summarize_result(command: str, result: object) -> dict:
    if not isinstance(result, dict):
        return {"schema": None, "ok": False, "cmd": None, "code": "NON_OBJECT"}
    allowed = {
        "schema",
        "ok",
        "cmd",
        "code",
        "build_commit",
        "persisted",
        "initiated",
        "scan_started",
        "setting_enabled",
        "build_enabled",
        "stack_active",
        "connected",
        "connecting",
        "scan_supported",
        "profile_saved",
        "password_saved",
        "password_printed",
        "public_rf_tx",
        "ssid",
        "state",
        "ip",
        "rssi_dbm",
        "channel",
        "reason",
        "total_count",
        "returned_count",
        "truncated",
        "boot_nonce",
        "uptime_ms",
        "heap_free",
        "internal_heap_free",
        "dma_heap_free",
        "psram_free",
        "current_task_stack_free_words",
        "ui_task_stack_free_words",
        "retained_task_stack_free_bytes",
        "nvs_ready",
        "nvs_error",
        "reset_reason",
        "board_ready",
        "ui_ready",
        "retry_task_stack_high_water_bytes",
        "safe_mode",
        "crash_loop_detected",
        "boot_guard_ready",
        "boot_guard_recovered",
        "consecutive_crash_boots",
        "retry_scheduled",
        "retry_attempt",
        "retry_delay_ms",
        "user_cancelled",
        "last_disconnect_reason",
        "last_failure_class",
        "last_active_subsystem",
        "boot_guard_error",
        "last_error",
        "policy",
        "live_network",
    }
    summary = {key: result[key] for key in allowed if key in result}
    if command == "wifi scan" and isinstance(result.get("networks"), list):
        summary["networks"] = [
            {
                key: network[key]
                for key in ("ssid", "rssi_dbm", "channel", "auth")
                if key in network
            }
            for network in result["networks"]
            if isinstance(network, dict)
        ][:WIFI_SCAN_MAX]
    return summary


class CommandRecorder:
    def __init__(self, sender: Callable[[str, float], dict], timeout: float):
        self.sender = sender
        self.timeout = timeout
        self.commands: list[str] = []
        self.steps: list[dict] = []

    def issue(self, command: str, label: str, *, timeout: float | None = None) -> dict:
        enforce_safe_command(command)
        self.commands.append(command)
        try:
            result = self.sender(
                command, timeout if timeout is not None else self.timeout
            )
        except Exception as exc:  # pragma: no cover - hardware exception type varies
            result = {
                "schema": 1,
                "ok": False,
                "cmd": command,
                "code": "HOST_EXCEPTION",
                "exception_type": type(exc).__name__,
            }
        self.steps.append(
            {
                "index": len(self.steps) + 1,
                "label": label,
                "command": command,
                "response": _summarize_result(command, result),
            }
        )
        return result


def _poll_status(
    recorder: CommandRecorder,
    *,
    target_ssid: str,
    connected: bool,
    wait_sec: float,
    poll_sec: float,
    label: str,
    sleep: Callable[[float], None],
) -> dict:
    attempts = max(1, int(math.ceil(wait_sec / poll_sec)) + 1)
    last: dict | None = None
    for attempt in range(attempts):
        last = wifi_status_snapshot(
            recorder.issue("wifi status", f"{label}-poll-{attempt + 1}")
        )
        _require_saved_target(last, target_ssid)
        if connected and connected_status_ok(last, target_ssid):
            return last
        if not connected and disabled_status_ok(last, target_ssid):
            return last
        if last["safe_mode"] or last["crash_loop_detected"]:
            raise AcceptanceFailure(
                "wifi_safe_mode", "Wi-Fi entered safe mode during acceptance"
            )
        if attempt + 1 < attempts:
            sleep(poll_sec)
    desired = "connected" if connected else "disabled"
    raise AcceptanceFailure(
        "wifi_state_timeout",
        f"Wi-Fi did not become {desired} within the bounded poll window",
    )


def _connect_result(result: object, target_ssid: str) -> None:
    row = _require_command_result(result, "wifi connect")
    if (
        row.get("initiated") is not True
        or row.get("setting_enabled") is not True
        or row.get("profile_saved") is not True
        or row.get("ssid") != target_ssid
        or row.get("password_printed") is not False
        or row.get("public_rf_tx") is not False
    ):
        raise AcceptanceFailure(
            "wifi_connect_invalid", "wifi connect response is incomplete"
        )


def _off_result(result: object) -> None:
    row = _require_command_result(result, "wifi off")
    if row.get("persisted") is not True or row.get("setting_enabled") is not False:
        raise AcceptanceFailure(
            "wifi_off_invalid", "wifi off did not persist disabled intent"
        )


def _health_summary(samples: list[dict]) -> dict:
    if not samples:
        return {"ok": False, "sample_count": 0}
    first = samples[0]
    loss_limits = {
        "heap_free": MAX_HEAP_LOSS_BYTES,
        "internal_heap_free": MAX_INTERNAL_HEAP_LOSS_BYTES,
        "dma_heap_free": MAX_DMA_HEAP_LOSS_BYTES,
        "psram_free": MAX_PSRAM_LOSS_BYTES,
    }
    losses = {
        field: max(max(0, first[field] - sample[field]) for sample in samples)
        for field in loss_limits
    }
    nonces = {sample["boot_nonce"] for sample in samples}
    return {
        "ok": len(nonces) == 1
        and all(losses[field] <= limit for field, limit in loss_limits.items()),
        "sample_count": len(samples),
        "boot_nonce": first["boot_nonce"] if len(nonces) == 1 else None,
        "boot_nonce_stable": len(nonces) == 1,
        "heap_loss_bytes": losses,
        "heap_loss_limits_bytes": loss_limits,
        "current_task_stack_free_words_floor": min(
            sample["current_task_stack_free_words"] for sample in samples
        ),
        "ui_task_stack_free_words_floor": min(
            sample["ui_task_stack_free_words"] for sample in samples
        ),
        "retained_task_stack_free_bytes_floor": min(
            sample["retained_task_stack_free_bytes"] for sample in samples
        ),
    }


def _base_report(
    args: argparse.Namespace, mode: str, expected_commit: str | None
) -> dict:
    return {
        "schema": 1,
        "kind": "wifi_saved_profile_resilience",
        "claim_scope": FEATURE_CLAIM_SCOPE,
        "full_wp13_matrix_covered": False,
        "uncovered_release_scenarios": list(FULL_WP13_UNCOVERED_SCENARIOS),
        "mode": mode,
        "port": normalize_port(args.port),
        "expected_firmware_commit": expected_commit,
        "target_ssid": args.target_ssid,
        "cycles_requested": args.cycles,
        "cycles_completed": 0,
        "started_at": utc_now(),
        "ended_at": None,
        "classification": "not_run",
        "ok": False,
        "acceptance_passed": False,
        "feature_evidence_eligible": False,
        "release_gate_eligible": False,
        "truth": {
            "physical_observed": False,
            "simulated": mode == "simulation",
            "dry_run": mode == "dry-run",
            "source_inspection": False,
            "exact_commit_verified": False,
        },
        "physical_observed": False,
        "simulated": mode == "simulation",
        "dry_run": mode == "dry-run",
        "hardware_required": True,
        "hardware_transport": None,
        "d1l_target": None,
        "d1l_target_after": None,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "rp2040_accessed": False,
        "commands_redacted": True,
        "commands": [],
        "steps": [],
        "credential_handling": {
            "saved_profile_required": True,
            "password_argument_supported": False,
            "credential_supplied_to_harness": False,
            "credential_logged": False,
            "credential_stored": False,
        },
        "profile_mutation": {
            "write_commands_allowed": False,
            "profile_written": False,
            "profile_cleared": False,
        },
        "checks": {},
    }


def _planned_commands() -> list[dict]:
    return [
        {"command": "version", "purpose": "exact firmware identity"},
        {"command": "health", "purpose": "boot nonce, heap, and stack boundary"},
        {"command": "wifi status", "purpose": "saved profile and runtime snapshot"},
        {
            "command": "wifi on",
            "purpose": "conditional enable and saved-profile autoconnect",
        },
        {
            "command": "wifi scan",
            "purpose": "bounded nearby network inventory using networks",
        },
        {"command": "wifi connect", "purpose": "explicit saved-profile reconnect"},
        {"command": "wifi off", "purpose": "disconnect and disabled-state proof"},
        {
            "command": "wifi on",
            "purpose": "conditional restoration of original enabled intent",
        },
    ]


def _validate_args(args: argparse.Namespace, mode: str) -> tuple[str, str | None]:
    port = enforce_port_guard(args.port)
    if args.baud != D1L_CONSOLE_BAUD:
        raise ValueError(f"--baud must be {D1L_CONSOLE_BAUD}")
    if not isinstance(args.target_ssid, str) or not args.target_ssid:
        raise ValueError("--target-ssid or D1L_WIFI_TARGET_SSID is required")
    try:
        encoded_ssid = args.target_ssid.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError("target SSID must be valid UTF-8") from exc
    if len(encoded_ssid) > 32 or any(
        ord(char) < 0x20 or ord(char) == 0x7F for char in args.target_ssid
    ):
        raise ValueError("target SSID must be 1-32 bytes without control characters")
    if not 1 <= args.cycles <= 100:
        raise ValueError("--cycles must be between 1 and 100")
    if (
        args.timeout <= 0
        or args.scan_timeout <= 0
        or args.wait_sec <= 0
        or args.poll_sec <= 0
    ):
        raise ValueError("timeouts and polling intervals must be positive")
    if args.open_settle_sec < 0:
        raise ValueError("--open-settle-sec must be non-negative")
    expected = exact_commit(args.expected_firmware_commit)
    if args.expected_firmware_commit is not None and expected is None:
        raise ValueError("--expected-firmware-commit must be an exact 40-character SHA")
    if mode != "dry-run" and expected is None:
        raise ValueError(
            "exact --expected-firmware-commit is required before hardware or simulation execution"
        )
    return port, expected


def run_acceptance(
    args: argparse.Namespace,
    *,
    command_sender: Callable[[str, float], dict] | None = None,
    serial_module=None,
    sleep: Callable[[float], None] = time.sleep,
    port_lister: Callable[[], object] | None = None,
    platform_name: str | None = None,
) -> dict:
    mode = (
        "dry-run"
        if args.dry_run
        else ("simulation" if command_sender is not None else "hardware")
    )
    port, expected = _validate_args(args, mode)
    report = _base_report(args, mode, expected)
    report["port"] = port
    if mode == "dry-run":
        report["planned_commands"] = _planned_commands()
        report["classification"] = "dry_run_only"
        report["ok"] = True
        report["checks"] = {
            "plan_safe": True,
            "physical_evidence_present": False,
            "saved_profile_required": True,
        }
        report["ended_at"] = utc_now()
        return report

    hardware_transport: dict | None = None
    d1l_target: dict | None = None
    d1l_target_after: dict | None = None
    resolved_port_lister: Callable[[], object] | None = port_lister
    if command_sender is None:
        if serial_module is None:
            try:
                import serial as serial_module
            except ImportError as exc:  # pragma: no cover - dependency dependent
                report["classification"] = "serial_dependency_missing"
                report["failure"] = {
                    "code": "serial_dependency_missing",
                    "message": type(exc).__name__,
                }
                report["ended_at"] = utc_now()
                return report
        if resolved_port_lister is None:
            try:
                from serial.tools import list_ports
            except ImportError as exc:  # pragma: no cover - dependency dependent
                report["classification"] = "serial_target_dependency_missing"
                report["failure"] = {
                    "code": "serial_target_dependency_missing",
                    "message": type(exc).__name__,
                }
                report["ended_at"] = utc_now()
                return report

            resolved_port_lister = lambda: list_ports.comports(include_links=True)
        try:
            d1l_target = resolve_core_target(
                port,
                port_lister=resolved_port_lister,
                platform_name=platform_name,
            )
            port = d1l_target["requested_path"]
            report["port"] = port
            report["d1l_target"] = d1l_target
        except (OSError, ValueError) as exc:
            report["classification"] = "serial_target_rejected"
            report["failure"] = {
                "code": "serial_target_rejected",
                "message": type(exc).__name__,
            }
            report["ended_at"] = utc_now()
            return report
        serial_handle = None
        try:
            serial_handle = open_d1l_serial(
                serial_module,
                port=port,
                baudrate=args.baud,
                timeout=min(args.timeout, 1.0),
            )
            observed_port = normalize_port(
                str(getattr(serial_handle, "port", port) or port)
            )
            observed_baud = _integer(
                getattr(serial_handle, "baudrate", args.baud), minimum=1
            )
            if observed_port != port or observed_baud != args.baud:
                raise ValueError("serial transport identity does not match request")
            hardware_transport = {
                "opened_by_runner": True,
                "port": observed_port,
                "baud": observed_baud,
                "closed_by_runner": False,
            }
            if args.open_settle_sec:
                sleep(args.open_settle_sec)
            if hasattr(serial_handle, "reset_input_buffer"):
                serial_handle.reset_input_buffer()
        except Exception as exc:  # pragma: no cover - hardware exception type varies
            if serial_handle is not None:
                try:
                    serial_handle.close()
                except Exception:
                    pass
            report["classification"] = "serial_open_failed"
            report["failure"] = {
                "code": "serial_open_failed",
                "message": type(exc).__name__,
            }
            report["ended_at"] = utc_now()
            return report

        def serial_sender(command: str, timeout: float) -> dict:
            return send_console_command(serial_handle, command, timeout)

        command_sender = serial_sender
    else:
        serial_handle = None

    recorder = CommandRecorder(command_sender, args.timeout)
    health_samples: list[dict] = []
    cycles: list[dict] = []
    original_enabled: bool | None = None
    profile_before: dict | None = None
    profile_after: dict | None = None
    mutation_attempted = False
    failure: AcceptanceFailure | None = None
    cleanup_ok = False
    exact_verified = False

    def record_health(label: str) -> dict:
        sample = health_snapshot(recorder.issue("health", label))
        if health_samples and sample["boot_nonce"] != health_samples[0]["boot_nonce"]:
            raise AcceptanceFailure(
                "boot_nonce_changed",
                "unexpected reboot occurred during Wi-Fi acceptance",
            )
        health_samples.append(sample)
        return sample

    def toggle_on(label: str) -> dict:
        nonlocal mutation_attempted
        mutation_attempted = True
        status = wifi_status_snapshot(recorder.issue("wifi on", label), "wifi on")
        _require_saved_target(status, args.target_ssid)
        if status["setting_enabled"] is not True or status["build_enabled"] is not True:
            raise AcceptanceFailure(
                "wifi_on_invalid", "wifi on did not enable the runtime intent"
            )
        return status

    def toggle_off(label: str) -> None:
        nonlocal mutation_attempted
        mutation_attempted = True
        _off_result(recorder.issue("wifi off", label))

    try:
        version = _require_command_result(
            recorder.issue("version", "preflight-version"), "version"
        )
        exact_verified = exact_commit(version.get("build_commit")) == expected
        if not exact_verified:
            raise AcceptanceFailure(
                "firmware_identity_mismatch",
                "device build commit does not match expected commit",
            )
        record_health("preflight-health")
        initial = wifi_status_snapshot(
            recorder.issue("wifi status", "preflight-wifi-status")
        )
        original_enabled = initial["setting_enabled"]
        profile_before = profile_metadata(initial)
        _require_saved_target(initial, args.target_ssid)
        if (
            initial["build_enabled"] is not True
            or initial["scan_supported"] is not True
        ):
            raise AcceptanceFailure(
                "wifi_runtime_unavailable", "Wi-Fi build or scan support is unavailable"
            )

        for cycle_number in range(1, args.cycles + 1):
            cycle: dict = {"cycle": cycle_number, "ok": False}
            current = wifi_status_snapshot(
                recorder.issue("wifi status", f"cycle-{cycle_number}-start")
            )
            _require_saved_target(current, args.target_ssid)
            if not current["setting_enabled"]:
                toggle_on(f"cycle-{cycle_number}-enable")
                current = _poll_status(
                    recorder,
                    target_ssid=args.target_ssid,
                    connected=True,
                    wait_sec=args.wait_sec,
                    poll_sec=args.poll_sec,
                    label=f"cycle-{cycle_number}-enable",
                    sleep=sleep,
                )
            elif not connected_status_ok(current, args.target_ssid):
                _connect_result(
                    recorder.issue("wifi connect", f"cycle-{cycle_number}-preconnect"),
                    args.target_ssid,
                )
                current = _poll_status(
                    recorder,
                    target_ssid=args.target_ssid,
                    connected=True,
                    wait_sec=args.wait_sec,
                    poll_sec=args.poll_sec,
                    label=f"cycle-{cycle_number}-preconnect",
                    sleep=sleep,
                )

            scan = scan_snapshot(
                recorder.issue(
                    "wifi scan",
                    f"cycle-{cycle_number}-scan",
                    timeout=args.scan_timeout,
                ),
                args.target_ssid,
            )
            _connect_result(
                recorder.issue("wifi connect", f"cycle-{cycle_number}-reconnect"),
                args.target_ssid,
            )
            connected = _poll_status(
                recorder,
                target_ssid=args.target_ssid,
                connected=True,
                wait_sec=args.wait_sec,
                poll_sec=args.poll_sec,
                label=f"cycle-{cycle_number}-reconnect",
                sleep=sleep,
            )
            record_health(f"cycle-{cycle_number}-connected-health")
            toggle_off(f"cycle-{cycle_number}-disable")
            disabled = _poll_status(
                recorder,
                target_ssid=args.target_ssid,
                connected=False,
                wait_sec=args.wait_sec,
                poll_sec=args.poll_sec,
                label=f"cycle-{cycle_number}-disable",
                sleep=sleep,
            )
            record_health(f"cycle-{cycle_number}-disabled-health")
            restored = disabled
            if original_enabled:
                toggle_on(f"cycle-{cycle_number}-restore-enable")
                restored = _poll_status(
                    recorder,
                    target_ssid=args.target_ssid,
                    connected=True,
                    wait_sec=args.wait_sec,
                    poll_sec=args.poll_sec,
                    label=f"cycle-{cycle_number}-restore-enable",
                    sleep=sleep,
                )
            record_health(f"cycle-{cycle_number}-restored-health")
            cycle.update(
                {
                    "ok": True,
                    "scan": scan,
                    "connected": connected,
                    "disabled": disabled,
                    "restored": restored,
                }
            )
            cycles.append(cycle)
            report["cycles_completed"] = cycle_number
    except AcceptanceFailure as exc:
        failure = exc
    finally:
        if original_enabled is not None:
            try:
                final_status = wifi_status_snapshot(
                    recorder.issue("wifi status", "cleanup-status")
                )
                _require_saved_target(final_status, args.target_ssid)
                if final_status["setting_enabled"] != original_enabled:
                    if original_enabled:
                        toggle_on("cleanup-restore-enable")
                    else:
                        toggle_off("cleanup-restore-disable")
                final_status = _poll_status(
                    recorder,
                    target_ssid=args.target_ssid,
                    connected=original_enabled,
                    wait_sec=args.wait_sec,
                    poll_sec=args.poll_sec,
                    label="cleanup-final",
                    sleep=sleep,
                )
                profile_after = profile_metadata(final_status)
                record_health("cleanup-final-health")
                cleanup_ok = (
                    final_status["setting_enabled"] == original_enabled
                    and profile_before == profile_after
                )
            except AcceptanceFailure as cleanup_exc:
                cleanup_ok = False
                report["cleanup_failure"] = {
                    "code": cleanup_exc.code,
                    "message": str(cleanup_exc),
                }
        if serial_handle is not None:
            try:
                serial_handle.close()
                if hardware_transport is not None:
                    hardware_transport["closed_by_runner"] = True
            except Exception:
                pass
        if d1l_target is not None and resolved_port_lister is not None:
            try:
                d1l_target_after = resolve_core_target(
                    port,
                    port_lister=resolved_port_lister,
                    platform_name=platform_name,
                )
                if (
                    d1l_target_after["stable_identity_sha256"]
                    != d1l_target["stable_identity_sha256"]
                ):
                    raise ValueError(
                        "D1L serial target identity changed during acceptance"
                    )
            except (OSError, ValueError) as target_exc:
                if failure is None:
                    failure = AcceptanceFailure(
                        "serial_target_changed",
                        "D1L serial target identity did not remain stable",
                    )
                report["target_failure"] = {
                    "code": "serial_target_changed",
                    "message": type(target_exc).__name__,
                }

    health = _health_summary(health_samples)
    if failure is None and not health["ok"]:
        failure = AcceptanceFailure(
            "health_regression", "heap or boot-nonce boundary failed"
        )
    if failure is None and not cleanup_ok:
        failure = AcceptanceFailure(
            "restore_failed",
            "original Wi-Fi enable intent or profile metadata was not restored",
        )

    report.update(
        {
            "commands": recorder.commands,
            "steps": recorder.steps,
            "cycles": cycles,
            "profile_before": profile_before,
            "profile_after": profile_after,
            "original_enable_intent": original_enabled,
            "profile_preserved": profile_before is not None
            and profile_before == profile_after,
            "enable_intent_restored": cleanup_ok,
            "mutation_attempted": mutation_attempted,
            "health": health,
            "truth": {
                "physical_observed": mode == "hardware" and bool(recorder.commands),
                "simulated": mode == "simulation",
                "dry_run": False,
                "source_inspection": False,
                "exact_commit_verified": exact_verified,
            },
            "physical_observed": mode == "hardware" and bool(recorder.commands),
            "simulated": mode == "simulation",
            "dry_run": False,
            "hardware_transport": hardware_transport,
            "d1l_target": d1l_target,
            "d1l_target_after": d1l_target_after,
        }
    )
    report["checks"] = {
        "exact_commit_verified": exact_verified,
        "saved_profile_precondition": profile_before is not None,
        "target_ssid_exact_match": profile_before is not None
        and profile_before.get("ssid") == args.target_ssid,
        "cycles_complete": report["cycles_completed"] == args.cycles,
        "networks_schema_consumed": bool(cycles)
        and all("networks" in cycle["scan"] for cycle in cycles),
        "bounded_scan_target_seen": bool(cycles)
        and all(cycle["scan"]["target_matches"] for cycle in cycles),
        "boot_nonce_stable": health.get("boot_nonce_stable") is True,
        "health_and_stack_ok": health.get("ok") is True,
        "profile_preserved": report["profile_preserved"],
        "enable_intent_restored": report["enable_intent_restored"],
        "no_profile_write_or_clear": True,
        "no_rf_or_sd_format": True,
    }
    if failure is None:
        report["classification"] = "passed"
        report["ok"] = True
    else:
        report["classification"] = failure.code
        report["failure"] = {"code": failure.code, "message": str(failure)}
    report["acceptance_passed"] = mode == "hardware" and report["ok"] is True
    # This runner intentionally proves only the bounded saved-profile
    # scan/connect/toggle slice. The full WP-13 release matrix has additional
    # credential, coexistence, fault-injection, reboot, Map, and exact-IDF
    # requirements, so this artifact can never close the release gate.
    report["feature_evidence_eligible"] = False
    report["release_gate_eligible"] = False
    report["ended_at"] = utc_now()
    return report


def _recorded_feature_evidence_valid(report: dict, expected_commit: str) -> bool:
    """Re-derive the bounded feature result from recorded device responses."""
    steps = report.get("steps")
    commands = report.get("commands")
    target_ssid = report.get("target_ssid")
    cycles = report.get("cycles")
    cycles_requested = _integer(report.get("cycles_requested"), minimum=1)
    if (
        not isinstance(steps, list)
        or not steps
        or not isinstance(commands, list)
        or not isinstance(target_ssid, str)
        or not target_ssid
        or not isinstance(cycles, list)
        or cycles_requested is None
        or cycles_requested > 100
        or len(cycles) != cycles_requested
        or report.get("cycles_completed") != cycles_requested
    ):
        return False

    parsed: dict[str, object] = {}
    label_metadata: dict[str, tuple[int, str]] = {}
    derived_commands: list[str] = []
    health_samples: list[dict] = []
    try:
        for expected_index, step in enumerate(steps, start=1):
            if (
                not isinstance(step, dict)
                or step.get("index") != expected_index
                or not isinstance(step.get("label"), str)
                or not step["label"]
                or step["label"] in parsed
                or not isinstance(step.get("command"), str)
            ):
                return False
            label = step["label"]
            command = step["command"]
            enforce_safe_command(command)
            response = _require_command_result(step.get("response"), command)
            label_metadata[label] = (expected_index, command)
            derived_commands.append(command)
            if command == "version":
                parsed[label] = response
            elif command == "health":
                sample = health_snapshot(response)
                health_samples.append(sample)
                parsed[label] = sample
            elif command in {"wifi status", "wifi on"}:
                status = wifi_status_snapshot(response, command)
                if command == "wifi on":
                    _require_saved_target(status, target_ssid)
                    if (
                        status["setting_enabled"] is not True
                        or status["build_enabled"] is not True
                    ):
                        return False
                parsed[label] = status
            elif command == "wifi scan":
                parsed[label] = scan_snapshot(response, target_ssid)
            elif command == "wifi connect":
                _connect_result(response, target_ssid)
                parsed[label] = response
            elif command == "wifi off":
                _off_result(response)
                parsed[label] = response
            else:  # pragma: no cover - SAFE_COMMANDS is exhaustive above
                return False
    except (AcceptanceFailure, KeyError, TypeError, ValueError):
        return False

    if commands != derived_commands:
        return False
    version = parsed.get("preflight-version")
    initial = parsed.get("preflight-wifi-status")
    if (
        not isinstance(version, dict)
        or exact_commit(version.get("build_commit")) != expected_commit
        or not isinstance(initial, dict)
    ):
        return False
    try:
        _require_saved_target(initial, target_ssid)
    except AcceptanceFailure:
        return False
    profile_before = profile_metadata(initial)
    original_enabled = initial["setting_enabled"]

    def last_status(prefix: str) -> dict | None:
        matches = [
            (step["index"], parsed[step["label"]])
            for step in steps
            if isinstance(step, dict)
            and isinstance(step.get("label"), str)
            and step["label"].startswith(prefix)
            and isinstance(parsed.get(step["label"]), dict)
        ]
        if not matches:
            return None
        return max(matches, key=lambda item: item[0])[1]

    derived_cycles: list[dict] = []
    for cycle_number in range(1, cycles_requested + 1):
        start_label = f"cycle-{cycle_number}-start"
        scan_label = f"cycle-{cycle_number}-scan"
        reconnect_label = f"cycle-{cycle_number}-reconnect"
        connected_health_label = f"cycle-{cycle_number}-connected-health"
        disable_label = f"cycle-{cycle_number}-disable"
        disabled_health_label = f"cycle-{cycle_number}-disabled-health"
        restored_health_label = f"cycle-{cycle_number}-restored-health"
        toggle_label = (
            f"cycle-{cycle_number}-restore-enable"
            if original_enabled
            else f"cycle-{cycle_number}-enable"
        )
        required_actions = {
            start_label: "wifi status",
            toggle_label: "wifi on",
            scan_label: "wifi scan",
            reconnect_label: "wifi connect",
            connected_health_label: "health",
            disable_label: "wifi off",
            disabled_health_label: "health",
            restored_health_label: "health",
        }
        if any(
            label_metadata.get(label, (None, None))[1] != command
            for label, command in required_actions.items()
        ):
            return False
        ordered_labels = (
            [
                start_label,
                toggle_label,
                scan_label,
                reconnect_label,
                connected_health_label,
                disable_label,
                disabled_health_label,
                restored_health_label,
            ]
            if not original_enabled
            else [
                start_label,
                scan_label,
                reconnect_label,
                connected_health_label,
                disable_label,
                disabled_health_label,
                toggle_label,
                restored_health_label,
            ]
        )
        action_indices = [label_metadata[label][0] for label in ordered_labels]
        if action_indices != sorted(action_indices) or len(set(action_indices)) != len(
            action_indices
        ):
            return False
        cycle = cycles[cycle_number - 1]
        scan = parsed.get(scan_label)
        connected = last_status(f"cycle-{cycle_number}-reconnect-poll-")
        disabled = last_status(f"cycle-{cycle_number}-disable-poll-")
        restored = (
            last_status(f"cycle-{cycle_number}-restore-enable-poll-")
            if original_enabled
            else disabled
        )
        if (
            not isinstance(cycle, dict)
            or cycle.get("cycle") != cycle_number
            or cycle.get("ok") is not True
            or not isinstance(scan, dict)
            or not isinstance(connected, dict)
            or not isinstance(disabled, dict)
            or not isinstance(restored, dict)
        ):
            return False
        try:
            if not connected_status_ok(connected, target_ssid):
                return False
            if not disabled_status_ok(disabled, target_ssid):
                return False
            if original_enabled:
                if not connected_status_ok(restored, target_ssid):
                    return False
            elif not disabled_status_ok(restored, target_ssid):
                return False
        except AcceptanceFailure:
            return False
        derived_cycle = {
            "cycle": cycle_number,
            "ok": True,
            "scan": scan,
            "connected": connected,
            "disabled": disabled,
            "restored": restored,
        }
        if cycle != derived_cycle:
            return False
        derived_cycles.append(derived_cycle)

    final_status = last_status("cleanup-final-poll-")
    if not isinstance(final_status, dict):
        return False
    try:
        _require_saved_target(final_status, target_ssid)
    except AcceptanceFailure:
        return False
    profile_after = profile_metadata(final_status)
    derived_health = _health_summary(health_samples)
    profile_preserved = profile_before == profile_after
    enable_intent_restored = (
        final_status.get("setting_enabled") == original_enabled
    )
    derived_checks = {
        "exact_commit_verified": True,
        "saved_profile_precondition": True,
        "target_ssid_exact_match": profile_before.get("ssid") == target_ssid,
        "cycles_complete": len(derived_cycles) == cycles_requested,
        "networks_schema_consumed": all(
            "networks" in cycle["scan"] for cycle in derived_cycles
        ),
        "bounded_scan_target_seen": all(
            bool(cycle["scan"].get("target_matches"))
            for cycle in derived_cycles
        ),
        "boot_nonce_stable": derived_health.get("boot_nonce_stable") is True,
        "health_and_stack_ok": derived_health.get("ok") is True,
        "profile_preserved": profile_preserved,
        "enable_intent_restored": enable_intent_restored,
        "no_profile_write_or_clear": True,
        "no_rf_or_sd_format": True,
    }
    return (
        report.get("classification") == "passed"
        and report.get("profile_before") == profile_before
        and report.get("profile_after") == profile_after
        and report.get("original_enable_intent") == original_enabled
        and report.get("profile_preserved") is profile_preserved
        and report.get("enable_intent_restored") is enable_intent_restored
        and report.get("health") == derived_health
        and report.get("checks") == derived_checks
    )


def validate_completed_report(report: object) -> bool:
    if not isinstance(report, dict):
        return False
    truth = report.get("truth")
    mutation = report.get("profile_mutation")
    commands = report.get("commands")
    checks = report.get("checks")
    credentials = report.get("credential_handling")
    health = report.get("health")
    git = report.get("git")
    transport = report.get("hardware_transport")
    expected_commit = exact_commit(report.get("expected_firmware_commit"))
    try:
        port = enforce_port_guard(report.get("port"))
        target_before = report.get("d1l_target")
        target_after = report.get("d1l_target_after")
        validate_snapshot(target_before, port)
        validate_snapshot(target_after, port)
        target_stable = (
            target_before["stable_identity_sha256"]
            == target_after["stable_identity_sha256"]
        )
    except (KeyError, TypeError, ValueError):
        return False
    return (
        report.get("schema") == 1
        and report.get("kind") == "wifi_saved_profile_resilience"
        and report.get("claim_scope") == FEATURE_CLAIM_SCOPE
        and report.get("full_wp13_matrix_covered") is False
        and report.get("uncovered_release_scenarios")
        == list(FULL_WP13_UNCOVERED_SCENARIOS)
        and report.get("mode") == "hardware"
        and report.get("ok") is True
        and report.get("acceptance_passed") is True
        and report.get("release_gate_eligible") is False
        and report.get("port") == port
        and target_stable
        and expected_commit is not None
        and exact_commit(report.get("commit")) == expected_commit
        and transport
        == {
            "opened_by_runner": True,
            "port": port,
            "baud": D1L_CONSOLE_BAUD,
            "closed_by_runner": True,
        }
        and isinstance(git, dict)
        and git.get("dirty") is False
        and isinstance(truth, dict)
        and truth.get("physical_observed") is True
        and truth.get("simulated") is False
        and truth.get("dry_run") is False
        and truth.get("exact_commit_verified") is True
        and report.get("public_rf_tx") is False
        and report.get("dm_rf_tx") is False
        and report.get("formats_sd") is False
        and report.get("rp2040_accessed") is False
        and report.get("commands_redacted") is True
        and isinstance(credentials, dict)
        and credentials.get("password_argument_supported") is False
        and credentials.get("credential_supplied_to_harness") is False
        and credentials.get("credential_logged") is False
        and credentials.get("credential_stored") is False
        and isinstance(mutation, dict)
        and mutation.get("write_commands_allowed") is False
        and mutation.get("profile_written") is False
        and mutation.get("profile_cleared") is False
        and isinstance(commands, list)
        and bool(commands)
        and all(command in SAFE_COMMANDS for command in commands)
        and report.get("profile_preserved") is True
        and report.get("enable_intent_restored") is True
        and _integer(report.get("cycles_requested"), minimum=1) is not None
        and report.get("cycles_completed") == report.get("cycles_requested")
        and isinstance(health, dict)
        and health.get("ok") is True
        and isinstance(checks, dict)
        and all(value is True for value in checks.values())
        and _recorded_feature_evidence_valid(report, expected_commit)
    )


def default_out_path(report: dict) -> Path:
    commit = exact_commit(report.get("expected_firmware_commit")) or "unknown"
    port = enforce_port_guard(str(report.get("port") or D1L_PORT))
    port_slug = safe_slug(port)
    port_label = D1L_PORT if port == D1L_PORT else port_slug
    filename = f"wifi_saved_profile_resilience_{commit}_{port_label}.json"
    if report.get("mode") == "hardware":
        return ROOT / "artifacts" / "hardware" / port_slug / filename
    return ROOT / "artifacts" / str(report.get("mode") or "dry-run") / filename


def write_report(report: dict, path: Path | None = None) -> Path:
    out = path or default_out_path(report)
    if not out.is_absolute():
        out = ROOT / out
    stamp_report(report, ROOT)
    report["feature_evidence_eligible"] = validate_completed_report(report)
    report["release_gate_eligible"] = False
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="ascii"
    )
    return out


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Saved-profile D1L Wi-Fi resilience acceptance; "
            "no password input is accepted."
        )
    )
    parser.add_argument("--port", required=True)
    parser.add_argument("--target-ssid", default=os.environ.get("D1L_WIFI_TARGET_SSID"))
    parser.add_argument("--expected-firmware-commit")
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--baud", type=int, default=D1L_CONSOLE_BAUD)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--scan-timeout", type=float, default=30.0)
    parser.add_argument("--wait-sec", type=float, default=45.0)
    parser.add_argument("--poll-sec", type=float, default=1.0)
    parser.add_argument("--open-settle-sec", type=float, default=0.25)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--out")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        report = run_acceptance(args)
    except ValueError as exc:
        report = _base_report(
            args,
            "dry-run" if args.dry_run else "hardware",
            exact_commit(args.expected_firmware_commit),
        )
        report["classification"] = "input_rejected"
        report["failure"] = {"code": "input_rejected", "message": str(exc)}
        report["ended_at"] = utc_now()
    written = write_report(report, Path(args.out) if args.out else None)
    print(
        json.dumps(
            {
                "schema": 1,
                "kind": report.get("kind"),
                "ok": report.get("ok") is True,
                "classification": report.get("classification"),
                "out": str(written),
            },
            sort_keys=True,
        )
    )
    return 0 if report.get("ok") is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
