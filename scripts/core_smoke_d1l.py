#!/usr/bin/env python3
"""Fail-closed Core 1.0 hardware smoke runner for the verified D1L target."""

from __future__ import annotations

import argparse
import json
import os
import re
import time
from collections.abc import Callable, Iterable
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from artifact_metadata import git_metadata, stamp_report
    from d1l_serial_target import (
        POSIX_D1L_TARGET,
        WINDOWS_D1L_TARGET,
        resolve_target,
        safe_slug,
    )
    from smoke_d1l import (
        exact_commit,
        open_d1l_serial,
        read_command_result,
        reboot_command_passed,
        send_console_command,
        software_reboot_transition_proven,
        timeout_for_reboot_command,
        wait_after_reboot,
        wait_for_console_ready,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import git_metadata, stamp_report
    from scripts.d1l_serial_target import (
        POSIX_D1L_TARGET,
        WINDOWS_D1L_TARGET,
        resolve_target,
        safe_slug,
    )
    from scripts.smoke_d1l import (
        exact_commit,
        open_d1l_serial,
        read_command_result,
        reboot_command_passed,
        send_console_command,
        software_reboot_transition_proven,
        timeout_for_reboot_command,
        wait_after_reboot,
        wait_for_console_ready,
    )


CORE_RELEASE_PROFILE = "core_1_0"
D1L_CORE_PORT = WINDOWS_D1L_TARGET
D1L_CORE_POSIX_TARGET = POSIX_D1L_TARGET
EXPECTED_IDF_VERSION = "v5.5.4"
SD_HISTORY_MODES = frozenset({"disabled", "conditional", "supported_optional"})

# Read-only or bounded local operations that are part of the Core product.
# This list intentionally contains no Public transmit command. Production
# profiles compile qualification-only display/touch commands out, so hardware
# admission uses the always-available board, I2C, and raw-touch diagnostics;
# visible pixels, backlight, and touch interaction remain a manual UI review.
CORE_SMOKE_COMMANDS = (
    "board",
    "settings get",
    "settings onboarding status",
    "i2c",
    "touch raw",
    "button",
    "ui status",
    "radiohw",
    "radio get",
    "map center",
    "mesh status",
    "companion status",
    "wifi status",
    "wifi scan",
    "ble status",
    "rp2040 status",
    "storage status",
    "storage map-policy",
    "storage setup",
    "packets",
    "packets filter any any",
    "packets search core_smoke",
    "messages public",
    "messages public search core_smoke",
    "messages dm",
    "messages unread",
    "nodes",
    "channels",
    "contacts",
    "contacts export",
    "routes",
    "signal",
    "roomservers",
    "repeaters",
    "admin status",
    "terminal status",
    "observer status",
    "update status",
    "crashlog",
)

# These commands would mutate unavailable features if they reached a handler.
# Core command admission must reject each command before any handler or side
# effect. Values are the immutable release-profile feature identifiers.
UNAVAILABLE_MUTATION_PROBES = (
    ("ble on", "ble"),
    ("update", "signed_update"),
    ("qr", "advanced_qr_emoji"),
)

# History/status replies can contain the complete retained window.  A normal
# five-second console timeout is too short for a full 128-packet or message
# response at 115200 baud and causes the valid reply to spill into the next
# command.  Keep the operator timeout for bounded status commands, but give
# retained-history reads enough time to finish before sending another command.
CORE_SLOW_COMMAND_PREFIXES = (
    "packets",
    "messages public",
    "messages dm",
    "messages unread",
    "nodes",
    "contacts export",
)
CORE_SLOW_COMMAND_TIMEOUT_SECONDS = 60.0

DISABLED_SD_MUTATION_PROBES = (
    ("storage mount", "sd_history"),
    ("rp2040 reset", "sd_history"),
    ("ui scroll-probe storage_card", "sd_history"),
    ("ui scroll-probe storage_data", "sd_history"),
)

DISABLED_SD_UNAVAILABLE_STATUS_PROBES = (
    ("storage diag", "sd_history"),
    ("storage diag raw", "sd_history"),
    ("storage setup", "sd_history"),
    ("rp2040 status", "sd_history"),
    ("rp2040 ping", "sd_history"),
    ("rp2040 stock-probe", "sd_history"),
)


def normalize_port(value: object) -> str | None:
    if not isinstance(value, str) or not value.strip():
        return None
    if value.strip() == D1L_CORE_POSIX_TARGET:
        return D1L_CORE_POSIX_TARGET
    normalized = value.strip().upper().replace("/", "\\")
    for prefix in ("\\\\.\\", "\\\\?\\"):
        if normalized.startswith(prefix):
            normalized = normalized[len(prefix) :]
            break
    return normalized


def enforce_core_port(value: object) -> str:
    normalized = normalize_port(value)
    if normalized not in {D1L_CORE_PORT, D1L_CORE_POSIX_TARGET}:
        raise ValueError(
            "Core 1.0 D1L validation requires COM12 or the exact "
            f"{D1L_CORE_POSIX_TARGET} by-id target; got "
            f"{normalized or '<missing>'}"
        )
    return normalized


def resolve_core_target(
    value: object,
    *,
    port_lister: Callable[[], Iterable[object]],
    platform_name: str | None = None,
) -> dict[str, Any]:
    """Resolve the selected target before any serial endpoint is opened."""

    requested = enforce_core_port(value)
    return resolve_target(
        requested,
        port_lister=port_lister,
        platform_name=platform_name,
    )


def exact_identity(
    result: object,
    expected_commit: str,
    expected_sd_history_mode: str,
) -> bool:
    return (
        isinstance(result, dict)
        and result.get("ok") is True
        and exact_commit(result.get("build_commit")) == expected_commit
        and result.get("release_profile") == CORE_RELEASE_PROFILE
        and result.get("sd_history_mode") == expected_sd_history_mode
    )


def exact_public_key(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    return (
        normalized
        if re.fullmatch(r"[0-9a-f]{64}", normalized) is not None
        else None
    )


def d1l_identity_status_ok(
    result: object,
    expected_public_key: object,
) -> bool:
    """Require the complete live D1L identity before later smoke commands."""

    public_key = exact_public_key(expected_public_key)
    return bool(
        public_key is not None
        and isinstance(result, dict)
        and type(result.get("schema")) is int
        and result.get("schema") == 1
        and result.get("ok") is True
        and result.get("cmd") == "identity status"
        and result.get("public_key_ready") is True
        and exact_public_key(result.get("public_key")) == public_key
        and result.get("fingerprint") == public_key[:16].upper()
        and result.get("role") == "desk_companion"
    )


def exact_version_identity(
    result: object,
    expected_commit: str,
    expected_sd_history_mode: str,
) -> bool:
    return (
        exact_identity(result, expected_commit, expected_sd_history_mode)
        and isinstance(result, dict)
        and result.get("cmd") == "version"
        and result.get("idf") == EXPECTED_IDF_VERSION
    )


def exact_unsupported_result(
    result: object, command: str, feature: str
) -> bool:
    return (
        isinstance(result, dict)
        and result.get("ok") is False
        and result.get("cmd") == command
        and result.get("code") == "ESP_ERR_NOT_SUPPORTED"
        and result.get("release_profile") == CORE_RELEASE_PROFILE
        and result.get("feature") == feature
    )


def exact_unavailable_status_result(
    result: object,
    command: str,
    feature: str,
    expected_commit: str,
    expected_sd_history_mode: str,
) -> bool:
    return (
        exact_identity(result, expected_commit, expected_sd_history_mode)
        and isinstance(result, dict)
        and result.get("cmd") == command
        and result.get("available") is False
        and result.get("feature") == feature
        and result.get("mutation_allowed") is False
        and result.get("reason") == "unavailable_in_release_profile"
        and "uart_ready" not in result
        and "bridge_ready" not in result
        and "protocol_supported" not in result
        and "diag_supported" not in result
    )


def disabled_storage_status_ok(result: object) -> bool:
    """Prove that a disabled-SD Core candidate is actually NVS-only."""
    if not isinstance(result, dict):
        return False
    manager = result.get("manager")
    sd = result.get("sd")
    stores = result.get("stores")
    retained_nvs = result.get("retained_nvs")
    if not all(
        isinstance(value, dict)
        for value in (manager, sd, stores, retained_nvs)
    ):
        return False
    return (
        result.get("schema") == 1
        and result.get("ok") is True
        and result.get("cmd") == "storage status"
        and manager.get("running") is False
        and manager.get("force_nvs") is True
        and manager.get("state") == "READY_NVS"
        and result.get("data_enabled") is False
        and result.get("data_backend") == "nvs"
        and result.get("message_store_backend") == "nvs"
        and result.get("dm_store_backend") == "nvs"
        and result.get("packet_log_backend") == "nvs"
        and result.get("route_store_backend") == "nvs"
        and result.get("map_tile_backend") == "unavailable"
        and result.get("map_tile_cache_ready") is False
        and result.get("map_tile_download_supported") is False
        and sd.get("rp2040_bridge_ready") is False
        and sd.get("rp2040_protocol_supported") is False
        and sd.get("mounted") is False
        and sd.get("data_root_ready") is False
        and sd.get("file_ops") is False
        and sd.get("atomic_rename") is False
        and stores
        == {
            "settings": "nvs",
            "identity": "nvs",
            "messages": "nvs",
            "dm": "nvs",
            "packets": "nvs",
            "routes": "nvs",
            "contacts": "nvs",
            "read_state": "nvs",
            "crashlog": "nvs",
            "map_tiles": "unavailable",
            "exports": "serial",
        }
        and result.get("fallback") == "nvs"
        and retained_nvs.get("marker_ready") is True
        and retained_nvs.get("markers_complete") is True
        and retained_nvs.get("anchor_ready") is True
        and retained_nvs.get("sentinel_ready") is True
        and retained_nvs.get("ready") is True
    )


def crashlog_clean(result: object) -> bool:
    """Require the pre-UI raw crash log to contain no crash-like record."""
    if not isinstance(result, dict):
        return False
    entries = result.get("entries")
    return (
        result.get("schema") == 1
        and result.get("ok") is True
        and result.get("cmd") == "crashlog"
        and isinstance(entries, list)
        and not any(
            isinstance(entry, dict) and entry.get("crash_like") is True
            for entry in entries
        )
    )


def send_exact_console_command(ser, command: str, timeout: float) -> dict:
    """Send an admission probe whose rejection echoes the full input command."""
    ser.write((command + "\n").encode("utf-8"))
    if hasattr(ser, "flush"):
        ser.flush()
    return read_command_result(ser, command, timeout)


def timeout_for_core_command(command: str, configured_timeout: float) -> float:
    if any(
        command == prefix or command.startswith(prefix + " ")
        for prefix in CORE_SLOW_COMMAND_PREFIXES
    ):
        return max(configured_timeout, CORE_SLOW_COMMAND_TIMEOUT_SECONDS)
    return configured_timeout


def mutation_probe_plan(sd_history_mode: str) -> list[dict]:
    if sd_history_mode not in SD_HISTORY_MODES:
        raise ValueError(f"invalid SD history mode: {sd_history_mode}")
    probes = list(UNAVAILABLE_MUTATION_PROBES)
    if sd_history_mode == "disabled":
        probes.extend(DISABLED_SD_MUTATION_PROBES)
    return [{"command": command, "feature": feature} for command, feature in probes]


def unavailable_status_probe_plan(sd_history_mode: str) -> list[dict]:
    if sd_history_mode not in SD_HISTORY_MODES:
        raise ValueError(f"invalid SD history mode: {sd_history_mode}")
    probes = (
        DISABLED_SD_UNAVAILABLE_STATUS_PROBES
        if sd_history_mode == "disabled"
        else ()
    )
    return [{"command": command, "feature": feature} for command, feature in probes]


def command_plan(sd_history_mode: str) -> dict:
    """Return a non-closing plan suitable for review and contract tests."""
    return {
        "schema": 2,
        "kind": "core_smoke_plan",
        "mode": "plan",
        "ok": False,
        "closure_eligible": False,
        "dry_run": True,
        "planning_only": True,
        "hardware_required": True,
        "physical_observed": False,
        "port": D1L_CORE_PORT,
        "supported_targets": [
            D1L_CORE_PORT,
            D1L_CORE_POSIX_TARGET,
        ],
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": sd_history_mode,
        "preflight_commands": ["version", "identity status", "health"],
        "supported_commands": list(CORE_SMOKE_COMMANDS),
        "unavailable_mutation_probes": mutation_probe_plan(sd_history_mode),
        "unavailable_status_probes": unavailable_status_probe_plan(
            sd_history_mode
        ),
        "public_rf_tx": False,
        "formats_sd": False,
        "expected_d1l_public_key": None,
        "d1l_identity_status": {},
        "d1l_identity_ok": None,
    }


def _settings_snapshot(result: object) -> dict | None:
    if not isinstance(result, dict) or result.get("ok") is not True:
        return None
    name = result.get("node_name")
    path_hash_bytes = result.get("path_hash_bytes")
    if (
        not isinstance(name, str)
        or not name
        or len(name.encode("utf-8")) > 31
        or any(character in name for character in "\r\n")
        or isinstance(path_hash_bytes, bool)
        or path_hash_bytes not in (1, 2, 3)
    ):
        return None
    return {"node_name": name, "path_hash_bytes": path_hash_bytes}


def _settings_equal(result: object, snapshot: dict) -> bool:
    return _settings_snapshot(result) == snapshot


def _identity_name_equal(result: object, snapshot: dict) -> bool:
    """Bind the rebooted live identity to the one mutated setting."""

    return bool(
        isinstance(result, dict)
        and result.get("node_name") == snapshot.get("node_name")
    )


def _set_settings(ser, timeout: float, snapshot: dict, steps: list[dict]) -> bool:
    # Core persistence mutates only the supported identity name. Path-hash
    # configuration is part of unavailable user TRACE/PATH tooling.
    commands = (f"settings set name {snapshot['node_name']}",)
    results = []
    for command in commands:
        result = send_console_command(ser, command, timeout)
        steps.append({"command": command, "result": result})
        results.append(result)
    observed = send_console_command(ser, "settings get", timeout)
    steps.append({"command": "settings get", "result": observed})
    return all(result.get("ok") is True for result in results) and _settings_equal(
        observed, snapshot
    )


def run_core_persistence_check(
    ser,
    timeout: float,
    expected_d1l_public_key: str,
) -> dict:
    """Prove one mutation/reboot/restore cycle on one exact D1L identity."""
    normalized_public_key = exact_public_key(expected_d1l_public_key)
    if normalized_public_key is None:
        raise ValueError(
            "expected D1L public key must be exactly 64 hexadecimal characters"
        )
    steps: list[dict] = []

    original_result = send_console_command(ser, "settings get", timeout)
    steps.append({"command": "settings get", "result": original_result})
    original = _settings_snapshot(original_result)
    if original is None:
        return {
            "schema": 1,
            "kind": "core_settings_persistence",
            "ok": False,
            "mutation_started": False,
            "reboot_count": 0,
            "expected_d1l_public_key": normalized_public_key,
            "post_reboot_identity_status": {},
            "post_reboot_identity_ok": False,
            "post_reboot_name_ok": False,
            "cleanup_post_reboot_identity_status": {},
            "cleanup_post_reboot_identity_ok": False,
            "cleanup_post_reboot_name_ok": False,
            "d1l_public_key_continuity_ok": False,
            "steps": steps,
        }

    changed = {
        "node_name": (
            "D1L Core Verify"
            if original["node_name"] == "D1L Core Persist"
            else "D1L Core Persist"
        ),
        "path_hash_bytes": original["path_hash_bytes"],
    }
    changed_written = _set_settings(ser, timeout, changed, steps)
    pre_reboot = send_console_command(ser, "health", timeout) if changed_written else {}
    if changed_written:
        steps.append({"command": "health", "result": pre_reboot})
    reboot = (
        send_console_command(
            ser,
            "reboot",
            timeout_for_reboot_command("reboot", timeout),
        )
        if changed_written and pre_reboot.get("ok") is True
        else {}
    )
    if reboot:
        steps.append({"command": "reboot", "result": reboot})

    first_reboot_ok = reboot_command_passed(reboot)
    post_reboot: dict = {}
    post_reboot_identity: dict = {}
    post_reboot_identity_ok = False
    post_reboot_name_ok = False
    first_transition = False
    if first_reboot_ok:
        wait_after_reboot(ser, max(2.5, timeout / 2.0))
        post_reboot = wait_for_console_ready(ser, timeout)
        steps.append({"command": "health", "result": post_reboot})
        first_transition = software_reboot_transition_proven(pre_reboot, post_reboot)
        post_reboot_identity = send_console_command(
            ser,
            "identity status",
            timeout,
        )
        steps.append(
            {
                "command": "identity status",
                "result": post_reboot_identity,
            }
        )
        post_reboot_identity_ok = d1l_identity_status_ok(
            post_reboot_identity,
            normalized_public_key,
        )
        post_reboot_name_ok = bool(
            post_reboot_identity_ok
            and _identity_name_equal(post_reboot_identity, changed)
        )

    persisted_ok = (
        first_transition
        and post_reboot_name_ok
    )
    restored = (
        _set_settings(ser, timeout, original, steps)
        if first_reboot_ok and post_reboot_identity_ok
        else False
    )
    cleanup_pre = send_console_command(ser, "health", timeout) if restored else {}
    if restored:
        steps.append({"command": "health", "result": cleanup_pre})
    cleanup_reboot = (
        send_console_command(
            ser,
            "reboot",
            timeout_for_reboot_command("reboot", timeout),
        )
        if restored and cleanup_pre.get("ok") is True
        else {}
    )
    if cleanup_reboot:
        steps.append({"command": "reboot", "result": cleanup_reboot})

    cleanup_reboot_ok = reboot_command_passed(cleanup_reboot)
    cleanup_post: dict = {}
    cleanup_post_identity: dict = {}
    cleanup_post_identity_ok = False
    cleanup_post_name_ok = False
    cleanup_transition = False
    if cleanup_reboot_ok:
        wait_after_reboot(ser, max(2.5, timeout / 2.0))
        cleanup_post = wait_for_console_ready(ser, timeout)
        steps.append({"command": "health", "result": cleanup_post})
        cleanup_transition = software_reboot_transition_proven(
            cleanup_pre, cleanup_post
        )
        cleanup_post_identity = send_console_command(
            ser,
            "identity status",
            timeout,
        )
        steps.append(
            {
                "command": "identity status",
                "result": cleanup_post_identity,
            }
        )
        cleanup_post_identity_ok = d1l_identity_status_ok(
            cleanup_post_identity,
            normalized_public_key,
        )
        cleanup_post_name_ok = bool(
            cleanup_post_identity_ok
            and _identity_name_equal(cleanup_post_identity, original)
        )

    cleanup_ok = (
        cleanup_transition
        and cleanup_post_name_ok
    )
    d1l_public_key_continuity_ok = bool(
        post_reboot_identity_ok and cleanup_post_identity_ok
    )
    return {
        "schema": 1,
        "kind": "core_settings_persistence",
        "ok": bool(
            changed_written
            and persisted_ok
            and restored
            and cleanup_ok
            and d1l_public_key_continuity_ok
        ),
        "mutation_started": changed_written,
        "reboot_count": int(first_reboot_ok) + int(cleanup_reboot_ok),
        "first_reboot_proven": first_transition,
        "persisted_after_reboot": persisted_ok,
        "original_restored": cleanup_ok,
        "expected_d1l_public_key": normalized_public_key,
        "post_reboot_identity_status": post_reboot_identity,
        "post_reboot_identity_ok": post_reboot_identity_ok,
        "post_reboot_name_ok": post_reboot_name_ok,
        "cleanup_post_reboot_identity_status": cleanup_post_identity,
        "cleanup_post_reboot_identity_ok": cleanup_post_identity_ok,
        "cleanup_post_reboot_name_ok": cleanup_post_name_ok,
        "d1l_public_key_continuity_ok": d1l_public_key_continuity_ok,
        "steps": steps,
    }


def identity_failure_report(
    *,
    port: str,
    baud: int,
    expected_commit: str,
    expected_sd_history_mode: str,
    expected_d1l_public_key: str,
    version: dict,
    github_run_id: str,
    workflow_run_attempt: str,
    d1l_target: dict[str, Any],
    d1l_identity_status: dict | None = None,
    health: dict | None = None,
) -> dict:
    public_key = exact_public_key(expected_d1l_public_key)
    if public_key is None:
        raise ValueError(
            "expected_d1l_public_key must be an exact 64-hex public key"
        )
    identity_status = (
        d1l_identity_status
        if isinstance(d1l_identity_status, dict)
        else {}
    )
    identity_ok = d1l_identity_status_ok(identity_status, public_key)
    health = health or {}
    return {
        "schema": 2,
        "kind": "core_smoke",
        "mode": "hardware",
        "ok": False,
        "closure_eligible": False,
        "hardware_required": True,
        "physical_observed": True,
        "port": port,
        "d1l_target": d1l_target,
        "baud": baud,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": expected_sd_history_mode,
        "expected_firmware_commit": expected_commit,
        "github_actions_run": github_run_id,
        "workflow_run_attempt": workflow_run_attempt,
        "expected_d1l_public_key": public_key,
        "d1l_identity_status": identity_status,
        "d1l_identity_ok": identity_ok,
        "device_build_commit": version.get("build_commit"),
        "firmware_identity_required": True,
        "firmware_identity_ok": False,
        "identity_preflight_only": True,
        "supported_commands_executed": [],
        "unavailable_mutation_probes": [],
        "unavailable_status_probes": [],
        "public_rf_tx": False,
        "formats_sd": False,
        "results": (
            [version]
            + ([identity_status] if identity_status else [])
            + ([health] if health else [])
        ),
    }


def run_core_smoke(
    *,
    port: str,
    baud: int,
    timeout: float,
    expected_commit: str,
    expected_sd_history_mode: str,
    expected_d1l_public_key: str,
    persistence_test: bool,
    manual_touch: bool,
    github_run_id: str,
    workflow_run_attempt: str,
    port_lister: Callable[[], Iterable[object]] | None = None,
    platform_name: str | None = None,
) -> dict:
    normalized_public_key = exact_public_key(expected_d1l_public_key)
    if normalized_public_key is None:
        raise ValueError(
            "expected_d1l_public_key must be an exact 64-hex public key"
        )
    try:
        import serial
        from serial.tools import list_ports
    except ImportError as exc:
        raise SystemExit(
            "pyserial is required for Core hardware smoke: "
            "python -m pip install pyserial"
        ) from exc

    port = enforce_core_port(port)
    normalized_commit = exact_commit(expected_commit)
    if normalized_commit is None:
        raise ValueError("expected_commit must be an exact 40-character hexadecimal SHA")
    if expected_sd_history_mode not in SD_HISTORY_MODES:
        raise ValueError("expected_sd_history_mode is invalid")
    if (
        not str(github_run_id).isdigit()
        or int(github_run_id) < 1
        or not str(workflow_run_attempt).isdigit()
        or int(workflow_run_attempt) < 1
    ):
        raise ValueError(
            "GitHub run id and run attempt must be positive integers"
        )
    root = Path(__file__).resolve().parents[1]
    source = git_metadata(root)
    if not (
        exact_commit(source.get("commit")) == normalized_commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    ):
        raise ValueError(
            "Core smoke must run from the exact clean candidate source"
        )
    d1l_target = resolve_core_target(
        port,
        port_lister=port_lister
        or (lambda: list_ports.comports(include_links=True)),
        platform_name=platform_name,
    )
    port = d1l_target["requested_path"]

    results: list[dict] = []
    probe_results: list[dict] = []
    status_probe_results: list[dict] = []
    persistence: dict | None = None
    with open_d1l_serial(
        serial, port=port, baudrate=baud, timeout=timeout
    ) as ser:
        time.sleep(1.0)
        ser.reset_input_buffer()

        # Identity and immutable profile admission always happen first. No
        # supported action or mutation probe runs on mismatch.
        version = send_console_command(ser, "version", timeout)
        if not exact_version_identity(
            version, normalized_commit, expected_sd_history_mode
        ):
            return identity_failure_report(
                port=port,
                baud=baud,
                expected_commit=normalized_commit,
                expected_sd_history_mode=expected_sd_history_mode,
                expected_d1l_public_key=normalized_public_key,
                version=version,
                github_run_id=str(github_run_id),
                workflow_run_attempt=str(workflow_run_attempt),
                d1l_target=d1l_target,
            )
        d1l_identity_status = send_console_command(
            ser,
            "identity status",
            timeout,
        )
        d1l_identity_ok = d1l_identity_status_ok(
            d1l_identity_status,
            normalized_public_key,
        )
        if not d1l_identity_ok:
            return identity_failure_report(
                port=port,
                baud=baud,
                expected_commit=normalized_commit,
                expected_sd_history_mode=expected_sd_history_mode,
                expected_d1l_public_key=normalized_public_key,
                version=version,
                d1l_identity_status=d1l_identity_status,
                github_run_id=str(github_run_id),
                workflow_run_attempt=str(workflow_run_attempt),
                d1l_target=d1l_target,
            )
        health_preflight = send_console_command(ser, "health", timeout)
        if not exact_identity(
            health_preflight, normalized_commit, expected_sd_history_mode
        ):
            return identity_failure_report(
                port=port,
                baud=baud,
                expected_commit=normalized_commit,
                expected_sd_history_mode=expected_sd_history_mode,
                expected_d1l_public_key=normalized_public_key,
                version=version,
                d1l_identity_status=d1l_identity_status,
                health=health_preflight,
                github_run_id=str(github_run_id),
                workflow_run_attempt=str(workflow_run_attempt),
                d1l_target=d1l_target,
            )
        results.extend([version, d1l_identity_status, health_preflight])

        for command in CORE_SMOKE_COMMANDS:
            results.append(
                send_console_command(
                    ser,
                    command,
                    timeout_for_core_command(command, timeout),
                )
            )

        for probe in unavailable_status_probe_plan(
            expected_sd_history_mode
        ):
            result = send_exact_console_command(
                ser, probe["command"], timeout
            )
            status_probe_results.append({**probe, "result": result})

        for probe in mutation_probe_plan(expected_sd_history_mode):
            result = send_exact_console_command(
                ser, probe["command"], timeout
            )
            probe_results.append({**probe, "result": result})

        persistence = (
            run_core_persistence_check(
                ser,
                timeout,
                normalized_public_key,
            )
            if persistence_test
            else None
        )
        health_final = send_console_command(ser, "health", timeout)
        d1l_identity_status_final = send_console_command(
            ser,
            "identity status",
            timeout,
        )
        results.extend([health_final, d1l_identity_status_final])

    if manual_touch:
        print(
            "Manual check required: confirm the 480x480 display, backlight, "
            "and touch response on this exact candidate."
        )

    supported_ok = all(result.get("ok") is True for result in results)
    storage_rows = [
        result for result in results if result.get("cmd") == "storage status"
    ]
    crashlog_rows = [
        result for result in results if result.get("cmd") == "crashlog"
    ]
    disabled_storage_ok = (
        expected_sd_history_mode != "disabled"
        or (
            len(storage_rows) == 1
            and disabled_storage_status_ok(storage_rows[0])
        )
    )
    crashlog_ok = (
        len(crashlog_rows) == 1 and crashlog_clean(crashlog_rows[0])
    )
    probes_ok = all(
        exact_unsupported_result(
            row["result"], row["command"], row["feature"]
        )
        for row in probe_results
    )
    status_probes_ok = all(
        exact_unavailable_status_result(
            row["result"],
            row["command"],
            row["feature"],
            normalized_commit,
            expected_sd_history_mode,
        )
        for row in status_probe_results
    )
    health_final = results[-2]
    health_ready = (
        exact_identity(health_final, normalized_commit, expected_sd_history_mode)
        and health_final.get("board_ready") is True
        and health_final.get("ui_ready") is True
    )
    d1l_identity_final_ok = d1l_identity_status_ok(
        d1l_identity_status_final,
        normalized_public_key,
    )
    d1l_public_key_continuity_ok = bool(
        d1l_identity_final_ok
        and (
            persistence is None
            or persistence.get("d1l_public_key_continuity_ok") is True
        )
    )
    report = {
        "schema": 2,
        "kind": "core_smoke",
        "mode": "hardware",
        "ok": bool(
            supported_ok
            and probes_ok
            and status_probes_ok
            and disabled_storage_ok
            and crashlog_ok
            and health_ready
            and d1l_public_key_continuity_ok
            and (persistence is None or persistence.get("ok") is True)
        ),
        "closure_eligible": True,
        "hardware_required": True,
        "physical_observed": True,
        "port": port,
        "d1l_target": d1l_target,
        "baud": baud,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": expected_sd_history_mode,
        "expected_firmware_commit": normalized_commit,
        "github_actions_run": str(github_run_id),
        "workflow_run_attempt": str(workflow_run_attempt),
        "expected_d1l_public_key": normalized_public_key,
        "d1l_identity_status": d1l_identity_status,
        "d1l_identity_ok": True,
        "d1l_identity_status_final": d1l_identity_status_final,
        "d1l_identity_final_ok": d1l_identity_final_ok,
        "d1l_public_key_continuity_ok": d1l_public_key_continuity_ok,
        "device_build_commit": version.get("build_commit"),
        "firmware_identity_required": True,
        "firmware_identity_ok": True,
        "identity_preflight_only": False,
        "manual_touch_requested": manual_touch,
        "supported_commands": list(CORE_SMOKE_COMMANDS),
        "supported_commands_executed": list(CORE_SMOKE_COMMANDS),
        "unavailable_mutation_probes": probe_results,
        "unavailable_status_probes": status_probe_results,
        "checks": {
            "exact_candidate": True,
            "esp_idf_v5_5_4": version.get("idf") == EXPECTED_IDF_VERSION,
            "core_profile": True,
            "exact_sd_history_mode": True,
            "supported_commands_pass": supported_ok,
            "disabled_sd_nvs_authoritative": disabled_storage_ok,
            "pre_ui_crashlog_clean": crashlog_ok,
            "unavailable_mutations_rejected": probes_ok,
            "disabled_sd_status_probes_truthful": status_probes_ok,
            "health_ready": health_ready,
            "d1l_identity_continuity": d1l_public_key_continuity_ok,
            "persistence_pass": persistence is None
            or persistence.get("ok") is True,
            "no_public_rf": True,
            "no_sd_format": True,
        },
        "public_rf_tx": False,
        "formats_sd": False,
        "results": results,
    }
    if persistence is not None:
        report["persistence"] = persistence
    return report


def resolve_out_path(
    out_arg: str | None,
    target: str = D1L_CORE_PORT,
) -> Path:
    root = Path(__file__).resolve().parents[1]
    if out_arg:
        path = Path(out_arg)
        return path if path.is_absolute() else root / path
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    slug = safe_slug(enforce_core_port(target))
    return (
        root
        / "artifacts"
        / "hardware"
        / slug
        / f"core_smoke_{stamp}_{slug}.json"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--expected-firmware-commit", required=True)
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-attempt", required=True)
    parser.add_argument("--expected-d1l-public-key")
    parser.add_argument(
        "--expected-sd-history-mode",
        required=True,
        choices=sorted(SD_HISTORY_MODES),
    )
    parser.add_argument("--persistence-test", action="store_true")
    parser.add_argument("--manual-touch", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--out")
    args = parser.parse_args(argv)

    try:
        port = enforce_core_port(
            D1L_CORE_PORT
            if args.dry_run and args.port is None
            else args.port
        )
    except ValueError as exc:
        parser.error(str(exc))
    commit = exact_commit(args.expected_firmware_commit)
    if commit is None:
        parser.error(
            "--expected-firmware-commit must be an exact 40-character hexadecimal SHA"
        )
    if (
        not str(args.github_run_id).isdigit()
        or int(args.github_run_id) < 1
        or not str(args.github_run_attempt).isdigit()
        or int(args.github_run_attempt) < 1
    ):
        parser.error(
            "--github-run-id and --github-run-attempt must be positive integers"
        )
    public_key = (
        exact_public_key(args.expected_d1l_public_key)
        if args.expected_d1l_public_key is not None
        else None
    )
    if args.expected_d1l_public_key is not None and public_key is None:
        parser.error(
            "--expected-d1l-public-key must be an exact 64-hex public key"
        )
    if not args.dry_run and public_key is None:
        parser.error(
            "--expected-d1l-public-key is required for hardware execution"
        )

    if args.dry_run:
        report = command_plan(args.expected_sd_history_mode)
        report.update(
            {
                "port": port,
                "expected_firmware_commit": commit,
                "github_actions_run": str(args.github_run_id),
                "workflow_run_attempt": str(args.github_run_attempt),
                "expected_d1l_public_key": public_key,
            }
        )
        if args.out:
            root = Path(__file__).resolve().parents[1]
            out_path = resolve_out_path(args.out, port)
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_text(
                json.dumps(report, indent=2) + "\n",
                encoding="ascii",
            )
            print(
                json.dumps(
                    {
                        "ok": False,
                        "dry_run": True,
                        "out": str(out_path),
                    },
                    indent=2,
                )
            )
        else:
            print(json.dumps(report, indent=2))
        return 0

    report = run_core_smoke(
        port=port,
        baud=args.baud,
        timeout=args.timeout,
        expected_commit=commit,
        expected_sd_history_mode=args.expected_sd_history_mode,
        expected_d1l_public_key=public_key,
        persistence_test=args.persistence_test,
        manual_touch=args.manual_touch,
        github_run_id=str(args.github_run_id),
        workflow_run_attempt=str(args.github_run_attempt),
    )
    root = Path(__file__).resolve().parents[1]
    stamp_report(report, root)
    out_path = resolve_out_path(args.out, report["port"])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2) + "\n", encoding="ascii")
    print(json.dumps({"ok": report["ok"], "out": str(out_path)}, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
