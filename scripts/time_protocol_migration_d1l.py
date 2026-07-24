#!/usr/bin/env python3
"""Migrate one exact D1L legacy protocol timestamp without inferring wall time.

The firmware deliberately treats the retained ``mesh_ts`` value as a lower
bound.  This runner supplies a conservative exact-device upper bound only
after:

* the exact clean Core candidate and Actions identity are established;
* the exact cross-platform D1L target is uniquely present;
* the live device reports the pinned full D1L public key and companion role;
* the live device reports the expected legacy value and a TX block; and
* the requested bound covers a deliberately pessimistic availability-window
  calculation for every predecessor timestamp allocation.

The outbound command is copied into a wipeable wire buffer.  The confirmation
phrase is never included in JSON/terminal output, and any device echo that
contains it is retained only as size and SHA-256 metadata.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
import re
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable

try:
    from core_reboot_persistence_d1l import (
        D1L_BAUD,
        exact_commit,
        exact_source_git,
        positive_decimal,
    )
    from capture_core_actions_run_d1l import (
        REPOSITORY as CORE_REPOSITORY,
        validate_capture_receipt,
    )
    from d1l_serial_target import (
        POSIX_D1L_TARGET,
        WINDOWS_D1L_TARGET,
        resolve_target,
        safe_slug,
        validate_snapshot,
    )
    from smoke_d1l import open_d1l_serial, parse_jsonl_line
    from verify_checksums import is_link_or_reparse, sha256_file
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.core_reboot_persistence_d1l import (
        D1L_BAUD,
        exact_commit,
        exact_source_git,
        positive_decimal,
    )
    from scripts.capture_core_actions_run_d1l import (
        REPOSITORY as CORE_REPOSITORY,
        validate_capture_receipt,
    )
    from scripts.d1l_serial_target import (
        POSIX_D1L_TARGET,
        WINDOWS_D1L_TARGET,
        resolve_target,
        safe_slug,
        validate_snapshot,
    )
    from scripts.smoke_d1l import open_d1l_serial, parse_jsonl_line
    from scripts.verify_checksums import is_link_or_reparse, sha256_file


D1L_CORE_PORT = WINDOWS_D1L_TARGET
D1L_CORE_POSIX_TARGET = POSIX_D1L_TARGET
CORE_RELEASE_PROFILE = "core_1_0"
CORE_SD_HISTORY_MODE = "disabled"
EXPECTED_IDF_VERSION = "v5.5.4"
UINT32_MAX = (1 << 32) - 1
PROTOCOL_RESERVATION_SIZE = 64
MAX_CONFIRMED_UPPER_BOUND = UINT32_MAX - PROTOCOL_RESERVATION_SIZE
FIRST_TIMESTAMP_COMMIT = "62207fee894ddd4e3b733f56912cf8822ae875ab"
FIRST_TIMESTAMP_AUTHORED_AT = "2026-06-29T06:56:55-04:00"
FIRST_TIMESTAMP_SOURCE = "main/app/settings_model.c"
FIRST_TX_SOURCE = "main/mesh/meshcore_service.c"
FIRST_TIMESTAMP_SOURCE_BLOB = "5da6cccedd42eb7c752b13352eb4f41cb7e65f73"
FIRST_TX_SOURCE_BLOB = "c78159bb3d517f2d273f981acd0fe34494ef8e0a"
PROTOCOL_POLICY_SOURCE = "main/platform/time_service_core.h"
PROTOCOL_POLICY_SOURCE_BLOB = "56d8897b51acf2f5bf7cc0236f4cabbda5879ccc"
PROTOCOL_POLICY_SOURCE_BLOBS = (
    (PROTOCOL_POLICY_SOURCE, PROTOCOL_POLICY_SOURCE_BLOB),
    (
        "main/platform/time_service_core.c",
        "44ee6fd6a9138463ddfba34bd8a220587426fb18",
    ),
    (
        "main/platform/time_service.h",
        "e754598c2d9d05a41f70433a3d97200b58e538ea",
    ),
    (
        "main/platform/time_service.c",
        "3d8ece05655fb769309310e0ed69c6ea41c71fb0",
    ),
    (
        "main/app/settings_protocol_migration.h",
        "3713cd15005c4701b32617702211a83f999488b4",
    ),
    (
        "main/app/settings_protocol_migration.c",
        "717e905d7c16fb3c30d516987b781e0b423a638c",
    ),
    (
        "main/app/settings_model.c",
        "988aa8b8abbb21efcc0d173a5ff92ebef3ee99cf",
    ),
)
MAX_PREDECESSOR_ALLOCATIONS_PER_SECOND = 1000
CONFIRMATION = "CONFIRM-EXACT-DEVICE-PROTOCOL-UPPER-BOUND"
MAX_RAW_LINE_BYTES = 131072
REJECTED_RECEIPT_TRUE_FLAGS = (
    "dry_run",
    "simulated",
    "simulation",
    "source_inspection",
    "source_only",
    "fabricated",
    "edited",
    "predecessor",
)
REJECTED_RECEIPT_PRESENT_FIELDS = (
    "execution_error",
    "validation_errors",
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def parse_utc(value: object) -> datetime | None:
    if not isinstance(value, str) or not value:
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None:
        return None
    return parsed.astimezone(timezone.utc)


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _positive_ascii_decimal(value: object) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[1-9][0-9]*", value) is not None


def _finite_positive_timeout(value: object) -> bool:
    if type(value) not in (int, float):
        return False
    try:
        return math.isfinite(value) and value > 0
    except (OverflowError, TypeError, ValueError):
        return False


def exact_public_key(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    return (
        normalized
        if re.fullmatch(r"[0-9a-f]{64}", normalized) is not None
        else None
    )


def identity_status_ok(result: object, expected_public_key: object) -> bool:
    """Require the complete live D1L identity before persistent mutation."""

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


def _authorized_target(value: object) -> str | None:
    if type(value) is not str:
        return None
    if value in {WINDOWS_D1L_TARGET, POSIX_D1L_TARGET}:
        return value
    return None


def _default_target() -> str:
    return (
        WINDOWS_D1L_TARGET
        if os.name == "nt"
        else POSIX_D1L_TARGET
    )


def enforce_core_port(value: object) -> str:
    target = _authorized_target(value)
    if target is None:
        raise ValueError("port is not an authorized canonical D1L target")
    return target


def _exact_json_scalar(actual: object, expected: object) -> bool:
    return type(actual) is type(expected) and actual == expected


def _json_exact_equal(left: object, right: object) -> bool:
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(
            _json_exact_equal(left[key], right[key]) for key in left
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _json_exact_equal(a, b) for a, b in zip(left, right)
        )
    return left == right


def _value_contains_confirmation(value: object) -> bool:
    marker = CONFIRMATION
    if isinstance(value, str):
        return marker in value
    if isinstance(value, (bytes, bytearray)):
        return marker.encode("ascii") in bytes(value)
    if isinstance(value, dict):
        return any(
            _value_contains_confirmation(key) or _value_contains_confirmation(item)
            for key, item in value.items()
        )
    if isinstance(value, (list, tuple)):
        return any(_value_contains_confirmation(item) for item in value)
    return False


def _git_text(root: Path, *args: str) -> str:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        raise ValueError(f"git metadata query failed: {' '.join(args)}") from exc
    value = result.stdout.strip()
    if not value:
        raise ValueError(f"git metadata query returned no value: {' '.join(args)}")
    return value


def predecessor_source_metadata(root: Path, candidate_commit: str) -> dict[str, Any]:
    try:
        subprocess.run(
            [
                "git",
                "merge-base",
                "--is-ancestor",
                FIRST_TIMESTAMP_COMMIT,
                candidate_commit,
            ],
            cwd=root,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        raise ValueError(
            "the first timestamp implementation is not an ancestor of the candidate"
        ) from exc
    authored_at = _git_text(
        root,
        "show",
        "-s",
        "--format=%aI",
        FIRST_TIMESTAMP_COMMIT,
    )
    if parse_utc(authored_at) is None:
        raise ValueError("first timestamp commit time is invalid")
    candidate_authored_at = _git_text(
        root,
        "show",
        "-s",
        "--format=%aI",
        candidate_commit,
    )
    candidate_committed_at = _git_text(
        root,
        "show",
        "-s",
        "--format=%cI",
        candidate_commit,
    )
    candidate_authored = parse_utc(candidate_authored_at)
    candidate_committed = parse_utc(candidate_committed_at)
    if candidate_authored is None or candidate_committed is None:
        raise ValueError("candidate commit time is invalid")
    candidate_not_before = max(candidate_authored, candidate_committed)
    settings_blob = _git_text(
        root,
        "rev-parse",
        f"{FIRST_TIMESTAMP_COMMIT}:{FIRST_TIMESTAMP_SOURCE}",
    )
    tx_blob = _git_text(
        root,
        "rev-parse",
        f"{FIRST_TIMESTAMP_COMMIT}:{FIRST_TX_SOURCE}",
    )
    if (
        authored_at != FIRST_TIMESTAMP_AUTHORED_AT
        or settings_blob != FIRST_TIMESTAMP_SOURCE_BLOB
        or tx_blob != FIRST_TX_SOURCE_BLOB
    ):
        raise ValueError("first timestamp source identity does not match policy")
    policy_path = root / PROTOCOL_POLICY_SOURCE
    try:
        policy_text = policy_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ValueError("candidate protocol reservation policy is unreadable") from exc
    matches = re.findall(
        r"^\s*#define\s+D1L_TIME_PROTOCOL_RESERVATION_SIZE\s+(\d+)U\s*$",
        policy_text,
        flags=re.MULTILINE,
    )
    if matches != [str(PROTOCOL_RESERVATION_SIZE)]:
        raise ValueError("candidate protocol reservation policy is unexpected")
    policy_files: list[dict[str, str]] = []
    for path, expected_blob in PROTOCOL_POLICY_SOURCE_BLOBS:
        blob = _git_text(root, "rev-parse", f"{candidate_commit}:{path}")
        if blob != expected_blob:
            raise ValueError(
                f"candidate protocol policy identity does not match policy: {path}"
            )
        policy_files.append({"path": path, "blob": blob})
    return {
        "first_possible_commit": FIRST_TIMESTAMP_COMMIT,
        "first_possible_authored_at": authored_at,
        "settings_source": {
            "path": FIRST_TIMESTAMP_SOURCE,
            "blob": settings_blob,
        },
        "tx_source": {
            "path": FIRST_TX_SOURCE,
            "blob": tx_blob,
        },
        "candidate_protocol_policy": {
            "path": PROTOCOL_POLICY_SOURCE,
            "blob": PROTOCOL_POLICY_SOURCE_BLOB,
            "files": policy_files,
            "reservation_size": PROTOCOL_RESERVATION_SIZE,
        },
        "candidate_source": {
            "commit": candidate_commit,
            "authored_at": candidate_authored_at,
            "committed_at": candidate_committed_at,
            "not_before_utc": candidate_not_before.isoformat().replace("+00:00", "Z"),
        },
        "candidate_contains_first_possible_commit": True,
    }


def derive_bound_attestation(
    *,
    device: str = D1L_CORE_PORT,
    expected_legacy_value: int,
    confirmed_upper_bound: int,
    source: dict[str, Any],
    observed_at: str,
    attest_exact_device_upper_bound: bool,
) -> dict[str, Any]:
    if attest_exact_device_upper_bound is not True:
        raise ValueError("explicit exact-device upper-bound attestation is required")
    canonical_device = _authorized_target(device)
    if canonical_device is None:
        raise ValueError("device is not an authorized canonical D1L target")
    if (
        type(expected_legacy_value) is not int
        or expected_legacy_value <= 0
        or expected_legacy_value > UINT32_MAX
    ):
        raise ValueError("expected legacy value must be a positive uint32")
    if (
        type(confirmed_upper_bound) is not int
        or confirmed_upper_bound < expected_legacy_value
        or confirmed_upper_bound > MAX_CONFIRMED_UPPER_BOUND
    ):
        raise ValueError(
            "confirmed upper bound must cover the legacy value and leave one "
            "protocol reservation"
        )
    start = parse_utc(source.get("first_possible_authored_at"))
    end = parse_utc(observed_at)
    if start is None or end is None or end < start:
        raise ValueError("predecessor availability window is invalid")
    candidate_source = source.get("candidate_source")
    if not isinstance(candidate_source, dict):
        raise ValueError("candidate source timestamp metadata is missing")
    candidate_authored = parse_utc(candidate_source.get("authored_at"))
    candidate_committed = parse_utc(candidate_source.get("committed_at"))
    candidate_not_before = parse_utc(candidate_source.get("not_before_utc"))
    if (
        candidate_authored is None
        or candidate_committed is None
        or candidate_not_before is None
        or candidate_not_before != max(candidate_authored, candidate_committed)
    ):
        raise ValueError("candidate source timestamp metadata is invalid")
    if end < candidate_not_before:
        raise ValueError("host clock predates the exact candidate commit timestamp")
    window_seconds = int((end - start).total_seconds()) + 1
    worst_case_allocations = window_seconds * MAX_PREDECESSOR_ALLOCATIONS_PER_SECOND
    worst_case_timestamp = expected_legacy_value + worst_case_allocations
    if worst_case_timestamp > UINT32_MAX:
        worst_case_timestamp = UINT32_MAX
    if confirmed_upper_bound < worst_case_timestamp:
        raise ValueError(
            "confirmed upper bound does not cover the pessimistic predecessor "
            "availability window"
        )
    return {
        "schema": 1,
        "kind": "exact_device_protocol_upper_bound_attestation",
        "device": canonical_device,
        "operator_attested": True,
        "human_present": False,
        "authority": "delegated_unattended_core_release_execution",
        "expected_legacy_value": expected_legacy_value,
        "confirmed_upper_bound": confirmed_upper_bound,
        "maximum_allowed_upper_bound": MAX_CONFIRMED_UPPER_BOUND,
        "remaining_uint32_values_after_bound": UINT32_MAX - confirmed_upper_bound,
        "basis": "pessimistic_predecessor_source_availability_window",
        "availability_window": {
            "start_utc": source["first_possible_authored_at"],
            "end_utc": observed_at,
            "candidate_not_before_utc": candidate_source["not_before_utc"],
            "seconds": window_seconds,
        },
        "maximum_predecessor_allocations_per_second": (
            MAX_PREDECESSOR_ALLOCATIONS_PER_SECOND
        ),
        "rate_bound_rationale": (
            "1000/s exceeds the 115200-baud command path and the "
            "radio-busy-serialized production TX paths by a conservative margin"
        ),
        "worst_case_predecessor_allocations": worst_case_allocations,
        "worst_case_predecessor_timestamp": worst_case_timestamp,
        "upper_bound_margin": confirmed_upper_bound - worst_case_timestamp,
        "wall_time_inferred_as_protocol_timestamp": False,
        "wall_time_use": "source_availability_window_only",
        "includes_ram_only_fallback": True,
        "predecessor_source": source,
    }


def _raw_line(raw: bytes, observed_at: str) -> dict[str, Any]:
    return {
        "observed_at": observed_at,
        "size": len(raw),
        "sha256": sha256_bytes(raw),
        "base64": base64.b64encode(raw).decode("ascii"),
    }


def _omitted_raw_line(
    raw: bytes,
    observed_at: str,
    reason: str,
) -> dict[str, Any]:
    return {
        "observed_at": observed_at,
        "size": len(raw),
        "sha256": sha256_bytes(raw),
        "base64_omitted": True,
        "redaction_reason": reason,
    }


def _decode_raw_line(row: object) -> tuple[bytes | None, list[str]]:
    if not isinstance(row, dict):
        return None, ["raw line is not an object"]
    errors: list[str] = []
    try:
        raw = base64.b64decode(row.get("base64"), validate=True)
    except (TypeError, ValueError):
        return None, ["raw line base64 is invalid"]
    if type(row.get("size")) is not int or row.get("size") != len(raw):
        errors.append("raw line size mismatch")
    if row.get("sha256") != sha256_bytes(raw):
        errors.append("raw line digest mismatch")
    if parse_utc(row.get("observed_at")) is None:
        errors.append("raw line observed_at is invalid")
    return raw, errors


def _capture_logical_line(
    raw: bytes,
    observed_at: str,
) -> tuple[dict[str, Any], dict[str, Any] | None, bool]:
    if CONFIRMATION.encode("ascii") in raw:
        return (
            _omitted_raw_line(
                raw,
                observed_at,
                "confirmation_phrase_echo",
            ),
            None,
            True,
        )
    if len(raw) > MAX_RAW_LINE_BYTES:
        return (
            _omitted_raw_line(raw, observed_at, "oversized_logical_line"),
            None,
            False,
        )
    try:
        parsed = parse_jsonl_line(raw.decode("utf-8", errors="strict"))
    except UnicodeDecodeError:
        parsed = None
    if parsed is not None and _value_contains_confirmation(parsed):
        return (
            _omitted_raw_line(
                raw,
                observed_at,
                "confirmation_phrase_decoded_value",
            ),
            None,
            True,
        )
    return _raw_line(raw, observed_at), parsed, False


def _raw_rows_contain_confirmation(rows: object) -> bool:
    if not isinstance(rows, list):
        return False
    raw_values: list[bytes] = []
    for row in rows:
        raw, _errors = _decode_raw_line(row)
        if raw is not None:
            raw_values.append(raw)
            try:
                parsed = parse_jsonl_line(raw.decode("utf-8", errors="strict"))
            except UnicodeDecodeError:
                parsed = None
            if parsed is not None and _value_contains_confirmation(parsed):
                return True
    if not raw_values:
        return False
    marker = CONFIRMATION.encode("ascii")
    direct = b"".join(raw_values)
    without_line_boundaries = b"".join(raw.rstrip(b"\r\n") for raw in raw_values)
    return marker in direct or marker in without_line_boundaries


def _redacted_raw_summary(
    rows: list[dict[str, Any]],
    observed_at: str,
) -> list[dict[str, Any]]:
    size = sum(
        row.get("size")
        for row in rows
        if type(row.get("size")) is int and row.get("size") >= 0
    )
    return [
        {
            "observed_at": observed_at,
            "captured_row_count": len(rows),
            "captured_size": size,
            "base64_omitted": True,
            "redaction_reason": "cross_row_confirmation_reconstruction",
        }
    ]


def transact(
    ser: Any,
    *,
    wire: bytearray,
    command_label: str,
    expected_cmd: str,
    timeout: float,
    redacted: bool,
    now: Callable[[], str] = utc_now,
    clock: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    raw_lines: list[dict[str, Any]] = []
    result: dict[str, Any] | None = None
    confirmation_echo_detected = False
    pending = bytearray()
    original_timeout: object = None
    restore_timeout = False
    started_at = ""
    try:
        if not _finite_positive_timeout(timeout):
            raise ValueError("command timeout must be finite and positive")
        started_at = now()
        try:
            original_timeout = getattr(ser, "timeout")
            restore_timeout = True
        except AttributeError:
            restore_timeout = False
        ser.write(wire)
        if hasattr(ser, "flush"):
            ser.flush()
        deadline = clock() + timeout
        while clock() < deadline:
            remaining = max(0.001, deadline - clock())
            if restore_timeout:
                ser.timeout = min(0.25, remaining)
            chunk = ser.readline(MAX_RAW_LINE_BYTES + 1)
            if not chunk:
                continue
            if not isinstance(chunk, (bytes, bytearray)):
                raise TypeError("serial readline must return bytes")
            pending.extend(chunk)
            while True:
                newline = pending.find(b"\n")
                if newline < 0:
                    break
                raw = bytes(pending[: newline + 1])
                del pending[: newline + 1]
                row, parsed, sensitive = _capture_logical_line(
                    raw,
                    now(),
                )
                raw_lines.append(row)
                confirmation_echo_detected |= sensitive
                if (
                    result is None
                    and parsed is not None
                    and parsed.get("cmd") == expected_cmd
                ):
                    result = parsed
            if result is not None:
                if pending:
                    raw = bytes(pending)
                    sensitive = CONFIRMATION.encode("ascii") in raw
                    confirmation_echo_detected |= sensitive
                    raw_lines.append(
                        _omitted_raw_line(
                            raw,
                            now(),
                            (
                                "confirmation_phrase_echo"
                                if sensitive
                                else "incomplete_logical_line"
                            ),
                        )
                    )
                    pending.clear()
                break
    finally:
        try:
            if restore_timeout:
                ser.timeout = original_timeout
        finally:
            for index in range(len(wire)):
                wire[index] = 0
    if pending:
        raw = bytes(pending)
        sensitive = CONFIRMATION.encode("ascii") in raw
        confirmation_echo_detected |= sensitive
        raw_lines.append(
            _omitted_raw_line(
                raw,
                now(),
                (
                    "confirmation_phrase_echo"
                    if sensitive
                    else "incomplete_logical_line"
                ),
            )
        )
    if _raw_rows_contain_confirmation(raw_lines):
        confirmation_echo_detected = True
        raw_lines = _redacted_raw_summary(raw_lines, now())
        result = None
    if result is not None and _value_contains_confirmation(result):
        confirmation_echo_detected = True
        raw_lines = _redacted_raw_summary(raw_lines, now())
        result = None
    if confirmation_echo_detected:
        result = None
    if result is None:
        result = {
            "schema": 1,
            "ok": False,
            "cmd": expected_cmd,
            "code": (
                "CONFIRMATION_ECHO_REDACTED"
                if confirmation_echo_detected
                else "TIMEOUT_OR_INVALID_RAW"
            ),
        }
    return {
        "command_label": command_label,
        "command_redacted": redacted,
        "expected_cmd": expected_cmd,
        "started_at": started_at,
        "ended_at": now(),
        "raw_lines": raw_lines,
        "result": result,
        "confirmation_echo_detected": confirmation_echo_detected,
    }


def read_only_command(
    ser: Any,
    command: str,
    timeout: float,
    *,
    now: Callable[[], str] = utc_now,
    clock: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    return transact(
        ser,
        wire=bytearray((command + "\n").encode("ascii")),
        command_label=command,
        expected_cmd=command,
        timeout=timeout,
        redacted=False,
        now=now,
        clock=clock,
    )


def migration_command(
    ser: Any,
    expected_legacy_value: int,
    confirmed_upper_bound: int,
    timeout: float,
    *,
    now: Callable[[], str] = utc_now,
    clock: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    wire = bytearray(
        (
            f"time migrate-legacy {expected_legacy_value} "
            f"{confirmed_upper_bound} {CONFIRMATION}\n"
        ).encode("ascii")
    )
    return transact(
        ser,
        wire=wire,
        command_label="time migrate-legacy <redacted-exact-device-attestation>",
        expected_cmd="time migrate-legacy",
        timeout=timeout,
        redacted=True,
        now=now,
        clock=clock,
    )


def _transaction_result(
    transaction: object,
) -> tuple[dict[str, Any] | None, list[str]]:
    if not isinstance(transaction, dict):
        return None, ["transaction is not an object"]
    expected = transaction.get("expected_cmd")
    if not isinstance(expected, str) or not expected:
        return None, ["transaction expected command is missing"]
    raw_rows = transaction.get("raw_lines")
    if not isinstance(raw_rows, list) or not raw_rows:
        return None, [f"{expected}: raw lines are missing"]
    errors: list[str] = []
    matches: list[dict[str, Any]] = []
    for index, row in enumerate(raw_rows):
        if isinstance(row, dict) and row.get("base64_omitted") is True:
            reason = row.get("redaction_reason")
            errors.append(
                f"raw[{index}]: raw content was safely omitted"
                + (f" ({reason})" if isinstance(reason, str) else "")
            )
            continue
        raw, row_errors = _decode_raw_line(row)
        errors.extend(f"raw[{index}]: {error}" for error in row_errors)
        if raw is None or len(raw) > MAX_RAW_LINE_BYTES:
            continue
        try:
            parsed = parse_jsonl_line(raw.decode("utf-8", errors="strict"))
        except UnicodeDecodeError:
            parsed = None
        if parsed is not None and parsed.get("cmd") == expected:
            matches.append(parsed)
    if _raw_rows_contain_confirmation(raw_rows):
        errors.append(
            f"{expected}: confirmation phrase is reconstructable from raw rows"
        )
    if len(matches) != 1:
        errors.append(f"{expected}: expected exactly one matching raw result")
        return None, errors
    if not _json_exact_equal(transaction.get("result"), matches[0]):
        errors.append(f"{expected}: stored result differs from raw result")
    if transaction.get("confirmation_echo_detected") is not False:
        errors.append(f"{expected}: confirmation echo detected")
    if parse_utc(transaction.get("started_at")) is None:
        errors.append(f"{expected}: started_at is invalid")
    if parse_utc(transaction.get("ended_at")) is None:
        errors.append(f"{expected}: ended_at is invalid")
    return matches[0], errors


def _exact_int(value: object, expected: int | None = None) -> bool:
    return type(value) is int and (expected is None or value == expected)


def exact_version(result: object, commit: str, *, tx_ready: bool) -> bool:
    if not isinstance(result, dict):
        return False
    time_status = result.get("time")
    return (
        _exact_int(result.get("schema"), 1)
        and result.get("ok") is True
        and result.get("cmd") == "version"
        and exact_commit(result.get("build_commit")) == commit
        and result.get("idf") == EXPECTED_IDF_VERSION
        and result.get("release_profile") == CORE_RELEASE_PROFILE
        and result.get("sd_history_mode") == CORE_SD_HISTORY_MODE
        and isinstance(time_status, dict)
        and time_status.get("protocol_tx_ready") is tx_ready
    )


def before_status_ok(
    result: object,
    *,
    expected_legacy_value: int,
    confirmed_upper_bound: int,
) -> bool:
    if not isinstance(result, dict) or result.get("ok") is not True:
        return False
    state = result.get("state")
    common = (
        _exact_int(result.get("schema"), 1)
        and result.get("cmd") == "time migration status"
        and result.get("automatic") is False
        and result.get("wall_time_inferred") is False
        and result.get("protocol_tx_ready") is False
    )
    if state == "required":
        legacy = result.get("legacy")
        return (
            common
            and result.get("stage") == "awaiting_operator_confirmation"
            and isinstance(legacy, dict)
            and legacy.get("present") is True
            and _exact_int(
                legacy.get("observed_mesh_ts"),
                expected_legacy_value,
            )
            and _exact_int(
                legacy.get("attested_mesh_ts"),
                expected_legacy_value,
            )
            and result.get("confirmation_required") is True
            and result.get("resume_required") is False
            and result.get("write_blocked") is True
            and result.get("protocol_tx_block")
            == "legacy_protocol_lower_bound_unconfirmed"
        )
    if state == "pending":
        legacy = result.get("legacy")
        high_water = result.get("high_water")
        return (
            common
            and isinstance(legacy, dict)
            and _exact_int(
                legacy.get("attested_mesh_ts"),
                expected_legacy_value,
            )
            and isinstance(high_water, dict)
            and _exact_int(
                high_water.get("confirmed_upper_bound"),
                confirmed_upper_bound,
            )
            and _exact_int(
                high_water.get("target"),
                confirmed_upper_bound,
            )
            and result.get("confirmation_required") is False
            and result.get("resume_required") is True
            and result.get("write_blocked") is True
        )
    return False


def migration_result_ok(
    result: object,
    *,
    expected_legacy_value: int,
    confirmed_upper_bound: int,
) -> bool:
    return (
        isinstance(result, dict)
        and _exact_int(result.get("schema"), 1)
        and result.get("ok") is True
        and result.get("cmd") == "time migrate-legacy"
        and result.get("state") == "complete"
        and _exact_int(
            result.get("legacy_value"),
            expected_legacy_value,
        )
        and _exact_int(
            result.get("confirmed_upper_bound"),
            confirmed_upper_bound,
        )
        and _exact_int(
            result.get("target_high_water"),
            confirmed_upper_bound,
        )
        and result.get("protocol_tx_unblocked") is True
        and result.get("protocol_tx_ready") is True
        and result.get("protocol_tx_block") == "none"
        and result.get("wall_time_inferred") is False
        and result.get("supplied_confirmation_logged") is False
    )


def after_status_ok(
    result: object,
    *,
    expected_legacy_value: int,
    confirmed_upper_bound: int,
) -> bool:
    if not isinstance(result, dict):
        return False
    legacy = result.get("legacy")
    high_water = result.get("high_water")
    receipt = result.get("receipt")
    return (
        _exact_int(result.get("schema"), 1)
        and result.get("ok") is True
        and result.get("cmd") == "time migration status"
        and result.get("state") == "complete"
        and result.get("stage") == "completion_receipt_committed"
        and isinstance(legacy, dict)
        and legacy.get("present") is False
        and _exact_int(
            legacy.get("attested_mesh_ts"),
            expected_legacy_value,
        )
        and isinstance(high_water, dict)
        and high_water.get("present") is True
        and _exact_int(
            high_water.get("observed"),
            confirmed_upper_bound,
        )
        and _exact_int(
            high_water.get("confirmed_upper_bound"),
            confirmed_upper_bound,
        )
        and _exact_int(
            high_water.get("target"),
            confirmed_upper_bound,
        )
        and isinstance(receipt, dict)
        and receipt.get("present") is True
        and _exact_int(receipt.get("phase"), 2)
        and receipt.get("completion_committed") is True
        and result.get("confirmation_required") is False
        and result.get("resume_required") is False
        and result.get("write_blocked") is False
        and result.get("protocol_tx_ready") is True
        and result.get("protocol_tx_block") == "none"
        and result.get("wall_time_inferred") is False
        and result.get("supplied_confirmation_logged") is False
    )


def _inside_existing_file(root: Path, path: Path, label: str) -> Path:
    resolved_root = root.resolve(strict=True)
    candidate = Path(os.path.abspath(str(path)))
    try:
        candidate.relative_to(resolved_root)
    except ValueError as exc:
        raise ValueError(f"{label} must stay inside repository root") from exc
    cursor = candidate
    while True:
        if os.path.lexists(cursor) and is_link_or_reparse(cursor):
            raise ValueError(f"{label} cannot use a link/reparse point")
        if cursor == resolved_root:
            break
        cursor = cursor.parent
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(resolved_root)
    except (OSError, RuntimeError, ValueError) as exc:
        raise ValueError(
            f"{label} must be an existing file inside repository root"
        ) from exc
    if not resolved.is_file():
        raise ValueError(f"{label} must be a regular file")
    return resolved


def _strict_actions_metadata_identity(
    receipt: object,
    *,
    commit: str,
    run_id: str,
    run_attempt: str,
) -> bool:
    if not isinstance(receipt, dict):
        return False
    source_git = receipt.get("git")
    return (
        _exact_int(receipt.get("schema"), 2)
        and receipt.get("kind") == "core_actions_run_metadata"
        and receipt.get("mode") == "github-api-artifact-capture"
        and receipt.get("ok") is True
        and receipt.get("repository") == CORE_REPOSITORY
        and exact_commit(receipt.get("expected_commit")) == commit
        and receipt.get("github_actions_run") == run_id
        and receipt.get("workflow_run_attempt") == run_attempt
        and isinstance(source_git, dict)
        and exact_commit(source_git.get("commit")) == commit
        and source_git.get("status_ok") is True
        and source_git.get("status_error") is None
        and source_git.get("dirty") is False
        and source_git.get("dirty_entries") == []
    )


def load_actions_metadata_binding(
    *,
    root: Path,
    path: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
) -> dict[str, Any]:
    if (
        exact_commit(commit) != commit
        or not _positive_ascii_decimal(run_id)
        or not _positive_ascii_decimal(run_attempt)
    ):
        raise ValueError("exact Actions identity is invalid")
    resolved_root = root.resolve(strict=True)
    resolved = _inside_existing_file(
        resolved_root,
        path,
        "Core Actions run metadata",
    )
    github_run_dir = resolved.parent.parent
    validation = validate_capture_receipt(
        receipt_path=resolved,
        root=resolved_root,
        github_run_dir=github_run_dir,
        commit=commit,
        run_id=run_id,
        run_attempt=run_attempt,
    )
    if not isinstance(validation, dict) or validation.get("ok") is not True:
        raise ValueError("Core Actions run metadata validation failed")
    try:
        receipt = json.loads(resolved.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("Core Actions run metadata is invalid JSON") from exc
    if not _strict_actions_metadata_identity(
        receipt,
        commit=commit,
        run_id=run_id,
        run_attempt=run_attempt,
    ):
        raise ValueError("Core Actions run metadata identity mismatch")
    return {
        "schema": 1,
        "kind": "core_actions_run_metadata_binding",
        "path": resolved.relative_to(resolved_root).as_posix(),
        "size": resolved.stat().st_size,
        "sha256": sha256_file(resolved),
        "github_run_dir": github_run_dir.relative_to(resolved_root).as_posix(),
        "receipt_schema": 2,
        "receipt_kind": "core_actions_run_metadata",
        "receipt_mode": "github-api-artifact-capture",
        "repository": CORE_REPOSITORY,
        "expected_commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "exact_clean_source": True,
        "raw_github_and_artifacts_validated": True,
    }


def _actions_binding_errors(
    binding: object,
    *,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
) -> list[str]:
    if not isinstance(binding, dict):
        return ["Core Actions metadata binding is missing"]
    required = {
        "schema": 1,
        "kind": "core_actions_run_metadata_binding",
        "receipt_schema": 2,
        "receipt_kind": "core_actions_run_metadata",
        "receipt_mode": "github-api-artifact-capture",
        "repository": CORE_REPOSITORY,
        "expected_commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "exact_clean_source": True,
        "raw_github_and_artifacts_validated": True,
    }
    errors = [
        f"Actions metadata binding {key} mismatch"
        for key, expected in required.items()
        if not _exact_json_scalar(binding.get(key), expected)
    ]
    raw_path = binding.get("path")
    if not isinstance(raw_path, str) or not raw_path:
        return [*errors, "Actions metadata binding path is invalid"]
    relative = Path(raw_path)
    if relative.is_absolute():
        return [*errors, "Actions metadata binding path is not relative"]
    try:
        recomputed = load_actions_metadata_binding(
            root=root,
            path=root / relative,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
        )
    except (OSError, RuntimeError, ValueError) as exc:
        return [
            *errors,
            "Core Actions metadata could not be independently validated: "
            + type(exc).__name__,
        ]
    if not _json_exact_equal(binding, recomputed):
        errors.append("Core Actions metadata binding does not recompute")
    return errors


def _strict_target_identity(
    snapshot: object,
    expected_target: str,
) -> str | None:
    try:
        validate_snapshot(snapshot, expected_target)
    except (TypeError, ValueError):
        return None
    if not isinstance(snapshot, dict):
        return None
    identity = snapshot.get("stable_identity_sha256")
    return identity if isinstance(identity, str) else None


def _validate_receipt_core(receipt: object) -> tuple[bool, list[str]]:
    errors: list[str] = []
    if not isinstance(receipt, dict):
        return False, ["receipt is not an object"]
    if _value_contains_confirmation(receipt):
        errors.append("confirmation phrase leaked into receipt")
    if receipt.get("mutation_outcome_uncertain") is True:
        errors.append("receipt mutation_outcome_uncertain is true")
    for field in REJECTED_RECEIPT_PRESENT_FIELDS:
        if field in receipt:
            errors.append(f"receipt contains rejected field {field}")
    for flag in REJECTED_RECEIPT_TRUE_FLAGS:
        if receipt.get(flag) is True:
            errors.append(f"receipt rejected flag {flag} is true")
    commit = exact_commit(receipt.get("commit"))
    run_id = str(receipt.get("github_actions_run") or "")
    attempt = str(receipt.get("workflow_run_attempt") or "")
    if commit is None or not positive_decimal(run_id) or not positive_decimal(attempt):
        errors.append("exact candidate/Actions identity is invalid")
    requested_target = _authorized_target(receipt.get("port"))
    if requested_target is None:
        errors.append("receipt port is not an authorized canonical D1L target")
    expected_public_key = exact_public_key(
        receipt.get("expected_d1l_public_key")
    )
    if (
        expected_public_key is None
        or receipt.get("expected_d1l_public_key") != expected_public_key
    ):
        errors.append("expected D1L public key is not exact normalized 64-hex")
    required = {
        "schema": 2,
        "kind": "time_protocol_migration",
        "mode": "hardware",
        "scope": "exact-device-legacy-protocol-migration",
        "baud": D1L_BAUD,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": CORE_SD_HISTORY_MODE,
        "ok": True,
        "closure_eligible": True,
        "physical_observed": True,
        "mutation_started": True,
        "mutation_outcome_uncertain": False,
        "release_closure_sufficient": False,
        "hardware_required": True,
        "automatic_migration": False,
        "wall_time_inferred_as_protocol_timestamp": False,
        "supplied_confirmation_logged": False,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "sd_access": False,
        "rp2040_access": False,
        "predecessor_evidence_used": False,
        "d1l_identity_ok": True,
    }
    for key, expected in required.items():
        if receipt.get(key) != expected:
            errors.append(f"receipt {key} mismatch")
    if (
        requested_target is not None
        and receipt.get("target_slug") != safe_slug(requested_target)
    ):
        errors.append("receipt target_slug mismatch")
    attestation = receipt.get("bound_attestation")
    if not isinstance(attestation, dict):
        errors.append("bound attestation is missing")
        return False, errors
    legacy = attestation.get("expected_legacy_value")
    upper = attestation.get("confirmed_upper_bound")
    if (
        type(legacy) is not int
        or type(upper) is not int
        or upper < legacy
        or upper > MAX_CONFIRMED_UPPER_BOUND
        or attestation.get("schema") != 1
        or attestation.get("kind") != "exact_device_protocol_upper_bound_attestation"
        or attestation.get("device") != requested_target
        or attestation.get("expected_legacy_value") != legacy
        or attestation.get("confirmed_upper_bound") != upper
        or attestation.get("operator_attested") is not True
        or attestation.get("human_present") is not False
        or attestation.get("authority") != "delegated_unattended_core_release_execution"
        or attestation.get("basis")
        != "pessimistic_predecessor_source_availability_window"
        or attestation.get("wall_time_inferred_as_protocol_timestamp") is not False
        or attestation.get("wall_time_use") != "source_availability_window_only"
        or attestation.get("includes_ram_only_fallback") is not True
        or attestation.get("maximum_allowed_upper_bound") != MAX_CONFIRMED_UPPER_BOUND
        or attestation.get("maximum_predecessor_allocations_per_second")
        != MAX_PREDECESSOR_ALLOCATIONS_PER_SECOND
        or attestation.get("remaining_uint32_values_after_bound")
        != UINT32_MAX - int(upper or 0)
    ):
        errors.append("bound attestation is invalid")
    predecessor_source = attestation.get("predecessor_source")
    if not (
        isinstance(predecessor_source, dict)
        and predecessor_source.get("first_possible_commit") == FIRST_TIMESTAMP_COMMIT
        and predecessor_source.get("candidate_contains_first_possible_commit") is True
        and predecessor_source.get("first_possible_authored_at")
        == FIRST_TIMESTAMP_AUTHORED_AT
        and isinstance(predecessor_source.get("settings_source"), dict)
        and predecessor_source["settings_source"].get("path") == FIRST_TIMESTAMP_SOURCE
        and predecessor_source["settings_source"].get("blob")
        == FIRST_TIMESTAMP_SOURCE_BLOB
        and isinstance(predecessor_source.get("tx_source"), dict)
        and predecessor_source["tx_source"].get("path") == FIRST_TX_SOURCE
        and predecessor_source["tx_source"].get("blob") == FIRST_TX_SOURCE_BLOB
        and isinstance(predecessor_source.get("candidate_protocol_policy"), dict)
        and predecessor_source["candidate_protocol_policy"].get("path")
        == PROTOCOL_POLICY_SOURCE
        and predecessor_source["candidate_protocol_policy"].get("blob")
        == PROTOCOL_POLICY_SOURCE_BLOB
        and predecessor_source["candidate_protocol_policy"].get("files")
        == [{"path": path, "blob": blob} for path, blob in PROTOCOL_POLICY_SOURCE_BLOBS]
        and predecessor_source["candidate_protocol_policy"].get("reservation_size")
        == PROTOCOL_RESERVATION_SIZE
    ):
        errors.append("predecessor source evidence is invalid")
    window = attestation.get("availability_window")
    if not isinstance(window, dict):
        errors.append("attestation availability window is invalid")
    else:
        start = parse_utc(window.get("start_utc"))
        end = parse_utc(window.get("end_utc"))
        source_start = (
            predecessor_source.get("first_possible_authored_at")
            if isinstance(predecessor_source, dict)
            else None
        )
        if (
            start is None
            or end is None
            or end < start
            or window.get("start_utc") != source_start
        ):
            errors.append("attestation time bounds are invalid")
        else:
            seconds = int((end - start).total_seconds()) + 1
            worst = seconds * MAX_PREDECESSOR_ALLOCATIONS_PER_SECOND
            worst_timestamp = min(UINT32_MAX, int(legacy or 0) + worst)
            if (
                window.get("seconds") != seconds
                or attestation.get("worst_case_predecessor_allocations") != worst
                or attestation.get("worst_case_predecessor_timestamp")
                != worst_timestamp
                or upper < worst_timestamp
                or attestation.get("upper_bound_margin") != upper - worst_timestamp
            ):
                errors.append("bound attestation recomputation failed")
    transactions = receipt.get("transactions")
    if not isinstance(transactions, list) or len(transactions) != 8:
        errors.append("exact eight-command transaction sequence is missing")
        return False, errors
    parsed: list[dict[str, Any] | None] = []
    for index, transaction in enumerate(transactions):
        result, transaction_errors = _transaction_result(transaction)
        parsed.append(result)
        errors.extend(f"transaction[{index}]: {error}" for error in transaction_errors)
    if [row.get("expected_cmd") for row in transactions] != [
        "identity status",
        "version",
        "health",
        "time migration status",
        "time migrate-legacy",
        "time migration status",
        "version",
        "health",
    ]:
        errors.append("transaction command order is invalid")
    if transactions[4].get("command_redacted") is not True:
        errors.append("migration command is not redacted")
    if any(
        row.get("command_redacted") is True
        for index, row in enumerate(transactions)
        if index != 4
    ):
        errors.append("read-only command unexpectedly marked redacted")
    if len(parsed) == 8:
        if (
            not identity_status_ok(parsed[0], expected_public_key)
            or not _json_exact_equal(
                receipt.get("d1l_identity_status"),
                parsed[0],
            )
            or receipt.get("d1l_identity_ok") is not True
        ):
            errors.append("exact D1L public-key identity binding failed")
    if commit is not None and len(parsed) == 8:
        if not exact_version(parsed[1], commit, tx_ready=False):
            errors.append("pre-migration exact version/TX block failed")
        if not (
            isinstance(parsed[2], dict)
            and _exact_int(parsed[2].get("schema"), 1)
            and parsed[2].get("ok") is True
            and parsed[2].get("cmd") == "health"
        ):
            errors.append("pre-migration health failed")
        if not before_status_ok(
            parsed[3],
            expected_legacy_value=int(legacy or 0),
            confirmed_upper_bound=int(upper or 0),
        ):
            errors.append("pre-migration status failed")
        if not migration_result_ok(
            parsed[4],
            expected_legacy_value=int(legacy or 0),
            confirmed_upper_bound=int(upper or 0),
        ):
            errors.append("migration mutation result failed")
        if not after_status_ok(
            parsed[5],
            expected_legacy_value=int(legacy or 0),
            confirmed_upper_bound=int(upper or 0),
        ):
            errors.append("post-migration status failed")
        if not exact_version(parsed[6], commit, tx_ready=True):
            errors.append("post-migration exact version/TX readiness failed")
        if not (
            isinstance(parsed[7], dict)
            and _exact_int(parsed[7].get("schema"), 1)
            and parsed[7].get("ok") is True
            and parsed[7].get("cmd") == "health"
        ):
            errors.append("post-migration health failed")
    before_target = receipt.get("d1l_target_before")
    after_target = receipt.get("d1l_target_after")
    before_identity = (
        _strict_target_identity(before_target, requested_target)
        if requested_target is not None
        else None
    )
    after_identity = (
        _strict_target_identity(after_target, requested_target)
        if requested_target is not None
        else None
    )
    if (
        before_identity is None
        or after_identity is None
        or before_identity != after_identity
        or receipt.get("target_identity_sha256") != before_identity
        or receipt.get("target_identity_continuity_ok") is not True
    ):
        errors.append("D1L target identity continuity failed")
    source_git = receipt.get("git")
    if not (
        isinstance(source_git, dict)
        and exact_commit(source_git.get("commit")) == commit
        and source_git.get("dirty") is False
        and source_git.get("dirty_entries") == []
    ):
        errors.append("exact clean producer source is invalid")
    return not errors, errors


def _strict_receipt_shape_errors(
    receipt: object,
    *,
    root: Path,
) -> list[str]:
    if not isinstance(receipt, dict):
        return ["receipt is not an object"]
    errors: list[str] = []
    if _value_contains_confirmation(receipt):
        errors.append("confirmation phrase leaked into receipt")
    if receipt.get("mutation_outcome_uncertain") is True:
        errors.append("receipt mutation_outcome_uncertain is true")
    for field in REJECTED_RECEIPT_PRESENT_FIELDS:
        if field in receipt:
            errors.append(f"receipt contains rejected field {field}")
    for flag in REJECTED_RECEIPT_TRUE_FLAGS:
        if receipt.get(flag) is True:
            errors.append(f"receipt rejected flag {flag} is true")
    commit = exact_commit(receipt.get("commit"))
    run_id = receipt.get("github_actions_run")
    run_attempt = receipt.get("workflow_run_attempt")
    if (
        commit is None
        or not _positive_ascii_decimal(run_id)
        or not _positive_ascii_decimal(run_attempt)
    ):
        errors.append("exact candidate/Actions identity is invalid")
    requested_target = _authorized_target(receipt.get("port"))
    if requested_target is None:
        errors.append("receipt port is not an authorized canonical D1L target")
    expected_public_key = exact_public_key(
        receipt.get("expected_d1l_public_key")
    )
    if (
        expected_public_key is None
        or not _exact_json_scalar(
            receipt.get("expected_d1l_public_key"),
            expected_public_key,
        )
    ):
        errors.append("expected D1L public key is not exact normalized 64-hex")
    required = {
        "schema": 2,
        "kind": "time_protocol_migration",
        "mode": "hardware",
        "scope": "exact-device-legacy-protocol-migration",
        "baud": D1L_BAUD,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": CORE_SD_HISTORY_MODE,
        "ok": True,
        "closure_eligible": True,
        "physical_observed": True,
        "mutation_started": True,
        "mutation_outcome_uncertain": False,
        "release_closure_sufficient": False,
        "hardware_required": True,
        "automatic_migration": False,
        "wall_time_inferred_as_protocol_timestamp": False,
        "supplied_confirmation_logged": False,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "sd_access": False,
        "rp2040_access": False,
        "predecessor_evidence_used": False,
        "d1l_identity_ok": True,
    }
    for key, expected in required.items():
        if not _exact_json_scalar(receipt.get(key), expected):
            errors.append(f"receipt {key} type/value mismatch")
    if (
        requested_target is not None
        and not _exact_json_scalar(
            receipt.get("target_slug"),
            safe_slug(requested_target),
        )
    ):
        errors.append("receipt target_slug type/value mismatch")

    source_git = receipt.get("git")
    if not (
        isinstance(source_git, dict)
        and commit is not None
        and exact_commit(source_git.get("commit")) == commit
        and source_git.get("status_ok") is True
        and source_git.get("status_error") is None
        and source_git.get("dirty") is False
        and source_git.get("dirty_entries") == []
    ):
        errors.append("exact clean producer source is invalid")

    attestation = receipt.get("bound_attestation")
    if not isinstance(attestation, dict):
        errors.append("bound attestation is missing")
        return errors
    legacy = attestation.get("expected_legacy_value")
    upper = attestation.get("confirmed_upper_bound")
    if not (
        _exact_int(legacy)
        and _exact_int(upper)
        and 0 < legacy <= UINT32_MAX
        and legacy <= upper <= MAX_CONFIRMED_UPPER_BOUND
    ):
        errors.append("bound attestation integer bounds are invalid")
        return errors
    attestation_required = {
        "schema": 1,
        "kind": "exact_device_protocol_upper_bound_attestation",
        "device": requested_target,
        "operator_attested": True,
        "human_present": False,
        "authority": "delegated_unattended_core_release_execution",
        "maximum_allowed_upper_bound": MAX_CONFIRMED_UPPER_BOUND,
        "remaining_uint32_values_after_bound": UINT32_MAX - upper,
        "basis": "pessimistic_predecessor_source_availability_window",
        "maximum_predecessor_allocations_per_second": (
            MAX_PREDECESSOR_ALLOCATIONS_PER_SECOND
        ),
        "wall_time_inferred_as_protocol_timestamp": False,
        "wall_time_use": "source_availability_window_only",
        "includes_ram_only_fallback": True,
    }
    for key, expected in attestation_required.items():
        if not _exact_json_scalar(attestation.get(key), expected):
            errors.append(f"bound attestation {key} type/value mismatch")

    predecessor = attestation.get("predecessor_source")
    candidate_source = (
        predecessor.get("candidate_source") if isinstance(predecessor, dict) else None
    )
    if not isinstance(candidate_source, dict):
        errors.append("candidate source timestamp metadata is missing")
        return errors
    authored = parse_utc(candidate_source.get("authored_at"))
    committed = parse_utc(candidate_source.get("committed_at"))
    not_before = parse_utc(candidate_source.get("not_before_utc"))
    git_authored = (
        _git_text(root, "show", "-s", "--format=%aI", commit)
        if commit is not None
        else None
    )
    git_committed = (
        _git_text(root, "show", "-s", "--format=%cI", commit)
        if commit is not None
        else None
    )
    if not (
        commit is not None
        and exact_commit(candidate_source.get("commit")) == commit
        and candidate_source.get("authored_at") == git_authored
        and candidate_source.get("committed_at") == git_committed
        and authored is not None
        and committed is not None
        and not_before is not None
        and not_before == max(authored, committed)
    ):
        errors.append("candidate source timestamp metadata is invalid")
        return errors

    window = attestation.get("availability_window")
    if not isinstance(window, dict):
        errors.append("attestation availability window is invalid")
        return errors
    start = parse_utc(window.get("start_utc"))
    end = parse_utc(window.get("end_utc"))
    if not (
        start is not None
        and end is not None
        and end >= start
        and end >= not_before
        and window.get("candidate_not_before_utc")
        == candidate_source.get("not_before_utc")
    ):
        errors.append("attestation candidate/time bounds are invalid")
        return errors
    seconds = int((end - start).total_seconds()) + 1
    worst = seconds * MAX_PREDECESSOR_ALLOCATIONS_PER_SECOND
    worst_timestamp = min(UINT32_MAX, legacy + worst)
    computed = {
        "seconds": seconds,
        "worst_case_predecessor_allocations": worst,
        "worst_case_predecessor_timestamp": worst_timestamp,
        "upper_bound_margin": upper - worst_timestamp,
    }
    if not _exact_int(window.get("seconds"), seconds):
        errors.append("attestation window seconds type/value mismatch")
    for key in (
        "worst_case_predecessor_allocations",
        "worst_case_predecessor_timestamp",
        "upper_bound_margin",
    ):
        if not _exact_int(attestation.get(key), computed[key]):
            errors.append(f"bound attestation {key} type/value mismatch")
    if upper < worst_timestamp:
        errors.append("bound attestation does not cover the recomputed window")

    transactions = receipt.get("transactions")
    if not (
        isinstance(transactions, list)
        and len(transactions) == 8
        and all(isinstance(row, dict) for row in transactions)
    ):
        errors.append("exact typed eight-command sequence is missing")
        return errors
    for index, transaction in enumerate(transactions):
        expected_redacted = index == 4
        if transaction.get("command_redacted") is not expected_redacted:
            errors.append(f"transaction[{index}] redaction flag type/value mismatch")
        if transaction.get("confirmation_echo_detected") is not False:
            errors.append(f"transaction[{index}] confirmation echo detected")
        if not isinstance(transaction.get("expected_cmd"), str):
            errors.append(f"transaction[{index}] expected command is invalid")
        if parse_utc(transaction.get("started_at")) is None:
            errors.append(f"transaction[{index}] started_at is invalid")
        if parse_utc(transaction.get("ended_at")) is None:
            errors.append(f"transaction[{index}] ended_at is invalid")
        raw_rows = transaction.get("raw_lines")
        if not (
            isinstance(raw_rows, list)
            and raw_rows
            and all(isinstance(row, dict) for row in raw_rows)
        ):
            errors.append(f"transaction[{index}] raw rows are invalid")
    if _raw_rows_contain_confirmation(
        [
            raw
            for transaction in transactions
            for raw in transaction.get("raw_lines", [])
        ]
    ):
        errors.append("confirmation phrase is reconstructable across receipt raw rows")
    identity_result, identity_errors = _transaction_result(transactions[0])
    errors.extend(
        f"transaction[0]: {error}" for error in identity_errors
    )
    if (
        not identity_status_ok(identity_result, expected_public_key)
        or not _json_exact_equal(
            receipt.get("d1l_identity_status"),
            identity_result,
        )
        or receipt.get("d1l_identity_ok") is not True
    ):
        errors.append("exact D1L public-key identity binding failed")

    before_identity = (
        _strict_target_identity(
            receipt.get("d1l_target_before"),
            requested_target,
        )
        if requested_target is not None
        else None
    )
    after_identity = (
        _strict_target_identity(
            receipt.get("d1l_target_after"),
            requested_target,
        )
        if requested_target is not None
        else None
    )
    if (
        before_identity is None
        or after_identity is None
        or before_identity != after_identity
        or not _exact_json_scalar(
            receipt.get("target_identity_sha256"),
            before_identity,
        )
        or receipt.get("target_identity_continuity_ok") is not True
    ):
        errors.append("exact D1L target identity continuity failed")

    if commit is not None and isinstance(run_id, str) and isinstance(run_attempt, str):
        errors.extend(
            _actions_binding_errors(
                receipt.get("actions_provenance"),
                root=root,
                commit=commit,
                run_id=run_id,
                run_attempt=run_attempt,
            )
        )
    else:
        errors.append("Actions metadata binding cannot be validated")
    return errors


def validate_receipt(
    receipt: object,
    *,
    root: Path,
) -> tuple[bool, list[str]]:
    try:
        strict_errors = _strict_receipt_shape_errors(
            receipt,
            root=root.resolve(strict=True),
        )
        if strict_errors:
            return False, strict_errors
        valid, errors = _validate_receipt_core(receipt)
        return valid, errors
    except Exception as exc:
        return (
            False,
            ["receipt validation failed safely: " + type(exc).__name__],
        )


def preflight_new_output_path(root: Path, out: Path) -> Path:
    resolved_root = root.resolve(strict=True)
    candidate = Path(os.path.abspath(str(out)))
    try:
        candidate.relative_to(resolved_root)
    except ValueError as exc:
        raise ValueError(
            f"evidence output must stay inside repository root: {candidate}"
        ) from exc
    if os.path.lexists(candidate):
        if is_link_or_reparse(candidate):
            raise ValueError(
                f"evidence output cannot be a link/reparse point: {candidate}"
            )
        raise ValueError(f"refusing to overwrite evidence: {candidate}")
    cursor = candidate.parent
    while True:
        if cursor.exists() and is_link_or_reparse(cursor):
            raise ValueError(
                f"evidence output parent cannot be a link/reparse point: {cursor}"
            )
        if cursor == resolved_root:
            break
        cursor = cursor.parent
    resolved = candidate.resolve()
    try:
        resolved.relative_to(resolved_root)
    except ValueError as exc:
        raise ValueError(
            f"evidence output must stay inside repository root: {resolved}"
        ) from exc
    if resolved.exists():
        raise ValueError(f"refusing to overwrite evidence: {resolved}")
    return resolved


def reserve_new_output_path(root: Path, out: Path) -> tuple[Path, Path]:
    resolved = preflight_new_output_path(root, out)
    resolved.parent.mkdir(parents=True, exist_ok=True)
    resolved = preflight_new_output_path(root, resolved)
    reservation = resolved.with_name(f".{resolved.name}.reservation")
    payload = {
        "schema": 1,
        "kind": "evidence_output_reservation",
        "output_name": resolved.name,
        "closure_eligible": False,
    }
    try:
        with reservation.open("x", encoding="ascii", newline="\n") as stream:
            stream.write(
                json.dumps(
                    payload,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=True,
                )
                + "\n"
            )
            stream.flush()
            os.fsync(stream.fileno())
    except FileExistsError as exc:
        raise ValueError(f"evidence output is already reserved: {resolved}") from exc
    return resolved, reservation


def write_json_exclusive(path: Path, value: dict[str, Any]) -> None:
    encoded = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    try:
        with path.open("x", encoding="ascii", newline="\n") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
    except FileExistsError as exc:
        raise ValueError(f"refusing to overwrite evidence: {path}") from exc


def _report_confirmation_exposure(report: object) -> bool:
    if _value_contains_confirmation(report):
        return True
    if not isinstance(report, dict):
        return True
    transactions = report.get("transactions")
    if not isinstance(transactions, list):
        return False
    raw_rows: list[object] = []
    for transaction in transactions:
        if not isinstance(transaction, dict):
            continue
        if transaction.get("confirmation_echo_detected") is True:
            return True
        rows = transaction.get("raw_lines")
        if isinstance(rows, list):
            raw_rows.extend(rows)
    return _raw_rows_contain_confirmation(raw_rows)


def _minimal_redacted_failure(
    report: dict[str, Any],
    *,
    mutation_started: bool,
) -> dict[str, Any]:
    return {
        "schema": 2,
        "kind": "time_protocol_migration_redacted_failure",
        "mode": "hardware",
        "scope": "exact-device-legacy-protocol-migration",
        "ok": False,
        "closure_eligible": False,
        "release_closure_sufficient": False,
        "mutation_started": mutation_started,
        "mutation_outcome_uncertain": mutation_started,
        "redaction_applied": True,
        "redaction_reason": "sensitive_serial_material_omitted",
        "supplied_confirmation_logged": False,
        "port": report.get("port"),
        "commit": report.get("commit"),
        "github_actions_run": report.get("github_actions_run"),
        "workflow_run_attempt": report.get("workflow_run_attempt"),
        "ended_at": report.get("ended_at"),
    }


def write_report_safely(
    path: Path,
    report: dict[str, Any],
    *,
    mutation_started: bool,
) -> dict[str, Any]:
    try:
        unsafe = _report_confirmation_exposure(report)
    except Exception:
        unsafe = True
    selected = (
        _minimal_redacted_failure(
            report,
            mutation_started=mutation_started,
        )
        if unsafe
        else report
    )
    if _report_confirmation_exposure(selected):
        raise RuntimeError("safe report guard rejected output")
    write_json_exclusive(path, selected)
    return selected


def _execute_migration_reserved(
    *,
    root: Path,
    out: Path,
    serial_module: Any,
    port_lister: Callable[[], Iterable[object]],
    requested_target: str,
    platform_name: str | None,
    target_exists: Callable[[str], bool],
    target_is_symlink: Callable[[str], bool],
    target_realpath: Callable[[str], str],
    target_access: Callable[[str, int], bool],
    target_hostname: Callable[[], str],
    commit: str,
    run_id: str,
    run_attempt: str,
    expected_d1l_public_key: str,
    expected_legacy_value: int,
    confirmed_upper_bound: int,
    attest_exact_device_upper_bound: bool,
    timeout: float,
    source_git: dict[str, Any],
    actions_provenance: dict[str, Any],
    mutation_state: dict[str, bool],
    now: Callable[[], str] = utc_now,
    clock: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    if attest_exact_device_upper_bound is not True:
        raise ValueError("explicit exact-device upper-bound attestation is required")
    normalized_public_key = exact_public_key(expected_d1l_public_key)
    if normalized_public_key is None:
        raise ValueError(
            "expected D1L public key must be exactly 64 hexadecimal characters"
        )
    observed_at = now()
    predecessor_source = predecessor_source_metadata(root, commit)
    canonical_requested_target = enforce_core_port(requested_target)
    before_target = resolve_target(
        canonical_requested_target,
        port_lister=port_lister,
        platform_name=platform_name,
        exists=target_exists,
        is_symlink=target_is_symlink,
        realpath=target_realpath,
        access=target_access,
        hostname=target_hostname,
    )
    port = before_target["requested_path"]
    before_identity = before_target["stable_identity_sha256"]
    attestation = derive_bound_attestation(
        device=port,
        expected_legacy_value=expected_legacy_value,
        confirmed_upper_bound=confirmed_upper_bound,
        source=predecessor_source,
        observed_at=observed_at,
        attest_exact_device_upper_bound=attest_exact_device_upper_bound,
    )
    transactions: list[dict[str, Any]] = []
    ser = open_d1l_serial(
        serial_module,
        port=port,
        baudrate=D1L_BAUD,
        timeout=timeout,
    )
    try:
        if hasattr(ser, "reset_input_buffer"):
            ser.reset_input_buffer()
        transactions.append(
            read_only_command(
                ser,
                "identity status",
                timeout,
                now=now,
                clock=clock,
            )
        )
        identity_result, identity_errors = _transaction_result(transactions[0])
        if (
            identity_errors
            or not identity_status_ok(identity_result, normalized_public_key)
        ):
            raise ValueError(
                "D1L identity preflight failed; no mutation sent"
            )
        transactions.append(
            read_only_command(ser, "version", timeout, now=now, clock=clock)
        )
        transactions.append(
            read_only_command(ser, "health", timeout, now=now, clock=clock)
        )
        transactions.append(
            read_only_command(
                ser,
                "time migration status",
                timeout,
                now=now,
                clock=clock,
            )
        )
        pre_version, pre_version_errors = _transaction_result(transactions[1])
        pre_health, pre_health_errors = _transaction_result(transactions[2])
        pre_status, pre_status_errors = _transaction_result(transactions[3])
        preflight_errors = [
            *pre_version_errors,
            *pre_health_errors,
            *pre_status_errors,
        ]
        if not exact_version(pre_version, commit, tx_ready=False):
            preflight_errors.append("exact pre-migration version/TX block failed")
        if not (
            isinstance(pre_health, dict)
            and pre_health.get("ok") is True
            and pre_health.get("cmd") == "health"
        ):
            preflight_errors.append("pre-migration health failed")
        if not before_status_ok(
            pre_status,
            expected_legacy_value=expected_legacy_value,
            confirmed_upper_bound=confirmed_upper_bound,
        ):
            preflight_errors.append("live legacy migration state is inadmissible")
        if preflight_errors:
            raise ValueError(
                "migration preflight failed; no mutation sent: "
                + "; ".join(preflight_errors)
            )
        mutation_state["started"] = True
        mutation_error: dict[str, str] | None = None
        try:
            transactions.append(
                migration_command(
                    ser,
                    expected_legacy_value,
                    confirmed_upper_bound,
                    timeout,
                    now=now,
                    clock=clock,
                )
            )
            transactions.append(
                read_only_command(
                    ser,
                    "time migration status",
                    timeout,
                    now=now,
                    clock=clock,
                )
            )
            transactions.append(
                read_only_command(
                    ser,
                    "version",
                    timeout,
                    now=now,
                    clock=clock,
                )
            )
            transactions.append(
                read_only_command(
                    ser,
                    "health",
                    timeout,
                    now=now,
                    clock=clock,
                )
            )
        except Exception as exc:
            mutation_error = {
                "phase": "mutation_or_postcheck",
                "exception_type": type(exc).__name__,
                "detail_omitted": "serial_exception_text_may_contain_wire_data",
            }
    finally:
        try:
            ser.close()
        except Exception:
            pass
    try:
        after_target = resolve_target(
            port,
            port_lister=port_lister,
            platform_name=platform_name,
            exists=target_exists,
            is_symlink=target_is_symlink,
            realpath=target_realpath,
            access=target_access,
            hostname=target_hostname,
        )
    except Exception as exc:
        after_target = None
        if mutation_error is None:
            mutation_error = {
                "phase": "post_mutation_target_snapshot",
                "exception_type": type(exc).__name__,
                "detail_omitted": "target_exception_text_not_persisted",
            }
    target_continuity_ok = bool(
        isinstance(after_target, dict)
        and after_target.get("stable_identity_sha256") == before_identity
    )
    if not target_continuity_ok and mutation_error is None:
        mutation_error = {
            "phase": "post_mutation_target_continuity",
            "exception_type": "TargetIdentityChanged",
            "detail_omitted": "target_identity_details_retained_in_snapshots",
        }
    report = {
        "schema": 2,
        "kind": "time_protocol_migration",
        "mode": "hardware",
        "scope": "exact-device-legacy-protocol-migration",
        "ok": mutation_error is None,
        "closure_eligible": mutation_error is None,
        "physical_observed": True,
        "mutation_started": mutation_state["started"],
        "mutation_outcome_uncertain": mutation_error is not None,
        "release_closure_sufficient": False,
        "hardware_required": True,
        "port": port,
        "target_slug": safe_slug(port),
        "baud": D1L_BAUD,
        "commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "expected_d1l_public_key": normalized_public_key,
        "d1l_identity_status": identity_result,
        "d1l_identity_ok": identity_status_ok(
            identity_result,
            normalized_public_key,
        ),
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": CORE_SD_HISTORY_MODE,
        "automatic_migration": False,
        "wall_time_inferred_as_protocol_timestamp": False,
        "supplied_confirmation_logged": False,
        "bound_attestation": attestation,
        "d1l_target_before": before_target,
        "d1l_target_after": after_target,
        "target_identity_sha256": before_identity,
        "target_identity_continuity_ok": target_continuity_ok,
        "transactions": transactions,
        "started_at": transactions[0].get("started_at"),
        "ended_at": now(),
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "sd_access": False,
        "rp2040_access": False,
        "predecessor_evidence_used": False,
        "git": source_git,
        "actions_provenance": actions_provenance,
    }
    if mutation_error is not None:
        report["mutation_outcome_uncertain"] = True
        report["execution_error"] = mutation_error
    valid, errors = validate_receipt(report, root=root)
    if not valid:
        report["ok"] = False
        report["closure_eligible"] = False
        if mutation_state["started"]:
            report["mutation_outcome_uncertain"] = True
        report["validation_errors"] = errors
    return write_report_safely(
        out,
        report,
        mutation_started=mutation_state["started"],
    )


def execute_migration(
    *,
    root: Path,
    out: Path,
    serial_module: Any,
    port_lister: Callable[[], Iterable[object]],
    requested_target: str | None = None,
    platform_name: str | None = None,
    target_exists: Callable[[str], bool] = os.path.exists,
    target_is_symlink: Callable[[str], bool] = os.path.islink,
    target_realpath: Callable[[str], str] = os.path.realpath,
    target_access: Callable[[str, int], bool] = os.access,
    target_hostname: Callable[[], str] = socket.gethostname,
    commit: str,
    run_id: str,
    run_attempt: str,
    expected_d1l_public_key: str,
    expected_legacy_value: int,
    confirmed_upper_bound: int,
    attest_exact_device_upper_bound: bool,
    timeout: float,
    source_git: dict[str, Any],
    actions_metadata_path: Path,
    now: Callable[[], str] = utc_now,
    clock: Callable[[], float] = time.monotonic,
) -> dict[str, Any]:
    if attest_exact_device_upper_bound is not True:
        raise ValueError("explicit exact-device upper-bound attestation is required")
    normalized_public_key = exact_public_key(expected_d1l_public_key)
    if normalized_public_key is None:
        raise ValueError(
            "expected D1L public key must be exactly 64 hexadecimal characters"
        )
    if not _finite_positive_timeout(timeout):
        raise ValueError("command timeout must be finite and positive")
    if not (
        isinstance(source_git, dict)
        and exact_commit(source_git.get("commit")) == commit
        and source_git.get("status_ok") is True
        and source_git.get("status_error") is None
        and source_git.get("dirty") is False
        and source_git.get("dirty_entries") == []
    ):
        raise ValueError("producer source must be the exact clean candidate")
    current_source_git = exact_source_git(root.resolve(strict=True), commit)
    if source_git != current_source_git:
        raise ValueError("producer source metadata changed before migration execution")
    actions_provenance = load_actions_metadata_binding(
        root=root,
        path=actions_metadata_path,
        commit=commit,
        run_id=run_id,
        run_attempt=run_attempt,
    )
    effective_target = (
        _default_target()
        if requested_target is None
        else requested_target
    )
    resolved_out, reservation = reserve_new_output_path(root, out)
    mutation_state = {"started": False}
    completed = False
    try:
        report = _execute_migration_reserved(
            root=root,
            out=resolved_out,
            serial_module=serial_module,
            port_lister=port_lister,
            requested_target=effective_target,
            platform_name=platform_name,
            target_exists=target_exists,
            target_is_symlink=target_is_symlink,
            target_realpath=target_realpath,
            target_access=target_access,
            target_hostname=target_hostname,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
            expected_d1l_public_key=normalized_public_key,
            expected_legacy_value=expected_legacy_value,
            confirmed_upper_bound=confirmed_upper_bound,
            attest_exact_device_upper_bound=(attest_exact_device_upper_bound),
            timeout=timeout,
            source_git=source_git,
            actions_provenance=actions_provenance,
            mutation_state=mutation_state,
            now=now,
            clock=clock,
        )
        completed = True
        return report
    finally:
        if completed or not mutation_state["started"]:
            reservation.unlink(missing_ok=True)


def _serial_runtime() -> tuple[Any, Callable[[], Iterable[object]]]:
    try:
        import serial
        import serial.tools.list_ports
    except ImportError as exc:
        raise RuntimeError(
            "pyserial is required for exact-device protocol migration"
        ) from exc
    return serial, serial.tools.list_ports.comports


def _resolve(root: Path, value: str) -> Path:
    path = Path(value)
    return Path(os.path.abspath(str(path if path.is_absolute() else root / path)))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--port", default=_default_target())
    parser.add_argument("--baud", type=int, default=D1L_BAUD)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-attempt", required=True)
    parser.add_argument("--core-actions-run-metadata", required=True)
    parser.add_argument("--expected-d1l-public-key", required=True)
    parser.add_argument("--expected-legacy-value", type=int, required=True)
    parser.add_argument("--confirmed-upper-bound", type=int, required=True)
    parser.add_argument(
        "--attest-exact-device-upper-bound",
        action="store_true",
        help=(
            "explicitly attest that the supplied bound covers every predecessor "
            "timestamp, including RAM-only fallback"
        ),
    )
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--out", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if not args.attest_exact_device_upper_bound:
            raise ValueError(
                "--attest-exact-device-upper-bound is required; migration is "
                "never automatic"
            )
        if args.baud != D1L_BAUD:
            raise ValueError(f"only {D1L_BAUD} baud is permitted")
        commit = exact_commit(args.commit)
        if commit is None:
            raise ValueError("--commit must be an exact 40-character SHA")
        run_id = str(args.github_run_id)
        run_attempt = str(args.github_run_attempt)
        if not (
            _positive_ascii_decimal(run_id) and _positive_ascii_decimal(run_attempt)
        ):
            raise ValueError("Actions run and attempt must be positive integers")
        if not _finite_positive_timeout(args.timeout):
            raise ValueError("--timeout must be finite and positive")
        expected_d1l_public_key = exact_public_key(
            args.expected_d1l_public_key
        )
        if expected_d1l_public_key is None:
            raise ValueError(
                "--expected-d1l-public-key must be exactly 64 hexadecimal "
                "characters"
            )
        root = Path(args.root).resolve(strict=True)
        source_git = exact_source_git(root, commit)
        serial_module, port_lister = _serial_runtime()
        report = execute_migration(
            root=root,
            out=_resolve(root, args.out),
            serial_module=serial_module,
            port_lister=port_lister,
            requested_target=args.port,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
            expected_d1l_public_key=expected_d1l_public_key,
            expected_legacy_value=args.expected_legacy_value,
            confirmed_upper_bound=args.confirmed_upper_bound,
            attest_exact_device_upper_bound=(args.attest_exact_device_upper_bound),
            timeout=args.timeout,
            source_git=source_git,
            actions_metadata_path=_resolve(
                root,
                args.core_actions_run_metadata,
            ),
        )
    except (OSError, RuntimeError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(
        json.dumps(
            {
                "ok": report.get("ok") is True,
                "kind": report.get("kind"),
                "out": str(args.out),
                "confirmation_logged": False,
            },
            indent=2,
        )
    )
    return 0 if report.get("ok") is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
