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
    from release_gate_audit_d1l import full_rf_acceptance_ok
    from scroll_probe_d1l import crashlog_has_crash_like_entries
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
    from scripts.release_gate_audit_d1l import full_rf_acceptance_ok
    from scripts.scroll_probe_d1l import crashlog_has_crash_like_entries


EVIDENCE_SCHEMA = 1
EVIDENCE_KIND = "d1l_rc1_bounded_physical_acceptance_evidence"
PROTOCOL_KIND = "d1l_rc1_protocol_acceptance_transcript"
MAP_KIND = "d1l_rc1_map_acceptance_transcript"
USB_VID = "1a86"
USB_PID = "7523"
SHA_RE = re.compile(r"[0-9a-f]{64}\Z")
COMMIT_RE = re.compile(r"[0-9a-f]{40}\Z")
POSITIVE_DECIMAL_RE = re.compile(r"[1-9][0-9]*\Z")
RADIO_LISTENER_STATUS_SCHEMA = "openclaw_radio_listener_v1"
MESHCOREBOT_STATUS_SCHEMA = "meshcorebot_v1"
PEER_PROFILE_BINDINGS = {
    RADIO_LISTENER_STATUS_SCHEMA: {
        "status_schema": RADIO_LISTENER_STATUS_SCHEMA,
        "status_path": (
            "/opt/canadaverse/com15-responder/data/"
            "radio_listener.status.json"
        ),
        "control_socket": "/run/canadaverse-control/com15/control.sock",
        "device": "/dev/krab-t-echo",
        "service": "openclaw-radio-listener",
        "public_key": (
            "024999dedfd26763c5606169c3ebd34e05a9475cf78220a81078b5dd27caca44"
        ),
    },
    MESHCOREBOT_STATUS_SCHEMA: {
        "status_schema": MESHCOREBOT_STATUS_SCHEMA,
        "status_path": (
            "/opt/canadaverse/com11-meshcorebot/data/logs/"
            "meshcorebot.status.json"
        ),
        "control_socket": "/run/canadaverse-control/com11/control.sock",
        "device": "/dev/krab-com11",
        "service": "meshcorebot",
        "public_key": (
            "0bf0a701d5ae2db679c641ee999a70d4b55b61a2b77c47337ce35c16c9c19193"
        ),
    },
}
MESHCOREBOT_HARDWARE_ID = "303a:1001"
MESHCOREBOT_BAUD = 115200

