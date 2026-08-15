#!/usr/bin/env python3
"""Fail-closed RC1 audit for one exact Actions package and physical receipt."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import stat
import sys
import zipfile
from pathlib import Path
from typing import Any

try:
    from capture_core_actions_run_d1l import (
        EXPECTED_ACTIONS_ARTIFACTS,
        validate_capture_receipt,
    )
except ImportError:  # pragma: no cover
    from scripts.capture_core_actions_run_d1l import (
        EXPECTED_ACTIONS_ARTIFACTS,
        validate_capture_receipt,
    )

try:
    from package_release_d1l import validate_production_package_surface
except ImportError:  # pragma: no cover
    from scripts.package_release_d1l import validate_production_package_surface


AUDIT_SCHEMA = 1
RECEIPT_SCHEMA = 1
RECEIPT_KIND = "d1l_rc1_bounded_physical_acceptance"
PROJECT = "MeshCore DeskOS D1L"
RELEASE_PROFILE = "core_1_0"
SD_HISTORY_MODE = "conditional"
REPOSITORY = "n30nex/DeskOS-MeshCore"
PI_HOST = "neopi5"
PI_SERIAL_PATH = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
USB_VID = "1a86"
USB_PID = "7523"
APP_NAME = "meshcore_deskos_d1l.bin"
SD_HISTORY_STATE = "runtime_conditional_sd_primary"
STORAGE_AUTHORITY = "sd_primary_live_only_without_sd"
SD_PREPARATION_SCRIPT = "scripts/prepare_deskos_sd.py"
SD_BUNDLE_ROOT = "sdcard"
SD_MINIMUM_CARD_BYTES = 28_000_000_000
REQUIRED_SD_PREPARATION_PATHS = frozenset(
    {
        SD_PREPARATION_SCRIPT,
        "sdcard/offline-tile-provider.example.json",
        "sdcard/deskos/manifest.json",
        "sdcard/deskos/README.txt",
        "sdcard/deskos/map/manifest.json",
        "sdcard/deskos/map/tiles/README.txt",
    }
)
RC1_USER_INSTALL_FILES = frozenset(
    {
        "START_HERE.md",
        "prepare_sd_card.ps1",
        "prepare_sd_card.sh",
        "flash_rp2040.ps1",
        "flash_rp2040.sh",
        "scripts/flash_rp2040_sd_bridge_uf2.py",
        "scripts/verify_package.py",
    }
)
RC1_USER_INSTALL_KEYS = frozenset(
    {
        "schema",
        "guide",
        "windows",
        "linux",
        "shared",
        "rp2040_artifact",
        "no_sd_format",
        "normal_esp32_flash_erases_flash",
        "files",
    }
)
RC1_WINDOWS_HELPERS = {
    "prepare_sd": "prepare_sd_card.ps1",
    "flash_rp2040": "flash_rp2040.ps1",
    "flash_esp32": "flash_project.ps1",
}
RC1_LINUX_HELPERS = {
    "prepare_sd": "prepare_sd_card.sh",
    "flash_rp2040": "flash_rp2040.sh",
    "flash_esp32": "flash_project.sh",
}
RC1_SHARED_HELPERS = {
    "prepare_sd": SD_PREPARATION_SCRIPT,
    "flash_rp2040": "scripts/flash_rp2040_sd_bridge_uf2.py",
    "verify_package": "scripts/verify_package.py",
}
RC1_RP2040_ARTIFACT = {
    "directory": "rp2040/rp2040-sd-bridge-firmware",
    "uf2": (
        "rp2040/rp2040-sd-bridge-firmware/"
        "deskos_sd_bridge.ino.uf2"
    ),
}
PHYSICAL_EVIDENCE_KIND = "d1l_rc1_bounded_physical_acceptance_evidence"
PHYSICAL_EVIDENCE_KEYS = frozenset(
    {"schema", "kind", "receipt", "candidate", "sources", "coverage"}
)
PHYSICAL_SOURCE_KINDS = {
    "flash": "esp32_flash",
    "rf": "rf_full_acceptance",
    "protocol": "d1l_rc1_protocol_acceptance_transcript",
    "map": "d1l_rc1_map_acceptance_transcript",
}
PHYSICAL_EVIDENCE_COVERAGE = {
    "target": "flash",
    "flash": "flash",
    "boot_advert": "protocol",
    "public_send_count": "protocol",
    "dm_ack": "rf",
    "path": "protocol",
    "ping": "protocol",
    "repeater_login": "protocol",
    "repeater_query": "protocol",
    "authorized_map_download": "map",
    "map_cache_revisit": "map",
}

TOP_LEVEL_RECEIPT_KEYS = frozenset(
    {
        "schema",
        "kind",
        "mode",
        "simulated",
        "dry_run",
        "candidate",
        "target",
        "flash",
        "bounded_gate",
        "outcomes",
    }
)
CANDIDATE_KEYS = frozenset(
    {
        "firmware_commit",
        "actions_run",
        "actions_run_attempt",
        "manifest_sha256",
        "checksum_manifest_sha256",
        "app_path",
        "app_sha256",
    }
)
TARGET_KEYS = frozenset({"host", "path", "vid", "pid"})
FLASH_KEYS = frozenset(
    {
        "performed",
        "method",
        "erase_flash",
        "non_erasing",
        "formats_sd",
        "settings_preserved",
        "artifact_app_sha256",
        "written_app_sha256",
    }
)
BOUNDED_GATE_KEYS = frozenset(
    {"bounded", "soak_required", "duration_requirement_seconds"}
)
OUTCOME_KEYS = frozenset(
    {
        "boot_advert",
        "public_send_count",
        "dm_ack",
        "path",
        "ping",
        "repeater_login",
        "repeater_query",
        "authorized_map_download",
        "map_cache_revisit",
    }
)


class DuplicateJsonKey(ValueError):
    """Raised when an evidence document contains an ambiguous JSON object."""


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateJsonKey(key)
        result[key] = value
    return result


def load_json(path: Path) -> dict[str, Any]:
    try:
        if is_link_or_reparse(path) or not path.is_file():
            return {}
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_strict_object,
        )
    except (DuplicateJsonKey, OSError, UnicodeError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def is_link_or_reparse(path: Path) -> bool:
    try:
        if path.is_symlink():
            return True
        attributes = getattr(path.lstat(), "st_file_attributes", 0)
    except OSError:
        return True
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return bool(attributes & reparse_flag)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def exact_sha(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    return value if re.fullmatch(r"[0-9a-f]{64}", value) else None


def exact_commit(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    return value if re.fullmatch(r"[0-9a-f]{40}", value) else None


def exact_decimal(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    return value if re.fullmatch(r"[1-9][0-9]*", value) else None


def exact_keys(value: object, expected: frozenset[str]) -> bool:
    return isinstance(value, dict) and frozenset(value) == expected


def safe_package_file(root: Path, relative: object) -> Path | None:
    if not isinstance(relative, str) or not relative or "\\" in relative:
        return None
    pure = Path(relative)
    if pure.is_absolute() or pure.drive:
        return None
    if any(part in {"", ".", ".."} for part in pure.parts):
        return None
    try:
        root_resolved = root.resolve(strict=True)
        current = root
        for part in pure.parts:
            current /= part
            if is_link_or_reparse(current):
                return None
        resolved = current.resolve(strict=True)
        resolved.relative_to(root_resolved)
    except (OSError, RuntimeError, ValueError):
        return None
    return current if current.is_file() else None


def _checksum_rows(path: Path, scope: Path) -> dict[str, str] | None:
    try:
        if is_link_or_reparse(path) or not path.is_file():
            return None
        lines = path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeError):
        return None
    if not lines or any(not line.strip() for line in lines):
        return None

    rows: dict[str, str] = {}
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  (?:\./)?(.+)", line)
        if match is None:
            return None
        digest, relative = match.groups()
        if relative in rows:
            return None
        target = safe_package_file(scope, relative)
        if target is None or target == path:
            return None
        rows[relative] = digest
    return rows


def _actual_files(scope: Path, excluded: Path) -> set[str] | None:
    try:
        actual: set[str] = set()
        for candidate in scope.rglob("*"):
            if is_link_or_reparse(candidate):
                return None
            if candidate.is_file():
                if candidate != excluded:
                    actual.add(candidate.relative_to(scope).as_posix())
            elif not candidate.is_dir():
                return None
    except (OSError, RuntimeError, ValueError):
        return None
    return actual


def verify_checksum_manifest(path: Path, scope: Path) -> bool:
    rows = _checksum_rows(path, scope)
    actual = _actual_files(scope, path)
    if rows is None or actual is None or set(rows) != actual:
        return False
    try:
        return all(
            sha256_file(scope / relative) == expected
            for relative, expected in rows.items()
        )
    except OSError:
        return False


def verify_single_sha256(path: Path) -> bool:
    try:
        if is_link_or_reparse(path) or not path.is_file():
            return False
        lines = path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeError):
        return False
    if len(lines) != 1:
        return False
    match = re.fullmatch(r"([0-9a-f]{64})  (?:\./)?([^\\]+)", lines[0])
    if match is None:
        return False
    expected, relative = match.groups()
    target = safe_package_file(path.parent, relative)
    try:
        return target is not None and sha256_file(target) == expected
    except OSError:
        return False


def verify_checksum_tree(package_dir: Path) -> bool:
    try:
        if is_link_or_reparse(package_dir) or not package_dir.is_dir():
            return False
        root_manifest = package_dir / "SHA256SUMS.txt"
        manifests = sorted(package_dir.rglob("SHA256SUMS.txt"))
        single_files = sorted(package_dir.rglob("*.sha256"))
    except (OSError, RuntimeError):
        return False
    if root_manifest not in manifests:
        return False
    for manifest in manifests:
        if not verify_checksum_manifest(manifest, manifest.parent):
            return False
    return all(verify_single_sha256(path) for path in single_files)


def manifest_identity(
    manifest: dict[str, Any],
) -> tuple[bool, str | None, str | None, str | None]:
    commit = exact_commit(manifest.get("firmware_commit"))
    run = exact_decimal(manifest.get("actions_run"))
    attempt = exact_decimal(manifest.get("actions_run_attempt"))
    git = manifest.get("git")
    workflow = manifest.get("workflow")
    ok = (
        manifest.get("schema") == 2
        and manifest.get("project") == PROJECT
        and commit is not None
        and run is not None
        and attempt is not None
        and isinstance(git, dict)
        and git.get("commit") == commit
        and git.get("dirty") is False
        and git.get("dirty_entries") == []
        and isinstance(workflow, dict)
        and workflow.get("sha") == commit
        and workflow.get("run_id") == run
        and workflow.get("run_attempt") == attempt
        and workflow.get("repository") == REPOSITORY
    )
    return ok, commit, run, attempt


def package_install_contract(manifest: dict[str, Any]) -> bool:
    install = manifest.get("install_recovery_guide")
    if not isinstance(install, dict):
        return False
    targets = install.get("normal_install_targets")
    policy = install.get("target_policy")
    posix = targets.get("posix") if isinstance(targets, dict) else None
    return (
        isinstance(posix, dict)
        and posix.get("requested_path") == PI_SERIAL_PATH
        and posix.get("target_kind") == "posix_by_id"
        and posix.get("vid") == int(USB_VID, 16)
        and posix.get("pid") == int(USB_PID, 16)
        and isinstance(policy, dict)
        and policy.get("stable_requested_path_only") is True
        and policy.get("resolved_tty_observational_only") is True
        and policy.get("hardware_identity_required") is True
        and policy.get("raw_posix_tty_forbidden") is True
        and install.get("normal_install_preserves_unrelated_nvs") is True
        and install.get("normal_install_checksum_verified") is True
        and install.get("no_on_device_sd_format") is True
    )


def production_package_surface_contract(package_dir: Path) -> bool:
    try:
        validate_production_package_surface(package_dir)
    except (OSError, RuntimeError, ValueError):
        return False
    return True


def app_artifact(
    package_dir: Path,
    manifest: dict[str, Any],
) -> tuple[bool, str | None, str | None]:
    entries = manifest.get("flash_files")
    if not isinstance(entries, list):
        return False, None, None
    matches = [
        row
        for row in entries
        if isinstance(row, dict)
        and row.get("role") == "app"
        and isinstance(row.get("source"), str)
        and Path(row["source"]).name == APP_NAME
    ]
    if len(matches) != 1:
        return False, None, None
    row = matches[0]
    path_text = row.get("path")
    expected = exact_sha(row.get("sha256"))
    path = safe_package_file(package_dir, path_text)
    try:
        ok = (
            path is not None
            and expected is not None
            and type(row.get("size")) is int
            and row["size"] > 0
            and path.stat().st_size == row["size"]
            and sha256_file(path) == expected
        )
    except OSError:
        ok = False
    return (
        ok,
        path_text if ok and isinstance(path_text, str) else None,
        expected if ok else None,
    )


def _file_inventory(root: Path) -> dict[str, tuple[int, str]] | None:
    try:
        if is_link_or_reparse(root) or not root.is_dir():
            return None
        rows: dict[str, tuple[int, str]] = {}
        for path in sorted(root.rglob("*")):
            if is_link_or_reparse(path):
                return None
            if path.is_file():
                rows[path.relative_to(root).as_posix()] = (
                    path.stat().st_size,
                    sha256_file(path),
                )
            elif not path.is_dir():
                return None
    except (OSError, RuntimeError, ValueError):
        return None
    return rows


def rc1_user_install_contract(
    package_dir: Path,
    manifest: dict[str, Any],
) -> bool:
    user_install = manifest.get("user_install")
    if not (
        exact_keys(user_install, RC1_USER_INSTALL_KEYS)
        and user_install.get("schema") == 1
        and user_install.get("guide") == "START_HERE.md"
        and user_install.get("windows") == RC1_WINDOWS_HELPERS
        and user_install.get("linux") == RC1_LINUX_HELPERS
        and user_install.get("shared") == RC1_SHARED_HELPERS
        and user_install.get("rp2040_artifact") == RC1_RP2040_ARTIFACT
        and user_install.get("no_sd_format") is True
        and user_install.get("normal_esp32_flash_erases_flash") is False
        and isinstance(user_install.get("files"), dict)
        and set(user_install["files"]) == RC1_USER_INSTALL_FILES
    ):
        return False

    for relative, binding in user_install["files"].items():
        if not exact_keys(binding, frozenset({"size", "sha256"})):
            return False
        path = safe_package_file(package_dir, relative)
        expected = exact_sha(binding.get("sha256"))
        try:
            if not (
                path is not None
                and path.is_file()
                and type(binding.get("size")) is int
                and binding["size"] > 0
                and path.stat().st_size == binding["size"]
                and expected is not None
                and sha256_file(path) == expected
            ):
                return False
        except OSError:
            return False

    referenced = {
        *RC1_WINDOWS_HELPERS.values(),
        *RC1_LINUX_HELPERS.values(),
        *RC1_SHARED_HELPERS.values(),
        RC1_RP2040_ARTIFACT["uf2"],
    }
    if any(safe_package_file(package_dir, relative) is None for relative in referenced):
        return False

    guide = safe_package_file(package_dir, "START_HERE.md")
    if guide is None:
        return False
    try:
        guide_text = guide.read_text(encoding="ascii")
    except (OSError, UnicodeError):
        return False
    required_guide_text = (
        "# DeskOS D1L 1.0 - Windows and Linux Install",
        str(manifest.get("firmware_commit")),
        "GitHub Actions run and attempt: see `manifest.json`",
        "prepare_sd_card.ps1",
        "prepare_sd_card.sh",
        "flash_rp2040.ps1",
        "flash_rp2040.sh",
        "flash_project.ps1",
        "flash_project.sh",
        "never a raw",
    )
    return all(value in guide_text for value in required_guide_text)


def sd_primary_package_contract(
    package_dir: Path, manifest: dict[str, Any]
) -> bool:
    preparation = manifest.get("sd_preparation")
    supported = manifest.get("supported_capabilities")
    unavailable = manifest.get("unavailable_capabilities")
    if not (
        manifest.get("sd_history_state") == SD_HISTORY_STATE
        and manifest.get("storage_authority") == STORAGE_AUTHORITY
        and isinstance(supported, list)
        and "sd_history" in supported
        and isinstance(unavailable, list)
        and "sd_history" not in unavailable
        and exact_keys(
            preparation,
            frozenset(
                {
                    "schema",
                    "script",
                    "bundle_root",
                    "minimum_card_bytes",
                    "filesystem",
                    "formats_sd",
                    "files",
                }
            ),
        )
        and preparation.get("schema") == 1
        and preparation.get("script") == SD_PREPARATION_SCRIPT
        and preparation.get("bundle_root") == SD_BUNDLE_ROOT
        and preparation.get("minimum_card_bytes") == SD_MINIMUM_CARD_BYTES
        and preparation.get("filesystem") == "FAT32"
        and preparation.get("formats_sd") is False
        and isinstance(preparation.get("files"), list)
        and rc1_user_install_contract(package_dir, manifest)
    ):
        return False

    rows_by_path: dict[str, dict[str, Any]] = {}
    for row in preparation["files"]:
        if not exact_keys(row, frozenset({"path", "size", "sha256"})):
            return False
        path_text = row.get("path")
        if (
            not isinstance(path_text, str)
            or path_text in rows_by_path
            or (
                path_text != SD_PREPARATION_SCRIPT
                and not path_text.startswith(SD_BUNDLE_ROOT + "/")
            )
        ):
            return False
        path = safe_package_file(package_dir, path_text)
        expected = exact_sha(row.get("sha256"))
        try:
            if not (
                path is not None
                and expected is not None
                and type(row.get("size")) is int
                and row["size"] >= 0
                and path.stat().st_size == row["size"]
                and sha256_file(path) == expected
            ):
                return False
        except OSError:
            return False
        rows_by_path[path_text] = row

    if not REQUIRED_SD_PREPARATION_PATHS.issubset(rows_by_path):
        return False
    sd_root = package_dir / SD_BUNDLE_ROOT
    actual_sd = _file_inventory(sd_root)
    if actual_sd is None:
        return False
    listed_sd = {
        path.removeprefix(SD_BUNDLE_ROOT + "/"): (
            row["size"],
            row["sha256"],
        )
        for path, row in rows_by_path.items()
        if path.startswith(SD_BUNDLE_ROOT + "/")
    }
    return actual_sd == listed_sd


def actions_capture_contract(
    *,
    package_dir: Path,
    manifest: dict[str, Any],
    actions_receipt: Path,
    repository_root: Path,
    commit: str | None,
    run: str | None,
    attempt: str | None,
) -> tuple[bool, str | None]:
    if commit is None or run is None or attempt is None:
        return False, None
    try:
        repository_root = repository_root.resolve(strict=True)
        actions_receipt = Path(actions_receipt)
        if not actions_receipt.is_absolute():
            actions_receipt = repository_root / actions_receipt
        if (
            is_link_or_reparse(repository_root)
            or not repository_root.is_dir()
            or is_link_or_reparse(actions_receipt)
            or not actions_receipt.is_file()
            or actions_receipt.name != f"core_actions_run_{run}.json"
            or actions_receipt.parent.name != "core-actions-run-metadata"
        ):
            return False, None
        actions_receipt.resolve(strict=True).relative_to(repository_root)
        github_run_dir = actions_receipt.parent.parent
        verified = validate_capture_receipt(
            receipt_path=actions_receipt,
            root=repository_root,
            github_run_dir=github_run_dir,
            commit=commit,
            run_id=run,
            run_attempt=attempt,
        )
        artifact_names = [
            row.get("name")
            for row in verified.get("artifacts", [])
            if isinstance(row, dict)
        ]
        package_name = manifest.get("package")
        actions_package = (
            github_run_dir / "d1l-release-package" / str(package_name)
        )
        local_inventory = _file_inventory(package_dir)
        actions_inventory = _file_inventory(actions_package)
        ok = (
            verified.get("ok") is True
            and verified.get("run_attempt") == attempt
            and artifact_names == list(EXPECTED_ACTIONS_ARTIFACTS)
            and package_name == f"d1l-release-{commit}"
            and local_inventory is not None
            and local_inventory == actions_inventory
        )
        return ok, sha256_file(actions_receipt) if ok else None
    except (OSError, RuntimeError, TypeError, ValueError, zipfile.BadZipFile):
        return False, None


def receipt_shape(receipt: dict[str, Any]) -> bool:
    return (
        exact_keys(receipt, TOP_LEVEL_RECEIPT_KEYS)
        and receipt.get("schema") == RECEIPT_SCHEMA
        and receipt.get("kind") == RECEIPT_KIND
        and receipt.get("mode") == "physical"
        and receipt.get("simulated") is False
        and receipt.get("dry_run") is False
        and exact_keys(receipt.get("candidate"), CANDIDATE_KEYS)
        and exact_keys(receipt.get("target"), TARGET_KEYS)
        and exact_keys(receipt.get("flash"), FLASH_KEYS)
        and exact_keys(receipt.get("bounded_gate"), BOUNDED_GATE_KEYS)
        and exact_keys(receipt.get("outcomes"), OUTCOME_KEYS)
    )


def _machine_source_payload(
    role: str,
    payload: dict[str, Any],
    candidate: dict[str, Any],
) -> bool:
    def rejected(value: object) -> bool:
        if isinstance(value, dict):
            for key, child in value.items():
                normalized = key.strip().lower().replace("-", "_")
                if (
                    normalized
                    in {
                        "dry_run",
                        "simulated",
                        "simulation",
                        "manual",
                        "manual_only",
                        "source_inspection",
                        "source_only",
                    }
                    and child is True
                ):
                    return True
                if (
                    normalized.endswith("mode")
                    and isinstance(child, str)
                    and any(
                        marker in child.strip().lower().replace("_", "-")
                        for marker in {
                            "dry-run",
                            "simulation",
                            "simulated",
                            "manual-only",
                            "source-inspection",
                        }
                    )
                ):
                    return True
                if rejected(child):
                    return True
        elif isinstance(value, list):
            return any(rejected(child) for child in value)
        return False

    if (
        not payload
        or payload.get("kind") != PHYSICAL_SOURCE_KINDS.get(role)
        or rejected(payload)
    ):
        return False
    mode = payload.get("mode")
    physical = payload.get("physical_observed")
    if role == "flash":
        return (
            mode == "hardware"
            and physical is True
            and payload.get("dry_run") is False
            and payload.get("simulated") is False
            and payload.get("simulation") is not True
            and payload.get("source_inspection") is not True
            and payload.get("manual_only") is False
            and payload.get("retained_nonempty_baseline") is True
        )
    if role == "rf":
        return (
            mode == "rf-full-acceptance"
            and physical is True
            and payload.get("dry_run") is False
            and payload.get("simulated") is False
            and payload.get("simulation") is not True
            and payload.get("source_inspection") is not True
            and payload.get("manual_only") is False
        )
    if role in {"protocol", "map"}:
        return (
            mode == "hardware"
            and physical is True
            and payload.get("dry_run") is False
            and payload.get("simulated") is False
            and payload.get("manual_only") is False
        )
    return False


def physical_evidence_contract(
    *,
    physical_receipt: Path,
    physical_evidence: Path,
    receipt: dict[str, Any],
    repository_root: Path,
) -> tuple[bool, str | None]:
    physical_receipt = Path(physical_receipt)
    physical_evidence = Path(physical_evidence)
    try:
        if (
            is_link_or_reparse(physical_receipt)
            or not physical_receipt.is_file()
            or is_link_or_reparse(physical_evidence)
            or not physical_evidence.is_file()
            or physical_evidence.parent.resolve(strict=True)
            != physical_receipt.parent.resolve(strict=True)
            or physical_evidence.name
            != physical_receipt.stem + ".evidence.json"
        ):
            return False, None
        evidence = load_json(physical_evidence)
        receipt_row = evidence.get("receipt")
        candidate = evidence.get("candidate")
        sources = evidence.get("sources")
        coverage = evidence.get("coverage")
        if not (
            exact_keys(evidence, PHYSICAL_EVIDENCE_KEYS)
            and evidence.get("schema") == 1
            and evidence.get("kind") == PHYSICAL_EVIDENCE_KIND
            and exact_keys(receipt_row, frozenset({"path", "sha256"}))
            and exact_keys(candidate, CANDIDATE_KEYS)
            and candidate == receipt.get("candidate")
            and exact_keys(sources, frozenset(PHYSICAL_SOURCE_KINDS))
            and coverage == PHYSICAL_EVIDENCE_COVERAGE
        ):
            return False, None
        bound_receipt = safe_package_file(
            physical_evidence.parent, receipt_row.get("path")
        )
        if not (
            bound_receipt is not None
            and bound_receipt.resolve(strict=True)
            == physical_receipt.resolve(strict=True)
            and exact_sha(receipt_row.get("sha256"))
            == sha256_file(physical_receipt)
        ):
            return False, None

        source_paths: set[str] = set()
        source_hashes: set[str] = set()
        source_payloads: dict[str, dict[str, Any]] = {}
        for role, expected_kind in PHYSICAL_SOURCE_KINDS.items():
            row = sources.get(role)
            if not exact_keys(row, frozenset({"path", "sha256", "kind"})):
                return False, None
            path_text = row.get("path")
            digest = exact_sha(row.get("sha256"))
            if (
                not isinstance(path_text, str)
                or path_text in source_paths
                or digest is None
                or digest in source_hashes
                or row.get("kind") != expected_kind
            ):
                return False, None
            path = safe_package_file(physical_evidence.parent, path_text)
            payload = load_json(path) if path is not None else {}
            if (
                path is None
                or sha256_file(path) != digest
                or not _machine_source_payload(
                    role, payload, candidate
                )
            ):
                return False, None
            source_paths.add(path_text)
            source_hashes.add(digest)
            source_payloads[role] = payload

        try:
            if __package__:
                from .produce_rc1_bounded_physical_receipt_d1l import (
                    EvidenceError,
                    VALIDATORS,
                    validate_rf,
                )
            else:  # pragma: no cover - direct script execution
                from produce_rc1_bounded_physical_receipt_d1l import (
                    EvidenceError,
                    VALIDATORS,
                    validate_rf,
                )
        except ImportError:
            return False, None
        if (
            set(VALIDATORS) != set(PHYSICAL_SOURCE_KINDS)
            or not all(callable(validator) for validator in VALIDATORS.values())
        ):
            return False, None
        semantic_outcomes: dict[str, bool | int] = {}
        try:
            for role in PHYSICAL_SOURCE_KINDS:
                validator = VALIDATORS[role]
                derived = (
                    validator(
                        source_payloads[role],
                        candidate,
                        evidence_root=repository_root,
                    )
                    if role == "rf" and validator is validate_rf
                    else validator(source_payloads[role], candidate)
                )
                if set(semantic_outcomes).intersection(derived):
                    return False, None
                semantic_outcomes.update(derived)
        except (EvidenceError, KeyError, OSError, TypeError, ValueError):
            return False, None
        if semantic_outcomes != receipt.get("outcomes"):
            return False, None
        return True, sha256_file(physical_evidence)
    except (OSError, RuntimeError, TypeError, ValueError):
        return False, None


def package_binding(
    receipt: dict[str, Any],
    *,
    commit: str | None,
    run: str | None,
    attempt: str | None,
    manifest_sha256: str | None,
    checksum_manifest_sha256: str | None,
    app_path: str | None,
    app_sha256: str | None,
) -> bool:
    candidate = receipt.get("candidate")
    return (
        isinstance(candidate, dict)
        and commit is not None
        and run is not None
        and attempt is not None
        and manifest_sha256 is not None
        and checksum_manifest_sha256 is not None
        and app_path is not None
        and app_sha256 is not None
        and candidate.get("firmware_commit") == commit
        and candidate.get("actions_run") == run
        and candidate.get("actions_run_attempt") == attempt
        and candidate.get("manifest_sha256") == manifest_sha256
        and candidate.get("checksum_manifest_sha256")
        == checksum_manifest_sha256
        and candidate.get("app_path") == app_path
        and candidate.get("app_sha256") == app_sha256
    )


def audit(
    package_dir: Path,
    actions_receipt: Path,
    physical_receipt: Path,
    physical_evidence: Path,
    *,
    repository_root: Path | None = None,
) -> dict[str, Any]:
    package_dir = Path(package_dir)
    actions_receipt = Path(actions_receipt)
    physical_receipt = Path(physical_receipt)
    physical_evidence = Path(physical_evidence)
    repository_root = Path(repository_root or ".")
    manifest_path = package_dir / "manifest.json"
    checksums_path = package_dir / "SHA256SUMS.txt"
    manifest = load_json(manifest_path)
    receipt = load_json(physical_receipt)
    physical_evidence_ok, physical_evidence_sha256 = (
        physical_evidence_contract(
            physical_receipt=physical_receipt,
            physical_evidence=physical_evidence,
            receipt=receipt,
            repository_root=repository_root,
        )
    )

    checksum_tree_ok = verify_checksum_tree(package_dir)
    identity_ok, commit, run, attempt = manifest_identity(manifest)
    app_ok, app_path, app_sha256 = app_artifact(package_dir, manifest)
    actions_ok, actions_receipt_sha256 = actions_capture_contract(
        package_dir=package_dir,
        manifest=manifest,
        actions_receipt=actions_receipt,
        repository_root=repository_root,
        commit=commit,
        run=run,
        attempt=attempt,
    )
    try:
        manifest_sha256 = (
            sha256_file(manifest_path)
            if not is_link_or_reparse(manifest_path) and manifest_path.is_file()
            else None
        )
        checksum_manifest_sha256 = (
            sha256_file(checksums_path)
            if not is_link_or_reparse(checksums_path)
            and checksums_path.is_file()
            else None
        )
    except OSError:
        manifest_sha256 = None
        checksum_manifest_sha256 = None

    shape_ok = receipt_shape(receipt)
    target = receipt.get("target")
    flash = receipt.get("flash")
    bounded = receipt.get("bounded_gate")
    outcomes = receipt.get("outcomes")

    checks = {
        "package_checksum_tree_and_manifest": (
            checksum_tree_ok and bool(manifest)
        ),
        "package_core_1_0_conditional": (
            manifest.get("release_profile") == RELEASE_PROFILE
            and manifest.get("sd_history_mode") == SD_HISTORY_MODE
        ),
        "package_sd_primary_truth_and_preparation": (
            sd_primary_package_contract(package_dir, manifest)
        ),
        "package_exact_commit_run_attempt": identity_ok,
        "actions_successful_main_push_exact_eight_artifacts_and_package": (
            actions_ok
        ),
        "package_stable_pi_install_contract": package_install_contract(manifest),
        "package_production_only_public_surface": (
            production_package_surface_contract(package_dir)
        ),
        "package_exact_app_artifact": app_ok,
        "one_bounded_physical_receipt": shape_ok,
        "physical_evidence_sidecar_machine_sources": physical_evidence_ok,
        "receipt_exact_package_binding": package_binding(
            receipt,
            commit=commit,
            run=run,
            attempt=attempt,
            manifest_sha256=manifest_sha256,
            checksum_manifest_sha256=checksum_manifest_sha256,
            app_path=app_path,
            app_sha256=app_sha256,
        ),
        "stable_pi_path_and_vid_pid": (
            isinstance(target, dict)
            and target.get("host") == PI_HOST
            and target.get("path") == PI_SERIAL_PATH
            and target.get("vid") == USB_VID
            and target.get("pid") == USB_PID
        ),
        "non_erasing_exact_app_flash": (
            isinstance(flash, dict)
            and flash.get("performed") is True
            and flash.get("method") == "project_write_flash"
            and flash.get("erase_flash") is False
            and flash.get("non_erasing") is True
            and app_sha256 is not None
            and flash.get("artifact_app_sha256") == app_sha256
            and flash.get("written_app_sha256") == app_sha256
        ),
        "formats_sd_false_and_settings_preserved": (
            isinstance(flash, dict)
            and flash.get("formats_sd") is False
            and flash.get("settings_preserved") is True
        ),
        "bounded_gate_without_soak_or_duration_requirement": (
            isinstance(bounded, dict)
            and bounded.get("bounded") is True
            and bounded.get("soak_required") is False
            and bounded.get("duration_requirement_seconds") is None
        ),
        "boot_advert_and_one_public_send": (
            isinstance(outcomes, dict)
            and outcomes.get("boot_advert") is True
            and type(outcomes.get("public_send_count")) is int
            and outcomes.get("public_send_count") == 1
        ),
        "dm_ack": (
            isinstance(outcomes, dict) and outcomes.get("dm_ack") is True
        ),
        "path_and_ping": (
            isinstance(outcomes, dict)
            and outcomes.get("path") is True
            and outcomes.get("ping") is True
        ),
        "repeater_login_and_query": (
            isinstance(outcomes, dict)
            and outcomes.get("repeater_login") is True
            and outcomes.get("repeater_query") is True
        ),
        "authorized_map_download_and_cache_revisit": (
            isinstance(outcomes, dict)
            and outcomes.get("authorized_map_download") is True
            and outcomes.get("map_cache_revisit") is True
        ),
    }
    failures = [name for name, ok in checks.items() if not ok]
    return {
        "schema": AUDIT_SCHEMA,
        "kind": "d1l_rc1_release_gate_audit",
        "ready_for_public_release": not failures,
        "identity": {
            "firmware_commit": commit,
            "actions_run": run,
            "actions_run_attempt": attempt,
            "actions_capture_receipt_sha256": actions_receipt_sha256,
            "physical_evidence_sha256": physical_evidence_sha256,
            "manifest_sha256": manifest_sha256,
            "checksum_manifest_sha256": checksum_manifest_sha256,
            "app_path": app_path,
            "app_sha256": app_sha256,
        },
        "checks": checks,
        "failures": failures,
    }


def canonical_json(report: dict[str, Any]) -> str:
    return json.dumps(
        report,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ) + "\n"


def _is_within(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
    except (OSError, RuntimeError, ValueError):
        return False
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Audit one exact RC1 Actions package and one bounded physical D1L "
            "receipt; soak and duration requirements are forbidden."
        )
    )
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--actions-receipt", type=Path, required=True)
    parser.add_argument("--physical-receipt", type=Path, required=True)
    parser.add_argument("--physical-evidence", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)

    if args.output is not None and _is_within(
        args.output, args.package_dir
    ):
        parser.error("--output must be outside --package-dir")
    if (
        args.output is not None
        and args.output.resolve()
        in {
            args.actions_receipt.resolve(),
            args.physical_receipt.resolve(),
            args.physical_evidence.resolve(),
        }
    ):
        parser.error("--output must not overwrite an input receipt")

    report = audit(
        args.package_dir,
        args.actions_receipt,
        args.physical_receipt,
        args.physical_evidence,
        repository_root=args.root,
    )
    payload = canonical_json(report)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="ascii")
    sys.stdout.write(payload)
    return 0 if report["ready_for_public_release"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
