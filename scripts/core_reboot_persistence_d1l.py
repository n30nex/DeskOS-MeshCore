#!/usr/bin/env python3
"""Produce fail-closed Core 1.0 reboot and retained-state evidence on a D1L.

The producer deliberately separates two phases:

* ``seed`` runs on the exact candidate, proves a stable witness-only snapshot
  of already-full retained Public/DM/contact stores, writes one settings
  retention marker only after that read-only proof passes, and captures the
  resulting Core state.
* ``verify`` requires the seed plus the exact-candidate non-erasing reflash
  receipt, then executes five software reboots and three operator-controlled
  cold power cycles.

Every console command is retained as hash-bound raw serial bytes.  The final
matrix references immutable child receipts and is only accepted after the
validator recomputes identity, state, reboot, crashlog, and USB-port facts from
those raw fields.  This proves a same-exact-candidate non-erasing reinstall; it
does not claim cross-version migration.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable

try:
    from artifact_metadata import git_metadata
    from d1l_serial_target import (
        EXPECTED_PID,
        EXPECTED_VID,
        POSIX_D1L_TARGET,
        WINDOWS_D1L_TARGET,
        resolve_target,
        safe_slug,
        validate_snapshot,
    )
    from smoke_d1l import (
        exact_commit,
        expected_command_name,
        open_d1l_serial,
        parse_jsonl_line,
    )
    from verify_checksums import is_link_or_reparse, sha256_file
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import git_metadata
    from scripts.d1l_serial_target import (
        EXPECTED_PID,
        EXPECTED_VID,
        POSIX_D1L_TARGET,
        WINDOWS_D1L_TARGET,
        resolve_target,
        safe_slug,
        validate_snapshot,
    )
    from scripts.smoke_d1l import (
        exact_commit,
        expected_command_name,
        open_d1l_serial,
        parse_jsonl_line,
    )
    from scripts.verify_checksums import is_link_or_reparse, sha256_file


ROOT = Path(__file__).resolve().parents[1]
D1L_CORE_PORT = WINDOWS_D1L_TARGET
D1L_CORE_POSIX_TARGET = POSIX_D1L_TARGET
D1L_BAUD = 115200
CORE_RELEASE_PROFILE = "core_1_0"
SD_HISTORY_MODE = "disabled"
EXPECTED_IDF_VERSION = "v5.5.4"
SOFTWARE_CYCLE_COUNT = 5
COLD_CYCLE_COUNT = 3
MINIMUM_POWER_OFF_SEC = 2.0
FORBIDDEN_PORTS = frozenset({"COM8", "COM11", "COM16", "COM29"})
STATE_BASE_COMMANDS = (
    "version",
    "health",
    "crashlog",
    "settings get",
)
STATE_TAIL_COMMANDS = (
    "messages unread",
    "contacts",
)
MAX_RAW_LINE_BYTES = 131072
MAX_PAGES = 32
CLAIM = "same_exact_candidate_non_erasing_reinstall"
CORE_WITNESS_TOKEN_DOMAIN = "sigui-core-retained-witness:"
CORE_WITNESS_LABEL_PREFIX = "core-retained-witness "

RESET_RE = re.compile(
    r"(?:^|\s)rst:0x(?P<code>[0-9a-f]+)\s*\((?P<reason>[^)]+)\)",
    re.IGNORECASE,
)
ROM_RE = re.compile(r"(?:ESP-ROM:|^ets\s)", re.IGNORECASE)
BOOT_RE = re.compile(r"(?:^|[,\s])boot:0x[0-9a-f]+", re.IGNORECASE)
CRASH_RE = re.compile(r"(?:WDT|PANIC|BROWNOUT)", re.IGNORECASE)
SOFTWARE_SYSTEM_RESET = (0x03, "RTC_SW_SYS_RST")
COLD_RESET_REASONS = frozenset(
    {
        "POWERON",
        "POWERON_RESET",
        "RTC_POWERON",
        "RTC_POWERON_RESET",
    }
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def canonical_json(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def positive_decimal(value: object) -> bool:
    text = str(value)
    return text.isdigit() and int(text) > 0


def candidate_witness_identity(
    commit: str,
    run_id: str,
    run_attempt: str,
) -> dict[str, Any]:
    commit = exact_commit(commit) or ""
    if (
        not commit
        or not positive_decimal(run_id)
        or not positive_decimal(run_attempt)
    ):
        raise ValueError("candidate witness identity is invalid")
    identity = canonical_json(
        {
            "commit": commit,
            "github_actions_run": str(run_id),
            "workflow_run_attempt": str(run_attempt),
        }
    )
    token = f"core-{sha256_bytes(identity)[:24]}"
    return {
        "settings_node_name": f"D1L-Core-{commit[:7]}",
        "token": token,
        "witness_request_label": f"{CORE_WITNESS_LABEL_PREFIX}{token}",
    }


def normalize_port(value: object) -> str | None:
    if not isinstance(value, str) or not value.strip():
        return None
    if value == D1L_CORE_POSIX_TARGET:
        return D1L_CORE_POSIX_TARGET
    normalized = value.strip().upper().replace("/", "\\")
    for prefix in ("\\\\.\\", "\\\\?\\"):
        if normalized.startswith(prefix):
            normalized = normalized[len(prefix) :]
            break
    return normalized


def enforce_core_port(value: object) -> str:
    port = normalize_port(value)
    if port not in {D1L_CORE_PORT, D1L_CORE_POSIX_TARGET}:
        raise ValueError(
            "Core reboot/persistence validation requires COM12 or the exact "
            f"{D1L_CORE_POSIX_TARGET} by-id target; got "
            f"{port or '<missing>'}"
        )
    if port in FORBIDDEN_PORTS:
        raise ValueError(f"refusing forbidden port {port}")
    return port


def resolve_core_target(
    value: object,
    *,
    port_lister: Callable[[], Iterable[object]],
    platform_name: str | None = None,
) -> dict[str, Any]:
    """Resolve the selected D1L before any serial or reboot operation."""

    requested = enforce_core_port(value)
    return resolve_target(
        requested,
        port_lister=port_lister,
        platform_name=platform_name,
    )


def exact_public_key(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    return (
        normalized
        if re.fullmatch(r"[0-9a-f]{64}", normalized)
        else None
    )


def identity_status_ok(result: object, expected_public_key: str) -> bool:
    public_key = exact_public_key(expected_public_key)
    return (
        public_key is not None
        and isinstance(result, dict)
        and result.get("schema") == 1
        and result.get("ok") is True
        and result.get("cmd") == "identity status"
        and result.get("public_key_ready") is True
        and exact_public_key(result.get("public_key")) == public_key
        and result.get("fingerprint") == public_key[:16].upper()
        and result.get("role") == "desk_companion"
    )


def target_identity(
    snapshot: object,
    expected_target: str,
) -> str | None:
    try:
        validate_snapshot(snapshot, expected_target)
    except ValueError:
        return None
    assert isinstance(snapshot, dict)
    identity = snapshot.get("stable_identity_sha256")
    return identity if isinstance(identity, str) else None


def target_continuity(
    before: object,
    after: object,
    expected_target: str,
) -> bool:
    before_identity = target_identity(before, expected_target)
    after_identity = target_identity(after, expected_target)
    return (
        before_identity is not None
        and before_identity == after_identity
    )


def exact_source_git(
    root: Path,
    commit: str,
    *,
    metadata: dict[str, Any] | None = None,
) -> dict[str, Any]:
    source = dict(metadata if metadata is not None else git_metadata(root))
    if not (
        exact_commit(source.get("commit")) == commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    ):
        raise ValueError(
            "producer source must be the exact clean candidate commit"
        )
    return source


def _lexical_absolute(path: Path) -> Path:
    """Return an absolute path without resolving links or reparse points."""
    return Path(os.path.abspath(os.fspath(path)))


def _reject_reparse_chain(
    root: Path,
    path: Path,
    label: str,
    *,
    include_leaf: bool,
) -> None:
    cursor = path if include_leaf else path.parent
    while True:
        if os.path.lexists(os.fspath(cursor)) and is_link_or_reparse(cursor):
            relation = "output" if cursor == path else "parent"
            raise ValueError(
                f"{label} {relation} cannot be a link/reparse point: {cursor}"
            )
        if cursor == root:
            return
        parent = cursor.parent
        if parent == cursor:
            raise ValueError(f"{label} must stay inside {root}")
        cursor = parent


def _inside_existing(root: Path, path: Path, label: str) -> Path:
    resolved_root = root.resolve(strict=True)
    lexical = _lexical_absolute(path)
    try:
        lexical.relative_to(resolved_root)
    except ValueError as exc:
        raise ValueError(f"{label} must stay inside {root}") from exc
    _reject_reparse_chain(
        resolved_root, lexical, label, include_leaf=True
    )
    try:
        resolved = lexical.resolve(strict=True)
        resolved.relative_to(resolved_root)
    except (OSError, RuntimeError, ValueError) as exc:
        raise ValueError(f"{label} must stay inside {root}") from exc
    return resolved


def _inside_output(root: Path, path: Path, label: str) -> Path:
    resolved_root = root.resolve(strict=True)
    lexical = _lexical_absolute(path)
    try:
        lexical.relative_to(resolved_root)
    except ValueError as exc:
        raise ValueError(f"{label} must stay inside {root}") from exc
    _reject_reparse_chain(
        resolved_root, lexical, label, include_leaf=True
    )
    return lexical


def load_json(path: Path, root: Path, label: str) -> dict[str, Any]:
    resolved = _inside_existing(root, path, label)
    if not resolved.is_file():
        raise ValueError(f"{label} is not a file: {path}")
    try:
        value = json.loads(resolved.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"{label} is invalid JSON: {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be a JSON object: {path}")
    return value


def relative_file_row(path: Path, root: Path, label: str) -> dict[str, Any]:
    resolved = _inside_existing(root, path, label)
    return {
        "path": resolved.relative_to(root.resolve()).as_posix(),
        "size": resolved.stat().st_size,
        "sha256": sha256_file(resolved),
    }


def validate_file_row(
    row: object, root: Path, label: str
) -> tuple[Path | None, list[str]]:
    errors: list[str] = []
    if not isinstance(row, dict):
        return None, [f"{label}: missing file row"]
    raw_path = row.get("path")
    if not isinstance(raw_path, str) or not raw_path:
        return None, [f"{label}: missing path"]
    try:
        path = _inside_existing(root, root / raw_path, label)
    except ValueError as exc:
        return None, [str(exc)]
    expected_size = row.get("size")
    expected_digest = row.get("sha256")
    if type(expected_size) is not int or expected_size != path.stat().st_size:
        errors.append(f"{label}: size mismatch")
    if (
        not isinstance(expected_digest, str)
        or not re.fullmatch(r"[0-9a-f]{64}", expected_digest)
        or expected_digest != sha256_file(path)
    ):
        errors.append(f"{label}: sha256 mismatch")
    return path, errors


def reserve_json_output(path: Path, root: Path) -> tuple[Path, Any]:
    lexical = _inside_output(root, path, "evidence output")
    lexical.parent.mkdir(parents=True, exist_ok=True)
    lexical = _inside_output(root, lexical, "evidence output")
    resolved_parent = lexical.parent.resolve(strict=True)
    try:
        resolved_parent.relative_to(root.resolve(strict=True))
    except ValueError as exc:
        raise ValueError("evidence output parent escaped the root") from exc
    try:
        handle = lexical.open("x", encoding="ascii", newline="\n")
    except FileExistsError as exc:
        raise ValueError(f"refusing to overwrite evidence: {lexical}") from exc
    try:
        reserved = _inside_existing(root, lexical, "evidence output reservation")
        if reserved != lexical:
            raise ValueError("evidence output reservation changed path identity")
    except BaseException:
        handle.close()
        raise
    return lexical, handle


def finalize_json_output(handle: Any, value: dict[str, Any]) -> None:
    try:
        handle.write(
            json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
        )
        handle.flush()
        os.fsync(handle.fileno())
    finally:
        handle.close()


def write_json_once(path: Path, root: Path, value: dict[str, Any]) -> None:
    _, handle = reserve_json_output(path, root)
    finalize_json_output(handle, value)


def reserve_reboot_outputs(
    out: Path,
    root: Path,
) -> tuple[Path, Any, dict[tuple[str, int], tuple[Path, Any]]]:
    final_path, final_handle = reserve_json_output(out, root)
    reservations: dict[tuple[str, int], tuple[Path, Any]] = {}
    try:
        child_dir = _inside_output(
            root,
            final_path.with_suffix("").with_name(final_path.stem + "_cycles"),
            "cycle directory",
        )
        child_dir.mkdir(parents=True, exist_ok=False)
        child_dir = _inside_output(root, child_dir, "cycle directory")
        if not child_dir.is_dir():
            raise ValueError("cycle directory reservation failed")
        for cycle_type, count in (
            ("software", SOFTWARE_CYCLE_COUNT),
            ("cold", COLD_CYCLE_COUNT),
        ):
            for ordinal in range(1, count + 1):
                path, handle = reserve_json_output(
                    child_dir / f"{cycle_type}_{ordinal}.json",
                    root,
                )
                reservations[(cycle_type, ordinal)] = (path, handle)
    except BaseException:
        if not final_handle.closed:
            final_handle.close()
        for _, handle in reservations.values():
            if not handle.closed:
                handle.close()
        raise
    return final_path, final_handle, reservations


def _unused_cycle_reservation_receipt(
    *,
    cycle_type: str,
    ordinal: int,
    commit: str,
    run_id: str,
    run_attempt: str,
    reason: str,
    requested_target: str,
    d1l_target: dict[str, Any] | None,
) -> dict[str, Any]:
    return {
        "schema": 2,
        "kind": "core_reboot_persistence_cycle_reservation",
        "mode": "hardware",
        "ok": False,
        "closure_eligible": False,
        "hardware_required": True,
        "physical_observed": False,
        "cycle_type": cycle_type,
        "ordinal": ordinal,
        "port": requested_target,
        "d1l_target": d1l_target,
        "commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": SD_HISTORY_MODE,
        "execution_state": "not_executed",
        "reason": reason,
        "physical_state_outcome_uncertain": False,
        "mutation_outcome_uncertain": False,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "predecessor_evidence_used": False,
    }


def finalize_unused_cycle_reservations(
    reservations: dict[tuple[str, int], tuple[Path, Any]],
    *,
    commit: str,
    run_id: str,
    run_attempt: str,
    reason: str,
    requested_target: str,
    d1l_target: dict[str, Any] | None,
) -> None:
    first_error: BaseException | None = None
    for (cycle_type, ordinal), (_, handle) in reservations.items():
        if handle.closed:
            continue
        try:
            finalize_json_output(
                handle,
                _unused_cycle_reservation_receipt(
                    cycle_type=cycle_type,
                    ordinal=ordinal,
                    commit=commit,
                    run_id=run_id,
                    run_attempt=run_attempt,
                    reason=reason,
                    requested_target=requested_target,
                    d1l_target=d1l_target,
                ),
            )
        except BaseException as exc:
            if first_error is None:
                first_error = exc
    if first_error is not None:
        raise first_error


def _raw_line(raw: bytes, observed_at: str) -> dict[str, Any]:
    return {
        "observed_at": observed_at,
        "size": len(raw),
        "sha256": sha256_bytes(raw),
        "base64": base64.b64encode(raw).decode("ascii"),
    }


def expected_raw_command_name(command: str) -> str:
    """Return the response ``cmd`` emitted for one exact console request."""
    if (
        command == "messages public"
        or command.startswith("messages public offset ")
    ):
        return "messages public"
    if command.startswith("core retained-witness "):
        return "core retained-witness"
    return expected_command_name(command)


def decode_raw_line(row: object) -> tuple[bytes | None, list[str]]:
    errors: list[str] = []
    if not isinstance(row, dict):
        return None, ["raw line is not an object"]
    encoded = row.get("base64")
    try:
        raw = base64.b64decode(encoded, validate=True)
    except (TypeError, ValueError):
        return None, ["raw line base64 is invalid"]
    if type(row.get("size")) is not int or row.get("size") != len(raw):
        errors.append("raw line size mismatch")
    digest = row.get("sha256")
    if (
        not isinstance(digest, str)
        or digest != sha256_bytes(raw)
    ):
        errors.append("raw line sha256 mismatch")
    if not isinstance(row.get("observed_at"), str):
        errors.append("raw line observed_at is missing")
    return raw, errors


def read_raw_command(
    ser: Any,
    command: str,
    timeout: float,
    *,
    clock: Callable[[], float] = time.monotonic,
    now: Callable[[], str] = utc_now,
    command_log: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    expected = expected_raw_command_name(command)
    started_at = now()
    lines: list[dict[str, Any]] = []
    result: dict[str, Any] | None = None
    receipt: dict[str, Any] = {
        "command": command,
        "expected_cmd": expected,
        "started_at": started_at,
        "ended_at": None,
        "write_attempted": False,
        "raw_lines": lines,
        "result": None,
    }
    if command_log is not None:
        command_log.append(receipt)
    original_timeout = getattr(ser, "timeout", None)
    try:
        receipt["write_attempted"] = True
        ser.write((command + "\n").encode("utf-8"))
        if hasattr(ser, "flush"):
            ser.flush()
        deadline = clock() + timeout
        while clock() < deadline:
            remaining = max(0.001, deadline - clock())
            if hasattr(ser, "timeout"):
                ser.timeout = min(0.25, remaining)
            raw = ser.readline(MAX_RAW_LINE_BYTES + 1)
            if not raw:
                continue
            lines.append(_raw_line(raw, now()))
            if len(raw) > MAX_RAW_LINE_BYTES:
                break
            try:
                text = raw.decode("utf-8", errors="strict")
            except UnicodeDecodeError:
                continue
            parsed = parse_jsonl_line(text)
            if parsed is not None and parsed.get("cmd") == expected:
                result = parsed
                break
    finally:
        receipt["ended_at"] = now()
        if hasattr(ser, "timeout"):
            ser.timeout = original_timeout
    if result is None:
        result = {
            "schema": 1,
            "ok": False,
            "cmd": expected,
            "code": "TIMEOUT_OR_INVALID_RAW",
        }
    receipt["result"] = result
    return receipt


def recompute_raw_command(row: object) -> tuple[dict[str, Any] | None, list[str]]:
    errors: list[str] = []
    if not isinstance(row, dict):
        return None, ["command receipt is not an object"]
    command = row.get("command")
    expected = row.get("expected_cmd")
    if not isinstance(command, str) or not command:
        errors.append("command receipt command is missing")
        return None, errors
    if expected != expected_raw_command_name(command):
        errors.append(f"{command}: expected command mismatch")
    raw_lines = row.get("raw_lines")
    if not isinstance(raw_lines, list) or not raw_lines:
        errors.append(f"{command}: raw lines are missing")
        return None, errors
    matched: list[dict[str, Any]] = []
    for index, raw_row in enumerate(raw_lines):
        raw, raw_errors = decode_raw_line(raw_row)
        errors.extend(f"{command} raw[{index}]: {error}" for error in raw_errors)
        if raw is None or len(raw) > MAX_RAW_LINE_BYTES:
            if raw is not None:
                errors.append(f"{command} raw[{index}]: line exceeds limit")
            continue
        try:
            parsed = parse_jsonl_line(raw.decode("utf-8", errors="strict"))
        except UnicodeDecodeError:
            parsed = None
        if parsed is not None and parsed.get("cmd") == expected:
            matched.append(parsed)
    if len(matched) != 1:
        errors.append(f"{command}: expected exactly one matching raw result")
        return None, errors
    if row.get("result") != matched[0]:
        errors.append(f"{command}: parsed result does not match raw result")
    return matched[0], errors


def capture_boot_lines(
    ser: Any,
    timeout: float,
    *,
    clock: Callable[[], float] = time.monotonic,
    now: Callable[[], str] = utc_now,
) -> list[dict[str, Any]]:
    deadline = clock() + timeout
    lines: list[dict[str, Any]] = []
    original_timeout = getattr(ser, "timeout", None)
    try:
        while clock() < deadline:
            remaining = max(0.001, deadline - clock())
            if hasattr(ser, "timeout"):
                ser.timeout = min(0.25, remaining)
            raw = ser.readline(MAX_RAW_LINE_BYTES + 1)
            if not raw:
                continue
            lines.append(_raw_line(raw, now()))
            if len(raw) > MAX_RAW_LINE_BYTES:
                break
            try:
                parsed = parse_jsonl_line(raw.decode("utf-8", errors="strict"))
            except UnicodeDecodeError:
                parsed = None
            if parsed is not None and parsed.get("cmd") == "help":
                break
    finally:
        if hasattr(ser, "timeout"):
            ser.timeout = original_timeout
    return lines


def analyze_boot_lines(rows: object) -> tuple[dict[str, Any], list[str]]:
    errors: list[str] = []
    analysis = {
        "rom_count": 0,
        "reset_count": 0,
        "boot_count": 0,
        "help_count": 0,
        "crash_marker_count": 0,
        "reset_events": [],
    }
    if not isinstance(rows, list):
        return analysis, ["boot raw lines are missing"]
    for index, row in enumerate(rows):
        raw, raw_errors = decode_raw_line(row)
        errors.extend(f"boot raw[{index}]: {error}" for error in raw_errors)
        if raw is None or len(raw) > MAX_RAW_LINE_BYTES:
            continue
        text = raw.decode("utf-8", errors="replace")
        if ROM_RE.search(text):
            analysis["rom_count"] += 1
        match = RESET_RE.search(text)
        if match is not None:
            analysis["reset_count"] += 1
            analysis["reset_events"].append(
                {
                    "code": int(match.group("code"), 16),
                    "reason": match.group("reason").strip().upper(),
                }
            )
        if BOOT_RE.search(text):
            analysis["boot_count"] += 1
        if CRASH_RE.search(text):
            analysis["crash_marker_count"] += 1
        try:
            parsed = parse_jsonl_line(raw.decode("utf-8", errors="strict"))
        except UnicodeDecodeError:
            parsed = None
        if parsed is not None and parsed.get("cmd") == "help":
            analysis["help_count"] += 1
    return analysis, errors


def _command_ok(result: object, command: str) -> bool:
    return (
        isinstance(result, dict)
        and result.get("schema") == 1
        and result.get("ok") is True
        and result.get("cmd") == command
    )


def exact_version(
    result: object, commit: str
) -> bool:
    return (
        _command_ok(result, "version")
        and isinstance(result, dict)
        and exact_commit(result.get("build_commit")) == commit
        and result.get("idf") == EXPECTED_IDF_VERSION
        and result.get("release_profile") == CORE_RELEASE_PROFILE
        and result.get("sd_history_mode") == SD_HISTORY_MODE
    )


def exact_health(result: object) -> bool:
    return (
        _command_ok(result, "health")
        and isinstance(result, dict)
        and result.get("release_profile") == CORE_RELEASE_PROFILE
        and result.get("sd_history_mode") == SD_HISTORY_MODE
        and result.get("board_ready") is True
        and result.get("ui_ready") is True
        and result.get("nvs_ready") is True
        and type(result.get("boot_nonce")) is int
        and result.get("boot_nonce") > 0
        and type(result.get("uptime_ms")) is int
        and result.get("uptime_ms") >= 0
    )


def _capture_pages(
    ser: Any,
    command: str,
    timeout: float,
    *,
    clock: Callable[[], float],
    now: Callable[[], str],
    command_log: list[dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    offset = 0
    for _ in range(MAX_PAGES):
        request = command if offset == 0 else f"{command} offset {offset}"
        row = read_raw_command(
            ser,
            request,
            timeout,
            clock=clock,
            now=now,
            command_log=command_log,
        )
        rows.append(row)
        result = row.get("result")
        if not _command_ok(result, command):
            break
        if result.get("has_older") is not True:
            break
        next_offset = result.get("next_offset")
        if (
            type(next_offset) is not int
            or next_offset <= offset
        ):
            break
        offset = next_offset
    return rows


def capture_state(
    ser: Any,
    timeout: float,
    *,
    clock: Callable[[], float] = time.monotonic,
    now: Callable[[], str] = utc_now,
    command_log: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    commands = [
        read_raw_command(
            ser,
            command,
            timeout,
            clock=clock,
            now=now,
            command_log=command_log,
        )
        for command in STATE_BASE_COMMANDS
    ]
    commands.extend(
        _capture_pages(
            ser,
            command,
            timeout,
            clock=clock,
            now=now,
            command_log=command_log,
        )
        for command in ("messages public", "messages dm")
    )
    flattened: list[dict[str, Any]] = []
    for row in commands:
        if isinstance(row, list):
            flattened.extend(row)
        else:
            flattened.append(row)
    flattened.extend(
        read_raw_command(
            ser,
            command,
            timeout,
            clock=clock,
            now=now,
            command_log=command_log,
        )
        for command in STATE_TAIL_COMMANDS
    )
    return {
        "captured_at": now(),
        "commands": flattened,
    }


def _clean_persistence(result: dict[str, Any]) -> bool:
    persistence = result.get("persistence")
    nvs = persistence.get("nvs") if isinstance(persistence, dict) else None
    return (
        isinstance(persistence, dict)
        and persistence.get("loaded") is True
        and persistence.get("dirty") is False
        and persistence.get("failures") == 0
        and isinstance(nvs, dict)
        and nvs.get("dirty") is False
        and nvs.get("failures") == 0
        and nvs.get("last_error") == "ESP_OK"
        and result.get("persisted") is True
    )


def _settings_projection(result: dict[str, Any]) -> dict[str, Any] | None:
    required_scalars = (
        "node_name",
        "role",
        "onboarding_complete",
        "wifi_enabled",
        "ble_companion_enabled",
        "observer_enabled",
        "high_contrast",
        "night_mode",
        "path_hash_bytes",
    )
    if not _command_ok(result, "settings get"):
        return None
    if not isinstance(result.get("node_name"), str) or not result["node_name"]:
        return None
    if any(key not in result for key in required_scalars):
        return None
    timezone_value = result.get("timezone")
    radio = result.get("radio")
    if not isinstance(timezone_value, dict) or not isinstance(radio, dict):
        return None
    return {
        key: result[key] for key in required_scalars
    } | {
        "timezone": timezone_value,
        "radio": radio,
    }


def _page_projection(
    command: str,
    pages: list[dict[str, Any]],
) -> tuple[dict[str, Any] | None, list[str]]:
    errors: list[str] = []
    if not pages:
        return None, [f"{command}: pages are missing"]
    entries: list[dict[str, Any]] = []
    offsets: list[int] = []
    first: dict[str, Any] | None = None
    stable_fields = (
        "count",
        "capacity",
        "total_written",
        "dropped_oldest",
        "total_matches",
        "retained_epoch",
        "content_revision",
        "volatile_preview_present",
        "volatile_preview_seq",
    )
    if command == "messages public":
        stable_fields += ("retained_store_count", "retained_public_count")
    else:
        stable_fields += ("retained_count",)
    for index, result in enumerate(pages):
        if not _command_ok(result, command):
            errors.append(f"{command}: page {index} failed")
            continue
        if result.get("filtered") is not False:
            errors.append(f"{command}: page {index} is unexpectedly filtered")
        if not _clean_persistence(result):
            errors.append(f"{command}: page {index} persistence is not clean")
        offset = result.get("offset")
        page_entries = result.get("entries")
        if type(offset) is not int or offset < 0:
            errors.append(f"{command}: page {index} offset is invalid")
            continue
        if (
            not isinstance(page_entries, list)
            or not all(isinstance(entry, dict) for entry in page_entries)
            or result.get("page_count") != len(page_entries)
        ):
            errors.append(f"{command}: page {index} entries are invalid")
            continue
        if first is None:
            first = result
        elif any(result.get(key) != first.get(key) for key in stable_fields):
            errors.append(f"{command}: page metadata changed during capture")
        offsets.append(offset)
        entries.extend(page_entries)
    if first is None:
        return None, errors
    expected_offsets: list[int] = []
    cursor = 0
    for result in pages:
        expected_offsets.append(cursor)
        if result.get("has_older") is True:
            next_offset = result.get("next_offset")
            if type(next_offset) is not int or next_offset <= cursor:
                errors.append(f"{command}: next_offset is invalid")
                break
            cursor = next_offset
        else:
            break
    if offsets != expected_offsets[: len(offsets)]:
        errors.append(f"{command}: pagination is not consecutive")
    if pages[-1].get("has_older") is not False:
        errors.append(f"{command}: capture is truncated")
    if first.get("total_matches") != len(entries):
        errors.append(f"{command}: total_matches does not equal captured entries")
    sequences = [
        entry.get("seq")
        for entry in entries
        if type(entry.get("seq")) is int
    ]
    if len(sequences) != len(entries) or len(set(sequences)) != len(sequences):
        errors.append(f"{command}: entry sequence values are invalid or duplicated")
    retained_entries: list[dict[str, Any]] = []
    volatile_entries: list[dict[str, Any]] = []
    for index, entry in enumerate(entries):
        retained = entry.get("retained")
        volatile_preview = entry.get("volatile_preview")
        if (
            type(retained) is not bool
            or type(volatile_preview) is not bool
            or retained == volatile_preview
        ):
            errors.append(
                f"{command}: entry {index} retained/volatile classification is invalid"
            )
            continue
        (retained_entries if retained else volatile_entries).append(entry)
    preview_present = first.get("volatile_preview_present")
    preview_seq = first.get("volatile_preview_seq")
    if type(preview_present) is not bool:
        errors.append(f"{command}: volatile preview presence is invalid")
    elif preview_present:
        if (
            len(volatile_entries) != 1
            or type(preview_seq) is not int
            or preview_seq <= 0
            or volatile_entries[0].get("seq") != preview_seq
        ):
            errors.append(f"{command}: volatile preview identity is invalid")
    elif volatile_entries or preview_seq != 0:
        errors.append(f"{command}: unexpected volatile preview row")
    retained_count = first.get("count")
    if type(retained_count) is not int or retained_count < 0:
        errors.append(f"{command}: retained count is invalid")
    elif retained_count != len(retained_entries):
        errors.append(
            f"{command}: retained count does not equal captured durable entries"
        )
    if command == "messages public":
        if first.get("retained_public_count") != retained_count:
            errors.append("messages public: retained Public count mismatch")
        store_count = first.get("retained_store_count")
        capacity = first.get("capacity")
        if (
            type(store_count) is not int
            or type(capacity) is not int
            or capacity <= 0
            or store_count < retained_count
            or store_count > capacity
        ):
            errors.append("messages public: retained shared-store count is invalid")
    elif first.get("retained_count") != retained_count:
        errors.append("messages dm: retained count mismatch")
    retained_entries.sort(key=lambda entry: entry["seq"])
    return {
        "count": first.get("count"),
        "capacity": first.get("capacity"),
        **(
            {"retained_store_count": first.get("retained_store_count")}
            if command == "messages public"
            else {}
        ),
        "total_written": first.get("total_written"),
        "dropped_oldest": first.get("dropped_oldest"),
        "retained_epoch": first.get("retained_epoch"),
        "content_revision": first.get("content_revision"),
        "entries": retained_entries,
    }, errors


def _contact_projection(
    result: dict[str, Any],
) -> tuple[dict[str, Any] | None, list[str]]:
    errors: list[str] = []
    if not _command_ok(result, "contacts") or result.get("persisted") is not True:
        return None, ["contacts: command or persistence failed"]
    entries = result.get("entries")
    count = result.get("count")
    capacity = result.get("capacity")
    if (
        type(count) is not int
        or type(capacity) is not int
        or count < 0
        or capacity <= 0
        or count > capacity
        or not isinstance(entries, list)
        or not all(isinstance(entry, dict) for entry in entries)
    ):
        return None, ["contacts: entries are invalid"]
    if count != len(entries):
        errors.append(
            "contacts: snapshot is truncated; all retained contacts are required"
        )
    durable_fields = (
        "seq",
        "fingerprint",
        "public_key",
        "alias",
        "heard_name",
        "type",
        "verification_source",
        "verified_at_ms",
        "signed_advert_timestamp",
        "canonical",
        "can_dm",
        "favorite",
        "muted",
    )
    projected: list[dict[str, Any]] = []
    for index, entry in enumerate(entries):
        if any(field not in entry for field in durable_fields):
            errors.append(f"contacts: entry {index} lacks durable fields")
            continue
        projected.append({field: entry[field] for field in durable_fields})
    projected.sort(key=lambda entry: entry["seq"])
    sequences = [entry.get("seq") for entry in projected]
    if (
        len(projected) != len(entries)
        or any(type(value) is not int or value <= 0 for value in sequences)
        or len(set(sequences)) != len(sequences)
    ):
        errors.append("contacts: durable sequence values are invalid or duplicated")
    for field in (
        "total_written",
        "dropped_oldest",
        "persistence_revision",
    ):
        if type(result.get(field)) is not int or result[field] < 0:
            errors.append(f"contacts: {field} is invalid")
    if (
        result.get("persistence_dirty") is not False
        or result.get("persistence_last_error") != "ESP_OK"
    ):
        errors.append("contacts: persistence state is not clean")
    return {
        "count": count,
        "capacity": capacity,
        "total_written": result.get("total_written"),
        "dropped_oldest": result.get("dropped_oldest"),
        "persistence_revision": result.get("persistence_revision"),
        "entries": projected,
    }, errors


def _read_state_projection(
    result: dict[str, Any],
) -> tuple[dict[str, Any] | None, list[str]]:
    fields = (
        "public_unread",
        "dm_unread",
        "muted_dm_unread",
        "dm_thread_count",
        "last_public_read_seq",
        "last_dm_read_seq",
        "newest_public_rx_seq",
        "newest_dm_rx_seq",
        "mark_read_count",
        "dm_threads",
    )
    if not _command_ok(result, "messages unread") or result.get("persisted") is not True:
        return None, ["messages unread: command or persistence failed"]
    if any(field not in result for field in fields):
        return None, ["messages unread: retained fields are incomplete"]
    if not isinstance(result.get("dm_threads"), list):
        return None, ["messages unread: dm_threads is invalid"]
    return {field: result[field] for field in fields}, []


def recompute_state_capture(
    capture: object,
    commit: str,
) -> tuple[dict[str, Any] | None, list[str], dict[str, Any]]:
    errors: list[str] = []
    raw_results: dict[str, Any] = {}
    if not isinstance(capture, dict) or not isinstance(capture.get("commands"), list):
        return None, ["state capture is missing commands"], raw_results
    results: list[tuple[str, dict[str, Any]]] = []
    for index, row in enumerate(capture["commands"]):
        result, row_errors = recompute_raw_command(row)
        errors.extend(f"state command[{index}]: {error}" for error in row_errors)
        if result is not None and isinstance(row, dict):
            results.append((str(row.get("command")), result))
    by_exact = {request: result for request, result in results}
    version = by_exact.get("version")
    health = by_exact.get("health")
    crashlog = by_exact.get("crashlog")
    settings = by_exact.get("settings get")
    unread = by_exact.get("messages unread")
    contacts = by_exact.get("contacts")
    if not exact_version(version, commit):
        errors.append("state: exact candidate version identity failed")
    if not exact_health(health):
        errors.append("state: exact candidate health identity/readiness failed")
    if not _command_ok(crashlog, "crashlog"):
        errors.append("state: crashlog command failed")
    settings_projection = (
        _settings_projection(settings) if isinstance(settings, dict) else None
    )
    if settings_projection is None:
        errors.append("state: settings projection failed")

    public_pages = [
        result
        for request, result in results
        if request == "messages public"
        or request.startswith("messages public offset ")
    ]
    dm_pages = [
        result
        for request, result in results
        if request == "messages dm"
        or request.startswith("messages dm offset ")
    ]
    public_projection, public_errors = _page_projection(
        "messages public", public_pages
    )
    direct_projection, direct_errors = _page_projection(
        "messages dm", dm_pages
    )
    errors.extend(public_errors)
    errors.extend(direct_errors)
    contacts_projection, contact_errors = (
        _contact_projection(contacts)
        if isinstance(contacts, dict)
        else (None, ["contacts: result is missing"])
    )
    unread_projection, unread_errors = (
        _read_state_projection(unread)
        if isinstance(unread, dict)
        else (None, ["messages unread: result is missing"])
    )
    errors.extend(contact_errors)
    errors.extend(unread_errors)

    raw_results = {
        "version": version,
        "health": health,
        "crashlog": crashlog,
    }
    if any(
        value is None
        for value in (
            settings_projection,
            public_projection,
            direct_projection,
            contacts_projection,
            unread_projection,
        )
    ):
        return None, errors, raw_results
    projection = {
        "settings": settings_projection,
        "public_messages": public_projection,
        "direct_messages": direct_projection,
        "read_state": unread_projection,
        "contacts": contacts_projection,
    }
    return projection, errors, raw_results


def projection_sha256(projection: dict[str, Any]) -> str:
    return sha256_bytes(canonical_json(projection))


def state_preserved(
    baseline: dict[str, Any],
    observed: dict[str, Any],
) -> tuple[bool, list[str]]:
    errors: list[str] = []
    if baseline.get("settings") != observed.get("settings"):
        errors.append("settings changed or were lost")
    if baseline.get("read_state") != observed.get("read_state"):
        errors.append("message read-state changed or was lost")
    for key in ("public_messages", "direct_messages"):
        before = baseline.get(key)
        after = observed.get(key)
        if not isinstance(before, dict) or not isinstance(after, dict):
            errors.append(f"{key} projection is missing")
            continue
        before_entries = {
            canonical_json(entry) for entry in before.get("entries", [])
        }
        after_entries = {
            canonical_json(entry) for entry in after.get("entries", [])
        }
        if not before_entries.issubset(after_entries):
            errors.append(f"{key} lost retained rows")
        for counter in ("total_written", "dropped_oldest", "content_revision"):
            before_value = before.get(counter)
            after_value = after.get(counter)
            if (
                type(before_value) is not int
                or type(after_value) is not int
                or after_value < before_value
            ):
                errors.append(f"{key} counter {counter} regressed")
    before_contacts = baseline.get("contacts")
    after_contacts = observed.get("contacts")
    if not isinstance(before_contacts, dict) or not isinstance(after_contacts, dict):
        errors.append("contacts projection is missing")
    else:
        before_rows = {
            canonical_json(entry) for entry in before_contacts.get("entries", [])
        }
        after_rows = {
            canonical_json(entry) for entry in after_contacts.get("entries", [])
        }
        if not before_rows.issubset(after_rows):
            errors.append("contacts lost retained rows")
    return not errors, errors


def recompute_retained_witness_proof(
    result: object,
    *,
    expected: dict[str, Any],
    initial_projection: dict[str, Any] | None,
) -> tuple[dict[str, Any] | None, list[str]]:
    errors: list[str] = []
    if not _command_ok(result, "core retained-witness"):
        return None, ["retained witness command failed"]
    if not isinstance(initial_projection, dict):
        return None, ["retained witness initial projection is missing"]
    assert isinstance(result, dict)
    required = {
        "token": expected.get("token"),
        "witness_request_label": expected.get("witness_request_label"),
        "persisted": True,
        "retention": "nvs",
        "backend_mode": "nvs_disabled",
        "synthetic_local": False,
        "retained_flush": "not_requested_zero_mutation",
        "witness_only": True,
        "public_mutated": False,
        "dm_mutated": False,
        "contact_mutated": False,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "sd_access": False,
        "rp2040_access": False,
        "formats_sd": False,
        "predecessor_evidence_used": False,
        "public_evicted": False,
        "dm_evicted": False,
        "contact_evicted": False,
    }
    for key, expected_value in required.items():
        observed = result.get(key)
        if observed != expected_value:
            errors.append(f"retained witness {key} mismatch")
    sequence_fields = ("public_seq", "dm_seq", "contact_seq")
    for key in sequence_fields:
        if type(result.get(key)) is not int or result[key] <= 0:
            errors.append(f"retained witness {key} is invalid")

    public_before = initial_projection.get("public_messages")
    dm_before = initial_projection.get("direct_messages")
    contacts_before = initial_projection.get("contacts")
    if not all(
        isinstance(value, dict)
        for value in (public_before, dm_before, contacts_before)
    ):
        return None, [
            *errors,
            "retained witness initial retained projections are incomplete",
        ]
    assert isinstance(public_before, dict)
    assert isinstance(dm_before, dict)
    assert isinstance(contacts_before, dict)

    def integer(name: str) -> int | None:
        value = result.get(name)
        if type(value) is not int or value < 0:
            errors.append(f"retained witness {name} is invalid")
            return None
        return value

    public_fields = {
        name: integer(name)
        for name in (
            "public_store_count_before",
            "public_store_count_after",
            "public_retained_count_before",
            "public_retained_count_after",
            "public_capacity",
            "public_total_written_before",
            "public_total_written_after",
            "public_dropped_oldest_before",
            "public_dropped_oldest_after",
            "public_content_revision_before",
            "public_content_revision_after",
        )
    }
    dm_fields = {
        name: integer(name)
        for name in (
            "dm_count_before",
            "dm_count_after",
            "dm_capacity",
            "dm_total_written_before",
            "dm_total_written_after",
            "dm_dropped_oldest_before",
            "dm_dropped_oldest_after",
            "dm_content_revision_before",
            "dm_content_revision_after",
        )
    }
    contact_fields = {
        name: integer(name)
        for name in (
            "contact_count_before",
            "contact_count_after",
            "contact_capacity",
            "contact_total_written_before",
            "contact_total_written_after",
            "contact_dropped_oldest_before",
            "contact_dropped_oldest_after",
            "contact_persistence_revision_before",
            "contact_persistence_revision_after",
        )
    }

    public_initial_required = {
        "public_store_count_before": public_before.get("retained_store_count"),
        "public_retained_count_before": public_before.get("count"),
        "public_capacity": public_before.get("capacity"),
        "public_total_written_before": public_before.get("total_written"),
        "public_dropped_oldest_before": public_before.get("dropped_oldest"),
        "public_content_revision_before": public_before.get("content_revision"),
    }
    dm_initial_required = {
        "dm_count_before": dm_before.get("count"),
        "dm_capacity": dm_before.get("capacity"),
        "dm_total_written_before": dm_before.get("total_written"),
        "dm_dropped_oldest_before": dm_before.get("dropped_oldest"),
        "dm_content_revision_before": dm_before.get("content_revision"),
    }
    contact_initial_required = {
        "contact_count_before": contacts_before.get("count"),
        "contact_capacity": contacts_before.get("capacity"),
        "contact_total_written_before": contacts_before.get("total_written"),
        "contact_dropped_oldest_before": contacts_before.get("dropped_oldest"),
        "contact_persistence_revision_before": contacts_before.get(
            "persistence_revision"
        ),
    }
    for values, expected_values, label in (
        (public_fields, public_initial_required, "Public"),
        (dm_fields, dm_initial_required, "DM"),
        (contact_fields, contact_initial_required, "contact"),
    ):
        for key, expected_value in expected_values.items():
            if values.get(key) != expected_value:
                errors.append(
                    f"retained witness {label} initial {key} mismatch"
                )

    def witness(
        entries: object,
        sequence: object,
        label: str,
    ) -> dict[str, Any] | None:
        if not isinstance(entries, list):
            errors.append(f"retained witness {label} entries are missing")
            return None
        matches = [
            entry
            for entry in entries
            if isinstance(entry, dict) and entry.get("seq") == sequence
        ]
        if len(matches) != 1:
            errors.append(
                f"retained witness {label} durable witness is not unique"
            )
            return None
        return matches[0]

    mode_witness = "existing_full_preserved"
    public_mode = result.get("public_mode")
    dm_mode = result.get("dm_mode")
    contact_mode = result.get("contact_mode")
    public_witness: dict[str, Any] | None = None
    dm_witness: dict[str, Any] | None = None
    contact_witness: dict[str, Any] | None = None

    if public_mode == mode_witness:
        if (
            result.get("public_mutated") is not False
            or public_fields["public_store_count_before"]
            != public_fields["public_capacity"]
            or public_fields["public_retained_count_before"]
            != public_fields["public_capacity"]
            or public_before.get("retained_store_count")
            != public_before.get("count")
            or not isinstance(public_before.get("entries"), list)
            or len(public_before["entries"])
            != public_fields["public_capacity"]
        ):
            errors.append("retained Public witness full-store mode is unsafe")
        for suffix in (
            "store_count",
            "retained_count",
            "total_written",
            "dropped_oldest",
            "content_revision",
        ):
            if public_fields[f"public_{suffix}_after"] != public_fields[
                f"public_{suffix}_before"
            ]:
                errors.append(
                    f"retained Public witness {suffix} changed"
                )
        public_witness = witness(
            public_before.get("entries"), result.get("public_seq"), "Public"
        )
    else:
        errors.append("retained Public proof must be witness-only")

    if dm_mode == mode_witness:
        if (
            result.get("dm_mutated") is not False
            or dm_fields["dm_count_before"] != dm_fields["dm_capacity"]
            or not isinstance(dm_before.get("entries"), list)
            or len(dm_before["entries"]) != dm_fields["dm_capacity"]
        ):
            errors.append("retained DM witness full-store mode is unsafe")
        for suffix in (
            "count",
            "total_written",
            "dropped_oldest",
            "content_revision",
        ):
            if dm_fields[f"dm_{suffix}_after"] != dm_fields[
                f"dm_{suffix}_before"
            ]:
                errors.append(
                    f"retained DM witness {suffix} changed"
                )
        dm_witness = witness(
            dm_before.get("entries"), result.get("dm_seq"), "DM"
        )
    else:
        errors.append("retained DM proof must be witness-only")

    fingerprint = result.get("fingerprint")
    public_key = result.get("public_key")
    normalized_fingerprint = (
        fingerprint.upper() if isinstance(fingerprint, str) else ""
    )
    normalized_public_key = (
        public_key.lower() if isinstance(public_key, str) else ""
    )
    if (
        not re.fullmatch(r"[0-9A-F]{16}", normalized_fingerprint)
        or not re.fullmatch(r"[0-9a-f]{64}", normalized_public_key)
        or normalized_public_key[:16].upper() != normalized_fingerprint
    ):
        errors.append("retained contact witness identity is invalid")
    if contact_mode == mode_witness:
        if (
            result.get("contact_result") != mode_witness
            or result.get("contact_mutated") is not False
            or contact_fields["contact_count_before"]
            != contact_fields["contact_capacity"]
            or not isinstance(contacts_before.get("entries"), list)
            or len(contacts_before["entries"])
            != contact_fields["contact_capacity"]
        ):
            errors.append("retained contact witness full-store mode is unsafe")
        for suffix in (
            "count",
            "total_written",
            "dropped_oldest",
            "persistence_revision",
        ):
            if contact_fields[f"contact_{suffix}_after"] != contact_fields[
                f"contact_{suffix}_before"
            ]:
                errors.append(
                    f"retained contact witness {suffix} changed"
                )
        contact_witness = witness(
            contacts_before.get("entries"),
            result.get("contact_seq"),
            "contact",
        )
        if (
            contact_witness is not None
            and (
                str(contact_witness.get("fingerprint") or "").upper()
                != normalized_fingerprint
                or str(contact_witness.get("public_key") or "").lower()
                != normalized_public_key
                or contact_witness.get("canonical") is not True
                or contact_witness.get("can_dm") is not True
            )
        ):
            errors.append(
                "selected contact was not the exact initial retained witness"
            )
    else:
        errors.append("retained contact proof must be witness-only")

    if errors:
        return None, errors
    return {
        **expected,
        **{key: result[key] for key in sequence_fields},
        "contact_fingerprint": normalized_fingerprint,
        "contact_public_key": normalized_public_key,
        "public_mode": public_mode,
        "dm_mode": dm_mode,
        "contact_mode": contact_mode,
        "contact_result": result.get("contact_result"),
        "public_witness": public_witness,
        "dm_witness": dm_witness,
        "contact_witness": contact_witness,
        "store_after": {
            "public_messages": {
                "retained_store_count": public_fields[
                    "public_store_count_after"
                ],
                "count": public_fields["public_retained_count_after"],
                "capacity": public_fields["public_capacity"],
                "total_written": public_fields["public_total_written_after"],
                "dropped_oldest": public_fields[
                    "public_dropped_oldest_after"
                ],
                "content_revision": public_fields[
                    "public_content_revision_after"
                ],
            },
            "direct_messages": {
                "count": dm_fields["dm_count_after"],
                "capacity": dm_fields["dm_capacity"],
                "total_written": dm_fields["dm_total_written_after"],
                "dropped_oldest": dm_fields["dm_dropped_oldest_after"],
                "content_revision": dm_fields[
                    "dm_content_revision_after"
                ],
            },
            "contacts": {
                "count": contact_fields["contact_count_after"],
                "capacity": contact_fields["contact_capacity"],
                "total_written": contact_fields[
                    "contact_total_written_after"
                ],
                "dropped_oldest": contact_fields[
                    "contact_dropped_oldest_after"
                ],
                "persistence_revision": contact_fields[
                    "contact_persistence_revision_after"
                ],
            },
        },
    }, []


def retention_witness_check(
    projection: dict[str, Any],
    *,
    witness: dict[str, Any],
) -> tuple[bool, list[str]]:
    errors: list[str] = []
    if (
        projection.get("settings", {}).get("node_name")
        != witness.get("settings_node_name")
    ):
        errors.append("settings retention marker is missing")
    public_rows = projection.get("public_messages", {}).get("entries", [])
    matching_public = (
        [row for row in public_rows if row == witness.get("public_witness")]
        if witness.get("public_mode") == "existing_full_preserved"
        else []
    )
    if len(matching_public) != 1:
        errors.append("exact retained Public witness is missing or duplicated")
    direct_rows = projection.get("direct_messages", {}).get("entries", [])
    matching_direct = (
        [row for row in direct_rows if row == witness.get("dm_witness")]
        if witness.get("dm_mode") == "existing_full_preserved"
        else []
    )
    if len(matching_direct) != 1:
        errors.append("exact retained DM witness is missing or duplicated")
    contact_rows = projection.get("contacts", {}).get("entries", [])
    matching_contacts = (
        [row for row in contact_rows if row == witness.get("contact_witness")]
        if witness.get("contact_mode") == "existing_full_preserved"
        else []
    )
    if len(matching_contacts) != 1:
        errors.append(
            "exact canonical retained contact witness is missing or duplicated"
        )
    return not errors, errors


def seed_store_transition_preserved(
    initial: dict[str, Any],
    observed: dict[str, Any],
    *,
    witness: dict[str, Any],
) -> tuple[bool, list[str]]:
    errors: list[str] = []
    store_after = witness.get("store_after")
    if not isinstance(store_after, dict):
        return False, ["seed store transition metadata is missing"]
    for key, mode_key in (
        ("public_messages", "public_mode"),
        ("direct_messages", "dm_mode"),
        ("contacts", "contact_mode"),
    ):
        before = initial.get(key)
        after = observed.get(key)
        expected_after = store_after.get(key)
        if not all(
            isinstance(value, dict)
            for value in (before, after, expected_after)
        ):
            errors.append(f"{key}: seed transition projection is incomplete")
            continue
        assert isinstance(before, dict)
        assert isinstance(after, dict)
        assert isinstance(expected_after, dict)
        for field, expected_value in expected_after.items():
            if after.get(field) != expected_value:
                errors.append(
                    f"{key}: final {field} differs from raw witness proof"
                )
        if witness.get(mode_key) != "existing_full_preserved":
            errors.append(f"{key}: non-witness seed mode is forbidden")
            continue
        if after != before:
            errors.append(f"{key}: full-store witness state changed")
    return not errors, errors


_ABSENT_TARGET_ERRORS = (
    "is missing or dangling",
    "is not present",
)


def target_presence_sample(
    requested_target: str,
    port_lister: Callable[[], Iterable[object]],
    *,
    platform_name: str | None = None,
    now: Callable[[], str] = utc_now,
    clock: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    snapshot: dict[str, Any] | None = None
    error: str | None = None
    state = "present"
    try:
        snapshot = resolve_core_target(
            requested_target,
            port_lister=port_lister,
            platform_name=platform_name,
        )
    except ValueError as exc:
        error = str(exc)
        state = (
            "absent"
            if any(fragment in error for fragment in _ABSENT_TARGET_ERRORS)
            else "invalid"
        )
    return {
        "observed_at": now(),
        "monotonic_sec": round(clock(), 6),
        "requested_path": requested_target,
        "state": state,
        "present": state == "present",
        "valid_absence": state == "absent",
        "d1l_target": snapshot,
        "error": error,
    }


def wait_for_target_state(
    requested_target: str,
    port_lister: Callable[[], Iterable[object]],
    *,
    present: bool,
    timeout: float,
    poll_sec: float,
    platform_name: str | None = None,
    clock: Callable[[], float] = time.monotonic,
    now: Callable[[], str] = utc_now,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[bool, list[dict[str, Any]]]:
    deadline = clock() + timeout
    samples: list[dict[str, Any]] = []
    while clock() <= deadline:
        sample = target_presence_sample(
            requested_target,
            port_lister,
            platform_name=platform_name,
            now=now,
            clock=clock,
        )
        samples.append(sample)
        desired = "present" if present else "absent"
        if sample.get("state") == desired:
            return True, samples
        sleep(poll_sec)
    return False, samples


def prove_power_off_window(
    requested_target: str,
    port_lister: Callable[[], Iterable[object]],
    *,
    minimum_sec: float,
    poll_sec: float,
    platform_name: str | None = None,
    clock: Callable[[], float] = time.monotonic,
    now: Callable[[], str] = utc_now,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[bool, list[dict[str, Any]], float]:
    started = clock()
    samples: list[dict[str, Any]] = []
    while clock() - started < minimum_sec:
        sample = target_presence_sample(
            requested_target,
            port_lister,
            platform_name=platform_name,
            now=now,
            clock=clock,
        )
        samples.append(sample)
        if sample.get("state") != "absent":
            return False, samples, max(0.0, clock() - started)
        sleep(min(poll_sec, max(0.0, minimum_sec - (clock() - started))))
    final = target_presence_sample(
        requested_target,
        port_lister,
        platform_name=platform_name,
        now=now,
        clock=clock,
    )
    samples.append(final)
    duration = max(0.0, clock() - started)
    return final.get("state") == "absent", samples, duration


# Compatibility for the independently migrated protocol-evidence producer.
# Reboot/persistence evidence never consumes these COM-only snapshots.
def _port_record(item: object) -> dict[str, Any]:
    def value(name: str) -> Any:
        if isinstance(item, dict):
            return item.get(name)
        return getattr(item, name, None)

    return {
        "device": normalize_port(value("device")),
        "description": value("description"),
        "hwid": value("hwid"),
        "serial_number": value("serial_number"),
        "vid": value("vid"),
        "pid": value("pid"),
        "location": value("location"),
        "manufacturer": value("manufacturer"),
        "product": value("product"),
    }


def port_snapshot(
    port_lister: Callable[[], Iterable[object]],
    *,
    now: Callable[[], str] = utc_now,
    clock: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    matches = [
        _port_record(item)
        for item in port_lister()
        if normalize_port(
            item.get("device")
            if isinstance(item, dict)
            else getattr(item, "device", None)
        )
        == D1L_CORE_PORT
    ]
    return {
        "observed_at": now(),
        "monotonic_sec": round(clock(), 6),
        "port": D1L_CORE_PORT,
        "present": len(matches) == 1,
        "matches": matches,
    }


def port_identity(snapshot: object) -> str | None:
    if not isinstance(snapshot, dict) or snapshot.get("present") is not True:
        return None
    matches = snapshot.get("matches")
    if not isinstance(matches, list) or len(matches) != 1:
        return None
    match = matches[0]
    if not isinstance(match, dict):
        return None
    strong = (
        match.get("serial_number"),
        match.get("hwid"),
        match.get("vid"),
        match.get("pid"),
        match.get("location"),
    )
    if not any(value not in (None, "") for value in strong):
        return None
    return sha256_bytes(canonical_json(match))


def _crash_transition(
    before: object, after: object, allowed_reasons: set[str] | frozenset[str]
) -> tuple[bool, dict[str, Any]]:
    detail = {
        "total_delta": None,
        "new_entries": [],
    }
    if not (
        isinstance(before, dict)
        and isinstance(after, dict)
        and _command_ok(before, "crashlog")
        and _command_ok(after, "crashlog")
        and type(before.get("total_written")) is int
        and type(after.get("total_written")) is int
        and isinstance(before.get("entries"), list)
        and isinstance(after.get("entries"), list)
    ):
        return False, detail
    detail["total_delta"] = after["total_written"] - before["total_written"]
    max_before = max(
        (
            entry.get("seq")
            for entry in before["entries"]
            if isinstance(entry, dict) and type(entry.get("seq")) is int
        ),
        default=0,
    )
    new_entries = [
        entry
        for entry in after["entries"]
        if (
            isinstance(entry, dict)
            and type(entry.get("seq")) is int
            and entry["seq"] > max_before
        )
    ]
    detail["new_entries"] = new_entries
    ok = (
        detail["total_delta"] == 1
        and len(new_entries) == 1
        and new_entries[0].get("crash_like") is False
        and str(new_entries[0].get("reset_reason") or "").upper()
        in allowed_reasons
    )
    return ok, detail


def _reboot_ack_ok(result: object) -> bool:
    return (
        _command_ok(result, "reboot")
        and isinstance(result, dict)
        and result.get("rebooting") is True
        and result.get("reset_scope") == "system"
        and result.get("storage_manager_quiesced") is True
        and result.get("retained_worker_quiesced") is True
        and result.get("rp2040_bridge_quiesced") is True
        and result.get("connectivity_prepare") == "ESP_OK"
        and result.get("retained_flush") == "ESP_OK"
        and result.get("route_flush") == "ESP_OK"
    )


def _transition_checks(
    pre_raw: dict[str, Any],
    post_raw: dict[str, Any],
    *,
    cycle_type: str,
) -> tuple[bool, dict[str, Any], list[str]]:
    errors: list[str] = []
    pre_health = pre_raw.get("health")
    post_health = post_raw.get("health")
    if not isinstance(pre_health, dict) or not isinstance(post_health, dict):
        return False, {}, ["health transition is missing"]
    nonce_changed = (
        type(pre_health.get("boot_nonce")) is int
        and type(post_health.get("boot_nonce")) is int
        and pre_health.get("boot_nonce") != post_health.get("boot_nonce")
    )
    post_uptime = post_health.get("uptime_ms")
    uptime_reset_window = (
        type(post_uptime) is int and 0 <= post_uptime <= 120000
    )
    if not nonce_changed:
        errors.append("boot nonce did not change")
    if not uptime_reset_window:
        errors.append("post-boot uptime is outside the bounded reset window")
    expected_reasons = (
        frozenset({"SW"}) if cycle_type == "software" else COLD_RESET_REASONS
    )
    reset_reason = str(post_health.get("reset_reason") or "").upper()
    if reset_reason not in expected_reasons:
        errors.append(f"unexpected {cycle_type} reset reason: {reset_reason}")
    crash_ok, crash_detail = _crash_transition(
        pre_raw.get("crashlog"),
        post_raw.get("crashlog"),
        expected_reasons,
    )
    if not crash_ok:
        errors.append("crashlog transition is not exactly one non-crash boot")
    return not errors, {
        "pre_boot_nonce": pre_health.get("boot_nonce"),
        "post_boot_nonce": post_health.get("boot_nonce"),
        "pre_uptime_ms": pre_health.get("uptime_ms"),
        "post_uptime_ms": post_uptime,
        "post_reset_reason": reset_reason,
        "nonce_changed": nonce_changed,
        "uptime_reset_window": uptime_reset_window,
        "crashlog": crash_detail,
    }, errors


def validate_seed_receipt(
    receipt: object,
    *,
    commit: str,
    run_id: str,
    run_attempt: str,
    expected_target: str | None = None,
    expected_target_identity_sha256: str | None = None,
    expected_d1l_public_key: str | None = None,
) -> tuple[dict[str, Any] | None, list[str]]:
    errors: list[str] = []
    if not isinstance(receipt, dict):
        return None, ["seed receipt is not an object"]
    if (
        exact_commit(commit) is None
        or not positive_decimal(run_id)
        or not positive_decimal(run_attempt)
    ):
        return None, ["seed expected candidate identity is invalid"]
    receipt_target = normalize_port(receipt.get("port"))
    target = (
        enforce_core_port(expected_target)
        if expected_target is not None
        else receipt_target
    )
    if target not in {D1L_CORE_PORT, D1L_CORE_POSIX_TARGET}:
        return None, ["seed target is invalid"]
    receipt_public_key = exact_public_key(
        receipt.get("expected_d1l_public_key")
    )
    public_key = (
        exact_public_key(expected_d1l_public_key)
        if expected_d1l_public_key is not None
        else receipt_public_key
    )
    if public_key is None:
        return None, ["seed expected D1L public key is invalid"]
    receipt_target_identity = target_identity(
        receipt.get("d1l_target"), target
    )
    required_target_identity = (
        expected_target_identity_sha256
        if expected_target_identity_sha256 is not None
        else receipt_target_identity
    )
    required = {
        "schema": 2,
        "kind": "core_retained_state_seed",
        "mode": "hardware",
        "ok": True,
        "closure_eligible": False,
        "hardware_required": True,
        "physical_observed": True,
        "port": target,
        "expected_target_identity_sha256": required_target_identity,
        "expected_d1l_public_key": public_key,
        "d1l_identity_ok": True,
        "commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": SD_HISTORY_MODE,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "sd_access": False,
        "rp2040_access": False,
        "formats_sd": False,
        "predecessor_evidence_used": False,
        "mutation_outcome_uncertain": False,
    }
    for key, expected in required.items():
        if receipt.get(key) != expected:
            errors.append(f"seed: {key} mismatch")
    if (
        receipt_target_identity is None
        or receipt_target_identity != required_target_identity
    ):
        errors.append("seed: D1L target snapshot is invalid")
    identity_result, identity_errors = recompute_raw_command(
        receipt.get("d1l_identity_status")
    )
    errors.extend(f"seed identity: {error}" for error in identity_errors)
    if not identity_status_ok(identity_result, public_key):
        errors.append("seed: live D1L identity does not match the pinned key")
    source = receipt.get("git")
    if not (
        isinstance(source, dict)
        and exact_commit(source.get("commit")) == commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    ):
        errors.append("seed: source git is not the exact clean candidate")
    producer_io = receipt.get("producer_io")
    if not (
        isinstance(producer_io, dict)
        and producer_io.get("stage") == "complete"
        and producer_io.get("serial_open_attempted") is True
        and producer_io.get("serial_opened") is True
        and producer_io.get("physical_observed") is True
        and producer_io.get("settings_mutation_may_have_executed") is True
        and producer_io.get("settings_mutation_confirmed_persisted") is True
        and producer_io.get("mutation_outcome_uncertain") is False
    ):
        errors.append("seed: producer I/O and mutation outcome are not closed")
    initial_projection, initial_errors, _ = recompute_state_capture(
        receipt.get("initial_state_capture"), commit
    )
    errors.extend(f"seed initial: {error}" for error in initial_errors)
    if initial_projection is None:
        errors.append("seed: initial exact-candidate state is missing")
    witness_result, witness_raw_errors = recompute_raw_command(
        receipt.get("retained_witness_proof")
    )
    errors.extend(
        f"seed retained witness proof: {error}" for error in witness_raw_errors
    )
    expected_witness = candidate_witness_identity(commit, run_id, run_attempt)
    expected_witness_command = (
        f"core retained-witness {expected_witness['token']}"
    )
    witness_receipt = receipt.get("retained_witness_proof")
    if not (
        isinstance(witness_receipt, dict)
        and witness_receipt.get("command") == expected_witness_command
    ):
        errors.append("seed retained witness proof: exact command mismatch")
    derived_witness, witness_errors = recompute_retained_witness_proof(
        witness_result,
        expected=expected_witness,
        initial_projection=initial_projection,
    )
    errors.extend(
        f"seed retained witness proof: {error}" for error in witness_errors
    )
    settings_result, settings_errors = recompute_raw_command(
        receipt.get("settings_retention_mutation")
    )
    errors.extend(f"seed settings mutation: {error}" for error in settings_errors)
    settings_receipt = receipt.get("settings_retention_mutation")
    if not (
        isinstance(settings_receipt, dict)
        and settings_receipt.get("command")
        == f"settings set name {expected_witness['settings_node_name']}"
    ):
        errors.append("seed settings mutation: exact command mismatch")
    projection, capture_errors, _ = recompute_state_capture(
        receipt.get("state_capture"), commit
    )
    errors.extend(f"seed: {error}" for error in capture_errors)
    witness = receipt.get("retention_witness")
    if not isinstance(witness, dict):
        errors.append("seed: retention witness descriptor is missing")
    elif derived_witness is None:
        errors.append("seed: retention witness cannot be derived from raw")
    else:
        if witness != derived_witness:
            errors.append(
                "seed: retention witness differs from raw witness proof"
            )
        if not (
            _command_ok(settings_result, "settings set name")
            and settings_result.get("persisted") is True
            and settings_result.get("node_name")
            == expected_witness["settings_node_name"]
        ):
            errors.append("seed: settings retention mutation is not proven")
        if projection is not None:
            witness_ok, witness_check_errors = retention_witness_check(
                projection,
                witness=derived_witness,
            )
            if not witness_ok:
                errors.extend(
                    f"seed: {error}" for error in witness_check_errors
                )
            if initial_projection is not None:
                transition_ok, transition_errors = seed_store_transition_preserved(
                    initial_projection,
                    projection,
                    witness=derived_witness,
                )
                if not transition_ok:
                    errors.extend(
                        f"seed: {error}" for error in transition_errors
                    )
    if projection is not None and receipt.get("projection_sha256") != projection_sha256(
        projection
    ):
        errors.append("seed: projection digest mismatch")
    return projection, errors


def _snapshot_has_retention_witnesses(
    snapshot: dict[str, Any],
    witness: dict[str, Any],
    commit: str,
) -> bool:
    results = snapshot.get("results")
    if not isinstance(results, list):
        return False
    rows = [row for row in results if isinstance(row, dict)]

    def first(command: str) -> dict[str, Any] | None:
        return next((row for row in rows if row.get("cmd") == command), None)

    if not exact_version(first("version"), commit):
        return False
    if not exact_health(first("health")):
        return False
    contact_result = first("contacts")
    contact_projection, contact_errors = (
        _contact_projection(contact_result)
        if isinstance(contact_result, dict)
        else (None, ["contacts: result is missing"])
    )
    if contact_projection is None or contact_errors:
        return False
    projected: dict[str, Any] = {
        "settings": first("settings get") or {},
        "public_messages": {
            "entries": [
                entry
                for row in rows
                if row.get("cmd") == "messages public"
                and isinstance(row.get("entries"), list)
                for entry in row["entries"]
                if isinstance(entry, dict)
            ],
        },
        "direct_messages": {
            "entries": [
                entry
                for row in rows
                if row.get("cmd") == "messages dm"
                and isinstance(row.get("entries"), list)
                for entry in row["entries"]
                if isinstance(entry, dict)
            ],
        },
        "contacts": contact_projection,
    }
    witness_ok, _ = retention_witness_check(projected, witness=witness)
    return witness_ok


def command_uses_only_target(command: object, target: str) -> bool:
    if not isinstance(command, list) or not all(
        isinstance(token, str) for token in command
    ):
        return False
    selected: list[str] = []
    for index, token in enumerate(command):
        if token in {"--port", "-p"}:
            if index + 1 >= len(command):
                return False
            selected.append(command[index + 1])
        elif token.startswith("--port="):
            selected.append(token.split("=", 1)[1])
    return selected == [target]


def validate_closing_flash_receipt(
    receipt: object,
    *,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
    witness: dict[str, Any],
    expected_target: str,
    expected_target_identity_sha256: str,
    expected_d1l_public_key: str,
) -> list[str]:
    errors: list[str] = []
    if not isinstance(receipt, dict):
        return ["flash receipt is not an object"]
    if (
        exact_commit(commit) is None
        or not positive_decimal(run_id)
        or not positive_decimal(run_attempt)
    ):
        return ["flash expected candidate identity is invalid"]
    target = enforce_core_port(expected_target)
    public_key = exact_public_key(expected_d1l_public_key)
    if public_key is None:
        return ["flash expected D1L public key is invalid"]
    required = {
        "schema": 2,
        "kind": "esp32_flash",
        "mode": "hardware",
        "scope": "core-retained-reflash-only",
        "flash_phase": "retained-reflash",
        "ok": True,
        "closure_eligible": True,
        "hardware_required": True,
        "physical_observed": True,
        "port": target,
        "commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": SD_HISTORY_MODE,
        "firmware_identity_ok": True,
        "runner_source_identity_ok": True,
        "expected_d1l_public_key": public_key,
        "target_identity_continuity_ok": True,
        "d1l_public_key_continuity_ok": True,
        "retained_state_preserved": True,
        "erase_flash": False,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "sd_access": False,
        "rp2040_access": False,
        "formats_sd": False,
        "legacy_suite_ran": False,
    }
    for key, expected in required.items():
        if receipt.get(key) != expected:
            errors.append(f"flash: {key} mismatch")
    target_snapshots = (
        receipt.get("d1l_target"),
        receipt.get("d1l_target_before"),
        receipt.get("d1l_target_after"),
    )
    target_identities = [
        target_identity(snapshot, target) for snapshot in target_snapshots
    ]
    if (
        any(identity is None for identity in target_identities)
        or any(
            identity != expected_target_identity_sha256
            for identity in target_identities
        )
    ):
        errors.append("flash: D1L target identity binding failed")
    if not identity_status_ok(
        receipt.get("pre_flash_identity"), public_key
    ):
        errors.append("flash: pre-flash D1L identity binding failed")
    if not identity_status_ok(
        receipt.get("post_flash_identity"), public_key
    ):
        errors.append("flash: post-flash D1L identity binding failed")
    source = receipt.get("git")
    if not (
        isinstance(source, dict)
        and exact_commit(source.get("commit")) == commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    ):
        errors.append("flash: source git is not the exact clean candidate")
    command = receipt.get("command")
    if not (
        isinstance(command, list)
        and "write-flash" in command
        and not any("erase" in str(token).lower() for token in command)
        and command_uses_only_target(command, target)
    ):
        errors.append(
            "flash: command is not an exact non-erasing target write-flash"
        )
    for label, field in (
        ("flash raw log", "raw_flash_log"),
        ("pre-flash retained snapshot", "retained_state_before"),
        ("post-flash retained snapshot", "retained_state_after"),
    ):
        path, row_errors = validate_file_row(receipt.get(field), root, label)
        errors.extend(row_errors)
        if path is not None and field != "raw_flash_log":
            try:
                snapshot = load_json(path, root, label)
            except ValueError as exc:
                errors.append(str(exc))
                continue
            expected_phase = "pre_flash" if field == "retained_state_before" else "post_flash"
            projection = snapshot.get("projection")
            if not (
                snapshot.get("schema") == 2
                and snapshot.get("kind") == "core_retained_state_snapshot"
                and snapshot.get("mode") == "hardware"
                and snapshot.get("phase") == expected_phase
                and snapshot.get("port") == target
                and target_identity(snapshot.get("d1l_target"), target)
                == expected_target_identity_sha256
                and exact_commit(snapshot.get("expected_firmware_commit")) == commit
                and isinstance(projection, dict)
                and snapshot.get("projection_sha256") == projection_sha256(projection)
                and _snapshot_has_retention_witnesses(
                    snapshot, witness, commit
                )
            ):
                errors.append(
                    f"flash: {expected_phase} snapshot failed witness/identity checks"
                )
    return errors


def _cycle_base(
    *,
    matrix_id: str,
    cycle_type: str,
    ordinal: int,
    commit: str,
    run_id: str,
    run_attempt: str,
    seed_sha256: str,
    flash_sha256: str,
    previous_sha256: str,
    d1l_target: dict[str, Any],
) -> dict[str, Any]:
    requested_target = d1l_target["requested_path"]
    return {
        "schema": 2,
        "kind": "core_reboot_persistence_cycle",
        "mode": "hardware",
        "matrix_id": matrix_id,
        "cycle_id": f"{matrix_id}:{cycle_type}:{ordinal}",
        "cycle_type": cycle_type,
        "ordinal": ordinal,
        "port": requested_target,
        "expected_target_identity_sha256": d1l_target[
            "stable_identity_sha256"
        ],
        "baud": D1L_BAUD,
        "commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": SD_HISTORY_MODE,
        "seed_receipt_sha256": seed_sha256,
        "closing_flash_receipt_sha256": flash_sha256,
        "previous_receipt_sha256": previous_sha256,
        "started_at": utc_now(),
        "ended_at": None,
        "pre": None,
        "post": None,
        "action": None,
        "checks": {},
        "ok": False,
        "closure_eligible": False,
        "hardware_required": True,
        "physical_observed": False,
        "stage": "reserved_before_io",
        "serial_open_attempted": False,
        "serial_opened": False,
        "reboot_or_power_action_may_have_executed": False,
        "physical_state_outcome_uncertain": False,
        "mutation_outcome_uncertain": False,
        "partial_command_receipts": [],
        "public_rf_tx": False,
        "formats_sd": False,
        "predecessor_evidence_used": False,
    }


def _open_serial(
    serial_module: Any,
    timeout: float,
    requested_target: str,
) -> Any:
    return open_d1l_serial(
        serial_module,
        port=requested_target,
        baudrate=D1L_BAUD,
        timeout=timeout,
    )


def _capture_and_recompute(
    ser: Any,
    *,
    timeout: float,
    commit: str,
    clock: Callable[[], float],
    now: Callable[[], str],
    command_log: list[dict[str, Any]] | None = None,
) -> tuple[dict[str, Any], dict[str, Any] | None, list[str], dict[str, Any]]:
    capture = capture_state(
        ser,
        timeout,
        clock=clock,
        now=now,
        command_log=command_log,
    )
    projection, errors, raw = recompute_state_capture(capture, commit)
    return capture, projection, errors, raw


def run_software_cycle(
    *,
    serial_module: Any,
    port_lister: Callable[[], Iterable[object]],
    timeout: float,
    transition_timeout: float,
    commit: str,
    baseline: dict[str, Any],
    report: dict[str, Any],
    requested_target: str,
    expected_target_identity_sha256: str,
    platform_name: str | None = None,
    clock: Callable[[], float] = time.monotonic,
    now: Callable[[], str] = utc_now,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    errors: list[str] = []
    command_log = report["partial_command_receipts"]
    report["stage"] = "resolving_pre_reboot_d1l_target"
    before_target = resolve_core_target(
        requested_target,
        port_lister=port_lister,
        platform_name=platform_name,
    )
    if (
        before_target["stable_identity_sha256"]
        != expected_target_identity_sha256
    ):
        raise ValueError("D1L target identity drifted before software reboot")
    report["physical_observed"] = True
    report["serial_open_attempted"] = True
    report["stage"] = "opening_pre_reboot_serial"
    with _open_serial(serial_module, timeout, requested_target) as ser:
        report["serial_opened"] = True
        report["physical_observed"] = True
        report["stage"] = "capturing_pre_reboot_state"
        if hasattr(ser, "reset_input_buffer"):
            ser.reset_input_buffer()
        pre, pre_projection, pre_errors, pre_raw = _capture_and_recompute(
            ser,
            timeout=timeout,
            commit=commit,
            clock=clock,
            now=now,
            command_log=command_log,
        )
        report["pre"] = pre
        errors.extend(f"pre: {error}" for error in pre_errors)
        if pre_projection is not None:
            preserved, state_errors = state_preserved(baseline, pre_projection)
            if not preserved:
                errors.extend(f"pre: {error}" for error in state_errors)
        report["stage"] = "software_reboot_command"
        report["reboot_or_power_action_may_have_executed"] = True
        reboot = read_raw_command(
            ser,
            "reboot",
            max(timeout, 30.0),
            clock=clock,
            now=now,
            command_log=command_log,
        )
        reboot_result, reboot_errors = recompute_raw_command(reboot)
        errors.extend(f"reboot: {error}" for error in reboot_errors)
        report["stage"] = "capturing_software_reboot_boot_lines"
        boot_lines = capture_boot_lines(
            ser, transition_timeout, clock=clock, now=now
        )
        if hasattr(ser, "reset_input_buffer"):
            ser.reset_input_buffer()
        sleep(0.1)
        report["stage"] = "capturing_post_reboot_state"
        post, post_projection, post_errors, post_raw = _capture_and_recompute(
            ser,
            timeout=timeout,
            commit=commit,
            clock=clock,
            now=now,
            command_log=command_log,
        )
        report["post"] = post
        errors.extend(f"post: {error}" for error in post_errors)
    report["stage"] = "resolving_post_reboot_d1l_target"
    after_target = resolve_core_target(
        requested_target,
        port_lister=port_lister,
        platform_name=platform_name,
    )
    boot_analysis, boot_errors = analyze_boot_lines(boot_lines)
    errors.extend(boot_errors)
    raw_sw_reset = (
        boot_analysis.get("reset_count") == 1
        and boot_analysis.get("reset_events") == [
            {"code": SOFTWARE_SYSTEM_RESET[0], "reason": SOFTWARE_SYSTEM_RESET[1]}
        ]
        and boot_analysis.get("help_count") >= 1
        and boot_analysis.get("crash_marker_count") == 0
    )
    if not raw_sw_reset:
        errors.append("software reboot raw boot/reset evidence failed")
    if not _reboot_ack_ok(reboot_result):
        errors.append("software reboot acknowledgement failed")
    transition_ok, transition, transition_errors = _transition_checks(
        pre_raw, post_raw, cycle_type="software"
    )
    errors.extend(transition_errors)
    if pre_projection is not None and post_projection is not None:
        preserved, state_errors = state_preserved(pre_projection, post_projection)
        if not preserved:
            errors.extend(f"post: {error}" for error in state_errors)
        baseline_preserved, baseline_errors = state_preserved(
            baseline, post_projection
        )
        if not baseline_preserved:
            errors.extend(f"baseline: {error}" for error in baseline_errors)
    target_ok = (
        target_continuity(
            before_target, after_target, requested_target
        )
        and before_target["stable_identity_sha256"]
        == expected_target_identity_sha256
    )
    if not target_ok:
        errors.append("D1L target identity changed across software reboot")
        report["physical_state_outcome_uncertain"] = True
        report["mutation_outcome_uncertain"] = True
    report["action"] = {
        "kind": "software_reboot",
        "reboot_command": reboot,
        "boot_raw_lines": boot_lines,
        "boot_analysis": boot_analysis,
        "port_disappear_required": False,
        "d1l_target_before": before_target,
        "d1l_target_after": after_target,
    }
    report["checks"] = {
        "raw_reboot_ack": _reboot_ack_ok(reboot_result),
        "raw_sw_system_reset": raw_sw_reset,
        "transition": transition,
        "transition_ok": transition_ok,
        "target_identity_stable": target_ok,
        "retained_state_preserved": not any(
            error.startswith(("pre:", "post:", "baseline:")) for error in errors
        ),
        "errors": errors,
    }
    report["ok"] = not errors
    report["closure_eligible"] = report["ok"]
    report["stage"] = "complete"
    report["ended_at"] = now()
    return report


def run_cold_cycle(
    *,
    serial_module: Any,
    port_lister: Callable[[], Iterable[object]],
    prompt: Callable[[str], str],
    timeout: float,
    port_timeout: float,
    port_poll_sec: float,
    minimum_power_off_sec: float,
    commit: str,
    baseline: dict[str, Any],
    report: dict[str, Any],
    requested_target: str,
    expected_target_identity_sha256: str,
    platform_name: str | None = None,
    clock: Callable[[], float] = time.monotonic,
    now: Callable[[], str] = utc_now,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    errors: list[str] = []
    command_log = report["partial_command_receipts"]
    report["stage"] = "resolving_pre_cold_cycle_d1l_target"
    before_target = resolve_core_target(
        requested_target,
        port_lister=port_lister,
        platform_name=platform_name,
    )
    if (
        before_target["stable_identity_sha256"]
        != expected_target_identity_sha256
    ):
        raise ValueError("D1L target identity drifted before cold reboot")
    report["physical_observed"] = True
    report["serial_open_attempted"] = True
    report["stage"] = "opening_pre_cold_cycle_serial"
    with _open_serial(serial_module, timeout, requested_target) as ser:
        report["serial_opened"] = True
        report["physical_observed"] = True
        report["stage"] = "capturing_pre_cold_cycle_state"
        if hasattr(ser, "reset_input_buffer"):
            ser.reset_input_buffer()
        pre, pre_projection, pre_errors, pre_raw = _capture_and_recompute(
            ser,
            timeout=timeout,
            commit=commit,
            clock=clock,
            now=now,
            command_log=command_log,
        )
    report["pre"] = pre
    errors.extend(f"pre: {error}" for error in pre_errors)
    if pre_projection is not None:
        preserved, state_errors = state_preserved(baseline, pre_projection)
        if not preserved:
            errors.extend(f"pre: {error}" for error in state_errors)

    report["stage"] = "operator_cold_power_removal"
    report["reboot_or_power_action_may_have_executed"] = True
    prompt(
        f"Cold cycle {report['ordinal']}/{COLD_CYCLE_COUNT}: press Enter to arm, "
        f"then remove all power from the D1L until {requested_target} disappears."
    )
    disappeared, disappear_samples = wait_for_target_state(
        requested_target,
        port_lister,
        present=False,
        timeout=port_timeout,
        poll_sec=port_poll_sec,
        platform_name=platform_name,
        clock=clock,
        now=now,
        sleep=sleep,
    )
    off_ok = False
    off_samples: list[dict[str, Any]] = []
    off_duration = 0.0
    if disappeared:
        off_ok, off_samples, off_duration = prove_power_off_window(
            requested_target,
            port_lister,
            minimum_sec=minimum_power_off_sec,
            poll_sec=port_poll_sec,
            platform_name=platform_name,
            clock=clock,
            now=now,
            sleep=sleep,
        )
    if not disappeared:
        errors.append(
            "stable D1L by-id target did not disappear during cold power removal"
        )
    if not off_ok or off_duration < minimum_power_off_sec:
        errors.append("cold power-off interval was not continuously observed")

    prompt(
        f"Cold cycle {report['ordinal']}/{COLD_CYCLE_COUNT}: press Enter, "
        f"then restore power and wait for {requested_target}."
    )
    reappeared, reappear_samples = wait_for_target_state(
        requested_target,
        port_lister,
        present=True,
        timeout=port_timeout,
        poll_sec=port_poll_sec,
        platform_name=platform_name,
        clock=clock,
        now=now,
        sleep=sleep,
    )
    after_sample = (
        reappear_samples[-1]
        if reappear_samples
        else target_presence_sample(
            requested_target,
            port_lister,
            platform_name=platform_name,
            now=now,
            clock=clock,
        )
    )
    after_target = after_sample.get("d1l_target")
    if not reappeared:
        errors.append(
            "stable D1L by-id target did not reappear with "
            f"VID {EXPECTED_VID:04X}/PID {EXPECTED_PID:04X}"
        )

    post: dict[str, Any] = {"captured_at": now(), "commands": []}
    post_projection: dict[str, Any] | None = None
    post_raw: dict[str, Any] = {}
    if reappeared:
        report["serial_open_attempted"] = True
        report["stage"] = "opening_post_cold_cycle_serial"
        with _open_serial(serial_module, timeout, requested_target) as ser:
            report["serial_opened"] = True
            report["physical_observed"] = True
            report["stage"] = "capturing_post_cold_cycle_state"
            sleep(1.0)
            post, post_projection, post_errors, post_raw = _capture_and_recompute(
                ser,
                timeout=timeout,
                commit=commit,
                clock=clock,
                now=now,
                command_log=command_log,
            )
            errors.extend(f"post: {error}" for error in post_errors)
    report["post"] = post

    transition_ok, transition, transition_errors = _transition_checks(
        pre_raw, post_raw, cycle_type="cold"
    )
    errors.extend(transition_errors)
    if pre_projection is not None and post_projection is not None:
        preserved, state_errors = state_preserved(pre_projection, post_projection)
        if not preserved:
            errors.extend(f"post: {error}" for error in state_errors)
        baseline_preserved, baseline_errors = state_preserved(
            baseline, post_projection
        )
        if not baseline_preserved:
            errors.extend(f"baseline: {error}" for error in baseline_errors)
    target_ok = (
        disappeared
        and off_ok
        and reappeared
        and target_continuity(
            before_target, after_target, requested_target
        )
        and before_target["stable_identity_sha256"]
        == expected_target_identity_sha256
    )
    if not target_ok:
        errors.append(
            "cold-cycle D1L by-id disappearance/reappearance identity failed"
        )
        report["physical_state_outcome_uncertain"] = True
        report["mutation_outcome_uncertain"] = True
    report["action"] = {
        "kind": "operator_controlled_cold_power_cycle",
        "operator_interactive": True,
        "minimum_power_off_sec": minimum_power_off_sec,
        "observed_power_off_sec": round(off_duration, 6),
        "d1l_target_before": before_target,
        "disappear_samples": disappear_samples,
        "power_off_samples": off_samples,
        "reappear_samples": reappear_samples,
        "d1l_target_after": after_target,
    }
    report["checks"] = {
        "port_disappeared": disappeared,
        "power_off_window_observed": off_ok,
        "port_reappeared": reappeared,
        "target_identity_stable": target_ok,
        "transition": transition,
        "transition_ok": transition_ok,
        "retained_state_preserved": not any(
            error.startswith(("pre:", "post:", "baseline:")) for error in errors
        ),
        "errors": errors,
    }
    report["ok"] = not errors
    report["closure_eligible"] = report["ok"]
    report["stage"] = "complete"
    report["ended_at"] = now()
    return report


def _presence_sample_valid(
    sample: object,
    *,
    requested_target: str,
    expected_target_identity_sha256: str,
) -> bool:
    if not isinstance(sample, dict):
        return False
    if (
        sample.get("requested_path") != requested_target
        or type(sample.get("monotonic_sec")) not in (int, float)
        or not isinstance(sample.get("observed_at"), str)
    ):
        return False
    state = sample.get("state")
    if state == "present":
        return (
            sample.get("present") is True
            and sample.get("valid_absence") is False
            and sample.get("error") is None
            and target_identity(
                sample.get("d1l_target"), requested_target
            )
            == expected_target_identity_sha256
        )
    if state == "absent":
        error = sample.get("error")
        return (
            sample.get("present") is False
            and sample.get("valid_absence") is True
            and sample.get("d1l_target") is None
            and isinstance(error, str)
            and any(fragment in error for fragment in _ABSENT_TARGET_ERRORS)
        )
    return False


def _target_action_recomputed(
    action: object,
    *,
    cycle_type: str,
    requested_target: str,
    expected_target_identity_sha256: str,
) -> tuple[bool, list[str]]:
    errors: list[str] = []
    if not isinstance(action, dict):
        return False, ["cycle action is missing"]
    before = action.get("d1l_target_before")
    after = action.get("d1l_target_after")
    stable = (
        target_continuity(before, after, requested_target)
        and target_identity(before, requested_target)
        == expected_target_identity_sha256
    )
    if cycle_type == "software":
        if not (
            action.get("kind") == "software_reboot"
            and action.get("port_disappear_required") is False
            and stable
        ):
            errors.append("software cycle port evidence failed")
        return not errors, errors
    disappear = action.get("disappear_samples")
    off = action.get("power_off_samples")
    reappear = action.get("reappear_samples")
    minimum = action.get("minimum_power_off_sec")
    observed = action.get("observed_power_off_sec")
    off_times = [
        sample.get("monotonic_sec")
        for sample in off
        if isinstance(sample, dict)
    ] if isinstance(off, list) else []
    off_times_valid = (
        len(off_times) >= 2
        and all(type(value) in (int, float) for value in off_times)
        and all(
            later >= earlier
            for earlier, later in zip(off_times, off_times[1:])
        )
    )
    recomputed_off_duration = (
        float(off_times[-1]) - float(off_times[0])
        if off_times_valid
        else -1.0
    )
    disappear_samples_valid = (
        isinstance(disappear, list)
        and disappear
        and all(
            _presence_sample_valid(
                sample,
                requested_target=requested_target,
                expected_target_identity_sha256=(
                    expected_target_identity_sha256
                ),
            )
            for sample in disappear
        )
    )
    off_samples_valid = (
        isinstance(off, list)
        and len(off) >= 2
        and all(
            _presence_sample_valid(
                sample,
                requested_target=requested_target,
                expected_target_identity_sha256=(
                    expected_target_identity_sha256
                ),
            )
            and sample.get("state") == "absent"
            for sample in off
        )
    )
    reappear_samples_valid = (
        isinstance(reappear, list)
        and reappear
        and all(
            _presence_sample_valid(
                sample,
                requested_target=requested_target,
                expected_target_identity_sha256=(
                    expected_target_identity_sha256
                ),
            )
            for sample in reappear
        )
    )
    if not (
        action.get("kind") == "operator_controlled_cold_power_cycle"
        and action.get("operator_interactive") is True
        and disappear_samples_valid
        and any(
            sample.get("state") == "absent"
            for sample in disappear
        )
        and off_samples_valid
        and type(minimum) in (int, float)
        and minimum >= MINIMUM_POWER_OFF_SEC
        and type(observed) in (int, float)
        and observed >= minimum
        and off_times_valid
        and recomputed_off_duration >= minimum
        and abs(float(observed) - recomputed_off_duration) <= 0.01
        and reappear_samples_valid
        and any(
            sample.get("state") == "present"
            for sample in reappear
        )
        and stable
    ):
        errors.append("cold cycle raw port transition evidence failed")
    return not errors, errors


def recompute_cycle(
    receipt: object,
    *,
    commit: str,
    run_id: str,
    run_attempt: str,
    matrix_id: str,
    baseline: dict[str, Any],
    expected_target: str,
    expected_target_identity_sha256: str,
) -> tuple[bool, list[str]]:
    errors: list[str] = []
    if not isinstance(receipt, dict):
        return False, ["cycle receipt is not an object"]
    cycle_type = receipt.get("cycle_type")
    if cycle_type not in {"software", "cold"}:
        return False, ["cycle type is invalid"]
    required = {
        "schema": 2,
        "kind": "core_reboot_persistence_cycle",
        "mode": "hardware",
        "matrix_id": matrix_id,
        "port": expected_target,
        "expected_target_identity_sha256": (
            expected_target_identity_sha256
        ),
        "baud": D1L_BAUD,
        "commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": SD_HISTORY_MODE,
        "hardware_required": True,
        "physical_observed": True,
        "ok": True,
        "closure_eligible": True,
        "public_rf_tx": False,
        "formats_sd": False,
        "predecessor_evidence_used": False,
        "stage": "complete",
        "serial_open_attempted": True,
        "serial_opened": True,
        "reboot_or_power_action_may_have_executed": True,
        "physical_state_outcome_uncertain": False,
        "mutation_outcome_uncertain": False,
    }
    for key, expected in required.items():
        if receipt.get(key) != expected:
            errors.append(f"cycle: {key} mismatch")
    pre_projection, pre_errors, pre_raw = recompute_state_capture(
        receipt.get("pre"), commit
    )
    post_projection, post_errors, post_raw = recompute_state_capture(
        receipt.get("post"), commit
    )
    errors.extend(f"pre: {error}" for error in pre_errors)
    errors.extend(f"post: {error}" for error in post_errors)
    if pre_projection is not None:
        preserved, state_errors = state_preserved(baseline, pre_projection)
        if not preserved:
            errors.extend(f"pre baseline: {error}" for error in state_errors)
    if pre_projection is not None and post_projection is not None:
        preserved, state_errors = state_preserved(pre_projection, post_projection)
        if not preserved:
            errors.extend(f"post: {error}" for error in state_errors)
        preserved, state_errors = state_preserved(baseline, post_projection)
        if not preserved:
            errors.extend(f"post baseline: {error}" for error in state_errors)
    transition_ok, _, transition_errors = _transition_checks(
        pre_raw, post_raw, cycle_type=cycle_type
    )
    errors.extend(transition_errors)
    action = receipt.get("action")
    target_ok, target_errors = _target_action_recomputed(
        action,
        cycle_type=cycle_type,
        requested_target=expected_target,
        expected_target_identity_sha256=(
            expected_target_identity_sha256
        ),
    )
    errors.extend(target_errors)
    if cycle_type == "software":
        reboot_result, reboot_errors = recompute_raw_command(
            action.get("reboot_command") if isinstance(action, dict) else None
        )
        errors.extend(reboot_errors)
        if not _reboot_ack_ok(reboot_result):
            errors.append("software reboot raw acknowledgement failed")
        boot_analysis, boot_errors = analyze_boot_lines(
            action.get("boot_raw_lines") if isinstance(action, dict) else None
        )
        errors.extend(boot_errors)
        if not (
            boot_analysis.get("reset_count") == 1
            and boot_analysis.get("reset_events")
            == [
                {
                    "code": SOFTWARE_SYSTEM_RESET[0],
                    "reason": SOFTWARE_SYSTEM_RESET[1],
                }
            ]
            and boot_analysis.get("help_count") >= 1
            and boot_analysis.get("crash_marker_count") == 0
        ):
            errors.append("software reboot raw boot/reset evidence failed")
    return not errors and transition_ok and target_ok, errors


def _validate_core_reboot_persistence_report(
    matrix: object,
    *,
    root: Path,
    expected_commit: str,
    expected_run_id: str,
    expected_run_attempt: str,
) -> tuple[bool, list[str], dict[str, Any]]:
    errors: list[str] = []
    if not isinstance(matrix, dict):
        return False, ["Core reboot persistence matrix is not an object"], {}
    commit = exact_commit(expected_commit)
    run_id = str(expected_run_id)
    run_attempt = str(expected_run_attempt)
    if (
        commit is None
        or not positive_decimal(run_id)
        or not positive_decimal(run_attempt)
    ):
        return False, ["validator expected identity is invalid"], matrix
    target = normalize_port(matrix.get("port"))
    if target not in {D1L_CORE_PORT, D1L_CORE_POSIX_TARGET}:
        return False, ["matrix: D1L target is invalid"], matrix
    target_digest = target_identity(matrix.get("d1l_target"), target)
    if target_digest is None:
        errors.append("matrix: D1L target snapshot is invalid")
        target_digest = ""
    public_key = exact_public_key(matrix.get("expected_d1l_public_key"))
    if public_key is None:
        errors.append("matrix: expected D1L public key is invalid")
        public_key = ""
    required = {
        "schema": 2,
        "kind": "core_reboot_persistence_matrix",
        "mode": "hardware",
        "ok": True,
        "closure_eligible": True,
        "hardware_required": True,
        "physical_observed": True,
        "port": target,
        "expected_target_identity_sha256": target_digest,
        "expected_d1l_public_key": public_key,
        "commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": SD_HISTORY_MODE,
        "claim": CLAIM,
        "cross_version_migration_proven": False,
        "predecessor_evidence_used": False,
        "software_cycle_count": SOFTWARE_CYCLE_COUNT,
        "cold_cycle_count": COLD_CYCLE_COUNT,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
    }
    for key, expected in required.items():
        if matrix.get(key) != expected:
            errors.append(f"matrix: {key} mismatch")
    matrix_id = matrix.get("matrix_id")
    if not isinstance(matrix_id, str) or not re.fullmatch(
        r"[0-9a-f]{32}", matrix_id
    ):
        errors.append("matrix: matrix_id is invalid")
        matrix_id = ""
    source = matrix.get("git")
    if not (
        isinstance(source, dict)
        and exact_commit(source.get("commit")) == commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    ):
        errors.append("matrix: source git is not the exact clean candidate")

    seed_path, seed_row_errors = validate_file_row(
        matrix.get("seed_receipt"), root, "seed receipt"
    )
    flash_path, flash_row_errors = validate_file_row(
        matrix.get("closing_flash_receipt"), root, "closing flash receipt"
    )
    errors.extend(seed_row_errors)
    errors.extend(flash_row_errors)
    baseline: dict[str, Any] | None = None
    seed: dict[str, Any] = {}
    if seed_path is not None:
        seed = load_json(seed_path, root, "seed receipt")
        baseline, seed_errors = validate_seed_receipt(
            seed,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
            expected_target=target,
            expected_target_identity_sha256=target_digest,
            expected_d1l_public_key=public_key,
        )
        errors.extend(seed_errors)
    if flash_path is not None:
        flash = load_json(flash_path, root, "closing flash receipt")
        errors.extend(
            validate_closing_flash_receipt(
                flash,
                root=root,
                commit=commit,
                run_id=run_id,
                run_attempt=run_attempt,
                witness=seed.get("retention_witness", {}),
                expected_target=target,
                expected_target_identity_sha256=target_digest,
                expected_d1l_public_key=public_key,
            )
        )

    live_projection, live_errors, _ = recompute_state_capture(
        matrix.get("post_reinstall_live_capture"), commit
    )
    errors.extend(f"post-reinstall live: {error}" for error in live_errors)
    live_identity_result, live_identity_errors = recompute_raw_command(
        matrix.get("post_reinstall_identity_status")
    )
    errors.extend(
        f"post-reinstall identity: {error}"
        for error in live_identity_errors
    )
    if (
        matrix.get("post_reinstall_identity_ok") is not True
        or not identity_status_ok(live_identity_result, public_key)
    ):
        errors.append(
            "matrix: live post-reinstall D1L identity is not pinned"
        )
    if (
        target_identity(
            matrix.get("post_reinstall_d1l_target"), target
        )
        != target_digest
    ):
        errors.append(
            "matrix: live post-reinstall D1L target identity drifted"
        )
    if baseline is not None and live_projection is not None:
        live_ok, live_state_errors = state_preserved(
            baseline, live_projection
        )
        if not live_ok:
            errors.extend(
                f"post-reinstall live: {error}"
                for error in live_state_errors
            )
        if matrix.get("post_reinstall_projection_sha256") != projection_sha256(
            live_projection
        ):
            errors.append("matrix: post-reinstall projection digest mismatch")

    cycles = matrix.get("cycle_receipts")
    if not isinstance(cycles, list) or len(cycles) != (
        SOFTWARE_CYCLE_COUNT + COLD_CYCLE_COUNT
    ):
        errors.append("matrix: cycle receipt count is not exactly eight")
        cycles = []
    seen_paths: set[str] = set()
    seen_hashes: set[str] = set()
    seen_ids: set[str] = set()
    expected_previous = (
        matrix.get("closing_flash_receipt", {}).get("sha256")
        if isinstance(matrix.get("closing_flash_receipt"), dict)
        else None
    )
    types: list[str] = []
    ordinals: dict[str, list[int]] = {"software": [], "cold": []}
    for index, row in enumerate(cycles):
        path, row_errors = validate_file_row(
            row, root, f"cycle receipt {index}"
        )
        errors.extend(row_errors)
        if not isinstance(row, dict):
            continue
        raw_path = row.get("path")
        digest = row.get("sha256")
        if raw_path in seen_paths:
            errors.append("matrix: duplicate cycle path")
        if digest in seen_hashes:
            errors.append("matrix: duplicate cycle content/hash")
        seen_paths.add(str(raw_path))
        seen_hashes.add(str(digest))
        if path is None or baseline is None:
            continue
        cycle = load_json(path, root, f"cycle receipt {index}")
        cycle_id = cycle.get("cycle_id")
        if cycle_id in seen_ids:
            errors.append("matrix: duplicate cycle_id")
        seen_ids.add(str(cycle_id))
        if cycle.get("previous_receipt_sha256") != expected_previous:
            errors.append(f"matrix: cycle {index} hash chain mismatch")
        if cycle.get("seed_receipt_sha256") != (
            matrix.get("seed_receipt", {}).get("sha256")
            if isinstance(matrix.get("seed_receipt"), dict)
            else None
        ):
            errors.append(f"matrix: cycle {index} seed hash mismatch")
        if cycle.get("closing_flash_receipt_sha256") != (
            matrix.get("closing_flash_receipt", {}).get("sha256")
            if isinstance(matrix.get("closing_flash_receipt"), dict)
            else None
        ):
            errors.append(f"matrix: cycle {index} flash hash mismatch")
        expected_previous = digest
        cycle_type = cycle.get("cycle_type")
        ordinal = cycle.get("ordinal")
        if cycle_type in ordinals and type(ordinal) is int:
            types.append(cycle_type)
            ordinals[cycle_type].append(ordinal)
        cycle_ok, cycle_errors = recompute_cycle(
            cycle,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
            matrix_id=matrix_id,
            baseline=baseline,
            expected_target=target,
            expected_target_identity_sha256=target_digest,
        )
        if not cycle_ok:
            errors.extend(
                f"cycle {cycle_type} {ordinal}: {error}"
                for error in cycle_errors
            )
    if types != (
        ["software"] * SOFTWARE_CYCLE_COUNT
        + ["cold"] * COLD_CYCLE_COUNT
    ):
        errors.append("matrix: cycle type order is invalid")
    if ordinals["software"] != list(range(1, SOFTWARE_CYCLE_COUNT + 1)):
        errors.append("matrix: software ordinals are invalid")
    if ordinals["cold"] != list(range(1, COLD_CYCLE_COUNT + 1)):
        errors.append("matrix: cold ordinals are invalid")
    if matrix.get("hash_chain_tail") != expected_previous:
        errors.append("matrix: hash chain tail mismatch")
    if matrix.get("all_child_receipts_unique") is not True:
        errors.append("matrix: uniqueness declaration is false")
    return not errors, errors, matrix


def validate_core_reboot_persistence_receipt(
    matrix_path: Path,
    *,
    root: Path,
    expected_commit: str,
    expected_run_id: str,
    expected_run_attempt: str,
) -> tuple[bool, list[str], dict[str, Any]]:
    matrix = load_json(matrix_path, root, "Core reboot persistence matrix")
    return _validate_core_reboot_persistence_report(
        matrix,
        root=root,
        expected_commit=expected_commit,
        expected_run_id=expected_run_id,
        expected_run_attempt=expected_run_attempt,
    )


def _seed_retained_state_report(
    *,
    root: Path,
    serial_module: Any,
    commit: str,
    run_id: str,
    run_attempt: str,
    timeout: float,
    source_git: dict[str, Any],
    progress: dict[str, Any],
    d1l_target: dict[str, Any],
    expected_d1l_public_key: str,
    now: Callable[[], str] = utc_now,
    clock: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    requested_target = d1l_target["requested_path"]
    expected_witness = candidate_witness_identity(commit, run_id, run_attempt)
    identity_receipt: dict[str, Any] | None = None
    identity_result: dict[str, Any] | None = None
    identity_errors: list[str] = []
    witness_receipt: dict[str, Any] | None = None
    settings_mutation: dict[str, Any] | None = None
    witness_result: dict[str, Any] | None = None
    settings_result: dict[str, Any] | None = None
    derived_witness: dict[str, Any] | None = None
    witness_errors: list[str] = []
    settings_errors: list[str] = []
    command_log = progress["partial_command_receipts"]
    progress["serial_open_attempted"] = True
    progress["stage"] = "opening_seed_serial"
    with _open_serial(serial_module, timeout, requested_target) as ser:
        progress["serial_opened"] = True
        progress["physical_observed"] = True
        progress["stage"] = "capturing_initial_state"
        if hasattr(ser, "reset_input_buffer"):
            ser.reset_input_buffer()
        initial = capture_state(
            ser,
            timeout,
            clock=clock,
            now=now,
            command_log=command_log,
        )
        progress["initial_state_capture"] = initial
        initial_projection, initial_errors, _ = recompute_state_capture(
            initial, commit
        )
        if initial_projection is not None and not initial_errors:
            progress["stage"] = "validating_live_d1l_identity"
            identity_receipt = read_raw_command(
                ser,
                "identity status",
                timeout,
                clock=clock,
                now=now,
                command_log=command_log,
            )
            progress["d1l_identity_status"] = identity_receipt
            identity_result, identity_raw_errors = recompute_raw_command(
                identity_receipt
            )
            identity_errors.extend(identity_raw_errors)
            if not identity_status_ok(
                identity_result, expected_d1l_public_key
            ):
                identity_errors.append(
                    "live identity does not match the pinned D1L public key"
                )
            if not identity_errors:
                progress["stage"] = "retained_witness_proof"
                witness_receipt = read_raw_command(
                    ser,
                    f"core retained-witness {expected_witness['token']}",
                    timeout,
                    clock=clock,
                    now=now,
                    command_log=command_log,
                )
                progress["retained_witness_proof"] = witness_receipt
                witness_result, witness_raw_errors = recompute_raw_command(
                    witness_receipt
                )
                witness_errors.extend(witness_raw_errors)
                retained_witness, retained_witness_errors = (
                    recompute_retained_witness_proof(
                        witness_result,
                        expected=expected_witness,
                        initial_projection=initial_projection,
                    )
                )
                witness_errors.extend(retained_witness_errors)
                if not witness_errors:
                    derived_witness = retained_witness
                    progress["retention_witness"] = derived_witness
                    progress["stage"] = "settings_retention_mutation"
                    progress["settings_mutation_may_have_executed"] = True
                    settings_mutation = read_raw_command(
                        ser,
                        "settings set name "
                        f"{expected_witness['settings_node_name']}",
                        timeout,
                        clock=clock,
                        now=now,
                        command_log=command_log,
                    )
                    progress["settings_retention_mutation"] = settings_mutation
                    settings_result, settings_errors = recompute_raw_command(
                        settings_mutation
                    )
                    progress["settings_mutation_confirmed_persisted"] = bool(
                        _command_ok(settings_result, "settings set name")
                        and settings_result.get("persisted") is True
                        and settings_result.get("node_name")
                        == expected_witness["settings_node_name"]
                    )
                else:
                    settings_errors.append(
                        "settings retention mutation skipped because the "
                        "candidate full-store witness proof failed"
                    )
            else:
                witness_errors.append(
                    "candidate full-store witness proof skipped because the "
                    "live D1L identity check failed"
                )
                settings_errors.append(
                    "settings retention mutation skipped because the live "
                    "D1L identity check failed"
                )
            progress["stage"] = "capturing_final_state"
            final = capture_state(
                ser,
                timeout,
                clock=clock,
                now=now,
                command_log=command_log,
            )
            progress["state_capture"] = final
        else:
            final = initial
            witness_errors.append(
                "candidate full-store witness proof skipped because initial "
                "exact candidate state failed"
            )
            settings_errors.append(
                "settings retention mutation skipped because initial exact "
                "candidate state failed"
            )
            progress["state_capture"] = final
    progress["stage"] = "recomputing_seed_report"
    projection, capture_errors, _ = recompute_state_capture(final, commit)
    errors = [
        *(f"initial: {error}" for error in initial_errors),
        *(f"identity: {error}" for error in identity_errors),
        *(f"retained witness proof: {error}" for error in witness_errors),
        *(f"settings mutation: {error}" for error in settings_errors),
        *(f"final: {error}" for error in capture_errors),
    ]
    if initial_projection is None:
        errors.append("initial exact-candidate state capture failed")
    if not (
        _command_ok(settings_result, "settings set name")
        and settings_result.get("persisted") is True
        and settings_result.get("node_name")
        == expected_witness["settings_node_name"]
    ):
        errors.append("settings retention mutation failed")
    witness = derived_witness or expected_witness
    witness_check_errors: list[str] = []
    if projection is not None and derived_witness is not None:
        witness_ok, witness_check_errors = retention_witness_check(
            projection,
            witness=derived_witness,
        )
        if not witness_ok:
            errors.extend(witness_check_errors)
        if initial_projection is not None:
            transition_ok, transition_errors = seed_store_transition_preserved(
                initial_projection,
                projection,
                witness=derived_witness,
            )
            if not transition_ok:
                errors.extend(transition_errors)
    else:
        errors.append(
            "final retained projection or raw-derived witness is unavailable"
        )
    mutation_outcome_uncertain = bool(
        progress["settings_mutation_may_have_executed"]
        and not progress["settings_mutation_confirmed_persisted"]
    )
    progress["mutation_outcome_uncertain"] = mutation_outcome_uncertain
    progress["stage"] = "complete"
    report = {
        "schema": 2,
        "kind": "core_retained_state_seed",
        "mode": "hardware",
        "ok": not errors,
        "closure_eligible": False,
        "hardware_required": True,
        "physical_observed": progress["physical_observed"],
        "port": requested_target,
        "d1l_target": d1l_target,
        "expected_target_identity_sha256": d1l_target[
            "stable_identity_sha256"
        ],
        "expected_d1l_public_key": expected_d1l_public_key,
        "d1l_identity_status": identity_receipt,
        "d1l_identity_ok": identity_status_ok(
            identity_result, expected_d1l_public_key
        ),
        "baud": D1L_BAUD,
        "commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": SD_HISTORY_MODE,
        "git": source_git,
        "captured_at": now(),
        "retention_witness": witness,
        "initial_state_capture": initial,
        "retained_witness_proof": witness_receipt,
        "settings_retention_mutation": settings_mutation,
        "state_capture": final,
        "producer_io": {
            key: progress[key]
            for key in (
                "stage",
                "serial_open_attempted",
                "serial_opened",
                "physical_observed",
                "settings_mutation_may_have_executed",
                "settings_mutation_confirmed_persisted",
                "mutation_outcome_uncertain",
            )
        },
        "mutation_outcome_uncertain": mutation_outcome_uncertain,
        "projection_sha256": (
            projection_sha256(projection) if projection is not None else None
        ),
        "checks": {
            "exact_candidate": not initial_errors and not capture_errors,
            "live_d1l_identity": not identity_errors,
            "candidate_full_store_witness_set_proven": (
                derived_witness is not None and not witness_errors
            ),
            "settings_retention_mutation_persisted": bool(
                progress["settings_mutation_confirmed_persisted"]
                and not settings_errors
            ),
            "retained_public_and_dm_witnesses_present": (
                projection is not None
                and derived_witness is not None
                and not witness_check_errors
            ),
            "canonical_contact_witness_present": (
                projection is not None
                and derived_witness is not None
                and not witness_check_errors
            ),
            "errors": errors,
        },
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "sd_access": False,
        "rp2040_access": False,
        "formats_sd": False,
        "predecessor_evidence_used": False,
    }
    return report


def seed_retained_state(
    *,
    root: Path,
    out: Path,
    serial_module: Any,
    commit: str,
    run_id: str,
    run_attempt: str,
    timeout: float,
    source_git: dict[str, Any],
    port: str,
    port_lister: Callable[[], Iterable[object]],
    expected_d1l_public_key: str,
    platform_name: str | None = None,
    now: Callable[[], str] = utc_now,
    clock: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    normalized_public_key = exact_public_key(expected_d1l_public_key)
    if normalized_public_key is None:
        raise ValueError(
            "expected D1L public key must be an exact 64-hex value"
        )
    _, output_handle = reserve_json_output(out, root)
    progress: dict[str, Any] = {
        "stage": "reserved_before_io",
        "serial_open_attempted": False,
        "serial_opened": False,
        "physical_observed": False,
        "settings_mutation_may_have_executed": False,
        "settings_mutation_confirmed_persisted": False,
        "mutation_outcome_uncertain": False,
        "partial_command_receipts": [],
        "d1l_target": None,
    }
    try:
        progress["stage"] = "resolving_d1l_target"
        d1l_target = resolve_core_target(
            port,
            port_lister=port_lister,
            platform_name=platform_name,
        )
        progress["d1l_target"] = d1l_target
        report = _seed_retained_state_report(
            root=root,
            serial_module=serial_module,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
            timeout=timeout,
            source_git=source_git,
            progress=progress,
            d1l_target=d1l_target,
            expected_d1l_public_key=normalized_public_key,
            now=now,
            clock=clock,
        )
    except BaseException as exc:
        mutation_outcome_uncertain = bool(
            progress["settings_mutation_may_have_executed"]
            and not progress["settings_mutation_confirmed_persisted"]
        )
        progress["mutation_outcome_uncertain"] = mutation_outcome_uncertain
        failure = {
            "schema": 2,
            "kind": "core_retained_state_seed",
            "mode": "hardware",
            "ok": False,
            "closure_eligible": False,
            "hardware_required": True,
            "physical_observed": bool(
                progress["physical_observed"] or progress["serial_opened"]
            ),
            "port": normalize_port(port),
            "d1l_target": progress.get("d1l_target"),
            "expected_target_identity_sha256": (
                progress.get("d1l_target", {}).get(
                    "stable_identity_sha256"
                )
                if isinstance(progress.get("d1l_target"), dict)
                else None
            ),
            "expected_d1l_public_key": normalized_public_key,
            "d1l_identity_status": progress.get("d1l_identity_status"),
            "d1l_identity_ok": False,
            "baud": D1L_BAUD,
            "commit": commit,
            "github_actions_run": run_id,
            "workflow_run_attempt": run_attempt,
            "release_profile": CORE_RELEASE_PROFILE,
            "sd_history_mode": SD_HISTORY_MODE,
            "git": source_git,
            "captured_at": now(),
            "retention_witness": progress.get(
                "retention_witness",
                candidate_witness_identity(commit, run_id, run_attempt),
            ),
            "initial_state_capture": progress.get("initial_state_capture"),
            "retained_witness_proof": progress.get(
                "retained_witness_proof"
            ),
            "settings_retention_mutation": progress.get(
                "settings_retention_mutation"
            ),
            "state_capture": progress.get("state_capture"),
            "partial_command_receipts": progress[
                "partial_command_receipts"
            ],
            "producer_io": {
                key: progress[key]
                for key in (
                    "stage",
                    "serial_open_attempted",
                    "serial_opened",
                    "physical_observed",
                    "settings_mutation_may_have_executed",
                    "settings_mutation_confirmed_persisted",
                    "mutation_outcome_uncertain",
                )
            },
            "mutation_outcome_uncertain": mutation_outcome_uncertain,
            "failure": {
                "stage": progress["stage"],
                "type": type(exc).__name__,
                "detail": str(exc),
            },
            "public_rf_tx": False,
            "dm_rf_tx": False,
            "sd_access": False,
            "rp2040_access": False,
            "formats_sd": False,
            "predecessor_evidence_used": False,
        }
        try:
            finalize_json_output(output_handle, failure)
        except BaseException as finalize_exc:
            raise RuntimeError(
                "seed operation failed and its reserved failure receipt "
                "could not be finalized; the exclusive reservation was retained"
            ) from finalize_exc
        raise
    finalize_json_output(output_handle, report)
    return report


def _verify_reboot_matrix_after_reservation(
    *,
    root: Path,
    final_handle: Any,
    cycle_reservations: dict[
        tuple[str, int], tuple[Path, Any]
    ],
    execution: dict[str, Any],
    seed_path: Path,
    flash_path: Path,
    serial_module: Any,
    port_lister: Callable[[], Iterable[object]],
    prompt: Callable[[str], str],
    commit: str,
    run_id: str,
    run_attempt: str,
    timeout: float,
    transition_timeout: float,
    port_timeout: float,
    port_poll_sec: float,
    minimum_power_off_sec: float,
    source_git: dict[str, Any],
    d1l_target: dict[str, Any],
    expected_d1l_public_key: str,
    platform_name: str | None = None,
    now: Callable[[], str] = utc_now,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    requested_target = d1l_target["requested_path"]
    target_digest = d1l_target["stable_identity_sha256"]
    execution["stage"] = "validating_seed_receipt"
    seed = load_json(seed_path, root, "seed receipt")
    baseline, seed_errors = validate_seed_receipt(
        seed,
        commit=commit,
        run_id=run_id,
        run_attempt=run_attempt,
        expected_target=requested_target,
        expected_target_identity_sha256=target_digest,
        expected_d1l_public_key=expected_d1l_public_key,
    )
    if seed_errors or baseline is None:
        raise ValueError("seed receipt failed validation: " + "; ".join(seed_errors))
    execution["stage"] = "validating_closing_flash_receipt"
    flash = load_json(flash_path, root, "closing flash receipt")
    flash_errors = validate_closing_flash_receipt(
        flash,
        root=root,
        commit=commit,
        run_id=run_id,
        run_attempt=run_attempt,
        witness=seed.get("retention_witness", {}),
        expected_target=requested_target,
        expected_target_identity_sha256=target_digest,
        expected_d1l_public_key=expected_d1l_public_key,
    )
    if flash_errors:
        raise ValueError(
            "closing flash receipt failed validation: " + "; ".join(flash_errors)
        )
    seed_row = relative_file_row(seed_path, root, "seed receipt")
    flash_row = relative_file_row(flash_path, root, "closing flash receipt")

    # Re-resolve and pin the live identity immediately before any reboot.
    execution["stage"] = "resolving_post_reinstall_d1l_target"
    live_target = resolve_core_target(
        requested_target,
        port_lister=port_lister,
        platform_name=platform_name,
    )
    if live_target["stable_identity_sha256"] != target_digest:
        raise ValueError(
            "post-reinstall D1L target identity drifted before serial open"
        )
    execution["post_reinstall_d1l_target"] = live_target
    execution["serial_open_attempted"] = True
    execution["stage"] = "opening_post_reinstall_serial"
    with _open_serial(serial_module, timeout, requested_target) as ser:
        execution["serial_opened"] = True
        execution["physical_observed"] = True
        if hasattr(ser, "reset_input_buffer"):
            ser.reset_input_buffer()
        execution["stage"] = "capturing_post_reinstall_identity"
        live_identity_receipt = read_raw_command(
            ser,
            "identity status",
            timeout,
            clock=clock,
            now=now,
            command_log=execution["partial_command_receipts"],
        )
        execution["post_reinstall_identity_status"] = (
            live_identity_receipt
        )
        live_identity_result, live_identity_errors = recompute_raw_command(
            live_identity_receipt
        )
        if live_identity_errors or not identity_status_ok(
            live_identity_result, expected_d1l_public_key
        ):
            raise ValueError(
                "post-reinstall live identity does not match the pinned D1L"
            )
        execution["stage"] = "capturing_post_reinstall_state"
        live_capture = capture_state(
            ser,
            timeout,
            clock=clock,
            now=now,
            command_log=execution["partial_command_receipts"],
        )
        execution["post_reinstall_live_capture"] = live_capture
    live_projection, live_errors, _ = recompute_state_capture(
        live_capture, commit
    )
    if live_projection is None:
        raise ValueError(
            "post-reinstall live state capture failed: " + "; ".join(live_errors)
        )
    live_preserved, live_state_errors = state_preserved(
        baseline, live_projection
    )
    if live_errors or not live_preserved:
        raise ValueError(
            "post-reinstall live state does not preserve the seed: "
            + "; ".join([*live_errors, *live_state_errors])
        )

    execution["stage"] = "running_reboot_cycles"
    matrix_id = uuid.uuid4().hex
    previous_sha = flash_row["sha256"]
    cycle_rows: list[dict[str, Any]] = []
    execution["cycle_receipts"] = cycle_rows
    all_cycles_ok = True
    for ordinal in range(1, SOFTWARE_CYCLE_COUNT + 1):
        key = ("software", ordinal)
        child, child_handle = cycle_reservations[key]
        cycle = _cycle_base(
            matrix_id=matrix_id,
            cycle_type="software",
            ordinal=ordinal,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
            seed_sha256=seed_row["sha256"],
            flash_sha256=flash_row["sha256"],
            previous_sha256=previous_sha,
            d1l_target=d1l_target,
        )
        execution["current_cycle_key"] = key
        execution["current_cycle_path"] = str(child)
        execution["current_cycle"] = cycle
        cycle = run_software_cycle(
            serial_module=serial_module,
            port_lister=port_lister,
            timeout=timeout,
            transition_timeout=transition_timeout,
            commit=commit,
            baseline=baseline,
            report=cycle,
            requested_target=requested_target,
            expected_target_identity_sha256=target_digest,
            platform_name=platform_name,
            clock=clock,
            now=now,
            sleep=sleep,
        )
        execution["physical_observed"] = bool(
            execution["physical_observed"]
            or cycle.get("physical_observed") is True
        )
        finalize_json_output(child_handle, cycle)
        row = relative_file_row(child, root, f"software cycle {ordinal}")
        cycle_rows.append(row)
        previous_sha = row["sha256"]
        execution["current_cycle_key"] = None
        execution["current_cycle_path"] = None
        execution["current_cycle"] = None
        all_cycles_ok = all_cycles_ok and cycle.get("ok") is True
        if not cycle.get("ok"):
            break

    if all_cycles_ok:
        for ordinal in range(1, COLD_CYCLE_COUNT + 1):
            key = ("cold", ordinal)
            child, child_handle = cycle_reservations[key]
            cycle = _cycle_base(
                matrix_id=matrix_id,
                cycle_type="cold",
                ordinal=ordinal,
                commit=commit,
                run_id=run_id,
                run_attempt=run_attempt,
                seed_sha256=seed_row["sha256"],
                flash_sha256=flash_row["sha256"],
                previous_sha256=previous_sha,
                d1l_target=d1l_target,
            )
            execution["current_cycle_key"] = key
            execution["current_cycle_path"] = str(child)
            execution["current_cycle"] = cycle
            cycle = run_cold_cycle(
                serial_module=serial_module,
                port_lister=port_lister,
                prompt=prompt,
                timeout=timeout,
                port_timeout=port_timeout,
                port_poll_sec=port_poll_sec,
                minimum_power_off_sec=minimum_power_off_sec,
                commit=commit,
                baseline=baseline,
                report=cycle,
                requested_target=requested_target,
                expected_target_identity_sha256=target_digest,
                platform_name=platform_name,
                clock=clock,
                now=now,
                sleep=sleep,
            )
            execution["physical_observed"] = bool(
                execution["physical_observed"]
                or cycle.get("physical_observed") is True
            )
            finalize_json_output(child_handle, cycle)
            row = relative_file_row(child, root, f"cold cycle {ordinal}")
            cycle_rows.append(row)
            previous_sha = row["sha256"]
            execution["current_cycle_key"] = None
            execution["current_cycle_path"] = None
            execution["current_cycle"] = None
            all_cycles_ok = all_cycles_ok and cycle.get("ok") is True
            if not cycle.get("ok"):
                break

    finalize_unused_cycle_reservations(
        cycle_reservations,
        commit=commit,
        run_id=run_id,
        run_attempt=run_attempt,
        reason=(
            "not executed because an earlier reboot/persistence cycle "
            "did not close"
        ),
        requested_target=requested_target,
        d1l_target=d1l_target,
    )
    complete = len(cycle_rows) == SOFTWARE_CYCLE_COUNT + COLD_CYCLE_COUNT
    execution["stage"] = "assembling_matrix"
    report = {
        "schema": 2,
        "kind": "core_reboot_persistence_matrix",
        "mode": "hardware",
        "ok": bool(all_cycles_ok and complete),
        "closure_eligible": bool(all_cycles_ok and complete),
        "hardware_required": True,
        "physical_observed": bool(execution["physical_observed"]),
        "matrix_id": matrix_id,
        "port": requested_target,
        "d1l_target": d1l_target,
        "expected_target_identity_sha256": target_digest,
        "expected_d1l_public_key": expected_d1l_public_key,
        "baud": D1L_BAUD,
        "commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": SD_HISTORY_MODE,
        "claim": CLAIM,
        "cross_version_migration_proven": False,
        "predecessor_evidence_used": False,
        "git": source_git,
        "seed_receipt": seed_row,
        "closing_flash_receipt": flash_row,
        "post_reinstall_d1l_target": live_target,
        "post_reinstall_identity_status": live_identity_receipt,
        "post_reinstall_identity_ok": True,
        "post_reinstall_live_capture": live_capture,
        "post_reinstall_projection_sha256": projection_sha256(live_projection),
        "software_cycle_count": SOFTWARE_CYCLE_COUNT,
        "cold_cycle_count": COLD_CYCLE_COUNT,
        "cycle_receipts": cycle_rows,
        "all_child_receipts_unique": len(
            {row["sha256"] for row in cycle_rows}
        )
        == len(cycle_rows),
        "hash_chain_tail": previous_sha,
        "started_at": seed.get("captured_at"),
        "ended_at": now(),
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
    }
    if report["ok"]:
        execution["stage"] = "validating_matrix_before_finalize"
        validated, validation_errors, _ = _validate_core_reboot_persistence_report(
            report,
            root=root,
            expected_commit=commit,
            expected_run_id=run_id,
            expected_run_attempt=run_attempt,
        )
        if not validated:
            report["ok"] = False
            report["closure_eligible"] = False
            report["producer_validation_errors"] = validation_errors
            finalize_json_output(final_handle, report)
            raise ValueError(
                "reboot matrix failed validation before finalization: "
                + "; ".join(validation_errors)
            )
    execution["stage"] = "finalizing_matrix"
    finalize_json_output(final_handle, report)
    execution["stage"] = "complete"
    return report


def _failed_cycle_receipt(
    cycle: dict[str, Any],
    exc: BaseException,
    *,
    now: Callable[[], str],
) -> dict[str, Any]:
    possible_action = bool(
        cycle.get("reboot_or_power_action_may_have_executed")
    )
    cycle["ok"] = False
    cycle["closure_eligible"] = False
    cycle["physical_observed"] = bool(
        cycle.get("physical_observed") or cycle.get("serial_opened")
    )
    cycle["physical_state_outcome_uncertain"] = possible_action
    cycle["mutation_outcome_uncertain"] = possible_action
    cycle["failure"] = {
        "stage": cycle.get("stage"),
        "type": type(exc).__name__,
        "detail": str(exc),
    }
    cycle["ended_at"] = now()
    return cycle


def verify_reboot_matrix(
    *,
    root: Path,
    out: Path,
    seed_path: Path,
    flash_path: Path,
    serial_module: Any,
    port_lister: Callable[[], Iterable[object]],
    prompt: Callable[[str], str],
    commit: str,
    run_id: str,
    run_attempt: str,
    timeout: float,
    transition_timeout: float,
    port_timeout: float,
    port_poll_sec: float,
    minimum_power_off_sec: float,
    source_git: dict[str, Any],
    port: str,
    expected_d1l_public_key: str,
    platform_name: str | None = None,
    now: Callable[[], str] = utc_now,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    if (
        exact_commit(commit) is None
        or not positive_decimal(run_id)
        or not positive_decimal(run_attempt)
    ):
        raise ValueError("reboot matrix candidate identity is invalid")
    requested_target = enforce_core_port(port)
    normalized_public_key = exact_public_key(expected_d1l_public_key)
    if normalized_public_key is None:
        raise ValueError(
            "expected D1L public key must be an exact 64-hex value"
        )

    # The final matrix and all eight child receipts are exclusively reserved
    # before any target enumeration/open, reboot command, or cold-cycle prompt.
    _, final_handle, cycle_reservations = reserve_reboot_outputs(
        out, root
    )
    execution: dict[str, Any] = {
        "stage": "all_outputs_reserved_before_io",
        "serial_open_attempted": False,
        "serial_opened": False,
        "physical_observed": False,
        "partial_command_receipts": [],
        "post_reinstall_d1l_target": None,
        "post_reinstall_identity_status": None,
        "post_reinstall_live_capture": None,
        "cycle_receipts": [],
        "current_cycle_key": None,
        "current_cycle_path": None,
        "current_cycle": None,
        "d1l_target": None,
    }
    try:
        execution["stage"] = "resolving_d1l_target_before_any_io"
        d1l_target = resolve_core_target(
            requested_target,
            port_lister=port_lister,
            platform_name=platform_name,
        )
        execution["d1l_target"] = d1l_target
        return _verify_reboot_matrix_after_reservation(
            root=root,
            final_handle=final_handle,
            cycle_reservations=cycle_reservations,
            execution=execution,
            seed_path=seed_path,
            flash_path=flash_path,
            serial_module=serial_module,
            port_lister=port_lister,
            prompt=prompt,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
            timeout=timeout,
            transition_timeout=transition_timeout,
            port_timeout=port_timeout,
            port_poll_sec=port_poll_sec,
            minimum_power_off_sec=minimum_power_off_sec,
            source_git=source_git,
            d1l_target=d1l_target,
            expected_d1l_public_key=normalized_public_key,
            platform_name=platform_name,
            now=now,
            clock=clock,
            sleep=sleep,
        )
    except BaseException as exc:
        receipt_finalize_errors: list[str] = []
        current_key = execution.get("current_cycle_key")
        current_cycle = execution.get("current_cycle")
        if (
            isinstance(current_key, tuple)
            and current_key in cycle_reservations
            and isinstance(current_cycle, dict)
        ):
            _, current_handle = cycle_reservations[current_key]
            execution["physical_observed"] = bool(
                execution["physical_observed"]
                or current_cycle.get("physical_observed")
                or current_cycle.get("serial_opened")
            )
            if not current_handle.closed:
                try:
                    finalize_json_output(
                        current_handle,
                        _failed_cycle_receipt(
                            current_cycle, exc, now=now
                        ),
                    )
                except BaseException as finalize_exc:
                    receipt_finalize_errors.append(
                        "current cycle failure receipt: "
                        f"{type(finalize_exc).__name__}: {finalize_exc}"
                    )
        try:
            finalize_unused_cycle_reservations(
                cycle_reservations,
                commit=commit,
                run_id=run_id,
                run_attempt=run_attempt,
                reason=(
                    "not executed because the verify producer failed before "
                    "this reserved cycle"
                ),
                requested_target=requested_target,
                d1l_target=(
                    execution.get("d1l_target")
                    if isinstance(execution.get("d1l_target"), dict)
                    else None
                ),
            )
        except BaseException as finalize_exc:
            receipt_finalize_errors.append(
                "unused cycle reservations: "
                f"{type(finalize_exc).__name__}: {finalize_exc}"
            )

        possible_action = bool(
            isinstance(current_cycle, dict)
            and current_cycle.get(
                "reboot_or_power_action_may_have_executed"
            )
        )
        failure = {
            "schema": 2,
            "kind": "core_reboot_persistence_matrix",
            "mode": "hardware",
            "ok": False,
            "closure_eligible": False,
            "hardware_required": True,
            "physical_observed": bool(
                execution["physical_observed"]
                or execution["serial_opened"]
            ),
            "port": requested_target,
            "d1l_target": execution.get("d1l_target"),
            "expected_target_identity_sha256": (
                execution.get("d1l_target", {}).get(
                    "stable_identity_sha256"
                )
                if isinstance(execution.get("d1l_target"), dict)
                else None
            ),
            "expected_d1l_public_key": normalized_public_key,
            "baud": D1L_BAUD,
            "commit": commit,
            "github_actions_run": run_id,
            "workflow_run_attempt": run_attempt,
            "release_profile": CORE_RELEASE_PROFILE,
            "sd_history_mode": SD_HISTORY_MODE,
            "claim": CLAIM,
            "git": source_git,
            "post_reinstall_live_capture": execution.get(
                "post_reinstall_live_capture"
            ),
            "post_reinstall_d1l_target": execution.get(
                "post_reinstall_d1l_target"
            ),
            "post_reinstall_identity_status": execution.get(
                "post_reinstall_identity_status"
            ),
            "post_reinstall_identity_ok": False,
            "partial_command_receipts": execution[
                "partial_command_receipts"
            ],
            "cycle_receipts": execution["cycle_receipts"],
            "current_cycle_path": execution.get("current_cycle_path"),
            "current_cycle": current_cycle,
            "producer_io": {
                "stage": execution["stage"],
                "serial_open_attempted": execution[
                    "serial_open_attempted"
                ],
                "serial_opened": execution["serial_opened"],
                "physical_observed": execution["physical_observed"],
            },
            "reboot_or_power_action_may_have_executed": possible_action,
            "physical_state_outcome_uncertain": possible_action,
            "mutation_outcome_uncertain": possible_action,
            "failure": {
                "stage": execution["stage"],
                "type": type(exc).__name__,
                "detail": str(exc),
                "receipt_finalize_errors": receipt_finalize_errors,
            },
            "ended_at": now(),
            "public_rf_tx": False,
            "dm_rf_tx": False,
            "formats_sd": False,
            "predecessor_evidence_used": False,
        }
        if not final_handle.closed:
            try:
                finalize_json_output(final_handle, failure)
            except BaseException as finalize_exc:
                raise RuntimeError(
                    "verify operation failed and its reserved final failure "
                    "receipt could not be finalized; all exclusive "
                    "reservations were retained"
                ) from finalize_exc
        raise


def _resolve(root: Path, value: str, *, for_output: bool = False) -> Path:
    path = Path(value)
    joined = path if path.is_absolute() else root / path
    return _lexical_absolute(joined) if for_output else joined.resolve()


def _serial_runtime() -> tuple[Any, Callable[[], Iterable[object]]]:
    try:
        import serial
        import serial.tools.list_ports
    except ImportError as exc:
        raise RuntimeError(
            "pyserial is required for Core reboot/persistence evidence"
        ) from exc

    def port_lister() -> Iterable[object]:
        return serial.tools.list_ports.comports(include_links=True)

    return serial, port_lister


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--port", default=D1L_CORE_PORT)
    parser.add_argument("--baud", type=int, default=D1L_BAUD)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--expected-d1l-public-key", required=True)
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-attempt", required=True)
    parser.add_argument("--timeout", type=float, default=8.0)
    subparsers = parser.add_subparsers(dest="operation", required=True)

    seed = subparsers.add_parser("seed")
    seed.add_argument("--out")

    verify = subparsers.add_parser("verify")
    verify.add_argument("--seed-receipt", required=True)
    verify.add_argument("--closing-flash-receipt", required=True)
    verify.add_argument("--transition-timeout", type=float, default=20.0)
    verify.add_argument("--port-timeout", type=float, default=120.0)
    verify.add_argument("--port-poll-sec", type=float, default=0.25)
    verify.add_argument(
        "--minimum-power-off-sec",
        type=float,
        default=MINIMUM_POWER_OFF_SEC,
    )
    verify.add_argument("--out")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        port = enforce_core_port(args.port)
        if args.baud != D1L_BAUD:
            raise ValueError(
                f"only {D1L_BAUD} baud is permitted"
            )
        public_key = exact_public_key(args.expected_d1l_public_key)
        if public_key is None:
            raise ValueError(
                "--expected-d1l-public-key must be an exact 64-hex value"
            )
        commit = exact_commit(args.commit)
        if commit is None:
            raise ValueError("--commit must be an exact 40-character SHA")
        run_id = str(args.github_run_id)
        run_attempt = str(args.github_run_attempt)
        if (
            not positive_decimal(run_id)
            or not positive_decimal(run_attempt)
        ):
            raise ValueError(
                "--github-run-id and "
                "--github-run-attempt must be a positive integer"
            )
        if args.timeout <= 0:
            raise ValueError("--timeout must be positive")
        root = Path(args.root).resolve(strict=True)
        source_git = exact_source_git(root, commit)
        serial_module, port_lister = _serial_runtime()
        target_slug = safe_slug(port)
        default_name = (
            f"core_retained_seed_{commit[:7]}.json"
            if args.operation == "seed"
            else f"core_reboot_persistence_{commit[:7]}.json"
        )
        output = _resolve(
            root,
            args.out
            or str(
                Path("artifacts")
                / "hardware"
                / target_slug
                / default_name
            ),
            for_output=True,
        )
        if args.operation == "seed":
            report = seed_retained_state(
                root=root,
                out=output,
                serial_module=serial_module,
                commit=commit,
                run_id=run_id,
                run_attempt=run_attempt,
                timeout=args.timeout,
                source_git=source_git,
                port=port,
                port_lister=port_lister,
                expected_d1l_public_key=public_key,
            )
        else:
            if (
                args.transition_timeout <= 0
                or args.port_timeout <= 0
                or args.port_poll_sec <= 0
                or args.minimum_power_off_sec < MINIMUM_POWER_OFF_SEC
            ):
                raise ValueError(
                    "transition/port timeouts must be positive and cold power-off "
                    f"must be at least {MINIMUM_POWER_OFF_SEC:.1f}s"
                )
            report = verify_reboot_matrix(
                root=root,
                out=output,
                seed_path=_resolve(root, args.seed_receipt),
                flash_path=_resolve(root, args.closing_flash_receipt),
                serial_module=serial_module,
                port_lister=port_lister,
                prompt=input,
                commit=commit,
                run_id=run_id,
                run_attempt=run_attempt,
                timeout=args.timeout,
                transition_timeout=args.transition_timeout,
                port_timeout=args.port_timeout,
                port_poll_sec=args.port_poll_sec,
                minimum_power_off_sec=args.minimum_power_off_sec,
                source_git=source_git,
                port=port,
                expected_d1l_public_key=public_key,
            )
    except (OSError, RuntimeError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(
        json.dumps(
            {
                "ok": report.get("ok") is True,
                "kind": report.get("kind"),
                "out": str(output),
            },
            indent=2,
        )
    )
    return 0 if report.get("ok") is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