SOURCE_KINDS = {
    "flash": "esp32_flash",
    "rf": "rf_full_acceptance",
    "protocol": PROTOCOL_KIND,
    "sd_degraded": "d1l_sd_remove_reinsert_source",
    "map": MAP_KIND,
}
SOURCE_ROLES = tuple(SOURCE_KINDS)
OUTCOME_KEYS = (
    "boot_advert",
    "public_send_count",
    "dm_ack",
    "path",
    "trace",
    "ping",
    "repeater_login",
    "repeater_query",
    "sd_degraded_notice",
    "authorized_map_download",
    "map_cache_revisit",
)
COVERAGE = {
    "target": "flash",
    "flash": "flash",
    "boot_advert": "protocol",
    "public_send_count": "protocol",
    "dm_ack": "rf",
    "path": "protocol",
    "trace": "protocol",
    "ping": "protocol",
    "repeater_login": "protocol",
    "repeater_query": "protocol",
    "sd_degraded_notice": "sd_degraded",
    "authorized_map_download": "map",
    "map_cache_revisit": "map",
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
PROTOCOL_TRANSCRIPT_KEYS = TRANSCRIPT_KEYS | {
    "controlled_peer",
    "protocol_targets",
}
STEP_KEYS = frozenset({"sequence", "operation", "command", "response"})
PROTOCOL_OPERATIONS = frozenset(
    {
        "version",
        "identity",
        "health_before",
        "mesh_status",
        "peer_advert",
        "contacts",
        "trace_path_request",
        "trace_path_result",
        "trace_request",
        "trace_result",
        "peer_before",
        "public_tx_authorization",
        "public_send",
        "public_tx_record",
        "peer_after_public",
        "peer_public_send",
        "public_receive",
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
MESHCOREBOT_PROTOCOL_OPERATIONS = frozenset(
    {"d1l_advert", "peer_resolve_d1l"}
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
        and _exact_commit(data.get("pre_flash_build_commit")) is not None
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
    expected_keys: frozenset[str] = TRANSCRIPT_KEYS,
) -> dict[str, dict[str, Any]]:
    if (
        set(data) != expected_keys
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


def _controlled_peer_binding(value: object) -> dict[str, str]:
    if (
        type(value) is not dict
        or set(value)
        != {
            "status_schema",
            "status_path",
            "control_socket",
            "device",
            "service",
            "public_key",
        }
        or _public_key(value.get("public_key")) != value.get("public_key")
        or not isinstance(value.get("status_path"), str)
        or not PurePosixPath(value["status_path"]).is_absolute()
        or not isinstance(value.get("control_socket"), str)
        or not PurePosixPath(value["control_socket"]).is_absolute()
    ):
        raise EvidenceError("controlled-peer binding shape is invalid")
    expected = PEER_PROFILE_BINDINGS.get(value.get("status_schema"))
    if value != expected:
        raise EvidenceError(
            "controlled-peer binding is not one exact authorized profile"
        )
    return value


def _peer_snapshot(
    value: object, binding: dict[str, str]
) -> dict[str, Any]:
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
        or value.get("path") != binding["status_path"]
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
    common_valid = bool(
        value.get("snapshot_sha256")
        == hashlib.sha256(canonical_json(snapshot)).hexdigest()
        and snapshot.get("service") == binding["service"]
        and isinstance(serial, dict)
        and isinstance(snapshot.get("counters"), dict)
        and isinstance(snapshot.get("mesh"), dict)
        and 0.0 <= freshness <= 120.0
    )
    if binding["status_schema"] == RADIO_LISTENER_STATUS_SCHEMA:
        identity_valid = bool(
            isinstance(snapshot.get("run_id"), str)
            and bool(snapshot["run_id"])
            and serial.get("mesh_connected") is True
            and serial.get("port") == binding["device"]
            and _public_key(serial.get("public_key"))
            == binding["public_key"]
        )
    else:
        started = _aware_timestamp(snapshot.get("started_at"))
        poll_at = _aware_timestamp(
            snapshot.get("mesh", {}).get("last_poll_at")
        )
        poll_freshness = (
            (captured - poll_at).total_seconds()
            if captured is not None and poll_at is not None
            else -1.0
        )
        pid = snapshot.get("pid")
        mqtt = snapshot.get("mqtt")
        discord = snapshot.get("discord")
        identity_valid = bool(
            binding["status_schema"] == MESHCOREBOT_STATUS_SCHEMA
            and isinstance(pid, int)
            and not isinstance(pid, bool)
            and pid > 0
            and started is not None
            and serial.get("active_port") == binding["device"]
            and serial.get("configured_port") == binding["device"]
            and serial.get("hardware_id") == MESHCOREBOT_HARDWARE_ID
            and serial.get("baud_rate") == MESHCOREBOT_BAUD
            and serial.get("meshcore_connected") is True
            and isinstance(discord, dict)
            and discord.get("connected") is True
            and isinstance(mqtt, dict)
            and _public_key(mqtt.get("device_public_key"))
            == binding["public_key"]
            and 0.0 <= poll_freshness <= 120.0
        )
    if not (common_valid and identity_valid):
        raise EvidenceError("controlled-peer status identity or freshness is invalid")
    return snapshot


def _peer_public_key(
    snapshot: dict[str, Any], status_schema: str
) -> str | None:
    if status_schema == RADIO_LISTENER_STATUS_SCHEMA:
        return _public_key(snapshot.get("serial", {}).get("public_key"))
    if status_schema == MESHCOREBOT_STATUS_SCHEMA:
        return _public_key(snapshot.get("mqtt", {}).get("device_public_key"))
    return None


def _peer_session_identity(
    snapshot: dict[str, Any], status_schema: str
) -> tuple[object, ...] | None:
    if status_schema == RADIO_LISTENER_STATUS_SCHEMA:
        run_id = snapshot.get("run_id")
        return ("run_id", run_id) if isinstance(run_id, str) and run_id else None
    if status_schema == MESHCOREBOT_STATUS_SCHEMA:
        pid = snapshot.get("pid")
        started_at = snapshot.get("started_at")
        if (
            isinstance(pid, int)
            and not isinstance(pid, bool)
            and pid > 0
            and _aware_timestamp(started_at) is not None
        ):
            return ("pid_started_at", pid, started_at)
    return None


def _peer_counter(
    snapshot: dict[str, Any], name: str, status_schema: str
) -> int | None:
    counters = snapshot.get("counters")
    counter_name = (
        "rx_contact_total"
        if status_schema == MESHCOREBOT_STATUS_SCHEMA
        and name == "rx_dm_total"
        else name
    )
    return (
        _integer(counters.get(counter_name))
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
    controlled_peer = _controlled_peer_binding(data.get("controlled_peer"))
    protocol_targets = data.get("protocol_targets")
    if not (
        isinstance(protocol_targets, dict)
        and set(protocol_targets)
        == {"admin_fingerprint", "trace_fingerprint"}
    ):
        raise EvidenceError("protocol transcript target binding is invalid")
    admin_fingerprint = _fingerprint(
        protocol_targets.get("admin_fingerprint")
    )
    trace_fingerprint = _fingerprint(
        protocol_targets.get("trace_fingerprint")
    )
    if admin_fingerprint is None or trace_fingerprint is None:
        raise EvidenceError("protocol transcript targets must be exact")
    peer_status_schema = controlled_peer["status_schema"]
    operations = PROTOCOL_OPERATIONS
    if peer_status_schema == MESHCOREBOT_STATUS_SCHEMA:
        operations = operations | MESHCOREBOT_PROTOCOL_OPERATIONS
    steps = _transcript(
        data,
        candidate,
        kind=PROTOCOL_KIND,
        operations=operations,
        expected_keys=PROTOCOL_TRANSCRIPT_KEYS,
    )
    version = _response(steps, "version")
    identity = _response(steps, "identity")
    health_before = _response(steps, "health_before")
    mesh_status = _response(steps, "mesh_status")
    peer_advert = _response(steps, "peer_advert")
    contacts = _response(steps, "contacts")
    trace_path_request = _response(steps, "trace_path_request")
    trace_path_result = _response(steps, "trace_path_result")
    trace_request = _response(steps, "trace_request")
    trace_result = _response(steps, "trace_result")
    before = _response(steps, "peer_before")
    d1l_advert = (
        _response(steps, "d1l_advert")
        if peer_status_schema == MESHCOREBOT_STATUS_SCHEMA
        else None
    )
    peer_resolution = (
        _response(steps, "peer_resolve_d1l")
        if peer_status_schema == MESHCOREBOT_STATUS_SCHEMA
        else None
    )
    public_tx_authorization = _response(steps, "public_tx_authorization")
    public_send = _response(steps, "public_send")
    public_tx_record = _response(steps, "public_tx_record")
    after_public = _response(steps, "peer_after_public")
    peer_public_send = _response(steps, "peer_public_send")
    public_receive = _response(steps, "public_receive")
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

    peer_before = _peer_snapshot(before, controlled_peer)
    peer_after_public = _peer_snapshot(after_public, controlled_peer)
    peer_public_key = _peer_public_key(
        peer_before, peer_status_schema
    )
    peer_fingerprint = (
        peer_public_key[:16].upper() if peer_public_key is not None else None
    )
    peer_session_identity = _peer_session_identity(
        peer_before, peer_status_schema
    )
    peer_snapshots = (
        peer_before,
        peer_after_public,
    )
    if not (
        peer_public_key is not None
        and peer_public_key == controlled_peer["public_key"]
        and peer_session_identity is not None
        and all(
            _peer_session_identity(snapshot, peer_status_schema)
            == peer_session_identity
            and _peer_public_key(snapshot, peer_status_schema)
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

    advert_control, advert_result = _control_exchange(
        peer_advert, "radio.advert"
    )
    public_control, public_control_result = _control_exchange(
        peer_public_send, "radio.send_channel"
    )
    if peer_status_schema == MESHCOREBOT_STATUS_SCHEMA:
        resolve_control, resolve_result = _control_exchange(
            peer_resolution, "radio.resolve_contact"
        )
    else:
        resolve_control, resolve_result = None, None

    peer_contact = _unique_contact(contacts, public_key=peer_public_key)
    login_command = steps["admin_login_request"]["command"]
    login_command_match = re.fullmatch(
        r"admin login ([0-9A-F]{16}) <redacted>", login_command
    )
    login_fingerprint = (
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
    trace_contact = _unique_contact(
        contacts, fingerprint=trace_fingerprint
    )
    trace_public_key = (
        _public_key(trace_contact.get("public_key"))
        if trace_contact is not None
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

    trace_path_token = trace_path_request.get("token")
    trace_path_tag = (
        int(trace_path_token[5:], 16)
        if isinstance(trace_path_token, str)
        and re.fullmatch(r"path_[0-9A-F]{8}", trace_path_token)
        else None
    )
    trace_path_entries = (
        trace_path_result.get("entries")
        if isinstance(trace_path_result.get("entries"), list)
        else []
    )
    trace_path_matches = [
        row
        for row in trace_path_entries
        if isinstance(row, dict)
        and row.get("tag") == trace_path_tag
        and _integer(row.get("sequence"), minimum=1) is not None
    ]

    trace_tag = _integer(trace_request.get("tag"), minimum=1)
    ping_tag = _integer(ping_request.get("tag"), minimum=1)
    advert_status = mesh_status.get("advert_tx")
    before_public_count = _peer_counter(
        peer_before, "rx_channel_total", peer_status_schema
    )
    after_public_count = _peer_counter(
        peer_after_public, "rx_channel_total", peer_status_schema
    )
    health_nonce = _integer(health_before.get("boot_nonce"), minimum=1)
    resolved_advert_timestamp = (
        _integer(resolve_result.get("last_advert"), minimum=1)
        if isinstance(resolve_result, dict)
        else None
    )

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
        and (
            (
                peer_status_schema == RADIO_LISTENER_STATUS_SCHEMA
                and advert_result == {"sent": True, "flood": False}
            )
            or (
                peer_status_schema == MESHCOREBOT_STATUS_SCHEMA
                and set(advert_result) == {"flood", "delivery"}
                and advert_result.get("flood") is False
                and _delivery_ok(advert_result.get("delivery"))
            )
        )
        and contacts.get("ok") is True
        and contacts.get("cmd") == "contacts"
        and peer_contact is not None
        and _fingerprint(peer_contact.get("fingerprint"))
        == peer_fingerprint
        and peer_contact.get("canonical") is True
        and peer_contact.get("can_dm") is True
        and peer_contact.get("can_admin") is False
        and peer_contact.get("type") == "chat"
        and peer_contact.get("verification_source") == "signed_advert"
        and login_fingerprint == admin_fingerprint
        and admin_contact is not None
        and admin_public_key is not None
        and admin_public_key[:16].upper() == admin_fingerprint
        and admin_contact.get("canonical") is True
        and admin_contact.get("can_dm") is False
        and admin_contact.get("can_admin") is True
        and admin_contact.get("type") == "repeater"
        and admin_contact.get("verification_source") == "signed_advert"
        and trace_contact is not None
        and trace_public_key is not None
        and trace_public_key[:16].upper() == trace_fingerprint
        and trace_contact.get("canonical") is True
        and trace_contact.get("can_dm") is False
        and trace_contact.get("can_admin") is True
        and trace_contact.get("type") in {"repeater", "room"}
        and trace_contact.get("verification_source") == "signed_advert"
        and (
            peer_status_schema == RADIO_LISTENER_STATUS_SCHEMA
            or (
                isinstance(d1l_advert, dict)
                and d1l_advert
                == {
                    "schema": 1,
                    "ok": True,
                    "cmd": "mesh advert flood",
                    "queued": True,
                    "flood": True,
                }
                and steps["d1l_advert"]["command"] == "mesh advert flood"
                and isinstance(resolve_control, dict)
                and re.fullmatch(
                    rf"rc1-resolve-{nonce}-[0-9]{{3}}",
                    str(resolve_control.get("id") or ""),
                )
                is not None
                and resolve_control.get("params") == {"name": "D1L"}
                and isinstance(resolve_result, dict)
                and set(resolve_result)
                == {
                    "name",
                    "match_count",
                    "unique",
                    "valid_signed_advert",
                    "public_key_prefix",
                    "last_advert",
                }
                and resolve_result.get("name") == "D1L"
                and resolve_result.get("match_count") == 1
                and resolve_result.get("unique") is True
                and resolve_result.get("valid_signed_advert") is True
                and str(resolve_result.get("public_key_prefix") or "").lower()
                == identity_public_key[:12].lower()
                and resolved_advert_timestamp is not None
                and steps["peer_resolve_d1l"]["command"]
                == "controlled-peer radio.resolve_contact D1L"
                and steps["peer_before"]["sequence"]
                < steps["d1l_advert"]["sequence"]
                < steps["peer_resolve_d1l"]["sequence"]
                < steps["public_tx_authorization"]["sequence"]
                < steps["public_send"]["sequence"]
            )
        )
        and trace_path_request.get("ok") is True
        and trace_path_request.get("cmd") == "routes probe"
        and _fingerprint(trace_path_request.get("fingerprint"))
        == trace_fingerprint
        and trace_path_request.get("queued") is True
        and trace_path_request.get("dm_rf_tx") is True
        and trace_path_request.get("public_rf_tx") is False
        and trace_path_request.get("telemetry_requested") is True
        and trace_path_tag is not None
        and steps["trace_path_request"]["command"]
        == f"routes probe {trace_fingerprint}"
        and trace_path_result.get("ok") is True
        and trace_path_result.get("cmd") == "routes telemetry"
        and _fingerprint(trace_path_result.get("fingerprint"))
        == trace_fingerprint
        and trace_path_result.get("state") == "received"
        and trace_path_result.get("pending") is False
        and trace_path_result.get("pending_tag") == 0
        and _integer(trace_path_result.get("history_count"), minimum=1)
        == len(trace_path_entries)
        and len(trace_path_matches) == 1
        and steps["trace_path_result"]["command"]
        == f"routes telemetry {trace_fingerprint}"
        and trace_request.get("ok") is True
        and trace_request.get("cmd") == "routes trace contact"
        and _fingerprint(trace_request.get("fingerprint"))
        == trace_fingerprint
        and trace_request.get("queued") is True
        and trace_request.get("pending") is True
        and trace_tag is not None
        and trace_request.get("targeted_trace_rf_tx") is True
        and trace_request.get("public_rf_tx") is False
        and steps["trace_request"]["command"]
        == f"routes trace contact {trace_fingerprint}"
        and trace_result.get("ok") is True
        and trace_result.get("cmd") == "routes trace status"
        and _fingerprint(trace_result.get("fingerprint"))
        == trace_fingerprint
        and trace_result.get("zero_hop") is False
        and trace_result.get("matched") is True
        and trace_result.get("pending", {}).get("active") is False
        and trace_result.get("last_attempt", {}).get("valid") is True
        and trace_result.get("last_attempt", {}).get("tag") == trace_tag
        and trace_result.get("last_attempt", {}).get("outcome") == "matched"
        and isinstance(trace_result.get("last_result"), dict)
        and trace_result["last_result"].get("valid") is True
        and trace_result["last_result"].get("tag") == trace_tag
        and steps["contacts"]["sequence"]
        < steps["trace_path_request"]["sequence"]
        < steps["trace_path_result"]["sequence"]
        < steps["trace_request"]["sequence"]
        < steps["trace_result"]["sequence"]
        < steps["peer_before"]["sequence"]
        < steps["public_tx_authorization"]["sequence"]
        < steps["public_send"]["sequence"]
        and steps["admin_logout"]["sequence"]
        < steps["path_request"]["sequence"]
        < steps["path_result"]["sequence"]
        < steps["ping_request"]["sequence"]
        < steps["ping_result"]["sequence"]
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
        and (
            peer_status_schema == RADIO_LISTENER_STATUS_SCHEMA
            or (
                peer_status_schema == MESHCOREBOT_STATUS_SCHEMA
                and peer_after_public.get("mesh", {}).get(
                    "last_rx_sender_source"
                )
                == "unique_signed_advert_name"
                and peer_after_public.get("mesh", {}).get(
                    "last_rx_sender_name"
                )
                == identity.get("node_name")
                and _integer(
                    peer_after_public.get("mesh", {}).get(
                        "last_rx_sender_advert_timestamp"
                    ),
                    minimum=1,
                )
                == resolved_advert_timestamp
            )
        )
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
        and path_request.get("ok") is True
        and path_request.get("cmd") == "routes probe"
        and _fingerprint(path_request.get("fingerprint"))
        == admin_fingerprint
        and path_request.get("queued") is True
        and path_request.get("dm_rf_tx") is True
        and path_request.get("public_rf_tx") is False
        and path_request.get("telemetry_requested") is True
        and path_tag is not None
        and steps["path_request"]["command"]
        == f"routes probe {admin_fingerprint}"
        and path_result.get("ok") is True
        and path_result.get("cmd") == "routes telemetry"
        and _fingerprint(path_result.get("fingerprint"))
        == admin_fingerprint
        and path_result.get("state") == "received"
        and path_result.get("pending") is False
        and path_result.get("pending_tag") == 0
        and _integer(path_result.get("history_count"), minimum=1)
        == len(path_entries)
        and len(path_matches) == 1
        and steps["path_result"]["command"]
        == f"routes telemetry {admin_fingerprint}"
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
    "rf": validate_rf,
    "protocol": validate_protocol,
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
        raise EvidenceError("all five exact source roles are required")
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
