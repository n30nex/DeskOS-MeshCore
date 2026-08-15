#!/usr/bin/env python3
"""Autonomous hardware validation runner for MeshCore DeskOS D1L.

This orchestrates existing evidence-producing scripts around the expected
current hardware routing: ESP32 console/UART bridge plus intentional RP2040
USB/UF2 maintenance.
It intentionally refuses the protected mesh/radio ports.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from collections.abc import Iterable
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from artifact_metadata import git_metadata
    from flash_rp2040_sd_bridge_uf2 import (
        FlashGuardError,
        candidate_volumes,
        choose_volume,
        metadata_for_volume,
        parse_sha256sums,
        sha256_file,
    )
    from smoke_d1l import open_d1l_serial, send_console_command
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import git_metadata
    from scripts.flash_rp2040_sd_bridge_uf2 import (
        FlashGuardError,
        candidate_volumes,
        choose_volume,
        metadata_for_volume,
        parse_sha256sums,
        sha256_file,
    )
    from scripts.smoke_d1l import open_d1l_serial, send_console_command


DEFAULT_D1L_PORT = "COM" + "12"
DEFAULT_RP2040_PORT = "COM" + "16"
FORBIDDEN_PORTS = {"COM" + "8", "COM" + "11", "COM" + "29"}
DEFAULT_ONBOARDING_NAME = "D1L Desk"
POST_ESP32_FLASH_SETTLE_SEC = 8.0
SMOKE_RETRY_SETTLE_SEC = 8.0
OFFICIAL_SMOKE_UF2 = "seeed_official_sd_smoke.ino.uf2"
BRIDGE_UF2 = "deskos_sd_bridge.ino.uf2"
DEFAULT_SCROLL_SCREENS = "home,public_messages,dm_thread,nodes,packets,settings,storage,wifi,map"
BOOTLOADER_TOUCH_TIMEOUT_SECONDS = 3.0
BOOTLOADER_ENTRY_RESCAN_SECONDS = 3.0
RP2040_USB_VIDS = {"2E8A", "239A", "CAFE"}
RP2040_USB_KEYWORDS = (
    "rp2040",
    "raspberry pi pico",
    "pico",
    "bootsel",
    "circuitpython",
    "adafruit",
)
RP2040_DOUBLE_RESET_SWEEP_MS = (
    (50, 150, 1500),
    (30, 300, 1500),
    (100, 500, 2000),
)
RP2040_BAUD_PROBE_TIMEOUT_MS = 700
RP2040_BAUD_PROBE_COMMAND_TIMEOUT_SECONDS = 20.0
RP2040_RESET_COMMAND_TIMEOUT_SECONDS = 15.0
RP2040_RESTORE_PING_ATTEMPTS = 5
RP2040_RESTORE_PING_INTERVAL_SECONDS = 3.0
SD_FILE_CANARY_TIMEOUT_SECONDS = 45
SD_FILE_CANARY_MOUNT_WAIT_SECONDS = 60
RAW_DIAG_SETTLE_SECONDS = 20
RAW_DIAG_POLL_SECONDS = 2
RAW_DIAG_DEADLINE_SECONDS = 60
RAW_DIAG_PROCESS_TIMEOUT_SECONDS = 120
RETAINED_STORE_NAMES = ("messages", "dm", "routes", "packets")
SD_BOOT_PREPARE_SCENARIOS = (
    "no-card",
    "correct-structure",
    "missing-structure",
    "unformatted",
    "existing-data",
    "rp2040-unavailable",
)
AUTONOMOUS_SAFE_SD_SCENARIOS = ("correct-structure", "missing-structure", "existing-data")
HARDWARE_EXECUTION_DISABLED = (
    "Autonomous hardware execution is disabled until this legacy orchestrator "
    "uses the exact full-key D1L admission and stable flash binding. Use the "
    "narrow Core flash, smoke, UI, reboot, RF, and soak runners instead."
)


@dataclass(frozen=True)
class RunContext:
    root: Path
    commit: str
    short_commit: str
    github_run_id: str
    github_run_dir: Path
    d1l_port: str
    rp2040_port: str
    hardware_dir: Path
    rp2040_hardware_dir: Path
    baud: int
    esp32_flash_baud: int


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def normalize_port(value: str | None) -> str:
    return (value or "").strip().upper()


def enforce_port_guard(*ports: str | None) -> None:
    forbidden = sorted({normalize_port(port) for port in ports if normalize_port(port) in FORBIDDEN_PORTS})
    if forbidden:
        raise ValueError(f"Refusing to use forbidden port(s): {', '.join(forbidden)}")


def git_value(root: Path, *args: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip() or None


def resolve_commit(root: Path, value: str | None) -> str:
    commit = value or git_value(root, "rev-parse", "HEAD")
    if not commit:
        raise ValueError("Could not determine commit; pass --commit")
    commit = commit.strip().lower()
    if len(commit) != 40 or any(ch not in "0123456789abcdef" for ch in commit):
        raise ValueError(
            "Resolved commit must be a canonical 40-hex Git commit SHA"
        )
    return commit


def resolve_github_run_dir(root: Path, run_id: str | None, explicit_dir: str | None) -> Path:
    if explicit_dir:
        return (root / explicit_dir).resolve() if not Path(explicit_dir).is_absolute() else Path(explicit_dir)
    if not run_id:
        raise ValueError("Pass --github-run-id or --github-run-dir")
    current = root / "artifacts" / "github" / f"{run_id}-current"
    if current.exists():
        return current.resolve()
    return (root / "artifacts" / "github" / run_id).resolve()


def sha_from_run_dir(path: Path) -> str | None:
    for part in path.parts:
        if len(part) >= 40 and all(ch in "0123456789abcdefABCDEF" for ch in part[:40]):
            return part[:40]
    return None


def build_context(args: argparse.Namespace) -> RunContext:
    root = Path(args.root).resolve()
    d1l_port = normalize_port(args.d1l_port)
    rp2040_port = normalize_port(args.rp2040_port)
    enforce_port_guard(d1l_port, rp2040_port)
    if d1l_port != DEFAULT_D1L_PORT or rp2040_port != DEFAULT_RP2040_PORT:
        raise ValueError(
            "Autonomous D1L validation mutations are restricted to "
            f"{DEFAULT_D1L_PORT} and {DEFAULT_RP2040_PORT}"
        )
    if not args.github_run_id or not str(args.github_run_id).isdigit():
        raise ValueError(
            "Pass an explicit numeric --github-run-id for Actions provenance"
        )
    commit = resolve_commit(root, args.commit)
    run_dir = resolve_github_run_dir(root, args.github_run_id, args.github_run_dir)
    run_id = str(args.github_run_id)
    return RunContext(
        root=root,
        commit=commit,
        short_commit=commit[:7],
        github_run_id=run_id,
        github_run_dir=run_dir,
        d1l_port=d1l_port,
        rp2040_port=rp2040_port,
        hardware_dir=root / "artifacts" / "hardware" / d1l_port.lower(),
        rp2040_hardware_dir=root / "artifacts" / "hardware" / rp2040_port.lower(),
        baud=args.baud,
        esp32_flash_baud=args.esp32_flash_baud,
    )


def artifact_dirs(ctx: RunContext) -> dict[str, Path]:
    return {
        "esp32": ctx.github_run_dir / "d1l-firmware-artifacts",
        "esp32_build": ctx.github_run_dir / "d1l-firmware-artifacts" / "build",
        "bridge": ctx.github_run_dir / "rp2040-sd-bridge-firmware",
        "official_smoke": ctx.github_run_dir / "rp2040-seeed-official-sd-smoke-firmware",
    }


def rp2040_refresh_requested(args: argparse.Namespace) -> bool:
    return bool(args.refresh_rp2040_smoke and not args.skip_rp2040_official_smoke)


def required_artifact_dirs(ctx: RunContext, args: argparse.Namespace) -> dict[str, Path]:
    dirs = artifact_dirs(ctx)
    required = {
        "esp32": dirs["esp32"],
        "esp32_build": dirs["esp32_build"],
    }
    if rp2040_refresh_requested(args) or not args.skip_sd_suite:
        required["bridge"] = dirs["bridge"]
    if rp2040_refresh_requested(args):
        required["official_smoke"] = dirs["official_smoke"]
    return required


def official_sd_smoke_out(ctx: RunContext) -> Path:
    return ctx.rp2040_hardware_dir / f"rp2040_seeed_official_sd_smoke_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.rp2040_port}.json"


def bridge_restore_out(ctx: RunContext, phase: str = "initial") -> Path:
    phase_suffix = "" if phase == "initial" else f"-{phase}"
    return ctx.root / "artifacts" / "rp2040-flash" / (
        f"rp2040-bridge-restore-copy{phase_suffix}-{ctx.short_commit}-{ctx.rp2040_port}.json"
    )


def esp32_flash_out(ctx: RunContext, phase: str = "initial") -> Path:
    phase_suffix = "" if phase == "initial" else f"_{phase}"
    return ctx.hardware_dir / (
        f"esp32_flash{phase_suffix}_{ctx.short_commit}_actions_"
        f"{ctx.github_run_id}_{ctx.d1l_port}.json"
    )


def rp2040_access_precheck_out(ctx: RunContext) -> Path:
    return ctx.rp2040_hardware_dir / f"rp2040_access_precheck_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.rp2040_port}.json"


def require_path(path: Path, label: str) -> Path:
    if not path.exists():
        raise FileNotFoundError(f"missing {label}: {path}")
    return path


def verify_named_artifact(artifact_dir: Path, filename: str, expected_sha256: str | None = None) -> dict:
    artifact_dir = artifact_dir.resolve()
    target = require_path(artifact_dir / filename, filename)
    sums = parse_sha256sums(require_path(artifact_dir / "SHA256SUMS.txt", "SHA256SUMS.txt"))
    expected = sums.get(filename) or sums.get(f"./{filename}")
    if not expected:
        raise FlashGuardError(f"SHA256SUMS.txt does not list {filename}")
    actual = sha256_file(target)
    if actual.lower() != expected.lower():
        raise FlashGuardError(f"checksum mismatch for {filename}: expected {expected}, actual {actual}")
    if expected_sha256 and actual.lower() != expected_sha256.lower():
        raise FlashGuardError(
            f"checksum mismatch for expected sha: expected {expected_sha256}, actual {actual}"
        )
    return {
        "path": str(target),
        "size": target.stat().st_size,
        "sha256": actual.upper(),
        "manifest": str(artifact_dir / "SHA256SUMS.txt"),
    }


def verify_esp32_flash_inputs(ctx: RunContext) -> dict:
    artifact_root = artifact_dirs(ctx)["esp32"].resolve()
    build_dir = artifact_dirs(ctx)["esp32_build"].resolve()
    manifest_path = require_path(artifact_root / "SHA256SUMS.txt", "ESP32 SHA256SUMS.txt")
    checksums = parse_sha256sums(manifest_path)
    manifest_rows = [
        line for line in manifest_path.read_text(encoding="ascii").splitlines()
        if line.strip()
    ]
    if not checksums:
        raise FlashGuardError("ESP32 SHA256SUMS.txt is empty")
    if len(checksums) != len(manifest_rows):
        raise FlashGuardError("ESP32 SHA256SUMS.txt contains duplicate paths")

    actual_files = {
        path.relative_to(artifact_root).as_posix(): path
        for path in artifact_root.rglob("*")
        if path.is_file() and path != manifest_path
    }
    if set(checksums) != set(actual_files):
        missing = sorted(set(actual_files) - set(checksums))
        extra = sorted(set(checksums) - set(actual_files))
        raise FlashGuardError(
            f"ESP32 checksum coverage mismatch: unlisted={missing}, missing_files={extra}"
        )
    for relative, path in actual_files.items():
        actual = sha256_file(path)
        if actual.lower() != checksums[relative].lower():
            raise FlashGuardError(
                f"checksum mismatch for {relative}: expected {checksums[relative]}, actual {actual}"
            )

    flasher_path = require_path(build_dir / "flasher_args.json", "flasher_args.json")
    try:
        flasher = json.loads(flasher_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FlashGuardError(f"invalid flasher_args.json: {exc}") from exc
    flash_files = flasher.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise FlashGuardError("flasher_args.json has no flash_files")

    verified_files: list[dict] = []
    seen_offsets: set[int] = set()
    for raw_offset, raw_relative in sorted(
        flash_files.items(), key=lambda item: int(str(item[0]), 0)
    ):
        try:
            offset = int(str(raw_offset), 0)
        except ValueError as exc:
            raise FlashGuardError(f"invalid flash offset {raw_offset!r}") from exc
        if offset < 0 or offset in seen_offsets:
            raise FlashGuardError(f"duplicate or invalid flash offset {raw_offset!r}")
        seen_offsets.add(offset)
        target = (build_dir / str(raw_relative)).resolve()
        try:
            target.relative_to(build_dir)
            artifact_relative = target.relative_to(artifact_root).as_posix()
        except ValueError as exc:
            raise FlashGuardError(f"flash file escapes Actions artifact: {raw_relative!r}") from exc
        if not target.is_file() or artifact_relative not in checksums:
            raise FlashGuardError(f"unverified flash file: {artifact_relative}")
        verified_files.append(
            {
                "offset": offset,
                "offset_hex": hex(offset),
                "path": artifact_relative,
                "size": target.stat().st_size,
                "sha256": checksums[artifact_relative].upper(),
            }
        )

    required_roles = {
        0x0: "build/bootloader/bootloader.bin",
        0x8000: "build/partition_table/partition-table.bin",
        0xF000: "build/ota_data_initial.bin",
        0x20000: "build/meshcore_deskos_d1l.bin",
    }
    verified_by_offset = {item["offset"]: item["path"] for item in verified_files}
    if verified_by_offset != required_roles:
        raise FlashGuardError(
            "ESP32 flash roles must exactly match the non-erasing project image: "
            f"expected={required_roles}, actual={verified_by_offset}"
        )

    return {
        "ok": True,
        "artifact_root": str(artifact_root),
        "manifest": str(manifest_path),
        "manifest_sha256": sha256_file(manifest_path).upper(),
        "manifest_entry_count": len(checksums),
        "manifest_complete": True,
        "required_flash_roles": {hex(offset): path for offset, path in required_roles.items()},
        "flasher_args_sha256": sha256_file(flasher_path).upper(),
        "flash_files": verified_files,
    }


def load_unique_json(root: Path, pattern: str, label: str) -> tuple[Path, dict[str, Any]]:
    matches = sorted(path for path in root.glob(pattern) if path.is_file())
    if len(matches) != 1:
        raise FlashGuardError(
            f"expected exactly one {label} matching {pattern!r}, found {len(matches)}"
        )
    path = matches[0]
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FlashGuardError(f"invalid {label} {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise FlashGuardError(f"invalid {label} {path}: expected JSON object")
    return path, payload


def require_metadata_match(label: str, actual: Any, expected: Any) -> None:
    if actual != expected:
        raise FlashGuardError(
            f"Actions provenance mismatch for {label}: expected {expected!r}, got {actual!r}"
        )


def verify_manifest_artifact_group(
    manifest: dict[str, Any],
    package_root: Path,
    group_name: str,
    artifact_dir: Path,
) -> dict[str, Any]:
    groups = manifest.get("rp2040_artifacts")
    if not isinstance(groups, list):
        raise FlashGuardError("release manifest has no rp2040_artifacts list")
    matches = [
        group for group in groups
        if isinstance(group, dict) and group.get("name") == group_name
    ]
    if len(matches) != 1:
        raise FlashGuardError(
            f"release manifest must contain exactly one {group_name!r} group"
        )
    group = matches[0]
    group_path = str(group.get("path") or "").replace("\\", "/").rstrip("/")
    files = group.get("files")
    if not group_path or not isinstance(files, list) or not files:
        raise FlashGuardError(f"release manifest group {group_name!r} is incomplete")

    expected: dict[str, dict[str, Any]] = {}
    prefix = f"{group_path}/"
    for item in files:
        if not isinstance(item, dict):
            raise FlashGuardError(f"release manifest group {group_name!r} has invalid file entry")
        package_path = str(item.get("path") or "").replace("\\", "/")
        if not package_path.startswith(prefix):
            raise FlashGuardError(
                f"release manifest path {package_path!r} escapes group {group_name!r}"
            )
        relative = package_path[len(prefix):]
        if not relative or relative.startswith("/") or ".." in Path(relative).parts:
            raise FlashGuardError(f"invalid release manifest artifact path {package_path!r}")
        if relative in expected:
            raise FlashGuardError(
                f"duplicate release manifest artifact path for {group_name!r}: {relative}"
            )
        expected[relative] = item

    actual = {
        path.relative_to(artifact_dir).as_posix(): path
        for path in artifact_dir.rglob("*")
        if path.is_file()
    }
    if set(actual) != set(expected):
        raise FlashGuardError(
            f"Actions provenance file-set mismatch for {group_name}: "
            f"unlisted={sorted(set(actual) - set(expected))}, "
            f"missing={sorted(set(expected) - set(actual))}"
        )

    verified_files: list[dict[str, Any]] = []
    for relative, path in sorted(actual.items()):
        item = expected[relative]
        actual_sha = sha256_file(path).lower()
        expected_sha = str(item.get("sha256") or "").lower()
        require_metadata_match(f"{group_name}/{relative} sha256", actual_sha, expected_sha)
        require_metadata_match(f"{group_name}/{relative} size", path.stat().st_size, item.get("size"))
        packaged = (package_root / str(item["path"])).resolve()
        try:
            packaged.relative_to(package_root.resolve())
        except ValueError as exc:
            raise FlashGuardError(
                f"release package path escapes package root: {item['path']!r}"
            ) from exc
        require_path(packaged, f"packaged {group_name}/{relative}")
        require_metadata_match(
            f"packaged {group_name}/{relative} sha256",
            sha256_file(packaged).lower(),
            expected_sha,
        )
        require_metadata_match(
            f"packaged {group_name}/{relative} size",
            packaged.stat().st_size,
            item.get("size"),
        )
        verified_files.append(
            {
                "path": relative,
                "size": path.stat().st_size,
                "sha256": actual_sha.upper(),
            }
        )
    return {
        "name": group_name,
        "artifact_dir": str(artifact_dir.resolve()),
        "files": verified_files,
    }


def verify_actions_artifact_provenance(
    ctx: RunContext, args: argparse.Namespace
) -> dict[str, Any]:
    run_root = ctx.github_run_dir.resolve()
    marker_path, marker = load_unique_json(
        run_root,
        "d1l-host-artifacts/host-checks/d1l_host_checks_success_*.json",
        "host-checks success marker",
    )
    require_metadata_match("host marker artifact_type", marker.get("artifact_type"), "d1l_host_checks_success")
    require_metadata_match("host marker status", marker.get("status"), "pass")
    require_metadata_match("host marker passed", marker.get("passed"), True)
    require_metadata_match("host marker all_prior_steps_completed", marker.get("all_prior_steps_completed"), True)
    require_metadata_match("host marker job", marker.get("job"), "host-checks")
    require_metadata_match("host marker commit", marker.get("repository_commit"), ctx.commit)
    require_metadata_match("host marker workflow run", str(marker.get("workflow_run_id")), ctx.github_run_id)

    manifest_path, manifest = load_unique_json(
        run_root,
        "d1l-release-package/d1l-release-*/manifest.json",
        "D1L release manifest",
    )
    git_info = manifest.get("git") if isinstance(manifest.get("git"), dict) else {}
    workflow = manifest.get("workflow") if isinstance(manifest.get("workflow"), dict) else {}
    require_metadata_match("release manifest git.commit", git_info.get("commit"), ctx.commit)
    require_metadata_match("release manifest git.dirty", git_info.get("dirty"), False)
    require_metadata_match("release manifest workflow.run_id", str(workflow.get("run_id")), ctx.github_run_id)
    require_metadata_match("release manifest workflow.sha", workflow.get("sha"), ctx.commit)
    require_metadata_match("release manifest workflow.repository", workflow.get("repository"), "n30nex/SIGUI")
    marker_attempt = str(marker.get("workflow_run_attempt"))
    manifest_attempt = str(workflow.get("run_attempt"))
    if not marker_attempt.isdigit() or not manifest_attempt.isdigit():
        raise FlashGuardError("Actions provenance has an invalid workflow run attempt")
    require_metadata_match("workflow run attempt", marker_attempt, manifest_attempt)
    require_metadata_match("release manifest package", manifest.get("package"), f"d1l-release-{ctx.commit}")
    require_metadata_match("release manifest directory", manifest_path.parent.name, f"d1l-release-{ctx.commit}")

    flash_entries = manifest.get("flash_files")
    if not isinstance(flash_entries, list):
        raise FlashGuardError("release manifest has no flash_files list")
    expected_roles = {
        "bootloader": "bootloader/bootloader.bin",
        "partition-table": "partition_table/partition-table.bin",
        "ota-data": "ota_data_initial.bin",
        "app": "meshcore_deskos_d1l.bin",
    }
    flash_by_role = {
        str(item.get("role")): item
        for item in flash_entries
        if isinstance(item, dict)
    }
    if set(flash_by_role) != set(expected_roles):
        raise FlashGuardError(
            f"release manifest flash roles mismatch: expected={sorted(expected_roles)}, "
            f"actual={sorted(flash_by_role)}"
        )

    esp32_build = artifact_dirs(ctx)["esp32_build"].resolve()
    verified_esp32: list[dict[str, Any]] = []
    for role, expected_source in expected_roles.items():
        item = flash_by_role[role]
        require_metadata_match(f"release manifest {role} source", item.get("source"), expected_source)
        source = (esp32_build / expected_source).resolve()
        try:
            source.relative_to(esp32_build)
        except ValueError as exc:
            raise FlashGuardError(f"release manifest {role} source escapes ESP32 artifact") from exc
        require_path(source, f"ESP32 {role} artifact")
        actual_sha = sha256_file(source).lower()
        require_metadata_match(f"ESP32 {role} sha256", actual_sha, str(item.get("sha256") or "").lower())
        require_metadata_match(f"ESP32 {role} size", source.stat().st_size, item.get("size"))
        packaged = (manifest_path.parent / str(item.get("path") or "")).resolve()
        try:
            packaged.relative_to(manifest_path.parent.resolve())
        except ValueError as exc:
            raise FlashGuardError(
                f"release manifest {role} package path escapes release package"
            ) from exc
        require_path(packaged, f"packaged ESP32 {role} artifact")
        require_metadata_match(
            f"packaged ESP32 {role} sha256",
            sha256_file(packaged).lower(),
            actual_sha,
        )
        require_metadata_match(
            f"packaged ESP32 {role} size",
            packaged.stat().st_size,
            item.get("size"),
        )
        verified_esp32.append(
            {
                "role": role,
                "path": expected_source,
                "size": source.stat().st_size,
                "sha256": actual_sha.upper(),
            }
        )

    verified_rp2040: list[dict[str, Any]] = []
    if rp2040_refresh_requested(args) or not args.skip_sd_suite:
        verified_rp2040.append(
            verify_manifest_artifact_group(
                manifest,
                manifest_path.parent,
                "rp2040-sd-bridge-firmware",
                artifact_dirs(ctx)["bridge"].resolve(),
            )
        )
    if rp2040_refresh_requested(args):
        verified_rp2040.append(
            verify_manifest_artifact_group(
                manifest,
                manifest_path.parent,
                "rp2040-seeed-official-sd-smoke-firmware",
                artifact_dirs(ctx)["official_smoke"].resolve(),
            )
        )

    return {
        "ok": True,
        "commit": ctx.commit,
        "github_actions_run": ctx.github_run_id,
        "workflow_run_attempt": marker_attempt,
        "repository": "n30nex/SIGUI",
        "host_success_marker": str(marker_path),
        "host_success_marker_sha256": sha256_file(marker_path).upper(),
        "release_manifest": str(manifest_path),
        "release_manifest_sha256": sha256_file(manifest_path).upper(),
        "esp32_flash_files": verified_esp32,
        "rp2040_artifact_groups": verified_rp2040,
    }


def command_report(name: str, args: list[str], cwd: Path, timeout: int | None = None) -> dict:
    started_at = utc_now()
    try:
        result = subprocess.run(
            args,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
    except Exception as exc:  # pragma: no cover - OS/tooling dependent
        return {
            "name": name,
            "ok": False,
            "args": args,
            "cwd": str(cwd),
            "started_at": started_at,
            "ended_at": utc_now(),
            "error": str(exc),
        }
    return {
        "name": name,
        "ok": result.returncode == 0,
        "returncode": result.returncode,
        "args": args,
        "cwd": str(cwd),
        "started_at": started_at,
        "ended_at": utc_now(),
        "stdout_tail": result.stdout[-4000:],
        "stderr_tail": result.stderr[-4000:],
    }


def terminate_process_tree(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    if os.name == "nt":
        run_kwargs: dict[str, Any] = {}
        if hasattr(subprocess, "CREATE_NO_WINDOW"):
            run_kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
        try:
            subprocess.run(
                ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5,
                **run_kwargs,
            )
            return
        except Exception:
            pass
    try:
        proc.kill()
    except Exception:
        pass


def esptool_flash_command(
    build_dir: Path,
    port: str,
    baud: int,
    flash_offsets: Iterable[int] | None = None,
) -> list[str]:
    flasher = json.loads(require_path(build_dir / "flasher_args.json", "flasher_args.json").read_text())
    extra = flasher.get("extra_esptool_args", {})
    write_args = [str(item).replace("_", "-") if str(item).startswith("--") else str(item) for item in flasher.get("write_flash_args", [])]
    flash_files = flasher.get("flash_files", {})
    ordered_files = sorted(flash_files.items(), key=lambda item: int(str(item[0]), 0))
    if flash_offsets is not None:
        selected = {int(offset) for offset in flash_offsets}
        available = {int(str(offset), 0) for offset, _ in ordered_files}
        if not selected or not selected <= available:
            raise FlashGuardError("requested flash offsets are empty or unavailable")
        ordered_files = [
            item for item in ordered_files if int(str(item[0]), 0) in selected
        ]
    file_args: list[str] = []
    for offset, rel_path in ordered_files:
        file_args.extend([str(offset), str((build_dir / str(rel_path)).resolve())])
    return [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        str(extra.get("chip", "esp32s3")),
        "-p",
        port,
        "-b",
        str(baud),
        "--before",
        str(extra.get("before", "default_reset")).replace("_", "-"),
        "--after",
        str(extra.get("after", "hard_reset")).replace("_", "-"),
        "write-flash",
        *write_args,
        *file_args,
    ]


def write_json(path: Path, payload: dict) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding="ascii")
    return path


def flash_esp32(ctx: RunContext, *, dry_run: bool, phase: str = "initial") -> dict:
    build_dir = artifact_dirs(ctx)["esp32_build"]
    out = esp32_flash_out(ctx, phase)
    try:
        artifact_verification = verify_esp32_flash_inputs(ctx)
        cmd = esptool_flash_command(build_dir, ctx.d1l_port, ctx.esp32_flash_baud)
    except Exception as exc:
        report = {
            "schema": 1,
            "kind": "esp32_flash",
            "mode": "dry-run" if dry_run else "hardware",
            "port": ctx.d1l_port,
            "commit": ctx.commit,
            "github_actions_run": ctx.github_run_id,
            "phase": phase,
            "public_rf_tx": False,
            "formats_sd": False,
            "artifact_verification": {"ok": False, "error": str(exc)},
            "ok": False,
        }
        write_json(out, report)
        report["path"] = str(out)
        return report
    report = {
        "schema": 1,
        "kind": "esp32_flash",
        "mode": "dry-run" if dry_run else "hardware",
        "port": ctx.d1l_port,
        "commit": ctx.commit,
        "github_actions_run": ctx.github_run_id,
        "phase": phase,
        "artifact_verification": artifact_verification,
        "public_rf_tx": False,
        "formats_sd": False,
        "command": cmd,
        "post_flash_settle_sec": POST_ESP32_FLASH_SETTLE_SEC,
        "ok": True,
    }
    if not dry_run:
        report["result"] = command_report("esp32_flash", cmd, ctx.root, timeout=240)
        report["ok"] = report["result"].get("ok") is True
        if report["ok"]:
            report["flashed_at"] = utc_now()
            time.sleep(POST_ESP32_FLASH_SETTLE_SEC)
    write_json(out, report)
    report["path"] = str(out)
    return report


def wait_for_uf2_volume(timeout_sec: float, volume: str | None) -> tuple[Path, list[dict]]:
    if not volume:
        raise FlashGuardError(
            "correlated or explicit RP2040 UF2 volume is required; refusing automatic selection"
        )
    deadline = time.monotonic() + timeout_sec
    last_candidates: list[dict] = []
    while True:
        try:
            selected, candidates = choose_volume(Path(volume))
            return selected, candidates
        except FlashGuardError:
            last_candidates = candidate_volumes()
            if time.monotonic() >= deadline:
                raise FlashGuardError(
                    f"RP2040 UF2 bootloader volume not found; candidates={last_candidates}"
                )
            time.sleep(0.5)


def uf2_volume_snapshot() -> dict[str, Any]:
    try:
        candidates = candidate_volumes()
    except Exception as exc:  # pragma: no cover - host/OS dependent
        return {"available": False, "candidates": [], "error": str(exc)}
    return {"available": bool(candidates), "candidates": candidates}


def uf2_candidate_path(candidate: dict[str, Any]) -> str | None:
    for key in ("path", "drive", "mount_point", "volume"):
        value = candidate.get(key)
        if value:
            return str(value)
    return None


def uf2_volume_key(value: str | Path) -> str:
    text = str(value).strip().strip('"').replace("/", "\\")
    return text.rstrip("\\").upper()


def uf2_volume_keys(snapshot: dict[str, Any]) -> set[str]:
    keys: set[str] = set()
    candidates = snapshot.get("candidates")
    if not isinstance(candidates, list):
        return keys
    for candidate in candidates:
        if not isinstance(candidate, dict):
            continue
        path = uf2_candidate_path(candidate)
        if path:
            keys.add(uf2_volume_key(path))
    return keys


def select_correlated_uf2_volume(
    initial_keys: set[str],
    snapshot: dict[str, Any],
    *,
    explicit_volume: str | None,
) -> tuple[str | None, list[str]]:
    candidates = snapshot.get("candidates")
    if not isinstance(candidates, list):
        return None, []
    available: dict[str, str] = {}
    for candidate in candidates:
        if not isinstance(candidate, dict):
            continue
        path = uf2_candidate_path(candidate)
        if path:
            available[uf2_volume_key(path)] = path

    if explicit_volume:
        selected = available.get(uf2_volume_key(explicit_volume))
        return selected, [selected] if selected else []

    new_keys = sorted(set(available) - initial_keys)
    new_paths = [available[key] for key in new_keys]
    if len(new_paths) > 1:
        raise FlashGuardError(
            "multiple UF2 volumes appeared after the commanded RP2040 transition; "
            "pass --uf2-volume explicitly"
        )
    return (new_paths[0] if new_paths else None), new_paths


def rp2040_port_snapshot(port: str) -> dict[str, Any]:
    target = normalize_port(port)
    report: dict[str, Any] = {"port": target, "present": False, "matches": []}
    try:
        import serial.tools.list_ports
    except ImportError as exc:  # pragma: no cover - dependency dependent
        report["error"] = f"pyserial is required: {exc}"
        return report

    for item in serial.tools.list_ports.comports():
        device = str(getattr(item, "device", "") or "")
        if normalize_port(device) != target:
            continue
        match: dict[str, Any] = {"device": device}
        for attr in ["description", "hwid", "manufacturer", "product", "serial_number"]:
            value = getattr(item, attr, None)
            if value:
                match[attr] = str(value)
        for attr in ["vid", "pid"]:
            value = getattr(item, attr, None)
            if value is not None:
                match[attr] = f"{int(value):04X}"
        report["matches"].append(match)
    report["present"] = bool(report["matches"])
    return report


def serial_port_inventory() -> list[dict[str, Any]]:
    try:
        import serial.tools.list_ports
    except ImportError as exc:  # pragma: no cover - dependency dependent
        raise RuntimeError(f"pyserial is required: {exc}") from exc

    ports: list[dict[str, Any]] = []
    for item in serial.tools.list_ports.comports():
        record: dict[str, Any] = {"device": str(getattr(item, "device", "") or "")}
        for attr in ["description", "hwid", "manufacturer", "product", "serial_number"]:
            value = getattr(item, attr, None)
            if value:
                record[attr] = str(value)
        for attr in ["vid", "pid"]:
            value = getattr(item, attr, None)
            if value is not None:
                record[attr] = f"{int(value):04X}"
        ports.append(record)
    return ports


def rp2040_candidate_reasons(record: dict[str, Any]) -> list[str]:
    reasons: list[str] = []
    vid = str(record.get("vid", "")).upper()
    if vid in RP2040_USB_VIDS:
        reasons.append(f"vid:{vid}")
    text = " ".join(
        str(record.get(field, "") or "")
        for field in ["device", "description", "hwid", "manufacturer", "product"]
    ).lower()
    for keyword in RP2040_USB_KEYWORDS:
        if keyword in text:
            reasons.append(f"keyword:{keyword}")
    return sorted(set(reasons))


def rp2040_port_discovery(preferred_port: str, d1l_port: str) -> dict[str, Any]:
    preferred = rp2040_port_snapshot(preferred_port)
    report: dict[str, Any] = {
        "preferred_port": normalize_port(preferred_port),
        "d1l_port": normalize_port(d1l_port),
        "present": False,
        "selected_port": None,
        "preferred": preferred,
        "candidates": [],
        "skipped": [],
        "configured_port_required": True,
        "alternatives_read_only": True,
    }
    if preferred.get("present"):
        report["present"] = True
        report["selected_port"] = normalize_port(preferred_port)
        report["selected_reason"] = "configured_port_present"
        report["selected"] = preferred["matches"][0]
        return report

    try:
        inventory = serial_port_inventory()
    except Exception as exc:  # pragma: no cover - host/OS dependent
        report["error"] = str(exc)
        return report

    d1l = normalize_port(d1l_port)
    for record in inventory:
        device = normalize_port(str(record.get("device", "") or ""))
        if not device:
            continue
        if device == d1l:
            report["skipped"].append({"device": device, "reason": "d1l_console_port"})
            continue
        if device in FORBIDDEN_PORTS:
            report["skipped"].append({"device": device, "reason": "forbidden_port"})
            continue
        reasons = rp2040_candidate_reasons(record)
        if not reasons:
            continue
        candidate = dict(record)
        candidate["device"] = device
        candidate["match_reasons"] = reasons
        report["candidates"].append(candidate)

    if report["candidates"]:
        report["alternative_ports"] = [
            candidate["device"] for candidate in report["candidates"]
        ]
        report["warning"] = (
            f"configured RP2040 port {normalize_port(preferred_port)} is absent; "
            "alternatives are inventory-only and will not be selected or touched"
        )
    return report


def d1l_console_ok(result: dict, *, require_protocol: bool = False) -> bool:
    if result.get("ok") is not True:
        return False
    if require_protocol and result.get("protocol_supported") is not True:
        return False
    return True


def enter_rp2040_bootloader_usb_touch(port: str) -> dict:
    started_at = utc_now()
    report = {
        "schema": 1,
        "kind": "rp2040_1200_baud_touch",
        "port": port,
        "baud": 1200,
        "started_at": started_at,
        "ended_at": None,
        "ok": True,
    }
    touch_code = (
        "import sys\n"
        "import serial\n"
        "ser = serial.Serial(port=sys.argv[1], baudrate=1200, timeout=0.2)\n"
        "ser.close()\n"
    )
    run_kwargs: dict[str, Any] = {}
    if os.name == "nt" and hasattr(subprocess, "CREATE_NO_WINDOW"):
        run_kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
    proc: subprocess.Popen[str] | None = None
    try:
        proc = subprocess.Popen(
            [sys.executable, "-c", touch_code, port],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            **run_kwargs,
        )
        stdout, stderr = proc.communicate(timeout=BOOTLOADER_TOUCH_TIMEOUT_SECONDS)
        report["returncode"] = proc.returncode
        if stdout:
            report["stdout_tail"] = stdout[-1000:]
        if stderr:
            report["stderr_tail"] = stderr[-1000:]
    except subprocess.TimeoutExpired as exc:
        report["ok"] = True
        report["warning"] = f"1200-baud touch timed out after {BOOTLOADER_TOUCH_TIMEOUT_SECONDS}s"
        if proc is not None:
            terminate_process_tree(proc)
            try:
                stdout, stderr = proc.communicate(timeout=1)
                report["returncode"] = proc.returncode
                if stdout:
                    report["stdout_tail"] = stdout[-1000:]
                if stderr:
                    report["stderr_tail"] = stderr[-1000:]
            except Exception:
                report["returncode"] = proc.poll()
        if exc.stderr:
            text = exc.stderr.decode("utf-8", errors="replace") if isinstance(exc.stderr, bytes) else str(exc.stderr)
            report["stderr_tail"] = text[-1000:]
    except Exception as exc:  # Windows often drops the CDC device mid-open.
        report["ok"] = True
        report["warning"] = str(exc)
    report["ended_at"] = utc_now()
    return report


def rp2040_double_reset_sweep(
    ctx: RunContext,
    *,
    initial_volume_keys: set[str] | None = None,
    explicit_volume: str | None = None,
) -> tuple[list[dict], dict | None]:
    attempts: list[dict] = []
    baseline_keys = initial_volume_keys or set()
    for hold_ms, gap_ms, settle_ms in RP2040_DOUBLE_RESET_SWEEP_MS:
        command = f"rp2040 double-reset {hold_ms} {gap_ms} {settle_ms}"
        attempt: dict[str, Any] = {
            "command": command,
            "hold_ms": hold_ms,
            "gap_ms": gap_ms,
            "settle_ms": settle_ms,
        }
        timeout = max(8.0, settle_ms / 1000.0 + 5.0)
        attempt["result"] = send_d1l_console(ctx.d1l_port, ctx.baud, command, timeout)
        time.sleep(BOOTLOADER_ENTRY_RESCAN_SECONDS)
        attempt["uf2_volume"] = uf2_volume_snapshot()
        try:
            selected_volume, new_volumes = select_correlated_uf2_volume(
                baseline_keys,
                attempt["uf2_volume"],
                explicit_volume=explicit_volume,
            )
        except FlashGuardError as exc:
            attempt["uf2_selection_error"] = str(exc)
            attempts.append(attempt)
            return attempts, attempt
        attempt["new_uf2_volumes"] = new_volumes
        if selected_volume:
            attempt["selected_uf2_volume"] = selected_volume
        attempt["rp2040_port"] = rp2040_port_discovery(ctx.rp2040_port, ctx.d1l_port)
        attempts.append(attempt)
        if (
            attempt.get("selected_uf2_volume")
            or attempt.get("uf2_selection_error")
            or attempt["rp2040_port"].get("present")
        ):
            return attempts, attempt
    return attempts, None


def enter_rp2040_bootloader(ctx: RunContext, *, volume: str | None) -> dict:
    report: dict[str, Any] = {
        "schema": 1,
        "kind": "rp2040_bootloader_entry",
        "port": ctx.rp2040_port,
        "d1l_port": ctx.d1l_port,
        "public_rf_tx": False,
        "formats_sd": False,
        "manual_user_required": False,
        "started_at": utc_now(),
        "ok": False,
    }
    initial_volumes = uf2_volume_snapshot()
    report["uf2_volume_initial"] = initial_volumes
    if initial_volumes.get("error") and not volume:
        report["error"] = "uf2_initial_snapshot_failed"
        report["detail"] = initial_volumes.get("error")
        report["ended_at"] = utc_now()
        return report
    initial_keys = uf2_volume_keys(initial_volumes)
    report["uf2_volume_initial_keys"] = sorted(initial_keys)

    def selected_from(snapshot: dict[str, Any], label: str) -> str | None:
        report[label] = snapshot
        try:
            selected, new_volumes = select_correlated_uf2_volume(
                initial_keys,
                snapshot,
                explicit_volume=volume,
            )
        except FlashGuardError as exc:
            report["error"] = "uf2_volume_selection_ambiguous"
            report["detail"] = str(exc)
            return None
        report[f"{label}_new_volumes"] = new_volumes
        if selected:
            report["selected_uf2_volume"] = selected
        return selected

    initial_selected = selected_from(initial_volumes, "uf2_volume_initial_selection")
    if report.get("error"):
        report["ended_at"] = utc_now()
        return report
    if volume and initial_selected:
        report["ok"] = True
        report["method"] = "explicit_uf2_volume"
        report["ended_at"] = utc_now()
        return report
    if initial_keys and not volume:
        report["preexisting_uf2_volumes"] = sorted(initial_keys)
        report["preexisting_uf2_warning"] = (
            "pre-existing UF2 volumes are ineligible for automatic selection; "
            "pass --uf2-volume to select one explicitly"
        )

    ping = send_d1l_console(ctx.d1l_port, ctx.baud, "rp2040 ping", 8.0, settle_sec=3.0)
    report["ping"] = ping
    if d1l_console_ok(ping, require_protocol=True):
        bridge = send_d1l_console(ctx.d1l_port, ctx.baud, "rp2040 bootloader", 8.0)
        report["bridge_bootloader"] = bridge
        time.sleep(BOOTLOADER_ENTRY_RESCAN_SECONDS)
        bridge_volume = selected_from(
            uf2_volume_snapshot(), "uf2_volume_after_bridge_command"
        )
        if report.get("error"):
            report["ended_at"] = utc_now()
            return report
        if bridge_volume:
            report["ok"] = True
            report["method"] = "rp2040_bridge_command"
            report["ended_at"] = utc_now()
            return report
        report["bridge_bootloader_warning"] = (
            "bridge command did not reveal a correlated UF2 volume"
        )

    port_initial = rp2040_port_discovery(ctx.rp2040_port, ctx.d1l_port)
    report["rp2040_port_initial"] = port_initial
    if port_initial.get("present"):
        selected_port = str(port_initial.get("selected_port") or ctx.rp2040_port)
        report["selected_rp2040_port"] = selected_port
        report["usb_touch"] = enter_rp2040_bootloader_usb_touch(selected_port)
        time.sleep(BOOTLOADER_ENTRY_RESCAN_SECONDS)
        touch_volume = selected_from(
            uf2_volume_snapshot(), "uf2_volume_after_usb_touch"
        )
        if report.get("error"):
            report["ended_at"] = utc_now()
            return report
        if touch_volume:
            report["ok"] = True
            report["method"] = "rp2040_1200_baud_touch"
            report["ended_at"] = utc_now()
            return report
        report["usb_touch_warning"] = "1200_baud_touch_did_not_reveal_uf2"

    sweep, success = rp2040_double_reset_sweep(
        ctx,
        initial_volume_keys=initial_keys,
        explicit_volume=volume,
    )
    report["double_reset_sweep"] = sweep
    if sweep:
        report["double_reset"] = sweep[0].get("result")
        report["uf2_volume_after_double_reset"] = sweep[-1].get("uf2_volume")
        report["rp2040_port_after_double_reset"] = sweep[-1].get("rp2040_port")
    if success and success.get("uf2_selection_error"):
        report["error"] = "uf2_volume_selection_ambiguous"
        report["detail"] = success["uf2_selection_error"]
        report["ended_at"] = utc_now()
        return report
    if success and success.get("selected_uf2_volume"):
        report["ok"] = True
        report["method"] = "double_reset_sweep_revealed_uf2"
        report["successful_double_reset"] = success
        report["selected_uf2_volume"] = success["selected_uf2_volume"]
        report["ended_at"] = utc_now()
        return report
    if success and success["rp2040_port"].get("present"):
        selected_port = str(success["rp2040_port"].get("selected_port") or ctx.rp2040_port)
        report["selected_rp2040_port"] = selected_port
        report["usb_touch_after_double_reset"] = enter_rp2040_bootloader_usb_touch(selected_port)
        time.sleep(BOOTLOADER_ENTRY_RESCAN_SECONDS)
        touch_volume = selected_from(
            uf2_volume_snapshot(), "uf2_volume_after_double_reset_touch"
        )
        if report.get("error"):
            report["ended_at"] = utc_now()
            return report
        if touch_volume:
            report["ok"] = True
            report["method"] = "double_reset_sweep_then_rp2040_1200_baud_touch"
            report["successful_double_reset"] = success
            report["ended_at"] = utc_now()
            return report
        report["double_reset_touch_warning"] = "1200_baud_touch_did_not_reveal_uf2"

    reset = send_d1l_console(
        ctx.d1l_port,
        ctx.baud,
        "rp2040 reset",
        RP2040_RESET_COMMAND_TIMEOUT_SECONDS,
        settle_sec=3.0,
    )
    report["reset"] = reset
    time.sleep(BOOTLOADER_ENTRY_RESCAN_SECONDS)
    port_after_reset = rp2040_port_discovery(ctx.rp2040_port, ctx.d1l_port)
    report["rp2040_port_after_reset"] = port_after_reset
    reset_volume = selected_from(uf2_volume_snapshot(), "uf2_volume_after_reset")
    if report.get("error"):
        report["ended_at"] = utc_now()
        return report
    if reset_volume:
        report["ok"] = True
        report["method"] = "reset_revealed_uf2"
    elif port_after_reset.get("present"):
        selected_port = str(port_after_reset.get("selected_port") or ctx.rp2040_port)
        report["selected_rp2040_port"] = selected_port
        report["usb_touch_after_reset"] = enter_rp2040_bootloader_usb_touch(selected_port)
        time.sleep(BOOTLOADER_ENTRY_RESCAN_SECONDS)
        touch_volume = selected_from(
            uf2_volume_snapshot(), "uf2_volume_after_reset_touch"
        )
        report["ok"] = touch_volume is not None and not report.get("error")
        report["method"] = "reset_then_rp2040_1200_baud_touch" if report["ok"] else "none"
        if not report["ok"]:
            report.setdefault("error", "rp2040_bootloader_unavailable_after_usb_touch")
    else:
        report["ok"] = False
        report["method"] = "none"
        report["error"] = "rp2040_bootloader_unavailable"
    report["ended_at"] = utc_now()
    return report


def copy_named_uf2(
    *,
    artifact_dir: Path,
    filename: str,
    volume: str | None,
    selected_volume: str | None = None,
    timeout_sec: float,
) -> dict:
    artifact = verify_named_artifact(artifact_dir, filename)
    target_spec = selected_volume
    if not target_spec:
        raise FlashGuardError(
            "bootloader entry did not return a correlated UF2 volume"
        )
    target_volume, candidates = wait_for_uf2_volume(timeout_sec, target_spec)
    destination = target_volume / filename
    shutil.copyfile(artifact["path"], destination)
    return {
        "ok": True,
        "artifact": artifact,
        "target_volume": metadata_for_volume(target_volume),
        "candidate_volumes": candidates,
        "destination": str(destination),
        "copied": True,
    }


def send_d1l_console(port: str, baud: int, command: str, timeout: float, *, settle_sec: float = 1.0) -> dict:
    try:
        import serial
    except ImportError as exc:  # pragma: no cover - dependency dependent
        return {"ok": False, "error": f"pyserial is required: {exc}", "cmd": command}
    with open_d1l_serial(serial, port=port, baudrate=baud, timeout=timeout) as ser:
        time.sleep(settle_sec)
        ser.reset_input_buffer()
        return send_console_command(ser, command, timeout)


def poll_d1l_bridge_ping(ctx: RunContext, *, attempts: int, interval_sec: float) -> dict:
    report = {
        "ok": False,
        "attempts": [],
    }
    for attempt in range(1, attempts + 1):
        ping = send_d1l_console(ctx.d1l_port, ctx.baud, "rp2040 ping", 8.0)
        ping["attempt"] = attempt
        report["attempts"].append(ping)
        if d1l_console_ok(ping, require_protocol=True):
            report["ok"] = True
            report["selected_attempt"] = attempt
            report["ping"] = ping
            return report
        if attempt < attempts:
            time.sleep(interval_sec)
    report["ping"] = report["attempts"][-1] if report["attempts"] else {}
    return report


def baud_probe_selected_deskos(result: dict) -> int | None:
    if result.get("ok") is not True or result.get("found_deskos") is not True:
        return None
    try:
        selected = int(result.get("selected_baud") or 0)
    except (TypeError, ValueError):
        return None
    return selected if selected > 0 else None


def rp2040_try_baud_probe(ctx: RunContext, report: dict[str, Any], label: str) -> bool:
    probe_key = f"baud_probe{label}"
    set_key = f"set_baud{label}"
    ping_key = f"ping_after_baud_probe{label}"
    report[probe_key] = send_d1l_console(
        ctx.d1l_port,
        ctx.baud,
        f"rp2040 baud-probe {RP2040_BAUD_PROBE_TIMEOUT_MS}",
        RP2040_BAUD_PROBE_COMMAND_TIMEOUT_SECONDS,
    )
    selected = baud_probe_selected_deskos(report[probe_key])
    if selected is None:
        return False
    report[set_key] = send_d1l_console(ctx.d1l_port, ctx.baud, f"rp2040 set-baud {selected}", 5.0)
    report[ping_key] = send_d1l_console(ctx.d1l_port, ctx.baud, "rp2040 ping", 8.0)
    if d1l_console_ok(report[ping_key], require_protocol=True):
        report["selected_rp2040_uart_baud"] = selected
        report["state"] = f"bridge_protocol_ready_after_baud_probe{label}"
        report["bootloader_entry"] = "rp2040 bootloader"
        return True
    return False


def rp2040_access_precheck(ctx: RunContext, *, dry_run: bool) -> dict:
    out = rp2040_access_precheck_out(ctx)
    report: dict[str, Any] = {
        "schema": 1,
        "kind": "rp2040_autonomous_access_precheck",
        "mode": "dry-run" if dry_run else "hardware",
        "d1l_port": ctx.d1l_port,
        "rp2040_port": ctx.rp2040_port,
        "commit": ctx.commit,
        "github_actions_run": ctx.github_run_id,
        "public_rf_tx": False,
        "formats_sd": False,
        "manual_user_required": False,
        "ok": True,
        "started_at": utc_now(),
    }

    def finish() -> dict:
        report["ended_at"] = utc_now()
        write_json(out, report)
        report["path"] = str(out)
        return report

    if dry_run:
        report["state"] = "dry_run"
        return finish()

    report["uf2_volume_initial"] = uf2_volume_snapshot()
    report["rp2040_port_initial"] = rp2040_port_discovery(ctx.rp2040_port, ctx.d1l_port)
    if report["uf2_volume_initial"].get("available"):
        report["state"] = "uf2_volume_available"
        return finish()
    if report["rp2040_port_initial"].get("present"):
        report["state"] = "usb_cdc_available_for_1200_baud_touch"
        report["selected_rp2040_port"] = report["rp2040_port_initial"].get("selected_port")
        return finish()

    report["status"] = send_d1l_console(ctx.d1l_port, ctx.baud, "rp2040 status", 8.0, settle_sec=3.0)
    report["ping"] = send_d1l_console(ctx.d1l_port, ctx.baud, "rp2040 ping", 8.0)
    if d1l_console_ok(report["ping"], require_protocol=True):
        report["state"] = "bridge_protocol_ready"
        report["bootloader_entry"] = "rp2040 bootloader"
        return finish()
    if rp2040_try_baud_probe(ctx, report, ""):
        return finish()

    sweep, success = rp2040_double_reset_sweep(ctx)
    report["double_reset_sweep"] = sweep
    if sweep:
        report["double_reset"] = sweep[0].get("result")
        report["uf2_volume_after_double_reset"] = sweep[-1].get("uf2_volume")
        report["rp2040_port_after_double_reset"] = sweep[-1].get("rp2040_port")
    if success and success["uf2_volume"].get("available"):
        report["state"] = "uf2_volume_available_after_double_reset"
        report["successful_double_reset"] = success
        return finish()
    if success and success["rp2040_port"].get("present"):
        report["state"] = "usb_cdc_available_after_double_reset"
        report["successful_double_reset"] = success
        report["selected_rp2040_port"] = success["rp2040_port"].get("selected_port")
        return finish()

    report["reset"] = send_d1l_console(
        ctx.d1l_port,
        ctx.baud,
        "rp2040 reset",
        RP2040_RESET_COMMAND_TIMEOUT_SECONDS,
    )
    time.sleep(BOOTLOADER_ENTRY_RESCAN_SECONDS)
    report["uf2_volume_after_reset"] = uf2_volume_snapshot()
    report["rp2040_port_after_reset"] = rp2040_port_discovery(ctx.rp2040_port, ctx.d1l_port)
    report["ping_after_reset"] = send_d1l_console(ctx.d1l_port, ctx.baud, "rp2040 ping", 8.0, settle_sec=3.0)
    if report["uf2_volume_after_reset"].get("available"):
        report["state"] = "uf2_volume_available_after_reset"
    elif report["rp2040_port_after_reset"].get("present"):
        report["state"] = "usb_cdc_available_after_reset"
        report["selected_rp2040_port"] = report["rp2040_port_after_reset"].get("selected_port")
    elif d1l_console_ok(report["ping_after_reset"], require_protocol=True):
        report["state"] = "bridge_protocol_ready_after_reset"
        report["bootloader_entry"] = "rp2040 bootloader"
    elif rp2040_try_baud_probe(ctx, report, "_after_reset"):
        pass
    else:
        report["ok"] = False
        report["state"] = "no_autonomous_bootloader_path"
        report["error"] = "rp2040_bootloader_unavailable"
        report["next_action"] = "requires_firmware_bootloader_command_or_hardware_bootsel_control"
    return finish()


def capture_official_smoke(port: str, baud: int, timeout: float) -> dict:
    started_at = utc_now()
    report: dict[str, Any] = {
        "ok": False,
        "port": port,
        "baud": baud,
        "started_at": started_at,
        "ended_at": None,
        "lines": [],
        "parsed": [],
    }
    try:
        import serial

        with serial.Serial(port=port, baudrate=baud, timeout=0.25) as ser:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                report["lines"].append(line)
                if line.startswith("{"):
                    try:
                        parsed = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    report["parsed"].append(parsed)
                    if parsed.get("test") == "seeed_official_sd_smoke":
                        report["result"] = parsed
                        report["ok"] = parsed.get("ok") is True
                        break
    except Exception as exc:  # pragma: no cover - serial dependent
        report["error"] = str(exc)
    report["ended_at"] = utc_now()
    return report


def run_official_sd_smoke(ctx: RunContext, *, volume: str | None, uf2_timeout: float, smoke_timeout: float, dry_run: bool) -> dict:
    out = official_sd_smoke_out(ctx)
    report: dict[str, Any] = {
        "schema": 1,
        "kind": "seeed_official_sd_smoke_capture",
        "mode": "dry-run" if dry_run else "hardware",
        "port": ctx.rp2040_port,
        "baud": ctx.baud,
        "commit": ctx.commit,
        "github_actions_run": ctx.github_run_id,
        "public_rf_tx": False,
        "formats_sd": False,
        "ok": True,
        "artifact_dir": str(artifact_dirs(ctx)["official_smoke"]),
    }
    if dry_run:
        report["planned_uf2"] = OFFICIAL_SMOKE_UF2
    else:
        report["bootloader_entry"] = enter_rp2040_bootloader(ctx, volume=volume)
        if report["bootloader_entry"].get("ok") is not True:
            report["ok"] = False
            report["error"] = report["bootloader_entry"].get("error", "rp2040_bootloader_unavailable")
            write_json(out, report)
            report["path"] = str(out)
            return report
        report["copy"] = copy_named_uf2(
            artifact_dir=artifact_dirs(ctx)["official_smoke"],
            filename=OFFICIAL_SMOKE_UF2,
            volume=volume,
            selected_volume=report["bootloader_entry"].get("selected_uf2_volume"),
            timeout_sec=uf2_timeout,
        )
        time.sleep(2.0)
        report["reset"] = send_d1l_console(
            ctx.d1l_port,
            ctx.baud,
            "rp2040 reset",
            RP2040_RESET_COMMAND_TIMEOUT_SECONDS,
        )
        capture_discovery = rp2040_port_discovery(
            str(report["bootloader_entry"].get("selected_rp2040_port") or ctx.rp2040_port),
            ctx.d1l_port,
        )
        report["rp2040_port_for_smoke_capture"] = capture_discovery
        capture_port = str(capture_discovery.get("selected_port") or ctx.rp2040_port)
        report["capture_port"] = capture_port
        capture = capture_official_smoke(capture_port, ctx.baud, smoke_timeout)
        if not capture.get("result"):
            report["reset_retry"] = send_d1l_console(
                ctx.d1l_port,
                ctx.baud,
                "rp2040 reset",
                RP2040_RESET_COMMAND_TIMEOUT_SECONDS,
            )
            capture_discovery = rp2040_port_discovery(capture_port, ctx.d1l_port)
            report["rp2040_port_for_smoke_capture_retry"] = capture_discovery
            capture_port = str(capture_discovery.get("selected_port") or capture_port)
            report["capture_port_retry"] = capture_port
            capture = capture_official_smoke(capture_port, ctx.baud, smoke_timeout)
        report.update(capture)
    write_json(out, report)
    report["path"] = str(out)
    return report


def restore_bridge(ctx: RunContext, *, volume: str | None, uf2_timeout: float,
                   dry_run: bool, phase: str = "initial") -> dict:
    out = bridge_restore_out(ctx, phase)
    report: dict[str, Any] = {
        "schema": 1,
        "kind": "rp2040_bridge_restore",
        "mode": "dry-run" if dry_run else "hardware",
        "port": ctx.rp2040_port,
        "commit": ctx.commit,
        "github_actions_run": ctx.github_run_id,
        "phase": phase,
        "public_rf_tx": False,
        "formats_sd": False,
        "ok": True,
        "artifact_dir": str(artifact_dirs(ctx)["bridge"]),
    }
    if dry_run:
        report["planned_uf2"] = BRIDGE_UF2
    else:
        report["bootloader_entry"] = enter_rp2040_bootloader(ctx, volume=volume)
        if report["bootloader_entry"].get("ok") is not True:
            report["ok"] = False
            report["error"] = report["bootloader_entry"].get("error", "rp2040_bootloader_unavailable")
            write_json(out, report)
            report["path"] = str(out)
            return report
        report["copy"] = copy_named_uf2(
            artifact_dir=artifact_dirs(ctx)["bridge"],
            filename=BRIDGE_UF2,
            volume=volume,
            selected_volume=report["bootloader_entry"].get("selected_uf2_volume"),
            timeout_sec=uf2_timeout,
        )
        time.sleep(2.0)
        report["reset"] = send_d1l_console(
            ctx.d1l_port,
            ctx.baud,
            "rp2040 reset",
            RP2040_RESET_COMMAND_TIMEOUT_SECONDS,
        )
        report["ping_poll"] = poll_d1l_bridge_ping(
            ctx,
            attempts=RP2040_RESTORE_PING_ATTEMPTS,
            interval_sec=RP2040_RESTORE_PING_INTERVAL_SECONDS,
        )
        report["ping"] = report["ping_poll"].get("ping", {})
        report["ok"] = (
            report["copy"].get("ok") is True
            and report["ping"].get("ok") is True
            and report["ping"].get("protocol_supported") is True
        )
        if not report["ok"]:
            report["error"] = "bridge_restore_not_verified"
    write_json(out, report)
    report["path"] = str(out)
    return report


def bridge_sha(ctx: RunContext) -> str:
    return verify_named_artifact(artifact_dirs(ctx)["bridge"], BRIDGE_UF2)["sha256"]


def run_existing_script(ctx: RunContext, name: str, args: list[str], out: Path, timeout: int | None, dry_run: bool) -> dict:
    command = [sys.executable, *args]
    if "--out" not in command:
        command.extend(["--out", str(out)])
    if dry_run and "--dry-run" not in command:
        command.append("--dry-run")
    report = {
        "schema": 1,
        "kind": name,
        "mode": "dry-run" if dry_run else "hardware",
        "commit": ctx.commit,
        "github_actions_run": ctx.github_run_id,
        "public_rf_tx": False,
        "formats_sd": False,
        "command": command,
        "path": str(out),
        "ok": True,
    }
    if not dry_run:
        report["result"] = command_report(name, command, ctx.root, timeout=timeout)
        report["ok"] = report["result"].get("ok") is True
    return report


def preflight_out(ctx: RunContext, phase: str = "initial") -> Path:
    phase_suffix = "" if phase == "initial" else f"_{phase}"
    return ctx.hardware_dir / (
        f"rp2040_preflight{phase_suffix}_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    )


def run_preflight(ctx: RunContext, dry_run: bool, *, verify_artifact: bool,
                  phase: str = "initial") -> dict:
    out = preflight_out(ctx, phase)
    args = [
        str(ctx.root / "scripts" / "rp2040_sd_bridge_preflight_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "8",
    ]
    if verify_artifact:
        args.extend([
            "--artifact-dir",
            str(artifact_dirs(ctx)["bridge"]),
            "--expected-sha256",
            bridge_sha(ctx),
        ])
    report = run_existing_script(ctx, "rp2040_preflight", args, out, timeout=90, dry_run=dry_run)
    report["phase"] = phase
    return report


def validate_clean_preflight_payload(payload: dict, ctx: RunContext) -> dict:
    violations: list[str] = []
    storage = payload.get("storage_status") if isinstance(payload.get("storage_status"), dict) else {}
    manager = storage.get("manager") if isinstance(storage.get("manager"), dict) else {}
    sd = storage.get("sd") if isinstance(storage.get("sd"), dict) else {}
    retained = storage.get("retained_sd") if isinstance(storage.get("retained_sd"), dict) else {}
    stores = retained.get("stores") if isinstance(retained.get("stores"), dict) else {}

    required_values = {
        "artifact.ok": (payload.get("ok"), True),
        "artifact.ready_for_sd_acceptance": (payload.get("ready_for_sd_acceptance"), True),
        "artifact.port": (normalize_port(payload.get("port")), ctx.d1l_port),
        "artifact.commit": (payload.get("commit"), ctx.commit),
        "storage.ok": (storage.get("ok"), True),
        "manager.running": (manager.get("running"), True),
        "manager.state": (manager.get("state"), "READY_SD"),
        "sd.state": (sd.get("state"), "ready"),
        "sd.present": (sd.get("present"), True),
        "sd.presence_stale": (sd.get("presence_stale"), False),
        "sd.mounted": (sd.get("mounted"), True),
        "sd.data_root_ready": (sd.get("data_root_ready"), True),
        "sd.file_ops": (sd.get("file_ops"), True),
        "sd.status_stale": (sd.get("status_stale"), False),
        "sd.refresh_failures": (sd.get("refresh_failures"), 0),
        "sd.last_error": (sd.get("last_error"), "ESP_OK"),
        "storage.data_backend": (storage.get("data_backend"), "mixed"),
        "storage.message_store_backend": (storage.get("message_store_backend"), "sd"),
        "storage.dm_store_backend": (storage.get("dm_store_backend"), "sd"),
        "storage.route_store_backend": (storage.get("route_store_backend"), "sd"),
        "storage.packet_log_backend": (storage.get("packet_log_backend"), "sd"),
        "retained.degraded": (retained.get("degraded"), False),
        "retained.backup_degraded": (retained.get("backup_degraded"), False),
    }
    for label, (actual, expected) in required_values.items():
        if isinstance(expected, bool):
            matches = actual is expected
        elif isinstance(expected, int):
            matches = isinstance(actual, int) and not isinstance(actual, bool) and actual == expected
        else:
            matches = actual == expected
        if not matches:
            violations.append(f"{label} expected {expected!r}, got {actual!r}")

    counter_fields = (
        "sd_read_fail_count",
        "sd_write_fail_count",
        "sd_rename_fail_count",
        "nvs_mirror_fail_count",
    )
    for store_name in RETAINED_STORE_NAMES:
        store = stores.get(store_name) if isinstance(stores.get(store_name), dict) else None
        if store is None:
            violations.append(f"retained.stores.{store_name} missing")
            continue
        for field in counter_fields:
            value = store.get(field)
            if not isinstance(value, int) or isinstance(value, bool) or value != 0:
                violations.append(
                    f"retained.stores.{store_name}.{field} expected 0, got {value!r}"
                )
        if store.get("sd_degraded_latched") is not False:
            violations.append(
                f"retained.stores.{store_name}.sd_degraded_latched expected False, "
                f"got {store.get('sd_degraded_latched')!r}"
            )
        for field in ("sd_last_error", "nvs_mirror_last_error"):
            if store.get(field) != "ESP_OK":
                violations.append(
                    f"retained.stores.{store_name}.{field} expected 'ESP_OK', got {store.get(field)!r}"
                )

    return {
        "ok": not violations,
        "violations": violations,
        "manager_state": manager.get("state"),
        "sd_state": sd.get("state"),
        "retained_degraded": retained.get("degraded"),
        "stores_checked": list(RETAINED_STORE_NAMES),
    }


def clean_preflight_gate_out(ctx: RunContext, phase: str) -> Path:
    return ctx.hardware_dir / (
        f"{phase}_clean_preflight_gate_{ctx.short_commit}_actions_"
        f"{ctx.github_run_id}_{ctx.d1l_port}.json"
    )


def run_clean_preflight_gate(ctx: RunContext, preflight: dict, *, dry_run: bool,
                             phase: str = "post_diag") -> dict:
    out = clean_preflight_gate_out(ctx, phase)
    report: dict[str, Any] = {
        "schema": 1,
        "kind": f"{phase}_clean_preflight_gate",
        "phase": phase,
        "mode": "dry-run" if dry_run else "hardware",
        "commit": ctx.commit,
        "github_actions_run": ctx.github_run_id,
        "port": ctx.d1l_port,
        "public_rf_tx": False,
        "formats_sd": False,
        "preflight_path": preflight.get("path"),
        "ok": True,
    }
    if dry_run:
        report["planned_requirements"] = {
            "manager_state": "READY_SD",
            "retained_store_fail_counts": 0,
            "retained_store_degraded_latches": False,
        }
    elif preflight.get("ok") is not True:
        report["ok"] = False
        report["error"] = f"{phase}_preflight_failed"
    else:
        try:
            artifact_path = Path(str(preflight.get("path") or ""))
            payload = json.loads(artifact_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            report["ok"] = False
            report["error"] = f"{phase}_preflight_artifact_unreadable: {exc}"
        else:
            report["validation"] = validate_clean_preflight_payload(payload, ctx)
            report["ok"] = report["validation"].get("ok") is True
            if not report["ok"]:
                report["error"] = f"{phase}_preflight_not_clean"
    write_json(out, report)
    report["path"] = str(out)
    return report


def run_sd_file_canary(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / f"sd_file_canary_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    args = [
        str(ctx.root / "scripts" / "sd_file_canary_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        str(SD_FILE_CANARY_TIMEOUT_SECONDS),
        "--mount-wait-sec",
        str(SD_FILE_CANARY_MOUNT_WAIT_SECONDS),
    ]
    return run_existing_script(ctx, "sd_file_canary", args, out, timeout=120, dry_run=dry_run)


def token_for(ctx: RunContext, prefix: str) -> str:
    return f"{prefix}{ctx.short_commit}"[:31]


def run_sd_raw_diag(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / f"sd_diag_raw_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    args = [
        str(ctx.root / "scripts" / "rp2040_raw_diag_capture_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "10",
        "--diag-settle-sec",
        str(RAW_DIAG_SETTLE_SECONDS),
        "--diag-poll-sec",
        str(RAW_DIAG_POLL_SECONDS),
        "--diag-deadline-sec",
        str(RAW_DIAG_DEADLINE_SECONDS),
    ]
    return run_existing_script(
        ctx,
        "sd_raw_diag",
        args,
        out,
        timeout=RAW_DIAG_PROCESS_TIMEOUT_SECONDS,
        dry_run=dry_run,
    )


def run_sd_boot_prepare_scenario(ctx: RunContext, scenario: str, dry_run: bool) -> dict:
    underscored = scenario.replace("-", "_")
    out = ctx.hardware_dir / (
        f"sd_boot_prepare_{underscored}_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    )
    args = [
        str(ctx.root / "scripts" / "sd_boot_prepare_acceptance_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "90",
        "--scenario",
        scenario,
    ]
    return run_existing_script(ctx, f"sd_boot_prepare_{underscored}", args, out, timeout=180, dry_run=dry_run)


def run_sd_map_tile_canary(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / f"sd_map_tile_canary_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    args = [
        str(ctx.root / "scripts" / "sd_map_tile_canary_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "90",
        "--token",
        token_for(ctx, "map"),
    ]
    return run_existing_script(ctx, "sd_map_tile_canary", args, out, timeout=180, dry_run=dry_run)


def run_sd_export_canary(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / f"sd_export_canary_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    args = [
        str(ctx.root / "scripts" / "sd_export_canary_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "90",
        "--token",
        token_for(ctx, "ex"),
    ]
    return run_existing_script(ctx, "sd_export_canary", args, out, timeout=180, dry_run=dry_run)


def run_sd_diagnostic_export(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / f"sd_diagnostic_export_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    args = [
        str(ctx.root / "scripts" / "sd_diagnostic_export_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "90",
        "--token",
        token_for(ctx, "diag"),
    ]
    return run_existing_script(ctx, "sd_diagnostic_export", args, out, timeout=180, dry_run=dry_run)


def run_sd_data_export(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / f"sd_data_export_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    args = [
        str(ctx.root / "scripts" / "sd_data_export_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "90",
        "--token",
        token_for(ctx, "data"),
    ]
    return run_existing_script(ctx, "sd_data_export", args, out, timeout=180, dry_run=dry_run)


def run_sd_retained_history(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / f"sd_retained_history_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    args = [
        str(ctx.root / "scripts" / "sd_retained_history_acceptance_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "90",
        "--token",
        token_for(ctx, "ret"),
    ]
    return run_existing_script(ctx, "sd_retained_history", args, out, timeout=240, dry_run=dry_run)


def run_sd_reboot_remount(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / f"sd_reboot_remount_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    args = [
        str(ctx.root / "scripts" / "sd_reboot_remount_acceptance_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "90",
        "--token",
        token_for(ctx, "rem"),
        "--expected-firmware-commit",
        ctx.commit,
    ]
    return run_existing_script(ctx, "sd_reboot_remount", args, out, timeout=300, dry_run=dry_run)


def run_smoke(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / f"smoke_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    args = [
        str(ctx.root / "scripts" / "smoke_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "5",
        "--persistence-test",
        "--expected-firmware-commit",
        ctx.commit,
    ]
    report = run_existing_script(ctx, "smoke", args, out, timeout=180, dry_run=dry_run)
    if dry_run or report.get("ok") is True:
        return report
    first_attempt = out.with_name(f"{out.stem}_attempt1{out.suffix}")
    if out.exists():
        shutil.copyfile(out, first_attempt)
    time.sleep(SMOKE_RETRY_SETTLE_SEC)
    retry = run_existing_script(ctx, "smoke", args, out, timeout=180, dry_run=dry_run)
    retry["retry_after_failure"] = True
    retry["first_attempt_path"] = str(first_attempt)
    retry["smoke_retry_settle_sec"] = SMOKE_RETRY_SETTLE_SEC
    return retry


def run_onboarding_complete(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / (
        f"onboarding_complete_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    )
    commands = [
        f"settings onboarding complete {DEFAULT_ONBOARDING_NAME}",
        "settings onboarding status",
        "ui status",
        "health",
    ]
    report: dict[str, Any] = {
        "schema": 1,
        "kind": "onboarding_complete",
        "mode": "dry-run" if dry_run else "hardware",
        "commit": ctx.commit,
        "github_actions_run": ctx.github_run_id,
        "port": ctx.d1l_port,
        "commands": commands,
        "public_rf_tx": False,
        "formats_sd": False,
        "ok": True,
    }
    if not dry_run:
        results = []
        for command in commands:
            timeout = 8.0 if command.startswith("settings onboarding complete ") else 5.0
            result = send_d1l_console(ctx.d1l_port, ctx.baud, command, timeout)
            results.append(result)
            if command.startswith("settings onboarding complete "):
                time.sleep(0.5)
        report["results"] = results
        complete = results[0] if results else {}
        status = results[1] if len(results) > 1 else {}
        health = results[3] if len(results) > 3 else {}
        report["onboarding_complete"] = (
            complete.get("ok") is True and
            (status.get("onboarding_complete") is True or status.get("complete") is True)
        )
        report["ok"] = (
            complete.get("ok") is True and
            report["onboarding_complete"] is True and
            health.get("board_ready") is True and
            health.get("ui_ready") is True
        )
    write_json(out, report)
    report["path"] = str(out)
    return report


def run_ui_corruption_probe(ctx: RunContext, rounds: int, dry_run: bool) -> dict:
    out = (
        ctx.hardware_dir /
        f"ui_corruption_probe_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    )
    args = [
        str(ctx.root / "scripts" / "ui_corruption_probe_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "15",
        "--rounds",
        str(rounds),
        "--clear-crashlog-before-start",
    ]
    return run_existing_script(
        ctx,
        "ui_corruption_probe",
        args,
        out,
        timeout=max(300, rounds * 20),
        dry_run=dry_run,
    )


def run_scroll_probe(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / f"scroll_probe_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    args = [
        str(ctx.root / "scripts" / "scroll_probe_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "5",
        "--screens",
        DEFAULT_SCROLL_SCREENS,
        "--clear-crashlog-before-start",
    ]
    return run_existing_script(ctx, "scroll_probe", args, out, timeout=180, dry_run=dry_run)


def run_ui_reference_simulator(ctx: RunContext, view: str, dry_run: bool) -> dict:
    out_dir = ctx.root / "artifacts" / "ui-sim-reference" / f"{ctx.short_commit}-actions{ctx.github_run_id}"
    report_path = out_dir / "ui-sim-report.json"
    png_path = out_dir / f"{view}.png"
    command = [
        sys.executable,
        str(ctx.root / "tools" / "ui_simulator.py"),
        "--out",
        str(out_dir),
        "--view",
        view,
    ]
    report = {
        "schema": 1,
        "kind": "ui_simulator_reference",
        "mode": "dry-run" if dry_run else "host",
        "view": view,
        "command": command,
        "path": str(report_path),
        "png_path": str(png_path),
        "ok": True,
        "public_rf_tx": False,
        "formats_sd": False,
    }
    if not dry_run:
        report["result"] = command_report("ui_simulator_reference", command, ctx.root, timeout=60)
        report["ok"] = (
            report["result"].get("ok") is True
            and report_path.is_file()
            and png_path.is_file()
        )
    return report


def run_ui_pixel_capture(ctx: RunContext, dry_run: bool) -> dict:
    reference = run_ui_reference_simulator(ctx, "home", dry_run)
    if reference.get("ok") is not True and not dry_run:
        return {
            "schema": 1,
            "kind": "ui_pixel_capture",
            "mode": "hardware",
            "commit": ctx.commit,
            "github_actions_run": ctx.github_run_id,
            "public_rf_tx": False,
            "formats_sd": False,
            "reference": reference,
            "ok": False,
            "error": "ui_simulator_reference_failed",
        }
    out = ctx.hardware_dir / f"ui_pixel_capture_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    png_out = out.with_suffix(".png")
    raw_out = out.with_suffix(".rgb565")
    diff_out = out.with_name(f"{out.stem}_simdiff.json")
    args = [
        str(ctx.root / "scripts" / "ui_capture_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "15",
        "--chunk-size",
        "1024",
        "--out",
        str(out),
        "--png-out",
        str(png_out),
        "--raw-out",
        str(raw_out),
        "--prep-command",
        "ui tab home",
        "--post-prep-settle-sec",
        "0.25",
        "--reference-png",
        str(reference["png_path"]),
        "--reference-view",
        "home",
        "--diff-out",
        str(diff_out),
    ]
    report = run_existing_script(ctx, "ui_pixel_capture", args, out, timeout=420, dry_run=dry_run)
    report["reference"] = reference
    report["reference_png_path"] = reference["png_path"]
    report["simulator_diff_path"] = str(diff_out)
    report["ok"] = report.get("ok") is True and reference.get("ok") is True
    return report


def run_ui_compose_keyboard_capture(ctx: RunContext, dry_run: bool) -> dict:
    out = ctx.hardware_dir / (
        f"ui_compose_keyboard_capture_{ctx.short_commit}_actions_{ctx.github_run_id}_{ctx.d1l_port}.json"
    )
    args = [
        str(ctx.root / "scripts" / "ui_compose_keyboard_capture_d1l.py"),
        "--port",
        ctx.d1l_port,
        "--timeout",
        "15",
        "--chunk-size",
        "1024",
        "--targets",
        "all",
        "--out",
        str(out),
    ]
    return run_existing_script(ctx, "ui_compose_keyboard_capture", args, out, timeout=900, dry_run=dry_run)


def release_gate_command(ctx: RunContext, out: Path) -> list[str]:
    return [
        sys.executable,
        str(ctx.root / "scripts" / "release_gate_audit_d1l.py"),
        "--root",
        str(ctx.root),
        "--github-run-id",
        ctx.github_run_id,
        "--github-run-dir",
        str(ctx.github_run_dir),
        "--commit",
        ctx.commit,
        "--d1l-port",
        ctx.d1l_port,
        "--rp2040-port",
        ctx.rp2040_port,
        "--hardware-dir",
        str(ctx.hardware_dir),
        "--rp2040-hardware-dir",
        str(ctx.rp2040_hardware_dir),
        "--out",
        str(out),
    ]


def release_gate_out(ctx: RunContext, phase: str | None = None) -> Path:
    phase_suffix = f"-{phase}" if phase else ""
    return ctx.root / "artifacts" / "release-gate" / (
        f"release-gate-audit-{ctx.short_commit}-actions{ctx.github_run_id}"
        f"-autonomous-hw{phase_suffix}.json"
    )


def run_release_gate(
    ctx: RunContext, dry_run: bool, *, phase: str | None = None
) -> dict:
    out = release_gate_out(ctx, phase)
    cmd = release_gate_command(ctx, out)
    report = {
        "schema": 1,
        "kind": "release_gate_audit",
        "mode": "dry-run" if dry_run else "hardware",
        "command": cmd,
        "path": str(out),
        "ok": True,
    }
    if phase:
        report["phase"] = phase
    if not dry_run:
        report["result"] = command_report("release_gate", cmd, ctx.root, timeout=60)
        report["ok"] = report["result"].get("ok") is True and out.exists()
        if out.exists():
            try:
                gate = json.loads(out.read_text(encoding="utf-8"))
                report["ready_for_public_release"] = gate.get("ready_for_public_release")
                report["failed_count"] = gate.get("failed_count")
                report["p0_failed_count"] = gate.get("p0_failed_count")
            except json.JSONDecodeError:
                report["ok"] = False
    return report


def exception_step(ctx: RunContext, kind: str, exc: Exception, *, out: Path | None = None, port: str | None = None) -> dict:
    report = {
        "schema": 1,
        "kind": kind,
        "mode": "hardware",
        "commit": ctx.commit,
        "github_actions_run": ctx.github_run_id,
        "public_rf_tx": False,
        "formats_sd": False,
        "ok": False,
        "error": str(exc),
    }
    if port is not None:
        report["port"] = port
    if out is not None:
        report["path"] = str(out)
        write_json(out, report)
    return report


def phase_exception_step(
    ctx: RunContext,
    kind: str,
    exc: Exception,
    *,
    phase: str,
    out: Path,
    port: str,
) -> dict:
    report = exception_step(ctx, kind, exc, port=port)
    report["phase"] = phase
    report["path"] = str(out)
    write_json(out, report)
    return report


def download_artifacts(ctx: RunContext, dry_run: bool) -> dict:
    cmd = ["gh", "run", "download", ctx.github_run_id, "--dir", str(ctx.github_run_dir)]
    report = {
        "schema": 1,
        "kind": "github_artifact_download",
        "mode": "dry-run" if dry_run else "download",
        "command": cmd,
        "path": str(ctx.github_run_dir),
        "ok": True,
    }
    if not dry_run:
        ctx.github_run_dir.mkdir(parents=True, exist_ok=True)
        report["result"] = command_report("gh_run_download", cmd, ctx.root, timeout=300)
        report["ok"] = report["result"].get("ok") is True
    return report


def verify_inputs(ctx: RunContext, args: argparse.Namespace, *, allow_download: bool, dry_run: bool) -> dict:
    dirs = required_artifact_dirs(ctx, args)
    missing = [str(path) for path in dirs.values() if not path.exists()]
    report = {
        "schema": 1,
        "kind": "input_artifact_check",
        "mode": "dry-run" if dry_run else "check",
        "github_run_dir": str(ctx.github_run_dir),
        "required_artifacts": sorted(dirs),
        "rp2040_refresh_requested": rp2040_refresh_requested(args),
        "sd_suite_enabled": not args.skip_sd_suite,
        "missing": missing,
        "ok": not missing,
    }
    if missing and allow_download:
        report["download"] = download_artifacts(ctx, dry_run=dry_run)
        report["ok"] = dry_run or report["download"].get("ok") is True
        if not dry_run:
            missing = [str(path) for path in dirs.values() if not path.exists()]
            report["missing_after_download"] = missing
            report["ok"] = report["ok"] and not missing
    if not missing:
        try:
            report["provenance"] = verify_actions_artifact_provenance(ctx, args)
        except Exception as exc:
            report["provenance"] = {"ok": False, "error": str(exc)}
            report["ok"] = False
    elif dry_run and allow_download:
        report["provenance"] = {
            "ok": True,
            "planned": True,
            "require_commit": ctx.commit,
            "require_github_actions_run": ctx.github_run_id,
        }
    return report


def plan_report(ctx: RunContext, args: argparse.Namespace) -> dict:
    safe_scenarios = list(args.sd_scenarios)
    refresh_rp2040 = rp2040_refresh_requested(args)
    sd_suite = not args.skip_sd_suite
    bridge_reflash_required = refresh_rp2040 or sd_suite
    if sd_suite and refresh_rp2040:
        rp2040_flash_mode = "official_smoke_then_exact_bridge_pre_and_post_diag"
    elif sd_suite:
        rp2040_flash_mode = "exact_bridge_pre_and_post_diag"
    elif refresh_rp2040:
        rp2040_flash_mode = "refresh_official_smoke_and_restore_bridge"
    else:
        rp2040_flash_mode = "use_existing_bridge"
    return {
        "schema": 1,
        "kind": "d1l_autonomous_hardware_validation",
        "mode": "dry-run" if args.dry_run else "hardware",
        "commit": ctx.commit,
        "short_commit": ctx.short_commit,
        "github_actions_run": ctx.github_run_id,
        "github_run_dir": str(ctx.github_run_dir),
        "ports": {"d1l": ctx.d1l_port, "rp2040": ctx.rp2040_port, "forbidden": sorted(FORBIDDEN_PORTS)},
        "public_rf_tx": False,
        "formats_sd": False,
        "manual_user_required": False,
        "rp2040_uf2_flash": bridge_reflash_required,
        "rp2040_flash_mode": rp2040_flash_mode,
        "sd_suite_enabled": sd_suite,
        "steps": [
            "verify_input_artifacts",
            *(["flash_esp32"] if not args.skip_esp32_flash else []),
            *(["rp2040_autonomous_access_precheck"] if bridge_reflash_required else []),
            *(["flash_official_rp2040_sd_smoke", "capture_official_rp2040_sd_smoke"] if refresh_rp2040 else []),
            *(["sd_boot_prepare_rp2040_unavailable"] if refresh_rp2040 and not args.skip_rp2040_unavailable_capture else []),
            *(
                ["restore_exact_rp2040_bridge_pre_diag"]
                if sd_suite
                else (["restore_rp2040_bridge"] if refresh_rp2040 else [])
            ),
            *(
                [
                    "reflash_exact_esp32_pre_diag"
                    if sd_suite
                    else "reflash_exact_esp32_post_official_smoke"
                ]
                if refresh_rp2040
                else []
            ),
            *((
                "rp2040_bridge_preflight_pre_diag",
                "pre_diag_clean_preflight_gate",
                "sd_raw_diag",
                "restore_exact_rp2040_bridge_post_diag",
                "reflash_exact_esp32_post_diag",
                "rp2040_bridge_preflight_post_diag",
                "post_diag_clean_preflight_gate",
                "sd_file_canary",
                *(f"sd_boot_prepare_{scenario.replace('-', '_')}" for scenario in safe_scenarios),
                "sd_map_tile_canary",
                "sd_export_canary",
                "sd_diagnostic_export",
                "sd_data_export",
                "sd_retained_history",
                "sd_reboot_remount",
            ) if sd_suite else ()),
            "d1l_smoke",
            *(["d1l_onboarding_complete_for_ui_probes"] if args.include_ui_probes else []),
            *(
                [
                    "d1l_ui_corruption_probe",
                    "d1l_scroll_probe",
                    "d1l_ui_pixel_capture",
                    "d1l_ui_compose_keyboard_capture",
                ]
                if args.include_ui_probes
                else []
            ),
            "release_gate_audit",
        ],
    }


def release_gate_summary(runs: list[dict]) -> dict:
    gate = next(
        (step for step in reversed(runs) if step.get("kind") == "release_gate_audit"),
        {},
    )
    return {
        "path": gate.get("path"),
        "ready_for_public_release": gate.get("ready_for_public_release"),
        "failed_count": gate.get("failed_count"),
        "p0_failed_count": gate.get("p0_failed_count"),
    }


def attach_release_gate_summary(report: dict, runs: list[dict]) -> dict:
    summary = release_gate_summary(runs)
    report["release_gate"] = summary
    report["ready_for_public_release"] = summary.get("ready_for_public_release")
    report["release_ready"] = summary.get("ready_for_public_release") is True
    return summary


def validation_runs_ok(runs: list[dict]) -> bool:
    """Reduce bounded bridge retries to their terminal logical outcome."""
    seen_bridge_phases: set[str] = set()
    index = 0
    while index < len(runs):
        step = runs[index]
        if step.get("kind") == "release_gate_audit":
            index += 1
            continue
        if step.get("kind") == "rp2040_bridge_restore" and "attempt" in step:
            phase = step.get("phase")
            attempt = step.get("attempt")
            if not isinstance(phase, str) or not phase or phase in seen_bridge_phases:
                return False
            if attempt != 1 or isinstance(attempt, bool):
                return False
            seen_bridge_phases.add(phase)
            if step.get("ok") is True:
                index += 1
                continue
            if index + 1 >= len(runs):
                return False
            retry = runs[index + 1]
            if (
                retry.get("kind") != "rp2040_bridge_restore"
                or retry.get("phase") != phase
                or retry.get("attempt") != 2
                or isinstance(retry.get("attempt"), bool)
                or retry.get("ok") is not True
            ):
                return False
            index += 2
            continue
        if step.get("ok") is not True:
            return False
        index += 1
    return True


def run_bridge_restore_with_retry(ctx: RunContext, args: argparse.Namespace,
                                  runs: list[dict], *, phase: str) -> dict:
    last_report: dict[str, Any] = {}
    for attempt in range(2):
        try:
            report = restore_bridge(
                ctx,
                volume=args.uf2_volume,
                uf2_timeout=args.uf2_timeout,
                dry_run=args.dry_run,
                phase=phase,
            )
        except Exception as exc:
            report = exception_step(
                ctx,
                "rp2040_bridge_restore",
                exc,
                out=bridge_restore_out(ctx, phase),
                port=ctx.rp2040_port,
            )
        report["attempt"] = attempt + 1
        report["phase"] = phase
        attempt_out = bridge_restore_out(ctx, f"{phase}_attempt_{attempt + 1}")
        report["path"] = str(attempt_out)
        write_json(attempt_out, report)
        runs.append(report)
        last_report = report
        if report.get("ok") is True:
            break
    return last_report


def release_gate_exception_out(ctx: RunContext, phase: str) -> Path:
    return ctx.hardware_dir / (
        f"release_gate_audit_exception_{phase}_{ctx.short_commit}_actions_"
        f"{ctx.github_run_id}_{ctx.d1l_port}.json"
    )


def append_release_gate_safely(
    ctx: RunContext, runs: list[dict], *, dry_run: bool, phase: str
) -> dict:
    try:
        gate = run_release_gate(ctx, dry_run=dry_run, phase=phase)
    except Exception as exc:
        gate = exception_step(
            ctx,
            "release_gate_audit",
            exc,
            out=release_gate_exception_out(ctx, phase),
            port=ctx.d1l_port,
        )
        gate["phase"] = phase
    runs.append(gate)
    return gate


def failed_stage_exception_out(ctx: RunContext, kind: str) -> Path:
    return ctx.hardware_dir / (
        f"{kind}_exception_{ctx.short_commit}_actions_"
        f"{ctx.github_run_id}_{ctx.d1l_port}.json"
    )


def run_sd_stage_safely(
    ctx: RunContext, kind: str, stage_runner: Any
) -> dict:
    try:
        return stage_runner()
    except Exception as exc:
        return exception_step(
            ctx,
            kind,
            exc,
            out=failed_stage_exception_out(ctx, kind),
            port=ctx.d1l_port,
        )


def run_exact_recovery_boundary(ctx: RunContext, args: argparse.Namespace,
                                runs: list[dict], *, phase: str) -> dict:
    bridge = run_bridge_restore_with_retry(ctx, args, runs, phase=phase)
    try:
        esp32 = flash_esp32(ctx, dry_run=args.dry_run, phase=phase)
    except Exception as exc:
        esp32 = phase_exception_step(
            ctx,
            "esp32_flash",
            exc,
            phase=phase,
            out=esp32_flash_out(ctx, phase),
            port=ctx.d1l_port,
        )
    runs.append(esp32)

    try:
        preflight = run_preflight(
            ctx,
            dry_run=args.dry_run,
            verify_artifact=True,
            phase=phase,
        )
    except Exception as exc:
        preflight = phase_exception_step(
            ctx,
            "rp2040_preflight",
            exc,
            phase=phase,
            out=preflight_out(ctx, phase),
            port=ctx.d1l_port,
        )
    runs.append(preflight)

    try:
        clean_gate = run_clean_preflight_gate(
            ctx,
            preflight,
            dry_run=args.dry_run,
            phase=phase,
        )
    except Exception as exc:
        clean_gate = phase_exception_step(
            ctx,
            f"{phase}_clean_preflight_gate",
            exc,
            phase=phase,
            out=clean_preflight_gate_out(ctx, phase),
            port=ctx.d1l_port,
        )
    runs.append(clean_gate)

    failures: list[str] = []
    if bridge.get("ok") is not True:
        failures.append(f"{phase}_bridge_restore_failed")
    if esp32.get("ok") is not True:
        failures.append(f"{phase}_esp32_reflash_failed")
    if preflight.get("ok") is not True:
        failures.append(f"{phase}_preflight_failed")
    if clean_gate.get("ok") is not True:
        failures.append(clean_gate.get("error", f"{phase}_preflight_not_clean"))
    return {
        "ok": not failures,
        "phase": phase,
        "failures": failures,
        "error": failures[0] if failures else None,
        "bridge": bridge,
        "esp32": esp32,
        "preflight": preflight,
        "clean_gate": clean_gate,
    }


def fail_after_sd_stage(report: dict, runs: list[dict], ctx: RunContext,
                        args: argparse.Namespace, *, error: str) -> dict:
    append_release_gate_safely(
        ctx,
        runs,
        dry_run=args.dry_run,
        phase=f"after_{error}",
    )
    recovery = run_exact_recovery_boundary(
        ctx,
        args,
        runs,
        phase=f"recovery_after_{error}",
    )
    append_release_gate_safely(
        ctx,
        runs,
        dry_run=args.dry_run,
        phase=f"after_{error}_post_recovery",
    )
    report["error"] = error
    report["recovery"] = {
        "ok": recovery["ok"],
        "phase": recovery["phase"],
        "error": recovery["error"],
        "failures": recovery["failures"],
    }
    report["ok"] = False
    attach_release_gate_summary(report, runs)
    return report


def run_validation(args: argparse.Namespace) -> dict:
    ctx = build_context(args)
    ctx.hardware_dir.mkdir(parents=True, exist_ok=True)
    ctx.rp2040_hardware_dir.mkdir(parents=True, exist_ok=True)

    report = plan_report(ctx, args)
    report["created_at"] = utc_now()
    report["git"] = git_metadata(ctx.root)
    runs: list[dict] = []
    report["runs"] = runs

    try:
        inputs = verify_inputs(ctx, args, allow_download=args.download_artifacts, dry_run=args.dry_run)
        runs.append(inputs)
        if not inputs.get("ok") and not args.dry_run:
            report["ok"] = False
            report["error"] = "input artifact check failed"
            return report

        if not args.skip_esp32_flash:
            esp32_flash = flash_esp32(ctx, dry_run=args.dry_run)
            runs.append(esp32_flash)
            if esp32_flash.get("ok") is not True and not args.dry_run:
                runs.append(run_release_gate(ctx, dry_run=args.dry_run))
                report["error"] = "esp32_flash_failed"
                report["ok"] = False
                attach_release_gate_summary(report, runs)
                return report

        refresh_rp2040 = rp2040_refresh_requested(args)
        sd_suite = not args.skip_sd_suite
        bridge_reflash_required = refresh_rp2040 or sd_suite
        if bridge_reflash_required:
            access_precheck = rp2040_access_precheck(ctx, dry_run=args.dry_run)
            runs.append(access_precheck)
            if access_precheck.get("ok") is not True and not args.dry_run:
                runs.append(run_release_gate(ctx, dry_run=args.dry_run))
                report["error"] = access_precheck.get("error", "rp2040_bootloader_unavailable")
                report["ok"] = False
                attach_release_gate_summary(report, runs)
                return report

        if refresh_rp2040:
            try:
                runs.append(
                    run_official_sd_smoke(
                        ctx,
                        volume=args.uf2_volume,
                        uf2_timeout=args.uf2_timeout,
                        smoke_timeout=args.smoke_capture_timeout,
                        dry_run=args.dry_run,
                    )
                )
            except Exception as exc:
                runs.append(
                    exception_step(
                        ctx,
                        "seeed_official_sd_smoke_capture",
                        exc,
                        out=official_sd_smoke_out(ctx),
                        port=ctx.rp2040_port,
                    )
                )
            if not args.skip_rp2040_unavailable_capture:
                runs.append(run_sd_boot_prepare_scenario(ctx, "rp2040-unavailable", dry_run=args.dry_run))

        if bridge_reflash_required:
            initial_bridge_phase = "pre_diag" if sd_suite else "post_official_smoke"
            bridge_restore = run_bridge_restore_with_retry(
                ctx,
                args,
                runs,
                phase=initial_bridge_phase,
            )
            if bridge_restore.get("ok") is not True and not args.dry_run:
                runs.append(run_release_gate(ctx, dry_run=args.dry_run))
                report["error"] = "bridge_restore_not_verified"
                report["ok"] = False
                attach_release_gate_summary(report, runs)
                return report

            # The official smoke image intentionally makes the production bridge
            # unavailable long enough for retained ESP32 stores to observe real
            # I/O failures. Restore the exact ESP32 image as well as the bridge so
            # the following clean gate starts from a fresh, checksum-bound boot.
            if refresh_rp2040:
                try:
                    preflight_esp32_flash = flash_esp32(
                        ctx,
                        dry_run=args.dry_run,
                        phase=initial_bridge_phase,
                    )
                except Exception as exc:
                    preflight_esp32_flash = phase_exception_step(
                        ctx,
                        "esp32_flash",
                        exc,
                        phase=initial_bridge_phase,
                        out=esp32_flash_out(ctx, initial_bridge_phase),
                        port=ctx.d1l_port,
                    )
                runs.append(preflight_esp32_flash)
                if preflight_esp32_flash.get("ok") is not True and not args.dry_run:
                    append_release_gate_safely(
                        ctx,
                        runs,
                        dry_run=args.dry_run,
                        phase=f"{initial_bridge_phase}_esp32_reflash_failed",
                    )
                    report["error"] = f"{initial_bridge_phase}_esp32_reflash_failed"
                    report["ok"] = False
                    attach_release_gate_summary(report, runs)
                    return report

        if sd_suite:
            try:
                initial_preflight = run_preflight(
                    ctx,
                    dry_run=args.dry_run,
                    verify_artifact=True,
                    phase="pre_diag",
                )
            except Exception as exc:
                initial_preflight = exception_step(
                    ctx,
                    "rp2040_preflight",
                    exc,
                    out=preflight_out(ctx, "pre_diag"),
                    port=ctx.d1l_port,
                )
                initial_preflight["phase"] = "pre_diag"
            runs.append(initial_preflight)
            pre_diag_gate = run_clean_preflight_gate(
                ctx,
                initial_preflight,
                dry_run=args.dry_run,
                phase="pre_diag",
            )
            runs.append(pre_diag_gate)
            if pre_diag_gate.get("ok") is not True and not args.dry_run:
                append_release_gate_safely(
                    ctx,
                    runs,
                    dry_run=args.dry_run,
                    phase="pre_diag_clean_gate_failed",
                )
                report["error"] = pre_diag_gate.get(
                    "error", "pre_diag_preflight_not_clean"
                )
                report["ok"] = False
                attach_release_gate_summary(report, runs)
                return report

            raw_diag = run_sd_stage_safely(
                ctx,
                "sd_raw_diag",
                lambda: run_sd_raw_diag(ctx, dry_run=args.dry_run),
            )
            runs.append(raw_diag)

            post_diag_recovery = run_exact_recovery_boundary(
                ctx,
                args,
                runs,
                phase="post_diag",
            )
            if raw_diag.get("ok") is not True and not args.dry_run:
                append_release_gate_safely(
                    ctx,
                    runs,
                    dry_run=args.dry_run,
                    phase="sd_raw_diag_failed",
                )
                report["error"] = "sd_raw_diag_failed"
                report["recovery"] = {
                    "ok": post_diag_recovery["ok"],
                    "phase": post_diag_recovery["phase"],
                    "error": post_diag_recovery.get("error"),
                    "failures": post_diag_recovery.get("failures", []),
                }
                report["ok"] = False
                attach_release_gate_summary(report, runs)
                return report
            if post_diag_recovery.get("ok") is not True and not args.dry_run:
                append_release_gate_safely(
                    ctx,
                    runs,
                    dry_run=args.dry_run,
                    phase="post_diag_recovery_failed",
                )
                report["error"] = post_diag_recovery.get(
                    "error", "post_diag_recovery_failed"
                )
                report["recovery"] = {
                    "ok": False,
                    "phase": post_diag_recovery["phase"],
                    "error": post_diag_recovery.get("error"),
                    "failures": post_diag_recovery.get("failures", []),
                }
                report["ok"] = False
                attach_release_gate_summary(report, runs)
                return report

            sd_file_canary = run_sd_stage_safely(
                ctx,
                "sd_file_canary",
                lambda: run_sd_file_canary(ctx, dry_run=args.dry_run),
            )
            runs.append(sd_file_canary)
            if sd_file_canary.get("ok") is not True and not args.dry_run:
                return fail_after_sd_stage(
                    report, runs, ctx, args, error="sd_file_canary_failed"
                )
            for scenario in args.sd_scenarios:
                scenario_kind = f"sd_boot_prepare_{scenario.replace('-', '_')}"
                scenario_step = run_sd_stage_safely(
                    ctx,
                    scenario_kind,
                    lambda scenario=scenario: run_sd_boot_prepare_scenario(
                        ctx, scenario, dry_run=args.dry_run
                    ),
                )
                runs.append(scenario_step)
                if scenario_step.get("ok") is not True and not args.dry_run:
                    return fail_after_sd_stage(
                        report,
                        runs,
                        ctx,
                        args,
                        error=f"sd_boot_prepare_{scenario.replace('-', '_')}_failed",
                    )
            later_sd_stages = (
                ("sd_map_tile_canary_failed", run_sd_map_tile_canary),
                ("sd_export_canary_failed", run_sd_export_canary),
                ("sd_diagnostic_export_failed", run_sd_diagnostic_export),
                ("sd_data_export_failed", run_sd_data_export),
                ("sd_retained_history_failed", run_sd_retained_history),
                ("sd_reboot_remount_failed", run_sd_reboot_remount),
            )
            for stage_error, stage_runner in later_sd_stages:
                stage_kind = stage_error.removesuffix("_failed")
                stage = run_sd_stage_safely(
                    ctx,
                    stage_kind,
                    lambda stage_runner=stage_runner: stage_runner(
                        ctx, dry_run=args.dry_run
                    ),
                )
                runs.append(stage)
                if stage.get("ok") is not True and not args.dry_run:
                    return fail_after_sd_stage(
                        report, runs, ctx, args, error=stage_error
                    )
        smoke = run_sd_stage_safely(
            ctx,
            "d1l_smoke",
            lambda: run_smoke(ctx, dry_run=args.dry_run),
        )
        runs.append(smoke)
        if smoke.get("ok") is not True and not args.dry_run:
            if sd_suite:
                return fail_after_sd_stage(
                    report, runs, ctx, args, error="d1l_smoke_failed"
                )
            append_release_gate_safely(
                ctx,
                runs,
                dry_run=args.dry_run,
                phase="d1l_smoke_failed_no_sd_suite",
            )
            report["error"] = "d1l_smoke_failed"
            report["ok"] = False
            attach_release_gate_summary(report, runs)
            return report
        if args.include_ui_probes:
            onboarding = run_onboarding_complete(ctx, dry_run=args.dry_run)
            runs.append(onboarding)
            if onboarding.get("ok") is not True and not args.dry_run:
                runs.append(run_release_gate(ctx, dry_run=args.dry_run))
                report["error"] = "onboarding_complete_failed"
                report["ok"] = False
                attach_release_gate_summary(report, runs)
                return report
            runs.append(run_ui_corruption_probe(ctx, rounds=args.ui_rounds, dry_run=args.dry_run))
            runs.append(run_scroll_probe(ctx, dry_run=args.dry_run))
            runs.append(run_ui_pixel_capture(ctx, dry_run=args.dry_run))
            runs.append(run_ui_compose_keyboard_capture(ctx, dry_run=args.dry_run))
        runs.append(run_release_gate(ctx, dry_run=args.dry_run))
    except Exception as exc:
        report["error"] = str(exc)
        report["ok"] = False
    else:
        report["ok"] = validation_runs_ok(runs)
        attach_release_gate_summary(report, runs)
    return report


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-dir")
    parser.add_argument("--github-run-dir-name", help=argparse.SUPPRESS)
    parser.add_argument("--download-artifacts", action="store_true")
    parser.add_argument("--commit")
    parser.add_argument("--d1l-port", default=os.environ.get("D1L_PORT", DEFAULT_D1L_PORT))
    parser.add_argument("--rp2040-port", default=os.environ.get("D1L_RP2040_PORT", DEFAULT_RP2040_PORT))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--esp32-flash-baud", type=int, default=460800)
    parser.add_argument(
        "--skip-esp32-flash",
        action="store_true",
        help="Skip ESP32 flashing only for --skip-sd-suite UI validation; the SD suite requires exact pre/post-diag flashes.",
    )
    parser.add_argument(
        "--refresh-rp2040-smoke",
        action="store_true",
        help="Flash the official RP2040 SD smoke UF2, capture it, then restore the DeskOS bridge UF2.",
    )
    parser.add_argument(
        "--skip-rp2040-official-smoke",
        action="store_true",
        help=(
            "Skip only the optional official Seeed smoke image; the SD suite still restores the "
            "exact bridge UF2 before and after raw diagnostics."
        ),
    )
    parser.add_argument("--skip-rp2040-unavailable-capture", action="store_true")
    parser.add_argument(
        "--skip-sd-suite",
        action="store_true",
        help="Skip RP2040 bridge preflight and SD canaries for ESP32/UI-only validation.",
    )
    parser.add_argument(
        "--sd-scenarios",
        default="safe",
        help="Comma-separated boot-prepare scenarios to run after bridge restore; use safe, all, none, or scenario names.",
    )
    parser.add_argument("--include-ui-probes", action="store_true")
    parser.add_argument("--ui-rounds", type=int, default=20)
    parser.add_argument("--uf2-volume")
    parser.add_argument("--uf2-timeout", type=float, default=30.0)
    parser.add_argument("--smoke-capture-timeout", type=float, default=30.0)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--out")
    args = parser.parse_args(argv)
    if args.refresh_rp2040_smoke and args.skip_rp2040_official_smoke:
        parser.error("--refresh-rp2040-smoke conflicts with --skip-rp2040-official-smoke")
    if args.skip_esp32_flash and not args.skip_sd_suite:
        parser.error("--skip-esp32-flash requires --skip-sd-suite because raw SD diagnostics require an exact post-diag reflash boundary")
    if args.ui_rounds <= 0:
        parser.error("--ui-rounds must be positive")
    try:
        args.sd_scenarios = parse_sd_scenarios(args.sd_scenarios)
    except ValueError as exc:
        parser.error(str(exc))
    return args


def parse_sd_scenarios(value: str) -> tuple[str, ...]:
    normalized = (value or "").strip().lower()
    if normalized in {"", "safe", "default"}:
        return AUTONOMOUS_SAFE_SD_SCENARIOS
    if normalized == "none":
        return ()
    if normalized == "all":
        return SD_BOOT_PREPARE_SCENARIOS
    scenarios = tuple(item.strip().lower() for item in normalized.split(",") if item.strip())
    unknown = [scenario for scenario in scenarios if scenario not in SD_BOOT_PREPARE_SCENARIOS]
    if unknown:
        raise ValueError(f"unknown --sd-scenarios value(s): {', '.join(unknown)}")
    return scenarios


def bounded_summary_text(value: object, max_chars: int) -> str | None:
    if value is None:
        return None
    text = str(value)
    if len(text) <= max_chars:
        return text
    return text[: max_chars - 3] + "..."


def console_summary(report: dict, out: Path) -> dict:
    """Return a bounded completion receipt while the full report stays on disk."""
    runs = report.get("runs")
    summary = {
        "schema": 1,
        "kind": "d1l_autonomous_hardware_validation_summary",
        "ok": report.get("ok") is True,
        "mode": bounded_summary_text(report.get("mode"), 32),
        "commit": bounded_summary_text(report.get("commit"), 40),
        "github_actions_run": bounded_summary_text(
            report.get("github_actions_run"), 32
        ),
        "completed_steps": len(runs) if isinstance(runs, list) else 0,
        "report": bounded_summary_text(out, 384),
    }
    if report.get("error"):
        summary["error"] = bounded_summary_text(report["error"], 160)
    if isinstance(report.get("recovery"), dict):
        summary["recovery_ok"] = report["recovery"].get("ok") is True
    if report.get("ready_for_public_release") is not None:
        summary["ready_for_public_release"] = (
            report.get("ready_for_public_release") is True
        )
    return summary


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    if not args.dry_run:
        print(
            json.dumps(
                {
                    "schema": 1,
                    "kind": "d1l_autonomous_hardware_validation",
                    "mode": "hardware",
                    "ok": False,
                    "classification": "hardware_execution_disabled",
                    "error": HARDWARE_EXECUTION_DISABLED,
                    "public_rf_tx": False,
                    "formats_sd": False,
                },
                separators=(",", ":"),
            )
        )
        return 2
    try:
        report = run_validation(args)
    except ValueError as exc:
        print(json.dumps({"schema": 1, "ok": False, "error": str(exc)}))
        return 2

    ctx = build_context(args)
    out = Path(args.out) if args.out else ctx.root / "artifacts" / "hardware" / f"d1l-autonomous-hardware-validation-{ctx.short_commit}-actions{ctx.github_run_id}.json"
    if not out.is_absolute():
        out = ctx.root / out
    write_json(out, report)
    print(json.dumps(console_summary(report, out), separators=(",", ":")))
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
