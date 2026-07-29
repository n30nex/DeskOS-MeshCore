#!/usr/bin/env python3
"""Read-only, exact-package post-install check for DeskOS D1L RC1."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any


EXPECTED_PROJECT = "MeshCore DeskOS D1L"
EXPECTED_PROFILE = "core_1_0"
EXPECTED_IDF = "v5.5.4"
EXPECTED_VID = 0x1A86
EXPECTED_PID = 0x7523
EXPECTED_COMMANDS = ("version", "health", "rp2040 ping", "storage status")
_CHECKSUM_ROW = re.compile(r"([0-9A-Fa-f]{64})  \./(.+)\Z")
_COM_PORT = re.compile(r"COM[1-9][0-9]*\Z", re.IGNORECASE)
_COMMIT = re.compile(r"[0-9a-f]{40}\Z")
_POSIX_TTY = re.compile(r"/dev/ttyUSB[0-9]+\Z")


class Rc1TestError(RuntimeError):
    """Raised when a fail-closed package or hardware check fails."""


def _is_link_or_reparse(path: Path) -> bool:
    try:
        info = path.stat(follow_symlinks=False)
    except OSError:
        return True
    attributes = getattr(info, "st_file_attributes", 0)
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    return path.is_symlink() or bool(attributes & reparse_flag)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _checked_package_file(root: Path, relative: str) -> Path:
    if (
        not relative
        or "\\" in relative
        or any(ord(char) < 0x20 or ord(char) == 0x7F for char in relative)
    ):
        raise Rc1TestError(f"unsafe checksum path: {relative!r}")
    posix_path = PurePosixPath(relative)
    if posix_path.is_absolute() or any(
        part in {"", ".", ".."} for part in posix_path.parts
    ):
        raise Rc1TestError(f"unsafe checksum path: {relative!r}")

    cursor = root
    for part in posix_path.parts:
        cursor /= part
        if _is_link_or_reparse(cursor):
            raise Rc1TestError(f"linked package path rejected: {relative}")
    try:
        resolved = cursor.resolve(strict=True)
        resolved.relative_to(root)
    except (OSError, ValueError) as exc:
        raise Rc1TestError(f"checksum path escapes package: {relative}") from exc
    if not resolved.is_file():
        raise Rc1TestError(f"missing checksummed file: {relative}")
    return resolved


def verify_complete_package(package_root: Path) -> dict[str, Any]:
    try:
        root = package_root.resolve(strict=True)
    except OSError as exc:
        raise Rc1TestError("release package directory is missing") from exc
    if _is_link_or_reparse(root):
        raise Rc1TestError("linked release package directory rejected")

    sums_path = root / "SHA256SUMS.txt"
    if (
        not sums_path.is_file()
        or _is_link_or_reparse(sums_path)
        or sums_path.stat().st_size <= 0
    ):
        raise Rc1TestError("missing or linked SHA256SUMS.txt")

    expected: dict[str, tuple[str, str]] = {}
    for raw_line in sums_path.read_text(encoding="ascii").splitlines():
        match = _CHECKSUM_ROW.fullmatch(raw_line)
        if match is None:
            raise Rc1TestError(f"invalid SHA256SUMS.txt row: {raw_line}")
        digest, relative = match.groups()
        folded = relative.casefold()
        if folded in expected:
            raise Rc1TestError(f"duplicate checksum path: {relative}")
        target = _checked_package_file(root, relative)
        actual = sha256_file(target)
        if actual != digest.lower():
            raise Rc1TestError(f"SHA256 mismatch: {relative}")
        expected[folded] = (relative, actual)

    actual_paths: dict[str, str] = {}
    for candidate in sorted(root.rglob("*")):
        if _is_link_or_reparse(candidate):
            raise Rc1TestError(f"linked package entry rejected: {candidate}")
        if not candidate.is_file() or candidate == sums_path:
            continue
        relative = candidate.relative_to(root).as_posix()
        folded = relative.casefold()
        if folded in actual_paths:
            raise Rc1TestError(f"ambiguous package path: {relative}")
        actual_paths[folded] = relative
    if not expected or set(expected) != set(actual_paths):
        unchecksummed = sorted(set(actual_paths) - set(expected))
        missing = sorted(set(expected) - set(actual_paths))
        raise Rc1TestError(
            "SHA256SUMS.txt is not a complete package inventory "
            f"(unchecksummed={unchecksummed}, missing={missing})"
        )
    for folded, (relative, _digest) in expected.items():
        if actual_paths[folded] != relative:
            raise Rc1TestError(f"checksum path spelling mismatch: {relative}")
    return {
        "ok": True,
        "package_root": str(root),
        "checksummed_files": len(expected),
    }


def load_package_identity(package_root: Path) -> dict[str, str]:
    try:
        value = json.loads(
            (package_root / "manifest.json").read_text(encoding="ascii")
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise Rc1TestError("manifest.json is missing or invalid") from exc
    if not isinstance(value, dict) or value.get("project") != EXPECTED_PROJECT:
        raise Rc1TestError("manifest is not a DeskOS D1L package")
    commit = value.get("firmware_commit")
    if not isinstance(commit, str) or _COMMIT.fullmatch(commit) is None:
        raise Rc1TestError("manifest firmware commit is invalid")
    profile = value.get("release_profile")
    if profile != EXPECTED_PROFILE:
        raise Rc1TestError(f"unexpected release profile: {profile!r}")
    sd_mode = value.get("sd_history_mode")
    if sd_mode not in {"conditional", "supported_optional"}:
        raise Rc1TestError(f"release package does not include RC1 SD support: {sd_mode!r}")
    run_id = str(value.get("actions_run", ""))
    run_attempt = str(value.get("actions_run_attempt", ""))
    if not run_id.isdigit() or not run_attempt.isdigit():
        raise Rc1TestError("manifest GitHub Actions identity is invalid")
    return {
        "commit": commit,
        "profile": profile,
        "sd_history_mode": sd_mode,
        "actions_run": run_id,
        "actions_run_attempt": run_attempt,
    }


def _row_value(row: object, field: str) -> Any:
    if isinstance(row, dict):
        return row.get(field)
    return getattr(row, field, None)


def _listed_ports() -> list[Any]:
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise Rc1TestError(
            "pyserial is required; install it with: python -m pip install pyserial"
        ) from exc
    try:
        return list(list_ports.comports(include_links=True))
    except TypeError:
        return list(list_ports.comports())


def validate_explicit_port(
    requested_port: str,
    *,
    platform_name: str | None = None,
    rows: list[Any] | None = None,
) -> dict[str, Any]:
    if not isinstance(requested_port, str) or not requested_port.strip():
        raise Rc1TestError("pass the D1L serial port explicitly")
    if any(
        ord(char) < 0x20 or ord(char) == 0x7F for char in requested_port
    ):
        raise Rc1TestError("serial port contains a control character")

    platform_value = os.name if platform_name is None else platform_name
    windows = platform_value.strip().lower() in {"nt", "win32", "windows"}
    listed = _listed_ports() if rows is None else list(rows)
    if windows:
        canonical = requested_port.strip().upper()
        if canonical.startswith("\\\\.\\"):
            canonical = canonical[4:]
        if _COM_PORT.fullmatch(canonical) is None:
            raise Rc1TestError(
                "Windows requires one explicit operator-confirmed COM port"
            )
        matches = [
            row
            for row in listed
            if str(_row_value(row, "device") or "").strip().upper() == canonical
        ]
        target_kind = "windows_com_operator_supplied"
        resolved = canonical
    else:
        canonical = requested_port.strip()
        path = Path(canonical)
        if not canonical.startswith("/dev/serial/by-id/") or not path.is_symlink():
            raise Rc1TestError(
                "Linux requires an explicit /dev/serial/by-id/ symlink; "
                "raw /dev/ttyUSB paths are rejected"
            )
        try:
            resolved = str(path.resolve(strict=True))
        except OSError as exc:
            raise Rc1TestError("D1L by-id path is missing or dangling") from exc
        if _POSIX_TTY.fullmatch(resolved) is None:
            raise Rc1TestError("D1L by-id path does not resolve to a USB serial tty")
        matches = []
        for row in listed:
            device = str(_row_value(row, "device") or "")
            if device == canonical:
                matches.append(row)
                continue
            try:
                if str(Path(device).resolve(strict=True)) == resolved:
                    matches.append(row)
            except OSError:
                continue
        target_kind = "posix_by_id_operator_supplied"

    if not matches:
        raise Rc1TestError("the explicitly selected D1L serial port was not found")
    identities = {
        (_row_value(row, "vid"), _row_value(row, "pid")) for row in matches
    }
    if identities != {(EXPECTED_VID, EXPECTED_PID)}:
        raise Rc1TestError(
            "selected port is not the D1L USB device "
            f"{EXPECTED_VID:04X}:{EXPECTED_PID:04X}; found {sorted(identities)!r}"
        )
    return {
        "requested_path": canonical,
        "resolved_tty": resolved,
        "target_kind": target_kind,
        "vid": EXPECTED_VID,
        "pid": EXPECTED_PID,
        "matching_rows": len(matches),
    }


def _expected_command_name(command: str) -> str:
    return command


def _read_command_result(serial_handle: Any, command: str, timeout: float) -> dict:
    serial_handle.write((command + "\n").encode("ascii"))
    serial_handle.flush()
    deadline = time.monotonic() + timeout
    expected = _expected_command_name(command)
    ignored = 0
    while time.monotonic() < deadline:
        raw = serial_handle.readline()
        if not raw:
            continue
        try:
            value = json.loads(raw.decode("utf-8", errors="replace").strip())
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and value.get("cmd") == expected:
            return value
        if isinstance(value, dict):
            ignored += 1
    return {
        "schema": 1,
        "ok": False,
        "cmd": expected,
        "code": "TIMEOUT",
        "ignored_json_count": ignored,
    }


def run_read_only_checks(
    port: str,
    *,
    baud: int = 115200,
    timeout: float = 10.0,
) -> dict[str, dict]:
    try:
        import serial
    except ImportError as exc:
        raise Rc1TestError(
            "pyserial is required; install it with: python -m pip install pyserial"
        ) from exc

    handle = serial.Serial(port=None, baudrate=baud, timeout=1.0)
    try:
        # pySerial applies DTR before RTS on open.  Preserve the ESP32-safe
        # asserted-DTR/deasserted-RTS intermediate state, then release DTR
        # only after the port is open to avoid the observed CH340 reset pulse.
        handle.dtr = True
        handle.rts = False
        handle.port = port
        handle.open()
        handle.dtr = False
        time.sleep(2.0)
        handle.reset_input_buffer()
        results: dict[str, dict] = {}
        for command in EXPECTED_COMMANDS:
            attempts = 3 if command in {"health", "storage status"} else 2
            result: dict = {}
            for attempt in range(attempts):
                result = _read_command_result(handle, command, timeout)
                if result.get("ok") is True:
                    break
                if attempt + 1 < attempts:
                    time.sleep(1.0)
                    handle.reset_input_buffer()
            results[command] = result
        return results
    except (OSError, ValueError) as exc:
        raise Rc1TestError(f"could not query the D1L console: {exc}") from exc
    finally:
        handle.close()


def evaluate_results(
    results: dict[str, dict],
    identity: dict[str, str],
) -> dict[str, bool]:
    commit = identity["commit"]
    sd_mode = identity["sd_history_mode"]
    version = results.get("version", {})
    health = results.get("health", {})
    rp2040 = results.get("rp2040 ping", {})
    storage = results.get("storage status", {})
    sd = storage.get("sd") if isinstance(storage.get("sd"), dict) else {}

    exact_version = (
        version.get("schema") == 1
        and version.get("ok") is True
        and version.get("cmd") == "version"
        and version.get("build_commit") == commit
        and version.get("release_profile") == EXPECTED_PROFILE
        and version.get("sd_history_mode") == sd_mode
        and version.get("idf") == EXPECTED_IDF
    )
    health_ready = (
        health.get("schema") == 1
        and health.get("ok") is True
        and health.get("cmd") == "health"
        and health.get("build_commit") == commit
        and health.get("release_profile") == EXPECTED_PROFILE
        and health.get("sd_history_mode") == sd_mode
        and health.get("board_ready") is True
        and health.get("ui_ready") is True
    )
    rp2040_ready = (
        rp2040.get("schema") == 1
        and rp2040.get("ok") is True
        and rp2040.get("cmd") == "rp2040 ping"
        and rp2040.get("bridge_ready") is True
        and rp2040.get("protocol_supported") is True
        and rp2040.get("formats_sd") is False
        and rp2040.get("public_rf_tx") is False
    )
    sd_ready = (
        storage.get("schema") == 1
        and storage.get("ok") is True
        and storage.get("cmd") == "storage status"
        and storage.get("build_commit") == commit
        and storage.get("release_profile") == EXPECTED_PROFILE
        and storage.get("sd_history_mode") == sd_mode
        and storage.get("data_enabled") is True
        and storage.get("data_backend") == "sd"
        and sd.get("present") is True
        and sd.get("mounted") is True
        and sd.get("filesystem") == "fat32"
        and sd.get("data_root_ready") is True
        and sd.get("rp2040_bridge_ready") is True
        and sd.get("rp2040_protocol_supported") is True
        and sd.get("state") == "ready"
    )
    return {
        "exact_firmware": exact_version,
        "esp32_board_and_ui_ready": health_ready,
        "rp2040_bridge_ready": rp2040_ready,
        "prepared_sd_card_ready": sd_ready,
    }


def _report_path(value: str | None, package_root: Path) -> Path:
    path = (
        Path(value)
        if value
        else Path(tempfile.gettempdir()) / "sigui-rc1-test-report.json"
    )
    path = path.expanduser().resolve()
    try:
        path.relative_to(package_root)
    except ValueError:
        return path
    raise Rc1TestError("write the test report outside the release package directory")


def _write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify the exact RC1 package and run read-only D1L checks."
    )
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--out")
    args = parser.parse_args(argv)

    package_root = Path(__file__).resolve().parents[1]
    try:
        package_verification = verify_complete_package(package_root)
        identity = load_package_identity(package_root)
        report: dict[str, Any] = {
            "schema": 1,
            "kind": "d1l_rc1_end_user_test",
            "created_at": datetime.now(timezone.utc).isoformat(),
            "mode": "verify-only" if args.verify_only else "hardware",
            "ok": True,
            "package_verification": package_verification,
            "identity": identity,
            "public_rf_tx": False,
            "formats_sd": False,
        }
        if not args.verify_only:
            if not args.port:
                parser.error("--port is required unless --verify-only is used")
            target = validate_explicit_port(args.port)
            results = run_read_only_checks(
                target["requested_path"],
                baud=args.baud,
                timeout=args.timeout,
            )
            checks = evaluate_results(results, identity)
            report.update(
                {
                    "target": target,
                    "commands": list(EXPECTED_COMMANDS),
                    "results": results,
                    "checks": checks,
                    "ok": all(checks.values()),
                }
            )
        out_path = _report_path(args.out, package_root)
        _write_report(out_path, report)
    except (OSError, Rc1TestError, ValueError) as exc:
        print(f"RC1 TEST FAILED: {exc}")
        return 1

    if args.verify_only:
        print(
            "RC1 PACKAGE VERIFIED: "
            f"commit={identity['commit']} "
            f"run={identity['actions_run']}/{identity['actions_run_attempt']}"
        )
    else:
        for name, passed in report["checks"].items():
            print(f"{'PASS' if passed else 'FAIL'}: {name}")
        print(f"Report: {out_path}")
        if report["ok"]:
            print("RC1 AUTOMATED TEST PASSED")
        else:
            print("RC1 AUTOMATED TEST FAILED")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
