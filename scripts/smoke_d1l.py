#!/usr/bin/env python3
"""Hardware smoke runner for MeshCore DeskOS D1L."""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

try:
    from artifact_metadata import stamp_report
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import stamp_report


SMOKE_COMMANDS = [
    "version",
    "board",
    "settings get",
    "settings onboarding status",
    "identity status",
    "i2c",
    "display test",
    "touch test",
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
    "packets search test",
    "messages public",
    "messages public search test",
    "messages dm",
    "messages unread",
    "nodes",
    "contacts",
    "contacts export",
    "routes",
    "routes trace 0BF0A701D5AE2DB6",
    "signal",
    "roomservers",
    "repeaters",
    "crashlog",
    "health",
]

# These fields describe host-observed console framing. Device JSON must never
# be able to supply or override them.
HOST_CONSOLE_EVIDENCE_FIELDS = frozenset(
    {
        "ignored_json_count",
        "ignored_json",
        "ignored_boot_help_seen",
        "expected_boot_help_after_reboot",
    }
)

REBOOT_MIN_TIMEOUT_SECONDS = 20.0


def parse_jsonl_line(line: str) -> dict | None:
    line = line.strip()
    if not line.startswith("{"):
        return None
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        return None
    if obj.get("schema") != 1:
        return None
    return obj


def parse_jsonl(lines: Iterable[str]) -> list[dict]:
    return [obj for line in lines if (obj := parse_jsonl_line(line)) is not None]


