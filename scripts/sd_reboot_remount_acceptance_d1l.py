#!/usr/bin/env python3
"""Serial-only SD reboot/remount acceptance runner for MeshCore DeskOS D1L."""

from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
import os
import re
import time
from datetime import datetime, timezone
from pathlib import Path

try:
    from artifact_metadata import stamp_report
    from d1l_serial_target import POSIX_D1L_TARGET
    from smoke_d1l import (
        boot_transition_proven,
        exact_commit,
        firmware_identity_matches,
        health_boot_nonce,
        open_d1l_serial,
        reboot_command_passed,
        send_console_command,
        timeout_for_reboot_command,
        wait_for_console_ready,
    )
    from sd_retained_history_acceptance_d1l import (
        EXPECTED_REBOOT_BOOT_HELP_FIELD,
        expected_reboot_boot_help,
        fingerprint_for_token,
        mark_expected_reboot_boot_help,
        readbacks_pass,
        result_has_boot_help,
        semantic_storage_copy,
        retained_canary_hash,
        retained_canary_metadata,
        retained_storage_clean,
        retained_storage_ready,
        retained_readback_commands,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import stamp_report
    from scripts.d1l_serial_target import POSIX_D1L_TARGET
    from scripts.smoke_d1l import (
        boot_transition_proven,
        exact_commit,
        firmware_identity_matches,
        health_boot_nonce,
        open_d1l_serial,
        reboot_command_passed,
        send_console_command,
        timeout_for_reboot_command,
        wait_for_console_ready,
    )
    from scripts.sd_retained_history_acceptance_d1l import (
        EXPECTED_REBOOT_BOOT_HELP_FIELD,
        expected_reboot_boot_help,
        fingerprint_for_token,
        mark_expected_reboot_boot_help,
        readbacks_pass,
        result_has_boot_help,
        semantic_storage_copy,
        retained_canary_hash,
        retained_canary_metadata,
        retained_storage_clean,
        retained_storage_ready,
        retained_readback_commands,
    )


TOKEN_RE = re.compile(r"^[A-Za-z0-9_.-]{1,31}$")
MOUNT_POLL_ATTEMPTS = 10
MOUNT_POLL_INTERVAL_SECONDS = 2.0
SD_OPERATION_MIN_TIMEOUT_SECONDS = 120.0
RETAINED_CANARY_MIN_TIMEOUT_SECONDS = 180.0
REMOUNT_COMMAND_MIN_TIMEOUT_SECONDS = 75.0
PERSISTENCE_POLL_ATTEMPTS = 30
PERSISTENCE_POLL_INTERVAL_SECONDS = 1.0
POSIX_SERIAL_STARTUP_SETTLE_SECONDS = 10.0
POSIX_SERIAL_READY_TIMEOUT_SECONDS = 12.0


class SerialContextProvider:
    """Reuse one Pi serial handle across the commanded reboot."""

    def __init__(
        self,
        serial_module,
        *,
        port: str,
        baud: int,
        timeout: float,
    ) -> None:
        self.serial_module = serial_module
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.reuse = port == POSIX_D1L_TARGET
        self.serial_handle = None
        self.physical_open_count = 0
        self.startup_health: dict = {}

    def context(self):
        if not self.reuse:
            self.physical_open_count += 1
            return open_d1l_serial(
                self.serial_module,
                port=self.port,
                baudrate=self.baud,
                timeout=self.timeout,
            )
        if self.serial_handle is None:
            self.serial_handle = open_d1l_serial(
                self.serial_module,
                port=self.port,
                baudrate=self.baud,
                timeout=self.timeout,
            )
            self.physical_open_count += 1
            time.sleep(POSIX_SERIAL_STARTUP_SETTLE_SECONDS)
            self.startup_health = wait_for_console_ready(
                self.serial_handle,
                POSIX_SERIAL_READY_TIMEOUT_SECONDS,
                attempts=3,
            )
        return nullcontext(self.serial_handle)

    def close(self) -> None:
        if self.serial_handle is not None:
            self.serial_handle.close()
            self.serial_handle = None


def persistence_readback_commands(token: str) -> list[str]:
    fingerprint = fingerprint_for_token(token)
    return [
        f"messages public search {token}",
        f"messages dm {fingerprint}",
        "routes",
        f"packets search {token}",
        "storage status",
    ]


def command_plan(
    token: str,
    *,
    include_reboot: bool = True,
    strict_evidence: bool = False,
) -> list[str]:
    commands: list[str] = []
    if strict_evidence:
        commands.extend(["version", "crashlog"])
    commands.extend(
        [
        "storage status",
        "storage remount",
        "storage status",
        "storage filecanary",
        f"storage retained-canary {token}",
        f"storage map-tile-canary {token}",
        *retained_readback_commands(token),
        "storage status",
        ]
    )
    if include_reboot:
        commands.append("health")
        if strict_evidence:
            commands.extend(persistence_readback_commands(token))
        commands.extend(
            [
                "reboot",
                "storage status",
                "storage remount",
                "storage status",
                *retained_readback_commands(token),
                f"storage map-tile-check {token}",
                "health",
            ]
        )
        if strict_evidence:
            commands.extend(["version", "crashlog", *persistence_readback_commands(token)])
    else:
        commands.append("health")
    return commands


def dry_run_report(
    token: str,
    *,
    include_reboot: bool = True,
    expected_firmware_commit: str | None = None,
) -> dict:
    strict_evidence = expected_firmware_commit is not None
    return {
        "schema": 1,
        "mode": "dry-run",
        "hardware_required": False,
        "token": token,
        "fingerprint": fingerprint_for_token(token),
        "commands": command_plan(
            token,
            include_reboot=include_reboot,
            strict_evidence=strict_evidence,
        ),
        "expected_firmware_commit": expected_firmware_commit,
        "firmware_identity_required": strict_evidence,
        "persistence_clean_required": strict_evidence and include_reboot,
        "crashlog_transition_required": strict_evidence and include_reboot,
        "public_rf_tx": False,
        "formats_sd": False,
        "ok": True,
    }


def sd_status(result: dict | None) -> dict:
    if not isinstance(result, dict):
        return {}
    sd = result.get("sd")
    return sd if isinstance(sd, dict) else {}


def sd_state(result: dict | None) -> str | None:
    state = sd_status(result).get("state")
    return state if isinstance(state, str) else None


def manager_status(result: dict | None) -> dict:
    if not isinstance(result, dict):
        return {}
    manager = result.get("manager")
    return manager if isinstance(manager, dict) else {}


def storage_manager_ready(result: dict | None) -> bool:
    manager = manager_status(result)
    return manager.get("running") is True and manager.get("state") == "READY_SD"


def storage_ready(result: dict | None) -> bool:
    sd = sd_status(result)
    return (
        isinstance(result, dict)
        and result.get("ok") is True
        and storage_manager_ready(result)
        and sd.get("state") == "ready"
        and sd.get("filesystem") == "fat32"
        and sd.get("present") is True
        and sd.get("mounted") is True
        and sd.get("data_root_ready") is True
        and sd.get("file_ops") is True
        and sd.get("atomic_rename") is True
    )


def _zero_int(value: object) -> bool:
    return type(value) is int and value == 0


def _message_persistence_clean(result: dict | None, expected_cmd: str) -> bool:
    if not isinstance(result, dict):
        return False
    persistence = result.get("persistence")
    sd = persistence.get("sd") if isinstance(persistence, dict) else None
    nvs = persistence.get("nvs") if isinstance(persistence, dict) else None
    return (
        result.get("ok") is True
        and result.get("cmd") == expected_cmd
        and result.get("persisted") is True
        and isinstance(persistence, dict)
        and persistence.get("loaded") is True
        and persistence.get("dirty") is False
        and _zero_int(persistence.get("failures"))
        and isinstance(sd, dict)
        and sd.get("required") is True
        and sd.get("dirty") is False
        and sd.get("reconcile_pending") is False
        and _zero_int(sd.get("failures"))
        and sd.get("last_error") == "ESP_OK"
        and isinstance(nvs, dict)
        and nvs.get("dirty") is False
        and _zero_int(nvs.get("failures"))
        and nvs.get("last_error") == "ESP_OK"
    )


def _route_persistence_clean(result: dict | None) -> bool:
    if not isinstance(result, dict):
        return False
    persistence = result.get("persistence")
    sd = persistence.get("sd_primary") if isinstance(persistence, dict) else None
    nvs = persistence.get("nvs_fallback") if isinstance(persistence, dict) else None
    return (
        result.get("ok") is True
        and result.get("cmd") == "routes"
        and result.get("persisted") is True
        and isinstance(persistence, dict)
        and persistence.get("dirty") is False
        and _zero_int(persistence.get("fail_count"))
        and persistence.get("clear_failure_latched") is False
        and _zero_int(persistence.get("clear_fail_count"))
        and persistence.get("clear_last_error") == "ESP_OK"
        and isinstance(sd, dict)
        and sd.get("required") is True
        and sd.get("dirty") is False
        and sd.get("reconcile_pending") is False
        and _zero_int(sd.get("fail_count"))
        and sd.get("last_error") == "ESP_OK"
        and isinstance(nvs, dict)
        and nvs.get("dirty") is False
        and _zero_int(nvs.get("fail_count"))
        and nvs.get("last_error") == "ESP_OK"
    )


def _packet_persistence_clean(result: dict | None) -> bool:
    if not isinstance(result, dict):
        return False
    persistence = result.get("persistence")
    reconcile = persistence.get("reconcile") if isinstance(persistence, dict) else None
    sd = persistence.get("sd") if isinstance(persistence, dict) else None
    nvs = persistence.get("nvs") if isinstance(persistence, dict) else None
    journal = persistence.get("journal") if isinstance(persistence, dict) else None
    return (
        result.get("ok") is True
        and result.get("cmd") == "packets search"
        and result.get("persisted") is True
        and isinstance(persistence, dict)
        and persistence.get("loaded") is True
        and persistence.get("dirty") is False
        and _zero_int(persistence.get("failures"))
        and isinstance(reconcile, dict)
        and reconcile.get("pending") is False
        and _zero_int(reconcile.get("failures"))
        and reconcile.get("last_error") == "ESP_OK"
        and isinstance(sd, dict)
        and sd.get("required") is True
        and sd.get("dirty") is False
        and _zero_int(sd.get("failures"))
        and sd.get("last_error") == "ESP_OK"
        and isinstance(nvs, dict)
        and nvs.get("dirty") is False
        and _zero_int(nvs.get("failures"))
        and nvs.get("last_error") == "ESP_OK"
        and isinstance(journal, dict)
        and journal.get("dirty") is False
        and _zero_int(journal.get("failures"))
        and journal.get("last_error") == "ESP_OK"
    )


def persistence_snapshot_clean(results: dict[str, dict], token: str) -> bool:
    fingerprint = fingerprint_for_token(token)
    return (
        _message_persistence_clean(
            results.get(f"messages public search {token}"), "messages public"
        )
        and _message_persistence_clean(
            results.get(f"messages dm {fingerprint}"), "messages dm"
        )
        and _route_persistence_clean(results.get("routes"))
        and _packet_persistence_clean(results.get(f"packets search {token}"))
        and retained_storage_clean(results.get("storage status"))
    )


def crashlog_transition_passed(before: dict | None, after: dict | None) -> bool:
    if not isinstance(before, dict) or not isinstance(after, dict):
        return False
    before_total = before.get("total_written")
    after_total = after.get("total_written")
    before_entries = before.get("entries")
    after_entries = after.get("entries")
    if (
        before.get("ok") is not True
        or before.get("cmd") != "crashlog"
        or after.get("ok") is not True
        or after.get("cmd") != "crashlog"
        or type(before_total) is not int
        or type(after_total) is not int
        or after_total != before_total + 1
        or not isinstance(before_entries, list)
        or not isinstance(after_entries, list)
    ):
        return False
    before_sequences = [
        entry.get("seq")
        for entry in before_entries
        if isinstance(entry, dict) and type(entry.get("seq")) is int
    ]
    previous_max = max(before_sequences, default=0)
    new_entries = [
        entry
        for entry in after_entries
        if isinstance(entry, dict)
        and type(entry.get("seq")) is int
        and entry.get("seq") > previous_max
    ]
    return (
        len(new_entries) == 1
        and new_entries[0].get("reset_reason") == "SW"
        and new_entries[0].get("crash_like") is False
    )


def remount_manager_busy(result: dict | None) -> bool:
    manager = manager_status(result)
    return (
        isinstance(result, dict)
        and result.get("cmd") == "storage remount"
        and result.get("ok") is False
        and result.get("code") == "ESP_ERR_INVALID_STATE"
        and manager.get("running") is True
    )


def remount_transition_passed(result: dict | None, storage_after: dict | None) -> bool:
    if not isinstance(result, dict) or result.get("cmd") != "storage remount":
        return False
    return (
        result.get("ok") is True
        and result.get("retained_worker_quiesce_acquired") is True
        and storage_ready(storage_after)
    )


def canary_passed(result: dict | None) -> bool:
    return isinstance(result, dict) and result.get("ok") is True


def semantic_reboot_skip_reason(
    *,
    include_reboot: bool,
    timeout_command: str | None,
    unexpected_restart: bool,
    pre_mount_complete: bool,
    pre_sequence_complete: bool,
    filecanary_ok: bool,
    retained_canary_ok: bool,
    map_tile_canary_ok: bool,
    pre_readbacks_ok: bool,
    retained_storage_clean_after_canary: bool,
    pre_health_ok: bool,
) -> str | None:
    if not include_reboot:
        return "reboot_disabled"
    if timeout_command is not None:
        return "pre_reboot_command_timeout"
    if unexpected_restart:
        return "unexpected_restart_before_reboot"
    if not pre_mount_complete:
        return "pre_remount_failed"
    if not pre_sequence_complete:
        return "pre_reboot_sequence_incomplete"
    if not filecanary_ok:
        return "filecanary_failed"
    if not retained_canary_ok:
        return "retained_canary_failed"
    if not map_tile_canary_ok:
        return "map_tile_canary_failed"
    if not pre_readbacks_ok:
        return "pre_reboot_readbacks_failed"
    if not retained_storage_clean_after_canary:
        return "post_canary_retained_storage_not_clean"
    if not pre_health_ok:
        return "pre_reboot_health_failed"
    return None


def map_tile_write_passed(result: dict | None, token: str) -> bool:
    if not isinstance(result, dict):
        return False
    return (
        result.get("ok") is True
        and result.get("token") == token
        and result.get("rename_replace") is True
        and result.get("read_final") is True
        and result.get("public_rf_tx") is False
        and result.get("formats_sd") is False
    )


def map_tile_check_passed(result: dict | None, token: str) -> bool:
    if not isinstance(result, dict):
        return False
    return (
        result.get("ok") is True
        and result.get("token") == token
        and result.get("stat_final") is True
        and result.get("read_final") is True
        and result.get("public_rf_tx") is False
        and result.get("formats_sd") is False
    )


def any_flag(results: list[dict], flag: str) -> bool:
    return any(result.get(flag) is True for result in results)


def host_timed_out(result: dict | None) -> bool:
    return isinstance(result, dict) and result.get("code") == "TIMEOUT"


def unexpected_console_restart(results: list[dict]) -> bool:
    return any(
        result_has_boot_help(result) and not expected_reboot_boot_help(result)
        for result in results
    )


def sequence_completed(
    commands: list[str], results: list[dict], expected_commands: list[str]
) -> bool:
    return (
        commands == expected_commands
        and len(results) == len(expected_commands)
        and not any(host_timed_out(result) for result in results)
        and not unexpected_console_restart(results)
    )


def run_command(ser, command: str, timeout: float) -> dict:
    if command.startswith("mesh send public"):
        raise RuntimeError(f"refusing destructive/RF command in SD remount acceptance: {command}")
    command_timeout = timeout_for_reboot_command(command, timeout)
    if command.startswith("storage retained-canary "):
        command_timeout = max(timeout, RETAINED_CANARY_MIN_TIMEOUT_SECONDS)
    elif command == "storage remount":
        command_timeout = max(timeout, REMOUNT_COMMAND_MIN_TIMEOUT_SECONDS)
    elif (
        command in {"storage mount", "storage filecanary"}
        or command.startswith("storage map-tile-")
    ):
        command_timeout = max(timeout, SD_OPERATION_MIN_TIMEOUT_SECONDS)
    result = send_console_command(ser, command, command_timeout)
    result.pop(EXPECTED_REBOOT_BOOT_HELP_FIELD, None)
    return result


def run_commands_until_terminal(
    ser,
    planned_commands: list[str],
    timeout: float,
    *,
    allow_initial_reboot_boot_help: bool = False,
) -> tuple[list[str], list[dict]]:
    commands: list[str] = []
    results: list[dict] = []
    for command in planned_commands:
        commands.append(command)
        result = run_command(ser, command, timeout)
        if not results and allow_initial_reboot_boot_help:
            mark_expected_reboot_boot_help(result)
        results.append(result)
        # The console task may still be executing a host-timed-out command.
        # Queuing anything else can shift JSON replies and turn one slow SD
        # operation into a destructive or misleading command sequence.
        if host_timed_out(result) or unexpected_console_restart([result]):
            break
    return commands, results


def run_persistence_poll(
    ser,
    *,
    token: str,
    timeout: float,
    attempts: int,
    interval_sec: float,
) -> tuple[list[str], list[dict], dict[str, dict], bool, int]:
    commands: list[str] = []
    results: list[dict] = []
    latest: dict[str, dict] = {}
    attempts_used = 0
    plan = persistence_readback_commands(token)
    for attempt in range(max(1, attempts)):
        attempts_used = attempt + 1
        poll_commands, poll_results = run_commands_until_terminal(ser, plan, timeout)
        commands.extend(poll_commands)
        results.extend(poll_results)
        latest = dict(zip(poll_commands, poll_results))
        if sequence_completed(poll_commands, poll_results, plan) and persistence_snapshot_clean(
            latest, token
        ):
            return commands, results, latest, True, attempts_used
        if len(poll_commands) != len(plan) or any(
            host_timed_out(result) or unexpected_console_restart([result])
            for result in poll_results
        ):
            break
        if attempt + 1 < max(1, attempts):
            time.sleep(interval_sec)
    return commands, results, latest, False, attempts_used


def persistence_poll_snapshot_checked(latest: dict[str, dict], token: str) -> bool:
    plan = persistence_readback_commands(token)
    if set(latest) != set(plan):
        return False
    results = [latest[command] for command in plan]
    return not any(host_timed_out(result) for result in results) and not unexpected_console_restart(
        results
    )


def run_pre_data_sequence(
    ser,
    *,
    token: str,
    fingerprint: str,
    timeout: float,
    include_health: bool,
) -> tuple[list[str], list[dict]]:
    commands: list[str] = []
    results: list[dict] = []
    mutating_stages = (
        ("storage filecanary", canary_passed),
        (
            f"storage retained-canary {token}",
            lambda result: retained_canary_metadata(
                result, token, fingerprint
            ) is not None,
        ),
        (
            f"storage map-tile-canary {token}",
            lambda result: map_tile_write_passed(result, token),
        ),
    )

    for command, stage_passed in mutating_stages:
        stage_commands, stage_results = run_commands_until_terminal(
            ser, [command], timeout
        )
        commands.extend(stage_commands)
        results.extend(stage_results)
        if not stage_results:
            return commands, results
        result = stage_results[-1]
        if host_timed_out(result) or unexpected_console_restart([result]):
            return commands, results
        if not stage_passed(result):
            break

    diagnostic_plan = [*retained_readback_commands(token), "storage status"]
    if include_health:
        diagnostic_plan.append("health")
    diagnostic_commands, diagnostic_results = run_commands_until_terminal(
        ser, diagnostic_plan, timeout
    )
    commands.extend(diagnostic_commands)
    results.extend(diagnostic_results)
    return commands, results


def run_mount_sequence(
    ser,
    *,
    timeout: float,
    mount_poll_attempts: int,
    mount_poll_interval_sec: float,
    allow_initial_reboot_boot_help: bool = False,
) -> tuple[list[str], list[dict], dict]:
    initial_commands = ["storage status", "storage remount", "storage status"]
    commands, results = run_commands_until_terminal(
        ser,
        initial_commands,
        timeout,
        allow_initial_reboot_boot_help=allow_initial_reboot_boot_help,
    )
    storage_after = results[-1] if results else {}
    if not sequence_completed(commands, results, initial_commands):
        return commands, results, storage_after

    # A remount can race the boot-time storage manager. A busy receipt is only
    # diagnostic: require a later status sample, then retry once and require
    # the retry to prove retained-worker quiesce before any data command.
    initial_remount_busy = remount_manager_busy(results[1])
    require_post_busy_poll = initial_remount_busy
    for _attempt in range(mount_poll_attempts):
        if storage_ready(storage_after) and not require_post_busy_poll:
            break
        time.sleep(mount_poll_interval_sec)
        commands.append("storage status")
        storage_after = run_command(ser, "storage status", timeout)
        results.append(storage_after)
        if host_timed_out(storage_after) or unexpected_console_restart([storage_after]):
            break
        require_post_busy_poll = False

    if (
        initial_remount_busy
        and not require_post_busy_poll
        and storage_ready(storage_after)
        and not host_timed_out(storage_after)
        and not unexpected_console_restart([storage_after])
    ):
        retry_commands = ["storage remount", "storage status"]
        retry_ran, retry_results = run_commands_until_terminal(
            ser, retry_commands, timeout
        )
        commands.extend(retry_ran)
        results.extend(retry_results)
        storage_after = retry_results[-1] if retry_results else storage_after
        if sequence_completed(retry_ran, retry_results, retry_commands):
            for _attempt in range(mount_poll_attempts):
                if storage_ready(storage_after):
                    break
                time.sleep(mount_poll_interval_sec)
                commands.append("storage status")
                storage_after = run_command(ser, "storage status", timeout)
                results.append(storage_after)
                if host_timed_out(storage_after) or unexpected_console_restart([storage_after]):
                    break
    return commands, results, storage_after


def run_acceptance(
    port: str,
    baud: int,
    timeout: float,
    token: str,
    *,
    include_reboot: bool,
    reboot_settle_sec: float,
    mount_poll_attempts: int,
    mount_poll_interval_sec: float,
    expected_firmware_commit: str | None = None,
    persistence_poll_attempts: int = PERSISTENCE_POLL_ATTEMPTS,
    persistence_poll_interval_sec: float = PERSISTENCE_POLL_INTERVAL_SECONDS,
) -> dict:
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("pyserial is required: python -m pip install pyserial") from exc

    serial_provider = SerialContextProvider(
        serial,
        port=port,
        baud=baud,
        timeout=timeout,
    )
    fingerprint = fingerprint_for_token(token)
    strict_evidence = expected_firmware_commit is not None
    results: list[dict] = []
    commands: list[str] = []
    preflight_commands: list[str] = []
    preflight_results: list[dict] = []
    pre_data_commands: list[str] = []
    pre_data_results: list[dict] = []
    pre_persistence_commands: list[str] = []
    pre_persistence_results: list[dict] = []
    pre_persistence_by_command: dict[str, dict] = {}
    pre_persistence_clean = not (strict_evidence and include_reboot)
    pre_persistence_poll_attempts_used = 0
    pre_mount_commands: list[str] = []
    pre_mount_results: list[dict] = []
    pre_storage: dict = {}
    with serial_provider.context() as ser:
        ser.reset_input_buffer()
        if strict_evidence:
            preflight_plan = ["version", "crashlog"]
            preflight_commands, preflight_results = run_commands_until_terminal(
                ser, preflight_plan, timeout
            )
            commands.extend(preflight_commands)
            results.extend(preflight_results)
        preflight_complete = (
            not strict_evidence
            or sequence_completed(
                preflight_commands,
                preflight_results,
                ["version", "crashlog"],
            )
        )
        preflight_by_command = dict(zip(preflight_commands, preflight_results))
        pre_version = preflight_by_command.get("version", {})
        pre_firmware_identity_ok = (
            firmware_identity_matches(pre_version, expected_firmware_commit)
            if strict_evidence and expected_firmware_commit is not None
            else None
        )
        if preflight_complete and pre_firmware_identity_ok is not False:
            pre_mount_commands, pre_mount_results, pre_storage = run_mount_sequence(
                ser,
                timeout=timeout,
                mount_poll_attempts=mount_poll_attempts,
                mount_poll_interval_sec=mount_poll_interval_sec,
            )
            commands.extend(pre_mount_commands)
            results.extend(pre_mount_results)
        pre_remount_results = [
            result
            for result in pre_mount_results
            if result.get("cmd") == "storage remount"
        ]
        pre_remount_result = pre_remount_results[-1] if pre_remount_results else {}
        pre_remount_busy = any(
            remount_manager_busy(result) for result in pre_mount_results
        )
        pre_remount_retained_worker_quiesce_acquired = (
            pre_remount_result.get("retained_worker_quiesce_acquired")
            if isinstance(pre_remount_result, dict)
            and pre_remount_result.get("ok") is True
            else None
        )
        pre_mount_transport_complete = (
            len(pre_mount_commands) >= 3
            and not any(host_timed_out(result) for result in pre_mount_results)
            and not unexpected_console_restart(pre_mount_results)
        )
        pre_remount_command_passed = (
            pre_mount_transport_complete
            and remount_transition_passed(pre_remount_result, pre_storage)
        )
        pre_mount_complete = (
            pre_mount_transport_complete
            and pre_remount_command_passed
            and storage_ready(pre_storage)
        )
        if (
            pre_mount_complete
            and preflight_complete
            and pre_firmware_identity_ok is not False
        ):
            pre_data_commands, pre_data_results = run_pre_data_sequence(
                ser,
                token=token,
                fingerprint=fingerprint,
                timeout=timeout,
                include_health=include_reboot,
            )
            commands.extend(pre_data_commands)
            results.extend(pre_data_results)
            pre_data_by_command = dict(zip(pre_data_commands, pre_data_results))
            pre_data_transport_complete = sequence_completed(
                pre_data_commands, pre_data_results, pre_data_commands
            )
            retained_result_for_poll = pre_data_by_command.get(
                f"storage retained-canary {token}", {}
            )
            pre_poll_eligible = (
                strict_evidence
                and include_reboot
                and pre_data_transport_complete
                and not unexpected_console_restart(
                    [*pre_mount_results, *preflight_results, *pre_data_results]
                )
                and canary_passed(
                    pre_data_by_command.get("storage filecanary", {})
                )
                and retained_canary_metadata(
                    retained_result_for_poll, token, fingerprint
                )
                is not None
                and map_tile_write_passed(
                    pre_data_by_command.get(
                        f"storage map-tile-canary {token}", {}
                    ),
                    token,
                )
                and readbacks_pass(
                    pre_data_by_command,
                    token,
                    fingerprint,
                    retained_result_for_poll,
                )
                and pre_data_by_command.get("health", {}).get("ok") is True
            )
            if pre_poll_eligible:
                (
                    pre_persistence_commands,
                    pre_persistence_results,
                    pre_persistence_by_command,
                    pre_persistence_clean,
                    pre_persistence_poll_attempts_used,
                ) = run_persistence_poll(
                    ser,
                    token=token,
                    timeout=timeout,
                    attempts=persistence_poll_attempts,
                    interval_sec=persistence_poll_interval_sec,
                )
                commands.extend(pre_persistence_commands)
                results.extend(pre_persistence_results)

    pre_sequence_complete = (
        pre_mount_complete
        and preflight_complete
        and pre_firmware_identity_ok is not False
        and bool(pre_data_commands)
        and sequence_completed(
            pre_data_commands, pre_data_results, pre_data_commands
        )
    )
    pre_results = [
        *pre_mount_results,
        *preflight_results,
        *pre_data_results,
        *pre_persistence_results,
    ]
    unexpected_restart_before_reboot = unexpected_console_restart(pre_results)
    pre_by_command = {
        **dict(zip(preflight_commands, preflight_results)),
        **dict(zip(pre_data_commands, pre_data_results)),
        **pre_persistence_by_command,
    }
    initial_pre_data_by_command = dict(zip(pre_data_commands, pre_data_results))
    filecanary_result = pre_by_command.get("storage filecanary", {})
    retained_result = pre_by_command.get(f"storage retained-canary {token}", {})
    pre_map_tile = pre_by_command.get(f"storage map-tile-canary {token}", {})
    post_storage: dict = {}
    post_by_command: dict[str, dict] = {}
    post_map_tile: dict = {}
    health: dict = {}
    post_mount_commands: list[str] = []
    post_mount_results: list[dict] = []
    post_data_commands: list[str] = []
    post_data_results: list[dict] = []
    post_persistence_commands: list[str] = []
    post_persistence_results: list[dict] = []
    post_persistence_by_command: dict[str, dict] = {}
    post_persistence_clean = not (strict_evidence and include_reboot)
    persistence_poll_attempts_used = 0
    post_remount_busy = False
    post_remount_retained_worker_quiesce_acquired = None
    post_remount_command_passed = not include_reboot
    post_sequence_complete = False
    reboot_result: dict | None = None
    reboot_ok = not include_reboot
    reboot_attempted = False
    post_reboot_serial_health: dict = {}

    pre_readbacks_ok = readbacks_pass(
        pre_by_command, token, fingerprint, retained_result
    )
    pre_health = pre_by_command.get("health", {})
    pre_health_ok = pre_health.get("ok") is True if include_reboot else None
    filecanary_ok = canary_passed(filecanary_result)
    retained_ok = retained_canary_metadata(
        retained_result, token, fingerprint
    ) is not None
    pre_map_tile_ok = map_tile_write_passed(pre_map_tile, token)
    storage_after_canary = initial_pre_data_by_command.get("storage status", {})
    storage_before_reboot = pre_by_command.get("storage status", {})
    retained_storage_clean_after_canary = (
        pre_persistence_clean
        if strict_evidence and include_reboot
        else retained_storage_clean(storage_after_canary)
    )
    pre_timeout_command = next(
        (
            result.get("cmd", "unknown")
            for result in pre_results
            if host_timed_out(result)
        ),
        None,
    )
    reboot_skipped_reason = semantic_reboot_skip_reason(
        include_reboot=include_reboot,
        timeout_command=pre_timeout_command,
        unexpected_restart=unexpected_restart_before_reboot,
        pre_mount_complete=pre_mount_complete,
        pre_sequence_complete=pre_sequence_complete,
        filecanary_ok=filecanary_ok,
        retained_canary_ok=retained_ok,
        map_tile_canary_ok=pre_map_tile_ok,
        pre_readbacks_ok=pre_readbacks_ok,
        retained_storage_clean_after_canary=(
            retained_storage_clean_after_canary
        ),
        pre_health_ok=pre_health_ok is True,
    )
    pre_reboot_gate_passed = (
        reboot_skipped_reason is None if include_reboot else None
    )

    if include_reboot and pre_reboot_gate_passed:
        reboot_attempted = True
        with serial_provider.context() as ser:
            commands.append("reboot")
            reboot_result = run_command(ser, "reboot", timeout)
            results.append(reboot_result)
        reboot_ok = reboot_command_passed(reboot_result)
        if reboot_ok:
            time.sleep(reboot_settle_sec)
            with serial_provider.context() as ser:
                if serial_provider.reuse:
                    post_reboot_serial_health = wait_for_console_ready(
                        ser,
                        POSIX_SERIAL_READY_TIMEOUT_SECONDS,
                        attempts=3,
                    )
                ser.reset_input_buffer()
                post_mount_commands, post_mount_results, post_storage = run_mount_sequence(
                    ser,
                    timeout=timeout,
                    mount_poll_attempts=mount_poll_attempts,
                    mount_poll_interval_sec=mount_poll_interval_sec,
                    allow_initial_reboot_boot_help=True,
                )
                commands.extend(post_mount_commands)
                results.extend(post_mount_results)
                post_mount_transport_complete = (
                    len(post_mount_commands) >= 3
                    and not any(host_timed_out(result) for result in post_mount_results)
                    and not unexpected_console_restart(post_mount_results)
                )
                post_remount_busy = any(
                    remount_manager_busy(result) for result in post_mount_results
                )
                post_remount_results = [
                    result
                    for result in post_mount_results
                    if result.get("cmd") == "storage remount"
                ]
                post_remount_result = (
                    post_remount_results[-1] if post_remount_results else {}
                )
                post_remount_command_passed = (
                    post_mount_transport_complete
                    and remount_transition_passed(post_remount_result, post_storage)
                )
                post_remount_retained_worker_quiesce_acquired = (
                    post_remount_result.get("retained_worker_quiesce_acquired")
                    if isinstance(post_remount_result, dict)
                    and post_remount_result.get("ok") is True
                    else None
                )
                post_mount_complete = (
                    post_mount_transport_complete
                    and post_remount_command_passed
                    and storage_ready(post_storage)
                )
                post_data_plan = [
                    *retained_readback_commands(token),
                    f"storage map-tile-check {token}",
                    "health",
                ]
                if strict_evidence:
                    post_data_plan.extend(["version", "crashlog"])
                if post_mount_complete:
                    post_data_commands, post_data_results = run_commands_until_terminal(
                        ser, post_data_plan, timeout
                    )
                    commands.extend(post_data_commands)
                    results.extend(post_data_results)
                post_base_sequence_complete = post_mount_complete and sequence_completed(
                    post_data_commands, post_data_results, post_data_plan
                )
                post_by_command = dict(zip(post_data_commands, post_data_results))
                if post_base_sequence_complete and strict_evidence:
                    (
                        post_persistence_commands,
                        post_persistence_results,
                        post_persistence_by_command,
                        post_persistence_clean,
                        persistence_poll_attempts_used,
                    ) = run_persistence_poll(
                        ser,
                        token=token,
                        timeout=timeout,
                        attempts=persistence_poll_attempts,
                        interval_sec=persistence_poll_interval_sec,
                    )
                    commands.extend(post_persistence_commands)
                    results.extend(post_persistence_results)
                    post_by_command.update(post_persistence_by_command)
                post_sequence_complete = (
                    post_base_sequence_complete and post_persistence_clean
                )
                post_map_tile = post_by_command.get(f"storage map-tile-check {token}", {})
                health = post_by_command.get("health", {})
    elif not include_reboot and pre_sequence_complete and not unexpected_restart_before_reboot:
        with serial_provider.context() as ser:
            ser.reset_input_buffer()
            health_commands, health_results = run_commands_until_terminal(
                ser, ["health"], timeout
            )
            commands.extend(health_commands)
            results.extend(health_results)
            health = health_results[0] if health_results else {}
            post_sequence_complete = sequence_completed(
                health_commands, health_results, ["health"]
            )
        post_storage = pre_storage
        post_by_command = pre_by_command
        post_map_tile = pre_map_tile

    public_rf_tx = any_flag(results, "public_rf_tx")
    formats_sd = any_flag(results, "formats_sd") or any(
        result.get("format_performed") is True for result in results
    )
    reboot_nonce_proven = (
        boot_transition_proven(pre_health, health) if include_reboot else True
    )
    reboot_reset_reason_ok = health.get("reset_reason") == "SW" if include_reboot else True
    reboot_proof_ok = reboot_nonce_proven and reboot_reset_reason_ok
    post_readbacks_ok = (
        reboot_proof_ok
        and readbacks_pass(post_by_command, token, fingerprint, retained_result)
        if include_reboot
        else True
    )
    pre_ready = storage_ready(pre_storage)
    post_ready = (
        reboot_proof_ok and storage_ready(post_storage)
        if include_reboot
        else storage_ready(post_storage)
    )
    pre_retained_sd_ready = retained_storage_ready(pre_storage)
    post_retained_sd_ready = (
        reboot_proof_ok and retained_storage_ready(post_storage)
        if include_reboot
        else retained_storage_ready(post_storage)
    )
    post_map_tile_ok = (
        reboot_proof_ok and map_tile_check_passed(post_map_tile, token)
        if include_reboot
        else map_tile_write_passed(post_map_tile, token)
    )
    health_ok = health.get("ok") is True
    post_version = post_by_command.get("version", {})
    post_firmware_identity_ok = (
        firmware_identity_matches(post_version, expected_firmware_commit)
        if strict_evidence and include_reboot and expected_firmware_commit is not None
        else None
    )
    firmware_identity_ok = (
        pre_firmware_identity_ok is True
        and (post_firmware_identity_ok is True if include_reboot else True)
        if strict_evidence
        else None
    )
    pre_crashlog = pre_by_command.get("crashlog", {})
    post_crashlog = post_by_command.get("crashlog", {})
    crashlog_transition_ok = (
        crashlog_transition_passed(pre_crashlog, post_crashlog)
        if strict_evidence and include_reboot
        else None
    )
    timed_out_command = next(
        (
            result.get("cmd", "unknown")
            for result in results
            if host_timed_out(result)
        ),
        None,
    )
    pre_persistence_checked = (
        strict_evidence
        and include_reboot
        and pre_persistence_poll_attempts_used > 0
        and persistence_poll_snapshot_checked(pre_persistence_by_command, token)
    )
    post_persistence_checked = (
        strict_evidence
        and include_reboot
        and persistence_poll_attempts_used > 0
        and persistence_poll_snapshot_checked(post_persistence_by_command, token)
    )

    report = {
        "schema": 1,
        "mode": "hardware",
        "port": port,
        "baud": baud,
        "token": token,
        "fingerprint": fingerprint,
        "expected_firmware_commit": expected_firmware_commit,
        "pre_device_build_commit": pre_version.get("build_commit"),
        "device_build_commit": post_version.get("build_commit") if include_reboot else pre_version.get("build_commit"),
        "firmware_identity_required": strict_evidence,
        "pre_firmware_identity_ok": pre_firmware_identity_ok,
        "post_firmware_identity_ok": post_firmware_identity_ok,
        "firmware_identity_ok": firmware_identity_ok,
        "persistence_clean_required": strict_evidence and include_reboot,
        "pre_reboot_persistence_checked": pre_persistence_checked,
        "pre_reboot_persistence_clean": (
            pre_persistence_clean if pre_persistence_checked else None
        ),
        "pre_reboot_pending_dirty": (
            not pre_persistence_clean if pre_persistence_checked else None
        ),
        "pre_reboot_persistence_poll_attempts_used": pre_persistence_poll_attempts_used,
        "pre_reboot_persistence": pre_persistence_by_command,
        "post_reboot_persistence_checked": post_persistence_checked,
        "post_reboot_persistence_clean": (
            post_persistence_clean if post_persistence_checked else None
        ),
        "post_reboot_pending_dirty": (
            not post_persistence_clean if post_persistence_checked else None
        ),
        "persistence_poll_attempts_used": persistence_poll_attempts_used,
        "post_reboot_persistence": post_persistence_by_command,
        "crashlog_transition_required": strict_evidence and include_reboot,
        "crashlog_transition_ok": crashlog_transition_ok,
        "crashlog_before_reboot": pre_crashlog,
        "crashlog_after_reboot": post_crashlog,
        "commands": commands,
        "public_rf_tx": public_rf_tx,
        "formats_sd": formats_sd,
        "pre_sequence_complete": pre_sequence_complete,
        "post_sequence_complete": post_sequence_complete,
        "timed_out_command": timed_out_command,
        "unexpected_restart_before_reboot": unexpected_restart_before_reboot,
        "pre_mutating_commands_attempted": [
            command
            for command in pre_data_commands
            if command == "storage filecanary"
            or command.startswith("storage retained-canary ")
            or command.startswith("storage map-tile-canary ")
        ],
        "pre_reboot_gate_passed": pre_reboot_gate_passed,
        "pre_reboot_health_ok": pre_health_ok,
        "reboot_attempted": reboot_attempted,
        "reboot_skipped_reason": (
            None if reboot_attempted else reboot_skipped_reason
        ),
        "pre_remount_ready": pre_ready,
        "post_remount_ready": post_ready,
        "pre_remount_manager_busy": pre_remount_busy,
        "post_remount_manager_busy": post_remount_busy,
        "pre_remount_retained_worker_quiesce_acquired": pre_remount_retained_worker_quiesce_acquired,
        "post_remount_retained_worker_quiesce_acquired": post_remount_retained_worker_quiesce_acquired,
        "pre_remount_command_passed": pre_remount_command_passed,
        "post_remount_command_passed": post_remount_command_passed,
        "retained_history_sd_ready_before": pre_retained_sd_ready,
        "retained_history_sd_ready_after": post_retained_sd_ready,
        "retained_history_sd_clean_after_canary": (
            retained_storage_clean_after_canary
        ),
        "reboot_command_passed": reboot_ok if include_reboot else None,
        "reboot_reset_scope": (
            reboot_result.get("reset_scope")
            if include_reboot and isinstance(reboot_result, dict)
            else None
        ),
        "reboot_connectivity_prepare": (
            reboot_result.get("connectivity_prepare")
            if include_reboot and isinstance(reboot_result, dict)
            else None
        ),
        "reboot_retained_flush": (
            reboot_result.get("retained_flush")
            if include_reboot and isinstance(reboot_result, dict)
            else None
        ),
        "reboot_route_flush": (
            reboot_result.get("route_flush")
            if include_reboot and isinstance(reboot_result, dict)
            else None
        ),
        "reboot_storage_manager_quiesced": (
            reboot_result.get("storage_manager_quiesced")
            if include_reboot and isinstance(reboot_result, dict)
            else None
        ),
        "reboot_retained_worker_quiesced": (
            reboot_result.get("retained_worker_quiesced")
            if include_reboot and isinstance(reboot_result, dict)
            else None
        ),
        "reboot_rp2040_bridge_quiesced": (
            reboot_result.get("rp2040_bridge_quiesced")
            if include_reboot and isinstance(reboot_result, dict)
            else None
        ),
        "pre_reboot_boot_nonce": health_boot_nonce(pre_health) if include_reboot else None,
        "post_reboot_boot_nonce": health_boot_nonce(health) if include_reboot else None,
        "post_reboot_reset_reason": health.get("reset_reason") if include_reboot else None,
        "reboot_nonce_proven": reboot_nonce_proven if include_reboot else None,
        "reboot_proven": reboot_proof_ok if include_reboot else None,
        "filecanary_passed": filecanary_ok,
        "retained_canary_passed": retained_ok,
        "pre_reboot_readbacks_ok": pre_readbacks_ok,
        "post_reboot_readbacks_ok": post_readbacks_ok,
        "pre_map_tile_canary_passed": pre_map_tile_ok,
        "post_map_tile_canary_passed": post_map_tile_ok,
        "health_ok": health_ok,
        "storage_before_canary": semantic_storage_copy(pre_storage),
        "storage_after_canary": semantic_storage_copy(storage_after_canary),
        "storage_before_reboot": semantic_storage_copy(storage_before_reboot),
        "storage_after_reboot": semantic_storage_copy(post_storage),
        "health": health,
        "results": results,
        "serial_session_reused": serial_provider.reuse,
        "serial_physical_open_count": serial_provider.physical_open_count,
        "serial_startup_health": serial_provider.startup_health,
        "post_reboot_serial_health": post_reboot_serial_health,
        "ok": (
            not public_rf_tx
            and not formats_sd
            and pre_sequence_complete
            and post_sequence_complete
            and timed_out_command is None
            and not unexpected_restart_before_reboot
            and pre_ready
            and post_ready
            and pre_remount_command_passed
            and post_remount_command_passed
            and pre_retained_sd_ready
            and post_retained_sd_ready
            and reboot_ok
            and reboot_proof_ok
            and filecanary_ok
            and retained_ok
            and pre_readbacks_ok
            and post_readbacks_ok
            and pre_map_tile_ok
            and post_map_tile_ok
            and health_ok
            and (
                serial_provider.startup_health.get("ok") is True
                if serial_provider.reuse
                else True
            )
            and (
                post_reboot_serial_health.get("ok") is True
                if serial_provider.reuse and include_reboot
                else True
            )
            and (firmware_identity_ok is not False)
            and (pre_persistence_clean if strict_evidence and include_reboot else True)
            and (post_persistence_clean if strict_evidence and include_reboot else True)
            and (crashlog_transition_ok is True if strict_evidence and include_reboot else True)
        ),
    }
    serial_provider.close()
    return report


def write_report(report: dict, out_path: Path | None) -> Path:
    root = Path(__file__).resolve().parents[1]
    stamp_report(report, root)
    if out_path is None:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        out_path = root / "artifacts" / "sd-reboot-remount" / f"d1l-sd-reboot-remount-{stamp}.json"
    elif not out_path.is_absolute():
        out_path = root / out_path
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2) + "\n", encoding="ascii")
    return out_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--token", default=datetime.now(timezone.utc).strftime("remount%Y%m%d%H%M%S"))
    parser.add_argument("--no-reboot", action="store_true")
    parser.add_argument("--reboot-settle-sec", type=float, default=8.0)
    parser.add_argument("--mount-poll-attempts", type=int, default=MOUNT_POLL_ATTEMPTS)
    parser.add_argument("--mount-poll-interval-sec", type=float, default=MOUNT_POLL_INTERVAL_SECONDS)
    parser.add_argument("--expected-firmware-commit")
    parser.add_argument("--persistence-poll-attempts", type=int, default=PERSISTENCE_POLL_ATTEMPTS)
    parser.add_argument("--persistence-poll-interval-sec", type=float, default=PERSISTENCE_POLL_INTERVAL_SECONDS)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--list-commands", action="store_true")
    parser.add_argument("--out")
    args = parser.parse_args()

    if not TOKEN_RE.match(args.token):
        parser.error("--token must be 1-31 chars: A-Z a-z 0-9 _ . -")
    if (
        args.expected_firmware_commit is not None
        and exact_commit(args.expected_firmware_commit) is None
    ):
        parser.error("--expected-firmware-commit must be an exact 40-character hexadecimal SHA")
    if args.persistence_poll_attempts < 1:
        parser.error("--persistence-poll-attempts must be at least 1")
    if args.persistence_poll_interval_sec < 0:
        parser.error("--persistence-poll-interval-sec cannot be negative")
    include_reboot = not args.no_reboot

    if args.list_commands:
        for command in command_plan(
            args.token,
            include_reboot=include_reboot,
            strict_evidence=args.expected_firmware_commit is not None,
        ):
            print(command)
        return 0

    if args.dry_run:
        report = dry_run_report(
            args.token,
            include_reboot=include_reboot,
            expected_firmware_commit=args.expected_firmware_commit,
        )
    else:
        if not args.port:
            parser.error("No D1L port supplied. Set D1L_PORT or pass --port.")
        if args.expected_firmware_commit is None:
            parser.error("Hardware evidence requires --expected-firmware-commit.")
        report = run_acceptance(
            args.port,
            args.baud,
            args.timeout,
            args.token,
            include_reboot=include_reboot,
            reboot_settle_sec=args.reboot_settle_sec,
            mount_poll_attempts=args.mount_poll_attempts,
            mount_poll_interval_sec=args.mount_poll_interval_sec,
            expected_firmware_commit=args.expected_firmware_commit,
            persistence_poll_attempts=args.persistence_poll_attempts,
            persistence_poll_interval_sec=args.persistence_poll_interval_sec,
        )

    written = write_report(report, Path(args.out) if args.out else None)
    print(json.dumps({"ok": report["ok"], "out": str(written), "mode": report["mode"]}))
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
