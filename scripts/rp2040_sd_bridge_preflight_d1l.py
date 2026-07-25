#!/usr/bin/env python3
"""Non-destructive RP2040 SD bridge bring-up preflight for MeshCore DeskOS D1L."""

from __future__ import annotations

import argparse
import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path

try:
    from artifact_metadata import stamp_report
    from d1l_serial_target import POSIX_D1L_TARGET
    from flash_rp2040_sd_bridge_uf2 import FlashGuardError, candidate_volumes, verify_artifact
    from sd_file_canary_d1l import storage_status_fresh
    from smoke_d1l import open_d1l_serial, send_console_command, wait_for_console_ready
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import stamp_report
    from scripts.d1l_serial_target import POSIX_D1L_TARGET
    from scripts.flash_rp2040_sd_bridge_uf2 import (
        FlashGuardError,
        candidate_volumes,
        verify_artifact,
    )
    from scripts.sd_file_canary_d1l import storage_status_fresh
    from scripts.smoke_d1l import (
        open_d1l_serial,
        send_console_command,
        wait_for_console_ready,
    )


PREFLIGHT_COMMANDS = [
    "rp2040 status",
    "rp2040 ping",
    "storage status",
    "storage mount",
    "storage status",
    "storage diag",
    "health",
]
MOUNT_POLL_ATTEMPTS = 30
MOUNT_POLL_INTERVAL_SECONDS = 2.0
POSIX_SERIAL_STARTUP_SETTLE_SECONDS = 10.0
POSIX_SERIAL_READY_TIMEOUT_SECONDS = 12.0
TRANSITIONAL_MANAGER_STATES = {"BRIDGE_WAIT", "PING", "STATUS", "MOUNT"}


def dry_run_report(artifact_dir: str | None = None) -> dict:
    return {
        "schema": 1,
        "mode": "dry-run",
        "hardware_required": False,
        "artifact_dir": artifact_dir,
        "commands": PREFLIGHT_COMMANDS,
        "public_rf_tx": False,
        "formats_sd": False,
        "copies_uf2": False,
        "ready_for_sd_acceptance": False,
        "storage_status_settle_exhausted": False,
        "ok": True,
    }


def verify_optional_artifact(artifact_dir: str | None, expected_sha256: str | None) -> dict:
    if not artifact_dir:
        return {
            "checked": False,
            "ok": None,
            "reason": "artifact_dir_not_supplied",
        }
    try:
        artifact = verify_artifact(Path(artifact_dir), expected_sha256)
    except FlashGuardError as exc:
        return {
            "checked": True,
            "ok": False,
            "error": str(exc),
        }
    return {
        "checked": True,
        "ok": True,
        **artifact,
    }


def _number_at_least(value, minimum: int) -> bool:
    try:
        return int(value) >= minimum
    except (TypeError, ValueError):
        return False


def storage_file_gate_ready(storage_status: dict) -> bool:
    sd = storage_status.get("sd")
    if not isinstance(sd, dict):
        return False
    return (
        storage_status_fresh(storage_status)
        and sd.get("state") == "ready"
        and sd.get("filesystem") == "fat32"
        and sd.get("present") is True
        and sd.get("mounted") is True
        and sd.get("data_root_ready") is True
        and sd.get("rp2040_protocol_supported") is True
        and sd.get("file_ops") is True
        and sd.get("atomic_rename") is True
        and _number_at_least(sd.get("file_line_max"), 512)
        and _number_at_least(sd.get("file_chunk_max"), 192)
        and _number_at_least(sd.get("path_max"), 96)
    )


def sd_state(result: dict) -> str | None:
    sd = result.get("sd")
    if not isinstance(sd, dict):
        return None
    state = sd.get("state")
    return state if isinstance(state, str) else None


def storage_manager_state(result: dict) -> str | None:
    manager = result.get("manager")
    if not isinstance(manager, dict):
        return None
    state = manager.get("state")
    return state if isinstance(state, str) else None


def storage_status_transitional(result: dict) -> bool:
    return (
        sd_state(result) == "mount_pending"
        or storage_manager_state(result) in TRANSITIONAL_MANAGER_STATES
    )


def sd_has_mount_error(sd: dict) -> bool:
    for key in ("mount_error", "mount_data"):
        value = sd.get(key)
        if isinstance(value, (int, float)) and value:
            return True
    return False


def sd_has_rejected_probe(sd: dict) -> bool:
    if sd.get("note") == "sd_probe_rejected_card":
        return True
    probe_error = sd.get("probe_error")
    if sd.get("state") == "error" and probe_error in (3, 4):
        return True
    return probe_error in (3, 4)


