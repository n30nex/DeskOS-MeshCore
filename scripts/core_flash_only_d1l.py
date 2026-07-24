#!/usr/bin/env python3
"""Flash one exact Actions-built Core candidate to the verified D1L target."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from collections.abc import Iterable
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

try:
    from autonomous_hardware_validate_d1l import (
        RunContext,
        esptool_flash_command,
        verify_esp32_flash_inputs,
    )
    from artifact_metadata import git_metadata
    from capture_core_actions_run_d1l import validate_capture_receipt
    from core_smoke_d1l import (
        CORE_RELEASE_PROFILE,
        D1L_CORE_PORT,
        enforce_core_port,
        exact_identity,
        exact_version_identity,
        resolve_core_target,
    )
    from d1l_serial_target import safe_slug
    from release_gate_audit_d1l import find_release_package
    from smoke_d1l import (
        exact_commit,
        open_d1l_serial,
        send_console_command,
    )
    from verify_checksums import (
        is_link_or_reparse,
        sha256_file,
        verify_checksum_tree,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.autonomous_hardware_validate_d1l import (
        RunContext,
        esptool_flash_command,
        verify_esp32_flash_inputs,
    )
    from scripts.artifact_metadata import git_metadata
    from scripts.capture_core_actions_run_d1l import (
        validate_capture_receipt,
    )
    from scripts.core_smoke_d1l import (
        CORE_RELEASE_PROFILE,
        D1L_CORE_PORT,
        enforce_core_port,
        exact_identity,
        exact_version_identity,
        resolve_core_target,
    )
    from scripts.d1l_serial_target import safe_slug
    from scripts.release_gate_audit_d1l import find_release_package
    from scripts.smoke_d1l import (
        exact_commit,
        open_d1l_serial,
        send_console_command,
    )
    from scripts.verify_checksums import (
        is_link_or_reparse,
        sha256_file,
        verify_checksum_tree,
    )


EXPECTED_SD_HISTORY_MODE = "disabled"
EXPECTED_REPOSITORY = "n30nex/SIGUI"
EXPECTED_FLASH_ROLES = {
    0x0: ("bootloader", "bootloader/bootloader.bin"),
    0x8000: ("partition-table", "partition_table/partition-table.bin"),
    0x10000: ("app", "meshcore_deskos_d1l.bin"),
}
FORBIDDEN_PORTS = frozenset({"COM8", "COM11", "COM16", "COM29"})
RETAINED_STATE_COMMANDS = (
    "version",
    "health",
    "settings get",
    "messages public",
    "messages dm",
    "contacts",
    "identity status",
)
EXPECTED_D1L_ROLE = "desk_companion"
FLASH_PHASE_BOOTSTRAP = "bootstrap"
FLASH_PHASE_RETAINED_REFLASH = "retained-reflash"
FLASH_PHASES = (
    FLASH_PHASE_BOOTSTRAP,
    FLASH_PHASE_RETAINED_REFLASH,
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _load_json(path: Path, label: str) -> dict[str, Any]:
    if is_link_or_reparse(path) or not path.is_file():
        raise ValueError(f"{label} is missing or is a link/reparse point: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"{label} is invalid JSON: {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be a JSON object: {path}")
    return value


def _inside(path: Path, parent: Path, label: str) -> Path:
    try:
        resolved_parent = parent.resolve(strict=True)
        resolved = path.resolve(strict=True)
        resolved.relative_to(resolved_parent)
    except (OSError, RuntimeError, ValueError) as exc:
        raise ValueError(f"{label} must stay inside {parent}") from exc
    if is_link_or_reparse(path):
        raise ValueError(f"{label} cannot be a link/reparse point: {path}")
    return resolved


def make_context(
    *,
    root: Path,
    commit: str,
    run_id: str,
    github_run_dir: Path,
    port: str,
    serial_baud: int,
    flash_baud: int,
) -> RunContext:
    target_slug = safe_slug(enforce_core_port(port))
    return RunContext(
        root=root,
        commit=commit,
        short_commit=commit[:7],
        github_run_id=run_id,
        github_run_dir=github_run_dir,
        d1l_port=port,
        rp2040_port="",
        hardware_dir=root / "artifacts" / "hardware" / target_slug,
        rp2040_hardware_dir=root / "artifacts" / "hardware" / "unused",
        baud=serial_baud,
        esp32_flash_baud=flash_baud,
    )


def verify_core_package(
    *,
    github_run_dir: Path,
    package_dir: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
    actions_verification: dict,
) -> dict:
    package_root = github_run_dir / "d1l-release-package"
    package = _inside(package_dir, package_root, "Core release package")
    manifests = sorted(package.rglob("SHA256SUMS.txt"))
    if manifests != [package / "SHA256SUMS.txt"] or not verify_checksum_tree(
        package
    ):
        raise ValueError(
            "Core package must have one complete valid root SHA256SUMS.txt"
        )
    manifest_path = package / "manifest.json"
    manifest = _load_json(manifest_path, "Core release manifest")
    workflow = manifest.get("workflow")
    git_info = manifest.get("git")
    install = manifest.get("install_recovery_guide")
    required_truth = (
        manifest.get("release_profile") == CORE_RELEASE_PROFILE
        and exact_commit(manifest.get("firmware_commit")) == commit
        and str(manifest.get("actions_run")) == run_id
        and manifest.get("sd_history_mode") == EXPECTED_SD_HISTORY_MODE
        and manifest.get("sd_history_state")
        == "disabled_nvs_authoritative"
        and manifest.get("storage_authority") == "nvs"
        and manifest.get("full_feature_release_ready") is False
        and manifest.get("rp2040_artifacts") == []
        and manifest.get("update_image") is None
        and isinstance(workflow, dict)
        and exact_commit(workflow.get("sha")) == commit
        and str(workflow.get("run_id")) == run_id
        and str(workflow.get("run_attempt")) == run_attempt
        and workflow.get("repository") == EXPECTED_REPOSITORY
        and isinstance(git_info, dict)
        and exact_commit(git_info.get("commit")) == commit
        and git_info.get("dirty") is False
        and git_info.get("dirty_entries") == []
        and isinstance(install, dict)
        and install.get("normal_install_script") == "flash_project.ps1"
        and install.get("normal_install_port") == D1L_CORE_PORT
        and install.get("normal_install_preserves_unrelated_nvs") is True
        and install.get("normal_install_package_root_only") is True
        and install.get("normal_install_checksum_verified") is True
        and install.get("no_on_device_sd_format") is True
    )
    if not required_truth:
        raise ValueError(
            "Core package manifest commit/run/profile/disabled-SD truth mismatch"
        )
    if (package / "rp2040").exists() or (package / "update").exists():
        raise ValueError("Disabled-SD Core package contains excluded payloads")

    action_rows = actions_verification.get("flash_files")
    if not isinstance(action_rows, list):
        raise ValueError("Actions flash verification has no flash_files")
    actions_by_offset = {
        row.get("offset"): row for row in action_rows if isinstance(row, dict)
    }
    package_rows = manifest.get("flash_files")
    if not isinstance(package_rows, list) or len(package_rows) != 3:
        raise ValueError("Core package must contain exactly three project images")
    checked_rows: list[dict] = []
    seen_offsets: set[int] = set()
    for row in package_rows:
        if not isinstance(row, dict):
            raise ValueError("Core package flash row is invalid")
        try:
            offset = int(str(row.get("offset")), 0)
        except (TypeError, ValueError) as exc:
            raise ValueError("Core package flash offset is invalid") from exc
        expected = EXPECTED_FLASH_ROLES.get(offset)
        action = actions_by_offset.get(offset)
        if expected is None or action is None or offset in seen_offsets:
            raise ValueError("Core package project image offsets are not exact")
        seen_offsets.add(offset)
        role, source = expected
        target = _inside(
            package / str(row.get("path") or ""),
            package,
            f"Core package {role} image",
        )
        digest = sha256_file(target)
        if not (
            row.get("role") == role
            and row.get("source") == source
            and row.get("size") == target.stat().st_size
            and row.get("sha256") == digest
            and action.get("path") == f"build/{source}"
            and action.get("size") == target.stat().st_size
            and str(action.get("sha256") or "").lower() == digest
        ):
            raise ValueError(
                f"Core package {role} does not match the Actions project image"
            )
        checked_rows.append(
            {
                "role": role,
                "offset": offset,
                "path": target.relative_to(package).as_posix(),
                "size": target.stat().st_size,
                "sha256": digest,
            }
        )
    if seen_offsets != set(EXPECTED_FLASH_ROLES):
        raise ValueError("Core package project image roles are incomplete")
    return {
        "ok": True,
        "package": str(package),
        "manifest": str(manifest_path),
        "manifest_sha256": sha256_file(manifest_path),
        "checksum_manifest_sha256": sha256_file(
            package / "SHA256SUMS.txt"
        ),
        "checksum_tree_verified": True,
        "firmware_commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": EXPECTED_SD_HISTORY_MODE,
        "storage_authority": "nvs",
        "repository": EXPECTED_REPOSITORY,
        "flash_files_match_actions": True,
        "flash_files": checked_rows,
    }


def default_flash_runner(
    command: list[str], cwd: Path, timeout: int
) -> tuple[dict, bytes]:
    started_at = utc_now()
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        raw = completed.stdout or b""
        result = {
            "name": "esp32_flash",
            "ok": completed.returncode == 0,
            "returncode": completed.returncode,
            "args": command,
            "cwd": str(cwd),
            "started_at": started_at,
            "ended_at": utc_now(),
        }
    except subprocess.TimeoutExpired as exc:
        raw = (exc.stdout or b"") + (exc.stderr or b"")
        result = {
            "name": "esp32_flash",
            "ok": False,
            "returncode": None,
            "args": command,
            "cwd": str(cwd),
            "started_at": started_at,
            "ended_at": utc_now(),
            "error": "timeout",
        }
    return result, raw


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
        and result.get("role") == EXPECTED_D1L_ROLE
    )


def read_identity_status(
    port: str,
    baud: int,
    timeout: float,
    settle_sec: float,
) -> dict:
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError(
            "pyserial is required for D1L identity preflight"
        ) from exc
    if settle_sec > 0:
        time.sleep(settle_sec)
    with open_d1l_serial(
        serial, port=port, baudrate=baud, timeout=timeout
    ) as handle:
        time.sleep(1.0)
        handle.reset_input_buffer()
        return send_console_command(handle, "identity status", timeout)


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


def read_device_identity(
    port: str, baud: int, timeout: float, settle_sec: float
) -> tuple[dict, dict]:
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError("pyserial is required for post-flash identity") from exc
    time.sleep(settle_sec)
    with open_d1l_serial(
        serial, port=port, baudrate=baud, timeout=timeout
    ) as handle:
        time.sleep(1.0)
        handle.reset_input_buffer()
        version = send_console_command(handle, "version", timeout)
        health = send_console_command(handle, "health", timeout)
    return version, health


def read_retained_state(
    port: str, baud: int, timeout: float, settle_sec: float
) -> list[dict]:
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError(
            "pyserial is required for retained-state capture"
        ) from exc
    if settle_sec > 0:
        time.sleep(settle_sec)
    with open_d1l_serial(
        serial, port=port, baudrate=baud, timeout=timeout
    ) as handle:
        time.sleep(1.0)
        handle.reset_input_buffer()
        return [
            send_console_command(handle, command, timeout)
            for command in RETAINED_STATE_COMMANDS
        ]


def retained_state_projection(results: object) -> dict | None:
    if not isinstance(results, list):
        return None
    by_command = {
        result.get("cmd"): result
        for result in results
        if isinstance(result, dict) and isinstance(result.get("cmd"), str)
    }
    if set(by_command) != set(RETAINED_STATE_COMMANDS):
        return None
    if not all(result.get("ok") is True for result in by_command.values()):
        return None
    settings = by_command["settings get"]
    settings_projection = {
        key: settings.get(key)
        for key in (
            "node_name",
            "path_hash_bytes",
            "radio_frequency_mhz",
            "radio_bandwidth_khz",
            "radio_spreading_factor",
            "radio_coding_rate",
            "radio_tx_power_dbm",
        )
        if key in settings
    }
    if (
        not isinstance(settings_projection.get("node_name"), str)
        or not settings_projection["node_name"]
    ):
        return None

    def projected_entries(command: str) -> list[dict] | None:
        rows = by_command[command].get("entries")
        if not isinstance(rows, list) or not all(
            isinstance(row, dict) for row in rows
        ):
            return None
        return sorted(
            rows,
            key=lambda row: json.dumps(
                row, sort_keys=True, separators=(",", ":")
            ),
        )

    public = projected_entries("messages public")
    direct = projected_entries("messages dm")
    contacts = projected_entries("contacts")
    identity = by_command["identity status"]
    identity_public_key = exact_public_key(identity.get("public_key"))
    if (
        public is None
        or direct is None
        or contacts is None
        or identity_public_key is None
        or identity.get("public_key_ready") is not True
        or identity.get("fingerprint")
        != identity_public_key[:16].upper()
        or identity.get("role") != EXPECTED_D1L_ROLE
    ):
        return None
    return {
        "settings": settings_projection,
        "public_messages": public,
        "direct_messages": direct,
        "contacts": contacts,
        "identity_public_key": identity_public_key,
    }


def projection_sha256(projection: dict) -> str:
    canonical = json.dumps(
        projection, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    import hashlib

    return hashlib.sha256(canonical).hexdigest()


def retained_state_preserved(before: dict, after: dict) -> bool:
    if (
        before.get("settings") != after.get("settings")
        or before.get("identity_public_key")
        != after.get("identity_public_key")
    ):
        return False
    for field in ("public_messages", "direct_messages", "contacts"):
        before_rows = {
            json.dumps(row, sort_keys=True, separators=(",", ":"))
            for row in before.get(field, [])
        }
        after_rows = {
            json.dumps(row, sort_keys=True, separators=(",", ":"))
            for row in after.get(field, [])
        }
        if not before_rows.issubset(after_rows):
            return False
    return True


def write_state_snapshot(
    *,
    path: Path,
    root: Path,
    phase: str,
    commit: str,
    results: list[dict],
    d1l_target: dict[str, Any],
) -> tuple[dict, dict]:
    projection = retained_state_projection(results)
    if projection is None:
        raise ValueError(f"{phase} retained-state results are incomplete")
    if path.exists():
        raise ValueError(f"refusing to overwrite retained snapshot: {path}")
    payload = {
        "schema": 2,
        "kind": "core_retained_state_snapshot",
        "mode": "hardware",
        "phase": phase,
        "captured_at": utc_now(),
        "port": d1l_target["requested_path"],
        "d1l_target": d1l_target,
        "expected_firmware_commit": commit,
        "results": results,
        "projection": projection,
        "projection_sha256": projection_sha256(projection),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
    )
    return payload, _relative_file_row(path, root)


def _relative_file_row(path: Path, root: Path) -> dict:
    resolved = _inside(path, root, "raw flash log")
    return {
        "path": resolved.relative_to(root.resolve()).as_posix(),
        "size": resolved.stat().st_size,
        "sha256": sha256_file(resolved),
    }


def run_core_flash_only(
    *,
    root: Path,
    github_run_dir: Path,
    package_dir: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
    actions_capture_receipt: Path,
    port: str,
    expected_d1l_public_key: str,
    serial_baud: int,
    flash_baud: int,
    serial_timeout: float,
    flash_timeout: int,
    settle_sec: float,
    raw_log_path: Path,
    flash_phase: str,
    flash_runner: Callable[
        [list[str], Path, int], tuple[dict, bytes]
    ] = default_flash_runner,
    retained_state_reader: Callable[
        [str, int, float, float], list[dict]
    ] = read_retained_state,
    identity_status_reader: Callable[
        [str, int, float, float], dict
    ] = read_identity_status,
    port_lister: Callable[[], Iterable[object]] | None = None,
    platform_name: str | None = None,
) -> dict:
    root = root.resolve()
    port = enforce_core_port(port)
    if port in FORBIDDEN_PORTS:
        raise ValueError(f"Refusing forbidden port {port}")
    normalized_commit = exact_commit(commit)
    if normalized_commit is None:
        raise ValueError("--commit must be an exact 40-character hexadecimal SHA")
    normalized_public_key = exact_public_key(expected_d1l_public_key)
    if normalized_public_key is None:
        raise ValueError(
            "--expected-d1l-public-key must be an exact 64-hex public key"
        )
    if (
        not str(run_id).isdigit()
        or int(run_id) < 1
        or not str(run_attempt).isdigit()
        or int(run_attempt) < 1
    ):
        raise ValueError(
            "GitHub run id and run attempt must be positive integers"
        )
    if flash_phase not in FLASH_PHASES:
        raise ValueError(
            f"--phase must be one of: {', '.join(FLASH_PHASES)}"
        )
    github_run_dir = _inside(
        github_run_dir, root, "downloaded GitHub Actions run directory"
    )
    raw_log_path = raw_log_path.resolve()
    try:
        raw_log_path.relative_to(root)
    except ValueError as exc:
        raise ValueError("raw flash log must stay inside the repository root") from exc
    source_git = git_metadata(root)
    source_identity_ok = (
        exact_commit(source_git.get("commit")) == normalized_commit
        and source_git.get("dirty") is False
        and source_git.get("dirty_entries") == []
    )
    if not source_identity_ok:
        raise ValueError(
            "Core flash runner source must be the exact clean candidate commit"
        )
    actions_capture_verification = validate_capture_receipt(
        receipt_path=actions_capture_receipt,
        root=root,
        github_run_dir=github_run_dir,
        commit=normalized_commit,
        run_id=str(run_id),
        run_attempt=str(run_attempt),
    )
    before_snapshot_path = raw_log_path.with_name(
        raw_log_path.stem + "_retained_before.json"
    )
    after_snapshot_path = raw_log_path.with_name(
        raw_log_path.stem + "_retained_after.json"
    )
    output_paths = [raw_log_path, after_snapshot_path]
    if flash_phase == FLASH_PHASE_RETAINED_REFLASH:
        output_paths.append(before_snapshot_path)
    for output_path in output_paths:
        if output_path.exists():
            raise ValueError(
                f"refusing to overwrite Core flash evidence: {output_path}"
            )

    if port_lister is None:
        try:
            from serial.tools import list_ports
        except ImportError as exc:
            raise RuntimeError(
                "pyserial is required for D1L target verification"
            ) from exc
        def runtime_port_lister() -> Iterable[object]:
            return list_ports.comports(include_links=True)

        port_lister = runtime_port_lister
    d1l_target_before = resolve_core_target(
        port,
        port_lister=port_lister,
        platform_name=platform_name,
    )
    port = d1l_target_before["requested_path"]
    context = make_context(
        root=root,
        commit=normalized_commit,
        run_id=str(run_id),
        github_run_dir=github_run_dir,
        port=port,
        serial_baud=serial_baud,
        flash_baud=flash_baud,
    )
    actions_verification = verify_esp32_flash_inputs(context)
    package_verification = verify_core_package(
        github_run_dir=github_run_dir,
        package_dir=package_dir,
        commit=normalized_commit,
        run_id=str(run_id),
        run_attempt=str(run_attempt),
        actions_verification=actions_verification,
    )
    pre_flash_identity = identity_status_reader(
        port, serial_baud, serial_timeout, 0.0
    )
    if not identity_status_ok(
        pre_flash_identity,
        normalized_public_key,
    ):
        raise ValueError(
            "Pre-flash identity status does not match the pinned D1L public key"
        )
    before_projection: dict | None = None
    before_snapshot_row: dict | None = None
    if flash_phase == FLASH_PHASE_RETAINED_REFLASH:
        before_results = retained_state_reader(
            port, serial_baud, serial_timeout, 0.0
        )
        before_projection = retained_state_projection(before_results)
        before_version = (
            before_results[0] if len(before_results) > 0 else {}
        )
        before_health = (
            before_results[1] if len(before_results) > 1 else {}
        )
        before_identity = next(
            (
                row
                for row in before_results
                if isinstance(row, dict)
                and row.get("cmd") == "identity status"
            ),
            {},
        )
        if not (
            before_projection is not None
            and exact_version_identity(
                before_version, normalized_commit, EXPECTED_SD_HISTORY_MODE
            )
            and exact_identity(
                before_health, normalized_commit, EXPECTED_SD_HISTORY_MODE
            )
            and before_health.get("board_ready") is True
            and before_health.get("ui_ready") is True
            and identity_status_ok(
                before_identity,
                normalized_public_key,
            )
        ):
            raise ValueError(
                "Closing reflash baseline must be the exact ready candidate; "
                "use --phase bootstrap for the initial candidate install"
            )
        _, before_snapshot_row = write_state_snapshot(
            path=before_snapshot_path,
            root=root,
            phase="pre_flash",
            commit=normalized_commit,
            results=before_results,
            d1l_target=d1l_target_before,
        )
    build_dir = github_run_dir / "d1l-firmware-artifacts" / "build"
    command = esptool_flash_command(build_dir, port, flash_baud)
    if (
        "write-flash" not in command
        or not command_uses_only_target(command, port)
        or any("erase" in token.lower() for token in command)
        or any(
            blocked in token.upper()
            for token in command
            for blocked in FORBIDDEN_PORTS
        )
    ):
        raise ValueError("Generated flash command violates the Core flash-only scope")

    result, raw_log = flash_runner(command, root, flash_timeout)
    raw_log_path.parent.mkdir(parents=True, exist_ok=True)
    raw_log_path.write_bytes(raw_log)
    raw_log_row = _relative_file_row(raw_log_path, root)
    version: dict = {}
    health: dict = {}
    after_results: list[dict] = []
    after_projection: dict | None = None
    after_snapshot_row: dict | None = None
    d1l_target_after: dict[str, Any] | None = None
    target_continuity_ok = False
    target_post_error: str | None = None
    post_flash_identity: dict = {}
    if (
        result.get("name") == "esp32_flash"
        and result.get("ok") is True
        and result.get("returncode") == 0
        and result.get("args") == command
    ):
        if settle_sec > 0:
            time.sleep(settle_sec)
        try:
            d1l_target_after = resolve_core_target(
                port,
                port_lister=port_lister,
                platform_name=platform_name,
            )
        except ValueError as exc:
            target_post_error = str(exc)
        else:
            target_continuity_ok = (
                d1l_target_after["stable_identity_sha256"]
                == d1l_target_before["stable_identity_sha256"]
            )
        if target_continuity_ok:
            after_results = retained_state_reader(
                port, serial_baud, serial_timeout, 0.0
            )
            version = after_results[0] if len(after_results) > 0 else {}
            health = after_results[1] if len(after_results) > 1 else {}
            post_flash_identity = next(
                (
                    row
                    for row in after_results
                    if isinstance(row, dict)
                    and row.get("cmd") == "identity status"
                ),
                {},
            )
            after_projection = retained_state_projection(after_results)
            if after_projection is not None:
                _, after_snapshot_row = write_state_snapshot(
                    path=after_snapshot_path,
                    root=root,
                    phase=(
                        "post_flash"
                        if flash_phase == FLASH_PHASE_RETAINED_REFLASH
                        else "post_bootstrap"
                    ),
                    commit=normalized_commit,
                    results=after_results,
                    d1l_target=d1l_target_after,
                )
    identity_ok = (
        target_continuity_ok
        and
        exact_version_identity(
            version, normalized_commit, EXPECTED_SD_HISTORY_MODE
        )
        and exact_identity(
            health, normalized_commit, EXPECTED_SD_HISTORY_MODE
        )
        and health.get("board_ready") is True
        and health.get("ui_ready") is True
        and identity_status_ok(
            post_flash_identity,
            normalized_public_key,
        )
    )
    ok = result.get("ok") is True and identity_ok
    retained_preserved = (
        bool(
            before_projection is not None
            and after_projection is not None
            and retained_state_preserved(
                before_projection, after_projection
            )
        )
        if flash_phase == FLASH_PHASE_RETAINED_REFLASH
        else None
    )
    ok = (
        ok
        and source_identity_ok
        and (
            retained_preserved is True
            if flash_phase == FLASH_PHASE_RETAINED_REFLASH
            else True
        )
    )
    closure_eligible = (
        ok and flash_phase == FLASH_PHASE_RETAINED_REFLASH
    )
    return {
        "schema": 2,
        "kind": "esp32_flash",
        "mode": "hardware",
        "scope": (
            "core-retained-reflash-only"
            if flash_phase == FLASH_PHASE_RETAINED_REFLASH
            else "core-bootstrap-flash-only"
        ),
        "flash_phase": flash_phase,
        "ok": ok,
        "closure_eligible": closure_eligible,
        "hardware_required": True,
        "physical_observed": True,
        "port": port,
        "d1l_target": d1l_target_before,
        "d1l_target_before": d1l_target_before,
        "d1l_target_after": d1l_target_after,
        "target_identity_continuity_ok": target_continuity_ok,
        "target_post_error": target_post_error,
        "commit": normalized_commit,
        "github_actions_run": str(run_id),
        "workflow_run_attempt": str(run_attempt),
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": EXPECTED_SD_HISTORY_MODE,
        "expected_firmware_commit": normalized_commit,
        "device_build_commit": version.get("build_commit"),
        "device_idf_version": version.get("idf"),
        "firmware_identity_required": True,
        "firmware_identity_ok": identity_ok,
        "expected_d1l_public_key": normalized_public_key,
        "pre_flash_identity": pre_flash_identity,
        "post_flash_identity": post_flash_identity,
        "d1l_public_key_continuity_ok": identity_status_ok(
            post_flash_identity,
            normalized_public_key,
        ),
        "git": source_git,
        "runner_source_identity_ok": source_identity_ok,
        "artifact_verification": actions_verification,
        "actions_capture_verification": actions_capture_verification,
        "package_verification": package_verification,
        "command": command,
        "result": result,
        "raw_flash_log": raw_log_row,
        "post_flash_version": version,
        "post_flash_health": health,
        "commands_before_flash": (
            ["identity status", *RETAINED_STATE_COMMANDS]
            if flash_phase == FLASH_PHASE_RETAINED_REFLASH
            else ["identity status"]
        ),
        "commands_after_flash": list(RETAINED_STATE_COMMANDS),
        "retained_state_before": before_snapshot_row,
        "retained_state_after": after_snapshot_row,
        "retained_state_preserved": retained_preserved,
        "erase_flash": False,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "sd_access": False,
        "rp2040_access": False,
        "formats_sd": False,
        "legacy_suite_ran": False,
        "flashed_at": utc_now() if result.get("ok") is True else None,
    }


def resolve_path(root: Path, value: str) -> Path:
    path = Path(value)
    return path.resolve() if path.is_absolute() else (root / path).resolve()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-attempt", required=True)
    parser.add_argument("--github-run-dir")
    parser.add_argument("--package-dir")
    parser.add_argument("--actions-capture-receipt")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--port", default=D1L_CORE_PORT)
    parser.add_argument("--expected-d1l-public-key", required=True)
    parser.add_argument("--serial-baud", type=int, default=115200)
    parser.add_argument("--flash-baud", type=int, default=460800)
    parser.add_argument("--serial-timeout", type=float, default=5.0)
    parser.add_argument("--flash-timeout", type=int, default=240)
    parser.add_argument("--settle-sec", type=float, default=8.0)
    parser.add_argument("--phase", choices=FLASH_PHASES, required=True)
    parser.add_argument("--out")
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    commit = exact_commit(args.commit)
    if commit is None:
        parser.error("--commit must be an exact 40-character hexadecimal SHA")
    if (
        not str(args.github_run_id).isdigit()
        or int(args.github_run_id) < 1
        or not str(args.github_run_attempt).isdigit()
        or int(args.github_run_attempt) < 1
    ):
        parser.error(
            "--github-run-id and --github-run-attempt must be positive integers"
        )
    run_dir = resolve_path(
        root,
        args.github_run_dir
        or f"artifacts/github/{args.github_run_id}",
    )
    package = (
        resolve_path(root, args.package_dir)
        if args.package_dir
        else find_release_package(run_dir)
    )
    if package is None:
        parser.error("No downloaded d1l-release-package was found")
    actions_capture_receipt = resolve_path(
        root,
        args.actions_capture_receipt
        or (
            f"artifacts/github/{args.github_run_id}/"
            "core-actions-run-metadata/"
            f"core_actions_run_{args.github_run_id}.json"
        ),
    )
    try:
        target_slug = safe_slug(enforce_core_port(args.port))
    except ValueError as exc:
        parser.error(str(exc))
    out = resolve_path(
        root,
        args.out
        or (
            f"artifacts/hardware/{target_slug}/"
            f"esp32_flash_{args.phase.replace('-', '_')}_{commit[:7]}_actions_"
            f"{args.github_run_id}_{target_slug}.json"
        ),
    )
    raw_log = out.with_suffix(".log")
    try:
        report = run_core_flash_only(
            root=root,
            github_run_dir=run_dir,
            package_dir=package,
            commit=commit,
            run_id=str(args.github_run_id),
            run_attempt=str(args.github_run_attempt),
            actions_capture_receipt=actions_capture_receipt,
            port=args.port,
            expected_d1l_public_key=args.expected_d1l_public_key,
            serial_baud=args.serial_baud,
            flash_baud=args.flash_baud,
            serial_timeout=args.serial_timeout,
            flash_timeout=args.flash_timeout,
            settle_sec=args.settle_sec,
            raw_log_path=raw_log,
            flash_phase=args.phase,
        )
    except (OSError, RuntimeError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
    )
    print(json.dumps({"ok": report["ok"], "out": str(out)}, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
