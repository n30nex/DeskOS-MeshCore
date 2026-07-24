#!/usr/bin/env python3
"""Short or long hardware soak runner for MeshCore DeskOS D1L."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable

try:
    import rf_full_acceptance_d1l as rf_acceptance
    from artifact_metadata import git_metadata, stamp_report
    from core_smoke_d1l import enforce_core_port, resolve_core_target
    from smoke_d1l import (
        exact_commit,
        firmware_identity_matches,
        open_d1l_serial,
        send_console_command,
    )
    from verify_checksums import is_link_or_reparse, sha256_file
except ModuleNotFoundError:
    from scripts import rf_full_acceptance_d1l as rf_acceptance
    from scripts.artifact_metadata import git_metadata, stamp_report
    from scripts.core_smoke_d1l import enforce_core_port, resolve_core_target
    from scripts.smoke_d1l import (
        exact_commit,
        firmware_identity_matches,
        open_d1l_serial,
        send_console_command,
    )
    from scripts.verify_checksums import is_link_or_reparse, sha256_file


SOAK_COMMANDS = [
    "health",
    "mesh status",
    "signal",
    "messages unread",
    "packets",
    "crashlog",
]
STORAGE_STATUS_COMMAND = "storage status"
SD_FILE_CANARY_COMMAND = "storage filecanary"
SD_FILE_CANARY_MIN_TIMEOUT_SECONDS = 120.0
DM_FINGERPRINT_RE = re.compile(r"^[0-9A-Fa-f]{16}$")
DM_TEXT_MAX_CHARS = 138
TERMINAL_COMMAND_CODES = {"TIMEOUT", "UNEXPECTED_RESTART"}
ACTIVE_LISTENER_COUNTER_FIELDS = (
    "rx_dm_total",
    "tx_dm_total",
    "local_fast_reply_total",
    "tx_dm_ack_miss_total",
)
STORAGE_REQUIRED_SD_FIELDS = (
    "state",
    "filesystem",
    "interface",
    "rp2040_protocol_supported",
    "file_ops",
    "atomic_rename",
    "file_line_max",
    "file_chunk_max",
    "path_max",
    "status_stale",
    "presence_stale",
    "refresh_failures",
)
STORAGE_REQUIRED_TOP_FIELDS = ("data_backend", "packet_log_backend", "stores")
STORAGE_STORE_KEYS = (
    "settings",
    "identity",
    "messages",
    "dm",
    "packets",
    "routes",
    "contacts",
    "read_state",
    "crashlog",
    "map_tiles",
    "exports",
)


def utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def number_or_none(value) -> int | None:
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def command_results(samples: Iterable[dict], command: str) -> list[dict]:
    out: list[dict] = []
    for sample in samples:
        for result in sample.get("results", []):
            if result.get("cmd") == command:
                out.append(result)
    return out


def numeric_values(rows: Iterable[dict], field: str) -> list[int]:
    return [value for row in rows if (value := number_or_none(row.get(field))) is not None]


def crashlog_entries(rows: Iterable[dict]) -> list[dict]:
    entries: list[dict] = []
    for row in rows:
        row_entries = row.get("entries")
        if isinstance(row_entries, list):
            entries.extend(entry for entry in row_entries if isinstance(entry, dict))
    return entries


def soak_commands(*, sample_storage: bool = False, sd_file_canary: bool = False) -> list[str]:
    commands = list(SOAK_COMMANDS)
    if sample_storage or sd_file_canary:
        commands.append(STORAGE_STATUS_COMMAND)
    if sd_file_canary:
        commands.append(SD_FILE_CANARY_COMMAND)
    return commands


def active_dm_command(
    active_public_text: str | None,
    active_dm_fingerprint: str | None,
    active_dm_text: str | None,
) -> str | None:
    if active_public_text:
        raise ValueError(
            "automated Public TX is disabled; use --active-dm-fingerprint and "
            "--active-dm-text (or the controlled #test channel once configured)"
        )
    if active_dm_fingerprint is None and active_dm_text is None:
        return None
    if not active_dm_fingerprint or not DM_FINGERPRINT_RE.fullmatch(
        active_dm_fingerprint
    ):
        raise ValueError("active DM requires a 16-hex fingerprint")
    if not active_dm_text or not active_dm_text.strip():
        raise ValueError("active DM requires non-empty text")
    if "\n" in active_dm_text or "\r" in active_dm_text:
        raise ValueError("active DM text must be a single console line")
    if len(active_dm_text) > DM_TEXT_MAX_CHARS:
        raise ValueError(f"active DM text must be at most {DM_TEXT_MAX_CHARS} characters")
    return f"mesh send dm {active_dm_fingerprint.upper()} {active_dm_text}"


def listener_test_text_ok(value: object) -> bool:
    return (
        isinstance(value, str)
        and re.search(
            r"(?i)(?<![A-Za-z0-9])test(?![A-Za-z0-9])",
            value,
        )
        is not None
    )


def expected_active_send_count(
    duration_sec: object, active_interval_sec: object
) -> int | None:
    if (
        not isinstance(duration_sec, (int, float))
        or isinstance(duration_sec, bool)
        or duration_sec <= 0
        or not isinstance(active_interval_sec, (int, float))
        or isinstance(active_interval_sec, bool)
        or active_interval_sec <= 0
    ):
        return None
    return max(1, math.ceil(duration_sec / active_interval_sec))


def pinned_peer_evidence_metadata_ok(
    data: dict,
    config: dict,
    *,
    local_mode: bool,
) -> bool:
    before_row = data.get("controlled_peer_before_receipt")
    after_row = data.get("controlled_peer_after_receipt")
    control = data.get("controlled_peer_control")
    if not all(
        isinstance(value, dict)
        for value in (before_row, after_row, control)
    ):
        return False
    request_row = control.get("request_receipt")
    response_row = control.get("response_receipt")
    if not all(
        isinstance(value, dict)
        for value in (request_row, response_row)
    ):
        return False

    def status_row_ok(row: dict, snapshot: object) -> bool:
        common = (
            row.get("source_path") == config["status_path"]
            and row.get("source_hostname") == config["hostname"]
            and rf_acceptance.parse_aware_timestamp(
                row.get("captured_at")
            )
            is not None
        )
        if not local_mode:
            return (
                common
                and row.get("source_host") == config["ssh_host"]
                and row.get("transport") == "ssh"
                and row.get("remote_sha256") == row.get("sha256")
                and isinstance(row.get("remote_mtime_ns"), int)
                and not isinstance(row.get("remote_mtime_ns"), bool)
            )
        observed = rf_acceptance.parse_aware_timestamp(
            row.get("captured_at")
        )
        status_written_at = (
            snapshot.get("status_written_at")
            if isinstance(snapshot, dict)
            else None
        )
        mtime = (
            rf_acceptance.validate_local_status_mtime(
                row.get("source_mtime_ns"),
                status_written_at=status_written_at,
                observed_at=observed,
                max_age_sec=config["max_status_age_sec"],
            )
            if observed is not None
            else {"ok": False}
        )
        return (
            common
            and row.get("source_host") == config["hostname"]
            and row.get("transport")
            == rf_acceptance.LOCAL_PEER_STATUS_TRANSPORT
            and row.get("source_sha256") == row.get("sha256")
            and "remote_sha256" not in row
            and "remote_mtime_ns" not in row
            and mtime.get("ok") is True
        )

    def control_row_ok(row: dict, transport: str) -> bool:
        common = (
            row.get("source_path") == config["control_socket"]
            and row.get("source_host")
            == (
                config["hostname"]
                if local_mode
                else config["ssh_host"]
            )
            and row.get("source_hostname") == config["hostname"]
            and row.get("transport") == transport
            and rf_acceptance.parse_aware_timestamp(
                row.get("captured_at")
            )
            is not None
        )
        if local_mode:
            return (
                common
                and isinstance(row.get("source_peer_pid"), int)
                and not isinstance(
                    row.get("source_peer_pid"), bool
                )
                and row.get("source_peer_pid") > 0
                and row.get("source_peer_uid")
                == rf_acceptance.LOCAL_PEER_CONTROL_UID
                and row.get("source_peer_gid")
                == rf_acceptance.LOCAL_PEER_CONTROL_GID
            )
        return (
            common
            and "source_peer_pid" not in row
            and "source_peer_uid" not in row
            and "source_peer_gid" not in row
        )

    return (
        status_row_ok(
            before_row, data.get("controlled_peer_before")
        )
        and status_row_ok(
            after_row, data.get("controlled_peer_after")
        )
        and control_row_ok(
            request_row,
            (
                rf_acceptance.LOCAL_PEER_CONTROL_REQUEST_TRANSPORT
                if local_mode
                else "ssh-unix-socket-request"
            ),
        )
        and control_row_ok(
            response_row,
            (
                rf_acceptance.LOCAL_PEER_CONTROL_RESPONSE_TRANSPORT
                if local_mode
                else "ssh-unix-socket-response"
            ),
        )
    )


def qualified_controlled_peer_receipt(
    *,
    path: Path,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
    fingerprint: str,
    expected_d1l_target_sha256: str | None = None,
) -> dict:
    try:
        resolved = path.resolve(strict=True)
        resolved.relative_to(root.resolve())
    except (OSError, RuntimeError, ValueError) as exc:
        raise ValueError(
            "controlled-peer receipt must stay inside the repository"
        ) from exc
    if not resolved.is_file() or is_link_or_reparse(resolved):
        raise ValueError("controlled-peer receipt is missing or linked")
    try:
        data = json.loads(resolved.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("controlled-peer receipt is invalid JSON") from exc
    peer = data.get("controlled_peer") if isinstance(data, dict) else None
    peer_public_key = (
        rf_acceptance.exact_public_key(peer.get("public_key"))
        if isinstance(peer, dict)
        else None
    )
    d1l_public_key = (
        rf_acceptance.exact_public_key(data.get("d1l_public_key"))
        if isinstance(data, dict)
        else None
    )
    evidence_source = (
        peer.get("evidence_source")
        if isinstance(peer, dict)
        else None
    )
    serial_peer_ok = False
    pinned_local_peer_ok = False
    remote_peer_ok = False
    if evidence_source == "explicit_peer_status":
        try:
            peer_status_path = Path(
                str(peer.get("status_path"))
            ).resolve()
        except (OSError, RuntimeError, TypeError, ValueError):
            peer_status_path = None
        serial_peer_ok = (
            data.get("controlled_peer_adapter")
            == rf_acceptance.RADIO_LISTENER_PROFILE
            and peer.get("port") == rf_acceptance.RADIO_LISTENER_PORT
            and peer_status_path
            == rf_acceptance.RADIO_LISTENER_STATUS_PATH.resolve()
        )
    elif (
        evidence_source
        == rf_acceptance.REMOTE_PEER_EVIDENCE_SOURCE
    ):
        remote_peer_ok = (
            data.get("controlled_peer_adapter")
            == rf_acceptance.REMOTE_PEER_ADAPTER
            and peer.get("port") is None
            and peer.get("ssh_host")
            == rf_acceptance.REMOTE_PEER_SSH_HOST
            and peer.get("hostname")
            == rf_acceptance.REMOTE_PEER_HOSTNAME
            and peer.get("status_path")
            == rf_acceptance.REMOTE_PEER_STATUS_PATH
            and peer.get("control_socket")
            == rf_acceptance.REMOTE_PEER_CONTROL_SOCKET
            and peer.get("device")
            == rf_acceptance.REMOTE_PEER_DEVICE
            and peer.get("public_key")
            == rf_acceptance.REMOTE_PEER_PUBLIC_KEY
            and pinned_peer_evidence_metadata_ok(
                data,
                rf_acceptance.validate_remote_peer_config(
                    {
                        "ssh_host": peer.get("ssh_host"),
                        "hostname": peer.get("hostname"),
                        "status_path": peer.get("status_path"),
                        "control_socket": peer.get("control_socket"),
                        "device": peer.get("device"),
                        "public_key": peer.get("public_key"),
                        "max_status_age_sec": peer.get(
                            "max_status_age_sec"
                        ),
                    }
                ),
                local_mode=False,
            )
            and rf_acceptance.remote_peer_report_shape_ok(data)
        )
    elif (
        evidence_source
        == rf_acceptance.LOCAL_PEER_EVIDENCE_SOURCE
    ):
        try:
            local_config = rf_acceptance.validate_local_peer_config(
                {
                    "hostname": peer.get("hostname"),
                    "status_path": peer.get("status_path"),
                    "control_socket": peer.get("control_socket"),
                    "device": peer.get("device"),
                    "public_key": peer.get("public_key"),
                    "max_status_age_sec": peer.get(
                        "max_status_age_sec"
                    ),
                }
            )
        except (TypeError, ValueError):
            local_config = None
        pinned_local_peer_ok = (
            local_config is not None
            and data.get("controlled_peer_adapter")
            == rf_acceptance.LOCAL_PEER_ADAPTER
            and peer.get("port") is None
            and peer.get("access_mode") == "local"
            and "ssh_host" not in peer
            and pinned_peer_evidence_metadata_ok(
                data, local_config, local_mode=True
            )
            and rf_acceptance.remote_peer_report_shape_ok(data)
        )
    if not (
        isinstance(data, dict)
        and data.get("mode") == "rf-full-acceptance"
        and data.get("ok") is True
        and data.get("closure_eligible") is True
        and exact_commit(data.get("expected_firmware_commit")) == commit
        and str(data.get("github_actions_run")) == str(run_id)
        and str(data.get("workflow_run_attempt")) == str(run_attempt)
        and data.get("target_fingerprint") == fingerprint
        and isinstance(peer, dict)
        and peer.get("fingerprint") == fingerprint
        and (
            serial_peer_ok
            or pinned_local_peer_ok
            or remote_peer_ok
        )
        and peer_public_key is not None
        and rf_acceptance.public_key_fingerprint(peer_public_key)
        == fingerprint
        and d1l_public_key is not None
        and data.get("public_rf_tx") is False
    ):
        raise ValueError(
            "active soak controlled-peer receipt does not match the exact "
            "qualified candidate/run/peer"
        )
    if expected_d1l_target_sha256 is not None:
        before_target = data.get("d1l_target")
        after_target = data.get("d1l_target_after")
        if not (
            data.get("schema") == 2
            and isinstance(before_target, dict)
            and isinstance(after_target, dict)
            and before_target.get("stable_identity_sha256")
            == expected_d1l_target_sha256
            and after_target.get("stable_identity_sha256")
            == expected_d1l_target_sha256
            and data.get("target_identity_continuity_ok") is True
            and data.get("port") == before_target.get("requested_path")
            and after_target.get("requested_path")
            == before_target.get("requested_path")
        ):
            raise ValueError(
                "controlled-peer RF receipt is not bound to the exact "
                "D1L serial target"
            )
    return data, {
        "path": resolved.relative_to(root.resolve()).as_posix(),
        "size": resolved.stat().st_size,
        "sha256": sha256_file(resolved),
    }


def controlled_peer_receipt_row(
    *,
    path: Path,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
    fingerprint: str,
) -> dict:
    _, row = qualified_controlled_peer_receipt(
        path=path,
        root=root,
        commit=commit,
        run_id=run_id,
        run_attempt=run_attempt,
        fingerprint=fingerprint,
    )
    return row


def active_listener_flow_ok(
    before: dict | None,
    after: dict | None,
    *,
    successful_send_count: int,
    d1l_public_key: object,
    peer_fingerprint: str,
    peer_public_key: object,
    minimum_send_count: int = 1,
    remote_config: dict | None = None,
    local_config: dict | None = None,
) -> tuple[bool, dict[str, int | None]]:
    deltas = {
        field: rf_acceptance.counter_delta(before, after, field)
        for field in ACTIVE_LISTENER_COUNTER_FIELDS
    }
    expected_peer_key = rf_acceptance.exact_public_key(peer_public_key)
    before_peer_key = rf_acceptance.exact_public_key(
        rf_acceptance.get_path(before, "serial", "public_key")
    )
    after_peer_key = rf_acceptance.exact_public_key(
        rf_acceptance.get_path(after, "serial", "public_key")
    )
    count_ok = (
        isinstance(successful_send_count, int)
        and not isinstance(successful_send_count, bool)
        and isinstance(minimum_send_count, int)
        and not isinstance(minimum_send_count, bool)
        and minimum_send_count >= 1
        and successful_send_count >= minimum_send_count
    )
    identity_ok = (
        expected_peer_key is not None
        and before_peer_key == expected_peer_key
        and after_peer_key == expected_peer_key
        and rf_acceptance.public_key_fingerprint(expected_peer_key)
        == peer_fingerprint
    )
    if remote_config is not None and local_config is not None:
        normalized_pinned = None
    elif local_config is not None:
        try:
            normalized_pinned = (
                rf_acceptance.validate_local_peer_config(
                    local_config
                )
            )
        except ValueError:
            normalized_pinned = None
    elif remote_config is not None:
        try:
            normalized_pinned = (
                rf_acceptance.validate_remote_peer_config(
                    remote_config
                )
            )
        except ValueError:
            normalized_pinned = None
    else:
        normalized_pinned = None
    if remote_config is not None or local_config is not None:
        continuity_binding_ok = (
            normalized_pinned is not None
            and rf_acceptance.get_path(before, "serial", "port")
            == normalized_pinned["device"]
            and rf_acceptance.get_path(after, "serial", "port")
            == normalized_pinned["device"]
            and rf_acceptance.get_path(
                before, "serial", "mesh_connected"
            )
            is True
            and rf_acceptance.get_path(
                after, "serial", "mesh_connected"
            )
            is True
            and expected_peer_key == normalized_pinned["public_key"]
        )
    else:
        continuity_binding_ok = (
            rf_acceptance.radio_listener_connected(
                before,
                rf_acceptance.RADIO_LISTENER_PORT,
                peer_fingerprint,
            )
            and rf_acceptance.radio_listener_connected(
                after,
                rf_acceptance.RADIO_LISTENER_PORT,
                peer_fingerprint,
            )
        )
    continuity_ok = (
        isinstance(before, dict)
        and isinstance(after, dict)
        and before.get("run_id") == after.get("run_id")
        and isinstance(before.get("run_id"), str)
        and bool(before.get("run_id"))
        and before.get("service") == after.get("service")
        and before.get("service") == "openclaw-radio-listener"
        and continuity_binding_ok
    )
    counters_ok = (
        count_ok
        and deltas["rx_dm_total"] == successful_send_count
        and deltas["tx_dm_total"] == successful_send_count
        and deltas["local_fast_reply_total"] == successful_send_count
        and deltas["tx_dm_ack_miss_total"] == 0
    )
    activity_ok = (
        rf_acceptance.listener_sender_matches(after, d1l_public_key)
        and rf_acceptance.get_path(after, "mesh", "last_rx_kind") == "dm"
        and rf_acceptance.get_path(after, "mesh", "last_tx_kind") == "dm"
        and rf_acceptance.get_path(before, "mesh", "last_rx_at")
        != rf_acceptance.get_path(after, "mesh", "last_rx_at")
        and rf_acceptance.get_path(before, "mesh", "last_tx_at")
        != rf_acceptance.get_path(after, "mesh", "last_tx_at")
    )
    return (
        count_ok and identity_ok and continuity_ok and counters_ok and activity_ok,
        deltas,
    )


def unexpected_console_restart(result: dict) -> bool:
    return result.get("ignored_boot_help_seen") is True or any(
        ignored.get("cmd") == "help"
        for ignored in (result.get("ignored_json") or [])
        if isinstance(ignored, dict)
    )


def sd_status(row: dict) -> dict:
    value = row.get("sd")
    return value if isinstance(value, dict) else {}


def stable_values(values: list[object]) -> bool:
    return len({json.dumps(value, sort_keys=True) for value in values}) <= 1


def storage_file_capability_ready(row: dict) -> bool:
    sd = sd_status(row)
    return (
        sd.get("file_ops") is True
        and sd.get("atomic_rename") is True
        and number_or_none(sd.get("file_line_max")) is not None
        and number_or_none(sd.get("file_line_max")) >= 512
        and number_or_none(sd.get("file_chunk_max")) is not None
        and number_or_none(sd.get("file_chunk_max")) >= 192
        and number_or_none(sd.get("path_max")) is not None
        and number_or_none(sd.get("path_max")) >= 96
    )


def file_ops_ready(row: dict) -> bool:
    sd = sd_status(row)
    return (
        row.get("data_enabled") is True
        and row.get("data_backend") == "mixed"
        and row.get("packet_log_backend") == "sd"
        and sd.get("state") == "ready"
        and sd.get("filesystem") == "fat32"
        and sd.get("present") is True
        and sd.get("mounted") is True
        and sd.get("data_root_ready") is True
        and sd.get("rp2040_protocol_supported") is True
        and sd.get("status_stale") is False
        and sd.get("presence_stale") is False
        and number_or_none(sd.get("refresh_failures")) == 0
        and storage_file_capability_ready(row)
    )


def storage_status_malformed(row: dict) -> bool:
    sd = sd_status(row)
    if not sd:
        return True
    if any(field not in sd for field in STORAGE_REQUIRED_SD_FIELDS):
        return True
    if any(field not in row for field in STORAGE_REQUIRED_TOP_FIELDS):
        return True
    return not isinstance(row.get("stores"), dict)


def storage_store_backend_values(rows: list[dict]) -> dict[str, list[str]]:
    values: dict[str, list[str]] = {}
    for key in STORAGE_STORE_KEYS:
        row_values = []
        for row in rows:
            stores = row.get("stores")
            if isinstance(stores, dict) and isinstance(stores.get(key), str):
                row_values.append(stores[key])
        if row_values:
            values[key] = row_values
    return values


def allowed_sd_unavailable(result: dict, allow_sd_unavailable: bool) -> bool:
    return (
        allow_sd_unavailable
        and result.get("cmd") == SD_FILE_CANARY_COMMAND
        and result.get("ok") is False
        and result.get("code") == "ESP_ERR_NOT_SUPPORTED"
        and result.get("step") == "preflight"
    )


def first_last_delta(rows: list[dict], field: str) -> int | None:
    values = numeric_values(rows, field)
    if len(values) < 2:
        return None
    return values[-1] - values[0]


def monotonic_non_decreasing(values: list[int]) -> bool:
    return all(curr >= prev for prev, curr in zip(values, values[1:]))


def send_soak_command(
    ser,
    command: str,
    timeout: float,
    retries: int,
    retry_delay_sec: float,
    terminal_failure_ok: Callable[[dict], bool] | None = None,
) -> dict:
    attempts = []
    command_timeout = (
        max(timeout, SD_FILE_CANARY_MIN_TIMEOUT_SECONDS)
        if command == SD_FILE_CANARY_COMMAND
        else timeout
    )
    for attempt in range(max(0, retries) + 1):
        result = send_console_command(ser, command, command_timeout)
        if unexpected_console_restart(result):
            result = {
                **result,
                "ok": False,
                "code": "UNEXPECTED_RESTART",
                "unexpected_console_restart": True,
            }
        attempts.append(result)
        terminal_ok = bool(terminal_failure_ok and terminal_failure_ok(result))
        if result.get("ok") or terminal_ok:
            if terminal_ok:
                result = dict(result)
                result["allowed_failure"] = True
            if attempt:
                result = dict(result)
                result["attempts"] = attempt + 1
                result["recovered_after_retry"] = True
                result["retry_failures"] = attempts[:-1]
            return result
        if result.get("code") in TERMINAL_COMMAND_CODES:
            # Firmware may still be executing this command. Retrying or
            # queueing another command would shift JSON replies and turn a
            # single slow operation into invalid soak evidence.
            break
        if attempt < retries:
            time.sleep(retry_delay_sec)

    result = dict(attempts[-1])
    result["attempts"] = len(attempts)
    result["recovered_after_retry"] = False
    result["retry_failures"] = attempts[:-1]
    return result


def collect_sample(
    ser,
    timeout: float,
    label: str,
    elapsed_sec: float,
    command_retries: int,
    retry_delay_sec: float,
    commands: list[str] | None = None,
    allow_sd_unavailable: bool = False,
) -> dict:
    commands = commands or SOAK_COMMANDS
    results: list[dict] = []
    aborted_after_timeout: str | None = None
    for command in commands:
        if aborted_after_timeout is not None:
            break
        result = send_soak_command(
            ser,
            command,
            timeout,
            command_retries,
            retry_delay_sec,
            terminal_failure_ok=(
                lambda result: allowed_sd_unavailable(result, True)
                if allow_sd_unavailable
                else False
            ),
        )
        results.append(result)
        if result.get("code") in TERMINAL_COMMAND_CODES:
            aborted_after_timeout = command
    return {
        "label": label,
        "elapsed_sec": round(elapsed_sec, 3),
        "aborted_after_timeout": aborted_after_timeout,
        "results": results,
    }


def summarize_soak(
    *,
    samples: list[dict],
    active_events: list[dict],
    require_rx_delta: bool,
    min_rx_delta: int,
    min_tx_delta: int,
    sample_storage: bool = False,
    sd_file_canary: bool = False,
    allow_sd_unavailable: bool = False,
) -> dict:
    health = command_results(samples, "health")
    mesh = command_results(samples, "mesh status")
    signal = command_results(samples, "signal")
    packet_rows = command_results(samples, "packets")
    crashlog = command_results(samples, "crashlog")
    storage = command_results(samples, STORAGE_STATUS_COMMAND)
    sd_file_canaries = command_results(samples, SD_FILE_CANARY_COMMAND)

    failures = []
    for sample in samples:
        for result in sample.get("results", []):
            if not result.get("ok") and not allowed_sd_unavailable(result, allow_sd_unavailable):
                failures.append(
                    {
                        "sample": sample.get("label"),
                        "elapsed_sec": sample.get("elapsed_sec"),
                        "cmd": result.get("cmd"),
                        "code": result.get("code"),
                    }
                )
    for event in active_events:
        result = event.get("result", {})
        if not result.get("ok"):
            failures.append(
                {
                    "sample": "active_dm_tx",
                    "elapsed_sec": event.get("elapsed_sec"),
                    "cmd": result.get("cmd"),
                    "code": result.get("code"),
                }
            )

    observed_results = [
        result
        for sample in samples
        for result in sample.get("results", [])
    ] + [event.get("result", {}) for event in active_events]
    host_timeout_seen = any(
        result.get("code") == "TIMEOUT"
        or any(
            retry.get("code") == "TIMEOUT"
            for retry in (result.get("retry_failures") or [])
            if isinstance(retry, dict)
        )
        for result in observed_results
    )
    unexpected_restart_seen = any(
        result.get("code") == "UNEXPECTED_RESTART"
        or unexpected_console_restart(result)
        or any(
            retry.get("code") == "UNEXPECTED_RESTART"
            or unexpected_console_restart(retry)
            for retry in (result.get("retry_failures") or [])
            if isinstance(retry, dict)
        )
        for result in observed_results
    )

    uptime_values = numeric_values(health, "uptime_ms")
    rx_delta = first_last_delta(mesh, "rx_packets")
    tx_delta = first_last_delta(mesh, "tx_packets")
    packet_written_delta = first_last_delta(packet_rows, "total_written")
    heap_free_delta = first_last_delta(health, "heap_free")
    psram_free_delta = first_last_delta(health, "psram_free")

    current_stack = numeric_values(health, "current_task_stack_free_words")
    ui_stack = numeric_values(health, "ui_task_stack_free_words")
    retained_stack = numeric_values(health, "retained_task_stack_free_bytes")
    lvgl_used = numeric_values(health, "lvgl_used_pct")
    heap_min = numeric_values(health, "heap_min_free")
    psram_min = numeric_values(health, "psram_min_free")
    signal_samples = numeric_values(signal, "sample_count")
    crashlog_total_delta = first_last_delta(crashlog, "total_written")
    crash_entries = crashlog_entries(crashlog)
    crash_like_count = sum(1 for entry in crash_entries if entry.get("crash_like") is True)
    crashlog_count_values = numeric_values(crashlog, "count")
    storage_states = [sd_status(row).get("state") for row in storage if sd_status(row).get("state")]
    storage_data_backends = [row.get("data_backend") for row in storage if row.get("data_backend")]
    storage_packet_log_backends = [
        row.get("packet_log_backend") for row in storage if row.get("packet_log_backend")
    ]
    storage_protocol_flags = [
        sd_status(row).get("rp2040_protocol_supported")
        for row in storage
        if "rp2040_protocol_supported" in sd_status(row)
    ]
    storage_stale_flags = [
        sd_status(row).get("status_stale")
        for row in storage
        if "status_stale" in sd_status(row)
    ]
    storage_presence_stale_flags = [
        sd_status(row).get("presence_stale")
        for row in storage
        if "presence_stale" in sd_status(row)
    ]
    storage_refresh_failures = [
        number_or_none(sd_status(row).get("refresh_failures"))
        for row in storage
        if "refresh_failures" in sd_status(row)
    ]
    storage_malformed_count = sum(1 for row in storage if storage_status_malformed(row))
    storage_file_capability_ready_values = [storage_file_capability_ready(row) for row in storage]
    storage_file_ops_ready = [file_ops_ready(row) for row in storage]
    storage_store_backend_samples = storage_store_backend_values(storage)
    storage_store_backends = {
        key: sorted(set(values)) for key, values in storage_store_backend_samples.items()
    }
    storage_store_backend_stable = {
        key: stable_values(values) for key, values in storage_store_backend_samples.items()
    }
    sd_file_canary_pass_count = sum(1 for row in sd_file_canaries if row.get("ok") is True)
    sd_file_canary_unavailable_count = sum(
        1 for row in sd_file_canaries if allowed_sd_unavailable(row, True)
    )
    retry_rows = [
        result
        for sample in samples
        for result in sample.get("results", [])
        if number_or_none(result.get("attempts")) is not None and result.get("attempts") > 1
    ]
    retry_rows.extend(
        event.get("result", {})
        for event in active_events
        if number_or_none(event.get("result", {}).get("attempts")) is not None
        and event.get("result", {}).get("attempts") > 1
    )

    board_ready_all = bool(health) and all(row.get("board_ready") is True for row in health)
    ui_ready_all = bool(health) and all(row.get("ui_ready") is True for row in health)
    mesh_ready_all = bool(mesh) and all(
        row.get("state") == "ready"
        and row.get("identity_ready") is True
        and row.get("radio_ready") is True
        for row in mesh
    )
    uptime_monotonic = monotonic_non_decreasing(uptime_values)
    active_tx_ok = all(event.get("result", {}).get("ok") for event in active_events)

    threshold_failures: list[str] = []
    if not board_ready_all:
        threshold_failures.append("board_not_ready")
    if not ui_ready_all:
        threshold_failures.append("ui_not_ready")
    if not mesh_ready_all:
        threshold_failures.append("mesh_not_ready")
    if not uptime_monotonic:
        threshold_failures.append("uptime_reset_or_reboot_seen")
    if host_timeout_seen:
        threshold_failures.append("command_timeout_seen")
    if unexpected_restart_seen:
        threshold_failures.append("unexpected_console_restart_seen")
    if current_stack and min(current_stack) <= 0:
        threshold_failures.append("current_task_stack_watermark_zero")
    if ui_stack and min(ui_stack) <= 0:
        threshold_failures.append("ui_task_stack_watermark_zero")
    if not health or len(retained_stack) != len(health):
        threshold_failures.append("retained_task_stack_watermark_missing")
    elif min(retained_stack) < 4096:
        threshold_failures.append("retained_task_stack_margin_below_4096_bytes")
    if active_events and not active_tx_ok:
        threshold_failures.append("active_dm_tx_failed")
    if crash_like_count > 0:
        threshold_failures.append("crash_like_reset_seen")
    if min_tx_delta > 0 and (tx_delta is None or tx_delta < min_tx_delta):
        threshold_failures.append("tx_delta_below_minimum")
    if require_rx_delta and (rx_delta is None or rx_delta < min_rx_delta):
        threshold_failures.append("rx_delta_below_minimum")
    if sample_storage and not storage:
        threshold_failures.append("storage_status_missing")
    if sample_storage and storage_malformed_count:
        threshold_failures.append("storage_status_malformed")
    if storage and not stable_values(storage_states):
        threshold_failures.append("storage_state_changed")
    if any(value is not False for value in storage_stale_flags):
        threshold_failures.append("storage_status_stale")
    if any(value is not False for value in storage_presence_stale_flags):
        threshold_failures.append("storage_presence_stale")
    if any(value is None or value > 0 for value in storage_refresh_failures):
        threshold_failures.append("storage_refresh_failures")
    if storage_data_backends and not stable_values(storage_data_backends):
        threshold_failures.append("storage_data_backend_changed")
    if storage_packet_log_backends and not stable_values(storage_packet_log_backends):
        threshold_failures.append("storage_packet_log_backend_changed")
    if storage_store_backend_stable and not all(storage_store_backend_stable.values()):
        threshold_failures.append("storage_store_backend_changed")
    if sd_file_canary and not sd_file_canaries:
        threshold_failures.append("sd_file_canary_missing")
    if sd_file_canary and not allow_sd_unavailable and sd_file_canary_pass_count != len(sd_file_canaries):
        threshold_failures.append("sd_file_canary_failed")

    ok = not failures and not threshold_failures

    return {
        "ok": ok,
        "sample_count": len(samples),
        "active_public_tx_count": len(active_events),
        "command_failure_count": len(failures),
        "command_failures": failures,
        "threshold_failures": threshold_failures,
        "command_timeout_seen": host_timeout_seen,
        "unexpected_console_restart_seen": unexpected_restart_seen,
        "board_ready_all": board_ready_all,
        "ui_ready_all": ui_ready_all,
        "mesh_ready_all": mesh_ready_all,
        "uptime_monotonic": uptime_monotonic,
        "mesh_rx_packet_delta": rx_delta,
        "mesh_tx_packet_delta": tx_delta,
        "packet_total_written_delta": packet_written_delta,
        "heap_free_delta": heap_free_delta,
        "psram_free_delta": psram_free_delta,
        "heap_min_free_floor": min(heap_min) if heap_min else None,
        "psram_min_free_floor": min(psram_min) if psram_min else None,
        "current_task_stack_free_words_floor": min(current_stack) if current_stack else None,
        "ui_task_stack_free_words_floor": min(ui_stack) if ui_stack else None,
        "retained_task_stack_free_bytes_floor": (
            min(retained_stack) if retained_stack else None
        ),
        "lvgl_used_pct_peak": max(lvgl_used) if lvgl_used else None,
        "signal_sample_count_peak": max(signal_samples) if signal_samples else None,
        "crashlog_total_written_delta": crashlog_total_delta,
        "crashlog_count_peak": max(crashlog_count_values) if crashlog_count_values else None,
        "crashlog_crash_like_count": crash_like_count,
        "storage_status_count": len(storage),
        "storage_states": sorted(set(storage_states)),
        "storage_data_backends": sorted(set(storage_data_backends)),
        "storage_packet_log_backends": sorted(set(storage_packet_log_backends)),
        "storage_state_stable": stable_values(storage_states) if storage_states else None,
        "storage_data_backend_stable": stable_values(storage_data_backends) if storage_data_backends else None,
        "storage_packet_log_backend_stable": stable_values(storage_packet_log_backends)
        if storage_packet_log_backends else None,
        "storage_status_malformed_count": storage_malformed_count,
        "storage_status_stale_count": sum(
            1 for value in storage_stale_flags if value is not False
        ),
        "storage_presence_stale_count": sum(
            1 for value in storage_presence_stale_flags if value is not False
        ),
        "storage_refresh_failures_max": (
            max(value for value in storage_refresh_failures if value is not None)
            if any(value is not None for value in storage_refresh_failures)
            else None
        ),
        "storage_store_backends": storage_store_backends,
        "storage_store_backend_stable": storage_store_backend_stable,
        "storage_store_backend_stable_all": bool(storage_store_backend_stable)
        and all(storage_store_backend_stable.values()),
        "storage_protocol_supported_any": any(flag is True for flag in storage_protocol_flags),
        "storage_protocol_supported_all": bool(storage_protocol_flags)
        and all(flag is True for flag in storage_protocol_flags),
        "storage_file_capability_ready_all": bool(storage_file_capability_ready_values)
        and all(storage_file_capability_ready_values),
        "storage_file_ops_ready_all": bool(storage_file_ops_ready)
        and all(storage_file_ops_ready),
        "sd_file_canary_count": len(sd_file_canaries),
        "sd_file_canary_pass_count": sd_file_canary_pass_count,
        "sd_file_canary_unavailable_count": sd_file_canary_unavailable_count,
        "sd_file_canary_required": sd_file_canary,
        "sd_unavailable_allowed": allow_sd_unavailable,
        "command_retry_count": len(retry_rows),
        "command_recovered_after_retry_count": sum(
            1 for row in retry_rows if row.get("recovered_after_retry") is True
        ),
        "command_retry_failure_count": sum(
            1 for row in retry_rows if row.get("recovered_after_retry") is False
        ),
    }


def dry_run_report(
    *,
    duration_sec: float,
    sample_interval_sec: float,
    active_public_text: str | None,
    active_interval_sec: float,
    require_rx_delta: bool,
    min_rx_delta: int,
    min_tx_delta: int,
    clear_crashlog_before_start: bool,
    command_retries: int,
    retry_delay_sec: float,
    sample_storage: bool = False,
    sd_file_canary: bool = False,
    allow_sd_unavailable: bool = False,
    active_dm_fingerprint: str | None = None,
    active_dm_text: str | None = None,
    expected_firmware_commit: str | None = None,
    github_run_id: str | None = None,
    workflow_run_attempt: str | None = None,
) -> dict:
    normalized_commit = (
        exact_commit(expected_firmware_commit)
        if expected_firmware_commit is not None
        else None
    )
    if expected_firmware_commit is not None and normalized_commit is None:
        raise ValueError(
            "expected_firmware_commit must be an exact 40-character hexadecimal SHA"
        )
    active_command = active_dm_command(
        active_public_text, active_dm_fingerprint, active_dm_text
    )
    return {
        "schema": 1,
        "mode": "dry-run",
        "hardware_required": False,
        "ok": True,
        "preflight_commands": ["version"] if normalized_commit is not None else [],
        "expected_firmware_commit": normalized_commit,
        "github_actions_run": (
            str(github_run_id) if github_run_id is not None else None
        ),
        "workflow_run_attempt": (
            str(workflow_run_attempt)
            if workflow_run_attempt is not None
            else None
        ),
        "device_build_commit": None,
        "firmware_identity_required": normalized_commit is not None,
        "firmware_identity_ok": None,
        "duration_sec": duration_sec,
        "sample_interval_sec": sample_interval_sec,
        "commands": soak_commands(sample_storage=sample_storage, sd_file_canary=sd_file_canary),
        "active_public_text": None,
        "active_dm_fingerprint": (
            active_dm_fingerprint.upper() if active_dm_fingerprint else None
        ),
        "active_dm_text": active_dm_text,
        "active_command": active_command,
        "dm_rf_tx": active_command is not None,
        "public_rf_tx": False,
        "formats_sd": False,
        "sample_storage": sample_storage or sd_file_canary,
        "sd_file_canary": sd_file_canary,
        "allow_sd_unavailable": allow_sd_unavailable,
        "active_interval_sec": active_interval_sec,
        "require_rx_delta": require_rx_delta,
        "min_rx_delta": min_rx_delta,
        "min_tx_delta": min_tx_delta,
        "clear_crashlog_before_start": clear_crashlog_before_start,
        "command_retries": command_retries,
        "retry_delay_sec": retry_delay_sec,
    }


def _run_serial_soak_reserved(
    *,
    port: str,
    baud: int,
    timeout: float,
    duration_sec: float,
    sample_interval_sec: float,
    active_public_text: str | None,
    active_interval_sec: float,
    startup_settle_sec: float,
    require_rx_delta: bool,
    min_rx_delta: int,
    min_tx_delta: int,
    clear_crashlog_before_start: bool,
    command_retries: int,
    retry_delay_sec: float,
    sample_storage: bool = False,
    sd_file_canary: bool = False,
    allow_sd_unavailable: bool = False,
    active_dm_fingerprint: str | None = None,
    active_dm_text: str | None = None,
    expected_firmware_commit: str | None = None,
    github_run_id: str | None = None,
    workflow_run_attempt: str | None = None,
    expected_release_profile: str | None = None,
    expected_sd_history_mode: str | None = None,
    controlled_peer_receipt: Path | None = None,
    peer_capture_dir: Path | None = None,
    port_lister: Callable[[], Iterable[object]] | None = None,
    platform_name: str | None = None,
    evidence_bundle: rf_acceptance.EvidenceBundle,
) -> dict:
    root = Path(__file__).resolve().parents[1]
    normalized_commit = (
        exact_commit(expected_firmware_commit)
        if expected_firmware_commit is not None
        else None
    )
    if expected_firmware_commit is not None and normalized_commit is None:
        raise ValueError(
            "expected_firmware_commit must be an exact 40-character hexadecimal SHA"
        )
    release_bound = (
        str(github_run_id or "").isdigit()
        and int(str(github_run_id or "0")) >= 1
        and str(workflow_run_attempt or "").isdigit()
        and int(str(workflow_run_attempt or "0")) >= 1
    )
    if (github_run_id is None) != (workflow_run_attempt is None) or (
        github_run_id is not None and not release_bound
    ):
        raise ValueError(
            "GitHub run id and run attempt must both be positive integers"
    )
    if normalized_commit is not None and release_bound:
        source = git_metadata(root)
        if not (
            exact_commit(source.get("commit")) == normalized_commit
            and source.get("dirty") is False
            and source.get("dirty_entries") == []
        ):
            raise ValueError(
                "hardware soak must run from the exact clean candidate source"
            )
    core_disabled = (
        expected_release_profile == "core_1_0"
        and expected_sd_history_mode == "disabled"
    )
    d1l_target: dict[str, Any] | None = None
    d1l_target_after: dict[str, Any] | None = None
    if core_disabled:
        try:
            port = enforce_core_port(port)
        except ValueError as exc:
            raise ValueError(
                "Core 1.0 soak requires COM12 or the exact Pi D1L by-id "
                "target"
            ) from exc
    if core_disabled and not (
        sample_storage
        and allow_sd_unavailable
        and not sd_file_canary
        and not clear_crashlog_before_start
    ):
        raise ValueError(
            "disabled Core soak requires --sample-storage, "
            "--allow-sd-unavailable, no SD canary, and no crashlog clear"
        )

    try:
        import serial
    except ImportError as exc:
        raise SystemExit("pyserial is required for hardware soak: python -m pip install pyserial") from exc
    if core_disabled:
        try:
            from serial.tools import list_ports
        except ImportError as exc:
            raise SystemExit(
                "pyserial list_ports is required to identify the Core D1L target"
            ) from exc
        d1l_target = resolve_core_target(
            port,
            port_lister=port_lister
            or (lambda: list_ports.comports(include_links=True)),
            platform_name=platform_name,
        )
        port = d1l_target["requested_path"]

    active_command = active_dm_command(
        active_public_text, active_dm_fingerprint, active_dm_text
    )
    if duration_sec <= 0:
        raise ValueError("duration_sec must be positive")
    if sample_interval_sec <= 0:
        raise ValueError("sample_interval_sec must be positive")
    if active_command and active_interval_sec <= 0:
        raise ValueError(
            "active_interval_sec must be positive when active DM TX is enabled"
        )
    peer_receipt_row = None
    peer_receipt: dict | None = None
    peer_before: dict | None = None
    peer_after: dict | None = None
    peer_before_receipt: dict | None = None
    peer_after_receipt: dict | None = None
    peer_counter_deltas: dict[str, int | None] | None = None
    peer_successful_send_count: int | None = None
    peer_expected_send_count: int | None = None
    peer_flow_ok: bool | None = None
    remote_peer_config: dict | None = None
    local_peer_config: dict | None = None
    peer_status_before_validation: dict | None = None
    peer_status_after_validation: dict | None = None
    peer_before_reservation: rf_acceptance.EvidenceReservation | None = None
    peer_after_reservation: rf_acceptance.EvidenceReservation | None = None
    after_capture: Path | None = None
    if active_command is not None and release_bound:
        if controlled_peer_receipt is None or normalized_commit is None:
            raise ValueError(
                "active DM soak requires --controlled-peer-receipt"
            )
        peer_receipt, peer_receipt_row = qualified_controlled_peer_receipt(
            path=controlled_peer_receipt,
            root=root,
            commit=normalized_commit,
            run_id=str(github_run_id),
            run_attempt=str(workflow_run_attempt),
            fingerprint=str(active_dm_fingerprint).upper(),
            expected_d1l_target_sha256=(
                d1l_target["stable_identity_sha256"]
                if d1l_target is not None
                else None
            ),
        )
        if not listener_test_text_ok(active_dm_text):
            raise ValueError(
                "controlled-peer active soak DM text must contain the "
                "case-insensitive word 'test'"
            )
        peer_expected_send_count = expected_active_send_count(
            duration_sec, active_interval_sec
        )
        if peer_expected_send_count is None:
            raise ValueError(
                "active soak duration and interval must define a positive "
                "send-count floor"
            )
        peer = peer_receipt["controlled_peer"]
        if (
            peer.get("evidence_source")
            == rf_acceptance.REMOTE_PEER_EVIDENCE_SOURCE
        ):
            remote_peer_config = rf_acceptance.validate_remote_peer_config(
                {
                    "ssh_host": peer.get("ssh_host"),
                    "hostname": peer.get("hostname"),
                    "status_path": peer.get("status_path"),
                    "control_socket": peer.get("control_socket"),
                    "device": peer.get("device"),
                    "public_key": peer.get("public_key"),
                    "max_status_age_sec": peer.get(
                        "max_status_age_sec"
                    ),
                }
            )
        elif (
            peer.get("evidence_source")
            == rf_acceptance.LOCAL_PEER_EVIDENCE_SOURCE
        ):
            local_peer_config = (
                rf_acceptance.validate_local_peer_config(
                    {
                        "hostname": peer.get("hostname"),
                        "status_path": peer.get("status_path"),
                        "control_socket": peer.get("control_socket"),
                        "device": peer.get("device"),
                        "public_key": peer.get("public_key"),
                        "max_status_age_sec": peer.get(
                            "max_status_age_sec"
                        ),
                    }
                )
            )
            rf_acceptance.require_local_peer_hostname()
        capture_dir = (
            peer_capture_dir
            or root / "artifacts" / "soak" / "rf-peer"
        )
        capture_token = (
            f"core-soak-{normalized_commit[:12]}-{github_run_id}-"
            f"{workflow_run_attempt}-"
            f"{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')}"
        )
        safe_token = re.sub(r"[^A-Za-z0-9_.-]", "_", capture_token)
        before_capture = capture_dir / f"{safe_token}_peer_before.json"
        after_capture = capture_dir / f"{safe_token}_peer_after.json"
        peer_before_reservation = evidence_bundle.reserve(
            "soak_peer_before",
            before_capture,
            label="active-soak controlled-peer before status",
        )
        peer_after_reservation = evidence_bundle.reserve(
            "soak_peer_after",
            after_capture,
            label="active-soak controlled-peer after status",
        )
        evidence_bundle.mark_external_io_started()
        if remote_peer_config is not None:
            (
                peer_before,
                peer_before_receipt,
                peer_status_before_validation,
            ) = rf_acceptance.capture_remote_peer_status(
                remote_peer_config,
                before_capture,
                root,
                reservation=peer_before_reservation,
                evidence_bundle=evidence_bundle,
            )
        elif local_peer_config is not None:
            (
                peer_before,
                peer_before_receipt,
                peer_status_before_validation,
            ) = rf_acceptance.capture_local_peer_status(
                local_peer_config,
                before_capture,
                root,
                reservation=peer_before_reservation,
                evidence_bundle=evidence_bundle,
            )
        else:
            (
                peer_before,
                peer_before_receipt,
            ) = rf_acceptance.capture_peer_status(
                rf_acceptance.RADIO_LISTENER_STATUS_PATH,
                before_capture,
                root,
                reservation=peer_before_reservation,
            )
        expected_peer_key = rf_acceptance.exact_public_key(
            peer.get("public_key")
        )
        identity_ready = (
            expected_peer_key is not None
            and rf_acceptance.exact_public_key(
                rf_acceptance.get_path(
                    peer_before, "serial", "public_key"
                )
            )
            == expected_peer_key
            and rf_acceptance.public_key_fingerprint(expected_peer_key)
            == str(active_dm_fingerprint).upper()
        )
        if (
            remote_peer_config is not None
            or local_peer_config is not None
        ):
            identity_ready = (
                identity_ready
                and peer_status_before_validation is not None
                and peer_status_before_validation.get("ok") is True
            )
        else:
            identity_ready = (
                identity_ready
                and rf_acceptance.radio_listener_connected(
                    peer_before,
                    rf_acceptance.RADIO_LISTENER_PORT,
                    str(active_dm_fingerprint).upper(),
                )
            )
        if not identity_ready:
            raise ValueError(
                "controlled-peer status/public-key identity is not "
                "ready for active soak"
            )
    if not evidence_bundle.external_io_started:
        evidence_bundle.mark_external_io_started()
    samples: list[dict] = []
    active_events: list[dict] = []
    setup_events: list[dict] = []
    version_preflight: dict = {}
    firmware_identity_ok: bool | None = None
    preflight_failure: str | None = None
    started_at = datetime.now(timezone.utc)
    commands = soak_commands(sample_storage=sample_storage, sd_file_canary=sd_file_canary)

    with open_d1l_serial(serial, port=port, baudrate=baud, timeout=timeout) as ser:
        time.sleep(startup_settle_sec)
        ser.reset_input_buffer()

        if normalized_commit is not None:
            version_preflight = send_soak_command(
                ser, "version", timeout, command_retries, retry_delay_sec
            )
            setup_events.append(
                {
                    "elapsed_sec": 0.0,
                    "cmd": "version",
                    "result": version_preflight,
                }
            )
            firmware_identity_ok = (
                version_preflight.get("ok") is True
                and firmware_identity_matches(version_preflight, normalized_commit)
                and (
                    expected_release_profile is None
                    or version_preflight.get("release_profile")
                    == expected_release_profile
                )
                and (
                    expected_sd_history_mode is None
                    or version_preflight.get("sd_history_mode")
                    == expected_sd_history_mode
                )
            )
            if version_preflight.get("ok") is not True:
                preflight_failure = "version_preflight_failed"
            elif not firmware_identity_ok:
                preflight_failure = "firmware_identity_mismatch"

        if clear_crashlog_before_start:
            if preflight_failure is None:
                setup_events.append(
                    {
                        "elapsed_sec": 0.0,
                        "cmd": "crashlog clear",
                        "result": send_soak_command(
                            ser,
                            "crashlog clear",
                            timeout,
                            command_retries,
                            retry_delay_sec,
                        ),
                    }
                )

        fatal_timeout_command = next(
            (
                event.get("cmd")
                for event in setup_events
                if event.get("result", {}).get("code") in TERMINAL_COMMAND_CODES
            ),
            None,
        )

        start = time.monotonic()
        deadline = start + duration_sec
        next_sample = start
        next_active = start if active_command else float("inf")
        sample_index = 0

        while fatal_timeout_command is None and preflight_failure is None:
            now = time.monotonic()
            elapsed = now - start

            if now >= next_sample:
                label = "start" if sample_index == 0 else f"sample-{sample_index}"
                samples.append(
                    collect_sample(
                        ser,
                        timeout,
                        label,
                        elapsed,
                        command_retries,
                        retry_delay_sec,
                        commands,
                        allow_sd_unavailable,
                    )
                )
                if samples[-1].get("aborted_after_timeout"):
                    fatal_timeout_command = samples[-1]["aborted_after_timeout"]
                    break
                sample_index += 1
                next_sample += sample_interval_sec

            now = time.monotonic()
            elapsed = now - start
            if active_command and now >= next_active and now < deadline:
                result = send_soak_command(
                    ser,
                    active_command,
                    timeout,
                    command_retries,
                    retry_delay_sec,
                )
                active_events.append(
                    {
                        "elapsed_sec": round(elapsed, 3),
                        "command": active_command,
                        "fingerprint": active_dm_fingerprint.upper(),
                        "text": active_dm_text,
                        "result": result,
                    }
                )
                if result.get("code") in TERMINAL_COMMAND_CODES:
                    fatal_timeout_command = result.get("cmd") or "active command"
                    break
                next_active += active_interval_sec

            if now >= deadline:
                break

            sleep_for = min(next_sample, next_active, deadline) - now
            time.sleep(max(0.1, min(sleep_for, 1.0)))

        if fatal_timeout_command is None and preflight_failure is None:
            final_elapsed = time.monotonic() - start
            final_sample = collect_sample(
                ser,
                timeout,
                "final",
                final_elapsed,
                command_retries,
                retry_delay_sec,
                commands,
                allow_sd_unavailable,
            )
            samples.append(final_sample)
            if final_sample.get("aborted_after_timeout"):
                fatal_timeout_command = final_sample["aborted_after_timeout"]

    if d1l_target is not None:
        d1l_target_after = resolve_core_target(
            port,
            port_lister=port_lister
            or (lambda: list_ports.comports(include_links=True)),
            platform_name=platform_name,
        )
        if (
            d1l_target_after["stable_identity_sha256"]
            != d1l_target["stable_identity_sha256"]
        ):
            raise ValueError("D1L serial target identity changed during soak")

    if peer_before is not None:
        if (
            peer_receipt is None
            or after_capture is None
            or peer_before_receipt is None
        ):
            raise ValueError("active soak peer capture state is incomplete")
        if remote_peer_config is not None:
            (
                peer_after,
                peer_after_receipt,
                peer_status_after_validation,
            ) = rf_acceptance.capture_remote_peer_status(
                remote_peer_config,
                after_capture,
                root,
                reservation=peer_after_reservation,
                evidence_bundle=evidence_bundle,
            )
        elif local_peer_config is not None:
            (
                peer_after,
                peer_after_receipt,
                peer_status_after_validation,
            ) = rf_acceptance.capture_local_peer_status(
                local_peer_config,
                after_capture,
                root,
                reservation=peer_after_reservation,
                evidence_bundle=evidence_bundle,
            )
        else:
            (
                peer_after,
                peer_after_receipt,
            ) = rf_acceptance.capture_peer_status(
                rf_acceptance.RADIO_LISTENER_STATUS_PATH,
                after_capture,
                root,
                reservation=peer_after_reservation,
            )
        peer_successful_send_count = sum(
            1
            for event in active_events
            if isinstance(event, dict)
            and isinstance(event.get("result"), dict)
            and event["result"].get("ok") is True
        )
        peer_flow_ok, peer_counter_deltas = active_listener_flow_ok(
            peer_before,
            peer_after,
            successful_send_count=peer_successful_send_count,
            d1l_public_key=peer_receipt.get("d1l_public_key"),
            peer_fingerprint=str(active_dm_fingerprint).upper(),
            peer_public_key=peer_receipt["controlled_peer"].get(
                "public_key"
            ),
            minimum_send_count=peer_expected_send_count or 1,
            remote_config=remote_peer_config,
            local_config=local_peer_config,
        )

    ended_at = datetime.now(timezone.utc)
    summary = summarize_soak(
        samples=samples,
        active_events=active_events,
        require_rx_delta=require_rx_delta,
        min_rx_delta=min_rx_delta,
        min_tx_delta=min_tx_delta,
        sample_storage=sample_storage or sd_file_canary,
        sd_file_canary=sd_file_canary,
        allow_sd_unavailable=allow_sd_unavailable,
    )
    setup_failures = [
        {
            "sample": "setup",
            "elapsed_sec": event.get("elapsed_sec"),
            "cmd": event.get("cmd"),
            "code": event.get("result", {}).get("code"),
        }
        for event in setup_events
        if not event.get("result", {}).get("ok")
    ]
    if setup_failures:
        summary["command_failures"].extend(setup_failures)
        summary["command_failure_count"] = len(summary["command_failures"])
        summary["ok"] = False
    if preflight_failure is not None:
        summary["threshold_failures"].append(preflight_failure)
        summary["ok"] = False
    if peer_flow_ok is False:
        summary["threshold_failures"].append(
            "controlled_peer_flow_not_exact"
        )
        summary["ok"] = False

    report = {
        "schema": 2 if core_disabled else 1,
        "mode": "hardware",
        "hardware_required": True,
        "physical_observed": True,
        "closure_eligible": summary["ok"],
        "port": port,
        "baud": baud,
        "preflight_commands": ["version"] if normalized_commit is not None else [],
        "expected_firmware_commit": normalized_commit,
        "github_actions_run": (
            str(github_run_id) if github_run_id is not None else None
        ),
        "workflow_run_attempt": (
            str(workflow_run_attempt)
            if workflow_run_attempt is not None
            else None
        ),
        "device_build_commit": version_preflight.get("build_commit"),
        "device_idf_version": version_preflight.get("idf"),
        "device_release_profile": version_preflight.get(
            "release_profile"
        ),
        "device_sd_history_mode": version_preflight.get(
            "sd_history_mode"
        ),
        "expected_release_profile": expected_release_profile,
        "expected_sd_history_mode": expected_sd_history_mode,
        "release_profile": expected_release_profile,
        "sd_history_mode": expected_sd_history_mode,
        "firmware_identity_required": normalized_commit is not None,
        "firmware_identity_ok": firmware_identity_ok,
        "version_preflight": version_preflight,
        "preflight_failure": preflight_failure,
        "started_at": started_at.isoformat().replace("+00:00", "Z"),
        "ended_at": ended_at.isoformat().replace("+00:00", "Z"),
        "duration_sec": duration_sec,
        "sample_interval_sec": sample_interval_sec,
        "commands": commands,
        "active_public_text": None,
        "active_dm_fingerprint": (
            active_dm_fingerprint.upper() if active_dm_fingerprint else None
        ),
        "active_dm_text": active_dm_text,
        "active_command": active_command,
        "controlled_peer_receipt": peer_receipt_row,
        "controlled_peer_before": rf_acceptance.status_snapshot(
            peer_before
        ),
        "controlled_peer_after": rf_acceptance.status_snapshot(peer_after),
        "controlled_peer_before_receipt": peer_before_receipt,
        "controlled_peer_after_receipt": peer_after_receipt,
        "controlled_peer_remote": (
            {
                "before_validation": peer_status_before_validation,
                "after_validation": peer_status_after_validation,
            }
            if (
                remote_peer_config is not None
                or local_peer_config is not None
            )
            else None
        ),
        "controlled_peer_counter_deltas": peer_counter_deltas,
        "controlled_peer_successful_send_count": (
            peer_successful_send_count
        ),
        "controlled_peer_expected_send_count": peer_expected_send_count,
        "controlled_peer_flow_ok": peer_flow_ok,
        "dm_rf_tx": active_command is not None,
        "public_rf_tx": False,
        "formats_sd": False,
        "sample_storage": sample_storage or sd_file_canary,
        "sd_file_canary": sd_file_canary,
        "allow_sd_unavailable": allow_sd_unavailable,
        "active_interval_sec": active_interval_sec,
        "require_rx_delta": require_rx_delta,
        "min_rx_delta": min_rx_delta,
        "min_tx_delta": min_tx_delta,
        "clear_crashlog_before_start": clear_crashlog_before_start,
        "command_retries": command_retries,
        "retry_delay_sec": retry_delay_sec,
        "aborted_after_timeout": fatal_timeout_command,
        "ok": summary["ok"],
        "summary": summary,
        "setup_events": setup_events,
        "active_events": active_events,
        "samples": samples,
    }
    if core_disabled:
        report.update(
            {
                "d1l_target": d1l_target,
                "d1l_target_after": d1l_target_after,
                "target_identity_continuity_ok": True,
            }
        )
    return report


def run_serial_soak(
    *,
    evidence_bundle: rf_acceptance.EvidenceBundle | None = None,
    **kwargs,
) -> dict:
    root = Path(__file__).resolve().parents[1]
    if evidence_bundle is not None:
        if evidence_bundle.root != root.resolve(strict=True):
            raise ValueError(
                "soak evidence bundle is bound to a different repository"
            )
        return _run_serial_soak_reserved(
            evidence_bundle=evidence_bundle,
            **kwargs,
        )
    with rf_acceptance.EvidenceBundle(root) as owned_bundle:
        return _run_serial_soak_reserved(
            evidence_bundle=owned_bundle,
            **kwargs,
        )


def resolve_out_path(out_arg: str | None, mode: str) -> Path:
    root = Path(__file__).resolve().parents[1]
    if out_arg:
        out_path = Path(out_arg)
        if not out_path.is_absolute():
            out_path = root / out_path
        return out_path
    return root / "artifacts" / "soak" / f"d1l-soak-{mode}-{utc_stamp()}.json"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--duration-sec", type=float, default=300.0)
    parser.add_argument("--sample-interval-sec", type=float, default=60.0)
    parser.add_argument(
        "--active-public-text",
        default=None,
        help="disabled safety compatibility flag; automated Public TX is prohibited",
    )
    parser.add_argument("--active-dm-fingerprint", default=None)
    parser.add_argument("--active-dm-text", default=None)
    parser.add_argument("--active-interval-sec", type=float, default=120.0)
    parser.add_argument("--startup-settle-sec", type=float, default=1.0)
    parser.add_argument("--require-rx-delta", action="store_true")
    parser.add_argument("--min-rx-delta", type=int, default=1)
    parser.add_argument("--min-tx-delta", type=int, default=0)
    parser.add_argument("--clear-crashlog-before-start", action="store_true")
    parser.add_argument("--command-retries", type=int, default=1)
    parser.add_argument("--retry-delay-sec", type=float, default=0.5)
    parser.add_argument("--sample-storage", action="store_true")
    parser.add_argument("--sd-file-canary", action="store_true")
    parser.add_argument("--allow-sd-unavailable", action="store_true")
    parser.add_argument("--expected-firmware-commit", default=None)
    parser.add_argument("--github-run-id")
    parser.add_argument("--github-run-attempt")
    parser.add_argument("--expected-release-profile")
    parser.add_argument("--expected-sd-history-mode")
    parser.add_argument("--controlled-peer-receipt")
    parser.add_argument(
        "--peer-capture-dir",
        help=(
            "repository-contained directory for read-only local COM15 or "
            "remote controlled-peer raw status "
            "snapshots (active release soak only)"
        ),
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    try:
        active_dm_command(
            args.active_public_text,
            args.active_dm_fingerprint,
            args.active_dm_text,
        )
    except ValueError as exc:
        parser.error(str(exc))

    normalized_commit = (
        exact_commit(args.expected_firmware_commit)
        if args.expected_firmware_commit is not None
        else None
    )
    if args.expected_firmware_commit is not None and normalized_commit is None:
        parser.error(
            "--expected-firmware-commit must be an exact 40-character hexadecimal SHA"
        )

    mode = "dry-run" if args.dry_run else "hardware"
    root = Path(__file__).resolve().parents[1]
    out_path = resolve_out_path(args.out, mode)
    try:
        with rf_acceptance.EvidenceBundle(root) as evidence_bundle:
            report_reservation = evidence_bundle.reserve(
                "report",
                out_path,
                label="D1L soak report",
            )
            if args.dry_run:
                report = dry_run_report(
                    duration_sec=args.duration_sec,
                    sample_interval_sec=args.sample_interval_sec,
                    active_public_text=args.active_public_text,
                    active_interval_sec=args.active_interval_sec,
                    require_rx_delta=args.require_rx_delta,
                    min_rx_delta=args.min_rx_delta,
                    min_tx_delta=args.min_tx_delta,
                    clear_crashlog_before_start=(
                        args.clear_crashlog_before_start
                    ),
                    command_retries=args.command_retries,
                    retry_delay_sec=args.retry_delay_sec,
                    sample_storage=args.sample_storage
                    or args.sd_file_canary,
                    sd_file_canary=args.sd_file_canary,
                    allow_sd_unavailable=args.allow_sd_unavailable,
                    active_dm_fingerprint=args.active_dm_fingerprint,
                    active_dm_text=args.active_dm_text,
                    expected_firmware_commit=normalized_commit,
                    github_run_id=args.github_run_id,
                    workflow_run_attempt=args.github_run_attempt,
                )
            else:
                if not args.port:
                    parser.error(
                        "No D1L port supplied. Set D1L_PORT or pass --port."
                    )
                if normalized_commit is None:
                    parser.error(
                        "Hardware soak requires --expected-firmware-commit."
                    )
                if (
                    not str(args.github_run_id or "").isdigit()
                    or int(str(args.github_run_id or "0")) < 1
                    or not str(args.github_run_attempt or "").isdigit()
                    or int(str(args.github_run_attempt or "0")) < 1
                ):
                    parser.error(
                        "Hardware soak requires positive --github-run-id and "
                        "--github-run-attempt."
                    )
                report = run_serial_soak(
                    port=args.port,
                    baud=args.baud,
                    timeout=args.timeout,
                    duration_sec=args.duration_sec,
                    sample_interval_sec=args.sample_interval_sec,
                    active_public_text=args.active_public_text,
                    active_interval_sec=args.active_interval_sec,
                    startup_settle_sec=args.startup_settle_sec,
                    require_rx_delta=args.require_rx_delta,
                    min_rx_delta=args.min_rx_delta,
                    min_tx_delta=args.min_tx_delta,
                    clear_crashlog_before_start=(
                        args.clear_crashlog_before_start
                    ),
                    command_retries=args.command_retries,
                    retry_delay_sec=args.retry_delay_sec,
                    sample_storage=args.sample_storage
                    or args.sd_file_canary,
                    sd_file_canary=args.sd_file_canary,
                    allow_sd_unavailable=args.allow_sd_unavailable,
                    active_dm_fingerprint=args.active_dm_fingerprint,
                    active_dm_text=args.active_dm_text,
                    expected_firmware_commit=normalized_commit,
                    github_run_id=str(args.github_run_id),
                    workflow_run_attempt=str(args.github_run_attempt),
                    expected_release_profile=args.expected_release_profile,
                    expected_sd_history_mode=args.expected_sd_history_mode,
                    controlled_peer_receipt=(
                        (
                            Path(args.controlled_peer_receipt)
                            if Path(
                                args.controlled_peer_receipt
                            ).is_absolute()
                            else root / args.controlled_peer_receipt
                        )
                        if args.controlled_peer_receipt
                        else None
                    ),
                    peer_capture_dir=(
                        (
                            Path(args.peer_capture_dir)
                            if Path(args.peer_capture_dir).is_absolute()
                            else root / args.peer_capture_dir
                        )
                        if args.peer_capture_dir
                        else None
                    ),
                    evidence_bundle=evidence_bundle,
                )

            stamp_report(report, root)
            report_reservation.write_bytes(
                (json.dumps(report, indent=2) + "\n").encode("ascii")
            )
    except ValueError as exc:
        parser.error(str(exc))
    print(json.dumps(report))
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
