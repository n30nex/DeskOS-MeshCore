#!/usr/bin/env python3
"""Serial-only RP2040 SD file-operation canary for MeshCore DeskOS D1L."""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

try:
    from artifact_metadata import stamp_report
    from smoke_d1l import open_d1l_serial, send_console_command
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import stamp_report
    from scripts.smoke_d1l import open_d1l_serial, send_console_command


CANARY_COMMANDS = [
    "storage status",
    "storage mount",
    "storage status",
    "storage filecanary",
    "storage status",
    "packets",
    "health",
]
FILE_CANARY_MIN_TIMEOUT_SECONDS = 120.0

RETAINED_HISTORY_BACKENDS = (
    "message_store_backend",
    "dm_store_backend",
    "route_store_backend",
    "packet_log_backend",
)


def _number_at_least(value, minimum: int) -> bool:
    try:
        return int(value) >= minimum
    except (TypeError, ValueError):
        return False


def storage_status_fresh(storage_status: dict) -> bool:
    sd = storage_status.get("sd")
    if not isinstance(sd, dict):
        return False
    try:
        refresh_failures = int(sd.get("refresh_failures"))
    except (TypeError, ValueError):
        return False
    return (
        sd.get("status_stale") is False
        and sd.get("presence_stale") is False
        and refresh_failures == 0
    )


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


def storage_manager_ready_for_file_ops(storage_status: dict) -> bool:
    manager = storage_status.get("manager")
    if not isinstance(manager, dict):
        return True
    return manager.get("state") == "READY_SD"


def storage_ready_for_filecanary(storage_status: dict) -> bool:
    return storage_file_gate_ready(storage_status) and storage_manager_ready_for_file_ops(storage_status)


def retained_history_backends_ready(storage_status: dict) -> bool:
    stores = storage_status.get("stores")
    store_backends = stores if isinstance(stores, dict) else {}
    retained_nvs = storage_status.get("retained_nvs")
    retained_sd = storage_status.get("retained_sd")
    retained_store_stats = retained_sd.get("stores") if isinstance(retained_sd, dict) else None
    return (
        storage_status.get("ok") is True
        and storage_status.get("data_enabled") is True
        # Current SD-first firmware reports "sd"; retain "mixed" for
        # previously banked receipts and fixtures from the split-backend era.
        and storage_status.get("data_backend") in {"sd", "mixed"}
        and all(storage_status.get(field) == "sd" for field in RETAINED_HISTORY_BACKENDS)
        and store_backends.get("messages") == "sd"
        and store_backends.get("dm") == "sd"
        and store_backends.get("routes") == "sd"
        and store_backends.get("packets") == "sd"
        and isinstance(retained_nvs, dict)
        and retained_nvs.get("partition") == "d1l_retained"
        and retained_nvs.get("marker_ready") is True
        and retained_nvs.get("markers_complete") is True
        and retained_nvs.get("anchor_ready") is True
        and retained_nvs.get("sentinel_ready") is True
        and retained_nvs.get("external_init_required") is False
        and retained_nvs.get("ready") is True
        and retained_nvs.get("init_error") == "ESP_OK"
        and retained_nvs.get("migration_error") == "ESP_OK"
        and isinstance(retained_sd, dict)
        and retained_sd.get("degraded") is False
        and retained_sd.get("backup_degraded") is False
        and isinstance(retained_store_stats, dict)
        and all(
            isinstance(retained_store_stats.get(name), dict)
            and retained_store_stats[name].get("nvs_mirror_last_error") == "ESP_OK"
            for name in ("messages", "dm", "routes", "packets")
        )
        and storage_file_gate_ready(storage_status)
    )


def filecanary_passed(canary: dict) -> bool:
    return (
        canary.get("ok") is True
        and canary.get("rename_replace") is True
        and canary.get("read_final") is True
        and canary.get("delete_final") is True
        and canary.get("stat_deleted") is True
    )


def filecanary_transient_timeout(canary: dict) -> bool:
    return (
        canary.get("ok") is False
        and canary.get("cmd") == "storage filecanary"
        and (
            canary.get("code") == "ESP_ERR_TIMEOUT"
            or canary.get("file_note") == "timeout"
        )
    )