def storage_diag_skipped_after_ready() -> dict:
    return {
        "schema": 1,
        "ok": True,
        "cmd": "storage diag",
        "skipped": True,
        "reason": "storage_file_gate_ready",
        "diag_supported": True,
        "public_rf_tx": False,
        "formats_sd": False,
    }


def storage_diag_skipped_while_mount_pending() -> dict:
    return {
        "schema": 1,
        "ok": True,
        "cmd": "storage diag",
        "skipped": True,
        "reason": "storage_mount_pending",
        "diag_supported": True,
        "public_rf_tx": False,
        "formats_sd": False,
    }


def classify_preflight(
    rp2040_status: dict,
    rp2040_ping: dict,
    storage_status: dict,
    uf2_candidates: list[dict],
    artifact: dict,
    storage_diag: dict | None = None,
    storage_mount: dict | None = None,
) -> dict:
    sd = storage_status.get("sd") if isinstance(storage_status.get("sd"), dict) else {}
    artifact_ok = artifact.get("ok") is True or artifact.get("ok") is None
    bridge_uart_ready = bool(
        rp2040_status.get("uart_ready") or
        rp2040_ping.get("bridge_ready") or
        sd.get("rp2040_bridge_ready")
    )
    ping_supported = rp2040_ping.get("protocol_supported") is True
    protocol_supported = sd.get("rp2040_protocol_supported") is True
    file_gate_ready = storage_file_gate_ready(storage_status)
    manager_state = storage_manager_state(storage_status)
    manager_ready = manager_state == "READY_SD"
    ready_for_sd_acceptance = file_gate_ready and manager_ready
    uf2_volume_available = len(uf2_candidates) == 1
    diag_attempted = isinstance(storage_diag, dict) and bool(storage_diag)
    diag_supported = storage_diag.get("diag_supported") is True if diag_attempted else False
    mount_attempted = isinstance(storage_mount, dict) and bool(storage_mount)
    mount_ok = storage_mount.get("ok") is True if mount_attempted else None

    if ready_for_sd_acceptance:
        state = "sd_bridge_ready"
        next_action = "run_sd_file_and_export_acceptance"
    elif manager_state in TRANSITIONAL_MANAGER_STATES:
        state = "storage_manager_transition_pending"
        next_action = "wait_for_storage_manager_ready_sd"
    elif protocol_supported and sd.get("state") == "mount_pending":
        state = "sd_mount_pending"
        next_action = "wait_for_storage_mount_or_reset_rp2040_bridge"
    elif bridge_uart_ready and ping_supported and not protocol_supported:
        state = "sd_status_pending"
        next_action = "inspect_storage_status_and_run_storage_diag"
    elif bridge_uart_ready and not protocol_supported:
        state = "rp2040_protocol_pending"
        next_action = (
            "dry_run_then_copy_rp2040_uf2" if uf2_volume_available
            else "put_rp2040_in_uf2_bootloader"
        )
    elif mount_attempted and mount_ok is False:
        state = "sd_mount_pending"
        next_action = "inspect_rp2040_sd_mount_timeout_and_reset_bridge"
    elif protocol_supported and sd.get("state") == "mount_required":
        state = "sd_mount_required"
        next_action = "run_storage_mount"
    elif protocol_supported and sd_has_rejected_probe(sd):
        state = "sd_probe_rejected_card"
        next_action = "inspect_rp2040_sd_cmd0_firmware_path"
    elif (
        protocol_supported
        and sd.get("present") is True
        and sd.get("mounted") is not True
        and sd.get("needs_fat32") is True
    ):
        state = "raw_card_present_mount_failed"
        next_action = (
            "inspect_rp2040_sd_mount_error_firmware_path"
            if sd_has_mount_error(sd)
            else "confirm_fat32_card_or_inspect_rp2040_sd_mount_path"
        )
    elif not bridge_uart_ready:
        state = "rp2040_uart_unavailable"
        next_action = "check_d1l_power_cable_and_rp2040_uart"
    elif protocol_supported and sd.get("present") is not True:
        if diag_attempted and not diag_supported and artifact_ok:
            state = "sd_card_not_present_diag_pending"
            next_action = (
                "dry_run_then_copy_rp2040_uf2" if uf2_volume_available
                else "put_rp2040_in_uf2_bootloader"
            )
        else:
            state = "sd_card_not_present"
            next_action = "insert_empty_or_sacrificial_sd_card"
    else:
        state = "sd_not_ready"
        next_action = "inspect_storage_status_and_run_sd_file_canary"

    return {
        "state": state,
        "next_action": next_action,
        "artifact_ready": artifact_ok,
        "uf2_volume_available": uf2_volume_available,
        "rp2040_uart_ready": bridge_uart_ready,
        "rp2040_ping_supported": ping_supported,
        "rp2040_ping_sd_touched": rp2040_ping.get("sd_touched"),
        "rp2040_protocol_supported": protocol_supported,
        "rp2040_diag_supported": diag_supported,
        "storage_mount_ok": mount_ok,
        "storage_file_gate_ready": file_gate_ready,
        "storage_manager_state": manager_state,
        "storage_manager_ready_sd": manager_ready,
        "ready_for_sd_acceptance": ready_for_sd_acceptance,
        "sd_state": sd.get("state"),
        "sd_last_error": sd.get("last_error"),
        "sd_mount_error": sd.get("mount_error"),
        "sd_mount_data": sd.get("mount_data"),
    }


