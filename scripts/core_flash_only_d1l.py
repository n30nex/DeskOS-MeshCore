#!/usr/bin/env python3
"""Flash one exact Actions-built Core candidate to the verified D1L target."""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import json
import math
import os
import re
import select
import signal
import subprocess
import sys
import time
import traceback
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
    from core_install_recovery_review_d1l import (
        CORE_INSTALL_CONTRACT_KEYS,
        CORE_INSTALL_CONTRACT_SCHEMA,
        CORE_PACKAGE_SCHEMA,
        GENERATED_INSTALL_FILES,
        expected_normal_install_targets,
        expected_target_policy,
    )
    from core_smoke_d1l import (
        CORE_RELEASE_PROFILE,
        D1L_CORE_POSIX_TARGET,
        D1L_CORE_PORT,
        enforce_core_port,
        exact_identity,
        exact_version_identity,
        resolve_core_target,
    )
    from d1l_serial_target import POSIX_TARGET_KIND, safe_slug
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
    from scripts.core_install_recovery_review_d1l import (
        CORE_INSTALL_CONTRACT_KEYS,
        CORE_INSTALL_CONTRACT_SCHEMA,
        CORE_PACKAGE_SCHEMA,
        GENERATED_INSTALL_FILES,
        expected_normal_install_targets,
        expected_target_policy,
    )
    from scripts.core_smoke_d1l import (
        CORE_RELEASE_PROFILE,
        D1L_CORE_POSIX_TARGET,
        D1L_CORE_PORT,
        enforce_core_port,
        exact_identity,
        exact_version_identity,
        resolve_core_target,
    )
    from scripts.d1l_serial_target import POSIX_TARGET_KIND, safe_slug
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