def unexpected_console_restart(result: dict) -> bool:
    return result.get("ignored_boot_help_seen") is True or any(
        ignored.get("cmd") == "help"
        for ignored in (result.get("ignored_json") or [])
        if isinstance(ignored, dict)
    )


def skipped_after_timeout(command: str, timed_out_command: str) -> dict:
    return {
        "schema": 1,
        "ok": False,
        "cmd": command,
        "code": "SKIPPED_AFTER_TIMEOUT",
        "timed_out_command": timed_out_command,
    }


def dry_run_report() -> dict:
    return {
        "schema": 1,
        "mode": "dry-run",
        "commands": CANARY_COMMANDS,
        "hardware_required": False,
        "public_rf_tx": False,
        "formats_sd": False,
        "ok": True,
    }


def wait_for_storage_ready(
    ser,
    timeout: float,
    *,
    mount_wait_sec: float,
    poll_interval_sec: float = 1.0,
    allow_unavailable: bool = False,
) -> tuple[dict, list[dict], dict | None]:
    initial = send_console_command(ser, "storage status", timeout)
    results = [initial]
    if initial.get("code") == "TIMEOUT" or unexpected_console_restart(initial):
        return initial, results, None
    if storage_ready_for_filecanary(initial) or allow_unavailable:
        return initial, results, None

    mount = None
    if not storage_file_gate_ready(initial):
        mount = send_console_command(ser, "storage mount", max(timeout, 8.0))
        results.append(mount)
        if mount.get("code") == "TIMEOUT" or unexpected_console_restart(mount):
            return initial, results, mount

    deadline = time.monotonic() + mount_wait_sec
    latest = initial
    while time.monotonic() < deadline:
        time.sleep(poll_interval_sec)
        latest = send_console_command(ser, "storage status", timeout)
        results.append(latest)
        if latest.get("code") == "TIMEOUT" or unexpected_console_restart(latest):
            return latest, results, mount
        if storage_ready_for_filecanary(latest):
            return latest, results, mount

    return latest, results, mount


