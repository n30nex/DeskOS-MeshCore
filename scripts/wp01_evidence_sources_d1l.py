#!/usr/bin/env python3
"""Deterministic, source-bound builders for autonomous WP-01 evidence."""

from __future__ import annotations

import hashlib
import json
import math
import re
from pathlib import Path
from typing import Any, Iterable

try:
    from sd_remove_reinsert_acceptance_d1l import (
        validate_report as remove_reinsert_source_ok,
    )
    from sd_reboot_remount_acceptance_d1l import (
        crashlog_transition_passed,
        map_tile_check_passed,
        map_tile_write_passed,
        persistence_readback_commands,
        persistence_snapshot_clean,
        storage_ready,
        unexpected_console_restart,
    )
    from sd_retained_history_acceptance_d1l import (
        fingerprint_for_token,
        readbacks_pass,
        retained_storage_clean,
    )
    from sd_file_canary_d1l import filecanary_passed as sd_filecanary_passed
    from soak_d1l import (
        file_ops_ready,
        sd_status,
        soak_commands,
        storage_status_malformed,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.sd_remove_reinsert_acceptance_d1l import (
        validate_report as remove_reinsert_source_ok,
    )
    from scripts.sd_reboot_remount_acceptance_d1l import (
        crashlog_transition_passed,
        map_tile_check_passed,
        map_tile_write_passed,
        persistence_readback_commands,
        persistence_snapshot_clean,
        storage_ready,
        unexpected_console_restart,
    )
    from scripts.sd_retained_history_acceptance_d1l import (
        fingerprint_for_token,
        readbacks_pass,
        retained_storage_clean,
    )
    from scripts.sd_file_canary_d1l import (
        filecanary_passed as sd_filecanary_passed,
    )
    from scripts.soak_d1l import (
        file_ops_ready,
        sd_status,
        soak_commands,
        storage_status_malformed,
    )


COMMIT_RE = re.compile(r"[0-9a-f]{40}")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
SOURCE_BOUND_KINDS = {
    "sd_inserted_stability",
    "sd_remove_reinsert",
    "retained_reboot_matrix",
    "storage_active_soak",
}
RETAINED_STACK_MIN_BYTES = 4096
INSERTED_STABILITY_MIN_SECONDS = 300.0
REBOOT_MIN_CYCLES = 5
STORAGE_ACTIVE_SOAK_MIN_SECONDS = 2 * 60 * 60


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def _port(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().upper()
    return normalized or None


def _exact_commit(value: object, commit: str) -> bool:
    return (
        isinstance(value, str)
        and COMMIT_RE.fullmatch(value.lower()) is not None
        and value.lower() == commit.lower()
    )


def _integer(value: object, *, minimum: int = 0) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        return None
    return value


def _number(value: object) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    normalized = float(value)
    return normalized if math.isfinite(normalized) else None


def _raw_git_clean(data: dict, commit: str) -> bool:
    git = data.get("git")
    return bool(
        _exact_commit(data.get("commit"), commit)
        and isinstance(git, dict)
        and _exact_commit(git.get("commit"), commit)
        and git.get("dirty") is False
        and git.get("dirty_entries") == []
    )


def _nested_true(value: object, names: set[str]) -> bool:
    if isinstance(value, dict):
        return any(
            (key in names and child is True) or _nested_true(child, names)
            for key, child in value.items()
        )
    if isinstance(value, list):
        return any(_nested_true(child, names) for child in value)
    return False


def _nested_code_count(value: object, code: str) -> int:
    if isinstance(value, dict):
        return (1 if value.get("code") == code else 0) + sum(
            _nested_code_count(child, code) for child in value.values()
        )
    if isinstance(value, list):
        return sum(_nested_code_count(child, code) for child in value)
    return 0


def _base_artifact(
    kind: str,
    *,
    root: Path,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
    provenance_path: Path,
) -> tuple[dict[str, Any], dict, str]:
    resolved_provenance = provenance_path.resolve()
    provenance_allowed = _within(resolved_provenance, root)
    provenance = read_json(resolved_provenance) if provenance_allowed else {}
    provenance_sha256 = (
        sha256_file(resolved_provenance)
        if provenance_allowed and resolved_provenance.is_file()
        else ""
    )
    artifact: dict[str, Any] = {
        "schema": 1,
        "kind": kind,
        "mode": "hardware",
        "ok": False,
        "commit": commit.lower(),
        "github_actions_run": str(github_actions_run),
        "ports": {"d1l": _port(d1l_port), "rp2040": _port(rp2040_port)},
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "provenance": provenance,
        "provenance_receipt_sha256": provenance_sha256,
        "provenance_receipt": {
            "path": str(resolved_provenance),
            "sha256": provenance_sha256,
        },
        "source_receipts": [],
        "wdt_count": 0,
        "panic_count": 0,
        "brownout_count": 0,
        "terminal_timeout_count": 0,
        "failures": [],
    }
    return artifact, provenance, provenance_sha256


def _provenance_context_ok(
    provenance: dict,
    provenance_sha256: str,
    *,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
) -> bool:
    ports = provenance.get("ports")
    return bool(
        SHA256_RE.fullmatch(provenance_sha256.lower())
        and provenance.get("schema") == 1
        and provenance.get("kind") == "wp01_exact_pair_provenance"
        and provenance.get("mode") == "hardware"
        and provenance.get("ok") is True
        and _exact_commit(provenance.get("source_commit"), commit)
        and str(provenance.get("github_actions_run")) == str(github_actions_run)
        and isinstance(ports, dict)
        and _port(ports.get("d1l")) == _port(d1l_port)
        and _port(ports.get("rp2040")) == _port(rp2040_port)
        and _exact_commit(provenance.get("esp32_device_build_commit"), commit)
        and _exact_commit(provenance.get("rp2040_device_build_commit"), commit)
        and provenance.get("exact_pair_installed") is True
        and provenance.get("checksums_verified") is True
        and provenance.get("esp32_flash_verified") is True
        and provenance.get("rp2040_flash_verified") is True
        and provenance.get("public_rf_tx") is False
        and provenance.get("formats_sd") is False
    )


def _source(path: Path, root: Path) -> tuple[Path, dict, str]:
    resolved = path.resolve()
    if not _within(resolved, root):
        raise ValueError(f"source is outside repository root: {resolved}")
    data = read_json(resolved)
    if not data:
        raise ValueError(f"source is missing or invalid JSON: {resolved}")
    return resolved, data, sha256_file(resolved)


def _sample_result(sample: dict, command: str) -> dict | None:
    rows = [
        result
        for result in sample.get("results", [])
        if isinstance(result, dict) and result.get("cmd") == command
    ]
    return rows[0] if len(rows) == 1 else None


def _retained_status_has_failures(status: dict) -> bool:
    return not retained_storage_clean(status)


def _packet_status_has_failures(packet: dict) -> bool:
    persistence = packet.get("persistence")
    if not (
        packet.get("schema") == 1
        and packet.get("ok") is True
        and packet.get("cmd") == "packets"
        and isinstance(persistence, dict)
        and persistence.get("loaded") is True
    ):
        return True
    containers = (
        (persistence, "failures", None),
        (persistence.get("reconcile"), "failures", "last_error"),
        (persistence.get("sd"), "failures", "last_error"),
        (persistence.get("nvs"), "failures", "last_error"),
        (persistence.get("journal"), "failures", "last_error"),
    )
    for container, counter, error in containers:
        if not isinstance(container, dict) or _integer(container.get(counter)) != 0:
            return True
        if error and container.get(error) != "ESP_OK":
            return True
    return False


def _new_crash_entries(samples: list[dict]) -> list[dict]:
    crashlogs = [
        row
        for sample in samples
        if (row := _sample_result(sample, "crashlog")) is not None
    ]
    if (
        len(crashlogs) != len(samples)
        or not crashlogs
        or any(
            row.get("schema") != 1
            or row.get("ok") is not True
            or row.get("cmd") != "crashlog"
            for row in crashlogs
        )
    ):
        raise ValueError("each soak sample must contain exactly one crashlog result")
    first_entries = crashlogs[0].get("entries")
    if not isinstance(first_entries, list) or any(
        not isinstance(row.get("entries"), list) for row in crashlogs
    ):
        raise ValueError("soak crashlog entries are missing")
    totals = [_integer(row.get("total_written")) for row in crashlogs]
    if any(value is None for value in totals) or any(
        current < previous
        for previous, current in zip(totals, totals[1:])
        if previous is not None and current is not None
    ):
        raise ValueError("soak crashlog total is not monotonic")
    first_sequences = [
        entry.get("seq")
        for entry in first_entries
        if isinstance(entry, dict) and _integer(entry.get("seq"), minimum=1) is not None
    ]
    baseline = max(first_sequences, default=0)
    new_entries: dict[int, dict] = {}
    for row in crashlogs:
        for entry in row["entries"]:
            if not isinstance(entry, dict):
                continue
            sequence = _integer(entry.get("seq"), minimum=1)
            if sequence is not None and sequence > baseline:
                new_entries[sequence] = entry
    total_delta = max(value for value in totals if value is not None) - min(
        value for value in totals if value is not None
    )
    if total_delta > len(new_entries):
        raise ValueError("soak crashlog increment lacks complete entry evidence")
    return [new_entries[sequence] for sequence in sorted(new_entries)]


def build_sd_inserted_stability_artifact(
    source_path: Path,
    provenance_path: Path,
    *,
    root: Path,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
) -> dict:
    artifact, provenance, provenance_sha256 = _base_artifact(
        "sd_inserted_stability",
        root=root,
        commit=commit,
        github_actions_run=github_actions_run,
        d1l_port=d1l_port,
        rp2040_port=rp2040_port,
        provenance_path=provenance_path,
    )
    try:
        if not _within(provenance_path, root):
            raise ValueError("exact-pair provenance is outside repository root")
        if not _provenance_context_ok(
            provenance,
            provenance_sha256,
            commit=commit,
            github_actions_run=github_actions_run,
            d1l_port=d1l_port,
            rp2040_port=rp2040_port,
        ):
            raise ValueError("exact-pair provenance does not match inserted-card context")
        resolved, source, source_sha256 = _source(source_path, root)
        samples = source.get("samples")
        summary = source.get("summary")
        version = source.get("version_preflight")
        if not (
            source.get("schema") == 1
            and source.get("mode") == "hardware"
            and source.get("ok") is True
            and _port(source.get("port")) == _port(d1l_port)
            and source.get("baud") == 115200
            and _raw_git_clean(source, commit)
            and _exact_commit(source.get("expected_firmware_commit"), commit)
            and _exact_commit(source.get("device_build_commit"), commit)
            and source.get("firmware_identity_required") is True
            and source.get("firmware_identity_ok") is True
            and source.get("preflight_failure") is None
            and source.get("preflight_commands") == ["version"]
            and isinstance(version, dict)
            and version.get("ok") is True
            and version.get("cmd") == "version"
            and _exact_commit(version.get("build_commit"), commit)
            and source.get("sample_storage") is True
            and source.get("sd_file_canary") is True
            and source.get("allow_sd_unavailable") is False
            and source.get("commands")
            == soak_commands(sample_storage=True, sd_file_canary=True)
            and source.get("active_command") is None
            and source.get("dm_rf_tx") is False
            and source.get("public_rf_tx") is False
            and source.get("formats_sd") is False
            and source.get("aborted_after_timeout") is None
            and isinstance(summary, dict)
            and summary.get("ok") is True
            and summary.get("command_failure_count") == 0
            and summary.get("command_failures") == []
            and summary.get("threshold_failures") == []
            and summary.get("command_timeout_seen") is False
            and summary.get("unexpected_console_restart_seen") is False
            and isinstance(samples, list)
            and len(samples) >= 2
        ):
            raise ValueError("inserted-card soak source failed identity or safety checks")
        if _nested_true(source, {"public_rf_tx", "formats_sd", "format_performed"}):
            raise ValueError("inserted-card soak source contains unsafe action evidence")

        expected_commands = soak_commands(sample_storage=True, sd_file_canary=True)
        elapsed = [_number(sample.get("elapsed_sec")) for sample in samples]
        if (
            any(value is None for value in elapsed)
            or not 0.0 <= elapsed[0] <= 5.0
            or any(
                current <= previous
                for previous, current in zip(elapsed, elapsed[1:])
                if previous is not None and current is not None
            )
            or samples[0].get("label") != "start"
            or samples[-1].get("label") != "final"
        ):
            raise ValueError("inserted-card soak has malformed elapsed times")
        duration_sec = round(elapsed[-1] - elapsed[0], 3)
        health_rows: list[dict] = []
        storage_rows: list[dict] = []
        packet_rows: list[dict] = []
        for sample in samples:
            results = sample.get("results")
            if not (
                sample.get("aborted_after_timeout") is None
                and isinstance(results, list)
                and [row.get("cmd") for row in results if isinstance(row, dict)]
                == expected_commands
                and len(results) == len(expected_commands)
                and all(
                    isinstance(row, dict)
                    and row.get("schema") == 1
                    and row.get("ok") is True
                    for row in results
                )
            ):
                raise ValueError("inserted-card soak sample command cohort failed")
            health = _sample_result(sample, "health")
            storage = _sample_result(sample, "storage status")
            packet = _sample_result(sample, "packets")
            filecanary = _sample_result(sample, "storage filecanary")
            if not all(isinstance(row, dict) for row in (health, storage, packet)):
                raise ValueError("each soak sample must contain health, packets, and storage status")
            if not isinstance(filecanary, dict) or not sd_filecanary_passed(filecanary):
                raise ValueError("each soak sample must pass the SD file canary")
            health_rows.append(health)
            storage_rows.append(storage)
            packet_rows.append(packet)

        card_inserted = [
            not storage_status_malformed(row) and file_ops_ready(row)
            for row in storage_rows
        ]
        false_no_card_count = sum(
            1
            for row in storage_rows
            if sd_status(row).get("state") == "no_card"
            or sd_status(row).get("present") is False
            or sd_status(row).get("mounted") is False
        )
        generations = []
        for row in packet_rows:
            persistence = row.get("persistence")
            sd = persistence.get("sd") if isinstance(persistence, dict) else None
            generation = _integer(sd.get("generation")) if isinstance(sd, dict) else None
            if generation is None:
                raise ValueError("packet persistence generation is missing")
            generations.append(generation)
        generation_change_count = sum(
            current != previous
            for previous, current in zip(generations, generations[1:])
        )
        retained_failure_count = sum(
            _retained_status_has_failures(storage) or _packet_status_has_failures(packet)
            for storage, packet in zip(storage_rows, packet_rows)
        )
        boot_nonces = [_integer(row.get("boot_nonce"), minimum=1) for row in health_rows]
        uptimes = [_integer(row.get("uptime_ms")) for row in health_rows]
        if any(value is None for value in boot_nonces + uptimes):
            raise ValueError("soak health nonce or uptime is missing")
        reset_count = sum(
            current != previous
            for previous, current in zip(boot_nonces, boot_nonces[1:])
        ) + sum(
            current < previous for previous, current in zip(uptimes, uptimes[1:])
        )
        reset_count += sum(
            1
            for sample in samples
            for result in sample.get("results", [])
            if isinstance(result, dict) and unexpected_console_restart([result])
        )
        new_crashes = _new_crash_entries(samples)
        wdt_count = sum("WDT" in str(entry.get("reset_reason", "")) for entry in new_crashes)
        panic_count = sum(entry.get("reset_reason") == "PANIC" for entry in new_crashes)
        brownout_count = sum(entry.get("reset_reason") == "BROWNOUT" for entry in new_crashes)
        terminal_timeout_count = _nested_code_count(source, "TIMEOUT")

        artifact.update(
            {
                "source_receipts": [
                    {
                        "kind": "soak_d1l",
                        "path": str(resolved),
                        "sha256": source_sha256,
                    }
                ],
                "duration_sec": duration_sec,
                "sample_count": len(samples),
                "status_poll_count": len(storage_rows),
                "card_inserted_all_samples": all(card_inserted),
                "final_ready_sd": card_inserted[-1],
                "false_no_card_count": false_no_card_count,
                "unintended_backend_generation_count": generation_change_count,
                "retained_failure_count": retained_failure_count,
                "reset_count": reset_count,
                "wdt_count": wdt_count,
                "panic_count": panic_count,
                "brownout_count": brownout_count,
                "new_crash_entry_count": len(new_crashes),
                "terminal_timeout_count": terminal_timeout_count,
            }
        )
        semantic_ok = bool(
            duration_sec >= INSERTED_STABILITY_MIN_SECONDS
            and all(card_inserted)
            and false_no_card_count == 0
            and generation_change_count == 0
            and retained_failure_count == 0
            and reset_count == 0
            and wdt_count == 0
            and panic_count == 0
            and brownout_count == 0
            and len(new_crashes) == 0
            and terminal_timeout_count == 0
        )
        if not semantic_ok:
            raise ValueError("inserted-card soak did not satisfy canonical thresholds")
    except Exception as exc:
        artifact["failures"] = [str(exc)]
    artifact["ok"] = not artifact["failures"]
    return artifact


def _command_result_transcript(data: dict) -> list[tuple[str, dict]] | None:
    commands = data.get("commands")
    results = data.get("results")
    if (
        not isinstance(commands, list)
        or not isinstance(results, list)
        or not commands
        or len(commands) != len(results)
    ):
        return None
    transcript: list[tuple[str, dict]] = []
    for command, result in zip(commands, results):
        if not isinstance(command, str) or not isinstance(result, dict):
            return None
        result_command = result.get("cmd")
        if (
            not isinstance(result_command, str)
            or (command != result_command and not command.startswith(result_command + " "))
        ):
            return None
        transcript.append((command, result))
    return transcript


def _allowed_reboot_command(command: str, token: str, fingerprint: str) -> bool:
    return command in {
        "version",
        "crashlog",
        "storage status",
        "storage remount",
        "storage filecanary",
        f"storage retained-canary {token}",
        f"storage map-tile-canary {token}",
        f"messages public search {token}",
        f"messages dm {fingerprint}",
        f"routes trace {fingerprint}",
        f"packets search {token}",
        "health",
        "reboot",
        f"storage map-tile-check {token}",
        "routes",
    }


def _full_command_map(transcript: Iterable[tuple[str, dict]]) -> dict[str, dict]:
    return {command: result for command, result in transcript}


def _successful_remount(rows: list[tuple[str, dict]]) -> bool:
    receipts = [result for command, result in rows if command == "storage remount"]
    return bool(
        receipts
        and receipts[-1].get("ok") is True
        and receipts[-1].get("retained_worker_quiesce_acquired") is True
    )


def _derive_reboot_cycle(
    source: dict,
    *,
    commit: str,
    d1l_port: str,
) -> tuple[dict, str, int, int, int, int]:
    transcript = _command_result_transcript(source)
    token = source.get("token")
    if not isinstance(token, str) or not token:
        raise ValueError("reboot source token is missing")
    fingerprint = fingerprint_for_token(token)
    if not transcript or not all(
        _allowed_reboot_command(command, token, fingerprint)
        for command, _result in transcript
    ):
        raise ValueError("reboot source transcript is malformed or unsafe")
    reboot_indices = [
        index for index, (command, _result) in enumerate(transcript) if command == "reboot"
    ]
    if len(reboot_indices) != 1:
        raise ValueError("reboot source must contain one reboot")
    reboot_index = reboot_indices[0]
    pre = transcript[:reboot_index]
    reboot_result = transcript[reboot_index][1]
    post = transcript[reboot_index + 1 :]
    if [command for command, _result in pre[:2]] != ["version", "crashlog"]:
        raise ValueError("exact version and crashlog preflight must precede all mutations")
    if not (
        source.get("schema") == 1
        and source.get("mode") == "hardware"
        and source.get("ok") is True
        and _port(source.get("port")) == _port(d1l_port)
        and source.get("baud") == 115200
        and _raw_git_clean(source, commit)
        and _exact_commit(source.get("expected_firmware_commit"), commit)
        and _exact_commit(source.get("pre_device_build_commit"), commit)
        and _exact_commit(source.get("device_build_commit"), commit)
        and source.get("firmware_identity_required") is True
        and source.get("pre_firmware_identity_ok") is True
        and source.get("post_firmware_identity_ok") is True
        and source.get("firmware_identity_ok") is True
        and source.get("persistence_clean_required") is True
        and source.get("pre_reboot_persistence_checked") is True
        and source.get("pre_reboot_persistence_clean") is True
        and source.get("pre_reboot_pending_dirty") is False
        and type(source.get("pre_reboot_persistence_poll_attempts_used")) is int
        and source.get("pre_reboot_persistence_poll_attempts_used") >= 1
        and source.get("post_reboot_persistence_checked") is True
        and source.get("post_reboot_persistence_clean") is True
        and source.get("post_reboot_pending_dirty") is False
        and type(source.get("persistence_poll_attempts_used")) is int
        and source.get("persistence_poll_attempts_used") >= 1
        and source.get("crashlog_transition_required") is True
        and source.get("crashlog_transition_ok") is True
        and source.get("timed_out_command") is None
        and source.get("unexpected_restart_before_reboot") is False
        and source.get("pre_reboot_gate_passed") is True
        and source.get("reboot_attempted") is True
        and source.get("reboot_skipped_reason") is None
        and source.get("reboot_reset_scope") == "system"
        and source.get("reboot_connectivity_prepare") == "ESP_OK"
        and source.get("public_rf_tx") is False
        and source.get("formats_sd") is False
        and not _nested_true(source, {"public_rf_tx", "formats_sd", "format_performed"})
        and _nested_code_count(source, "TIMEOUT") == 0
        and not unexpected_console_restart([result for _command, result in transcript])
    ):
        raise ValueError("reboot source failed context, safety, or transport checks")

    pre_versions = [result for command, result in pre if command == "version"]
    post_versions = [result for command, result in post if command == "version"]
    pre_crashlogs = [result for command, result in pre if command == "crashlog"]
    post_crashlogs = [result for command, result in post if command == "crashlog"]
    if not all(
        len(rows) == 1
        for rows in (pre_versions, post_versions, pre_crashlogs, post_crashlogs)
    ):
        raise ValueError("version and crashlog must straddle the reboot")
    if not all(
        row.get("ok") is True and _exact_commit(row.get("build_commit"), commit)
        for row in (pre_versions[0], post_versions[0])
    ):
        raise ValueError("reboot source device commit mismatch")
    if not crashlog_transition_passed(pre_crashlogs[0], post_crashlogs[0]):
        raise ValueError("reboot source crashlog transition failed")
    crash_before_total = _integer(pre_crashlogs[0].get("total_written"))
    crash_after_total = _integer(post_crashlogs[0].get("total_written"))
    if crash_before_total is None or crash_after_total is None:
        raise ValueError("reboot source crashlog totals are missing")

    pre_commands = [command for command, _result in pre]
    persistence_plan = persistence_readback_commands(token)
    mutating_plan = [
        "storage filecanary",
        f"storage retained-canary {token}",
        f"storage map-tile-canary {token}",
    ]
    try:
        mutation_indices = [pre_commands.index(command) for command in mutating_plan]
    except ValueError as exc:
        raise ValueError("reboot source pre-reboot mutation cohort is incomplete") from exc
    health_indices = [
        index for index, command in enumerate(pre_commands) if command == "health"
    ]
    pre_persistence_tail = (
        pre_commands[health_indices[0] + 1 :] if len(health_indices) == 1 else []
    )
    if not (
        all(pre_commands.count(command) == 1 for command in mutating_plan)
        and mutation_indices == list(
            range(mutation_indices[0], mutation_indices[0] + len(mutating_plan))
        )
        and "storage remount" in pre_commands[: mutation_indices[0]]
        and len(health_indices) == 1
        and len(pre_persistence_tail) >= len(persistence_plan)
        and len(pre_persistence_tail) % len(persistence_plan) == 0
        and all(
            pre_persistence_tail[index : index + len(persistence_plan)]
            == persistence_plan
            for index in range(0, len(pre_persistence_tail), len(persistence_plan))
        )
    ):
        raise ValueError("reboot source pre-reboot command order is invalid")
    pre_poll_cohorts = len(pre_persistence_tail) // len(persistence_plan)
    if source.get("pre_reboot_persistence_poll_attempts_used") != pre_poll_cohorts:
        raise ValueError("reboot source pre-reboot persistence attempt count is inconsistent")
    filecanary = pre[mutation_indices[0]][1]
    if not sd_filecanary_passed(filecanary):
        raise ValueError("reboot source SD file canary failed")

    final_pre_persistence = dict(pre[-len(persistence_plan) :])
    if not persistence_snapshot_clean(final_pre_persistence, token):
        raise ValueError("reboot source pre-reboot persistence remained dirty")
    if source.get("pre_reboot_persistence") != final_pre_persistence:
        raise ValueError("reboot source pre-reboot persistence receipt is inconsistent")

    post_commands = [command for command, _result in post]
    version_index = post_commands.index("version")
    crashlog_index = post_commands.index("crashlog")
    persistence_tail = post_commands[crashlog_index + 1 :]
    if not (
        crashlog_index == version_index + 1
        and post_commands[version_index - 2 : version_index]
        == [f"storage map-tile-check {token}", "health"]
        and len(persistence_tail) >= len(persistence_plan)
        and len(persistence_tail) % len(persistence_plan) == 0
        and all(
            persistence_tail[index : index + len(persistence_plan)]
            == persistence_plan
            for index in range(0, len(persistence_tail), len(persistence_plan))
        )
    ):
        raise ValueError("reboot source post-reboot evidence order is invalid")
    post_poll_cohorts = len(persistence_tail) // len(persistence_plan)
    if source.get("persistence_poll_attempts_used") != post_poll_cohorts:
        raise ValueError("reboot source post-reboot persistence attempt count is inconsistent")
    if [command for command, _result in transcript[-len(persistence_plan) :]] != persistence_plan:
        raise ValueError("reboot source final persistence cohort is incomplete")
    final_persistence = dict(transcript[-len(persistence_plan) :])
    if not persistence_snapshot_clean(final_persistence, token):
        raise ValueError("reboot source persistence remained dirty")
    if source.get("post_reboot_persistence") != final_persistence:
        raise ValueError("reboot source post-reboot persistence receipt is inconsistent")
    if not _successful_remount(pre) or not _successful_remount(post):
        raise ValueError("reboot source remount quiesce receipt is missing")

    canary_command = f"storage retained-canary {token}"
    canary_rows = [result for command, result in pre if command == canary_command]
    if len(canary_rows) != 1:
        raise ValueError("reboot source retained canary is missing")
    pre_map = _full_command_map(pre)
    post_map = _full_command_map(post)
    if not (
        readbacks_pass(pre_map, token, fingerprint, canary_rows[0])
        and readbacks_pass(post_map, token, fingerprint, canary_rows[0])
        and map_tile_write_passed(
            pre_map.get(f"storage map-tile-canary {token}"), token
        )
        and map_tile_check_passed(
            post_map.get(f"storage map-tile-check {token}"), token
        )
        and storage_ready(final_persistence.get("storage status"))
        and retained_storage_clean(final_persistence.get("storage status"))
    ):
        raise ValueError("reboot source canary or final storage readback failed")

    pre_health = [result for command, result in pre if command == "health"]
    post_health = [result for command, result in post if command == "health"]
    if not (
        len(pre_health) == 1
        and len(post_health) == 1
        and all(
            row.get("schema") == 1
            and row.get("ok") is True
            and row.get("cmd") == "health"
            for row in (pre_health[0], post_health[0])
        )
        and source.get("pre_reboot_health_ok") is True
        and source.get("health_ok") is True
        and source.get("reboot_nonce_proven") is True
        and source.get("reboot_proven") is True
        and source.get("post_reboot_reset_reason") == "SW"
    ):
        raise ValueError("reboot source health boundary is missing")
    before = _integer(pre_health[-1].get("boot_nonce"), minimum=1)
    after = _integer(post_health[-1].get("boot_nonce"), minimum=1)
    stack = _integer(post_health[-1].get("retained_task_stack_free_bytes"))
    if (
        before is None
        or after is None
        or before == after
        or post_health[-1].get("reset_reason") != "SW"
        or stack is None
        or stack < RETAINED_STACK_MIN_BYTES
        or source.get("pre_reboot_boot_nonce") != before
        or source.get("post_reboot_boot_nonce") != after
    ):
        raise ValueError("reboot nonce, reset reason, or stack margin failed")
    if not (
        reboot_result.get("ok") is True
        and reboot_result.get("rebooting") is True
        and reboot_result.get("reset_scope") == "system"
        and reboot_result.get("storage_manager_quiesced") is True
        and reboot_result.get("retained_worker_quiesced") is True
        and reboot_result.get("rp2040_bridge_quiesced") is True
        and reboot_result.get("connectivity_prepare") == "ESP_OK"
        and reboot_result.get("retained_flush") == "ESP_OK"
        and reboot_result.get("route_flush") == "ESP_OK"
    ):
        raise ValueError("reboot quiesce or flush receipt failed")

    cycle = {
        "ok": True,
        "post_reboot_reset_reason": "SW",
        "reboot_proven": True,
        "reset_scope": "system",
        "connectivity_prepare": "ESP_OK",
        "storage_manager_quiesced": True,
        "retained_worker_quiesced": True,
        "rp2040_bridge_quiesced": True,
        "retained_flush": "ESP_OK",
        "route_flush": "ESP_OK",
        "pending_dirty": False,
        "wdt": False,
        "panic": False,
        "brownout": False,
        "retained_task_stack_free_bytes": stack,
        "pre_reboot_boot_nonce": before,
        "post_reboot_boot_nonce": after,
        "crashlog_total_before": crash_before_total,
        "crashlog_total_after": crash_after_total,
    }
    return cycle, token, before, after, crash_before_total, crash_after_total


def build_retained_reboot_matrix_artifact(
    source_paths: list[Path],
    provenance_path: Path,
    *,
    root: Path,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
) -> dict:
    artifact, provenance, provenance_sha256 = _base_artifact(
        "retained_reboot_matrix",
        root=root,
        commit=commit,
        github_actions_run=github_actions_run,
        d1l_port=d1l_port,
        rp2040_port=rp2040_port,
        provenance_path=provenance_path,
    )
    cycles: list[dict] = []
    tokens: list[str] = []
    nonces: list[tuple[int, int]] = []
    crash_totals: list[tuple[int, int]] = []
    try:
        if not _within(provenance_path, root):
            raise ValueError("exact-pair provenance is outside repository root")
        if not _provenance_context_ok(
            provenance,
            provenance_sha256,
            commit=commit,
            github_actions_run=github_actions_run,
            d1l_port=d1l_port,
            rp2040_port=rp2040_port,
        ):
            raise ValueError("exact-pair provenance does not match reboot context")
        if len(source_paths) < REBOOT_MIN_CYCLES:
            raise ValueError("at least five reboot sources are required")
        resolved_seen: set[str] = set()
        receipts: list[dict] = []
        for index, path in enumerate(source_paths, start=1):
            resolved, source, source_sha256 = _source(path, root)
            normalized_path = str(resolved).lower()
            if normalized_path in resolved_seen:
                raise ValueError("reboot source paths must be unique")
            resolved_seen.add(normalized_path)
            cycle, token, before, after, crash_before, crash_after = _derive_reboot_cycle(
                source, commit=commit, d1l_port=d1l_port
            )
            cycle.update({"index": index, "source_sha256": source_sha256})
            cycles.append(cycle)
            tokens.append(token)
            nonces.append((before, after))
            crash_totals.append((crash_before, crash_after))
            receipts.append(
                {
                    "kind": "sd_reboot_remount",
                    "path": str(resolved),
                    "sha256": source_sha256,
                    "token": token,
                    "pre_reboot_boot_nonce": before,
                    "post_reboot_boot_nonce": after,
                    "crashlog_total_before": crash_before,
                    "crashlog_total_after": crash_after,
                }
            )
        if len(set(tokens)) != len(tokens) or len(set(nonces)) != len(nonces):
            raise ValueError("reboot tokens and nonce pairs must be unique")
        if any(current[0] != previous[1] for previous, current in zip(nonces, nonces[1:])):
            raise ValueError("reboot nonce chain is not consecutive")
        if any(
            current[0] != previous[1]
            for previous, current in zip(crash_totals, crash_totals[1:])
        ):
            raise ValueError("reboot crashlog chain is not consecutive")
        artifact.update(
            {
                "source_receipts": receipts,
                "cycle_count": len(cycles),
                "cycles": cycles,
                "all_cycles_passed": all(cycle.get("ok") is True for cycle in cycles),
                "dirty_recovery_failure_count": sum(
                    cycle.get("pending_dirty") is not False for cycle in cycles
                ),
                "wdt_count": sum(cycle.get("wdt") is True for cycle in cycles),
                "panic_count": sum(cycle.get("panic") is True for cycle in cycles),
                "brownout_count": sum(cycle.get("brownout") is True for cycle in cycles),
                "terminal_timeout_count": 0,
            }
        )
    except Exception as exc:
        artifact["failures"] = [str(exc)]
        artifact.setdefault("cycle_count", len(cycles))
        artifact.setdefault("cycles", cycles)
        artifact.setdefault("all_cycles_passed", False)
        artifact.setdefault("dirty_recovery_failure_count", len(cycles))
    artifact["ok"] = not artifact["failures"]
    return artifact


def build_sd_remove_reinsert_artifact(
    source_path: Path,
    provenance_path: Path,
    *,
    root: Path,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
) -> dict:
    artifact, provenance, provenance_sha256 = _base_artifact(
        "sd_remove_reinsert",
        root=root,
        commit=commit,
        github_actions_run=github_actions_run,
        d1l_port=d1l_port,
        rp2040_port=rp2040_port,
        provenance_path=provenance_path,
    )
    try:
        if not _within(provenance_path, root):
            raise ValueError("exact-pair provenance is outside repository root")
        if not _provenance_context_ok(
            provenance,
            provenance_sha256,
            commit=commit,
            github_actions_run=github_actions_run,
            d1l_port=d1l_port,
            rp2040_port=rp2040_port,
        ):
            raise ValueError("exact-pair provenance does not match removal context")
        resolved, source, source_sha256 = _source(source_path, root)
        if not (
            remove_reinsert_source_ok(source)
            and _port(source.get("port")) == _port(d1l_port)
            and source.get("baud") == 115200
            and _raw_git_clean(source, commit)
            and _exact_commit(source.get("expected_firmware_commit"), commit)
            and source.get("blocker") is None
            and not _nested_true(
                source,
                {"public_rf_tx", "dm_rf_tx", "formats_sd", "format_performed"},
            )
            and _nested_code_count(source, "TIMEOUT") == 0
        ):
            raise ValueError("remove/reinsert source failed identity or safety checks")

        raw_cycles = source.get("cycles")
        if not isinstance(raw_cycles, list) or len(raw_cycles) < 1:
            raise ValueError("remove/reinsert source has no completed cycle")
        cycles = []
        for index, raw in enumerate(raw_cycles, 1):
            before = raw.get("before_remove") if isinstance(raw, dict) else None
            after = raw.get("after_reinsert") if isinstance(raw, dict) else None
            before_generations = (
                before.get("generations") if isinstance(before, dict) else None
            )
            after_generations = (
                after.get("generations") if isinstance(after, dict) else None
            )
            if (
                not isinstance(before_generations, dict)
                or not isinstance(after_generations, dict)
                or before_generations == after_generations
            ):
                raise ValueError("remove/reinsert generation transition is missing")
            cycles.append(
                {
                    "cycle": index,
                    "ok": True,
                    "true_removal_detected": True,
                    "nvs_fallback_active": True,
                    "deterministic_remount": True,
                    "generation_changed": True,
                    "read_merge_before_overwrite": isinstance(
                        raw.get("post_reinsert_readback_before"), dict
                    ),
                    "all_stores_reconciled": True,
                    "write_before_edge": True,
                    "write_during_edge": True,
                    "write_after_edge": True,
                    "reset_storm": False,
                    "delete_or_rename_against_replacement": False,
                    "user_data_loss": False,
                }
            )
        artifact.update(
            {
                "source_receipts": [
                    {
                        "kind": "d1l_sd_remove_reinsert_source",
                        "path": str(resolved),
                        "sha256": source_sha256,
                    }
                ],
                "cycle_count": len(cycles),
                "cycles": cycles,
                "all_cycles_passed": True,
                "reset_storm_count": 0,
                "cross_media_mutation_count": 0,
                "data_loss_count": 0,
            }
        )
    except Exception as exc:
        artifact["failures"] = [str(exc)]
    artifact["ok"] = not artifact["failures"]
    return artifact


def _active_soak_segment_stats(
    report: dict,
    *,
    commit: str,
    d1l_port: str,
) -> dict:
    commands = soak_commands(sample_storage=True, sd_file_canary=True)
    samples = report.get("samples")
    summary = report.get("summary")
    version = report.get("version_preflight")
    if not (
        report.get("schema") == 1
        and report.get("mode") == "hardware"
        and report.get("ok") is True
        and _port(report.get("port")) == _port(d1l_port)
        and report.get("baud") == 115200
        and _raw_git_clean(report, commit)
        and _exact_commit(report.get("expected_firmware_commit"), commit)
        and _exact_commit(report.get("device_build_commit"), commit)
        and report.get("firmware_identity_required") is True
        and report.get("firmware_identity_ok") is True
        and report.get("preflight_failure") is None
        and report.get("preflight_commands") == ["version"]
        and isinstance(version, dict)
        and version.get("ok") is True
        and version.get("cmd") == "version"
        and _exact_commit(version.get("build_commit"), commit)
        and report.get("commands") == commands
        and report.get("sample_storage") is True
        and report.get("sd_file_canary") is True
        and report.get("allow_sd_unavailable") is False
        and report.get("active_command") is None
        and report.get("active_events") == []
        and report.get("command_retries") == 0
        and report.get("dm_rf_tx") is False
        and report.get("public_rf_tx") is False
        and report.get("formats_sd") is False
        and report.get("aborted_after_timeout") is None
        and isinstance(summary, dict)
        and summary.get("ok") is True
        and summary.get("command_failure_count") == 0
        and summary.get("command_failures") == []
        and summary.get("threshold_failures") == []
        and summary.get("command_timeout_seen") is False
        and summary.get("unexpected_console_restart_seen") is False
        and summary.get("command_retry_count") == 0
        and summary.get("command_recovered_after_retry_count") == 0
        and summary.get("command_retry_failure_count") == 0
        and isinstance(samples, list)
        and len(samples) >= 2
        and not _nested_true(report, {"public_rf_tx", "dm_rf_tx", "formats_sd", "format_performed"})
        and _nested_code_count(report, "TIMEOUT") == 0
    ):
        raise ValueError("storage-active soak segment failed identity or safety checks")

    elapsed = [_number(sample.get("elapsed_sec")) for sample in samples]
    if (
        any(value is None for value in elapsed)
        or not 0.0 <= elapsed[0] <= 5.0
        or any(
            current <= previous
            for previous, current in zip(elapsed, elapsed[1:])
            if previous is not None and current is not None
        )
        or samples[0].get("label") != "start"
        or samples[-1].get("label") != "final"
    ):
        raise ValueError("storage-active soak segment elapsed times are malformed")

    health_rows: list[dict] = []
    storage_rows: list[dict] = []
    packet_rows: list[dict] = []
    crash_rows: list[dict] = []
    for sample in samples:
        results = sample.get("results")
        if not (
            sample.get("aborted_after_timeout") is None
            and isinstance(results, list)
            and len(results) == len(commands)
            and [row.get("cmd") for row in results if isinstance(row, dict)] == commands
            and all(
                isinstance(row, dict)
                and row.get("schema") == 1
                and row.get("ok") is True
                and row.get("recovered_after_retry") is not True
                and row.get("retry_failures") in (None, [])
                for row in results
            )
        ):
            raise ValueError("storage-active soak segment command cohort failed")
        health = _sample_result(sample, "health")
        storage = _sample_result(sample, "storage status")
        packet = _sample_result(sample, "packets")
        crash = _sample_result(sample, "crashlog")
        filecanary = _sample_result(sample, "storage filecanary")
        if not all(isinstance(row, dict) for row in (health, storage, packet, crash)):
            raise ValueError("storage-active soak sample is incomplete")
        if not isinstance(filecanary, dict) or not sd_filecanary_passed(filecanary):
            raise ValueError("storage-active soak SD file canary failed")
        if not storage_ready(storage) or not file_ops_ready(storage):
            raise ValueError("storage-active soak sample is not READY_SD")
        if _retained_status_has_failures(storage) or _packet_status_has_failures(packet):
            raise ValueError("storage-active soak retained persistence failed")
        health_rows.append(health)
        storage_rows.append(storage)
        packet_rows.append(packet)
        crash_rows.append(crash)

    boot_nonces = [_integer(row.get("boot_nonce"), minimum=1) for row in health_rows]
    uptimes = [_integer(row.get("uptime_ms")) for row in health_rows]
    stacks = [_integer(row.get("retained_task_stack_free_bytes")) for row in health_rows]
    crash_totals = [_integer(row.get("total_written")) for row in crash_rows]
    if (
        any(value is None for value in boot_nonces + uptimes + stacks + crash_totals)
        or len(set(boot_nonces)) != 1
        or any(current < previous for previous, current in zip(uptimes, uptimes[1:]))
        or min(value for value in stacks if value is not None) < RETAINED_STACK_MIN_BYTES
        or len(set(crash_totals)) != 1
        or _new_crash_entries(samples)
    ):
        raise ValueError("storage-active soak segment reset, crash, or stack boundary failed")

    generations: list[int] = []
    for packet in packet_rows:
        persistence = packet.get("persistence")
        sd = persistence.get("sd") if isinstance(persistence, dict) else None
        generation = _integer(sd.get("generation")) if isinstance(sd, dict) else None
        if generation is None:
            raise ValueError("storage-active soak packet generation is missing")
        generations.append(generation)
    false_no_card_count = sum(
        sd_status(row).get("state") == "no_card"
        or sd_status(row).get("present") is False
        or sd_status(row).get("mounted") is False
        for row in storage_rows
    )
    generation_change_count = sum(
        current != previous for previous, current in zip(generations, generations[1:])
    )
    if false_no_card_count or generation_change_count:
        raise ValueError("storage-active soak segment observed a media-state change")
    return {
        "duration_sec": round(elapsed[-1] - elapsed[0], 3),
        "sample_count": len(samples),
        "status_poll_count": len(storage_rows),
        "boot_nonce": boot_nonces[0],
        "crashlog_total": crash_totals[0],
        "retained_task_stack_free_bytes_floor": min(
            value for value in stacks if value is not None
        ),
        "packet_generations": generations,
        "false_no_card_count": false_no_card_count,
        "retained_failure_count": 0,
        "command_retry_count": 0,
        "final_ready_sd": True,
    }


def _active_soak_canary_safety(report: dict, token: str) -> bool:
    transcript = _command_result_transcript(report)
    if transcript is None:
        return False
    rows = [
        result
        for command, result in transcript
        if command == f"storage retained-canary {token}"
    ]
    return bool(
        len(rows) == 1
        and report.get("dm_rf_tx") is False
        and rows[0].get("public_rf_tx") is False
        and rows[0].get("dm_rf_tx") is False
        and rows[0].get("formats_sd") is False
    )


def build_storage_active_soak_artifact(
    source_path: Path,
    provenance_path: Path,
    *,
    root: Path,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
) -> dict:
    artifact, provenance, provenance_sha256 = _base_artifact(
        "storage_active_soak",
        root=root,
        commit=commit,
        github_actions_run=github_actions_run,
        d1l_port=d1l_port,
        rp2040_port=rp2040_port,
        provenance_path=provenance_path,
    )
    segments: list[dict] = []
    cycles: list[dict] = []
    try:
        if not _within(provenance_path, root):
            raise ValueError("exact-pair provenance is outside repository root")
        if not _provenance_context_ok(
            provenance,
            provenance_sha256,
            commit=commit,
            github_actions_run=github_actions_run,
            d1l_port=d1l_port,
            rp2040_port=rp2040_port,
        ):
            raise ValueError("exact-pair provenance does not match storage-active context")
        resolved, source, source_sha256 = _source(source_path, root)
        events = source.get("events")
        segment_count = _integer(source.get("segment_count"), minimum=2)
        if not (
            source.get("schema") == 1
            and source.get("kind") == "d1l_storage_active_soak_source"
            and source.get("mode") == "hardware"
            and source.get("ok") is True
            and _port(source.get("port")) == _port(d1l_port)
            and source.get("baud") == 115200
            and _raw_git_clean(source, commit)
            and _exact_commit(source.get("expected_firmware_commit"), commit)
            and segment_count is not None
            and source.get("failure") is None
            and source.get("public_rf_tx") is False
            and source.get("dm_rf_tx") is False
            and source.get("formats_sd") is False
            and isinstance(events, list)
            and len(events) == segment_count * 2 - 1
            and source.get("event_topology")
            == ["segment" if index % 2 == 0 else "reboot" for index in range(len(events))]
            and not _nested_true(source, {"public_rf_tx", "dm_rf_tx", "formats_sd", "format_performed"})
            and _nested_code_count(source, "TIMEOUT") == 0
        ):
            raise ValueError("storage-active source failed context or safety checks")

        reboot_tokens: list[str] = []
        for event_index, event in enumerate(events):
            if not isinstance(event, dict) or not isinstance(event.get("report"), dict):
                raise ValueError("storage-active source event is malformed")
            expected_kind = "segment" if event_index % 2 == 0 else "reboot"
            expected_index = event_index // 2 + 1
            if event.get("kind") != expected_kind or event.get("index") != expected_index:
                raise ValueError("storage-active source event topology is inconsistent")
            report = event["report"]
            if expected_kind == "segment":
                stats = _active_soak_segment_stats(
                    report, commit=commit, d1l_port=d1l_port
                )
                stats.update({"index": expected_index, "ok": True})
                segments.append(stats)
                continue

            token = event.get("token")
            if not isinstance(token, str) or not token or token in reboot_tokens:
                raise ValueError("storage-active reboot token is missing or duplicated")
            if event.get("canary_safety_explicit") is not True:
                raise ValueError("storage-active reboot lacks an explicit canary safety receipt")
            cycle, derived_token, before, after, crash_before, crash_after = _derive_reboot_cycle(
                report, commit=commit, d1l_port=d1l_port
            )
            if token != derived_token or not _active_soak_canary_safety(report, token):
                raise ValueError("storage-active retained canary is unsafe or inconsistent")
            cycle.update({"index": expected_index, "token": token})
            cycles.append(cycle)
            reboot_tokens.append(token)
            previous_segment = segments[-1]
            if (
                previous_segment["boot_nonce"] != before
                or previous_segment["crashlog_total"] != crash_before
            ):
                raise ValueError("storage-active pre-reboot segment boundary is discontinuous")
            cycle["pre_reboot_boot_nonce"] = before
            cycle["post_reboot_boot_nonce"] = after
            cycle["crashlog_total_before"] = crash_before
            cycle["crashlog_total_after"] = crash_after

        if len(segments) != segment_count or len(cycles) != segment_count - 1:
            raise ValueError("storage-active source did not complete every segment and reboot")
        for index, cycle in enumerate(cycles):
            following = segments[index + 1]
            if (
                following["boot_nonce"] != cycle["post_reboot_boot_nonce"]
                or following["crashlog_total"] != cycle["crashlog_total_after"]
            ):
                raise ValueError("storage-active post-reboot segment boundary is discontinuous")

        duration_sec = round(sum(segment["duration_sec"] for segment in segments), 3)
        controlled_reboot_generation_transition_count = sum(
            previous["packet_generations"][-1]
            != current["packet_generations"][0]
            for previous, current in zip(segments, segments[1:])
        )
        stack_floor = min(
            [segment["retained_task_stack_free_bytes_floor"] for segment in segments]
            + [cycle["retained_task_stack_free_bytes"] for cycle in cycles]
        )
        reboot_count = len(cycles)
        if (
            duration_sec < STORAGE_ACTIVE_SOAK_MIN_SECONDS
            or reboot_count < 1
            or stack_floor < RETAINED_STACK_MIN_BYTES
        ):
            raise ValueError("storage-active source did not satisfy canonical thresholds")
        artifact.update(
            {
                "source_receipts": [
                    {
                        "kind": "d1l_storage_active_soak_source",
                        "path": str(resolved),
                        "sha256": source_sha256,
                    }
                ],
                "duration_sec": duration_sec,
                "segment_count": len(segments),
                "segments": segments,
                "sample_count": sum(segment["sample_count"] for segment in segments),
                "status_poll_count": sum(
                    segment["status_poll_count"] for segment in segments
                ),
                "storage_active": True,
                "dirty_event_counts": {
                    name: reboot_count for name in ("public", "dm", "routes", "packets")
                },
                "controlled_reboot_count": reboot_count,
                "controlled_reboots": cycles,
                "all_segments_passed": True,
                "all_controlled_reboots_passed": True,
                "final_ready_sd": segments[-1]["final_ready_sd"],
                "retained_task_stack_free_bytes_floor": stack_floor,
                "false_no_card_count": sum(
                    segment["false_no_card_count"] for segment in segments
                ),
                # The backend generation is RAM-only and may restart at a new
                # positive value after each proven system reboot. Each segment
                # is already rejected if that generation changes while the
                # device remains booted; cross-reboot transitions are expected
                # topology, not a media edge.
                "unintended_backend_generation_count": 0,
                "controlled_reboot_backend_generation_transition_count": (
                    controlled_reboot_generation_transition_count
                ),
                "retained_failure_count": sum(
                    segment["retained_failure_count"] for segment in segments
                ),
                "command_retry_count": sum(
                    segment["command_retry_count"] for segment in segments
                ),
                "unexpected_reset_count": 0,
                "wdt_count": sum(cycle["wdt"] is True for cycle in cycles),
                "panic_count": sum(cycle["panic"] is True for cycle in cycles),
                "brownout_count": sum(cycle["brownout"] is True for cycle in cycles),
                "terminal_timeout_count": 0,
            }
        )
    except Exception as exc:
        artifact["failures"] = [str(exc)]
        artifact.setdefault("segment_count", len(segments))
        artifact.setdefault("segments", segments)
        artifact.setdefault("controlled_reboot_count", len(cycles))
        artifact.setdefault("controlled_reboots", cycles)
    artifact["ok"] = not artifact["failures"]
    return artifact


def rebuild_source_bound_artifact(
    data: dict,
    *,
    root: Path,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
) -> dict | None:
    kind = data.get("kind")
    provenance_receipt = data.get("provenance_receipt")
    source_receipts = data.get("source_receipts")
    if (
        kind not in SOURCE_BOUND_KINDS
        or not isinstance(provenance_receipt, dict)
        or not isinstance(provenance_receipt.get("path"), str)
        or not isinstance(source_receipts, list)
        or not source_receipts
        or not all(
            isinstance(receipt, dict) and isinstance(receipt.get("path"), str)
            for receipt in source_receipts
        )
    ):
        return None
    provenance_path = Path(provenance_receipt["path"])
    source_paths = [Path(receipt["path"]) for receipt in source_receipts]
    if kind == "sd_inserted_stability" and len(source_paths) == 1:
        return build_sd_inserted_stability_artifact(
            source_paths[0],
            provenance_path,
            root=root,
            commit=commit,
            github_actions_run=github_actions_run,
            d1l_port=d1l_port,
            rp2040_port=rp2040_port,
        )
    if kind == "sd_remove_reinsert" and len(source_paths) == 1:
        return build_sd_remove_reinsert_artifact(
            source_paths[0],
            provenance_path,
            root=root,
            commit=commit,
            github_actions_run=github_actions_run,
            d1l_port=d1l_port,
            rp2040_port=rp2040_port,
        )
    if kind == "retained_reboot_matrix":
        return build_retained_reboot_matrix_artifact(
            source_paths,
            provenance_path,
            root=root,
            commit=commit,
            github_actions_run=github_actions_run,
            d1l_port=d1l_port,
            rp2040_port=rp2040_port,
        )
    if kind == "storage_active_soak" and len(source_paths) == 1:
        return build_storage_active_soak_artifact(
            source_paths[0],
            provenance_path,
            root=root,
            commit=commit,
            github_actions_run=github_actions_run,
            d1l_port=d1l_port,
            rp2040_port=rp2040_port,
        )
    return None
