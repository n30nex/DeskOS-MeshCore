#!/usr/bin/env python3
"""Build one bounded RC1 physical receipt from machine-generated evidence.

This script does not flash, transmit, format media, or run a soak.  It accepts
only structured receipts emitted by the existing hardware runners plus two
bounded serial transcripts for the protocol/admin and live Map gates.  Every
accepted source is copied into an immutable evidence bundle and SHA-256 bound
to a sidecar consumed by the release audit.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import sys
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any, Callable

try:
    from d1l_serial_target import POSIX_D1L_TARGET, validate_snapshot
    from rc1_release_gate_audit_d1l import (
        APP_NAME,
        PI_HOST,
        RECEIPT_KIND,
        RECEIPT_SCHEMA,
        RELEASE_PROFILE,
        SD_HISTORY_MODE,
        app_artifact,
        load_json as load_package_json,
        manifest_identity,
        sha256_file,
        verify_checksum_tree,
    )
    from release_gate_audit_d1l import (
        full_rf_acceptance_ok,
        scroll_probe_ok,
        sd_reboot_remount_artifact_ok,
    )
    from scroll_probe_d1l import crashlog_has_crash_like_entries
    from wifi_resilience_d1l import validate_completed_report as validate_wifi_report
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.d1l_serial_target import POSIX_D1L_TARGET, validate_snapshot
    from scripts.rc1_release_gate_audit_d1l import (
        APP_NAME,
        PI_HOST,
        RECEIPT_KIND,
        RECEIPT_SCHEMA,
        RELEASE_PROFILE,
        SD_HISTORY_MODE,
        app_artifact,
        load_json as load_package_json,
        manifest_identity,
        sha256_file,
        verify_checksum_tree,
    )
    from scripts.release_gate_audit_d1l import (
        full_rf_acceptance_ok,
        scroll_probe_ok,
        sd_reboot_remount_artifact_ok,
    )
    from scripts.scroll_probe_d1l import crashlog_has_crash_like_entries
    from scripts.wifi_resilience_d1l import (
        validate_completed_report as validate_wifi_report,
    )


EVIDENCE_SCHEMA = 1
EVIDENCE_KIND = "d1l_rc1_bounded_physical_acceptance_evidence"
PROTOCOL_KIND = "d1l_rc1_protocol_acceptance_transcript"
MAP_KIND = "d1l_rc1_map_acceptance_transcript"
USB_VID = "1a86"
USB_PID = "7523"
SHA_RE = re.compile(r"[0-9a-f]{64}\Z")
COMMIT_RE = re.compile(r"[0-9a-f]{40}\Z")
POSITIVE_DECIMAL_RE = re.compile(r"[1-9][0-9]*\Z")

SOURCE_KINDS = {
    "flash": "esp32_flash",
    "ui": "scroll_probe_d1l",
    "rf": "rf_full_acceptance",
    "protocol": PROTOCOL_KIND,
    "wifi": "wifi_saved_profile_resilience",
    "sd": "sd_reboot_remount_acceptance_d1l",
    "sd_degraded": "d1l_sd_remove_reinsert_source",
    "map": MAP_KIND,
}
SOURCE_ROLES = tuple(SOURCE_KINDS)
OUTCOME_KEYS = (
    "boot",
    "ui_navigation",
    "boot_advert",
    "public_send_count",
    "dm_ack",
    "path",
    "trace",
    "ping",
    "repeater_login",
    "repeater_query",
    "wifi_reconnect",
    "sd_write",
    "sd_remount",
    "sd_degraded_notice",
    "authorized_map_download",
    "map_cache_revisit",
    "no_panic",
    "no_unexpected_reset",
)
COVERAGE = {
    "target": "flash",
    "flash": "flash",
    "boot": "ui",
    "ui_navigation": "ui",
    "boot_advert": "protocol",
    "public_send_count": "protocol",
    "dm_ack": "rf",
    "path": "protocol",
    "trace": "protocol",
    "ping": "protocol",
    "repeater_login": "protocol",
    "repeater_query": "protocol",
    "wifi_reconnect": "wifi",
    "sd_write": "sd",
    "sd_remount": "sd",
    "sd_degraded_notice": "sd_degraded",
    "authorized_map_download": "map",
    "map_cache_revisit": "map",
    "no_panic": "ui",
    "no_unexpected_reset": "ui",
}
TRANSCRIPT_KEYS = frozenset(
    {
        "schema",
        "kind",
        "mode",
        "physical_observed",
        "simulated",
        "dry_run",
        "manual_only",
        "port",
        "d1l_target",
        "d1l_target_after",
        "runner_commit",
        "runner_source_clean",
        "expected_firmware_commit",
        "github_actions_run",
        "workflow_run_attempt",
        "steps",
    }
)
STEP_KEYS = frozenset({"sequence", "operation", "command", "response"})
PROTOCOL_OPERATIONS = frozenset(
    {
        "version",
        "identity",
        "health_before",
        "mesh_status",
        "peer_advert",
        "contacts",
        "trace_request",
        "trace_result",
        "peer_before",
        "public_tx_authorization",
        "public_send",
        "public_tx_record",
        "peer_after_public",
        "peer_public_send",
        "public_receive",
        "peer_before_dm",
        "dm_send",
        "dm_ack",
        "peer_after_dm",
        "peer_dm_send",
        "dm_receive_ack",
        "path_request",
        "path_result",
        "admin_login_request",
        "admin_login_status",
        "admin_query_request",
        "admin_query_status",
        "admin_logout",
        "ping_request",
        "ping_result",
        "health_after",
        "crashlog",
    }
)
MAP_OPERATIONS = frozenset(
    {
        "version",
        "provider",
        "before",
        "download",
        "revisit",
        "health",
        "crashlog",
    }
)


class EvidenceError(ValueError):
    """Raised when an input cannot qualify the bounded physical receipt."""


class DuplicateJsonKey(ValueError):
    pass


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise DuplicateJsonKey(key)
        value[key] = item
    return value


def is_link_or_reparse(path: Path) -> bool:
    try:
        info = path.lstat()
    except OSError:
        return True
    attributes = getattr(info, "st_file_attributes", 0)
    reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return stat.S_ISLNK(info.st_mode) or bool(attributes & reparse)


def load_source(path: Path, role: str) -> dict[str, Any]:
    path = Path(path)
    if is_link_or_reparse(path) or not path.is_file():
        raise EvidenceError(f"{role} source is missing or linked: {path}")
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_strict_object,
        )
    except (DuplicateJsonKey, OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"{role} source is not strict JSON: {path}") from exc
    if type(value) is not dict:
        raise EvidenceError(f"{role} source must be a JSON object")
    return value


def _exact_commit(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    return normalized if COMMIT_RE.fullmatch(normalized) else None


def _positive_decimal(value: object) -> str | None:
    normalized = str(value) if isinstance(value, (str, int)) else ""
    return normalized if POSITIVE_DECIMAL_RE.fullmatch(normalized) else None


def _integer(value: object, *, minimum: int = 0) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        return None
    return value


def _machine_physical(data: dict[str, Any], *, mode: str) -> bool:
    return (
        data.get("mode") == mode
        and data.get("physical_observed") is True
        and data.get("dry_run") is False
        and data.get("simulated") is False
        and data.get("simulation") is not True
        and data.get("source_inspection") is not True
        and data.get("manual_only") is not True
    )


def _candidate_binding(
    data: dict[str, Any],
    candidate: dict[str, str],
    *,
    commit_field: str = "expected_firmware_commit",
    require_run: bool,
) -> bool:
    if _exact_commit(data.get(commit_field)) != candidate["firmware_commit"]:
        return False
    if require_run:
        return (
            _positive_decimal(data.get("github_actions_run"))
            == candidate["actions_run"]
            and _positive_decimal(
                data.get(
                    "workflow_run_attempt",
                    data.get("github_actions_run_attempt"),
                )
            )
            == candidate["actions_run_attempt"]
        )
    return True


def _runner_source_binding(
    data: dict[str, Any], candidate: dict[str, str]
) -> bool:
    git = data.get("git")
    return (
        _exact_commit(data.get("commit")) == candidate["firmware_commit"]
        and isinstance(git, dict)
        and _exact_commit(git.get("commit")) == candidate["firmware_commit"]
        and git.get("status_ok") is True
        and git.get("dirty") is False
        and git.get("dirty_entries") == []
    )


def _target(data: dict[str, Any], field: str = "d1l_target") -> dict[str, Any]:
    snapshot = data.get(field)
    try:
        validate_snapshot(snapshot, POSIX_D1L_TARGET)
    except (TypeError, ValueError) as exc:
        raise EvidenceError(f"{field} is not the stable D1L target") from exc
    if snapshot.get("hostname") != PI_HOST:
        raise EvidenceError(f"{field} hostname is not {PI_HOST}")
    return snapshot


def _target_pair(data: dict[str, Any]) -> bool:
    before = _target(data)
    after = _target(data, "d1l_target_after")
    return before["stable_identity_sha256"] == after["stable_identity_sha256"]


def _find_app_row(value: object, app_sha256: str) -> bool:
    if isinstance(value, dict):
        rows = value.get("flash_files")
        if isinstance(rows, list) and any(
            isinstance(row, dict)
            and Path(str(row.get("path") or row.get("source") or "")).name
            == APP_NAME
            and str(row.get("sha256") or "").lower() == app_sha256
            for row in rows
        ):
            return True
        return any(_find_app_row(item, app_sha256) for item in value.values())
    if isinstance(value, list):
        return any(_find_app_row(item, app_sha256) for item in value)
    return False


def validate_flash(
    data: dict[str, Any], candidate: dict[str, str]
) -> dict[str, bool | int]:
    result = data.get("result")
    if not (
        data.get("schema") == 2
        and data.get("kind") == "esp32_flash"
        and _machine_physical(data, mode="hardware")
        and data.get("ok") is True
        and data.get("closure_eligible") is True
        and data.get("release_profile") == RELEASE_PROFILE
        and data.get("sd_history_mode") == SD_HISTORY_MODE
        and _candidate_binding(
            data, candidate, commit_field="commit", require_run=True
        )
        and _exact_commit(data.get("device_build_commit"))
        == candidate["firmware_commit"]
        and data.get("erase_flash") is False
        and data.get("formats_sd") is False
        and data.get("retained_state_preserved") is True
        and isinstance(result, dict)
        and result.get("name") == "esp32_flash"
        and result.get("ok") is True
        and _target_pair(data)
        and _find_app_row(data, candidate["app_sha256"])
    ):
        raise EvidenceError("flash receipt does not prove the exact non-erasing app flash")
    return {}


def _ui_health_truth(data: dict[str, Any], commit: str) -> bool:
    events = data.get("events")
    if not isinstance(events, list) or not events:
        return False
    nonces: set[int] = set()
    for event in events:
        if not isinstance(event, dict):
            return False
        health = event.get("health")
        crashlog = event.get("crashlog")
        if (
            not isinstance(health, dict)
            or health.get("ok") is not True
            or _exact_commit(health.get("build_commit")) != commit
            or health.get("board_ready") is not True
            or health.get("ui_ready") is not True
            or _integer(health.get("boot_nonce"), minimum=1) is None
            or not isinstance(crashlog, dict)
            or crashlog.get("ok") is not True
            or crashlog_has_crash_like_entries(crashlog)
        ):
            return False
        nonces.add(health["boot_nonce"])
    return len(nonces) == 1


def validate_ui(
    data: dict[str, Any], candidate: dict[str, str]
) -> dict[str, bool | int]:
    if not (
        data.get("schema") == 2
        and data.get("mode") == "hardware"
        and data.get("physical_observed") is True
        and data.get("manual_touch") is False
        and data.get("dry_run") is not True
        and data.get("simulated") is not True
        and data.get("manual_only") is not True
        and data.get("closure_eligible") is True
        and data.get("release_profile") == RELEASE_PROFILE
        and data.get("expected_sd_history_mode") == SD_HISTORY_MODE
        and _candidate_binding(data, candidate, require_run=True)
        and _runner_source_binding(data, candidate)
        and scroll_probe_ok(data, POSIX_D1L_TARGET)
        and _ui_health_truth(data, candidate["firmware_commit"])
        and _target(data)
    ):
        raise EvidenceError("UI receipt is not an exact automated physical navigation pass")
    return {
        "boot": True,
        "ui_navigation": True,
        "no_panic": True,
        "no_unexpected_reset": True,
    }


def validate_rf(
    data: dict[str, Any],
    candidate: dict[str, str],
    *,
    evidence_root: Path,
) -> dict[str, bool | int]:
    checks = data.get("checks")
    if not (
        _machine_physical(data, mode="rf-full-acceptance")
        and data.get("execution_complete") is True
        and data.get("closure_eligible") is True
        and _candidate_binding(data, candidate, require_run=True)
        and _runner_source_binding(data, candidate)
        and _exact_commit(data.get("device_build_commit"))
        == candidate["firmware_commit"]
        and data.get("device_release_profile") == RELEASE_PROFILE
        and data.get("device_sd_history_mode") == SD_HISTORY_MODE
        and _target_pair(data)
        and full_rf_acceptance_ok(
            data,
            POSIX_D1L_TARGET,
            evidence_root=evidence_root,
        )
        and isinstance(checks, dict)
        and checks.get("ack_path") is True
    ):
        raise EvidenceError("RF receipt is not an exact controlled-peer DM/ACK pass")
    return {"dm_ack": True}


def _transcript(
    data: dict[str, Any],
    candidate: dict[str, str],
    *,
    kind: str,
    operations: frozenset[str],
) -> dict[str, dict[str, Any]]:
    if (
        set(data) != TRANSCRIPT_KEYS
        or data.get("schema") != 1
        or data.get("kind") != kind
        or not _machine_physical(data, mode="hardware")
        or data.get("manual_only") is not False
        or data.get("port") != POSIX_D1L_TARGET
        or _exact_commit(data.get("runner_commit"))
        != candidate["firmware_commit"]
        or data.get("runner_source_clean") is not True
        or not _candidate_binding(data, candidate, require_run=True)
        or not _target_pair(data)
    ):
        raise EvidenceError(f"{kind} identity/truth envelope is invalid")
    steps = data.get("steps")
    if not isinstance(steps, list) or len(steps) != len(operations):
        raise EvidenceError(f"{kind} has an incomplete step set")
    by_operation: dict[str, dict[str, Any]] = {}
    for sequence, step in enumerate(steps, start=1):
        if (
            type(step) is not dict
            or set(step) != STEP_KEYS
            or step.get("sequence") != sequence
            or step.get("operation") not in operations
            or step["operation"] in by_operation
            or not isinstance(step.get("command"), str)
            or not isinstance(step.get("response"), dict)
        ):
            raise EvidenceError(f"{kind} step {sequence} is invalid")
        by_operation[step["operation"]] = step
    if set(by_operation) != operations:
        raise EvidenceError(f"{kind} operation coverage is not exact")
    return by_operation


def _response(
    steps: dict[str, dict[str, Any]], operation: str
) -> dict[str, Any]:
    return steps[operation]["response"]


def _public_key(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    return normalized if SHA_RE.fullmatch(normalized) else None


def _fingerprint(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().upper()
    return (
        normalized
        if re.fullmatch(r"[0-9A-F]{16}", normalized)
        else None
    )


def _aware_timestamp(value: object) -> datetime | None:
    if not isinstance(value, str) or not value:
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed.astimezone(timezone.utc) if parsed.tzinfo else None


def _peer_snapshot(value: object) -> dict[str, Any]:
    if (
        type(value) is not dict
        or set(value)
        != {
            "source",
            "path",
            "captured_at",
            "snapshot_sha256",
            "snapshot",
        }
        or value.get("source") != "local_peer_status_file"
        or not isinstance(value.get("path"), str)
        or not PurePosixPath(value["path"]).is_absolute()
        or not isinstance(value.get("snapshot"), dict)
    ):
        raise EvidenceError("controlled-peer status capture shape is invalid")
    snapshot = value["snapshot"]
    captured = _aware_timestamp(value.get("captured_at"))
    written = _aware_timestamp(snapshot.get("status_written_at"))
    freshness = (
        (captured - written).total_seconds()
        if captured is not None and written is not None
        else -1.0
    )
    serial = snapshot.get("serial")
    if not (
        value.get("snapshot_sha256")
        == hashlib.sha256(canonical_json(snapshot)).hexdigest()
        and snapshot.get("service") == "openclaw-radio-listener"
        and isinstance(snapshot.get("run_id"), str)
        and bool(snapshot["run_id"])
        and isinstance(serial, dict)
        and serial.get("mesh_connected") is True
        and serial.get("port") == "/dev/krab-t-echo"
        and _public_key(serial.get("public_key")) is not None
        and isinstance(snapshot.get("counters"), dict)
        and isinstance(snapshot.get("mesh"), dict)
        and 0.0 <= freshness <= 120.0
    ):
        raise EvidenceError("controlled-peer status identity or freshness is invalid")
    return snapshot


def _peer_counter(snapshot: dict[str, Any], name: str) -> int | None:
    counters = snapshot.get("counters")
    return (
        _integer(counters.get(name))
        if isinstance(counters, dict)
        else None
    )


def _control_exchange(
    value: object, operation: str
) -> tuple[dict[str, Any], dict[str, Any]]:
    if type(value) is not dict or set(value) != {"request", "response"}:
        raise EvidenceError(f"{operation} control exchange shape is invalid")
    request = value["request"]
    response = value["response"]
    if not (
        type(request) is dict
        and set(request) == {"id", "op", "params"}
        and isinstance(request.get("id"), str)
        and bool(request["id"])
        and request.get("op") == operation
        and type(request.get("params")) is dict
        and type(response) is dict
        and set(response)
        == {
            "id",
            "op",
            "ok",
            "cached",
            "duration_ms",
            "result",
            "error",
        }
        and response.get("id") == request["id"]
        and response.get("op") == operation
        and response.get("ok") is True
        and response.get("cached") is False
        and _integer(response.get("duration_ms")) is not None
        and isinstance(response.get("result"), dict)
        and response.get("error") is None
    ):
        raise EvidenceError(f"{operation} control exchange is not an exact fresh success")
    return request, response["result"]


def _delivery_ok(value: object) -> bool:
    return (
        isinstance(value, dict)
        and value.get("acknowledged") is True
        and value.get("event") is not None
        and str(value.get("event")).upper() != "ERROR"
        and "payload" in value
    )


def _unique_message(
    value: object, *, text: str, direction: str
) -> dict[str, Any] | None:
    entries = value.get("entries") if isinstance(value, dict) else None
    if not isinstance(entries, list):
        return None
    matches = [
        row
        for row in entries
        if isinstance(row, dict)
        and row.get("text") == text
        and row.get("direction") == direction
    ]
    return matches[0] if len(matches) == 1 else None


def _unique_contact(
    value: object,
    *,
    public_key: str | None = None,
    fingerprint: str | None = None,
) -> dict[str, Any] | None:
    entries = value.get("entries") if isinstance(value, dict) else None
    if not isinstance(entries, list):
        return None
    matches: list[dict[str, Any]] = []
    for row in entries:
        if not isinstance(row, dict):
            continue
        if public_key is not None and _public_key(row.get("public_key")) != public_key:
            continue
        if fingerprint is not None and _fingerprint(row.get("fingerprint")) != fingerprint:
            continue
        matches.append(row)
    return matches[0] if len(matches) == 1 else None


def validate_protocol(
    data: dict[str, Any], candidate: dict[str, str]
) -> dict[str, bool | int]:
    steps = _transcript(
        data,
        candidate,
        kind=PROTOCOL_KIND,
        operations=PROTOCOL_OPERATIONS,
    )
    version = _response(steps, "version")
    identity = _response(steps, "identity")
    health_before = _response(steps, "health_before")
    mesh_status = _response(steps, "mesh_status")
    peer_advert = _response(steps, "peer_advert")
    contacts = _response(steps, "contacts")
    trace_request = _response(steps, "trace_request")
    trace_result = _response(steps, "trace_result")
    before = _response(steps, "peer_before")
    public_tx_authorization = _response(steps, "public_tx_authorization")
    public_send = _response(steps, "public_send")
    public_tx_record = _response(steps, "public_tx_record")
    after_public = _response(steps, "peer_after_public")
    peer_public_send = _response(steps, "peer_public_send")
    public_receive = _response(steps, "public_receive")
    before_dm = _response(steps, "peer_before_dm")
    dm_send = _response(steps, "dm_send")
    dm_ack = _response(steps, "dm_ack")
    after_dm = _response(steps, "peer_after_dm")
    peer_dm_send = _response(steps, "peer_dm_send")
    dm_receive_ack = _response(steps, "dm_receive_ack")
    path_request = _response(steps, "path_request")
    path_result = _response(steps, "path_result")
    login_request = _response(steps, "admin_login_request")
    login_status = _response(steps, "admin_login_status")
    query_request = _response(steps, "admin_query_request")
    query_status = _response(steps, "admin_query_status")
    logout = _response(steps, "admin_logout")
    ping_request = _response(steps, "ping_request")
    ping_result = _response(steps, "ping_result")
    health_after = _response(steps, "health_after")
    crashlog = _response(steps, "crashlog")

    identity_public_key = _public_key(identity.get("public_key"))
    identity_fingerprint = _fingerprint(identity.get("fingerprint"))
    if identity_public_key is None:
        raise EvidenceError("protocol transcript D1L identity is invalid")

    peer_before = _peer_snapshot(before)
    peer_after_public = _peer_snapshot(after_public)
    peer_before_dm = _peer_snapshot(before_dm)
    peer_after_dm = _peer_snapshot(after_dm)
    peer_public_key = _public_key(
        peer_before.get("serial", {}).get("public_key")
    )
    peer_fingerprint = (
        peer_public_key[:16].upper() if peer_public_key is not None else None
    )
    peer_run_id = peer_before.get("run_id")
    peer_snapshots = (
        peer_before,
        peer_after_public,
        peer_before_dm,
        peer_after_dm,
    )
    if not (
        peer_public_key is not None
        and all(
            snapshot.get("run_id") == peer_run_id
            and _public_key(snapshot.get("serial", {}).get("public_key"))
            == peer_public_key
            for snapshot in peer_snapshots
        )
    ):
        raise EvidenceError("protocol transcript changed controlled peer identity")

    token_match = re.fullmatch(
        rf"rc1-public-out-{candidate['firmware_commit'][:8]}-([0-9a-f]{{12}})",
        str(public_send.get("text") or ""),
    )
    if token_match is None:
        raise EvidenceError("protocol transcript Public token is not candidate-bound")
    nonce = token_match.group(1)
    public_out_token = (
        f"rc1-public-out-{candidate['firmware_commit'][:8]}-{nonce}"
    )
    public_in_token = (
        f"rc1-public-in-{candidate['firmware_commit'][:8]}-{nonce}"
    )
    dm_out_token = f"rc1-dm-out-{candidate['firmware_commit'][:8]}-{nonce}"
    dm_in_token = f"rc1-dm-in-{candidate['firmware_commit'][:8]}-{nonce}"

    advert_control, advert_result = _control_exchange(
        peer_advert, "radio.advert"
    )
    public_control, public_control_result = _control_exchange(
        peer_public_send, "radio.send_channel"
    )
    dm_control, dm_control_result = _control_exchange(
        peer_dm_send, "radio.send_dm"
    )

    peer_contact = _unique_contact(contacts, public_key=peer_public_key)
    login_command = steps["admin_login_request"]["command"]
    login_command_match = re.fullmatch(
        r"admin login ([0-9A-F]{16}) <redacted>", login_command
    )
    admin_fingerprint = (
        login_command_match.group(1) if login_command_match else None
    )
    admin_contact = _unique_contact(
        contacts, fingerprint=admin_fingerprint
    )
    admin_public_key = (
        _public_key(admin_contact.get("public_key"))
        if admin_contact is not None
        else None
    )

    public_tx_rows = (
        public_tx_record.get("entries")
        if isinstance(public_tx_record.get("entries"), list)
        else []
    )
    matching_public_tx_rows = [
        row
        for row in public_tx_rows
        if isinstance(row, dict)
        and row.get("direction") == "tx"
        and row.get("kind") in {"public_text", "channel_text"}
        and public_out_token in str(row.get("note") or "")
    ]
    public_rx_entry = _unique_message(
        public_receive, text=public_in_token, direction="rx"
    )
    dm_tx_entry = _unique_message(
        dm_ack, text=dm_out_token, direction="tx"
    )
    dm_rx_entry = _unique_message(
        dm_receive_ack, text=dm_in_token, direction="rx"
    )

    path_token = path_request.get("token")
    path_tag = (
        int(path_token[5:], 16)
        if isinstance(path_token, str)
        and re.fullmatch(r"path_[0-9A-F]{8}", path_token)
        else None
    )
    path_entries = (
        path_result.get("entries")
        if isinstance(path_result.get("entries"), list)
        else []
    )
    path_matches = [
        row
        for row in path_entries
        if isinstance(row, dict)
        and row.get("tag") == path_tag
        and _integer(row.get("sequence"), minimum=1) is not None
    ]

    trace_tag = _integer(trace_request.get("tag"), minimum=1)
    ping_tag = _integer(ping_request.get("tag"), minimum=1)
    advert_status = mesh_status.get("advert_tx")
    before_public_count = _peer_counter(peer_before, "rx_channel_total")
    after_public_count = _peer_counter(peer_after_public, "rx_channel_total")
    before_dm_count = _peer_counter(peer_before_dm, "rx_dm_total")
    after_dm_count = _peer_counter(peer_after_dm, "rx_dm_total")
    health_nonce = _integer(health_before.get("boot_nonce"), minimum=1)

    if not (
        version.get("ok") is True
        and version.get("cmd") == "version"
        and _exact_commit(version.get("build_commit"))
        == candidate["firmware_commit"]
        and version.get("release_profile") == RELEASE_PROFILE
        and version.get("sd_history_mode") == SD_HISTORY_MODE
        and identity.get("ok") is True
        and identity.get("cmd") == "identity status"
        and identity.get("node_name") == "D1L"
        and identity.get("role") == "desk_companion"
        and identity.get("public_key_ready") is True
        and identity_fingerprint == identity_public_key[:16].upper()
        and health_before.get("ok") is True
        and health_before.get("cmd") == "health"
        and _exact_commit(health_before.get("build_commit"))
        == candidate["firmware_commit"]
        and health_before.get("release_profile") == RELEASE_PROFILE
        and health_before.get("sd_history_mode") == SD_HISTORY_MODE
        and health_before.get("board_ready") is True
        and health_before.get("ui_ready") is True
        and health_nonce is not None
        and mesh_status.get("ok") is True
        and mesh_status.get("cmd") == "mesh status"
        and mesh_status.get("release_profile") == RELEASE_PROFILE
        and mesh_status.get("sd_history_mode") == SD_HISTORY_MODE
        and mesh_status.get("identity_ready") is True
        and mesh_status.get("radio_ready") is True
        and isinstance(advert_status, dict)
        and _integer(advert_status.get("queued"), minimum=1) is not None
        and _integer(advert_status.get("done"), minimum=1) is not None
        and _integer(advert_status.get("failed")) is not None
        and advert_status["done"] + advert_status["failed"]
        <= advert_status["queued"]
        and advert_status.get("boot_queued") == 1
        and advert_status.get("boot_done") == 1
        and advert_status.get("boot_failed") == 0
        and advert_status.get("last_boot") is True
        and advert_status.get("last_flood") is True
        and advert_status.get("last_node_name") == "D1L"
        and str(advert_status.get("last_public_key_prefix") or "").upper()
        == identity_public_key[:16].upper()
        and advert_status.get("boot_flood") is True
        and advert_status.get("boot_node_name") == "D1L"
        and str(advert_status.get("boot_public_key_prefix") or "").upper()
        == identity_public_key[:16].upper()
        and advert_control.get("id") == f"rc1-advert-{nonce}"
        and advert_control.get("params") == {"flood": False}
        and advert_result == {"sent": True, "flood": False}
        and contacts.get("ok") is True
        and contacts.get("cmd") == "contacts"
        and peer_contact is not None
        and _fingerprint(peer_contact.get("fingerprint"))
        == peer_fingerprint
        and peer_contact.get("canonical") is True
        and peer_contact.get("can_dm") is True
        and admin_fingerprint is not None
        and admin_contact is not None
        and admin_public_key is not None
        and admin_public_key[:16].upper() == admin_fingerprint
        and admin_contact.get("canonical") is True
        and admin_contact.get("can_admin") is True
        and admin_contact.get("type") == "repeater"
        and trace_request.get("ok") is True
        and trace_request.get("cmd") == "routes trace contact"
        and _fingerprint(trace_request.get("fingerprint"))
        == peer_fingerprint
        and trace_request.get("queued") is True
        and trace_request.get("pending") is True
        and trace_tag is not None
        and trace_request.get("targeted_trace_rf_tx") is True
        and trace_request.get("public_rf_tx") is False
        and trace_result.get("ok") is True
        and trace_result.get("cmd") == "routes trace status"
        and _fingerprint(trace_result.get("fingerprint"))
        == peer_fingerprint
        and trace_result.get("zero_hop") is False
        and trace_result.get("matched") is True
        and trace_result.get("pending", {}).get("active") is False
        and trace_result.get("last_attempt", {}).get("valid") is True
        and trace_result.get("last_attempt", {}).get("tag") == trace_tag
        and trace_result.get("last_attempt", {}).get("outcome") == "matched"
        and isinstance(trace_result.get("last_result"), dict)
        and trace_result["last_result"].get("valid") is True
        and trace_result["last_result"].get("tag") == trace_tag
        and steps["public_tx_authorization"]["command"]
        == "operator flag --authorize-public-tx"
        and public_tx_authorization
        == {
            "schema": 1,
            "ok": True,
            "authorized": True,
            "source": "cli_flag",
            "bounded_public_tx_count": 1,
        }
        and public_send.get("ok") is True
        and public_send.get("cmd") == "mesh send public"
        and public_send.get("queued") is True
        and public_send.get("text") == public_out_token
        and steps["public_send"]["command"]
        == f"mesh send public {public_out_token}"
        and public_tx_record.get("ok") is True
        and public_tx_record.get("cmd") == "packets search"
        and steps["public_tx_record"]["command"]
        == f"packets search {public_out_token}"
        and len(matching_public_tx_rows) == 1
        and before_public_count is not None
        and after_public_count == before_public_count + 1
        and str(
            peer_after_public.get("mesh", {}).get("last_rx_sender") or ""
        ).upper()
        == identity_public_key[:12].upper()
        and public_control.get("id") == f"rc1-public-{nonce}"
        and public_control.get("params")
        == {"channel": 0, "text": public_in_token}
        and public_control_result.get("channel") == 0
        and public_control_result.get("utf8_bytes")
        == len(public_in_token.encode("utf-8"))
        and _delivery_ok(public_control_result.get("delivery"))
        and public_receive.get("ok") is True
        and public_receive.get("cmd") == "messages public"
        and steps["public_receive"]["command"]
        == f"messages public search {public_in_token}"
        and public_rx_entry is not None
        and _peer_counter(peer_before_dm, "rx_channel_total")
        == after_public_count
        and before_dm_count
        == _peer_counter(peer_after_public, "rx_dm_total")
        and dm_send.get("ok") is True
        and dm_send.get("cmd") == "mesh send dm"
        and dm_send.get("queued") is True
        and _fingerprint(dm_send.get("fingerprint")) == peer_fingerprint
        and steps["dm_send"]["command"]
        == f"mesh send dm {peer_fingerprint} {dm_out_token}"
        and dm_ack.get("ok") is True
        and dm_ack.get("cmd") == "messages dm"
        and _fingerprint(dm_ack.get("fingerprint")) == peer_fingerprint
        and steps["dm_ack"]["command"]
        == f"messages dm {peer_fingerprint}"
        and dm_tx_entry is not None
        and dm_tx_entry.get("acked") is True
        and dm_tx_entry.get("delivered") is True
        and _integer(dm_tx_entry.get("ack_hash"), minimum=1) is not None
        and before_dm_count is not None
        and after_dm_count is not None
        and 1 <= after_dm_count - before_dm_count <= 2
        and dm_control.get("id") == f"rc1-dm-{nonce}"
        and dm_control.get("params")
        == {"target": identity_public_key, "text": dm_in_token}
        and str(dm_control_result.get("target") or "").lower()
        == identity_public_key[:12].lower()
        and dm_control_result.get("utf8_bytes")
        == len(dm_in_token.encode("utf-8"))
        and _delivery_ok(dm_control_result.get("delivery"))
        and dm_receive_ack.get("ok") is True
        and dm_receive_ack.get("cmd") == "messages dm"
        and _fingerprint(dm_receive_ack.get("fingerprint"))
        == peer_fingerprint
        and steps["dm_receive_ack"]["command"]
        == f"messages dm {peer_fingerprint}"
        and dm_rx_entry is not None
        and dm_rx_entry.get("ack_response", {}).get("identity_valid") is True
        and dm_rx_entry.get("ack_response", {}).get("state") == "sent"
        and _integer(
            dm_rx_entry.get("ack_response", {}).get("dispatch_count"),
            minimum=1,
        )
        is not None
        and dm_rx_entry.get("ack_response", {}).get("last_kind")
        in {"direct_ack", "flood_ack", "flood_ack_path", "path_ack"}
        and dm_rx_entry.get("ack_response", {}).get("last_error") == "ESP_OK"
        and path_request.get("ok") is True
        and path_request.get("cmd") == "routes probe"
        and _fingerprint(path_request.get("fingerprint"))
        == peer_fingerprint
        and path_request.get("queued") is True
        and path_request.get("dm_rf_tx") is True
        and path_request.get("public_rf_tx") is False
        and path_request.get("telemetry_requested") is True
        and path_tag is not None
        and steps["path_request"]["command"]
        == f"routes probe {peer_fingerprint}"
        and path_result.get("ok") is True
        and path_result.get("cmd") == "routes telemetry"
        and _fingerprint(path_result.get("fingerprint"))
        == peer_fingerprint
        and path_result.get("state") == "received"
        and path_result.get("pending") is False
        and path_result.get("pending_tag") == 0
        and _integer(path_result.get("history_count"), minimum=1)
        == len(path_entries)
        and len(path_matches) == 1
        and steps["path_result"]["command"]
        == f"routes telemetry {peer_fingerprint}"
        and login_request.get("ok") is True
        and login_request.get("cmd") == "admin login"
        and login_request.get("state") == "login_pending"
        and _fingerprint(login_request.get("fingerprint"))
        == admin_fingerprint
        and login_request.get("credential_exposed") is False
        and login_request.get("session_secret_exposed") is False
        and _integer(login_request.get("login_tx_queued"), minimum=1)
        is not None
        and login_status.get("ok") is True
        and login_status.get("cmd") == "admin status"
        and login_status.get("state") == "authenticated"
        and login_status.get("role") == "repeater"
        and _fingerprint(login_status.get("fingerprint"))
        == admin_fingerprint
        and login_status.get("credential_exposed") is False
        and login_status.get("session_secret_exposed") is False
        and query_request.get("ok") is True
        and query_request.get("cmd") == "admin telemetry"
        and query_request.get("state") == "query_pending"
        and _fingerprint(query_request.get("fingerprint"))
        == admin_fingerprint
        and query_request.get("credential_exposed") is False
        and query_request.get("session_secret_exposed") is False
        and query_status.get("ok") is True
        and query_status.get("cmd") == "admin status"
        and query_status.get("state") == "authenticated"
        and query_status.get("role") == "repeater"
        and _fingerprint(query_status.get("fingerprint"))
        == admin_fingerprint
        and query_status.get("credential_exposed") is False
        and query_status.get("session_secret_exposed") is False
        and query_status.get("query_result", {}).get("valid") is True
        and query_status.get("query_result", {}).get("kind") == "telemetry"
        and isinstance(query_status.get("query_result", {}).get("text"), str)
        and bool(query_status["query_result"]["text"])
        and _integer(query_status.get("query_accepted"), minimum=1)
        is not None
        and logout.get("ok") is True
        and logout.get("cmd") == "admin logout"
        and logout.get("state") == "idle"
        and logout.get("role") == "none"
        and logout.get("credential_exposed") is False
        and logout.get("session_secret_exposed") is False
        and ping_request.get("ok") is True
        and ping_request.get("cmd") == "repeater ping"
        and _fingerprint(ping_request.get("fingerprint"))
        == admin_fingerprint
        and ping_request.get("queued") is True
        and ping_request.get("pending") is True
        and ping_request.get("zero_hop") is True
        and ping_request.get("targeted_trace_rf_tx") is True
        and ping_request.get("public_rf_tx") is False
        and ping_tag is not None
        and ping_result.get("ok") is True
        and ping_result.get("cmd") == "repeater ping status"
        and _fingerprint(ping_result.get("fingerprint"))
        == admin_fingerprint
        and ping_result.get("zero_hop") is True
        and ping_result.get("matched") is True
        and ping_result.get("pending", {}).get("active") is False
        and ping_result.get("last_attempt", {}).get("valid") is True
        and ping_result.get("last_attempt", {}).get("tag") == ping_tag
        and ping_result.get("last_attempt", {}).get("outcome") == "matched"
        and ping_result.get("last_result", {}).get("valid") is True
        and ping_result.get("last_result", {}).get("tag") == ping_tag
        and health_after.get("ok") is True
        and health_after.get("cmd") == "health"
        and _exact_commit(health_after.get("build_commit"))
        == candidate["firmware_commit"]
        and health_after.get("release_profile") == RELEASE_PROFILE
        and health_after.get("sd_history_mode") == SD_HISTORY_MODE
        and health_after.get("board_ready") is True
        and health_after.get("ui_ready") is True
        and health_after.get("boot_nonce") == health_nonce
        and crashlog.get("ok") is True
        and crashlog.get("cmd") == "crashlog"
        and not crashlog_has_crash_like_entries(crashlog)
        and sum(
            1
            for step in steps.values()
            if step["command"].startswith("mesh send public ")
        )
        == 1
    ):
        raise EvidenceError("protocol transcript does not prove the bounded RF/admin gate")
    return {
        "boot_advert": True,
        "public_send_count": 1,
        "path": True,
        "trace": True,
        "ping": True,
        "repeater_login": True,
        "repeater_query": True,
    }


def validate_wifi(
    data: dict[str, Any], candidate: dict[str, str]
) -> dict[str, bool | int]:
    if not (
        _candidate_binding(data, candidate, require_run=False)
        and validate_wifi_report(data)
        and data.get("rc1_gate_eligible") is True
        and data.get("release_gate_eligible") is False
        and _target_pair(data)
    ):
        raise EvidenceError("Wi-Fi receipt is not an exact saved-profile reconnect pass")
    return {"wifi_reconnect": True}


def validate_sd(
    data: dict[str, Any], candidate: dict[str, str]
) -> dict[str, bool | int]:
    if not (
        data.get("mode") == "hardware"
        and data.get("dry_run") is not True
        and data.get("simulated") is not True
        and data.get("manual_only") is not True
        and _candidate_binding(data, candidate, require_run=False)
        and sd_reboot_remount_artifact_ok(
            data, POSIX_D1L_TARGET, candidate["firmware_commit"]
        )
    ):
        raise EvidenceError("SD receipt is not an exact write/remount pass")
    return {"sd_write": True, "sd_remount": True}


def validate_sd_degraded(
    data: dict[str, Any], candidate: dict[str, str]
) -> dict[str, bool | int]:
    events = data.get("events")
    cycles = data.get("cycles")
    required = _integer(data.get("cycles_required"), minimum=1)
    completed = _integer(data.get("cycles_completed"), minimum=1)
    version = data.get("version")
    ordered_events = (
        isinstance(events, list)
        and bool(events)
        and all(
            isinstance(event, dict)
            and event.get("sequence") == sequence
            and (
                (
                    event.get("kind") == "command"
                    and isinstance(event.get("command"), str)
                    and isinstance(event.get("result"), dict)
                    and event["result"].get("ok") is True
                    and event["result"].get("public_rf_tx") is not True
                    and event["result"].get("dm_rf_tx") is not True
                    and event["result"].get("formats_sd") is not True
                    and event["result"].get("format_performed") is not True
                )
                or (
                    event.get("kind") == "prompt"
                    and event.get("action") in {"remove", "reinsert"}
                )
            )
            for sequence, event in enumerate(events, 1)
        )
    )
    cycle_truth = (
        required is not None
        and completed == required
        and isinstance(cycles, list)
        and len(cycles) == required
        and all(
            isinstance(cycle, dict)
            and cycle.get("cycle") == sequence
            and cycle.get("ok") is True
            and isinstance(cycle.get("absent"), dict)
            and cycle["absent"].get("mode") == "live_only_no_card"
            and cycle["absent"].get("ok") is True
            and cycle["absent"].get("degraded_notice_visible") is True
            and isinstance(cycle.get("health"), dict)
            and cycle["health"].get("ok") is True
            and _integer(cycle["health"].get("boot_nonce"), minimum=1) is not None
            and isinstance(cycle.get("crashlog"), dict)
            and cycle["crashlog"].get("ok") is True
            and not crashlog_has_crash_like_entries(cycle["crashlog"])
            for sequence, cycle in enumerate(cycles, 1)
        )
    )
    if not (
        data.get("schema") == 1
        and data.get("kind") == SOURCE_KINDS["sd_degraded"]
        and data.get("mode") == "hardware"
        and data.get("dry_run") is not True
        and data.get("simulated") is not True
        and data.get("manual_only") is not True
        and data.get("port") == POSIX_D1L_TARGET
        and _candidate_binding(data, candidate, require_run=False)
        and data.get("strict_evidence") is True
        and data.get("ok") is True
        and data.get("public_rf_tx") is False
        and data.get("dm_rf_tx") is False
        and data.get("formats_sd") is False
        and isinstance(version, dict)
        and version.get("ok") is True
        and version.get("cmd") == "version"
        and _exact_commit(version.get("build_commit"))
        == candidate["firmware_commit"]
        and ordered_events
        and cycle_truth
    ):
        raise EvidenceError("SD degraded receipt lacks machine-observed live-only notice")
    return {"sd_degraded_notice": True}


def validate_map(
    data: dict[str, Any], candidate: dict[str, str]
) -> dict[str, bool | int]:
    steps = _transcript(
        data,
        candidate,
        kind=MAP_KIND,
        operations=MAP_OPERATIONS,
    )
    version = _response(steps, "version")
    provider = _response(steps, "provider")
    before = _response(steps, "before")
    download = _response(steps, "download")
    revisit = _response(steps, "revisit")
    health = _response(steps, "health")
    crashlog = _response(steps, "crashlog")
    before_requests = _integer(before.get("network_requests"))
    download_requests = _integer(download.get("network_requests"))
    revisit_requests = _integer(revisit.get("network_requests"))
    if not (
        version.get("ok") is True
        and _exact_commit(version.get("build_commit"))
        == candidate["firmware_commit"]
        and provider.get("ok") is True
        and provider.get("configured") is True
        and provider.get("https") is True
        and provider.get("offline_storage_permitted") is True
        and provider.get("background_prefetch_permitted") is True
        and isinstance(provider.get("attribution"), str)
        and bool(provider["attribution"].strip())
        and before.get("ok") is True
        and download.get("ok") is True
        and revisit.get("ok") is True
        and before_requests is not None
        and download_requests is not None
        and revisit_requests is not None
        and download_requests > before_requests
        and revisit_requests == download_requests
        and _integer(download.get("downloaded_tiles"), minimum=1) is not None
        and _integer(revisit.get("cache_hits"), minimum=1) is not None
        and revisit.get("offline") is True
        and revisit.get("frame_ready") is True
        and health.get("ok") is True
        and health.get("board_ready") is True
        and health.get("ui_ready") is True
        and crashlog.get("ok") is True
        and not crashlog_has_crash_like_entries(crashlog)
    ):
        raise EvidenceError("Map transcript lacks authorized download/offline cache proof")
    return {"authorized_map_download": True, "map_cache_revisit": True}


VALIDATORS: dict[
    str, Callable[[dict[str, Any], dict[str, str]], dict[str, bool | int]]
] = {
    "flash": validate_flash,
    "ui": validate_ui,
    "rf": validate_rf,
    "protocol": validate_protocol,
    "wifi": validate_wifi,
    "sd": validate_sd,
    "sd_degraded": validate_sd_degraded,
    "map": validate_map,
}


def package_candidate(package_dir: Path) -> dict[str, str]:
    package_dir = Path(package_dir)
    manifest_path = package_dir / "manifest.json"
    checksums_path = package_dir / "SHA256SUMS.txt"
    manifest = load_package_json(manifest_path)
    identity_ok, commit, run, attempt = manifest_identity(manifest)
    app_ok, app_path, app_sha = app_artifact(package_dir, manifest)
    if not (
        verify_checksum_tree(package_dir)
        and identity_ok
        and manifest.get("release_profile") == RELEASE_PROFILE
        and manifest.get("sd_history_mode") == SD_HISTORY_MODE
        and app_ok
        and all(
            isinstance(value, str) and bool(value)
            for value in (commit, run, attempt, app_path, app_sha)
        )
    ):
        raise EvidenceError("package is not one exact checksummed RC1 candidate")
    return {
        "firmware_commit": commit,
        "actions_run": run,
        "actions_run_attempt": attempt,
        "manifest_sha256": sha256_file(manifest_path),
        "checksum_manifest_sha256": sha256_file(checksums_path),
        "app_path": app_path,
        "app_sha256": app_sha,
    }


def canonical_json(value: dict[str, Any]) -> bytes:
    return (
        json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        )
        + "\n"
    ).encode("ascii")


def _safe_new_output(path: Path, label: str) -> Path:
    path = Path(path).resolve(strict=False)
    if path.exists() or os.path.lexists(path):
        raise EvidenceError(f"refusing to overwrite {label}: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    if is_link_or_reparse(path.parent):
        raise EvidenceError(f"{label} parent cannot be linked")
    return path


def _relative(path: Path, parent: Path) -> str:
    value = Path(os.path.relpath(path, parent)).as_posix()
    if value.startswith("../") or value == ".." or Path(value).is_absolute():
        raise EvidenceError("bundled evidence path escaped output directory")
    return value


def produce(
    *,
    package_dir: Path,
    sources: dict[str, Path],
    output: Path,
    evidence_root: Path,
    evidence_output: Path | None = None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    if set(sources) != set(SOURCE_ROLES):
        raise EvidenceError("all eight exact source roles are required")
    resolved = {role: Path(path).resolve(strict=True) for role, path in sources.items()}
    if len(set(resolved.values())) != len(resolved):
        raise EvidenceError("one source file cannot fill multiple evidence roles")
    evidence_root = Path(evidence_root).resolve(strict=True)
    if not evidence_root.is_dir() or is_link_or_reparse(evidence_root):
        raise EvidenceError("RF evidence root must be one exact regular directory")
    candidate = package_candidate(package_dir)
    loaded = {role: load_source(path, role) for role, path in resolved.items()}
    outcomes: dict[str, bool | int] = {}
    for role in SOURCE_ROLES:
        validator = VALIDATORS[role]
        derived = (
            validator(
                loaded[role],
                candidate,
                evidence_root=evidence_root,
            )
            if role == "rf" and validator is validate_rf
            else validator(loaded[role], candidate)
        )
        overlap = set(outcomes).intersection(derived)
        if overlap:
            raise EvidenceError(
                f"duplicate outcome coverage from {role}: {sorted(overlap)}"
            )
        outcomes.update(derived)
    if set(outcomes) != set(OUTCOME_KEYS):
        missing = sorted(set(OUTCOME_KEYS) - set(outcomes))
        extra = sorted(set(outcomes) - set(OUTCOME_KEYS))
        raise EvidenceError(
            f"outcome coverage is not exact; missing={missing}, extra={extra}"
        )

    flash = loaded["flash"]
    target_snapshot = _target(flash)
    receipt = {
        "schema": RECEIPT_SCHEMA,
        "kind": RECEIPT_KIND,
        "mode": "physical",
        "simulated": False,
        "dry_run": False,
        "candidate": candidate,
        "target": {
            "host": PI_HOST,
            "path": POSIX_D1L_TARGET,
            "vid": USB_VID,
            "pid": USB_PID,
        },
        "flash": {
            "performed": True,
            "method": "project_write_flash",
            "erase_flash": False,
            "non_erasing": True,
            "formats_sd": False,
            "settings_preserved": True,
            "artifact_app_sha256": candidate["app_sha256"],
            "written_app_sha256": candidate["app_sha256"],
        },
        "bounded_gate": {
            "bounded": True,
            "soak_required": False,
            "duration_requirement_seconds": None,
        },
        "outcomes": {key: outcomes[key] for key in OUTCOME_KEYS},
    }
    if (
        target_snapshot.get("vid") != int(USB_VID, 16)
        or target_snapshot.get("pid") != int(USB_PID, 16)
    ):
        raise EvidenceError("flash target VID:PID is not the D1L USB identity")

    output = _safe_new_output(output, "physical receipt")
    if evidence_output is None:
        evidence_output = output.with_name(f"{output.stem}.evidence.json")
    evidence_output = _safe_new_output(evidence_output, "evidence sidecar")
    if output.parent != evidence_output.parent:
        raise EvidenceError("receipt and evidence sidecar must share one directory")
    source_dir = output.parent / f"{output.stem}.sources"
    if source_dir.exists() or os.path.lexists(source_dir):
        raise EvidenceError(f"refusing to overwrite evidence source directory: {source_dir}")
    source_dir.mkdir()

    source_rows: dict[str, dict[str, str]] = {}
    source_digests: set[str] = set()
    for role in SOURCE_ROLES:
        destination = source_dir / f"{role}.json"
        before = sha256_file(resolved[role])
        if before in source_digests:
            raise EvidenceError("evidence sources must have unique SHA-256 values")
        shutil.copyfile(resolved[role], destination)
        after = sha256_file(destination)
        if before != after:
            raise EvidenceError(f"{role} source changed while bundling")
        source_digests.add(after)
        source_rows[role] = {
            "path": _relative(destination, output.parent),
            "sha256": after,
            "kind": SOURCE_KINDS[role],
        }

    receipt_bytes = canonical_json(receipt)
    output.write_bytes(receipt_bytes)
    receipt_sha = hashlib.sha256(receipt_bytes).hexdigest()
    sidecar = {
        "schema": EVIDENCE_SCHEMA,
        "kind": EVIDENCE_KIND,
        "receipt": {
            "path": _relative(output, output.parent),
            "sha256": receipt_sha,
        },
        "candidate": candidate,
        "sources": source_rows,
        "coverage": dict(COVERAGE),
    }
    evidence_output.write_bytes(canonical_json(sidecar))
    return receipt, sidecar


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Aggregate exact machine-generated RC1 physical evidence; performs "
            "no flash, RF operation, SD operation, or soak."
        )
    )
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--evidence-root", type=Path, required=True)
    for role in SOURCE_ROLES:
        parser.add_argument(
            f"--{role.replace('_', '-')}-receipt",
            dest=f"{role}_receipt",
            type=Path,
            required=True,
        )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--evidence-output", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    sources = {
        role: getattr(args, f"{role}_receipt") for role in SOURCE_ROLES
    }
    try:
        receipt, sidecar = produce(
            package_dir=args.package_dir,
            sources=sources,
            output=args.output,
            evidence_root=args.evidence_root,
            evidence_output=args.evidence_output,
        )
    except (EvidenceError, FileNotFoundError, OSError, ValueError) as exc:
        print(
            json.dumps(
                {
                    "schema": 1,
                    "kind": "d1l_rc1_bounded_physical_acceptance_producer",
                    "ok": False,
                    "error": str(exc),
                },
                sort_keys=True,
            ),
            file=sys.stderr,
        )
        return 1
    print(
        json.dumps(
            {
                "schema": 1,
                "kind": "d1l_rc1_bounded_physical_acceptance_producer",
                "ok": True,
                "firmware_commit": receipt["candidate"]["firmware_commit"],
                "receipt": str(args.output),
                "receipt_sha256": sidecar["receipt"]["sha256"],
                "evidence": str(
                    args.evidence_output
                    or args.output.with_name(f"{args.output.stem}.evidence.json")
                ),
                "soak_required": False,
                "formats_sd": False,
                "erase_flash": False,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