def exact_commit(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    if len(normalized) != 40 or any(char not in "0123456789abcdef" for char in normalized):
        return None
    return normalized


def firmware_identity_matches(version_result: dict | None, expected_commit: str) -> bool:
    expected = exact_commit(expected_commit)
    actual = exact_commit(version_result.get("build_commit") if isinstance(version_result, dict) else None)
    return expected is not None and actual == expected


def dry_run_report(expected_firmware_commit: str | None = None) -> dict:
    return {
        "schema": 1,
        "mode": "dry-run",
        "commands": SMOKE_COMMANDS,
        "hardware_required": False,
        "expected_firmware_commit": expected_firmware_commit,
        "firmware_identity_required": expected_firmware_commit is not None,
        "ok": True,
    }


def read_command_result(ser, command: str, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    ignored: list[dict] = []
    ignored_boot_help_seen = False
    original_timeout = getattr(ser, "timeout", None)
    try:
        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            if hasattr(ser, "timeout"):
                ser.timeout = max(0.001, min(timeout, remaining))
            raw_bytes = ser.readline()
            if not raw_bytes:
                continue
            raw = raw_bytes.decode("utf-8", errors="replace")
            parsed = parse_jsonl_line(raw)
            if parsed and parsed.get("cmd") == command:
                parsed = {
                    key: value
                    for key, value in parsed.items()
                    if key not in HOST_CONSOLE_EVIDENCE_FIELDS
                }
                if ignored:
                    parsed = {
                        **parsed,
                        "ignored_json_count": len(ignored),
                        "ignored_json": ignored[-5:],
                        "ignored_boot_help_seen": ignored_boot_help_seen,
                    }
                return parsed
            if parsed:
                ignored_boot_help_seen = (
                    ignored_boot_help_seen or parsed.get("cmd") == "help"
                )
                ignored.append(
                    {
                        "cmd": parsed.get("cmd"),
                        "ok": parsed.get("ok"),
                    }
                )
    finally:
        if hasattr(ser, "timeout"):
            ser.timeout = original_timeout
    return {
        "schema": 1,
        "ok": False,
        "cmd": command,
        "code": "TIMEOUT",
        "ignored_json_count": len(ignored),
        "ignored_json": ignored[-5:],
        "ignored_boot_help_seen": ignored_boot_help_seen,
    }


def expected_command_name(command: str) -> str:
    if command.startswith("messages public search "):
        return "messages public"
    if command == "messages dm clear":
        return command
    for prefix in [
        "settings set name ",
        "settings set pathhash ",
        "settings onboarding complete ",
        "contacts add ",
        "contacts export ",
        "contacts import ",
        "contacts rename ",
        "contacts delete ",
        "messages dm ",
        "mesh send dm ",
        "routes trace ",
        "routes probe ",
        "packets filter ",
        "packets search ",
        "packets raw ",
        "ui tab ",
        "ui scroll-probe ",
        "ui compose-probe ",
        "ui data-canary ",
        "ui capture chunk ",
        "radio set freq ",
        "radio set bw ",
        "radio set sf ",
        "radio set cr ",
        "radio set txpower ",
        "radio set rxboost ",
        "storage map-tile-canary ",
        "storage map-tile-check ",
        "storage export-canary ",
        "storage export-diagnostics ",
        "storage export-data ",
        "storage retained-canary ",
        "core retained-witness ",
        "backlight ",
        "mesh send public ",
        "mesh send dm ",
        "rp2040 baud-probe ",
        "rp2040 set-baud ",
    ]:
        if command.startswith(prefix):
            return prefix.strip()
    return command


def send_console_command(ser, command: str, timeout: float) -> dict:
    ser.write((command + "\n").encode("utf-8"))
    if hasattr(ser, "flush"):
        ser.flush()
    return read_command_result(ser, expected_command_name(command), timeout)


def timeout_for_reboot_command(command: str, configured_timeout: float) -> float:
    if command == "reboot":
        return max(configured_timeout, REBOOT_MIN_TIMEOUT_SECONDS)
    return configured_timeout


def reboot_command_passed(result: dict | None) -> bool:
    return (
        isinstance(result, dict)
        and result.get("ok") is True
        and result.get("rebooting") is True
        and result.get("reset_scope") == "system"
        and result.get("storage_manager_quiesced") is True
        and result.get("retained_worker_quiesced") is True
        and result.get("rp2040_bridge_quiesced") is True
        and result.get("connectivity_prepare") == "ESP_OK"
        and result.get("retained_flush") == "ESP_OK"
        and result.get("route_flush") == "ESP_OK"
    )


def health_boot_nonce(result: dict | None) -> int | None:
    if not isinstance(result, dict) or result.get("ok") is not True:
        return None
    value = result.get("boot_nonce")
    if isinstance(value, bool) or not isinstance(value, int) or value == 0:
        return None
    return value


def boot_transition_proven(before_health: dict | None, after_health: dict | None) -> bool:
    before = health_boot_nonce(before_health)
    after = health_boot_nonce(after_health)
    return before is not None and after is not None and before != after


def software_reboot_transition_proven(
    before_health: dict | None, after_health: dict | None
) -> bool:
    return (
        boot_transition_proven(before_health, after_health)
        and isinstance(after_health, dict)
        and after_health.get("reset_reason") == "SW"
    )


def persistence_settings_snapshot(result: dict | None) -> dict | None:
    if not isinstance(result, dict) or result.get("ok") is not True:
        return None
    node_name = result.get("node_name")
    path_hash_bytes = result.get("path_hash_bytes")
    wifi_enabled = result.get("wifi_enabled")
    ble_enabled = result.get("ble_companion_enabled")
    try:
        node_name_bytes = node_name.encode("utf-8") if isinstance(node_name, str) else b""
    except UnicodeEncodeError:
        return None
    if (
        not node_name_bytes
        or len(node_name_bytes) > 31
        or any(char in node_name for char in "\r\n")
        or any(ord(char) < 0x20 or ord(char) == 0x7F for char in node_name)
        or isinstance(path_hash_bytes, bool)
        or not isinstance(path_hash_bytes, int)
        or path_hash_bytes not in (1, 2, 3)
        or not isinstance(wifi_enabled, bool)
        or not isinstance(ble_enabled, bool)
    ):
        return None
    return {
        "node_name": node_name,
        "path_hash_bytes": path_hash_bytes,
        "wifi_enabled": wifi_enabled,
        "ble_companion_enabled": ble_enabled,
    }


def wifi_profile_snapshot(result: dict | None) -> dict | None:
    if not isinstance(result, dict) or result.get("ok") is not True:
        return None
    fields = ("setting_enabled", "profile_saved", "password_saved")
    if any(not isinstance(result.get(field), bool) for field in fields):
        return None
    ssid = result.get("ssid")
    if not isinstance(ssid, str):
        return None
    return {field: result[field] for field in fields} | {"ssid": ssid}


def settings_match_snapshot(result: dict | None, snapshot: dict | None) -> bool:
    parsed = persistence_settings_snapshot(result)
    return parsed is not None and parsed == snapshot


def restore_persistence_settings(
    ser,
    timeout: float,
    snapshot: dict,
    steps: list[dict],
    attempts: int = 2,
) -> dict:
    rows: list[dict] = []
    for _attempt in range(max(1, attempts)):
        name = send_console_command(
            ser, f"settings set name {snapshot['node_name']}", timeout
        )
        path_hash = send_console_command(
            ser,
            f"settings set pathhash {snapshot['path_hash_bytes']}",
            timeout,
        )
        settings = send_console_command(ser, "settings get", timeout)
        steps.extend([name, path_hash, settings])
        row = {
            "name": name,
            "path_hash": path_hash,
            "settings": settings,
            "ok": (
                name.get("ok") is True
                and path_hash.get("ok") is True
                and settings_match_snapshot(settings, snapshot)
            ),
        }
        rows.append(row)
        if row["ok"]:
            break
    return {
        "ok": bool(rows and rows[-1]["ok"]),
        "attempt_count": len(rows),
        "attempts": rows,
        "name": rows[-1]["name"] if rows else {},
        "path_hash": rows[-1]["path_hash"] if rows else {},
        "settings": rows[-1]["settings"] if rows else {},
    }


def open_d1l_serial(serial_module, *, port: str, baudrate: int, timeout: float):
    """Open the ESP32 console without pulsing its auto-reset control lines."""
    ser = serial_module.Serial(port=None, baudrate=baudrate, timeout=timeout)
    try:
        # pySerial applies its cached DTR/RTS states while opening. Configure
        # both while the handle is still closed so CH340-backed D1L consoles
        # never observe pySerial's reset-prone default asserted states.
        ser.dtr = False
        ser.rts = False
        ser.port = port
        ser.open()
    except Exception:
        try:
            ser.close()
        except Exception:
            pass
        raise
    return ser


def wait_after_reboot(ser, settle_seconds: float) -> None:
    time.sleep(max(settle_seconds, 7.0))
    ser.reset_input_buffer()


def wait_for_console_ready(ser, timeout: float, attempts: int = 3) -> dict:
    last: dict | None = None
    for attempt in range(max(1, attempts)):
        last = send_console_command(ser, "health", max(timeout, 8.0))
        if last.get("ok") is True:
            return last
        if attempt + 1 < attempts:
            time.sleep(1.0)
            ser.reset_input_buffer()
    return last or {
        "schema": 1,
        "ok": False,
        "cmd": "health",
        "code": "CONSOLE_NOT_READY",
    }


def run_persistence_check(ser, timeout: float) -> dict:
    steps: list[dict] = []
    original = send_console_command(ser, "settings get", timeout)
    steps.append(original)
    original_wifi_status = send_console_command(ser, "wifi status", timeout)
    steps.append(original_wifi_status)
    snapshot = persistence_settings_snapshot(original)
    wifi_profile_before = wifi_profile_snapshot(original_wifi_status)
    snapshot_valid = (
        snapshot is not None
        and wifi_profile_before is not None
        and wifi_profile_before["setting_enabled"] is snapshot["wifi_enabled"]
    )
    if not snapshot_valid:
        return {
            "schema": 1,
            "ok": False,
            "test": "settings_persistence_reboot",
            "settings_snapshot_valid": False,
            "mutation_started": False,
            "reboot_attempted": False,
            "reboot_command_passed": False,
            "reboot_reset_scope": None,
            "reboot_connectivity_prepare": None,
            "reboot_route_flush": None,
            "reboot_storage_manager_quiesced": None,
            "reboot_rp2040_bridge_quiesced": None,
            "post_reboot_reset_reason": None,
            "reboot_proven": False,
            "cleanup_reboot_command_passed": False,
            "cleanup_reboot_reset_scope": None,
            "cleanup_reboot_connectivity_prepare": None,
            "cleanup_reboot_route_flush": None,
            "cleanup_reboot_storage_manager_quiesced": None,
            "cleanup_reboot_rp2040_bridge_quiesced": None,
            "cleanup_post_reboot_reset_reason": None,
            "cleanup_reboot_proven": False,
            "cancelled_reboot_cleanup_attempted": False,
            "cancelled_reboot_cleanup_ok": False,
            "before_reboot_ok": False,
            "after_reboot_ok": False,
            "cleanup_ok": False,
            "steps": steps,
        }

    original_name = snapshot["node_name"]
    original_path_hash = snapshot["path_hash_bytes"]
    test_name = (
        "D1L Smoke Verify"
        if original_name == "D1L Smoke Persist"
        else "D1L Smoke Persist"
    )
    test_path_hash = 2 if original_path_hash == 3 else 3
    mutation_results = [
        send_console_command(ser, f"settings set name {test_name}", timeout),
        send_console_command(
            ser, f"settings set pathhash {test_path_hash}", timeout
        ),
    ]
    steps.extend(mutation_results)
    before = send_console_command(ser, "settings get", timeout)
    steps.append(before)
    mutation_commands_ok = all(result.get("ok") is True for result in mutation_results)
    before_ok = (
        mutation_commands_ok
        and before.get("node_name") == test_name
        and before.get("path_hash_bytes") == test_path_hash
        and before.get("wifi_enabled") is snapshot["wifi_enabled"]
        and before.get("ble_companion_enabled")
        is snapshot["ble_companion_enabled"]
    )
    reboot_attempted = before_ok
    pre_reboot_health: dict = {}
    reboot: dict = {}
    if reboot_attempted:
        pre_reboot_health = send_console_command(ser, "health", timeout)
        steps.append(pre_reboot_health)
        reboot = send_console_command(
            ser,
            "reboot",
            timeout_for_reboot_command("reboot", timeout),
        )
        steps.append(reboot)
    reboot_ok = reboot_command_passed(reboot)
    post_reboot_health: dict = {}
    reboot_proven = False
    after: dict = {}
    after_wifi_status: dict = {}
    restore_result: dict = {}
    restore_name: dict = {}
    restore_path_hash: dict = {}
    restored_before_cleanup_reboot: dict = {}
    cleanup_pre_reboot_health: dict = {}
    cleanup_reboot: dict = {}
    cleanup_reboot_ok = False
    cleanup_post_reboot_health: dict = {}
    cleanup_reboot_proven = False
    cleanup: dict = {}
    cleanup_wifi_status: dict = {}
    restore_before_cleanup_reboot_ok = False
    cancelled_reboot_cleanup_attempted = False
    cancelled_reboot_restore_name: dict = {}
    cancelled_reboot_restore_path_hash: dict = {}
    cancelled_reboot_cleanup: dict = {}
    cancelled_reboot_cleanup_wifi_status: dict = {}
    cancelled_reboot_cleanup_ok = False
    if reboot_ok:
        wait_after_reboot(ser, max(2.5, timeout / 2.0))
        post_reboot_health = wait_for_console_ready(ser, timeout)
        steps.append(post_reboot_health)
        reboot_proven = software_reboot_transition_proven(
            pre_reboot_health, post_reboot_health
        )

        after = send_console_command(ser, "settings get", timeout)
        steps.append(after)
        after_wifi_status = send_console_command(ser, "wifi status", timeout)
        steps.append(after_wifi_status)
        restore_result = restore_persistence_settings(
            ser, timeout, snapshot, steps
        )
        restore_name = restore_result["name"]
        restore_path_hash = restore_result["path_hash"]
        restored_before_cleanup_reboot = restore_result["settings"]
        restore_before_cleanup_reboot_ok = restore_result["ok"]
        if restore_before_cleanup_reboot_ok:
            cleanup_pre_reboot_health = send_console_command(ser, "health", timeout)
            steps.append(cleanup_pre_reboot_health)
            cleanup_reboot = send_console_command(
                ser,
                "reboot",
                timeout_for_reboot_command("reboot", timeout),
            )
            steps.append(cleanup_reboot)
            cleanup_reboot_ok = reboot_command_passed(cleanup_reboot)
            if cleanup_reboot_ok:
                wait_after_reboot(ser, max(2.5, timeout / 2.0))
                cleanup_post_reboot_health = wait_for_console_ready(ser, timeout)
                steps.append(cleanup_post_reboot_health)
                cleanup_reboot_proven = software_reboot_transition_proven(
                    cleanup_pre_reboot_health,
                    cleanup_post_reboot_health,
                )
        cleanup = send_console_command(ser, "settings get", timeout)
        steps.append(cleanup)
        cleanup_wifi_status = send_console_command(ser, "wifi status", timeout)
        steps.append(cleanup_wifi_status)
    else:
        # A mutation verification failure prevents the first reboot; a
        # controlled reboot can also be rejected by the route/NVS durability
        # gate. In either case the console remains available, so restore the
        # exact original name/path-hash fields without touching saved Wi-Fi or
        # unrelated settings. Evidence remains failed because no reboot
        # transition was proven.
        cancelled_reboot_cleanup_attempted = True
        cancelled_restore_result = restore_persistence_settings(
            ser, timeout, snapshot, steps
        )
        cancelled_reboot_restore_name = cancelled_restore_result["name"]
        cancelled_reboot_restore_path_hash = cancelled_restore_result["path_hash"]
        cancelled_reboot_cleanup = cancelled_restore_result["settings"]
        cancelled_reboot_cleanup_wifi_status = send_console_command(
            ser, "wifi status", timeout
        )
        steps.append(cancelled_reboot_cleanup_wifi_status)
        cancelled_reboot_cleanup_ok = (
            cancelled_restore_result["ok"]
            and wifi_profile_snapshot(cancelled_reboot_cleanup_wifi_status)
            == wifi_profile_before
        )

    after_ok = reboot_proven and (
        after.get("node_name") == test_name
        and after.get("path_hash_bytes") == test_path_hash
        and after.get("wifi_enabled") is snapshot["wifi_enabled"]
        and after.get("ble_companion_enabled")
        is snapshot["ble_companion_enabled"]
        and wifi_profile_snapshot(after_wifi_status) == wifi_profile_before
    )
    cleanup_ok = (
        cleanup_reboot_proven
        and restore_before_cleanup_reboot_ok
        and settings_match_snapshot(cleanup, snapshot)
        and wifi_profile_snapshot(cleanup_wifi_status) == wifi_profile_before
    )

    return {
        "schema": 1,
        "ok": all(step.get("ok") for step in steps) and before_ok and after_ok and cleanup_ok,
        "test": "settings_persistence_reboot",
        "settings_snapshot_valid": snapshot_valid,
        "mutation_started": True,
        "mutation_commands_ok": mutation_commands_ok,
        "mutation_verified": before_ok,
        "reboot_attempted": reboot_attempted,
        "original_node_name": original_name,
        "original_path_hash_bytes": original_path_hash,
        "original_wifi_enabled": snapshot["wifi_enabled"],
        "original_ble_companion_enabled": snapshot["ble_companion_enabled"],
        "wifi_profile_before": wifi_profile_before,
        "temporary_node_name": test_name,
        "temporary_path_hash_bytes": test_path_hash,
        "reboot_command_passed": reboot_ok,
        "reboot_reset_scope": reboot.get("reset_scope"),
        "reboot_connectivity_prepare": reboot.get("connectivity_prepare"),
        "reboot_route_flush": reboot.get("route_flush"),
        "reboot_storage_manager_quiesced": reboot.get("storage_manager_quiesced"),
        "reboot_rp2040_bridge_quiesced": reboot.get("rp2040_bridge_quiesced"),
        "post_reboot_reset_reason": post_reboot_health.get("reset_reason"),
        "pre_reboot_boot_nonce": health_boot_nonce(pre_reboot_health),
        "post_reboot_boot_nonce": health_boot_nonce(post_reboot_health),
        "reboot_proven": reboot_proven,
        "cleanup_reboot_command_passed": cleanup_reboot_ok,
        "cleanup_reboot_reset_scope": cleanup_reboot.get("reset_scope"),
        "cleanup_reboot_connectivity_prepare": cleanup_reboot.get("connectivity_prepare"),
        "cleanup_reboot_route_flush": cleanup_reboot.get("route_flush"),
        "cleanup_reboot_storage_manager_quiesced": cleanup_reboot.get("storage_manager_quiesced"),
        "cleanup_reboot_rp2040_bridge_quiesced": cleanup_reboot.get("rp2040_bridge_quiesced"),
        "cleanup_post_reboot_reset_reason": cleanup_post_reboot_health.get("reset_reason"),
        "cleanup_pre_reboot_boot_nonce": health_boot_nonce(cleanup_pre_reboot_health),
        "cleanup_post_reboot_boot_nonce": health_boot_nonce(cleanup_post_reboot_health),
        "cleanup_reboot_proven": cleanup_reboot_proven,
        "restore_before_cleanup_reboot_ok": restore_before_cleanup_reboot_ok,
        "restore_result": restore_result,
        "cancelled_reboot_cleanup_attempted": cancelled_reboot_cleanup_attempted,
        "cancelled_reboot_cleanup_ok": cancelled_reboot_cleanup_ok,
        "cancelled_reboot_restore_name": cancelled_reboot_restore_name,
        "cancelled_reboot_restore_path_hash": cancelled_reboot_restore_path_hash,
        "cancelled_reboot_cleanup": cancelled_reboot_cleanup,
        "cancelled_reboot_cleanup_wifi_status": cancelled_reboot_cleanup_wifi_status,
        "before_reboot_ok": before_ok,
        "after_reboot_ok": after_ok,
        "cleanup_ok": cleanup_ok,
        "steps": steps,
    }


def run_serial_smoke(
    port: str,
    baud: int,
    timeout: float,
    manual_touch: bool,
    persistence_test: bool,
    expected_firmware_commit: str | None = None,
) -> dict:
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("pyserial is required for hardware smoke: python -m pip install pyserial") from exc

    results: list[dict] = []
    with open_d1l_serial(serial, port=port, baudrate=baud, timeout=timeout) as ser:
        time.sleep(1.0)
        ser.reset_input_buffer()
        for command in SMOKE_COMMANDS:
            results.append(send_console_command(ser, command, timeout))
        persistence = run_persistence_check(ser, timeout) if persistence_test else None

    if manual_touch:
        print("Manual check: confirm the display showed color bars and touch coordinates changed.")

    version_result = next((item for item in results if item.get("cmd") == "version"), {})
    identity_ok = (
        firmware_identity_matches(version_result, expected_firmware_commit)
        if expected_firmware_commit is not None
        else None
    )
    report = {
        "schema": 1,
        "mode": "hardware",
        "port": port,
        "baud": baud,
        "expected_firmware_commit": expected_firmware_commit,
        "device_build_commit": version_result.get("build_commit"),
        "firmware_identity_required": expected_firmware_commit is not None,
        "firmware_identity_ok": identity_ok,
        "ok": (
            all(item.get("ok") for item in results if item.get("cmd") != "touch test")
            and (identity_ok is not False)
        ),
        "results": results,
    }
    if persistence is not None:
        report["persistence"] = persistence
        report["ok"] = report["ok"] and persistence.get("ok", False)
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--list-commands", action="store_true")
    parser.add_argument("--manual-touch", action="store_true")
    parser.add_argument("--persistence-test", action="store_true")
    parser.add_argument("--expected-firmware-commit", default=None)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    if args.list_commands:
        for command in SMOKE_COMMANDS:
            print(command)
        return 0

    if args.expected_firmware_commit is not None and exact_commit(args.expected_firmware_commit) is None:
        parser.error("--expected-firmware-commit must be an exact 40-character hexadecimal SHA")

    if args.dry_run:
        report = dry_run_report(args.expected_firmware_commit)
    else:
        if not args.port:
            parser.error("No D1L port supplied. Set D1L_PORT or pass --port.")
        report = run_serial_smoke(
            args.port,
            args.baud,
            args.timeout,
            args.manual_touch,
            args.persistence_test,
            args.expected_firmware_commit,
        )

    root = Path(__file__).resolve().parents[1]
    if args.out:
        out_path = Path(args.out)
        if not out_path.is_absolute():
            out_path = root / out_path
    else:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        out_path = root / "artifacts" / "smoke" / f"d1l-smoke-{stamp}.json"
    stamp_report(report, root)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2), encoding="ascii")
    print(json.dumps(report))
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