def run_preflight(
    *,
    port: str,
    baud: int,
    timeout: float,
    artifact_dir: str | None,
    expected_sha256: str | None,
) -> dict:
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("pyserial is required for hardware preflight: python -m pip install pyserial") from exc

    artifact = verify_optional_artifact(artifact_dir, expected_sha256)
    uf2_candidates = candidate_volumes()
    results: list[dict] = []
    commands_run: list[str] = []
    storage_status_poll_reasons: list[dict] = []

    def send_command(ser, command: str, command_timeout: float) -> dict:
        commands_run.append(command)
        result = send_console_command(ser, command, command_timeout)
        results.append(result)
        return result

    startup_health: dict = {}
    with open_d1l_serial(serial, port=port, baudrate=baud, timeout=timeout) as ser:
        if port == POSIX_D1L_TARGET:
            time.sleep(POSIX_SERIAL_STARTUP_SETTLE_SECONDS)
            startup_health = wait_for_console_ready(
                ser,
                POSIX_SERIAL_READY_TIMEOUT_SECONDS,
                attempts=3,
            )
        else:
            time.sleep(1.0)
        ser.reset_input_buffer()
        rp2040_status = send_command(ser, "rp2040 status", timeout)
        rp2040_ping = send_command(ser, "rp2040 ping", timeout)
        if rp2040_ping.get("ok") is not True:
            bridge_ready = bool(rp2040_status.get("uart_ready") or rp2040_ping.get("bridge_ready"))
            storage_status = {
                "schema": 1,
                "ok": False,
                "cmd": "storage status",
                "skipped": True,
                "reason": "rp2040_ping_failed",
                "sd": {
                    "state": "protocol_pending" if bridge_ready else "pending_bridge",
                    "rp2040_bridge_ready": bridge_ready,
                    "rp2040_protocol_supported": False,
                    "last_error": rp2040_ping.get("code") or rp2040_ping.get("error") or "ESP_ERR_TIMEOUT",
                },
            }
            initial_storage_status = storage_status
            storage_mount = {"ok": None, "cmd": "storage mount", "skipped": True, "reason": "rp2040_ping_failed"}
            storage_diag = {"ok": None, "cmd": "storage diag", "skipped": True, "reason": "rp2040_ping_failed"}
            post_mount_statuses = []
            health = send_command(ser, "health", timeout)
        else:
            initial_storage_status = send_command(ser, "storage status", timeout)
            command = "storage mount"
            command_timeout = max(timeout, 15.0) if command in {"storage mount", "storage diag"} else timeout
            storage_mount = send_command(ser, command, command_timeout)
            storage_status = send_command(ser, "storage status", timeout)
            post_mount_statuses = [storage_status]
            for _attempt in range(MOUNT_POLL_ATTEMPTS):
                if not storage_status_transitional(storage_status):
                    break
                storage_status_poll_reasons.append(
                    {
                        "sd_state": sd_state(storage_status),
                        "manager_state": storage_manager_state(storage_status),
                    }
                )
                time.sleep(MOUNT_POLL_INTERVAL_SECONDS)
                storage_status = send_command(ser, "storage status", timeout)
                post_mount_statuses.append(storage_status)
            if storage_file_gate_ready(storage_status):
                storage_diag = storage_diag_skipped_after_ready()
            elif sd_state(storage_status) == "mount_pending":
                storage_diag = storage_diag_skipped_while_mount_pending()
            else:
                command = "storage diag"
                command_timeout = max(timeout, 15.0) if command in {"storage mount", "storage diag"} else timeout
                storage_diag = send_command(ser, command, command_timeout)
            health = send_command(ser, "health", timeout)
    classification = classify_preflight(
        rp2040_status,
        rp2040_ping,
        storage_status,
        uf2_candidates,
        artifact,
        storage_diag,
        storage_mount,
    )
    storage_ok = storage_status.get("ok") is True
    storage_mount_ok = storage_mount.get("ok") is True
    health_ok = health.get("ok") is True
    storage_diag_ok = storage_diag.get("ok") is True
    rp2040_status_ok = rp2040_status.get("ok") is True
    rp2040_ping_ok = rp2040_ping.get("ok") is True
    rp2040_status_optional_ok = rp2040_status_ok or classification["rp2040_uart_ready"]
    startup_ready = (
        startup_health.get("ok") is True
        if port == POSIX_D1L_TARGET
        else True
    )
    command_ok = (
        startup_ready
        and rp2040_status_optional_ok
        and rp2040_ping_ok
        and storage_ok
        and health_ok
    )
    artifact_ok = artifact.get("ok") is not False
    storage_status_settle_exhausted = storage_status_transitional(storage_status)
    return {
        "schema": 1,
        "mode": "hardware-preflight",
        "port": port,
        "baud": baud,
        "commands": commands_run,
        "public_rf_tx": False,
        "formats_sd": False,
        "copies_uf2": False,
        "ok": command_ok and artifact_ok and not storage_status_settle_exhausted,
        "serial_commands_ok": command_ok,
        "rp2040_status_ok": rp2040_status_ok,
        "rp2040_ping_ok": rp2040_ping_ok,
        "rp2040_status_optional_ok": rp2040_status_optional_ok,
        "storage_mount_ok": storage_mount_ok,
        "storage_diag_ok": storage_diag_ok,
        "ready_for_sd_acceptance": classification["ready_for_sd_acceptance"],
        "classification": classification,
        "artifact": artifact,
        "candidate_volumes": uf2_candidates,
        "rp2040_status": rp2040_status,
        "rp2040_ping": rp2040_ping,
        "initial_storage_status": initial_storage_status,
        "storage_mount": storage_mount,
        "storage_status": storage_status,
        "post_mount_storage_statuses": post_mount_statuses,
        "storage_mount_poll_count": sum(
            1
            for reason in storage_status_poll_reasons
            if reason.get("sd_state") == "mount_pending"
        ),
        "storage_status_poll_count": max(0, len(post_mount_statuses) - 1),
        "storage_status_poll_reasons": storage_status_poll_reasons,
        "storage_manager_transition_poll_count": sum(
            1
            for reason in storage_status_poll_reasons
            if reason.get("manager_state") in TRANSITIONAL_MANAGER_STATES
        ),
        "storage_manager_initial_state": (
            storage_manager_state(post_mount_statuses[0]) if post_mount_statuses else None
        ),
        "storage_manager_final_state": storage_manager_state(storage_status),
        "storage_manager_transition_exhausted": (
            storage_manager_state(storage_status) in TRANSITIONAL_MANAGER_STATES
        ),
        "storage_status_settle_exhausted": storage_status_settle_exhausted,
        "storage_diag": storage_diag,
        "health": health,
        "serial_startup_health": startup_health,
        "results": results,
    }