def run_canary(
    port: str,
    baud: int,
    timeout: float,
    allow_unavailable: bool,
    mount_wait_sec: float = 30.0,
) -> dict:
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("pyserial is required for hardware SD canary: python -m pip install pyserial") from exc

    results: list[dict] = []
    with open_d1l_serial(serial, port=port, baudrate=baud, timeout=timeout) as ser:
        ser.reset_input_buffer()
        before, ready_wait_results, mount_attempt = wait_for_storage_ready(
            ser,
            timeout,
            mount_wait_sec=mount_wait_sec,
            allow_unavailable=allow_unavailable,
        )
        results.extend(ready_wait_results)
        timed_out_command = next(
            (
                result.get("cmd", "storage preflight")
                for result in ready_wait_results
                if result.get("code") == "TIMEOUT"
                or unexpected_console_restart(result)
            ),
            None,
        )
        canary = (
            skipped_after_timeout("storage filecanary", timed_out_command)
            if timed_out_command is not None
            else send_console_command(
                ser,
                "storage filecanary",
                max(timeout, FILE_CANARY_MIN_TIMEOUT_SECONDS),
            )
        )
        initial_canary = canary
        results.append(canary)
        if canary.get("code") == "TIMEOUT" or unexpected_console_restart(canary):
            timed_out_command = "storage filecanary"
        canary_retry_status = None
        canary_retry = None
        if timed_out_command is None and filecanary_transient_timeout(canary):
            canary_retry_status = send_console_command(ser, "storage status", timeout)
            results.append(canary_retry_status)
            if (
                canary_retry_status.get("code") == "TIMEOUT"
                or unexpected_console_restart(canary_retry_status)
            ):
                timed_out_command = "storage status"
            elif storage_ready_for_filecanary(canary_retry_status):
                time.sleep(1.0)
                canary_retry = send_console_command(
                    ser,
                    "storage filecanary",
                    max(timeout, FILE_CANARY_MIN_TIMEOUT_SECONDS),
                )
                results.append(canary_retry)
                canary = canary_retry
                if (
                    canary_retry.get("code") == "TIMEOUT"
                    or unexpected_console_restart(canary_retry)
                ):
                    timed_out_command = "storage filecanary"
        after = (
            skipped_after_timeout("storage status", timed_out_command)
            if timed_out_command is not None
            else send_console_command(ser, "storage status", timeout)
        )
        if after.get("code") == "TIMEOUT" or unexpected_console_restart(after):
            timed_out_command = "storage status"
        packets = (
            skipped_after_timeout("packets", timed_out_command)
            if timed_out_command is not None
            else send_console_command(ser, "packets", timeout)
        )
        if packets.get("code") == "TIMEOUT" or unexpected_console_restart(packets):
            timed_out_command = "packets"
        health = (
            skipped_after_timeout("health", timed_out_command)
            if timed_out_command is not None
            else send_console_command(ser, "health", timeout)
        )
        if health.get("code") == "TIMEOUT" or unexpected_console_restart(health):
            timed_out_command = "health"
        results.extend([after, packets, health])

    retained_history_sd_before = retained_history_backends_ready(before)
    retained_history_sd_after = retained_history_backends_ready(after)
    canary_ok = filecanary_passed(canary)
    unavailable_ok = (
        allow_unavailable
        and canary.get("ok") is False
        and canary.get("code") == "ESP_ERR_NOT_SUPPORTED"
        and canary.get("step") == "preflight"
    )

    return {
        "schema": 1,
        "mode": "hardware",
        "port": port,
        "baud": baud,
        "commands": CANARY_COMMANDS,
        "public_rf_tx": False,
        "formats_sd": False,
        "mount_wait_sec": mount_wait_sec,
        "sequence_completed": timed_out_command is None,
        "timed_out_command": timed_out_command,
        "unexpected_console_restart": any(
            unexpected_console_restart(result) for result in results
        ),
        "ok": (
            timed_out_command is None
            and before.get("ok", False)
            and (canary_ok or unavailable_ok)
            and after.get("ok", False)
            and packets.get("ok", False)
            and health.get("ok", False)
        ),
        "allow_unavailable": allow_unavailable,
        "canary_passed": canary_ok,
        "canary_unavailable_ok": unavailable_ok,
        "canary_retried": canary_retry is not None,
        "canary_retry_ready": (
            storage_ready_for_filecanary(canary_retry_status)
            if canary_retry_status is not None else None
        ),
        "storage_file_gate_ready_before": storage_file_gate_ready(before),
        "storage_file_gate_ready_after": storage_file_gate_ready(after),
        "storage_manager_ready_before": storage_manager_ready_for_file_ops(before),
        "storage_manager_ready_after": storage_manager_ready_for_file_ops(after),
        "storage_ready_waited": len(ready_wait_results) > 1,
        "storage_ready_poll_count": sum(
            1 for result in ready_wait_results if result.get("cmd") == "storage status"
        ),
        "retained_history_sd_ready_before": retained_history_sd_before,
        "retained_history_sd_ready_after": retained_history_sd_after,
        "initial_storage_status": ready_wait_results[0] if ready_wait_results else None,
        "mount_attempt": mount_attempt,
        "storage_before": before,
        "storage_after": after,
        "canary": canary,
        "initial_canary": initial_canary,
        "canary_retry_status": canary_retry_status,
        "canary_retry": canary_retry,
        "packets": packets,
        "health": health,
        "results": results,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--mount-wait-sec", type=float, default=30.0)
    parser.add_argument("--allow-unavailable", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--list-commands", action="store_true")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    if args.list_commands:
        for command in CANARY_COMMANDS:
            print(command)
        return 0

    if args.dry_run:
        report = dry_run_report()
    else:
        if not args.port:
            parser.error("No D1L port supplied. Set D1L_PORT or pass --port.")
        report = run_canary(
            args.port,
            args.baud,
            args.timeout,
            args.allow_unavailable,
            mount_wait_sec=args.mount_wait_sec,
        )

    root = Path(__file__).resolve().parents[1]
    stamp_report(report, root)
    if args.out:
        out_path = Path(args.out)
        if not out_path.is_absolute():
            out_path = root / out_path
    else:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        out_path = root / "artifacts" / "sd-canary" / f"d1l-sd-file-canary-{stamp}.json"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2), encoding="ascii")
    print(json.dumps(report))
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
