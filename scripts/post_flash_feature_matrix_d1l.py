#!/usr/bin/env python3
"""Run the bounded D1L post-flash feature subset without flashing or soaking.

This is an orchestration receipt, not a release-closure shortcut.  It batches
the existing exact-commit smoke/persistence, saved-profile Wi-Fi, UI scroll,
and SD file-canary runners under one deadline.  Missing full-feature gates are
listed explicitly in the output.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

try:
    from artifact_metadata import git_metadata
    from smoke_d1l import exact_commit, open_d1l_serial, send_console_command
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import git_metadata
    from scripts.smoke_d1l import (
        exact_commit,
        open_d1l_serial,
        send_console_command,
    )


ROOT = Path(__file__).resolve().parents[1]
D1L_PORT_NUMBER = 12
D1L_PORT = f"COM{D1L_PORT_NUMBER}"
D1L_BAUD = 115200
FORBIDDEN_PORTS = {f"COM{number}" for number in (8, 11, 29)}
RUN_ID_RE = re.compile(r"[1-9][0-9]*")
COM_PORT_RE = re.compile(r"COM[1-9][0-9]*")
CREDENTIAL_ARGUMENT_RE = re.compile(
    r"--(?:wifi[-_])?(?:password|passwd|psk|secret|credential)(?:=|$)",
    re.IGNORECASE,
)
MAX_SESSION_TIMEOUT_SEC = 1800.0
CHILD_ENV_ALLOWLIST = frozenset(
    {
        "APPDATA",
        "COMSPEC",
        "LOCALAPPDATA",
        "PATH",
        "PATHEXT",
        "PROGRAMDATA",
        "PYTHONDONTWRITEBYTECODE",
        "PYTHONIOENCODING",
        "PYTHONUTF8",
        "SYSTEMDRIVE",
        "SYSTEMROOT",
        "TEMP",
        "TMP",
        "TMPDIR",
        "USERPROFILE",
        "WINDIR",
    }
)
UI_SCREENS = (
    "home",
    "public_messages",
    "dm_thread",
    "nodes",
    "packets",
    "settings",
    "storage",
    "storage_card",
    "storage_data",
    "wifi",
    "map",
    "map_options",
    "map_location",
    "map_cache",
)
UNCOVERED_FULL_FEATURE_GATES = (
    "meshcore_full_conformance_complete",
    "exact_actions_esp32_flash",
    "ui_corruption_probe",
    "ui_pixel_capture",
    "ui_compose_keyboard_capture",
    "outbound_dm_controlled_peer",
    "controlled_peer_rf_and_admin_acceptance",
    "ble_companion_transport_acceptance",
    "live_map_pan_zoom_cache_acceptance",
    "sd_official_seeed_smoke_passed",
    "sd_acceptance_matrix",
    "sd_raw_diag_complete",
    "sd_boot_retry_manager",
    "sd_retained_canary_passed",
    "sd_reboot_remount_passed",
    "wp01_exact_pair_storage_reboot",
    "sd_map_tile_canary_passed",
    "sd_export_canary_passed",
    "sd_diagnostic_export_passed",
    "sd_data_export_passed",
    "sd_fat32_only_enforced",
    "sd_power_rail_measured",
    "sd_32gb_max_matrix_passed",
    "manual_touch_and_photo_review",
    "signed_update_upgrade_rollback_and_powerloss",
    "full_duration_idle_soak",
    "full_duration_active_soak",
    "manual_physical_ui_review",
    "full_rf_dm_acceptance",
)
HARDWARE_EXECUTION_DISABLED = (
    "Post-flash feature-matrix hardware execution is disabled until the "
    "runner admits the exact full D1L public key and stable Pi/Windows target "
    "before every Wi-Fi or storage mutation. Use the narrow Core runners."
)
COVERED_FEATURE_SUBSETS = (
    "com12_smoke_and_settings_persistence",
    "saved_profile_wifi_resilience",
    "ui_scroll_read_only_surface_walk",
    "sd_file_canary",
)
WIFI_ALLOWED_COMMANDS = frozenset(
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
SD_FILE_CANARY_COMMANDS = (
    "storage status",
    "storage mount",
    "storage status",
    "storage filecanary",
    "storage status",
    "packets",
    "health",
)


@dataclass(frozen=True)
class StepSpec:
    name: str
    script: str
    timeout_sec: int


STEP_SPECS = (
    StepSpec("smoke_persistence", "smoke_d1l.py", 240),
    StepSpec("wifi_saved_profile", "wifi_resilience_d1l.py", 360),
    StepSpec("ui_scroll", "scroll_probe_d1l.py", 420),
    StepSpec("sd_file_canary", "sd_file_canary_d1l.py", 240),
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def normalize_port(value: object) -> str:
    return str(value or "").strip().upper()


def enforce_port(value: object) -> str:
    port = normalize_port(value)
    if not COM_PORT_RE.fullmatch(port):
        raise ValueError("an explicit Windows COM port is required")
    if port in FORBIDDEN_PORTS:
        raise ValueError(f"refusing forbidden port {port}")
    if port != D1L_PORT:
        raise ValueError(f"post-flash D1L feature validation is pinned to {D1L_PORT}")
    return port


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def sanitized_child_environment(
    environment: dict[str, str],
    *,
    target_ssid: str,
) -> dict[str, str]:
    """Return the minimum non-secret environment needed by child runners."""
    child = {
        key: value
        for key, value in environment.items()
        if key.upper() in CHILD_ENV_ALLOWLIST
    }
    child["D1L_WIFI_TARGET_SSID"] = target_ssid
    return child


def _unique_json(root: Path, pattern: str, label: str) -> tuple[Path, dict[str, Any]]:
    paths = sorted(path for path in root.rglob(pattern) if path.is_file())
    if len(paths) != 1:
        raise ValueError(
            f"{label} must resolve to exactly one JSON file under --github-run-dir"
        )
    data = read_json(paths[0])
    if not data:
        raise ValueError(f"{label} is missing or invalid JSON")
    return paths[0], data


def validate_actions_context(
    run_dir: Path,
    *,
    expected_commit: str,
    run_id: str,
) -> dict[str, Any]:
    """Bind local Actions artifacts to one exact commit/run pair."""
    run_dir = run_dir.resolve()
    if not run_dir.is_dir():
        raise ValueError("--github-run-dir must be an existing directory")

    marker_path, marker = _unique_json(
        run_dir,
        "d1l_host_checks_success_*.json",
        "host-checks success marker",
    )
    manifest_path, manifest = _unique_json(
        run_dir,
        "d1l-release-*/manifest.json",
        "release manifest",
    )
    git = manifest.get("git") if isinstance(manifest.get("git"), dict) else {}
    workflow = (
        manifest.get("workflow")
        if isinstance(manifest.get("workflow"), dict)
        else {}
    )
    marker_attempt = str(marker.get("workflow_run_attempt") or "")
    manifest_attempt = str(workflow.get("run_attempt") or "")
    checks = {
        "host_marker_schema": marker.get("schema") == 1,
        "host_marker_type": marker.get("artifact_type")
        == "d1l_host_checks_success",
        "host_marker_passed": marker.get("status") == "pass"
        and marker.get("passed") is True
        and marker.get("all_prior_steps_completed") is True
        and marker.get("job") == "host-checks",
        "host_marker_commit": exact_commit(marker.get("repository_commit"))
        == expected_commit,
        "host_marker_run": str(marker.get("workflow_run_id")) == run_id,
        "release_manifest_schema": manifest.get("schema") == 1,
        "release_package": manifest.get("package")
        == f"d1l-release-{expected_commit}",
        "release_git_commit": exact_commit(git.get("commit")) == expected_commit,
        "release_git_clean": git.get("dirty") is False
        and git.get("dirty_entries") in (None, []),
        "release_workflow_repository": workflow.get("repository") == "n30nex/SIGUI",
        "release_workflow_name": workflow.get("workflow") == "d1l-ci",
        "release_workflow_commit": exact_commit(workflow.get("sha"))
        == expected_commit,
        "release_workflow_run": str(workflow.get("run_id")) == run_id,
        "release_workflow_attempt": RUN_ID_RE.fullmatch(marker_attempt) is not None
        and marker_attempt == manifest_attempt,
    }
    failures = sorted(name for name, passed in checks.items() if not passed)
    return {
        "ok": not failures,
        "github_run_dir": str(run_dir),
        "expected_firmware_commit": expected_commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": manifest_attempt or None,
        "host_marker": str(marker_path),
        "host_marker_sha256": sha256_file(marker_path),
        "release_manifest": str(manifest_path),
        "release_manifest_sha256": sha256_file(manifest_path),
        "checks": checks,
        "failures": failures,
    }


def validate_source_context(
    source: dict[str, Any],
    *,
    expected_commit: str,
) -> dict[str, Any]:
    checks = {
        "source_commit_exact": exact_commit(source.get("commit"))
        == expected_commit,
        "source_clean": source.get("dirty") is False
        and source.get("dirty_entries") in (None, []),
    }
    failures = sorted(name for name, passed in checks.items() if not passed)
    return {"ok": not failures, "checks": checks, "failures": failures}


def _walk(value: object):
    yield value
    if isinstance(value, dict):
        for key, child in value.items():
            yield key
            yield from _walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk(child)


def claimed_ports(value: object) -> set[str]:
    ports: set[str] = set()
    for item in _walk(value):
        if isinstance(item, str):
            ports.update(match.group(0).upper() for match in re.finditer(r"COM[1-9][0-9]*", item.upper()))
    return ports


def actual_commands(value: object) -> list[str]:
    commands: list[str] = []
    if isinstance(value, dict):
        command = value.get("command")
        cmd = value.get("cmd")
        if isinstance(command, str):
            commands.append(command)
        if isinstance(cmd, str):
            commands.append(cmd)
        for child in value.values():
            commands.extend(actual_commands(child))
    elif isinstance(value, list):
        for child in value:
            commands.extend(actual_commands(child))
    return commands


def safety_failures(value: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    ports = claimed_ports(value)
    if ports - {D1L_PORT}:
        failures.append("receipt_claims_non_d1l_or_forbidden_port")
    for item in _walk(value):
        if isinstance(item, dict):
            if item.get("formats_sd") is True or item.get("format_performed") is True:
                failures.append("receipt_reports_sd_format")
            if item.get("public_rf_tx") is True or item.get("dm_rf_tx") is True:
                failures.append("receipt_reports_rf_transmit")
            if item.get("password_printed") is True:
                failures.append("receipt_reports_password_output")
            for key in ("password", "wifi_password", "psk", "credential", "secret"):
                if key in item and item.get(key) not in (None, "", False):
                    failures.append("receipt_contains_credential_material")
        elif isinstance(item, str):
            lowered = item.strip().lower()
            if lowered.startswith("mesh send "):
                failures.append("receipt_contains_mesh_send")
            if (
                "soak_d1l.py" in lowered
                or lowered.startswith("storage diag raw")
                or lowered.startswith("sd diag raw")
            ):
                failures.append("receipt_contains_deferred_or_isolated_test")
            if "storage format" in lowered or "format-confirm" in lowered:
                failures.append("receipt_contains_format_command")
    for command in actual_commands(value):
        lowered = command.strip().lower()
        if "flash" in lowered:
            failures.append("receipt_contains_flash_command")
        if "soak" in lowered:
            failures.append("receipt_contains_soak_command")
        if lowered.startswith(("storage diag raw", "sd diag raw")):
            failures.append("receipt_contains_raw_diag_command")
        if lowered.startswith("mesh send "):
            failures.append("receipt_contains_mesh_send")
        if "storage format" in lowered or "format-confirm" in lowered:
            failures.append("receipt_contains_format_command")
    return sorted(set(failures))


def _common_receipt_failures(
    data: dict[str, Any],
    *,
    expected_commit: str,
    require_port: bool = True,
) -> list[str]:
    failures = safety_failures(data)
    if data.get("schema") != 1:
        failures.append("schema_invalid")
    if data.get("mode") != "hardware":
        failures.append("not_hardware")
    if data.get("ok") is not True:
        failures.append("child_not_ok")
    if exact_commit(data.get("commit")) != expected_commit:
        failures.append("source_commit_mismatch")
    git = data.get("git") if isinstance(data.get("git"), dict) else {}
    if exact_commit(git.get("commit")) != expected_commit:
        failures.append("source_git_commit_mismatch")
    if git.get("dirty") is not False or git.get("dirty_entries") not in (None, []):
        failures.append("source_git_dirty")
    if require_port and normalize_port(data.get("port")) != D1L_PORT:
        failures.append("port_mismatch")
    if require_port and data.get("baud") != D1L_BAUD:
        failures.append("baud_mismatch")
    return failures


def validate_child_receipt(
    name: str,
    data: dict[str, Any],
    *,
    expected_commit: str,
    screens: tuple[str, ...] = UI_SCREENS,
) -> dict[str, Any]:
    failures = _common_receipt_failures(
        data,
        expected_commit=expected_commit,
    )
    checks: dict[str, bool] = {}

    if name == "smoke_persistence":
        persistence = (
            data.get("persistence")
            if isinstance(data.get("persistence"), dict)
            else {}
        )
        checks = {
            "expected_commit": exact_commit(data.get("expected_firmware_commit"))
            == expected_commit,
            "device_commit": exact_commit(data.get("device_build_commit"))
            == expected_commit,
            "firmware_identity": data.get("firmware_identity_ok") is True,
            "persistence": persistence.get("schema") == 1
            and persistence.get("test") == "settings_persistence_reboot"
            and persistence.get("ok") is True
            and persistence.get("reboot_proven") is True
            and persistence.get("cleanup_reboot_proven") is True
            and persistence.get("cleanup_ok") is True,
        }
    elif name == "wifi_saved_profile":
        truth = data.get("truth") if isinstance(data.get("truth"), dict) else {}
        credentials = (
            data.get("credential_handling")
            if isinstance(data.get("credential_handling"), dict)
            else {}
        )
        profile = (
            data.get("profile_mutation")
            if isinstance(data.get("profile_mutation"), dict)
            else {}
        )
        transport = (
            data.get("hardware_transport")
            if isinstance(data.get("hardware_transport"), dict)
            else {}
        )
        commands = data.get("commands")
        child_checks = data.get("checks")
        checks = {
            "kind": data.get("kind") == "wifi_saved_profile_resilience",
            "expected_commit": exact_commit(data.get("expected_firmware_commit"))
            == expected_commit,
            "exact_device": truth.get("exact_commit_verified") is True,
            "physical": truth.get("physical_observed") is True
            and truth.get("simulated") is False
            and truth.get("dry_run") is False,
            "top_level_truth": data.get("physical_observed") is True
            and data.get("simulated") is False
            and data.get("dry_run") is False,
            "transport": normalize_port(transport.get("port")) == D1L_PORT
            and transport.get("baud") == D1L_BAUD
            and transport.get("opened_by_runner") is True
            and transport.get("closed_by_runner") is True,
            "feature_evidence": data.get("feature_evidence_eligible") is True
            and data.get("acceptance_passed") is True,
            "profile_preserved": data.get("profile_preserved") is True
            and data.get("enable_intent_restored") is True,
            "no_credentials": credentials.get("password_argument_supported") is False
            and credentials.get("credential_supplied_to_harness") is False
            and credentials.get("credential_logged") is False
            and credentials.get("credential_stored") is False,
            "no_profile_write": profile.get("write_commands_allowed") is False
            and profile.get("profile_written") is False
            and profile.get("profile_cleared") is False,
            "not_release_closure": data.get("release_gate_eligible") is False
            and data.get("full_wp13_matrix_covered") is False
            and isinstance(data.get("uncovered_release_scenarios"), list)
            and bool(data.get("uncovered_release_scenarios")),
            "command_allowlist": isinstance(commands, list)
            and bool(commands)
            and all(
                isinstance(command, str) and command in WIFI_ALLOWED_COMMANDS
                for command in commands
            ),
            "child_checks": isinstance(child_checks, dict)
            and bool(child_checks)
            and all(value is True for value in child_checks.values()),
            "no_rp2040": data.get("rp2040_accessed") is False,
            "commands_redacted": data.get("commands_redacted") is True,
        }
    elif name == "ui_scroll":
        network = (
            data.get("map_network_evidence")
            if isinstance(data.get("map_network_evidence"), dict)
            else {}
        )
        checks = {
            "screens": data.get("screens") == list(screens),
            "all_events": data.get("failure_count") == 0
            and data.get("failures") == [],
            "map_network_suppressed": network.get("measured") is True
            and network.get("unchanged") is True
            and data.get("network_tx") is False
            and data.get("map_network_requests") is False,
            "read_only": data.get("read_only") is True
            and data.get("manual_touch") is False
            and data.get("clear_crashlog_before_start") is False
            and data.get("save_actions") is False
            and data.get("clear_actions") is False
            and data.get("storage_mutation") is False
            and data.get("wifi_mutation") is False
            and data.get("rf_tx") is False
            and data.get("public_rf_tx") is False
            and data.get("dm_rf_tx") is False
            and data.get("formats_sd") is False,
        }
    elif name == "sd_file_canary":
        checks = {
            "sequence_complete": data.get("sequence_completed") is True
            and data.get("unexpected_console_restart") is False,
            "canary_passed": data.get("canary_passed") is True
            and data.get("canary_unavailable_ok") is False,
            "ready_before_after": data.get("storage_file_gate_ready_before") is True
            and data.get("storage_file_gate_ready_after") is True,
            "retained_sd_before_after": data.get("retained_history_sd_ready_before")
            is True
            and data.get("retained_history_sd_ready_after") is True,
            "command_allowlist": data.get("commands")
            == list(SD_FILE_CANARY_COMMANDS),
            "unavailable_not_allowed": data.get("allow_unavailable") is False,
            "no_format_or_rf": data.get("formats_sd") is False
            and data.get("public_rf_tx") is False,
        }
    else:
        failures.append("unknown_step")

    failures.extend(name for name, passed in checks.items() if not passed)
    return {
        "valid": not failures,
        "checks": checks,
        "failures": sorted(set(failures)),
    }


def identity_guard(
    *,
    port: str,
    baud: int,
    timeout: float,
    expected_commit: str,
    serial_module=None,
    deadline: float | None = None,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    if serial_module is None:
        try:
            import serial as serial_module
        except ImportError as exc:  # pragma: no cover - dependency dependent
            return {"ok": False, "error": f"serial_dependency_missing:{type(exc).__name__}"}

    handle = None
    transport = {
        "opened_by_runner": False,
        "closed_by_runner": False,
        "port": port,
        "baud": baud,
    }

    def bounded_timeout() -> float:
        if deadline is None:
            return timeout
        remaining = deadline - clock()
        if remaining <= 0:
            raise TimeoutError("absolute session deadline exhausted")
        return min(timeout, remaining)

    try:
        open_timeout = min(bounded_timeout(), 1.0)
        handle = open_d1l_serial(
            serial_module,
            port=port,
            baudrate=baud,
            timeout=open_timeout,
        )
        transport["opened_by_runner"] = True
        settle = min(0.25, bounded_timeout())
        sleep(settle)
        handle.reset_input_buffer()
        version = send_console_command(handle, "version", bounded_timeout())
        health = send_console_command(handle, "health", bounded_timeout())
    except TimeoutError:
        return {
            "ok": False,
            "error": "identity_guard_deadline_exhausted",
            "hardware_transport": transport,
        }
    except Exception as exc:  # pragma: no cover - hardware exception type varies
        return {
            "ok": False,
            "error": f"identity_guard_exception:{type(exc).__name__}",
            "hardware_transport": transport,
        }
    finally:
        if handle is not None:
            try:
                handle.close()
                transport["closed_by_runner"] = True
            except Exception:
                pass

    return {
        "schema": 1,
        "kind": "post_flash_identity_guard",
        "mode": "hardware",
        "port": port,
        "baud": baud,
        "expected_firmware_commit": expected_commit,
        "device_build_commit": version.get("build_commit"),
        "boot_nonce": health.get("boot_nonce"),
        "hardware_transport": transport,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "ok": (
            version.get("ok") is True
            and exact_commit(version.get("build_commit")) == expected_commit
            and health.get("ok") is True
            and health.get("board_ready") is True
            and health.get("ui_ready") is True
            and health.get("nvs_ready") is True
            and health.get("nvs_error") == "ESP_OK"
            and isinstance(health.get("boot_nonce"), int)
            and not isinstance(health.get("boot_nonce"), bool)
            and health.get("boot_nonce") != 0
            and transport["opened_by_runner"] is True
            and transport["closed_by_runner"] is True
        ),
    }


def validate_identity_receipt(
    data: dict[str, Any],
    *,
    expected_commit: str,
) -> dict[str, Any]:
    transport = (
        data.get("hardware_transport")
        if isinstance(data.get("hardware_transport"), dict)
        else {}
    )
    checks = {
        "schema": data.get("schema") == 1,
        "kind": data.get("kind") == "post_flash_identity_guard",
        "hardware": data.get("mode") == "hardware",
        "ok": data.get("ok") is True,
        "port": normalize_port(data.get("port")) == D1L_PORT,
        "baud": data.get("baud") == D1L_BAUD,
        "expected_commit": exact_commit(data.get("expected_firmware_commit"))
        == expected_commit,
        "device_commit": exact_commit(data.get("device_build_commit"))
        == expected_commit,
        "boot_nonce": isinstance(data.get("boot_nonce"), int)
        and not isinstance(data.get("boot_nonce"), bool)
        and data.get("boot_nonce") != 0,
        "transport": transport
        == {
            "opened_by_runner": True,
            "closed_by_runner": True,
            "port": D1L_PORT,
            "baud": D1L_BAUD,
        },
        "no_rf_or_format": data.get("public_rf_tx") is False
        and data.get("dm_rf_tx") is False
        and data.get("formats_sd") is False,
    }
    failures = safety_failures(data)
    failures.extend(name for name, passed in checks.items() if not passed)
    failures = sorted(set(failures))
    return {"valid": not failures, "checks": checks, "failures": failures}


def child_command(
    spec: StepSpec,
    *,
    root: Path,
    port: str,
    expected_commit: str,
    receipt: Path,
    wifi_cycles: int,
    screens: tuple[str, ...],
) -> list[str]:
    script = root / "scripts" / spec.script
    common = [
        sys.executable,
        str(script),
        "--port",
        port,
        "--baud",
        str(D1L_BAUD),
    ]
    if spec.name == "smoke_persistence":
        return [
            *common,
            "--timeout",
            "8",
            "--persistence-test",
            "--expected-firmware-commit",
            expected_commit,
            "--out",
            str(receipt),
        ]
    if spec.name == "wifi_saved_profile":
        return [
            *common,
            "--expected-firmware-commit",
            expected_commit,
            "--cycles",
            str(wifi_cycles),
            "--timeout",
            "8",
            "--scan-timeout",
            "30",
            "--wait-sec",
            "45",
            "--poll-sec",
            "1",
            "--out",
            str(receipt),
        ]
    if spec.name == "ui_scroll":
        return [
            *common,
            "--timeout",
            "15",
            "--screens",
            ",".join(screens),
            "--out",
            str(receipt),
        ]
    if spec.name == "sd_file_canary":
        return [
            *common,
            "--timeout",
            "8",
            "--mount-wait-sec",
            "30",
            "--out",
            str(receipt),
        ]
    raise ValueError(f"unknown step {spec.name}")


def dry_run_report(args: argparse.Namespace) -> dict[str, Any]:
    port = enforce_port(args.port)
    commit = exact_commit(args.expected_firmware_commit)
    if commit is None:
        raise ValueError("--expected-firmware-commit must be an exact 40-character SHA")
    if not RUN_ID_RE.fullmatch(str(args.github_actions_run)):
        raise ValueError("--github-actions-run must be a positive numeric run ID")
    if args.baud != D1L_BAUD:
        raise ValueError(f"--baud must be {D1L_BAUD}")
    if not 1 <= args.wifi_cycles <= 100:
        raise ValueError("--wifi-cycles must be between 1 and 100")
    if not 0 < args.session_timeout_sec <= MAX_SESSION_TIMEOUT_SEC:
        raise ValueError(
            f"--session-timeout-sec must be between 0 and {MAX_SESSION_TIMEOUT_SEC:g}"
        )
    plan = [
        {
            "name": spec.name,
            "script": f"scripts/{spec.script}",
            "timeout_sec": spec.timeout_sec,
            "argv": child_command(
                spec,
                root=ROOT,
                port=port,
                expected_commit=commit,
                receipt=Path("<preserved-child-receipt>"),
                wifi_cycles=args.wifi_cycles,
                screens=UI_SCREENS,
            )[2:],
        }
        for spec in STEP_SPECS
    ]
    source = git_metadata(ROOT)
    source_validation = validate_source_context(
        source,
        expected_commit=commit,
    )
    return {
        "schema": 1,
        "kind": "post_flash_feature_matrix",
        "claim_scope": "core_persistence_saved_profile_wifi_ui_scroll_sd_file_subset",
        "mode": "dry-run",
        "classification": "dry_run_plan_only",
        "port": port,
        "expected_firmware_commit": commit,
        "github_actions_run": str(args.github_actions_run),
        "commit": source.get("commit"),
        "git": source,
        "github_run_dir": str(args.github_run_dir)
        if args.github_run_dir
        else None,
        "actions_provenance_required_before_hardware": True,
        "source": source,
        "source_validation": source_validation,
        "session_timeout_sec": args.session_timeout_sec,
        "wifi_cycles": args.wifi_cycles,
        "ui_screens": list(UI_SCREENS),
        "plan": plan,
        "covered_feature_subsets": list(COVERED_FEATURE_SUBSETS),
        "hardware_required": True,
        "physical_observed": False,
        "simulated": False,
        "dry_run": True,
        "full_feature_matrix_covered": False,
        "uncovered_full_feature_gates": list(UNCOVERED_FULL_FEATURE_GATES),
        "feature_subset_complete": False,
        "release_gate_eligible": False,
        "soak_started": False,
        "soak_deferred_until_feature_tests_complete": True,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "credential_handling": {
            "password_argument_supported": False,
            "password_forwarded_to_children": False,
            "password_recorded": False,
            "saved_profile_target_from_environment": True,
        },
        "ok": True,
    }


def _receipt_name(spec: StepSpec, commit: str, run_id: str, port: str) -> str:
    return f"{spec.name}_{commit}_actions_{run_id}_{port}.json"


def run_matrix(
    args: argparse.Namespace,
    *,
    process_runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    identity_probe: Callable[..., dict[str, Any]] = identity_guard,
    actions_probe: Callable[..., dict[str, Any]] = validate_actions_context,
    source_probe: Callable[[Path], dict[str, Any]] = git_metadata,
    clock: Callable[[], float] = time.monotonic,
    environment: dict[str, str] | None = None,
) -> dict[str, Any]:
    if args.dry_run:
        return dry_run_report(args)

    port = enforce_port(args.port)
    commit = exact_commit(args.expected_firmware_commit)
    run_id = str(args.github_actions_run)
    if commit is None:
        raise ValueError("--expected-firmware-commit must be an exact 40-character SHA")
    if not RUN_ID_RE.fullmatch(run_id):
        raise ValueError("--github-actions-run must be a positive numeric run ID")
    if args.baud != D1L_BAUD:
        raise ValueError(f"--baud must be {D1L_BAUD}")
    if not 1 <= args.wifi_cycles <= 100:
        raise ValueError("--wifi-cycles must be between 1 and 100")
    if not 0 < args.session_timeout_sec <= MAX_SESSION_TIMEOUT_SEC:
        raise ValueError(
            f"--session-timeout-sec must be between 0 and {MAX_SESSION_TIMEOUT_SEC:g}"
        )
    if args.timeout <= 0:
        raise ValueError("--timeout must be positive")
    if not args.github_run_dir:
        raise ValueError("--github-run-dir is required for exact Actions provenance")

    parent_environment = dict(os.environ if environment is None else environment)
    target_ssid = parent_environment.get("D1L_WIFI_TARGET_SSID")
    if not target_ssid:
        raise ValueError(
            "D1L_WIFI_TARGET_SSID must identify the already-saved profile; "
            "this orchestrator accepts no Wi-Fi password"
        )
    try:
        encoded_ssid = target_ssid.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError("D1L_WIFI_TARGET_SSID must be valid UTF-8") from exc
    if not encoded_ssid or len(encoded_ssid) > 32 or any(
        ord(char) < 0x20 or ord(char) == 0x7F for char in target_ssid
    ):
        raise ValueError(
            "D1L_WIFI_TARGET_SSID must be 1-32 bytes without control characters"
        )

    source = source_probe(ROOT)
    source_validation = validate_source_context(
        source,
        expected_commit=commit,
    )
    report: dict[str, Any] = {
        "schema": 1,
        "kind": "post_flash_feature_matrix",
        "claim_scope": "core_persistence_saved_profile_wifi_ui_scroll_sd_file_subset",
        "mode": "hardware",
        "created_at": utc_now(),
        "port": port,
        "baud": args.baud,
        "expected_firmware_commit": commit,
        "github_actions_run": run_id,
        "commit": source.get("commit"),
        "git": source,
        "source": source,
        "source_validation": source_validation,
        "session_timeout_sec": args.session_timeout_sec,
        "absolute_session_deadline_enforced": True,
        "wifi_cycles": args.wifi_cycles,
        "ui_screens": list(UI_SCREENS),
        "covered_feature_subsets": list(COVERED_FEATURE_SUBSETS),
        "physical_observed": False,
        "simulated": False,
        "dry_run": False,
        "full_feature_matrix_covered": False,
        "uncovered_full_feature_gates": list(UNCOVERED_FULL_FEATURE_GATES),
        "feature_subset_complete": False,
        "release_gate_eligible": False,
        "soak_started": False,
        "soak_deferred_until_feature_tests_complete": True,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "credential_handling": {
            "password_argument_supported": False,
            "password_forwarded_to_children": False,
            "password_recorded": False,
            "saved_profile_target_from_environment": True,
            "target_ssid_recorded_by_orchestrator": False,
        },
        "steps": [],
        "ok": False,
    }

    if source_validation["ok"] is not True:
        report["classification"] = "source_context_invalid"
        return report

    run_dir = Path(args.github_run_dir)
    if not run_dir.is_absolute():
        run_dir = ROOT / run_dir
    try:
        actions_context = actions_probe(
            run_dir,
            expected_commit=commit,
            run_id=run_id,
        )
    except (OSError, ValueError) as exc:
        actions_context = {
            "ok": False,
            "failures": ["actions_context_unreadable"],
            "error": str(exc),
        }
    report["actions_context"] = actions_context
    if actions_context.get("ok") is not True:
        report["classification"] = "actions_context_invalid"
        return report

    child_env = sanitized_child_environment(
        parent_environment,
        target_ssid=target_ssid,
    )
    sensitive_parent_keys = [
        key
        for key in parent_environment
        if any(
            token in key.upper()
            for token in ("PASSWORD", "PSK", "SECRET", "TOKEN", "CREDENTIAL")
        )
    ]
    report["credential_handling"].update(
        {
            "sensitive_parent_key_count": len(sensitive_parent_keys),
            "sensitive_parent_keys_forwarded": False,
            "forwarded_environment_keys": sorted(
                key for key in child_env if key != "D1L_WIFI_TARGET_SSID"
            ),
        }
    )

    hardware_dir = Path(args.hardware_dir)
    if not hardware_dir.is_absolute():
        hardware_dir = ROOT / hardware_dir
    session_stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    session_dir = hardware_dir / (
        f"post-flash-feature-matrix-{commit[:7]}-actions{run_id}-"
        f"{session_stamp}-{os.getpid()}"
    )
    session_dir.mkdir(parents=True, exist_ok=False)
    report["session_dir"] = str(session_dir)

    started = clock()
    deadline = started + args.session_timeout_sec
    preflight = identity_probe(
        port=port,
        baud=args.baud,
        timeout=min(args.timeout, max(0.001, deadline - clock())),
        expected_commit=commit,
        deadline=deadline,
        clock=clock,
    )
    report["identity_preflight"] = preflight
    preflight_validation = validate_identity_receipt(
        preflight,
        expected_commit=commit,
    )
    report["identity_preflight_validation"] = preflight_validation
    report["physical_observed"] = preflight_validation["valid"]
    if preflight_validation["valid"] is not True:
        report["classification"] = "identity_preflight_failed"
        report["elapsed_sec"] = round(clock() - started, 3)
        report["deadline_exhausted"] = clock() >= deadline
        return report

    child_timed_out = False
    for spec in STEP_SPECS:
        remaining = deadline - clock()
        receipt = session_dir / _receipt_name(spec, commit, run_id, port)
        command = child_command(
            spec,
            root=ROOT,
            port=port,
            expected_commit=commit,
            receipt=receipt,
            wifi_cycles=args.wifi_cycles,
            screens=UI_SCREENS,
        )
        step: dict[str, Any] = {
            "name": spec.name,
            "script": f"scripts/{spec.script}",
            "receipt": str(receipt),
            "evidence_context": {
                "expected_firmware_commit": commit,
                "github_actions_run": run_id,
                "port": port,
            },
            "timeout_sec": round(min(spec.timeout_sec, max(0.0, remaining)), 3),
            "ok": False,
        }
        if remaining <= 0:
            step["classification"] = "session_deadline_exhausted"
            step["receipt_exists"] = receipt.is_file()
            step["receipt_sha256"] = (
                sha256_file(receipt) if receipt.is_file() else None
            )
            report["steps"].append(step)
            break
        if not (ROOT / "scripts" / spec.script).is_file():
            step["classification"] = "required_runner_missing"
            step["receipt_exists"] = False
            step["receipt_sha256"] = None
            report["steps"].append(step)
            break
        try:
            result = process_runner(
                command,
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
                timeout=min(spec.timeout_sec, remaining),
                env=child_env,
            )
        except subprocess.TimeoutExpired:
            step["classification"] = "child_timeout"
            step["receipt_exists"] = receipt.is_file()
            step["receipt_sha256"] = (
                sha256_file(receipt) if receipt.is_file() else None
            )
            report["steps"].append(step)
            child_timed_out = True
            break
        except OSError as exc:
            step["classification"] = "child_start_failed"
            step["error_type"] = type(exc).__name__
            step["receipt_exists"] = receipt.is_file()
            step["receipt_sha256"] = (
                sha256_file(receipt) if receipt.is_file() else None
            )
            report["steps"].append(step)
            break

        step["returncode"] = result.returncode
        step["stdout_bytes"] = len((result.stdout or "").encode("utf-8"))
        step["stderr_bytes"] = len((result.stderr or "").encode("utf-8"))
        data = read_json(receipt)
        validation = validate_child_receipt(
            spec.name,
            data,
            expected_commit=commit,
        )
        step["receipt_exists"] = receipt.is_file()
        step["receipt_sha256"] = sha256_file(receipt) if receipt.is_file() else None
        step["validation"] = validation
        completed_before_deadline = clock() <= deadline
        step["completed_before_deadline"] = completed_before_deadline
        step["ok"] = (
            result.returncode == 0
            and receipt.is_file()
            and validation["valid"]
            and completed_before_deadline
        )
        step["classification"] = (
            "passed"
            if step["ok"]
            else (
                "session_deadline_exhausted"
                if not completed_before_deadline
                else "failed"
            )
        )
        report["steps"].append(step)
        if not step["ok"]:
            break

    postflight: dict[str, Any] | None = None
    remaining = deadline - clock()
    if remaining > 0:
        postflight = identity_probe(
            port=port,
            baud=args.baud,
            timeout=min(args.timeout, remaining),
            expected_commit=commit,
            deadline=deadline,
            clock=clock,
        )
    report["identity_postflight"] = postflight
    postflight_validation = (
        validate_identity_receipt(postflight, expected_commit=commit)
        if isinstance(postflight, dict)
        else {"valid": False, "checks": {}, "failures": ["postflight_missing"]}
    )
    report["identity_postflight_validation"] = postflight_validation
    report["elapsed_sec"] = round(clock() - started, 3)
    report["deadline_exhausted"] = clock() >= deadline
    report["child_timeout"] = child_timed_out
    report["identity_continuity"] = bool(
        preflight_validation["valid"] is True
        and postflight_validation["valid"] is True
        and exact_commit(preflight.get("device_build_commit")) == commit
        and exact_commit(postflight.get("device_build_commit")) == commit
    )
    report["preserved_child_receipts"] = [
        {
            "name": step["name"],
            "path": step["receipt"],
            "sha256": step["receipt_sha256"],
        }
        for step in report["steps"]
        if step.get("receipt_exists") is True
    ]
    report["all_executed_child_receipts_preserved"] = all(
        step.get("receipt_exists") is True
        for step in report["steps"]
        if step.get("classification") != "session_deadline_exhausted"
    )
    passed_names = {
        step["name"] for step in report["steps"] if step.get("ok") is True
    }
    required_names = {spec.name for spec in STEP_SPECS}
    report["feature_subset_complete"] = (
        passed_names == required_names
        and report["identity_continuity"] is True
        and report["deadline_exhausted"] is False
        and report["child_timeout"] is False
        and report["all_executed_child_receipts_preserved"] is True
    )
    report["classification"] = (
        "passed_feature_subset_only"
        if report["feature_subset_complete"]
        else "feature_subset_failed"
    )
    report["ok"] = report["feature_subset_complete"]
    return report


def default_out(args: argparse.Namespace) -> Path:
    commit = exact_commit(args.expected_firmware_commit) or "unknown"
    run_id = str(args.github_actions_run or "unknown")
    port = normalize_port(args.port) or "unknown"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    return (
        ROOT
        / "artifacts"
        / "hardware"
        / port.lower()
        / (
            f"post_flash_feature_matrix_{commit}_actions_{run_id}_"
            f"{port}_{stamp}.json"
        )
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            f"Bounded post-flash {D1L_PORT} feature subset; accepts no password, "
            "performs no flash, RF send, format, raw diagnostic, or soak."
        )
    )
    parser.add_argument("--port", required=True)
    parser.add_argument("--expected-firmware-commit", required=True)
    parser.add_argument("--github-actions-run", required=True)
    parser.add_argument(
        "--github-run-dir",
        help="Extracted exact-run Actions artifacts; required for hardware mode",
    )
    parser.add_argument("--baud", type=int, default=D1L_BAUD)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--session-timeout-sec", type=float, default=900.0)
    parser.add_argument("--wifi-cycles", type=int, default=1)
    parser.add_argument("--hardware-dir", default="artifacts/hardware")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--out")
    if any(CREDENTIAL_ARGUMENT_RE.match(argument) for argument in argv):
        parser.error(
            "credential arguments are not supported; provision the saved profile "
            "separately"
        )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    if not args.dry_run:
        print(
            json.dumps(
                {
                    "schema": 1,
                    "kind": "post_flash_feature_matrix",
                    "mode": "hardware",
                    "ok": False,
                    "classification": "hardware_execution_disabled",
                    "error": HARDWARE_EXECUTION_DISABLED,
                    "public_rf_tx": False,
                    "dm_rf_tx": False,
                    "formats_sd": False,
                },
                sort_keys=True,
            )
        )
        return 2
    try:
        report = run_matrix(args)
    except ValueError as exc:
        report = {
            "schema": 1,
            "kind": "post_flash_feature_matrix",
            "mode": "dry-run" if args.dry_run else "hardware",
            "ok": False,
            "classification": "input_rejected",
            "error": str(exc),
            "expected_firmware_commit": exact_commit(
                args.expected_firmware_commit
            ),
            "github_actions_run": str(args.github_actions_run),
            "port": normalize_port(args.port),
            "full_feature_matrix_covered": False,
            "uncovered_full_feature_gates": list(UNCOVERED_FULL_FEATURE_GATES),
            "release_gate_eligible": False,
            "soak_started": False,
            "public_rf_tx": False,
            "dm_rf_tx": False,
            "formats_sd": False,
        }
    out = Path(args.out) if args.out else default_out(args)
    if not out.is_absolute():
        out = ROOT / out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
    )
    print(
        json.dumps(
            {
                "schema": 1,
                "kind": report.get("kind"),
                "ok": report.get("ok") is True,
                "classification": report.get("classification"),
                "out": str(out),
            },
            sort_keys=True,
        )
    )
    return 0 if report.get("ok") is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
