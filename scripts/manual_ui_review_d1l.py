#!/usr/bin/env python3
"""Capture and verify exact-candidate Core 1.0 manual UI review evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import zlib
from collections.abc import Collection, Sequence
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from artifact_metadata import git_metadata
    from core_smoke_d1l import (
        D1L_CORE_PORT,
        enforce_core_port,
    )
    from d1l_serial_target import safe_slug, validate_snapshot
    from rf_full_acceptance_d1l import (
        d1l_identity_status_ok,
        exact_public_key,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import git_metadata
    from scripts.core_smoke_d1l import (
        D1L_CORE_PORT,
        enforce_core_port,
    )
    from scripts.d1l_serial_target import safe_slug, validate_snapshot
    from scripts.rf_full_acceptance_d1l import (
        d1l_identity_status_ok,
        exact_public_key,
    )


REVIEW_SCHEMA = 4
CORE_PORT = D1L_CORE_PORT
CORE_RELEASE_PROFILE = "core_1_0"
CORE_SD_HISTORY_MODE = "disabled"
RELEASE_MIN_ROUNDS = 20
CORE_TAB_SEQUENCE = ("home", "messages", "nodes", "packets", "settings")
CORE_SCROLL_SURFACES = (
    "home",
    "public_messages",
    "dm_thread",
    "nodes",
    "packets",
    "settings",
)
CORE_COMPOSE_TARGETS = (
    "public",
    "public-long",
    "dm",
    "dm-long",
    "public-search",
    "dm-search",
    "packet-search",
    "contact-edit",
    "onboarding",
)
REQUIRED_UI_CHECKS = frozenset(
    {
        "exact_candidate",
        "esp_idf_v5_5_4",
        "core_profile",
        "pre_ui_crashlog_clean",
        "core_tab_sequence_exact",
        "core_scroll_surfaces_exact",
        "core_compose_targets_exact",
        "tab_switches_settle",
        "data_refresh_exercised",
        "data_refreshes_pass",
        "unavailable_destinations_rejected",
        "no_public_rf",
        "no_network_tx",
        "no_map_network_requests",
        "no_formatting",
        "uptime_monotonic",
        "no_stuck_pending",
        "final_active_tab_known",
    }
)
REQUIRED_CONFIRMATIONS = (
    "display_480x480_stable",
    "touch_accurate",
    "backlight_works",
    "home",
    "messages_public",
    "dm_workflow",
    "nodes",
    "packets",
    "settings",
    "unavailable_features_absent",
)
PHOTO_COVERAGE = frozenset(
    {
        "display_touch_backlight",
        "home",
        "messages",
        "nodes",
        "packets",
        "settings",
    }
)
REJECTED_FALSE_FLAGS = (
    "dry_run",
    "simulated",
    "simulation",
    "source_inspection",
    "source_only",
    "fabricated",
    "edited",
    "predecessor",
    "predecessor_evidence_used",
    "reused",
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def exact_sha(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    return normalized if re.fullmatch(r"[0-9a-f]{40}", normalized) else None


def positive_decimal(value: object) -> str | None:
    if isinstance(value, bool):
        return None
    normalized = str(value).strip()
    return normalized if re.fullmatch(r"[1-9][0-9]*", normalized) else None


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_link_or_reparse(path: Path) -> bool:
    try:
        info = path.lstat()
    except OSError:
        return True
    return path.is_symlink() or bool(
        getattr(info, "st_file_attributes", 0)
        & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    )


def _resolved_root(root: Path) -> Path:
    resolved = root.resolve(strict=True)
    if not resolved.is_dir() or is_link_or_reparse(resolved):
        raise ValueError("evidence root must be a direct regular directory")
    return resolved


def resolve_regular_file(root: Path, raw_path: str | Path) -> Path:
    root = _resolved_root(root)
    candidate = Path(raw_path)
    if not candidate.is_absolute():
        candidate = root / candidate
    if is_link_or_reparse(candidate):
        raise ValueError(f"evidence path is a link or reparse point: {raw_path}")
    resolved = candidate.resolve(strict=True)
    try:
        resolved.relative_to(root)
    except ValueError as exc:
        raise ValueError(
            f"evidence path is outside the repository root: {raw_path}"
        ) from exc
    if not resolved.is_file() or is_link_or_reparse(resolved):
        raise ValueError(f"evidence path is not a direct regular file: {raw_path}")
    return resolved


def resolve_output_path(root: Path, raw_path: str | Path) -> Path:
    root = _resolved_root(root)
    candidate = Path(raw_path)
    if not candidate.is_absolute():
        candidate = root / candidate
    if candidate.suffix.lower() != ".json" or not candidate.name:
        raise ValueError("manual review output must be a JSON file")
    if candidate.exists() or candidate.is_symlink():
        raise FileExistsError(f"refusing to overwrite existing receipt: {candidate}")
    candidate.parent.mkdir(parents=True, exist_ok=True)
    if is_link_or_reparse(candidate.parent):
        raise ValueError("manual review output parent cannot be a link")
    parent = candidate.parent.resolve(strict=True)
    try:
        parent.relative_to(root)
    except ValueError as exc:
        raise ValueError("manual review output must remain inside the root") from exc
    resolved = parent / candidate.name
    if resolved.exists() or resolved.is_symlink():
        raise FileExistsError(f"refusing to overwrite existing receipt: {resolved}")
    return resolved


def file_receipt(root: Path, path: Path) -> dict[str, object]:
    root = _resolved_root(root)
    path = resolve_regular_file(root, path)
    return {
        "path": path.relative_to(root).as_posix(),
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def parse_utc_timestamp(value: object) -> datetime | None:
    if not isinstance(value, str) or not re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z", value
    ):
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None or parsed.utcoffset() != timezone.utc.utcoffset(parsed):
        return None
    return parsed


def valid_timestamp_range(started_at: object, ended_at: object) -> bool:
    started = parse_utc_timestamp(started_at)
    ended = parse_utc_timestamp(ended_at)
    return started is not None and ended is not None and ended >= started


def _png_dimensions(raw: bytes) -> tuple[int, int]:
    if len(raw) <= 100 or not raw.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("photo is not a nonempty PNG")
    offset = 8
    chunk_index = 0
    width = 0
    height = 0
    saw_idat = False
    saw_iend = False
    while offset + 12 <= len(raw):
        length = int.from_bytes(raw[offset : offset + 4], "big")
        chunk_type = raw[offset + 4 : offset + 8]
        data_start = offset + 8
        data_end = data_start + length
        crc_end = data_end + 4
        if crc_end > len(raw):
            raise ValueError("PNG chunk extends beyond the file")
        chunk_data = raw[data_start:data_end]
        expected_crc = int.from_bytes(raw[data_end:crc_end], "big")
        observed_crc = zlib.crc32(chunk_type + chunk_data) & 0xFFFFFFFF
        if expected_crc != observed_crc:
            raise ValueError("PNG chunk checksum is invalid")
        if chunk_index == 0:
            if chunk_type != b"IHDR" or length != 13:
                raise ValueError("PNG does not start with a canonical IHDR")
            width = int.from_bytes(chunk_data[0:4], "big")
            height = int.from_bytes(chunk_data[4:8], "big")
            if (
                width <= 0
                or height <= 0
                or chunk_data[10] != 0
                or chunk_data[11] != 0
                or chunk_data[12] not in (0, 1)
            ):
                raise ValueError("PNG IHDR is invalid")
        elif chunk_type == b"IHDR":
            raise ValueError("PNG contains more than one IHDR")
        if chunk_type == b"IDAT":
            saw_idat = saw_idat or bool(chunk_data)
        if chunk_type == b"IEND":
            if length != 0 or crc_end != len(raw):
                raise ValueError("PNG IEND is malformed or not final")
            saw_iend = True
            break
        offset = crc_end
        chunk_index += 1
    if not saw_idat or not saw_iend:
        raise ValueError("PNG is missing IDAT or IEND")
    return width, height


def _normalized_coverage(coverage: object) -> list[str]:
    if not isinstance(coverage, list) or not coverage:
        raise ValueError("photo coverage must be a nonempty list")
    if any(
        not isinstance(item, str) or item not in PHOTO_COVERAGE for item in coverage
    ) or len(coverage) != len(set(coverage)):
        raise ValueError("photo coverage is invalid or duplicated")
    return list(coverage)


def photo_receipt(root: Path, path: str | Path, coverage: object) -> dict[str, object]:
    resolved = resolve_regular_file(root, path)
    if resolved.suffix.lower() != ".png":
        raise ValueError(f"photo is not a PNG: {path}")
    width, height = _png_dimensions(resolved.read_bytes())
    row = file_receipt(root, resolved)
    row.update(
        {
            "width": width,
            "height": height,
            "coverage": _normalized_coverage(coverage),
        }
    )
    return row


def parse_photo_spec(root: Path, spec: str) -> dict[str, object]:
    if "=" not in spec:
        raise ValueError("--photo must use COVERAGE[,COVERAGE...]=PATH")
    raw_coverage, raw_path = spec.split("=", 1)
    coverage = [item.strip() for item in raw_coverage.split(",") if item.strip()]
    return photo_receipt(root, raw_path.strip(), coverage)


def _source_row(value: object, commit: str) -> tuple[bool, dict[str, object]]:
    source = value if isinstance(value, dict) else {}
    canonical = {
        "commit": exact_sha(source.get("commit")),
        "dirty": source.get("dirty"),
        "dirty_entries": source.get("dirty_entries"),
    }
    return (
        canonical
        == {
            "commit": commit,
            "dirty": False,
            "dirty_entries": [],
        },
        canonical,
    )


def _current_source(root: Path, commit: str) -> tuple[bool, dict[str, object]]:
    return _source_row(git_metadata(_resolved_root(root)), commit)


def _profile_ready_health(result: object, commit: str) -> bool:
    return (
        isinstance(result, dict)
        and result.get("ok") is True
        and result.get("cmd") == "health"
        and exact_sha(result.get("build_commit")) == commit
        and result.get("release_profile") == CORE_RELEASE_PROFILE
        and result.get("sd_history_mode") == CORE_SD_HISTORY_MODE
        and result.get("board_ready") is True
        and result.get("ui_ready") is True
    )


def _validated_ui_target(
    data: object,
) -> tuple[bool, str | None, dict[str, Any] | None]:
    if not isinstance(data, dict):
        return False, None, None
    port = data.get("port")
    try:
        canonical = enforce_core_port(port)
        if canonical != port:
            return False, None, None
        target = data.get("d1l_target")
        validate_snapshot(target, canonical)
    except (KeyError, TypeError, ValueError):
        return False, None, None
    return True, canonical, target


def _validate_ui_receipt_data(
    data: dict[str, Any],
    *,
    commit: str,
    run_id: str,
    run_attempt: str,
) -> tuple[bool, list[str], dict[str, object]]:
    reasons: list[str] = []
    target_ok, target_port, target_snapshot = _validated_ui_target(data)
    source_ok, _source = _source_row(data.get("git"), commit)
    checks = data.get("checks")
    timestamps_ok = valid_timestamp_range(data.get("started_at"), data.get("ended_at"))
    exact_checks = (
        isinstance(checks, dict)
        and set(checks) == REQUIRED_UI_CHECKS
        and all(checks[name] is True for name in REQUIRED_UI_CHECKS)
    )
    expected_d1l_public_key = exact_public_key(
        data.get("expected_d1l_public_key")
    )
    d1l_identity_status = data.get("d1l_identity_status")
    d1l_identity_ok = (
        expected_d1l_public_key is not None
        and data.get("expected_d1l_public_key") == expected_d1l_public_key
        and data.get("d1l_identity_ok") is True
        and d1l_identity_status_ok(
            d1l_identity_status,
            expected_d1l_public_key,
        )
    )
    identity_ok = (
        exact_sha(data.get("expected_firmware_commit")) == commit
        and exact_sha(data.get("device_build_commit")) == commit
        and data.get("firmware_identity_required") is True
        and data.get("firmware_identity_ok") is True
        and d1l_identity_ok
        and source_ok
    )
    run_alias = data.get("github_actions_run_attempt")
    run_alias_ok = run_alias is None or run_alias == run_attempt
    contract_ok = (
        data.get("schema") == 2
        and data.get("kind") == "core_ui_corruption_probe"
        and data.get("mode") == "hardware"
        and data.get("ok") is True
        and data.get("closure_eligible") is True
        and data.get("hardware_required") is True
        and data.get("physical_observed") is True
        and data.get("identity_preflight_only") is False
        and target_ok
        and data.get("port") == target_port
        and data.get("release_profile") == CORE_RELEASE_PROFILE
        and data.get("sd_history_mode") == CORE_SD_HISTORY_MODE
        and data.get("github_actions_run") == run_id
        and data.get("workflow_run_attempt") == run_attempt
        and run_alias_ok
        and isinstance(data.get("rounds"), int)
        and not isinstance(data.get("rounds"), bool)
        and data["rounds"] >= RELEASE_MIN_ROUNDS
        and data.get("tabs") == list(CORE_TAB_SEQUENCE)
        and data.get("scroll_surfaces") == list(CORE_SCROLL_SURFACES)
        and data.get("compose_targets") == list(CORE_COMPOSE_TARGETS)
        and data.get("skip_data_canary") is False
        and data.get("data_refresh_events") == data.get("rounds")
        and data.get("failure_count") == 0
        and data.get("failures") == []
        and data.get("identity_failures") == []
        and data.get("telemetry_failures") == []
        and data.get("public_rf_tx") is False
        and data.get("network_tx") is False
        and data.get("map_network_requests") is False
        and data.get("formats_sd") is False
        and all(data.get(flag) is not True for flag in REJECTED_FALSE_FLAGS)
    )
    final_health_ok = _profile_ready_health(data.get("final_health"), commit)
    if not identity_ok:
        reasons.append("automated_ui_identity_invalid")
    if not target_ok:
        reasons.append("automated_ui_target_invalid")
    if not contract_ok:
        reasons.append("automated_ui_contract_invalid")
    if not exact_checks:
        reasons.append("automated_ui_checks_not_exact")
    if not timestamps_ok:
        reasons.append("automated_ui_timestamps_invalid")
    if not final_health_ok:
        reasons.append("automated_ui_final_health_invalid")
    return (
        not reasons,
        reasons,
        {
            "identity_ok": identity_ok,
            "contract_ok": contract_ok,
            "checks_exact": exact_checks,
            "timestamps_ok": timestamps_ok,
            "final_health_ok": final_health_ok,
            "target_ok": target_ok,
            "target_port": target_port,
            "d1l_target": target_snapshot,
            "expected_d1l_public_key": expected_d1l_public_key,
            "d1l_identity_status": d1l_identity_status,
            "d1l_identity_ok": d1l_identity_ok,
        },
    )


def validate_ui_receipt(
    data: dict[str, Any],
    *,
    commit: str,
    run_id: str,
    run_attempt: str,
) -> bool:
    """Return whether one automated Core UI receipt has strict candidate identity."""
    normalized_commit = exact_sha(commit)
    normalized_run = positive_decimal(run_id)
    normalized_attempt = positive_decimal(run_attempt)
    if (
        normalized_commit is None
        or normalized_run is None
        or normalized_attempt is None
    ):
        return False
    valid, _reasons, _details = _validate_ui_receipt_data(
        data,
        commit=normalized_commit,
        run_id=normalized_run,
        run_attempt=normalized_attempt,
    )
    return valid


def _validate_photos(
    rows: object, root: Path
) -> tuple[bool, list[str], list[dict[str, object]]]:
    reasons: list[str] = []
    recomputed: list[dict[str, object]] = []
    if not isinstance(rows, list):
        return False, ["photo_receipts_not_a_list"], recomputed
    hashes: set[object] = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            reasons.append(f"photo_receipt_{index}_invalid")
            continue
        try:
            expected = photo_receipt(
                root, str(row.get("path") or ""), row.get("coverage")
            )
        except (OSError, RuntimeError, ValueError):
            reasons.append(f"photo_receipt_{index}_invalid")
            continue
        recomputed.append(expected)
        if row != expected:
            reasons.append(f"photo_receipt_{index}_mismatch")
        digest = expected["sha256"]
        if digest in hashes:
            reasons.append("photo_receipt_hash_duplicated")
        hashes.add(digest)
    return not reasons, reasons, recomputed


def validate_core_manual_ui_review_receipt(
    path: Path,
    *,
    root: Path,
    core_ui_path: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
) -> tuple[bool, list[str], dict[str, Any]]:
    """Recompute one Core manual UI receipt from its exact retained files."""
    reasons: list[str] = []
    details: dict[str, Any] = {}
    normalized_commit = exact_sha(commit)
    normalized_run = positive_decimal(run_id)
    normalized_attempt = positive_decimal(run_attempt)
    if (
        normalized_commit is None
        or normalized_run is None
        or normalized_attempt is None
    ):
        return (
            False,
            ["validator_expected_candidate_identity_invalid"],
            details,
        )
    root = _resolved_root(root)
    try:
        receipt_path = resolve_regular_file(root, path)
    except (OSError, RuntimeError, ValueError):
        return False, ["receipt_file_invalid"], details
    data = read_json(receipt_path)
    if not data:
        reasons.append("receipt_json_invalid")

    try:
        ui_path = resolve_regular_file(root, core_ui_path)
        expected_ui_row = file_receipt(root, ui_path)
        ui_data = read_json(ui_path)
    except (OSError, RuntimeError, ValueError):
        ui_path = None
        expected_ui_row = None
        ui_data = {}
        reasons.append("automated_ui_file_invalid")
    ui_row_ok = (
        expected_ui_row is not None
        and data.get("automated_ui_receipt") == expected_ui_row
    )
    if not ui_row_ok:
        reasons.append("automated_ui_file_row_mismatch")
    ui_valid, ui_reasons, ui_details = _validate_ui_receipt_data(
        ui_data,
        commit=normalized_commit,
        run_id=normalized_run,
        run_attempt=normalized_attempt,
    )
    reasons.extend(ui_reasons)
    ui_target_port = ui_details.get("target_port")
    ui_target_snapshot = ui_details.get("d1l_target")
    ui_public_key = ui_details.get("expected_d1l_public_key")
    ui_identity_status = ui_details.get("d1l_identity_status")
    manual_identity_ok = (
        isinstance(ui_public_key, str)
        and data.get("expected_d1l_public_key") == ui_public_key
        and data.get("d1l_identity_status") == ui_identity_status
        and data.get("d1l_identity_ok") is True
        and d1l_identity_status_ok(ui_identity_status, ui_public_key)
    )
    if not manual_identity_ok:
        reasons.append("manual_ui_d1l_identity_binding_invalid")
    manual_target_ok = (
        isinstance(ui_target_port, str)
        and data.get("port") == ui_target_port
        and data.get("d1l_target") == ui_target_snapshot
    )
    if manual_target_ok:
        try:
            validate_snapshot(data.get("d1l_target"), ui_target_port)
        except (TypeError, ValueError):
            manual_target_ok = False
    if not manual_target_ok:
        reasons.append("manual_ui_target_binding_invalid")

    photos_ok, photo_reasons, recomputed_photos = _validate_photos(
        data.get("photo_receipts"), root
    )
    reasons.extend(photo_reasons)
    confirmations = data.get("confirmations")
    confirmations_ok = (
        isinstance(confirmations, dict)
        and set(confirmations) == set(REQUIRED_CONFIRMATIONS)
        and all(confirmations[name] is True for name in REQUIRED_CONFIRMATIONS)
        and data.get("required_confirmations") == list(REQUIRED_CONFIRMATIONS)
        and data.get("missing_confirmations") == []
    )
    if not confirmations_ok:
        reasons.append("confirmations_not_exact")
    operator = data.get("operator")
    reviewer = data.get("reviewer")
    people_ok = (
        isinstance(operator, str)
        and bool(operator)
        and len(operator) <= 200
        and operator == operator.strip()
        and "\x00" not in operator
        and "\n" not in operator
        and "\r" not in operator
        and isinstance(reviewer, str)
        and bool(reviewer)
        and len(reviewer) <= 200
        and reviewer == reviewer.strip()
        and "\x00" not in reviewer
        and "\n" not in reviewer
        and "\r" not in reviewer
        and operator.casefold() != reviewer.casefold()
    )
    if not people_ok:
        reasons.append("operator_reviewer_invalid")
    timestamps_ok = valid_timestamp_range(
        data.get("started_at"), data.get("ended_at")
    ) and data.get("created_at") == data.get("ended_at")
    if not timestamps_ok:
        reasons.append("review_timestamps_invalid")
    receipt_source_ok, receipt_source = _source_row(data.get("git"), normalized_commit)
    current_source_ok, current_source = _current_source(root, normalized_commit)
    if not receipt_source_ok:
        reasons.append("receipt_source_git_invalid")
    if not current_source_ok:
        reasons.append("current_source_git_invalid")
    rejected_flags_ok = all(data.get(flag) is False for flag in REJECTED_FALSE_FLAGS)
    if not rejected_flags_ok:
        reasons.append("rejected_flags_not_explicitly_false")
    run_attempts_ok = (
        data.get("github_actions_run_attempt") == normalized_attempt
        and data.get("workflow_run_attempt") == normalized_attempt
        and data.get("github_actions_run_attempt") == data.get("workflow_run_attempt")
    )
    if not run_attempts_ok:
        reasons.append("run_attempt_aliases_invalid")
    notes = data.get("notes")
    notes_ok = isinstance(notes, str) and notes == notes.strip() and len(notes) <= 4000
    if not notes_ok:
        reasons.append("notes_invalid")
    contract_ok = (
        data.get("schema") == REVIEW_SCHEMA
        and data.get("kind") == "core_manual_ui_review"
        and data.get("mode") == "manual-physical-ui-review"
        and data.get("ok") is True
        and data.get("closure_eligible") is True
        and data.get("hardware_required") is True
        and data.get("physical_observed") is True
        and manual_target_ok
        and manual_identity_ok
        and data.get("port") == ui_target_port
        and data.get("release_profile") == CORE_RELEASE_PROFILE
        and data.get("sd_history_mode") == CORE_SD_HISTORY_MODE
        and exact_sha(data.get("commit")) == normalized_commit
        and data.get("github_actions_run") == normalized_run
        and data.get("candidate_source_clean") is True
        and data.get("automated_ui_receipt_valid") is True
        and data.get("photos_optional") is True
        and data.get("photo_receipts_valid") is True
        and data.get("public_rf_tx") is False
        and data.get("dm_rf_tx") is False
        and data.get("network_tx") is False
        and data.get("map_network_requests") is False
        and data.get("formats_sd") is False
    )
    if not contract_ok:
        reasons.append("receipt_contract_invalid")
    reasons = list(dict.fromkeys(reasons))
    valid = (
        not reasons
        and ui_path is not None
        and ui_row_ok
        and ui_valid
        and photos_ok
        and confirmations_ok
        and people_ok
        and timestamps_ok
        and receipt_source_ok
        and current_source_ok
        and rejected_flags_ok
        and run_attempts_ok
        and notes_ok
        and contract_ok
        and manual_target_ok
        and manual_identity_ok
    )
    details.update(
        {
            "receipt_file": file_receipt(root, receipt_path),
            "automated_ui_file": expected_ui_row,
            "automated_ui_file_row_ok": ui_row_ok,
            "automated_ui_valid": ui_valid,
            "automated_ui_details": ui_details,
            "photos_valid": photos_ok,
            "recomputed_photo_receipts": recomputed_photos,
            "confirmations_exact": confirmations_ok,
            "operator_reviewer_distinct": people_ok,
            "timestamps_valid": timestamps_ok,
            "receipt_source_git_ok": receipt_source_ok,
            "receipt_source_git": receipt_source,
            "current_source_git_ok": current_source_ok,
            "current_source_git": current_source,
            "rejected_flags_explicitly_false": rejected_flags_ok,
            "run_attempt_aliases_exact": run_attempts_ok,
            "receipt_contract_ok": contract_ok,
            "manual_target_binding_ok": manual_target_ok,
            "manual_d1l_identity_binding_ok": manual_identity_ok,
            "expected_d1l_public_key": ui_public_key,
        }
    )
    return valid, reasons, details


def _operator_label(value: object, label: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be text")
    normalized = value.strip()
    if (
        not normalized
        or len(normalized) > 200
        or any(character in normalized for character in ("\x00", "\r", "\n"))
    ):
        raise ValueError(f"{label} is invalid")
    return normalized


def _write_json_exclusive(path: Path, report: dict[str, Any]) -> None:
    serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
    with path.open("x", encoding="ascii", newline="\n") as handle:
        handle.write(serialized)


def capture(
    *,
    root: Path,
    out_path: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
    port: str,
    operator: str,
    reviewer: str,
    confirmed: Collection[str],
    core_ui_path: Path,
    photo_specs: Sequence[str] = (),
    notes: str = "",
) -> dict[str, Any]:
    """Create one non-overwriting receipt after recomputing every objective input."""
    started_at = utc_now()
    root = _resolved_root(root)
    normalized_commit = exact_sha(commit)
    normalized_run = positive_decimal(run_id)
    normalized_attempt = positive_decimal(run_attempt)
    if normalized_commit is None:
        raise ValueError("candidate commit must be an exact 40-hex SHA")
    if normalized_run is None or normalized_attempt is None:
        raise ValueError("GitHub run ID and attempt must be positive decimals")
    requested_port = enforce_core_port(port)
    if (
        isinstance(confirmed, (str, bytes, dict))
        or any(not isinstance(name, str) for name in confirmed)
        or set(confirmed) != set(REQUIRED_CONFIRMATIONS)
    ):
        raise ValueError("every exact Core manual confirmation is required")
    operator = _operator_label(operator, "operator")
    reviewer = _operator_label(reviewer, "reviewer")
    if operator.casefold() == reviewer.casefold():
        raise ValueError("operator and reviewer must be distinct people")
    if not isinstance(notes, str) or len(notes.strip()) > 4000:
        raise ValueError("notes must contain at most 4000 characters")
    notes = notes.strip()
    current_source_ok, _current = _current_source(root, normalized_commit)
    if not current_source_ok:
        raise ValueError("current source is not the exact clean candidate")
    core_ui_path = resolve_regular_file(root, core_ui_path)
    core_ui_data = read_json(core_ui_path)
    ui_valid, ui_reasons, ui_details = _validate_ui_receipt_data(
        core_ui_data,
        commit=normalized_commit,
        run_id=normalized_run,
        run_attempt=normalized_attempt,
    )
    if not ui_valid:
        raise ValueError(
            "automated Core UI receipt is invalid: " + ", ".join(ui_reasons)
        )
    ui_target_port = ui_details.get("target_port")
    ui_target_snapshot = ui_details.get("d1l_target")
    ui_public_key = ui_details.get("expected_d1l_public_key")
    ui_identity_status = ui_details.get("d1l_identity_status")
    if (
        not isinstance(ui_target_port, str)
        or requested_port != ui_target_port
        or not isinstance(ui_target_snapshot, dict)
        or not isinstance(ui_public_key, str)
        or not d1l_identity_status_ok(ui_identity_status, ui_public_key)
    ):
        raise ValueError(
            "manual UI target and full D1L identity must exactly match the "
            "automated UI receipt"
        )
    validate_snapshot(ui_target_snapshot, ui_target_port)
    ui_row = file_receipt(root, core_ui_path)
    if isinstance(photo_specs, (str, bytes)):
        raise ValueError("photo specs must be a sequence of complete specifications")
    photo_rows = [parse_photo_spec(root, spec) for spec in photo_specs]
    hashes = [row["sha256"] for row in photo_rows]
    if len(hashes) != len(set(hashes)):
        raise ValueError("photo evidence cannot reuse the same file hash")
    out_path = resolve_output_path(root, out_path)
    ended_at = utc_now()
    report: dict[str, Any] = {
        "schema": REVIEW_SCHEMA,
        "kind": "core_manual_ui_review",
        "mode": "manual-physical-ui-review",
        "created_at": ended_at,
        "started_at": started_at,
        "ended_at": ended_at,
        "ok": True,
        "closure_eligible": True,
        "hardware_required": True,
        "physical_observed": True,
        "dry_run": False,
        "simulated": False,
        "simulation": False,
        "source_inspection": False,
        "source_only": False,
        "fabricated": False,
        "edited": False,
        "predecessor": False,
        "predecessor_evidence_used": False,
        "reused": False,
        "port": ui_target_port,
        "d1l_target": ui_target_snapshot,
        "expected_d1l_public_key": ui_public_key,
        "d1l_identity_status": ui_identity_status,
        "d1l_identity_ok": True,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": CORE_SD_HISTORY_MODE,
        "commit": normalized_commit,
        "github_actions_run": normalized_run,
        "github_actions_run_attempt": normalized_attempt,
        "workflow_run_attempt": normalized_attempt,
        "git": {
            "commit": normalized_commit,
            "dirty": False,
            "dirty_entries": [],
        },
        "candidate_source_clean": True,
        "operator": operator,
        "reviewer": reviewer,
        "confirmations": {name: True for name in REQUIRED_CONFIRMATIONS},
        "required_confirmations": list(REQUIRED_CONFIRMATIONS),
        "missing_confirmations": [],
        "automated_ui_receipt": ui_row,
        "automated_ui_receipt_valid": True,
        "photo_receipts": photo_rows,
        "photo_receipts_valid": True,
        "photos_optional": True,
        "notes": notes,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "network_tx": False,
        "map_network_requests": False,
        "formats_sd": False,
    }
    _write_json_exclusive(out_path, report)
    valid, reasons, _details = validate_core_manual_ui_review_receipt(
        out_path,
        root=root,
        core_ui_path=core_ui_path,
        commit=normalized_commit,
        run_id=normalized_run,
        run_attempt=normalized_attempt,
    )
    if not valid:
        out_path.unlink(missing_ok=True)
        raise RuntimeError(
            "new manual UI receipt failed strict validation: " + ", ".join(reasons)
        )
    return report


def default_out_path(
    root: Path,
    commit: str,
    run_id: str,
    target: str = CORE_PORT,
) -> Path:
    slug = safe_slug(enforce_core_port(target))
    return (
        root
        / "artifacts"
        / "hardware"
        / slug
        / f"core_manual_ui_review_{commit[:12]}_run_{run_id}_{slug}.json"
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT") or CORE_PORT)
    parser.add_argument("--expected-firmware-commit", required=True)
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-attempt", required=True)
    parser.add_argument("--operator", required=True)
    parser.add_argument("--reviewer", required=True)
    parser.add_argument("--automated-ui-receipt", required=True)
    parser.add_argument(
        "--photo",
        action="append",
        default=[],
        metavar="COVERAGE[,COVERAGE...]=PATH",
        help="Optional current-candidate PNG evidence; repeat for more photos.",
    )
    parser.add_argument("--notes", default="")
    for name in REQUIRED_CONFIRMATIONS:
        parser.add_argument(
            "--confirm-" + name.replace("_", "-"),
            action="store_true",
            dest=name,
        )
    parser.add_argument("--out")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    commit = exact_sha(args.expected_firmware_commit)
    run_id = positive_decimal(args.github_run_id)
    run_attempt = positive_decimal(args.github_run_attempt)
    if commit is None:
        raise SystemExit("--expected-firmware-commit must be an exact 40-hex SHA")
    if run_id is None or run_attempt is None:
        raise SystemExit("GitHub run ID and attempt must be positive decimals")
    out_path = (
        Path(args.out)
        if args.out
        else default_out_path(root, commit, run_id, args.port)
    )
    confirmed = {name for name in REQUIRED_CONFIRMATIONS if getattr(args, name) is True}
    try:
        report = capture(
            root=root,
            out_path=out_path,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
            port=args.port,
            operator=args.operator,
            reviewer=args.reviewer,
            confirmed=confirmed,
            core_ui_path=Path(args.automated_ui_receipt),
            photo_specs=args.photo,
            notes=args.notes,
        )
    except (OSError, RuntimeError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc
    resolved_out = resolve_regular_file(root, out_path)
    print(
        json.dumps(
            {
                "ok": report["ok"],
                "out": str(resolved_out),
                "kind": report["kind"],
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