def write_report(report: dict, out_path: Path | None) -> Path:
    root = Path(__file__).resolve().parents[1]
    if out_path is None:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        out_path = root / "artifacts" / "rp2040-preflight" / f"d1l-rp2040-sd-bridge-preflight-{stamp}.json"
    elif not out_path.is_absolute():
        out_path = root / out_path
    stamp_report(report, root)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2) + "\n", encoding="ascii")
    return out_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--artifact-dir")
    parser.add_argument("--expected-sha256")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--list-commands", action="store_true")
    parser.add_argument("--out")
    args = parser.parse_args()

    if args.list_commands:
        for command in PREFLIGHT_COMMANDS:
            print(command)
        return 0

    if args.dry_run:
        report = dry_run_report(args.artifact_dir)
    else:
        if not args.port:
            parser.error("No D1L port supplied. Set D1L_PORT or pass --port.")
        report = run_preflight(
            port=args.port,
            baud=args.baud,
            timeout=args.timeout,
            artifact_dir=args.artifact_dir,
            expected_sha256=args.expected_sha256,
        )

    written = write_report(report, Path(args.out) if args.out else None)
    print(
        json.dumps(
            {
                "ok": report["ok"],
                "ready_for_sd_acceptance": report.get("ready_for_sd_acceptance"),
                "storage_status_settle_exhausted": report.get(
                    "storage_status_settle_exhausted", False
                ),
                "out": str(written),
                "mode": report["mode"],
            }
        )
    )
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