EXPECTED_SD_HISTORY_MODE = "conditional"
EXPECTED_SD_HISTORY_STATE = "runtime_conditional_sd_primary"
EXPECTED_STORAGE_AUTHORITY = "sd_primary_live_only_without_sd"
EXPECTED_RP2040_ARTIFACT_NAMES = (
    "rp2040-sd-bridge-firmware",
)
EXPECTED_REPOSITORY = "n30nex/DeskOS-MeshCore"
EXPECTED_FLASH_ROLES = {
    0x0: ("bootloader", "bootloader/bootloader.bin"),
    0x8000: ("partition-table", "partition_table/partition-table.bin"),
    0xF000: ("ota-data", "ota_data_initial.bin"),
    0x20000: ("app", "meshcore_deskos_d1l.bin"),
}
FORBIDDEN_PORTS = frozenset({"COM8", "COM11", "COM16", "COM29"})
RETAINED_STATE_COMMANDS = (
    "contacts",
    "channels",
    "version",
    "health",
    "settings get",
    "wifi profiles",
    "messages public",
    "messages public offset 8",
    "messages dm",
    "messages dm offset 8",
    "messages unread",
    "identity status",
)
PUBLIC_RETAINED_PAGE_COMMANDS = (
    "messages public",
    "messages public offset 8",
)
PUBLIC_RETAINED_PAGE_SIZE = 8
DM_RETAINED_PAGE_COMMANDS = (
    "messages dm",
    "messages dm offset 8",
)
DM_RETAINED_PAGE_SIZE = 8
CONTACT_RETAINED_CAPACITY = 16
D1L_READ_STATE_CURSOR_CAPACITY = 16
CONTACT_RETENTION_REQUIRED_FIELDS = frozenset(
    {
        "alias",
        "created_ms",
        "favorite",
        "fingerprint",
        "muted",
        "public_key",
    }
)
CONTACT_RETENTION_VOLATILE_FIELDS = frozenset(
    {
        "last_heard_ms",
        "last_rssi_dbm",
        "last_snr_tenths",
        "out_path_known",
        "out_path_len",
        "out_path_updated_ms",
        "path_hash_bytes",
        "path_hops",
        "seq",
        "signed_advert_timestamp",
        "updated_ms",
        "verified_at_ms",
    }
)
EXPECTED_D1L_ROLE = "desk_companion"
FLASH_PHASE_BOOTSTRAP = "bootstrap"
FLASH_PHASE_RETAINED_REFLASH = "retained-reflash"
FLASH_PHASES = (
    FLASH_PHASE_BOOTSTRAP,
    FLASH_PHASE_RETAINED_REFLASH,
)
POST_FLASH_RESET_ASSERT_SECONDS = 0.2
POST_FLASH_RESET_RELEASE_SECONDS = 0.1
POST_FLASH_RESET_LINE_SEQUENCE = (
    "dtr_deassert_before_reset",
    "rts_assert_reset",
    "dtr_reaffirm_while_reset_asserted",
    "rts_release_reset",
    "dtr_reaffirm_after_release",
)
POST_FLASH_RESET_RESULT_KEYS = frozenset(
    {
        "schema",
        "kind",
        "ok",
        "method",
        "same_admitted_handle",
        "dtr_deasserted",
        "dtr_reaffirmed_after_release",
        "line_sequence",
        "reset_assert_seconds",
        "post_release_seconds",
        "admitted_target_stable_identity_sha256",
    }
)
MIN_POSIX_POST_FLASH_BOOT_SETTLE_SECONDS = 90.0
POST_FLASH_CAPTURE_BAUD = 115200
POST_FLASH_RESET_BINDING = "posix_exclusive_reopen_after_flash"
POST_FLASH_CAPTURE_BINDING = "same_fresh_reset_settle_handle"
POST_FLASH_BOOT_SETTLE_RESULT_KEYS = frozenset(
    {
        "schema",
        "kind",
        "ok",
        "method",
        "same_admitted_handle",
        "separate_from_flash_handle",
        "flash_handle_closed",
        "console_io_attempted",
        "settle_seconds",
        "admitted_target_stable_identity_sha256",
        "settled_target_stable_identity_sha256",
    }
)
POST_FLASH_CAPTURE_RESULT_KEYS = frozenset(
    {
        "schema",
        "kind",
        "ok",
        "method",
        "separate_from_flash_handle",
        "same_as_reset_settle_handle",
        "flash_handle_closed",
        "baudrate",
        "commands",
        "recovery_target_stable_identity_sha256",
        "settled_target_stable_identity_sha256",
    }
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
    if not verify_checksum_tree(package):
        raise ValueError("Core package checksum tree is incomplete or invalid")
    expected_manifests = {
        package / "SHA256SUMS.txt",
        *(
            package / "rp2040" / name / "SHA256SUMS.txt"
            for name in EXPECTED_RP2040_ARTIFACT_NAMES
        ),
    }
    manifest_path = package / "manifest.json"
    manifest = _load_json(manifest_path, "Core release manifest")
    workflow = manifest.get("workflow")
    git_info = manifest.get("git")
    install = manifest.get("install_recovery_guide")
    scripts = manifest.get("scripts")
    generated_files: dict[str, dict[str, Any]] = {}
    for relative in GENERATED_INSTALL_FILES:
        generated = _inside(package / relative, package, "generated install file")
        if not generated.is_file() or generated.stat().st_size <= 0:
            raise ValueError(f"Core generated install file is invalid: {relative}")
        generated_files[relative] = {
            "size": generated.stat().st_size,
            "sha256": sha256_file(generated),
        }
    rp2040_artifacts = manifest.get("rp2040_artifacts")
    rp2040_artifacts_ok = (
        isinstance(rp2040_artifacts, list)
        and [row.get("name") for row in rp2040_artifacts if isinstance(row, dict)]
        == list(EXPECTED_RP2040_ARTIFACT_NAMES)
        and all(
            isinstance(row, dict)
            and isinstance(row.get("uf2_files"), list)
            and bool(row["uf2_files"])
            and all(
                isinstance(relative, str)
                and relative.startswith("rp2040/")
                and _inside(
                    package / relative,
                    package,
                    "RP2040 package artifact",
                ).is_file()
                for relative in row["uf2_files"]
            )
            for row in rp2040_artifacts
        )
    )
    required_truth = (
        type(manifest.get("schema")) is int
        and manifest.get("schema") == CORE_PACKAGE_SCHEMA
        and manifest.get("release_profile") == CORE_RELEASE_PROFILE
        and exact_commit(manifest.get("firmware_commit")) == commit
        and str(manifest.get("actions_run")) == run_id
        and str(manifest.get("actions_run_attempt")) == run_attempt
        and manifest.get("sd_history_mode") == EXPECTED_SD_HISTORY_MODE
        and manifest.get("sd_history_state") == EXPECTED_SD_HISTORY_STATE
        and manifest.get("storage_authority") == EXPECTED_STORAGE_AUTHORITY
        and manifest.get("full_feature_release_ready") is False
        and rp2040_artifacts_ok
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
        and scripts
        == {
            "shared_project_flash": "flash_project.py",
            "serial_target_resolver": "d1l_serial_target.py",
            "windows_project_flash": "flash_project.ps1",
            "posix_project_flash": "flash_project.sh",
            "windows_update_flash": "flash_update_bin.ps1",
            "posix_update_flash": "flash_update_bin.sh",
            "windows_full_flash": "flash_full_8mb.ps1",
            "posix_full_flash": "flash_full_8mb.sh",
        }
        and isinstance(install, dict)
        and set(install) == CORE_INSTALL_CONTRACT_KEYS
        and type(install.get("schema")) is int
        and install.get("schema") == CORE_INSTALL_CONTRACT_SCHEMA
        and install.get("usb_only") is True
        and install.get("normal_install_script") == "flash_project.py"
        and install.get("normal_install_scripts")
        == {
            "windows": "flash_update_bin.ps1",
            "posix": "flash_update_bin.sh",
        }
        and install.get("normal_install_port") == D1L_CORE_POSIX_TARGET
        and install.get("normal_install_targets")
        == expected_normal_install_targets()
        and install.get("target_policy") == expected_target_policy()
        and install.get("normal_install_preserves_unrelated_nvs") is True
        and install.get("normal_install_package_root_only") is True
        and install.get("normal_install_checksum_verified") is True
        and install.get("recovery_script") == "flash_full_8mb.ps1"
        and install.get("recovery_platform") == "windows_and_posix"
        and install.get("posix_recovery_script") == "flash_full_8mb.sh"
        and install.get("recovery_requires_typed_confirmation") is True
        and install.get("recovery_checksum_verified") is True
        and install.get("recovery_target_identity_verified") is True
        and install.get("install_guide") == "docs/CORE_INSTALL_RECOVERY.md"
        and install.get("recovery_guide") == "docs/CORE_INSTALL_RECOVERY.md"
        and install.get("no_on_device_sd_format") is True
        and install.get("generated_files") == generated_files
    )
    if not required_truth:
        raise ValueError(
            "Core package manifest commit/run/profile/conditional-SD truth mismatch"
        )
    if set(manifests) != expected_manifests:
        raise ValueError(
            "Core package must have the complete valid root and RP2040 "
            "SHA256SUMS.txt manifests"
        )
    if not (package / "rp2040").is_dir() or (package / "update").exists():
        raise ValueError(
            "Conditional-SD Core package is missing RP2040 payloads or "
            "contains an excluded update payload"
        )

    action_rows = actions_verification.get("flash_files")
    if not isinstance(action_rows, list):
        raise ValueError("Actions flash verification has no flash_files")
    actions_by_offset = {
        row.get("offset"): row for row in action_rows if isinstance(row, dict)
    }
    package_rows = manifest.get("flash_files")
    if (
        not isinstance(package_rows, list)
        or len(package_rows) != len(EXPECTED_FLASH_ROLES)
    ):
        raise ValueError("Core package must contain exactly four project images")
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
        "storage_authority": EXPECTED_STORAGE_AUTHORITY,
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


@contextlib.contextmanager
def open_posix_admitted_serial(
    port: str,
    baud: int,
    timeout: float,
):
    """Keep the exact identity-admitted POSIX serial endpoint open."""
    if os.name != "posix":
        raise RuntimeError("POSIX serial admission requires a POSIX runtime")
    try:
        import fcntl
        import serial
        import termios
    except ImportError as exc:
        raise RuntimeError(
            "Linux tty locking and pyserial are required for bound "
            "POSIX D1L flashing"
        ) from exc
    handle = serial.Serial(
        port=None,
        baudrate=baud,
        timeout=timeout,
        exclusive=True,
    )
    try:
        # Match open_d1l_serial's ESP32-safe ordering.  pySerial applies DTR
        # before RTS on open, so caching both as false can briefly leave RTS
        # asserted by the driver while DTR is already deasserted and pulse EN.
        handle.dtr = True
        handle.rts = False
        handle.port = port
        handle.open()
        handle.dtr = False
        fcntl.ioctl(handle.fileno(), termios.TIOCEXCL)
    except BaseException:
        try:
            handle.close()
        except Exception:
            pass
        raise
    try:
        time.sleep(1.0)
        handle.reset_input_buffer()
        yield handle
    finally:
        handle.close()


def read_identity_status_from_handle(
    handle: Any,
    timeout: float,
    command_sender: Callable[[Any, str, float], dict],
) -> dict:
    return command_sender(handle, "identity status", timeout)


def _read_retained_state_command(
    handle: Any,
    command: str,
    timeout: float,
    command_sender: Callable[[Any, str, float], dict],
) -> dict:
    result = command_sender(handle, command, timeout)
    if result.get("ok") is True or result.get("code") != "TIMEOUT":
        return result
    time.sleep(1.0)
    retry_result = dict(command_sender(handle, command, timeout))
    retry_result["retry_count"] = 1
    return retry_result


def read_retained_state_from_handle(
    handle: Any,
    timeout: float,
    command_sender: Callable[[Any, str, float], dict],
) -> list[dict]:
    results: list[dict] = []
    for command in RETAINED_STATE_COMMANDS:
        result = _read_retained_state_command(
            handle,
            command,
            timeout,
            command_sender,
        )
        if command in (
            *PUBLIC_RETAINED_PAGE_COMMANDS,
            *DM_RETAINED_PAGE_COMMANDS,
        ):
            result = dict(result)
            result["capture_command"] = command
        results.append(result)
    return results


def _command_option(command: list[str], *names: str) -> str | None:
    for index, token in enumerate(command):
        if token in names and index + 1 < len(command):
            return command[index + 1]
        for name in names:
            prefix = f"{name}="
            if token.startswith(prefix):
                return token[len(prefix) :]
    return None


def _run_esptool_with_open_serial(
    command: list[str],
    serial_handle: Any,
) -> None:
    """Run esptool through an already-open serial object."""
    if len(command) < 4 or command[1:3] != ["-m", "esptool"]:
        raise ValueError("Unexpected esptool command prefix")
    chip = _command_option(command, "--chip")
    before = _command_option(command, "--before") or "default-reset"
    baud_text = _command_option(command, "--baud", "-b")
    if chip != "esp32s3" or baud_text is None:
        raise ValueError("Bound esptool command must select ESP32-S3 and baud")
    baud = int(baud_text)
    if baud <= 0:
        raise ValueError("Bound esptool baud must be positive")

    import esptool
    from esptool.cmds import detect_chip

    esp = detect_chip(
        port=serial_handle,
        baud=min(115200, baud),
        connect_mode=before,
    )
    detected = str(getattr(esp, "CHIP_NAME", "")).lower().replace("-", "")
    if detected != chip:
        raise RuntimeError(
            f"Bound esptool endpoint is {detected or 'unknown'}, not {chip}"
        )
    esptool.main(argv=command[3:], esp=esp)


def _arm_linux_parent_death_signal(expected_parent_pid: int) -> None:
    """Kill the flash child if its supervising parent disappears."""
    if not sys.platform.startswith("linux"):
        raise RuntimeError(
            "Bound POSIX flashing requires Linux parent-death signaling"
        )
    libc = ctypes.CDLL(None, use_errno=True)
    prctl = libc.prctl
    prctl.argtypes = [
        ctypes.c_int,
        ctypes.c_ulong,
        ctypes.c_ulong,
        ctypes.c_ulong,
        ctypes.c_ulong,
    ]
    prctl.restype = ctypes.c_int
    if prctl(1, signal.SIGKILL, 0, 0, 0) != 0:
        error_number = ctypes.get_errno()
        raise OSError(
            error_number,
            "Could not arm Linux parent-death signal",
        )
    if os.getppid() != expected_parent_pid:
        os.kill(os.getpid(), signal.SIGKILL)


def _kill_and_reap_child(pid: int) -> int | None:
    """Stop and reap one fork child, including an already-exited zombie."""
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    while True:
        try:
            waited_pid, status = os.waitpid(pid, 0)
        except InterruptedError:
            continue
        except ChildProcessError:
            return None
        return status if waited_pid == pid else None


def reset_bound_posix_target_after_flash(
    serial_handle: Any,
    admitted_target_stable_identity_sha256: str,
) -> dict:
    """Release the exact admitted ESP32 through one explicit EN pulse."""
    os.fstat(serial_handle.fileno())
    operation_error: BaseException | None = None
    try:
        serial_handle.dtr = False
        serial_handle.rts = True
        time.sleep(POST_FLASH_RESET_ASSERT_SECONDS)
    except BaseException as exc:
        operation_error = exc

    cleanup_errors: list[BaseException] = []
    for line, value in (
        ("dtr", False),
        ("rts", False),
        ("dtr", False),
    ):
        try:
            setattr(serial_handle, line, value)
        except BaseException as exc:
            cleanup_errors.append(exc)

    if operation_error is not None:
        raise operation_error
    if cleanup_errors:
        raise RuntimeError(
            "post-flash reset cleanup failed: "
            + "; ".join(
                f"{type(exc).__name__}: {exc}" for exc in cleanup_errors
            )
        )
    time.sleep(POST_FLASH_RESET_RELEASE_SECONDS)
    return {
        "schema": 1,
        "kind": "d1l_post_flash_reset",
        "ok": True,
        "method": "bound_posix_rts_en_pulse",
        "same_admitted_handle": True,
        "dtr_deasserted": True,
        "dtr_reaffirmed_after_release": True,
        "line_sequence": list(POST_FLASH_RESET_LINE_SEQUENCE),
        "reset_assert_seconds": POST_FLASH_RESET_ASSERT_SECONDS,
        "post_release_seconds": POST_FLASH_RESET_RELEASE_SECONDS,
        "admitted_target_stable_identity_sha256": (
            admitted_target_stable_identity_sha256
        ),
    }


def post_flash_reset_result_ok(
    reset: object,
    admitted_target: object,
) -> bool:
    if not isinstance(reset, dict) or not isinstance(admitted_target, dict):
        return False
    admitted_identity = admitted_target.get("stable_identity_sha256")
    return (
        set(reset) == POST_FLASH_RESET_RESULT_KEYS
        and reset.get("schema") == 1
        and reset.get("kind") == "d1l_post_flash_reset"
        and reset.get("ok") is True
        and reset.get("method") == "bound_posix_rts_en_pulse"
        and reset.get("same_admitted_handle") is True
        and reset.get("dtr_deasserted") is True
        and reset.get("dtr_reaffirmed_after_release") is True
        and reset.get("line_sequence")
        == list(POST_FLASH_RESET_LINE_SEQUENCE)
        and reset.get("reset_assert_seconds")
        == POST_FLASH_RESET_ASSERT_SECONDS
        and reset.get("post_release_seconds")
        == POST_FLASH_RESET_RELEASE_SECONDS
        and isinstance(admitted_identity, str)
        and re.fullmatch(r"[0-9a-f]{64}", admitted_identity) is not None
        and reset.get("admitted_target_stable_identity_sha256")
        == admitted_identity
    )


def post_flash_reset_contract_ok(receipt: object) -> bool:
    if not isinstance(receipt, dict):
        return False
    pre_flash_target = receipt.get("pre_flash_target_after_open")
    reset_target_before_open = receipt.get(
        "post_flash_reset_target_before_open"
    )
    reset_target_after_open = receipt.get(
        "post_flash_reset_target_after_open"
    )
    identities = [
        _target_stable_identity(target)
        for target in (
            pre_flash_target,
            reset_target_before_open,
            reset_target_after_open,
        )
    ]
    return (
        receipt.get("post_flash_reset_required") is True
        and receipt.get("post_flash_reset_ok") is True
        and receipt.get("post_flash_reset_error") is None
        and receipt.get("post_flash_reset_binding")
        == POST_FLASH_RESET_BINDING
        and receipt.get("post_flash_reset_binding_ok") is True
        and identities[0] is not None
        and all(identity == identities[0] for identity in identities[1:])
        and post_flash_reset_result_ok(
            receipt.get("post_flash_reset"),
            reset_target_after_open,
        )
    )


def _target_stable_identity(target: object) -> str | None:
    if not isinstance(target, dict):
        return None
    identity = target.get("stable_identity_sha256")
    if (
        not isinstance(identity, str)
        or re.fullmatch(r"[0-9a-f]{64}", identity) is None
    ):
        return None
    return identity


def post_flash_boot_settle_result_ok(
    settle: object,
    admitted_target: object,
    settled_target: object,
) -> bool:
    if not isinstance(settle, dict):
        return False
    admitted_identity = _target_stable_identity(admitted_target)
    settled_identity = _target_stable_identity(settled_target)
    settle_seconds = settle.get("settle_seconds")
    return (
        set(settle) == POST_FLASH_BOOT_SETTLE_RESULT_KEYS
        and settle.get("schema") == 1
        and settle.get("kind") == "d1l_post_flash_boot_settle"
        and settle.get("ok") is True
        and settle.get("method")
        == "fresh_reset_handle_hold_no_console_io"
        and settle.get("same_admitted_handle") is True
        and settle.get("separate_from_flash_handle") is True
        and settle.get("flash_handle_closed") is True
        and settle.get("console_io_attempted") is False
        and isinstance(settle_seconds, (int, float))
        and not isinstance(settle_seconds, bool)
        and math.isfinite(float(settle_seconds))
        and float(settle_seconds)
        >= MIN_POSIX_POST_FLASH_BOOT_SETTLE_SECONDS
        and admitted_identity is not None
        and settled_identity == admitted_identity
        and settle.get("admitted_target_stable_identity_sha256")
        == admitted_identity
        and settle.get("settled_target_stable_identity_sha256")
        == admitted_identity
    )


def post_flash_capture_result_ok(
    capture: object,
    recovery_target: object,
    settled_target: object,
) -> bool:
    if not isinstance(capture, dict):
        return False
    identities = [
        _target_stable_identity(target)
        for target in (
            recovery_target,
            settled_target,
        )
    ]
    if identities[0] is None or any(
        identity != identities[0] for identity in identities[1:]
    ):
        return False
    return (
        set(capture) == POST_FLASH_CAPTURE_RESULT_KEYS
        and capture.get("schema") == 1
        and capture.get("kind") == "d1l_post_flash_capture"
        and capture.get("ok") is True
        and capture.get("method") == "same_fresh_reset_settle_handle"
        and capture.get("separate_from_flash_handle") is True
        and capture.get("same_as_reset_settle_handle") is True
        and capture.get("flash_handle_closed") is True
        and capture.get("baudrate") == POST_FLASH_CAPTURE_BAUD
        and capture.get("commands") == list(RETAINED_STATE_COMMANDS)
        and capture.get("recovery_target_stable_identity_sha256")
        == identities[0]
        and capture.get("settled_target_stable_identity_sha256")
        == identities[0]
    )


def post_flash_capture_contract_ok(receipt: object) -> bool:
    if not isinstance(receipt, dict):
        return False
    recovery_target = receipt.get("post_flash_reset_target_after_open")
    settled_target = receipt.get("post_flash_target_after_settle")
    return (
        post_flash_reset_contract_ok(receipt)
        and receipt.get("post_flash_capture_binding")
        == POST_FLASH_CAPTURE_BINDING
        and receipt.get("post_flash_capture_binding_ok") is True
        and receipt.get("post_flash_capture_error") is None
        and receipt.get("target_identity_continuity_ok") is True
        and receipt.get("d1l_target_after")
        == settled_target
        and post_flash_boot_settle_result_ok(
            receipt.get("post_flash_boot_settle"),
            recovery_target,
            settled_target,
        )
        and post_flash_capture_result_ok(
            receipt.get("post_flash_capture"),
            recovery_target,
            settled_target,
        )
    )


def _serial_handle_is_closed(handle: object) -> bool:
    is_open = getattr(handle, "is_open", None)
    if isinstance(is_open, bool):
        return not is_open
    closed = getattr(handle, "closed", None)
    return closed is True


def default_posix_flash_runner(
    command: list[str],
    cwd: Path,
    timeout: int,
    serial_handle: Any,
) -> tuple[dict, bytes]:
    """Fork esptool with the exact serial file description admitted above."""
    started_at = utc_now()
    if (
        os.name != "posix"
        or not sys.platform.startswith("linux")
        or not hasattr(os, "fork")
    ):
        raise RuntimeError(
            "Bound POSIX flashing requires Linux fork descriptor inheritance"
        )
    os.fstat(serial_handle.fileno())
    sys.stdout.flush()
    sys.stderr.flush()
    read_fd, write_fd = os.pipe()
    parent_pid = os.getpid()
    try:
        pid = os.fork()
    except BaseException:
        os.close(read_fd)
        os.close(write_fd)
        raise
    if pid == 0:  # pragma: no cover - exercised by Linux hardware
        try:
            _arm_linux_parent_death_signal(parent_pid)
            os.close(read_fd)
            os.dup2(write_fd, 1)
            os.dup2(write_fd, 2)
            if write_fd not in {1, 2}:
                os.close(write_fd)
            sys.stdout = open(
                1,
                "w",
                buffering=1,
                encoding="utf-8",
                closefd=False,
            )
            sys.stderr = open(
                2,
                "w",
                buffering=1,
                encoding="utf-8",
                closefd=False,
            )
            os.chdir(cwd)
            _run_esptool_with_open_serial(command, serial_handle)
            sys.stdout.flush()
            sys.stderr.flush()
            os._exit(0)
        except BaseException:
            traceback.print_exc()
            sys.stdout.flush()
            sys.stderr.flush()
            os._exit(1)

    os.close(write_fd)
    raw_parts: list[bytes] = []
    status: int | None = None
    timed_out = False
    deadline = time.monotonic() + timeout
    try:
        while status is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                status = _kill_and_reap_child(pid)
                break
            readable, _, _ = select.select(
                [read_fd], [], [], min(0.1, remaining)
            )
            if readable:
                chunk = os.read(read_fd, 65536)
                if chunk:
                    raw_parts.append(chunk)
            waited_pid, waited_status = os.waitpid(pid, os.WNOHANG)
            if waited_pid == pid:
                status = waited_status
        while True:
            chunk = os.read(read_fd, 65536)
            if not chunk:
                break
            raw_parts.append(chunk)
    finally:
        try:
            if status is None:
                status = _kill_and_reap_child(pid)
        finally:
            os.close(read_fd)

    returncode = (
        None
        if timed_out or status is None
        else os.waitstatus_to_exitcode(status)
    )
    result = {
        "name": "esp32_flash",
        "ok": returncode == 0 and not timed_out,
        "returncode": returncode,
        "args": command,
        "cwd": str(cwd),
        "started_at": started_at,
        "ended_at": utc_now(),
        "serial_handoff": "fork_inherited_open_serial",
    }
    if timed_out:
        result["error"] = "timeout"
    return result, b"".join(raw_parts)


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
        results: list[dict] = []
        for command in RETAINED_STATE_COMMANDS:
            result = _read_retained_state_command(
                handle,
                command,
                timeout,
                send_console_command,
            )
            if command in (
                *PUBLIC_RETAINED_PAGE_COMMANDS,
                *DM_RETAINED_PAGE_COMMANDS,
            ):
                result = dict(result)
                result["capture_command"] = command
            results.append(result)
        return results


def retained_state_projection(results: object) -> dict | None:
    if not isinstance(results, list):
        return None
    by_command: dict[str, dict] = {}
    public_pages: dict[str, dict] = {}
    dm_pages: dict[str, dict] = {}
    for result in results:
        if not isinstance(result, dict):
            return None
        command = result.get("cmd")
        if not isinstance(command, str):
            return None
        if command in {"messages public", "messages dm"}:
            capture_command = result.get("capture_command")
            page_commands = (
                PUBLIC_RETAINED_PAGE_COMMANDS
                if command == "messages public"
                else DM_RETAINED_PAGE_COMMANDS
            )
            pages = public_pages if command == "messages public" else dm_pages
            if (
                capture_command not in page_commands
                or capture_command in pages
            ):
                return None
            pages[capture_command] = result
        else:
            if command in by_command:
                return None
            by_command[command] = result
    expected_commands = set(RETAINED_STATE_COMMANDS) - set(
        (*PUBLIC_RETAINED_PAGE_COMMANDS, *DM_RETAINED_PAGE_COMMANDS)
    )
    if (
        set(by_command) != expected_commands
        or set(public_pages) != set(PUBLIC_RETAINED_PAGE_COMMANDS)
        or set(dm_pages) != set(DM_RETAINED_PAGE_COMMANDS)
    ):
        return None
    if not all(
        result.get("ok") is True
        for result in (
            *by_command.values(),
            *public_pages.values(),
            *dm_pages.values(),
        )
    ):
        return None
    settings = by_command["settings get"]
    timezone = settings.get("timezone")
    map_location = settings.get("map_location")
    map_tiles = settings.get("map_tiles")
    radio = settings.get("radio")
    wifi_profiles = by_command["wifi profiles"]
    profiles = wifi_profiles.get("profiles")

    def contains_secret_field(value: object) -> bool:
        if isinstance(value, dict):
            for raw_key, child in value.items():
                key = str(raw_key).strip().lower().replace("-", "_")
                if key not in {
                    "password_saved",
                    "passwords_printed",
                    "secret_material_redacted",
                } and any(
                    marker in key
                    for marker in (
                        "password",
                        "passphrase",
                        "psk",
                        "secret",
                        "credential",
                        "token",
                        "private_key",
                    )
                ):
                    return True
                if contains_secret_field(child):
                    return True
        elif isinstance(value, list):
            return any(contains_secret_field(child) for child in value)
        return False

    settings_projection = {
        "node_name": settings.get("node_name"),
        "role": settings.get("role"),
        "onboarding_complete": settings.get("onboarding_complete"),
        "wifi_enabled": settings.get("wifi_enabled"),
        "ble_companion_enabled": settings.get("ble_companion_enabled"),
        "observer_enabled": settings.get("observer_enabled"),
        "high_contrast": settings.get("high_contrast"),
        "night_mode": settings.get("night_mode"),
        "path_hash_bytes": settings.get("path_hash_bytes"),
        "timezone": {
            key: timezone.get(key)
            for key in (
                "settings_ready",
                "settings_error",
                "schema_version",
                "offset_minutes",
            )
        } if isinstance(timezone, dict) else None,
        "map_location": {
            key: map_location.get(key)
            for key in ("set", "lat", "lon", "source")
        } if isinstance(map_location, dict) else None,
        "map_tile_zoom": (
            map_tiles.get("zoom") if isinstance(map_tiles, dict) else None
        ),
        "radio": {
            key: radio.get(key)
            for key in (
                "frequency_hz",
                "bandwidth_khz",
                "sf",
                "cr",
                "tx_power_dbm",
                "rx_boost",
                "tcxo",
            )
        } if isinstance(radio, dict) else None,
        "wifi_profiles": {
            "count": wifi_profiles.get("count"),
            "active_profile": wifi_profiles.get("active_profile"),
            "capacity": wifi_profiles.get("capacity"),
            "passwords_printed": wifi_profiles.get("passwords_printed"),
            "profiles": profiles,
        },
    }
    channels_result = by_command["channels"]
    channel_entries = channels_result.get("entries")
    channel_count = channels_result.get("count")
    channel_capacity = channels_result.get("capacity")
    channel_revision = channels_result.get("revision")
    active_channel_id = channels_result.get("active_channel_id")
    if (
        contains_secret_field(channels_result)
        or set(channels_result)
        != {
            "schema",
            "cmd",
            "ok",
            "count",
            "capacity",
            "revision",
            "active_channel_id",
            "entries",
            "persisted",
            "secret_material_redacted",
            "public_rf_tx",
            "formats_sd",
        }
        or isinstance(channel_count, bool)
        or not isinstance(channel_count, int)
        or channel_count < 1
        or channel_capacity != 8
        or channel_count > channel_capacity
        or isinstance(channel_revision, bool)
        or not isinstance(channel_revision, int)
        or channel_revision < 1
        or not isinstance(active_channel_id, str)
        or re.fullmatch(r"[0-9a-f]{16}", active_channel_id) is None
        or not isinstance(channel_entries, list)
        or len(channel_entries) != channel_count
        or channels_result.get("persisted") is not True
        or channels_result.get("secret_material_redacted") is not True
        or channels_result.get("public_rf_tx") is not False
        or channels_result.get("formats_sd") is not False
    ):
        return None
    channel_projection: list[dict] = []
    channel_cursors: dict[str, dict] = {}
    selected_ids: list[str] = []
    for channel in channel_entries:
        if (
            not isinstance(channel, dict)
            or set(channel)
            != {
                "channel_id",
                "name",
                "source",
                "enabled",
                "selected",
                "unread",
                "newest_message_seq",
                "read_through_seq",
            }
        ):
            return None
        channel_id = channel.get("channel_id")
        if (
            not isinstance(channel_id, str)
            or re.fullmatch(r"[0-9a-f]{16}", channel_id) is None
            or channel_id in channel_cursors
            or not isinstance(channel.get("name"), str)
            or not channel["name"]
            or channel.get("source")
            not in {"builtin", "manual", "uri_import", "migrated"}
            or type(channel.get("enabled")) is not bool
            or type(channel.get("selected")) is not bool
            or any(
                isinstance(channel.get(field), bool)
                or not isinstance(channel.get(field), int)
                or channel[field] < 0
                for field in (
                    "unread",
                    "newest_message_seq",
                    "read_through_seq",
                )
            )
        ):
            return None
        if channel["selected"]:
            selected_ids.append(channel_id)
        channel_projection.append(
            {
                key: channel[key]
                for key in (
                    "channel_id",
                    "name",
                    "source",
                    "enabled",
                    "selected",
                )
            }
        )
        channel_cursors[channel_id] = {
            key: channel[key]
            for key in (
                "unread",
                "newest_message_seq",
                "read_through_seq",
            )
        }
    if selected_ids != [active_channel_id]:
        return None
    channel_projection.sort(key=lambda row: row["channel_id"])
    if (
        not isinstance(settings_projection.get("node_name"), str)
        or not settings_projection["node_name"]
        or settings_projection.get("role") != EXPECTED_D1L_ROLE
        or contains_secret_field(settings)
        or contains_secret_field(wifi_profiles)
        or not {
            "node_name",
            "role",
            "onboarding_complete",
            "wifi_enabled",
            "ble_companion_enabled",
            "observer_enabled",
            "high_contrast",
            "night_mode",
            "path_hash_bytes",
            "timezone",
            "map_location",
            "map_tiles",
            "radio",
        }.issubset(settings)
        or any(
            type(settings_projection.get(field)) is not bool
            for field in (
                "onboarding_complete",
                "wifi_enabled",
                "ble_companion_enabled",
                "observer_enabled",
                "high_contrast",
                "night_mode",
            )
        )
        or settings_projection.get("path_hash_bytes") not in (1, 2, 3)
        or not isinstance(timezone, dict)
        or not {
            "settings_ready",
            "settings_error",
            "schema_version",
            "offset_minutes",
        }.issubset(timezone)
        or timezone.get("settings_ready") is not True
        or timezone.get("settings_error") != "ESP_OK"
        or isinstance(timezone.get("schema_version"), bool)
        or not isinstance(timezone.get("schema_version"), int)
        or isinstance(timezone.get("offset_minutes"), bool)
        or not isinstance(timezone.get("offset_minutes"), int)
        or not isinstance(map_location, dict)
        or not {"set", "lat", "lon", "source"}.issubset(map_location)
        or type(map_location.get("set")) is not bool
        or any(
            isinstance(map_location.get(field), bool)
            or not isinstance(map_location.get(field), (int, float))
            for field in ("lat", "lon")
        )
        or not isinstance(map_location.get("source"), str)
        or (
            map_location.get("set") is True
            and (
                not (-90.0 <= float(map_location.get("lat")) <= 90.0)
                or not (-180.0 <= float(map_location.get("lon")) <= 180.0)
                or map_location.get("source")
                not in {"manual", "authenticated_companion"}
            )
        )
        or (
            map_location.get("set") is False
            and (
                float(map_location.get("lat")) != 0.0
                or float(map_location.get("lon")) != 0.0
                or map_location.get("source")
                not in {"unset", "unavailable_in_release_profile"}
            )
        )
        or not isinstance(map_tiles, dict)
        or "zoom" not in map_tiles
        or isinstance(map_tiles.get("zoom"), bool)
        or not isinstance(map_tiles.get("zoom"), int)
        or not isinstance(radio, dict)
        or not {
            "frequency_hz",
            "bandwidth_khz",
            "sf",
            "cr",
            "tx_power_dbm",
            "rx_boost",
            "tcxo",
        }.issubset(radio)
        or any(
            isinstance(radio.get(field), bool)
            or not isinstance(radio.get(field), int)
            for field in ("frequency_hz", "sf", "cr", "tx_power_dbm")
        )
        or isinstance(radio.get("bandwidth_khz"), bool)
        or not isinstance(radio.get("bandwidth_khz"), (int, float))
        or type(radio.get("rx_boost")) is not bool
        or not isinstance(radio.get("tcxo"), str)
        or wifi_profiles.get("passwords_printed") is not False
        or set(wifi_profiles)
        != {
            "schema",
            "cmd",
            "ok",
            "count",
            "active_profile",
            "capacity",
            "passwords_printed",
            "profiles",
        }
        or isinstance(wifi_profiles.get("count"), bool)
        or not isinstance(wifi_profiles.get("count"), int)
        or isinstance(wifi_profiles.get("capacity"), bool)
        or not isinstance(wifi_profiles.get("capacity"), int)
        or isinstance(wifi_profiles.get("active_profile"), bool)
        or not isinstance(wifi_profiles.get("active_profile"), int)
        or not isinstance(profiles, list)
        or wifi_profiles.get("count") != len(profiles)
        or wifi_profiles.get("capacity") != 3
        or wifi_profiles.get("active_profile") < 0
        or wifi_profiles.get("active_profile") > wifi_profiles.get("count")
        or (
            bool(profiles)
            and wifi_profiles.get("active_profile") < 1
        )
        or not all(
            isinstance(profile, dict)
            and set(profile)
            == {"index", "active", "saved", "password_saved", "ssid"}
            and isinstance(profile.get("index"), int)
            and not isinstance(profile.get("index"), bool)
            and type(profile.get("active")) is bool
            and profile.get("saved") is True
            and type(profile.get("password_saved")) is bool
            and isinstance(profile.get("ssid"), str)
            and bool(profile.get("ssid"))
            for profile in profiles
        )
        or [profile["index"] for profile in profiles]
        != list(range(1, len(profiles) + 1))
        or sum(profile["active"] for profile in profiles)
        != (1 if profiles else 0)
        or (
            profiles
            and not profiles[wifi_profiles["active_profile"] - 1]["active"]
        )
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

    contact_result = by_command["contacts"]
    contacts = projected_entries("contacts")
    contact_persistence = contact_result.get("persistence")
    contact_sd = (
        contact_persistence.get("sd")
        if isinstance(contact_persistence, dict)
        else None
    )
    contact_count = contact_result.get("count")
    contact_capacity = contact_result.get("capacity")
    contact_next_seq = contact_result.get("next_seq")
    contact_total_written = contact_result.get("total_written")
    contact_dropped_oldest = contact_result.get("dropped_oldest")
    if (
        contacts is None
        or isinstance(contact_count, bool)
        or not isinstance(contact_count, int)
        or contact_count < 0
        or contact_count != len(contacts)
        or contact_capacity != CONTACT_RETAINED_CAPACITY
        or contact_count > contact_capacity
        or isinstance(contact_next_seq, bool)
        or not isinstance(contact_next_seq, int)
        or contact_next_seq < 1
        or any(
            isinstance(row.get("seq"), bool)
            or not isinstance(row.get("seq"), int)
            or row.get("seq") < 1
            or row.get("seq") >= contact_next_seq
            for row in contacts
        )
        or isinstance(contact_total_written, bool)
        or not isinstance(contact_total_written, int)
        or contact_total_written < contact_count
        or isinstance(contact_dropped_oldest, bool)
        or not isinstance(contact_dropped_oldest, int)
        or contact_dropped_oldest < 0
        or contact_result.get("persisted") is not True
        or not isinstance(contact_persistence, dict)
        or contact_persistence.get("loaded") is not True
        or contact_persistence.get("dirty") is not False
        or isinstance(contact_persistence.get("revision"), bool)
        or not isinstance(contact_persistence.get("revision"), int)
        or contact_persistence.get("revision") < 0
        or isinstance(contact_persistence.get("commits"), bool)
        or not isinstance(contact_persistence.get("commits"), int)
        or contact_persistence.get("commits") < 0
        or isinstance(contact_persistence.get("coalesced"), bool)
        or not isinstance(contact_persistence.get("coalesced"), int)
        or contact_persistence.get("coalesced") < 0
        or isinstance(contact_persistence.get("failures"), bool)
        or not isinstance(contact_persistence.get("failures"), int)
        or contact_persistence["failures"] < 0
        or contact_persistence.get("last_error") != "ESP_OK"
        or not isinstance(contact_sd, dict)
        or contact_sd.get("required") is not True
        or isinstance(contact_sd.get("generation"), bool)
        or not isinstance(contact_sd.get("generation"), int)
        or contact_sd.get("generation") < 1
        or contact_sd.get("reconcile_pending") is not False
    ):
        return None
    if contact_retention_projection(contacts) is None:
        return None

    read_state_result = by_command["messages unread"]
    read_state_persistence = read_state_result.get("persistence")
    read_state_sd = (
        read_state_persistence.get("sd")
        if isinstance(read_state_persistence, dict)
        else None
    )
    read_state_nvs = (
        read_state_persistence.get("nvs")
        if isinstance(read_state_persistence, dict)
        else None
    )
    read_state_threads = read_state_result.get("dm_threads")
    persisted_cursors = read_state_result.get("persisted_dm_cursors")
    if (
        any(
            isinstance(read_state_result.get(field), bool)
            or not isinstance(read_state_result.get(field), int)
            or read_state_result[field] < 0
            for field in (
                "public_unread",
                "dm_unread",
                "muted_dm_unread",
                "dm_thread_count",
                "persisted_dm_cursor_count",
                "last_public_read_seq",
                "last_dm_read_seq",
                "newest_public_rx_seq",
                "newest_dm_rx_seq",
                "mark_read_count",
            )
        )
        or read_state_result.get("cursor_capacity")
        != D1L_READ_STATE_CURSOR_CAPACITY
        or not isinstance(read_state_threads, list)
        or read_state_result.get("dm_thread_count")
        != len(read_state_threads)
        or not isinstance(persisted_cursors, list)
        or read_state_result.get("persisted_dm_cursor_count")
        != len(persisted_cursors)
        or len(persisted_cursors) > D1L_READ_STATE_CURSOR_CAPACITY
        or read_state_result.get("persisted") is not True
        or not isinstance(read_state_persistence, dict)
        or read_state_persistence.get("loaded") is not True
        or read_state_persistence.get("dirty") is not False
        or isinstance(read_state_persistence.get("revision"), bool)
        or not isinstance(read_state_persistence.get("revision"), int)
        or read_state_persistence.get("revision") < 0
        or isinstance(read_state_persistence.get("commits"), bool)
        or not isinstance(read_state_persistence.get("commits"), int)
        or read_state_persistence.get("commits") < 0
        or read_state_persistence.get("failures") != 0
        or read_state_persistence.get("last_error") != "ESP_OK"
        or read_state_persistence.get("clear_tombstone_pending") is not False
        or not isinstance(read_state_sd, dict)
        or read_state_sd.get("required") is not True
        or isinstance(read_state_sd.get("accepted_generation"), bool)
        or not isinstance(read_state_sd.get("accepted_generation"), int)
        or read_state_sd.get("accepted_generation") < 1
        or read_state_sd.get("generation")
        != read_state_sd.get("accepted_generation")
        or read_state_sd.get("dirty") is not False
        or read_state_sd.get("reconcile_pending") is not False
        or not isinstance(read_state_nvs, dict)
        or read_state_nvs.get("dirty") is not False
    ):
        return None
    seen_visible_threads: set[str] = set()
    for thread in read_state_threads:
        if not isinstance(thread, dict):
            return None
        fingerprint = thread.get("fingerprint")
        if (
            set(thread)
            != {
                "fingerprint",
                "last_read_seq",
                "newest_rx_seq",
                "unread",
                "muted",
            }
            or not isinstance(fingerprint, str)
            or re.fullmatch(r"[0-9A-Fa-f]{16}", fingerprint) is None
            or fingerprint.lower() in seen_visible_threads
            or any(
                isinstance(thread.get(field), bool)
                or not isinstance(thread.get(field), int)
                or thread[field] < 0
                for field in ("last_read_seq", "newest_rx_seq", "unread")
            )
            or thread["last_read_seq"] > thread["newest_rx_seq"]
            or type(thread.get("muted")) is not bool
        ):
            return None
        seen_visible_threads.add(fingerprint.lower())
    persisted_cursor_projection: dict[str, int] = {}
    for cursor in persisted_cursors:
        if not isinstance(cursor, dict):
            return None
        fingerprint = cursor.get("fingerprint")
        last_read_seq = cursor.get("last_read_seq")
        if (
            set(cursor) != {"fingerprint", "last_read_seq"}
            or not isinstance(fingerprint, str)
            or re.fullmatch(r"[0-9A-Fa-f]{16}", fingerprint) is None
            or fingerprint.lower() in persisted_cursor_projection
            or isinstance(last_read_seq, bool)
            or not isinstance(last_read_seq, int)
            or last_read_seq < 0
        ):
            return None
        persisted_cursor_projection[fingerprint.lower()] = last_read_seq

    public_result = public_pages["messages public"]
    public_persistence = public_result.get("persistence")
    public_sd = (
        public_persistence.get("sd")
        if isinstance(public_persistence, dict)
        else None
    )
    public_count = public_result.get("count")
    public_capacity = public_result.get("capacity")
    public_store_count = public_result.get("retained_store_count")
    public_retained_count = public_result.get("retained_public_count")
    public_total_written = public_result.get("total_written")
    public_dropped_oldest = public_result.get("dropped_oldest")
    public_epoch = public_result.get("retained_epoch")
    public_content_revision = public_result.get("content_revision")
    if (
        isinstance(public_count, bool)
        or not isinstance(public_count, int)
        or public_count < 0
        or isinstance(public_capacity, bool)
        or not isinstance(public_capacity, int)
        or public_capacity != PUBLIC_RETAINED_PAGE_SIZE * 2
        or public_count > public_capacity
        or isinstance(public_store_count, bool)
        or not isinstance(public_store_count, int)
        or public_store_count < public_count
        or public_store_count > public_capacity
        or public_retained_count != public_count
        or isinstance(public_total_written, bool)
        or not isinstance(public_total_written, int)
        or public_total_written < public_store_count
        or isinstance(public_dropped_oldest, bool)
        or not isinstance(public_dropped_oldest, int)
        or public_dropped_oldest < 0
        or isinstance(public_epoch, bool)
        or not isinstance(public_epoch, int)
        or public_epoch < 1
        or isinstance(public_content_revision, bool)
        or not isinstance(public_content_revision, int)
        or public_content_revision < 1
    ):
        return None
    public_rows: list[dict] = []
    public_sequences: set[int] = set()
    for capture_command, expected_offset in (
        ("messages public", 0),
        ("messages public offset 8", PUBLIC_RETAINED_PAGE_SIZE),
    ):
        page = public_pages[capture_command]
        rows = page.get("entries")
        expected_page_count = (
            min(public_count, PUBLIC_RETAINED_PAGE_SIZE)
            if expected_offset == 0
            else max(public_count - PUBLIC_RETAINED_PAGE_SIZE, 0)
        )
        expected_has_older = (
            expected_offset == 0
            and public_count > PUBLIC_RETAINED_PAGE_SIZE
        )
        expected_next_offset = (
            PUBLIC_RETAINED_PAGE_SIZE
            if expected_has_older or expected_offset > 0
            else 0
        )
        if (
            not isinstance(rows, list)
            or not all(isinstance(row, dict) for row in rows)
            or page.get("filtered") is not False
            or page.get("count") != public_count
            or page.get("capacity") != public_capacity
            or page.get("retained_store_count") != public_store_count
            or page.get("retained_public_count") != public_retained_count
            or page.get("offset") != expected_offset
            or page.get("page_size") != PUBLIC_RETAINED_PAGE_SIZE
            or page.get("page_count") != expected_page_count
            or page.get("page_count") != len(rows)
            or page.get("has_older") is not expected_has_older
            or page.get("next_offset") != expected_next_offset
            or page.get("total_matches") != public_retained_count
            or page.get("total_written") != public_total_written
            or page.get("dropped_oldest") != public_dropped_oldest
            or page.get("history_counters_scope") != "shared_all_channels"
            or page.get("retained_epoch") != public_epoch
            or page.get("content_revision") != public_content_revision
            or page.get("volatile_preview_present") is not False
            or page.get("volatile_preview_seq") != 0
            or page.get("persisted") is not True
            or page.get("persistence") != public_persistence
        ):
            return None
        for row in rows:
            sequence = row.get("seq")
            if (
                isinstance(sequence, bool)
                or not isinstance(sequence, int)
                or sequence < 1
                or sequence in public_sequences
                or row.get("retained") is not True
                or row.get("volatile_preview") is not False
            ):
                return None
            public_sequences.add(sequence)
        public_rows.extend(rows)
    public = sorted(
        public_rows,
        key=lambda row: json.dumps(
            row, sort_keys=True, separators=(",", ":")
        ),
    )
    direct_result = dm_pages["messages dm"]
    direct_persistence = direct_result.get("persistence")
    direct_sd = (
        direct_persistence.get("sd")
        if isinstance(direct_persistence, dict)
        else None
    )
    direct_count = direct_result.get("count")
    direct_capacity = direct_result.get("capacity")
    direct_retained_count = direct_result.get("retained_count")
    direct_total_written = direct_result.get("total_written")
    direct_dropped_oldest = direct_result.get("dropped_oldest")
    direct_epoch = direct_result.get("retained_epoch")
    direct_content_revision = direct_result.get("content_revision")
    if (
        isinstance(direct_count, bool)
        or not isinstance(direct_count, int)
        or direct_count < 0
        or isinstance(direct_capacity, bool)
        or not isinstance(direct_capacity, int)
        or direct_capacity != DM_RETAINED_PAGE_SIZE * 2
        or direct_count > direct_capacity
        or direct_retained_count != direct_count
        or isinstance(direct_total_written, bool)
        or not isinstance(direct_total_written, int)
        or direct_total_written < direct_count
        or isinstance(direct_dropped_oldest, bool)
        or not isinstance(direct_dropped_oldest, int)
        or direct_dropped_oldest < 0
        or isinstance(direct_content_revision, bool)
        or not isinstance(direct_content_revision, int)
        or direct_content_revision < 1
    ):
        return None
    direct_rows: list[dict] = []
    direct_sequences: set[int] = set()
    for capture_command, expected_offset in (
        ("messages dm", 0),
        ("messages dm offset 8", DM_RETAINED_PAGE_SIZE),
    ):
        page = dm_pages[capture_command]
        rows = page.get("entries")
        expected_page_count = (
            min(direct_count, DM_RETAINED_PAGE_SIZE)
            if expected_offset == 0
            else max(direct_count - DM_RETAINED_PAGE_SIZE, 0)
        )
        expected_has_older = (
            expected_offset == 0
            and direct_count > DM_RETAINED_PAGE_SIZE
        )
        expected_next_offset = (
            DM_RETAINED_PAGE_SIZE
            if expected_has_older or expected_offset > 0
            else 0
        )
        if (
            not isinstance(rows, list)
            or not all(isinstance(row, dict) for row in rows)
            or page.get("filtered") is not False
            or page.get("count") != direct_count
            or page.get("capacity") != direct_capacity
            or page.get("offset") != expected_offset
            or page.get("page_size") != DM_RETAINED_PAGE_SIZE
            or page.get("page_count") != expected_page_count
            or page.get("page_count") != len(rows)
            or page.get("has_older") is not expected_has_older
            or page.get("next_offset") != expected_next_offset
            or page.get("retained_count") != direct_retained_count
            or page.get("total_matches") != direct_retained_count
            or page.get("total_written") != direct_total_written
            or page.get("dropped_oldest") != direct_dropped_oldest
            or page.get("retained_epoch") != direct_epoch
            or page.get("content_revision") != direct_content_revision
            or page.get("volatile_preview_present") is not False
            or page.get("volatile_preview_seq") != 0
            or page.get("persisted") is not True
            or page.get("persistence") != direct_persistence
        ):
            return None
        for row in rows:
            sequence = row.get("seq")
            if (
                isinstance(sequence, bool)
                or not isinstance(sequence, int)
                or sequence < 1
                or sequence in direct_sequences
                or row.get("retained") is not True
                or row.get("volatile_preview") is not False
            ):
                return None
            direct_sequences.add(sequence)
        direct_rows.extend(rows)
    direct = sorted(
        direct_rows,
        key=lambda row: json.dumps(
            row, sort_keys=True, separators=(",", ":")
        ),
    )
    identity = by_command["identity status"]
    identity_public_key = exact_public_key(identity.get("public_key"))
    if (
        public is None
        or direct is None
        or contacts is None
        or isinstance(direct_retained_count, bool)
        or not isinstance(direct_retained_count, int)
        or isinstance(direct_epoch, bool)
        or not isinstance(direct_epoch, int)
        or direct_epoch < 1
        or not isinstance(direct_persistence, dict)
        or direct_result.get("persisted") is not True
        or direct_persistence.get("loaded") is not True
        or direct_persistence.get("dirty") is not False
        or not isinstance(direct_sd, dict)
        or direct_sd.get("required") is not True
        or direct_sd.get("dirty") is not False
        or direct_sd.get("reconcile_pending") is not False
        or direct_sd.get("last_error") != "ESP_OK"
        or direct_retained_count != direct_result.get("total_matches")
        or direct_retained_count != len(direct)
        or len(public) != public_retained_count
        or not isinstance(public_persistence, dict)
        or public_result.get("persisted") is not True
        or public_persistence.get("loaded") is not True
        or public_persistence.get("dirty") is not False
        or not isinstance(public_sd, dict)
        or public_sd.get("required") is not True
        or public_sd.get("dirty") is not False
        or public_sd.get("reconcile_pending") is not False
        or public_sd.get("last_error") != "ESP_OK"
        or identity_public_key is None
        or identity.get("public_key_ready") is not True
        or identity.get("fingerprint")
        != identity_public_key[:16].upper()
        or identity.get("role") != EXPECTED_D1L_ROLE
    ):
        return None
    return {
        "settings": settings_projection,
        "channels": channel_projection,
        "channel_state": {
            "capacity": channel_capacity,
            "active_channel_id": active_channel_id,
            "revision": channel_revision,
        },
        "channel_cursors": channel_cursors,
        "public_messages": public,
        "public_messages_state": {
            "count": public_count,
            "capacity": public_capacity,
            "retained_store_count": public_store_count,
            "retained_public_count": public_retained_count,
            "total_written": public_total_written,
            "dropped_oldest": public_dropped_oldest,
            "retained_epoch": public_epoch,
            "content_revision": public_content_revision,
        },
        "direct_messages": direct,
        "direct_messages_state": {
            "count": direct_count,
            "capacity": direct_capacity,
            "retained_count": direct_retained_count,
            "total_written": direct_total_written,
            "dropped_oldest": direct_dropped_oldest,
            "retained_epoch": direct_epoch,
            "content_revision": direct_content_revision,
        },
        "contacts": contacts,
        "contacts_state": {
            "count": contact_count,
            "capacity": contact_capacity,
            "next_seq": contact_next_seq,
            "total_written": contact_total_written,
            "dropped_oldest": contact_dropped_oldest,
            "persistence_revision": contact_persistence["revision"],
            "persistence_commits": contact_persistence["commits"],
            "persistence_coalesced": contact_persistence["coalesced"],
            "persistence_failures": contact_persistence["failures"],
        },
        "read_state": {
            "cursor_capacity": read_state_result["cursor_capacity"],
            "last_public_read_seq":
                read_state_result["last_public_read_seq"],
            "last_dm_read_seq": read_state_result["last_dm_read_seq"],
            "mark_read_count": read_state_result["mark_read_count"],
            "persisted_dm_cursors": persisted_cursor_projection,
        },
        "identity_public_key": identity_public_key,
    }


def projection_sha256(projection: dict) -> str:
    canonical = json.dumps(
        projection, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    import hashlib

    return hashlib.sha256(canonical).hexdigest()


def contact_retention_projection(rows: object) -> dict[str, dict] | None:
    """Key contacts by identity while excluding only live RF observations.

    All unlisted fields remain part of the comparison so aliases, user flags,
    capabilities, and future schema additions still fail closed on mutation.
    Contacts stay keyed by their validated short fingerprint so a legitimate
    heard-only placeholder can be promoted by a signed advert without looking
    like a deletion plus unrelated insertion. Full keys, once present, remain
    immutable; user-owned alias/favorite/muted/creation state is projected
    separately from RF-owned observations and capabilities.
    """
    if not isinstance(rows, list):
        return None
    projected: dict[str, dict] = {}
    seen_fingerprints: set[str] = set()
    required_fields = (
        CONTACT_RETENTION_REQUIRED_FIELDS
        | CONTACT_RETENTION_VOLATILE_FIELDS
    )
    for row in rows:
        if not isinstance(row, dict) or not required_fields.issubset(row):
            return None
        public_key_value = row.get("public_key")
        public_key = exact_public_key(public_key_value)
        fingerprint = row.get("fingerprint")
        alias = row.get("alias")
        created_ms = row.get("created_ms")
        normalized_fingerprint = (
            fingerprint.strip().lower()
            if isinstance(fingerprint, str)
            else ""
        )
        if (
            re.fullmatch(r"[0-9a-f]{16}", normalized_fingerprint) is None
            or normalized_fingerprint in seen_fingerprints
            or (
                public_key is not None
                and normalized_fingerprint != public_key[:16]
            )
            or (
                public_key is None
                and (
                    not isinstance(public_key_value, str)
                    or public_key_value != ""
                )
            )
            or not isinstance(alias, str)
            or not isinstance(row.get("favorite"), bool)
            or not isinstance(row.get("muted"), bool)
            or isinstance(created_ms, bool)
            or not isinstance(created_ms, int)
            or created_ms < 0
        ):
            return None
        canonical = row.get("canonical")
        if (
            type(canonical) is not bool
            or type(row.get("can_dm")) is not bool
            or type(row.get("can_admin")) is not bool
            or (public_key is not None and canonical is not True)
            or (
                public_key is None
                and (
                    canonical is not False
                    or row.get("can_dm") is not False
                    or row.get("can_admin") is not False
                )
            )
        ):
            return None
        projected[normalized_fingerprint] = {
            "fingerprint": normalized_fingerprint,
            "public_key": public_key or "",
            "alias": alias,
            "favorite": row["favorite"],
            "muted": row["muted"],
            "created_ms": created_ms,
        }
        seen_fingerprints.add(normalized_fingerprint)
    return projected


def retained_state_preserved(before: dict, after: dict) -> bool:
    if (
        before.get("settings") != after.get("settings")
        or before.get("channels") != after.get("channels")
        or before.get("identity_public_key")
        != after.get("identity_public_key")
        or before.get("direct_messages_state")
        != after.get("direct_messages_state")
    ):
        return False
    before_channel_state = before.get("channel_state")
    after_channel_state = after.get("channel_state")
    before_channel_cursors = before.get("channel_cursors")
    after_channel_cursors = after.get("channel_cursors")
    if not (
        isinstance(before_channel_state, dict)
        and isinstance(after_channel_state, dict)
        and before_channel_state.get("capacity")
        == after_channel_state.get("capacity")
        and before_channel_state.get("active_channel_id")
        == after_channel_state.get("active_channel_id")
        and isinstance(before_channel_state.get("revision"), int)
        and not isinstance(before_channel_state.get("revision"), bool)
        and isinstance(after_channel_state.get("revision"), int)
        and not isinstance(after_channel_state.get("revision"), bool)
        and after_channel_state["revision"]
        >= before_channel_state["revision"]
        and isinstance(before_channel_cursors, dict)
        and isinstance(after_channel_cursors, dict)
        and set(before_channel_cursors) == set(after_channel_cursors)
    ):
        return False
    for channel_id, before_cursor in before_channel_cursors.items():
        after_cursor = after_channel_cursors.get(channel_id)
        if not (
            isinstance(before_cursor, dict)
            and isinstance(after_cursor, dict)
            and all(
                isinstance(before_cursor.get(field), int)
                and not isinstance(before_cursor.get(field), bool)
                and isinstance(after_cursor.get(field), int)
                and not isinstance(after_cursor.get(field), bool)
                and after_cursor[field] >= before_cursor[field]
                for field in (
                    "unread",
                    "newest_message_seq",
                    "read_through_seq",
                )
            )
        ):
            return False
    before_public_state = before.get("public_messages_state")
    after_public_state = after.get("public_messages_state")
    if not (
        isinstance(before_public_state, dict)
        and isinstance(after_public_state, dict)
        and before_public_state.get("capacity")
        == after_public_state.get("capacity")
        and before_public_state.get("retained_epoch")
        == after_public_state.get("retained_epoch")
        and all(
            isinstance(before_public_state.get(field), int)
            and not isinstance(before_public_state.get(field), bool)
            and isinstance(after_public_state.get(field), int)
            and not isinstance(after_public_state.get(field), bool)
            and after_public_state[field] >= before_public_state[field]
            for field in (
                "count",
                "retained_store_count",
                "retained_public_count",
                "total_written",
                "dropped_oldest",
                "content_revision",
            )
        )
    ):
        return False
    for field in ("public_messages", "direct_messages"):
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
    before_read_state = before.get("read_state")
    after_read_state = after.get("read_state")
    if (
        not isinstance(before_read_state, dict)
        or not isinstance(after_read_state, dict)
        or before_read_state.get("cursor_capacity")
        != after_read_state.get("cursor_capacity")
        or any(
            isinstance(before_read_state.get(field), bool)
            or not isinstance(before_read_state.get(field), int)
            or isinstance(after_read_state.get(field), bool)
            or not isinstance(after_read_state.get(field), int)
            or after_read_state[field] < before_read_state[field]
            for field in (
                "last_public_read_seq",
                "last_dm_read_seq",
                "mark_read_count",
            )
        )
        or not isinstance(
            before_read_state.get("persisted_dm_cursors"), dict
        )
        or not isinstance(
            after_read_state.get("persisted_dm_cursors"), dict
        )
    ):
        return False
    for fingerprint, before_cursor in before_read_state[
        "persisted_dm_cursors"
    ].items():
        after_cursor = after_read_state["persisted_dm_cursors"].get(
            fingerprint
        )
        if (
            isinstance(before_cursor, bool)
            or not isinstance(before_cursor, int)
            or isinstance(after_cursor, bool)
            or not isinstance(after_cursor, int)
            or after_cursor < before_cursor
        ):
            return False
    before_contacts = contact_retention_projection(before.get("contacts"))
    after_contacts = contact_retention_projection(after.get("contacts"))
    before_contact_state = before.get("contacts_state")
    after_contact_state = after.get("contacts_state")
    if (
        before_contacts is None
        or after_contacts is None
        or not isinstance(before_contact_state, dict)
        or not isinstance(after_contact_state, dict)
        or before_contact_state.get("capacity")
        != after_contact_state.get("capacity")
        or any(
            isinstance(state.get("persistence_failures"), bool)
            or not isinstance(state.get("persistence_failures"), int)
            or state["persistence_failures"] < 0
            for state in (before_contact_state, after_contact_state)
        )
        or any(
            isinstance(before_contact_state.get(field), bool)
            or not isinstance(before_contact_state.get(field), int)
            or isinstance(after_contact_state.get(field), bool)
            or not isinstance(after_contact_state.get(field), int)
            or after_contact_state[field] < before_contact_state[field]
            for field in (
                "next_seq",
                "total_written",
                "dropped_oldest",
            )
        )
    ):
        return False
    for fingerprint, before_contact in before_contacts.items():
        after_contact = after_contacts.get(fingerprint)
        if not isinstance(after_contact, dict):
            return False
        if any(
            before_contact.get(field) != after_contact.get(field)
            for field in (
                "fingerprint",
                "alias",
                "favorite",
                "muted",
                "created_ms",
            )
        ):
            return False
        before_public_key = before_contact.get("public_key")
        after_public_key = after_contact.get("public_key")
        if (
            not isinstance(before_public_key, str)
            or not isinstance(after_public_key, str)
            or (
                before_public_key
                and after_public_key != before_public_key
            )
        ):
            return False
    return True


def retained_reflash_baseline_ready(projection: object) -> bool:
    """Require a real, non-empty durable DM witness for a closing reflash.

    An empty projection is a valid fresh-install state, but it cannot prove
    that a firmware replacement retained user data: empty is trivially a
    subset of empty.  The closing gate therefore requires at least one
    retained SD-primary DM row and stable store metadata.
    """
    if not isinstance(projection, dict):
        return False
    state = projection.get("direct_messages_state")
    rows = projection.get("direct_messages")
    return (
        isinstance(state, dict)
        and isinstance(rows, list)
        and len(rows) > 0
        and isinstance(state.get("retained_count"), int)
        and not isinstance(state.get("retained_count"), bool)
        and state["retained_count"] > 0
        and state["retained_count"] == len(rows)
        and isinstance(state.get("total_written"), int)
        and not isinstance(state.get("total_written"), bool)
        and state["total_written"] >= state["retained_count"]
        and isinstance(state.get("retained_epoch"), int)
        and not isinstance(state.get("retained_epoch"), bool)
        and state["retained_epoch"] >= 1
    )


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
    posix_serial_opener: Callable[
        [str, int, float], Any
    ] = open_posix_admitted_serial,
    posix_flash_runner: Callable[
        [list[str], Path, int, Any], tuple[dict, bytes]
    ] = default_posix_flash_runner,
    post_flash_resetter: Callable[
        [Any, str], dict
    ] = reset_bound_posix_target_after_flash,
    serial_command_sender: Callable[
        [Any, str, float], dict
    ] = send_console_command,
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

    posix_binding = (
        d1l_target_before.get("target_kind") == POSIX_TARGET_KIND
    )
    if posix_binding and serial_baud != POST_FLASH_CAPTURE_BAUD:
        raise ValueError(
            "Bound POSIX post-flash capture requires --serial-baud "
            f"{POST_FLASH_CAPTURE_BAUD}"
        )
    if (
        posix_binding
        and (
            isinstance(settle_sec, bool)
            or not isinstance(settle_sec, (int, float))
            or not math.isfinite(float(settle_sec))
            or float(settle_sec)
            < MIN_POSIX_POST_FLASH_BOOT_SETTLE_SECONDS
        )
    ):
        raise ValueError(
            "Bound POSIX post-flash capture requires --settle-sec "
            f">= {MIN_POSIX_POST_FLASH_BOOT_SETTLE_SECONDS:g}"
        )
    admission_context = (
        posix_serial_opener(port, serial_baud, serial_timeout)
        if posix_binding
        else contextlib.nullcontext(None)
    )
    pre_flash_identity: dict = {}
    pre_flash_target_after_open: dict[str, Any] | None = None
    before_projection: dict | None = None
    before_snapshot_row: dict | None = None
    before_build_commit: str | None = None
    post_flash_reset: dict[str, Any] = {}
    post_flash_reset_error: str | None = None
    post_flash_reset_target_before_open: dict[str, Any] | None = None
    post_flash_reset_target_after_open: dict[str, Any] | None = None
    post_flash_reset_binding = (
        POST_FLASH_RESET_BINDING
        if posix_binding
        else "not_required"
    )
    post_flash_reset_binding_ok = False
    version: dict = {}
    health: dict = {}
    after_results: list[dict] = []
    after_projection: dict | None = None
    after_snapshot_row: dict | None = None
    d1l_target_after: dict[str, Any] | None = None
    post_flash_target_after_settle: dict[str, Any] | None = None
    post_flash_boot_settle: dict[str, Any] = {}
    post_flash_capture: dict[str, Any] = {}
    target_continuity_ok = False
    target_post_error: str | None = None
    post_flash_identity: dict = {}
    post_flash_capture_binding = (
        POST_FLASH_CAPTURE_BINDING
        if posix_binding
        else "validated_path_reopen"
    )
    post_flash_capture_binding_ok = False
    post_flash_capture_error: str | None = None
    raw_log_row: dict[str, Any] | None = None
    with admission_context as admitted_handle:
        if posix_binding:
            if admitted_handle is None:
                raise RuntimeError(
                    "Bound POSIX serial admission returned no handle"
                )
            pre_flash_target_after_open = resolve_core_target(
                port,
                port_lister=port_lister,
                platform_name=platform_name,
            )
            if (
                pre_flash_target_after_open["stable_identity_sha256"]
                != d1l_target_before["stable_identity_sha256"]
            ):
                raise ValueError(
                    "D1L target changed while opening the admitted serial handle"
                )
            pre_flash_identity = read_identity_status_from_handle(
                admitted_handle,
                serial_timeout,
                serial_command_sender,
            )
        else:
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
        if flash_phase == FLASH_PHASE_RETAINED_REFLASH:
            before_results = (
                read_retained_state_from_handle(
                    admitted_handle,
                    serial_timeout,
                    serial_command_sender,
                )
                if posix_binding
                else retained_state_reader(
                    port, serial_baud, serial_timeout, 0.0
                )
            )
            before_projection = retained_state_projection(before_results)
            before_version = next(
                (
                    row
                    for row in before_results
                    if isinstance(row, dict) and row.get("cmd") == "version"
                ),
                {},
            )
            before_health = next(
                (
                    row
                    for row in before_results
                    if isinstance(row, dict) and row.get("cmd") == "health"
                ),
                {},
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
            before_build_commit = exact_commit(
                before_version.get("build_commit")
            )
            if not (
                before_projection is not None
                and retained_reflash_baseline_ready(before_projection)
                and before_build_commit is not None
                and exact_version_identity(
                    before_version,
                    before_build_commit,
                    EXPECTED_SD_HISTORY_MODE,
                )
                and exact_identity(
                    before_health,
                    before_build_commit,
                    EXPECTED_SD_HISTORY_MODE,
                )
                and before_health.get("board_ready") is True
                and before_health.get("ui_ready") is True
                and identity_status_ok(
                    before_identity,
                    normalized_public_key,
                )
            ):
                raise ValueError(
                    "Closing reflash baseline must be a ready compatible Core "
                    "candidate with the pinned D1L identity and a non-empty "
                    "clean SD-primary DM witness"
                )
            _, before_snapshot_row = write_state_snapshot(
                path=before_snapshot_path,
                root=root,
                phase="pre_flash",
                commit=before_build_commit,
                results=before_results,
                d1l_target=(
                    pre_flash_target_after_open
                    if pre_flash_target_after_open is not None
                    else d1l_target_before
                ),
            )
        if posix_binding:
            result, raw_log = posix_flash_runner(
                command,
                root,
                flash_timeout,
                admitted_handle,
            )
            raw_log_path.parent.mkdir(parents=True, exist_ok=True)
            raw_log_path.write_bytes(raw_log)
            raw_log_row = _relative_file_row(raw_log_path, root)
        else:
            result, raw_log = flash_runner(command, root, flash_timeout)
            raw_log_path.parent.mkdir(parents=True, exist_ok=True)
            raw_log_path.write_bytes(raw_log)
            raw_log_row = _relative_file_row(raw_log_path, root)
    if raw_log_row is None:
        raise RuntimeError("flash runner returned without a persisted raw log")
    post_flash_reset_required = posix_binding
    flash_succeeded = (
        result.get("name") == "esp32_flash"
        and result.get("ok") is True
        and result.get("returncode") == 0
        and result.get("args") == command
    )
    if posix_binding and flash_succeeded:
        flash_handle_closed = _serial_handle_is_closed(admitted_handle)
        reset_contract_complete = False
        try:
            if not flash_handle_closed:
                raise RuntimeError(
                    "original admitted flash handle was not closed "
                    "before recovery reset"
                )
            post_flash_reset_target_before_open = resolve_core_target(
                port,
                port_lister=port_lister,
                platform_name=platform_name,
            )
            recovery_identity = pre_flash_target_after_open[
                "stable_identity_sha256"
            ]
            if (
                post_flash_reset_target_before_open[
                    "stable_identity_sha256"
                ]
                != recovery_identity
            ):
                raise ValueError(
                    "D1L target changed before post-flash recovery reset"
                )
            recovery_results: list[dict]
            recovery_baudrate: object = None
            recovery_handle_distinct = False
            with posix_serial_opener(
                port,
                POST_FLASH_CAPTURE_BAUD,
                serial_timeout,
            ) as recovery_handle:
                if recovery_handle is admitted_handle:
                    raise RuntimeError(
                        "post-flash recovery reused the esptool-tainted "
                        "serial handle"
                    )
                recovery_handle_distinct = True
                recovery_baudrate = getattr(
                    recovery_handle, "baudrate", None
                )
                post_flash_reset_target_after_open = resolve_core_target(
                    port,
                    port_lister=port_lister,
                    platform_name=platform_name,
                )
                if (
                    post_flash_reset_target_after_open[
                        "stable_identity_sha256"
                    ]
                    != recovery_identity
                ):
                    raise ValueError(
                        "D1L target changed while opening the fresh "
                        "post-flash recovery handle"
                    )
                reset_value = post_flash_resetter(
                    recovery_handle,
                    recovery_identity,
                )
                if not isinstance(reset_value, dict):
                    raise TypeError(
                        "post-flash resetter returned "
                        f"{type(reset_value).__name__}, expected dict"
                    )
                post_flash_reset = reset_value
                if not post_flash_reset_result_ok(
                    post_flash_reset,
                    post_flash_reset_target_after_open,
                ):
                    raise RuntimeError(
                        "fresh-handle post-flash reset contract did not pass"
                    )
                time.sleep(settle_sec)
                post_flash_target_after_settle = resolve_core_target(
                    port,
                    port_lister=port_lister,
                    platform_name=platform_name,
                )
                settled_identity = post_flash_target_after_settle[
                    "stable_identity_sha256"
                ]
                d1l_target_after = post_flash_target_after_settle
                post_flash_boot_settle = {
                    "schema": 1,
                    "kind": "d1l_post_flash_boot_settle",
                    "ok": settled_identity == recovery_identity,
                    "method": "fresh_reset_handle_hold_no_console_io",
                    "same_admitted_handle": True,
                    "separate_from_flash_handle": (
                        recovery_handle_distinct
                    ),
                    "flash_handle_closed": flash_handle_closed,
                    "console_io_attempted": False,
                    "settle_seconds": float(settle_sec),
                    "admitted_target_stable_identity_sha256": (
                        recovery_identity
                    ),
                    "settled_target_stable_identity_sha256": (
                        settled_identity
                    ),
                }
                if not post_flash_boot_settle_result_ok(
                    post_flash_boot_settle,
                    post_flash_reset_target_after_open,
                    post_flash_target_after_settle,
                ):
                    raise RuntimeError(
                        "fresh-handle post-flash boot settle contract "
                        "did not pass"
                    )
                post_flash_reset_binding_ok = True
                reset_contract_complete = True
                recovery_handle.reset_input_buffer()
                recovery_results = read_retained_state_from_handle(
                    recovery_handle,
                    serial_timeout,
                    serial_command_sender,
                )
            after_results = recovery_results
            target_continuity_ok = True
            post_flash_capture = {
                "schema": 1,
                "kind": "d1l_post_flash_capture",
                "ok": True,
                "method": "same_fresh_reset_settle_handle",
                "separate_from_flash_handle": (
                    recovery_handle_distinct
                ),
                "same_as_reset_settle_handle": True,
                "flash_handle_closed": flash_handle_closed,
                "baudrate": recovery_baudrate,
                "commands": list(RETAINED_STATE_COMMANDS),
                "recovery_target_stable_identity_sha256": (
                    recovery_identity
                ),
                "settled_target_stable_identity_sha256": (
                    post_flash_target_after_settle[
                        "stable_identity_sha256"
                    ]
                ),
            }
            post_flash_capture_binding_ok = (
                post_flash_capture_result_ok(
                    post_flash_capture,
                    post_flash_reset_target_after_open,
                    post_flash_target_after_settle,
                )
            )
        except Exception as exc:
            error = f"{type(exc).__name__}: {exc}"
            if reset_contract_complete:
                post_flash_capture_error = error
            else:
                post_flash_reset_error = error
                post_flash_capture_error = (
                    "post-flash reset/settle contract did not pass"
                )
    post_flash_reset_ok = (
        not post_flash_reset_required
        or (
            post_flash_reset_binding_ok
            and post_flash_reset_result_ok(
                post_flash_reset,
                post_flash_reset_target_after_open,
            )
            and post_flash_boot_settle_result_ok(
                post_flash_boot_settle,
                post_flash_reset_target_after_open,
                post_flash_target_after_settle,
            )
        )
    )
    if (
        not posix_binding
        and flash_succeeded
        and post_flash_reset_ok
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
            post_flash_capture_binding_ok = True
    if target_continuity_ok:
        version = next(
            (
                row
                for row in after_results
                if isinstance(row, dict) and row.get("cmd") == "version"
            ),
            {},
        )
        health = next(
            (
                row
                for row in after_results
                if isinstance(row, dict) and row.get("cmd") == "health"
            ),
            {},
        )
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
    flash_serial_binding = (
        "posix_fork_inherited_open_serial"
        if posix_binding
        else "windows_com_path"
    )
    flash_serial_binding_ok = (
        result.get("serial_handoff") == "fork_inherited_open_serial"
        if posix_binding
        else True
    )
    identity_ok = (
        target_continuity_ok
        and exact_version_identity(
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
    ok = (
        result.get("ok") is True
        and identity_ok
        and flash_serial_binding_ok
        and post_flash_reset_ok
        and post_flash_capture_binding_ok
    )
    retained_preserved = (
        bool(
            before_projection is not None
            and after_projection is not None
            and retained_reflash_baseline_ready(before_projection)
            and retained_reflash_baseline_ready(after_projection)
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
        "dry_run": False,
        "simulated": False,
        "manual_only": False,
        "port": port,
        "d1l_target": d1l_target_before,
        "d1l_target_before": d1l_target_before,
        "pre_flash_target_after_open": pre_flash_target_after_open,
        "post_flash_reset_target_before_open": (
            post_flash_reset_target_before_open
        ),
        "post_flash_reset_target_after_open": (
            post_flash_reset_target_after_open
        ),
        "post_flash_target_after_settle": (
            post_flash_target_after_settle
        ),
        "d1l_target_after": d1l_target_after,
        "target_identity_continuity_ok": target_continuity_ok,
        "target_post_error": target_post_error,
        "flash_serial_binding": flash_serial_binding,
        "flash_serial_binding_ok": flash_serial_binding_ok,
        "post_flash_reset_required": post_flash_reset_required,
        "post_flash_reset_ok": post_flash_reset_ok,
        "post_flash_reset": post_flash_reset,
        "post_flash_reset_error": post_flash_reset_error,
        "post_flash_reset_binding": post_flash_reset_binding,
        "post_flash_reset_binding_ok": post_flash_reset_binding_ok,
        "post_flash_boot_settle": post_flash_boot_settle,
        "post_flash_capture": post_flash_capture,
        "post_flash_capture_binding": post_flash_capture_binding,
        "post_flash_capture_binding_ok": post_flash_capture_binding_ok,
        "post_flash_capture_error": post_flash_capture_error,
        "commit": normalized_commit,
        "github_actions_run": str(run_id),
        "workflow_run_attempt": str(run_attempt),
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": EXPECTED_SD_HISTORY_MODE,
        "expected_firmware_commit": normalized_commit,
        "pre_flash_build_commit": before_build_commit,
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
        "retained_nonempty_baseline": (
            retained_reflash_baseline_ready(before_projection)
            if flash_phase == FLASH_PHASE_RETAINED_REFLASH
            else None
        ),
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


def default_actions_capture_receipt(
    github_run_dir: Path,
    run_id: str,
) -> Path:
    return (
        github_run_dir
        / "core-actions-run-metadata"
        / f"core_actions_run_{run_id}.json"
    ).resolve()


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
    parser.add_argument(
        "--settle-sec",
        type=float,
        default=MIN_POSIX_POST_FLASH_BOOT_SETTLE_SECONDS,
    )
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
    actions_capture_receipt = (
        resolve_path(root, args.actions_capture_receipt)
        if args.actions_capture_receipt
        else default_actions_capture_receipt(run_dir, args.github_run_id)
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
