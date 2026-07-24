#!/usr/bin/env python3
"""Strict, separate release audit for MeshCore DeskOS D1L Core 1.0."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


DEFECT_RECEIPT_MAX_AGE_SEC = 15 * 60
DEFECT_RECEIPT_MAX_FUTURE_SKEW_SEC = 30

try:
    import release_gate_audit_d1l as full_release_audit
    import rf_full_acceptance_d1l as rf_acceptance
    import soak_d1l as soak_runner
    import time_protocol_migration_d1l as time_protocol_migration
    from artifact_metadata import git_metadata
    from capture_core_actions_run_d1l import (
        EXPECTED_ACTIONS_ARTIFACTS,
        tree_inventory,
        validate_artifacts,
        validate_capture_receipt,
        validate_run,
        zip_inventory,
    )
    from core_install_recovery_review_d1l import (
        validate_install_review_receipt,
    )
    from manual_ui_review_d1l import (
        validate_core_manual_ui_review_receipt,
    )
    from core_smoke_d1l import (
        CORE_RELEASE_PROFILE,
        CORE_SMOKE_COMMANDS,
        D1L_CORE_PORT,
        EXPECTED_IDF_VERSION,
        crashlog_clean,
        disabled_storage_status_ok,
        exact_unsupported_result,
        exact_unavailable_status_result,
        mutation_probe_plan,
        unavailable_status_probe_plan,
    )
    from core_flash_only_d1l import (
        FLASH_PHASE_RETAINED_REFLASH,
        RETAINED_STATE_COMMANDS,
        projection_sha256,
        retained_state_preserved,
        retained_state_projection,
    )
    from core_ui_corruption_probe_d1l import (
        CORE_COMPOSE_TARGETS,
        CORE_SCROLL_SURFACES,
        CORE_TAB_SEQUENCE,
        RELEASE_MIN_ROUNDS,
        core_compose_result_ok,
        core_scroll_result_ok,
        core_tab_sequence_ok,
        unavailable_ui_events_ok,
        unavailable_ui_probe_plan,
    )
    from package_release_d1l import core_capability_truth
    from provenance_d1l import (
        discover_source_identity,
        validate_against_inputs as validate_provenance_against_inputs,
        validate_core_actions_binding,
    )
    from release_gate_audit_d1l import (
        checksum_gate,
        esp32_flash_receipt_gate,
        find_release_package,
        host_checks_success_gate,
        immutable_release_source_inputs_gate,
        meshcore_conformance_evidence_gate,
        meshcore_signed_advert_evidence_gate,
        newest_commit_json,
        notices_gate,
    )
    from verify_checksums import (
        is_link_or_reparse,
        verify_sha256_manifest,
    )
    from ui_corruption_probe_d1l import event_failures as ui_event_failures
    from smoke_d1l import expected_command_name, reboot_command_passed
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts import release_gate_audit_d1l as full_release_audit
    from scripts import rf_full_acceptance_d1l as rf_acceptance
    from scripts import soak_d1l as soak_runner
    from scripts import time_protocol_migration_d1l as time_protocol_migration
    from scripts.artifact_metadata import git_metadata
    from scripts.capture_core_actions_run_d1l import (
        EXPECTED_ACTIONS_ARTIFACTS,
        tree_inventory,
        validate_artifacts,
        validate_capture_receipt,
        validate_run,
        zip_inventory,
    )
    from scripts.core_install_recovery_review_d1l import (
        validate_install_review_receipt,
    )
    from scripts.manual_ui_review_d1l import (
        validate_core_manual_ui_review_receipt,
    )
    from scripts.core_smoke_d1l import (
        CORE_RELEASE_PROFILE,
        CORE_SMOKE_COMMANDS,
        D1L_CORE_PORT,
        EXPECTED_IDF_VERSION,
        crashlog_clean,
        disabled_storage_status_ok,
        exact_unsupported_result,
        exact_unavailable_status_result,
        mutation_probe_plan,
        unavailable_status_probe_plan,
    )
    from scripts.core_flash_only_d1l import (
        FLASH_PHASE_RETAINED_REFLASH,
        RETAINED_STATE_COMMANDS,
        projection_sha256,
        retained_state_preserved,
        retained_state_projection,
    )
    from scripts.core_ui_corruption_probe_d1l import (
        CORE_COMPOSE_TARGETS,
        CORE_SCROLL_SURFACES,
        CORE_TAB_SEQUENCE,
        RELEASE_MIN_ROUNDS,
        core_compose_result_ok,
        core_scroll_result_ok,
        core_tab_sequence_ok,
        unavailable_ui_events_ok,
        unavailable_ui_probe_plan,
    )
    from scripts.package_release_d1l import core_capability_truth
    from scripts.provenance_d1l import (
        discover_source_identity,
        validate_against_inputs as validate_provenance_against_inputs,
        validate_core_actions_binding,
    )
    from scripts.release_gate_audit_d1l import (
        checksum_gate,
        esp32_flash_receipt_gate,
        find_release_package,
        host_checks_success_gate,
        immutable_release_source_inputs_gate,
        meshcore_conformance_evidence_gate,
        meshcore_signed_advert_evidence_gate,
        newest_commit_json,
        notices_gate,
    )
    from scripts.verify_checksums import (
        is_link_or_reparse,
        verify_sha256_manifest,
    )
    from scripts.ui_corruption_probe_d1l import (
        event_failures as ui_event_failures,
    )
    from scripts.smoke_d1l import (
        expected_command_name,
        reboot_command_passed,
    )

try:
    from core_reboot_persistence_d1l import (
        validate_core_reboot_persistence_receipt,
    )
except ImportError:  # pragma: no cover - integration dependency
    try:
        from scripts.core_reboot_persistence_d1l import (
            validate_core_reboot_persistence_receipt,
        )
    except ImportError:  # pragma: no cover - fail closed until R11 lands
        validate_core_reboot_persistence_receipt = None

try:
    from capture_core_github_defects_d1l import (
        validate_core_github_defect_receipt,
    )
except ImportError:  # pragma: no cover - integration dependency
    try:
        from scripts.capture_core_github_defects_d1l import (
            validate_core_github_defect_receipt,
        )
    except ImportError:  # pragma: no cover - fail closed until R8 lands
        validate_core_github_defect_receipt = None


CORE_AUDIT_SCHEMA = 1
FINAL_SD_HISTORY_MODES = frozenset({"disabled"})
REQUIRED_ACTIONS_ARTIFACTS = EXPECTED_ACTIONS_ARTIFACTS
REQUIRED_MANUAL_CONFIRMATIONS = (
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
REJECTED_TRUE_FLAGS = (
    "dry_run",
    "simulated",
    "simulation",
    "source_inspection",
    "source_only",
    "fabricated",
    "edited",
    "predecessor",
)


@dataclass
class CoreGate:
    gate_id: str
    ok: bool
    title: str
    evidence: list[str] = field(default_factory=list)
    details: dict[str, Any] = field(default_factory=dict)
    severity: str = "P0"

    def to_dict(self) -> dict:
        return {
            "id": self.gate_id,
            "severity": self.severity,
            "ok": self.ok,
            "title": self.title,
            "evidence": self.evidence,
            "details": self.details,
        }


def exact_sha(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    return normalized if re.fullmatch(r"[0-9a-f]{40}", normalized) else None


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path | None) -> dict:
    if path is None or not path.is_file() or is_link_or_reparse(path):
        return {}
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def path_text(path: Path | None, root: Path) -> list[str]:
    if path is None:
        return []
    try:
        return [path.resolve().relative_to(root.resolve()).as_posix()]
    except ValueError:
        return [str(path.resolve())]


def real_evidence(data: dict) -> bool:
    return (
        bool(data)
        and all(data.get(flag) is not True for flag in REJECTED_TRUE_FLAGS)
        and data.get("mode") not in {
            "dry-run",
            "plan",
            "source-inspection",
            "simulation",
        }
    )


def exact_candidate_fields(data: dict, commit: str) -> bool:
    expected = exact_sha(data.get("expected_firmware_commit"))
    device = exact_sha(data.get("device_build_commit"))
    git_info = data.get("git")
    return (
        expected == commit
        and device == commit
        and data.get("firmware_identity_required") is True
        and data.get("firmware_identity_ok") is True
        and isinstance(git_info, dict)
        and exact_sha(git_info.get("commit")) == commit
        and git_info.get("dirty") is False
        and git_info.get("dirty_entries") == []
    )


def all_true(value: object) -> bool:
    return (
        isinstance(value, dict)
        and bool(value)
        and all(item is True for item in value.values())
    )


def actions_inventory_gate(github_run_dir: Path, root: Path) -> CoreGate:
    missing: list[str] = []
    linked: list[str] = []
    empty: list[str] = []
    inventories: dict[str, dict] = {}
    for artifact_name in REQUIRED_ACTIONS_ARTIFACTS:
        artifact_dir = github_run_dir / artifact_name
        if not artifact_dir.is_dir():
            missing.append(artifact_name)
            continue
        files = sorted(path for path in artifact_dir.rglob("*") if path.is_file())
        if not files:
            empty.append(artifact_name)
            continue
        rows = []
        for path in files:
            if is_link_or_reparse(path):
                linked.append(path.relative_to(github_run_dir).as_posix())
                continue
            rows.append(
                {
                    "path": path.relative_to(artifact_dir).as_posix(),
                    "size": path.stat().st_size,
                    "sha256": sha256_file(path),
                }
            )
        canonical = json.dumps(rows, sort_keys=True, separators=(",", ":")).encode(
            "ascii"
        )
        inventories[artifact_name] = {
            "file_count": len(rows),
            "aggregate_sha256": hashlib.sha256(canonical).hexdigest(),
            "files": rows,
        }
    ok = not missing and not empty and not linked and len(inventories) == len(
        REQUIRED_ACTIONS_ARTIFACTS
    )
    return CoreGate(
        "actions_artifacts_downloaded_and_checksummed",
        ok,
        "Every required Actions artifact is downloaded and checksummed",
        path_text(github_run_dir, root),
        {
            "missing": missing,
            "empty": empty,
            "linked_or_reparse": linked,
            "inventories": inventories,
        },
    )


def _normalized_packages(value: object) -> dict[str, str] | None:
    if not isinstance(value, list):
        return None
    packages: dict[str, str] = {}
    for item in value:
        if not isinstance(item, dict):
            return None
        name = item.get("name")
        version = item.get("version")
        if (
            not isinstance(name, str)
            or not name.strip()
            or not isinstance(version, str)
            or not version.strip()
        ):
            return None
        normalized = re.sub(r"[-_.]+", "-", name.strip()).lower()
        if normalized in packages:
            return None
        packages[normalized] = version.strip()
    return packages


def core_immutable_source_inputs_gate(
    github_run_dir: Path,
    root: Path,
    commit: str,
    run_id: str,
    sd_history_mode: str,
) -> CoreGate:
    if sd_history_mode == "supported_optional":
        return imported_gate(
            immutable_release_source_inputs_gate(
                github_run_dir, root, commit, run_id
            )
        )

    metadata_path = (
        root / full_release_audit.SUPPORTED_ESP_IDF_BUILD_INPUTS
    )
    requirements_path = root / "requirements" / "ci-host-windows.txt"
    dependency_lock = root / "dependencies.lock"
    host_dir = github_run_dir / "d1l-host-artifacts" / "build-inputs"
    idf_dir = github_run_dir / "d1l-idf55-migration-state"
    host_metadata = host_dir / "d1l-build-inputs.json"
    host_requirements = host_dir / "ci-host-windows.txt"
    host_installed = host_dir / "ci-host-windows-installed.json"
    host_scope = host_dir / "d1l-candidate-scope.json"
    host_checksums = host_dir / "SHA256SUMS.txt"
    idf_metadata = idf_dir / "build-inputs.json"
    idf_container = idf_dir / "container-image.txt"
    idf_version = idf_dir / "idf-version.txt"
    idf_dependency_lock = idf_dir / "dependencies.lock"
    idf_dependency_patch = idf_dir / "dependencies.lock.patch"

    metadata = read_json(metadata_path)
    scope = read_json(host_scope)
    try:
        installed = json.loads(host_installed.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        installed = None
    expected_raw = (
        metadata.get("host_python", {})
        .get("requirements", {})
        .get("packages")
    )
    expected_packages = (
        {
            re.sub(r"[-_.]+", "-", name).lower(): version
            for name, version in expected_raw.items()
        }
        if isinstance(expected_raw, dict)
        and expected_raw
        and all(
            isinstance(name, str)
            and name
            and isinstance(version, str)
            and version
            for name, version in expected_raw.items()
        )
        else None
    )
    host_entries = full_release_audit.checksum_manifest_entries(
        host_checksums
    )
    esp_idf = metadata.get("esp_idf")
    esp_idf = esp_idf if isinstance(esp_idf, dict) else {}
    container = esp_idf.get("container")
    container = container if isinstance(container, dict) else {}
    requirements = metadata.get("host_python", {}).get("requirements", {})
    requirements = (
        requirements if isinstance(requirements, dict) else {}
    )

    def digest(path: Path) -> str | None:
        try:
            return sha256_file(path) if path.is_file() else None
        except OSError:
            return None

    def text(path: Path) -> str | None:
        try:
            return path.read_text(encoding="utf-8").strip()
        except (OSError, UnicodeError):
            return None

    rp2040_paths = [
        github_run_dir / name
        for name in (
            "rp2040-sd-bridge-firmware",
            "rp2040-sd-smoke-firmware",
            "rp2040-seeed-official-sd-smoke-firmware",
        )
    ]
    checks = {
        "metadata_schema": metadata.get("schema")
        == full_release_audit.BUILD_INPUTS_SCHEMA
        and metadata.get("kind") == full_release_audit.BUILD_INPUTS_KIND,
        "esp_idf_lock_exact": esp_idf.get("version")
        == full_release_audit.SUPPORTED_ESP_IDF_VERSION
        and container.get("reference")
        == full_release_audit.SUPPORTED_ESP_IDF_IMAGE
        and container.get("index_digest")
        == full_release_audit.SUPPORTED_ESP_IDF_IMAGE_DIGEST,
        "requirements_lock_exact": requirements.get("path")
        == "requirements/ci-host-windows.txt"
        and digest(requirements_path) == requirements.get("sha256"),
        "host_metadata_copy_exact": digest(metadata_path) is not None
        and digest(host_metadata) == digest(metadata_path),
        "host_requirements_copy_exact": digest(requirements_path) is not None
        and digest(host_requirements) == digest(requirements_path),
        "host_installed_packages_exact": expected_packages is not None
        and _normalized_packages(installed) == expected_packages,
        "host_checksum_manifest_complete": verify_sha256_manifest(
            host_checksums
        )
        and set(host_entries)
        == {
            "d1l-build-inputs.json",
            "ci-host-windows.txt",
            "ci-host-windows-installed.json",
            "d1l-candidate-scope.json",
        },
        "candidate_scope_exact": scope
        == {
            "schema": 1,
            "kind": "d1l_candidate_scope",
            "source_commit": commit,
            "workflow_run_id": str(run_id),
            "workflow_run_attempt": str(
                scope.get("workflow_run_attempt")
            ),
            "repository": "n30nex/SIGUI",
            "workflow": "d1l-ci",
            "event": "workflow_dispatch",
            "include_sd_bridge": False,
            "scope_reason": "esp32_only",
            "release_profile": CORE_RELEASE_PROFILE,
            "sd_history_mode": "disabled",
        }
        and str(scope.get("workflow_run_attempt")).isdigit()
        and int(scope.get("workflow_run_attempt")) >= 1,
        "idf_metadata_copy_exact": digest(metadata_path) is not None
        and digest(idf_metadata) == digest(metadata_path),
        "idf_container_exact": text(idf_container)
        == full_release_audit.SUPPORTED_ESP_IDF_IMAGE,
        "idf_version_exact": text(idf_version)
        == f"ESP-IDF {full_release_audit.SUPPORTED_ESP_IDF_VERSION}",
        "idf_dependency_lock_exact": digest(dependency_lock) is not None
        and digest(idf_dependency_lock) == digest(dependency_lock),
        "idf_dependency_patch_empty": digest(idf_dependency_patch)
        == hashlib.sha256(b"").hexdigest(),
        "disabled_rp2040_artifacts_absent": not any(
            path.exists() for path in rp2040_paths
        ),
    }
    failures = [name for name, value in checks.items() if not value]
    evidence_paths = (
        metadata_path,
        requirements_path,
        dependency_lock,
        host_checksums,
        host_metadata,
        host_requirements,
        host_installed,
        host_scope,
        idf_metadata,
        idf_container,
        idf_version,
        idf_dependency_lock,
        idf_dependency_patch,
    )
    return CoreGate(
        "core_immutable_release_source_inputs",
        not failures,
        "Core host and IDF inputs are exact; disabled RP2040 scope is absent",
        [
            path_text(path, root)[0]
            for path in evidence_paths
            if path.is_file()
        ],
        {"checks": checks, "failures": failures},
    )


def actions_run_metadata_gate(
    path: Path | None,
    github_run_dir: Path,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt_expected: str,
) -> CoreGate:
    receipt = read_json(path)
    raw_path, raw_ok = _verified_file_row(
        receipt.get("raw_run_response"), root
    )
    artifacts_raw_path, artifacts_raw_ok = _verified_file_row(
        receipt.get("raw_artifacts_response"), root
    )
    raw = read_json(raw_path)
    artifacts_raw = read_json(artifacts_raw_path)
    scope_path = (
        github_run_dir
        / "d1l-host-artifacts"
        / "build-inputs"
        / "d1l-candidate-scope.json"
    )
    scope = read_json(scope_path)
    repository = raw.get("repository")
    repository = repository if isinstance(repository, dict) else {}
    try:
        captured_at = datetime.fromisoformat(
            str(receipt.get("captured_at")).replace("Z", "+00:00")
        ).astimezone(timezone.utc)
        age_sec = (
            datetime.now(timezone.utc) - captured_at
        ).total_seconds()
        capture_fresh = -300 <= age_sec <= 24 * 60 * 60
    except (TypeError, ValueError):
        age_sec = None
        capture_fresh = False
    run_attempt = raw.get("run_attempt")
    raw_contract_ok = False
    artifacts_by_name: dict[str, dict] = {}
    try:
        validate_run(raw, run_id=str(run_id), commit=commit)
        artifacts_by_name = validate_artifacts(
            artifacts_raw, run_id=str(run_id), commit=commit
        )
        raw_contract_ok = True
    except ValueError:
        pass

    receipt_rows = receipt.get("artifacts")
    receipt_rows = receipt_rows if isinstance(receipt_rows, list) else []
    rows_by_name: dict[str, dict] = {}
    duplicate_names: list[str] = []
    for row in receipt_rows:
        name = row.get("name") if isinstance(row, dict) else None
        if not isinstance(name, str) or name in rows_by_name:
            duplicate_names.append(str(name))
            continue
        rows_by_name[name] = row
    artifact_checks: dict[str, bool] = {}
    artifact_evidence: list[str] = []
    for name in REQUIRED_ACTIONS_ARTIFACTS:
        api_row = artifacts_by_name.get(name, {})
        receipt_row = rows_by_name.get(name, {})
        archive, archive_ok = _verified_file_row(
            receipt_row.get("archive") if isinstance(receipt_row, dict) else None,
            root,
        )
        expected_root = (github_run_dir / name).resolve()
        extracted_root_text = (
            receipt_row.get("extracted_root")
            if isinstance(receipt_row, dict)
            else None
        )
        extracted_root = (
            (root / extracted_root_text).resolve()
            if isinstance(extracted_root_text, str)
            and extracted_root_text
            and not Path(extracted_root_text).is_absolute()
            else None
        )
        try:
            archive_inventory = zip_inventory(archive) if archive_ok and archive else []
            extracted_inventory = (
                tree_inventory(extracted_root, root)
                if extracted_root == expected_root
                else {}
            )
        except (OSError, RuntimeError, ValueError):
            archive_inventory = []
            extracted_inventory = {}
        extracted_files = (
            extracted_inventory.get("files")
            if isinstance(extracted_inventory, dict)
            else None
        )
        expected_digest = (
            str(api_row.get("digest")).removeprefix("sha256:")
            if isinstance(api_row, dict)
            else ""
        )
        check = (
            isinstance(api_row, dict)
            and isinstance(receipt_row, dict)
            and receipt_row.get("artifact_id") == api_row.get("id")
            and receipt_row.get("api_digest") == api_row.get("digest")
            and receipt_row.get("api_size_in_bytes")
            == api_row.get("size_in_bytes")
            and archive_ok
            and archive is not None
            and archive.stat().st_size == api_row.get("size_in_bytes")
            and sha256_file(archive) == expected_digest
            and receipt_row.get("zip_members") == archive_inventory
            and extracted_root == expected_root
            and receipt_row.get("extracted_inventory") == extracted_inventory
            and archive_inventory == extracted_files
        )
        artifact_checks[name] = check
        artifact_evidence.extend(path_text(archive, root))
        if extracted_root is not None:
            artifact_evidence.extend(path_text(extracted_root, root))

    ok = (
        real_evidence(receipt)
        and receipt.get("kind") == "core_actions_run_metadata"
        and receipt.get("schema") == 2
        and receipt.get("mode") == "github-api-artifact-capture"
        and receipt.get("ok") is True
        and receipt.get("repository") == "n30nex/SIGUI"
        and exact_sha(receipt.get("expected_commit")) == commit
        and str(receipt.get("github_actions_run")) == str(run_id)
        and str(receipt.get("workflow_run_attempt"))
        == str(run_attempt_expected)
        and isinstance(receipt.get("git"), dict)
        and exact_sha(receipt["git"].get("commit")) == commit
        and receipt["git"].get("dirty") is False
        and receipt["git"].get("dirty_entries") == []
        and raw_ok
        and artifacts_raw_ok
        and raw_contract_ok
        and not duplicate_names
        and set(rows_by_name) == set(REQUIRED_ACTIONS_ARTIFACTS)
        and all(artifact_checks.values())
        and str(scope.get("workflow_run_attempt")) == str(run_attempt)
        and str(run_attempt) == str(run_attempt_expected)
        and scope.get("source_commit") == commit
        and str(scope.get("workflow_run_id")) == str(run_id)
        and scope.get("include_sd_bridge") is False
        and scope.get("release_profile") == CORE_RELEASE_PROFILE
        and scope.get("sd_history_mode") == "disabled"
        and capture_fresh
    )
    return CoreGate(
        "github_actions_run_completed_success",
        ok,
        "GitHub run metadata and checksum-bound Core scope match the exact candidate",
        path_text(path, root)
        + (path_text(raw_path, root) if raw_path else [])
        + (
            path_text(artifacts_raw_path, root)
            if artifacts_raw_path
            else []
        )
        + path_text(scope_path, root)
        + artifact_evidence,
        {
            "raw_response_ok": raw_ok,
            "raw_artifacts_response_ok": artifacts_raw_ok,
            "raw_contract_ok": raw_contract_ok,
            "capture_age_sec": age_sec,
            "status": raw.get("status"),
            "conclusion": raw.get("conclusion"),
            "head_sha": raw.get("head_sha"),
            "run_attempt": run_attempt,
            "include_sd_bridge": scope.get("include_sd_bridge"),
            "artifact_checks": artifact_checks,
            "duplicate_artifact_names": duplicate_names,
            "artifact_evidence": artifact_evidence,
        },
    )


def imported_gate(gate: Any) -> CoreGate:
    value = gate.to_dict()
    return CoreGate(
        value.get("id", "unknown"),
        value.get("ok") is True,
        value.get("title", "Imported strict gate"),
        list(value.get("evidence") or []),
        dict(value.get("details") or {}),
        value.get("severity", "P0"),
    )


def audit_runner_source_gate(root: Path, commit: str) -> CoreGate:
    source = git_metadata(root)
    ok = (
        exact_sha(source.get("commit")) == commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    )
    return CoreGate(
        "core_audit_runner_exact_source",
        ok,
        "Core audit logic runs from the exact clean candidate source",
        [],
        {"git": source},
    )


def package_gate(
    github_run_dir: Path,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
    sd_history_mode: str,
) -> CoreGate:
    package = find_release_package(github_run_dir)
    manifest_path = package / "manifest.json" if package else None
    manifest = read_json(manifest_path)
    truth = core_capability_truth(sd_history_mode)
    failures: list[str] = []
    provenance_validation_errors: list[str] = []

    expected = {
        "release_profile": CORE_RELEASE_PROFILE,
        "firmware_commit": commit,
        "actions_run": str(run_id),
        "actions_run_attempt": str(run_attempt),
        "supported_capabilities": truth["supported_capabilities"],
        "unavailable_capabilities": truth["unavailable_capabilities"],
        "sd_history_mode": sd_history_mode,
        "sd_history_state": truth["sd_history_state"],
        "storage_authority": truth["storage_authority"],
        "full_feature_release_ready": False,
    }
    for field_name, expected_value in expected.items():
        if manifest.get(field_name) != expected_value:
            failures.append(field_name)
    if exact_sha(manifest.get("git", {}).get("commit")) != commit:
        failures.append("git.commit")
    workflow = manifest.get("workflow")
    if not isinstance(workflow, dict):
        failures.append("workflow")
    else:
        if exact_sha(workflow.get("sha")) != commit:
            failures.append("workflow.sha")
        if str(workflow.get("run_id")) != str(run_id):
            failures.append("workflow.run_id")
        if str(workflow.get("run_attempt")) != str(run_attempt):
            failures.append("workflow.run_attempt")
        if workflow.get("repository") != "n30nex/SIGUI":
            failures.append("workflow.repository")

    install = manifest.get("install_recovery_guide")
    if not isinstance(install, dict) or not all(
        (
            install.get("usb_only") is True,
            install.get("normal_install_port") == D1L_CORE_PORT,
            install.get("normal_install_preserves_unrelated_nvs") is True,
            install.get("normal_install_package_root_only") is True,
            install.get("normal_install_checksum_verified") is True,
            install.get("recovery_requires_typed_confirmation") is True,
            install.get("recovery_checksum_verified") is True,
            install.get("no_on_device_sd_format") is True,
            install.get("install_guide") == "docs/CORE_INSTALL_RECOVERY.md",
            install.get("recovery_guide") == "docs/CORE_INSTALL_RECOVERY.md",
        )
    ):
        failures.append("install_recovery_guide")

    for metadata_name in ("supported_features", "sbom", "provenance"):
        metadata = manifest.get(metadata_name)
        if not isinstance(metadata, dict):
            failures.append(metadata_name)
            continue
        raw_target = metadata.get("path")
        target = None
        if (
            package
            and isinstance(raw_target, str)
            and raw_target
            and not Path(raw_target).is_absolute()
            and "\\" not in raw_target
            and all(
                part not in {"", ".", ".."}
                for part in Path(raw_target).parts
            )
        ):
            try:
                candidate = (package / raw_target).resolve(strict=True)
                candidate.relative_to(package.resolve(strict=True))
                target = candidate
            except (OSError, RuntimeError, ValueError):
                target = None
        if (
            target is None
            or not target.is_file()
            or is_link_or_reparse(target)
            or metadata.get("sha256") != sha256_file(target)
        ):
            failures.append(f"{metadata_name}.checksum")
        if metadata_name in {"sbom", "provenance"} and (
            metadata.get("valid") is not True
            or exact_sha(metadata.get("source_commit")) != commit
        ):
            failures.append(f"{metadata_name}.identity")
        if metadata_name == "provenance":
            expected_provenance_metadata = {
                "release_profile": CORE_RELEASE_PROFILE,
                "workflow_repository": "n30nex/SIGUI",
                "workflow_name": "d1l-ci",
                "workflow_path": ".github/workflows/d1l-ci.yml",
                "workflow_run_id": str(run_id),
                "workflow_run_attempt": str(run_attempt),
            }
            if any(
                metadata.get(field_name) != expected_value
                for field_name, expected_value in expected_provenance_metadata.items()
            ):
                failures.append("provenance.metadata_binding")
            provenance_statement = read_json(target)
            if target is None:
                provenance_validation_errors.append(
                    "provenance statement is unavailable"
                )
            else:
                try:
                    provenance_validation_errors.extend(
                        validate_core_actions_binding(
                            provenance_statement,
                            commit,
                            str(run_id),
                            str(run_attempt),
                        )
                    )
                    source_git = git_metadata(root)
                    if not (
                        exact_sha(source_git.get("commit")) == commit
                        and source_git.get("dirty") is False
                        and source_git.get("dirty_entries") == []
                    ):
                        provenance_validation_errors.append(
                            "provenance verifier source is not the exact clean candidate"
                        )
                    else:
                        source_identity = discover_source_identity(root, commit)
                        provenance_validation_errors.extend(
                            validate_provenance_against_inputs(
                                provenance_statement,
                                root,
                                package,
                                manifest,
                                source_identity,
                            )
                        )
                except (FileNotFoundError, OSError, RuntimeError, ValueError) as exc:
                    provenance_validation_errors.append(
                        "cannot recompute deterministic provenance: "
                        f"{type(exc).__name__}: {exc}"
                    )
            if provenance_validation_errors:
                failures.append("provenance.semantic_binding")

    if package:
        for guide in (
            "docs/CORE_INSTALL_RECOVERY.md",
            "README_RELEASE.md",
            "SUPPORTED_FEATURES.md",
            "SHA256SUMS.txt",
        ):
            if not (package / guide).is_file():
                failures.append(guide)
        for excluded_guide in (
            "docs/USER_GUIDE_D1L.md",
            "docs/DEVELOPER_GUIDE_D1L.md",
            "docs/FLASH_RECOVERY_D1L.md",
            "docs/RP2040_SD_BRIDGE_FLASH_D1L.md",
        ):
            if (package / excluded_guide).exists():
                failures.append(f"excluded_full_guide:{excluded_guide}")
    if sd_history_mode == "disabled":
        if manifest.get("rp2040_artifacts") != []:
            failures.append("rp2040_artifacts")
        if package and (package / "rp2040").exists():
            failures.append("rp2040_directory")
        if manifest.get("update_image") is not None:
            failures.append("unsupported_update_image")
        if package and (package / "update").exists():
            failures.append("unsupported_update_directory")
    elif not manifest.get("rp2040_artifacts"):
        failures.append("paired_rp2040_artifacts")

    return CoreGate(
        "core_profile_bound_package",
        package is not None and not failures,
        "Core package is bound to exact profile, commit, run, capabilities, and SD decision",
        path_text(manifest_path, root),
        {
            "failures": failures,
            "package": str(package) if package else None,
            "provenance_validation_errors": provenance_validation_errors,
        },
    )


def _verified_file_row(
    row: object, root: Path
) -> tuple[Path | None, bool]:
    if not isinstance(row, dict):
        return None, False
    raw_path = row.get("path")
    if not isinstance(raw_path, str) or not raw_path:
        return None, False
    path = Path(raw_path)
    if not path.is_absolute():
        path = root / path
    try:
        resolved = path.resolve(strict=True)
        resolved.relative_to(root.resolve())
        size = resolved.stat().st_size
        digest = sha256_file(resolved)
    except (OSError, RuntimeError, ValueError):
        return None, False
    ok = (
        resolved.is_file()
        and not is_link_or_reparse(resolved)
        and isinstance(row.get("size"), int)
        and not isinstance(row.get("size"), bool)
        and row.get("size") == size
        and size > 0
        and isinstance(row.get("sha256"), str)
        and row.get("sha256") == digest
    )
    return resolved, ok


def _retained_snapshot(
    row: object,
    root: Path,
    commit: str,
    phase: str,
) -> tuple[Path | None, dict | None, bool]:
    path, file_ok = _verified_file_row(row, root)
    data = read_json(path)
    results = data.get("results")
    projection = retained_state_projection(results)
    version = (
        results[0]
        if isinstance(results, list) and len(results) > 0
        else {}
    )
    health = (
        results[1]
        if isinstance(results, list) and len(results) > 1
        else {}
    )
    ok = (
        file_ok
        and real_evidence(data)
        and data.get("kind") == "core_retained_state_snapshot"
        and data.get("mode") == "hardware"
        and data.get("phase") == phase
        and data.get("port") == D1L_CORE_PORT
        and exact_sha(data.get("expected_firmware_commit")) == commit
        and projection is not None
        and data.get("projection") == projection
        and data.get("projection_sha256")
        == projection_sha256(projection)
        and _profile_version_result(version, commit, "disabled")
        and _profile_ready_health_result(health, commit, "disabled")
    )
    return path, projection, ok


def core_flash_receipt_gate(
    hardware_dir: Path,
    github_run_dir: Path,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
) -> CoreGate:
    legacy = esp32_flash_receipt_gate(
        hardware_dir,
        github_run_dir,
        root,
        commit,
        run_id,
        D1L_CORE_PORT,
    )
    receipt = newest_commit_json(
        hardware_dir, commit, "esp32_flash_*.json"
    )
    data = read_json(receipt)
    package = data.get("package_verification")
    package = package if isinstance(package, dict) else {}
    capture = data.get("actions_capture_verification")
    capture = capture if isinstance(capture, dict) else {}
    capture_receipt_path, capture_receipt_row_ok = _verified_file_row(
        capture.get("receipt"), root
    )
    try:
        if capture_receipt_path is None:
            raise ValueError("Actions capture receipt is missing")
        capture_recomputed = validate_capture_receipt(
            receipt_path=capture_receipt_path,
            root=root,
            github_run_dir=github_run_dir,
            commit=commit,
            run_id=str(run_id),
            run_attempt=str(run_attempt),
        )
        capture_ok = (
            capture_receipt_row_ok
            and capture_recomputed == capture
        )
    except (OSError, RuntimeError, ValueError):
        capture_recomputed = {}
        capture_ok = False
    raw_log, raw_log_ok = _verified_file_row(
        data.get("raw_flash_log"), root
    )
    before_path, before_projection, before_ok = _retained_snapshot(
        data.get("retained_state_before"),
        root,
        commit,
        "pre_flash",
    )
    after_path, after_projection, after_ok = _retained_snapshot(
        data.get("retained_state_after"),
        root,
        commit,
        "post_flash",
    )
    retained_ok = (
        before_ok
        and after_ok
        and before_projection is not None
        and after_projection is not None
        and retained_state_preserved(
            before_projection, after_projection
        )
    )
    version = data.get("post_flash_version")
    health = data.get("post_flash_health")
    scope_ok = (
        real_evidence(data)
        and data.get("scope") == "core-retained-reflash-only"
        and data.get("flash_phase") == FLASH_PHASE_RETAINED_REFLASH
        and data.get("ok") is True
        and data.get("closure_eligible") is True
        and data.get("physical_observed") is True
        and exact_candidate_fields(data, commit)
        and str(data.get("github_actions_run")) == str(run_id)
        and str(data.get("workflow_run_attempt")) == str(run_attempt)
        and data.get("release_profile") == CORE_RELEASE_PROFILE
        and data.get("sd_history_mode") == "disabled"
        and data.get("runner_source_identity_ok") is True
        and _profile_version_result(version, commit, "disabled")
        and _profile_identity_result(health, commit, "disabled")
        and isinstance(health, dict)
        and health.get("board_ready") is True
        and health.get("ui_ready") is True
        and package.get("ok") is True
        and package.get("checksum_tree_verified") is True
        and exact_sha(package.get("firmware_commit")) == commit
        and str(package.get("github_actions_run")) == str(run_id)
        and package.get("release_profile") == CORE_RELEASE_PROFILE
        and package.get("sd_history_mode") == "disabled"
        and package.get("storage_authority") == "nvs"
        and package.get("repository") == "n30nex/SIGUI"
        and str(package.get("workflow_run_attempt"))
        == str(run_attempt)
        and package.get("flash_files_match_actions") is True
        and capture_ok
        and data.get("commands_before_flash")
        == list(RETAINED_STATE_COMMANDS)
        and data.get("commands_after_flash")
        == list(RETAINED_STATE_COMMANDS)
        and data.get("retained_state_preserved") is True
        and retained_ok
        and data.get("erase_flash") is False
        and data.get("public_rf_tx") is False
        and data.get("dm_rf_tx") is False
        and data.get("sd_access") is False
        and data.get("rp2040_access") is False
        and data.get("formats_sd") is False
        and data.get("legacy_suite_ran") is False
        and raw_log_ok
    )
    legacy_ok = legacy.ok is True
    return CoreGate(
        "exact_actions_core_flash",
        legacy_ok and scope_ok,
        "Exact checksummed Actions Core package is flashed non-erasing to COM12",
        (
            path_text(receipt, root)
            + (path_text(raw_log, root) if raw_log else [])
            + (path_text(before_path, root) if before_path else [])
            + (path_text(after_path, root) if after_path else [])
            + (
                path_text(capture_receipt_path, root)
                if capture_receipt_path
                else []
            )
        ),
        {
            "legacy_receipt_contract_ok": legacy_ok,
            "core_flash_only_scope_ok": scope_ok,
            "raw_flash_log_ok": raw_log_ok,
            "retained_before_ok": before_ok,
            "retained_after_ok": after_ok,
            "non_erasing_retained_state_ok": retained_ok,
            "actions_archive_binding_ok": capture_ok,
            "legacy_details": legacy.to_dict().get("details", {}),
        },
    )


def _settings_result_snapshot(result: object) -> tuple[str, int] | None:
    if not isinstance(result, dict) or result.get("ok") is not True:
        return None
    name = result.get("node_name")
    path_hash_bytes = result.get("path_hash_bytes")
    if (
        not isinstance(name, str)
        or not name
        or isinstance(path_hash_bytes, bool)
        or path_hash_bytes not in (1, 2, 3)
    ):
        return None
    return name, path_hash_bytes


def core_persistence_raw_ok(
    persistence: object, commit: str, sd_history_mode: str
) -> bool:
    if not isinstance(persistence, dict):
        return False
    steps = persistence.get("steps")
    if not isinstance(steps, list) or len(steps) != 13:
        return False
    if not all(
        isinstance(step, dict)
        and isinstance(step.get("command"), str)
        and isinstance(step.get("result"), dict)
        for step in steps
    ):
        return False
    original = _settings_result_snapshot(steps[0]["result"])
    changed_observed = _settings_result_snapshot(steps[2]["result"])
    persisted = _settings_result_snapshot(steps[6]["result"])
    restored_pre = _settings_result_snapshot(steps[8]["result"])
    restored_post = _settings_result_snapshot(steps[12]["result"])
    if (
        original is None
        or changed_observed is None
        or changed_observed == original
        or changed_observed[1] != original[1]
    ):
        return False
    changed_name = changed_observed[0]
    original_name = original[0]
    expected_commands = [
        "settings get",
        f"settings set name {changed_name}",
        "settings get",
        "health",
        "reboot",
        "health",
        "settings get",
        f"settings set name {original_name}",
        "settings get",
        "health",
        "reboot",
        "health",
        "settings get",
    ]
    if [step["command"] for step in steps] != expected_commands:
        return False
    before_one = steps[3]["result"]
    after_one = steps[5]["result"]
    before_two = steps[9]["result"]
    after_two = steps[11]["result"]

    def software_transition(before: dict, after: dict) -> bool:
        before_nonce = before.get("boot_nonce")
        after_nonce = after.get("boot_nonce")
        return (
            _profile_identity_result(before, commit, sd_history_mode)
            and _profile_identity_result(after, commit, sd_history_mode)
            and isinstance(before_nonce, int)
            and not isinstance(before_nonce, bool)
            and isinstance(after_nonce, int)
            and not isinstance(after_nonce, bool)
            and before_nonce != 0
            and after_nonce != 0
            and before_nonce != after_nonce
            and after.get("reset_reason") == "SW"
        )

    return (
        persistence.get("schema") == 1
        and persistence.get("kind") == "core_settings_persistence"
        and persistence.get("ok") is True
        and persistence.get("mutation_started") is True
        and persistence.get("reboot_count") == 2
        and persistence.get("first_reboot_proven") is True
        and persistence.get("persisted_after_reboot") is True
        and persistence.get("original_restored") is True
        and steps[1]["result"].get("ok") is True
        and steps[1]["result"].get("cmd") == "settings set name"
        and steps[7]["result"].get("ok") is True
        and steps[7]["result"].get("cmd") == "settings set name"
        and reboot_command_passed(steps[4]["result"])
        and reboot_command_passed(steps[10]["result"])
        and software_transition(before_one, after_one)
        and software_transition(before_two, after_two)
        and persisted == changed_observed
        and restored_pre == original
        and restored_post == original
    )


def core_smoke_gate(
    path: Path | None,
    root: Path,
    commit: str,
    sd_history_mode: str,
    run_id: str,
    run_attempt: str,
) -> CoreGate:
    data = read_json(path)
    probes = data.get("unavailable_mutation_probes")
    expected_plan = mutation_probe_plan(sd_history_mode)
    probes_ok = (
        isinstance(probes, list)
        and len(probes) == len(expected_plan)
        and all(
            isinstance(observed, dict)
            and observed.get("command") == expected["command"]
            and observed.get("feature") == expected["feature"]
            and exact_unsupported_result(
                observed.get("result"),
                expected["command"],
                expected["feature"],
            )
            for observed, expected in zip(probes, expected_plan)
        )
    )
    status_probes = data.get("unavailable_status_probes")
    expected_status_plan = unavailable_status_probe_plan(sd_history_mode)
    status_probes_ok = (
        isinstance(status_probes, list)
        and len(status_probes) == len(expected_status_plan)
        and all(
            isinstance(observed, dict)
            and observed.get("command") == expected["command"]
            and observed.get("feature") == expected["feature"]
            and exact_unavailable_status_result(
                observed.get("result"),
                expected["command"],
                expected["feature"],
                commit,
                sd_history_mode,
            )
            for observed, expected in zip(
                status_probes, expected_status_plan
            )
        )
    )
    commands = data.get("supported_commands_executed")
    commands_exact = commands == list(CORE_SMOKE_COMMANDS)
    no_public_send = isinstance(commands, list) and not any(
        isinstance(command, str)
        and command.strip().lower().startswith("mesh send public ")
        for command in commands
    )
    persistence = data.get("persistence")
    results = data.get("results")
    results = results if isinstance(results, list) else []
    expected_result_commands = [
        "version",
        "health",
        *(expected_command_name(command) for command in CORE_SMOKE_COMMANDS),
        "health",
    ]
    result_commands = [
        result.get("cmd") if isinstance(result, dict) else None
        for result in results
    ]
    raw_results_ok = (
        result_commands == expected_result_commands
        and all(
            isinstance(result, dict)
            and result.get("schema") == 1
            and result.get("ok") is True
            for result in results
        )
        and _profile_version_result(results[0], commit, sd_history_mode)
        and _profile_identity_result(results[1], commit, sd_history_mode)
        and _profile_identity_result(results[-1], commit, sd_history_mode)
    ) if results else False
    storage_rows = [
        result
        for result in results
        if isinstance(result, dict) and result.get("cmd") == "storage status"
    ]
    crashlog_rows = [
        result
        for result in results
        if isinstance(result, dict) and result.get("cmd") == "crashlog"
    ]
    version_rows = [
        result
        for result in results
        if isinstance(result, dict) and result.get("cmd") == "version"
    ]
    idf_ok = (
        len(version_rows) == 1
        and version_rows[0].get("idf") == EXPECTED_IDF_VERSION
    )
    disabled_storage_ok = (
        sd_history_mode != "disabled"
        or (
            len(storage_rows) == 1
            and disabled_storage_status_ok(storage_rows[0])
        )
    )
    crashlog_ok = (
        len(crashlog_rows) == 1 and crashlog_clean(crashlog_rows[0])
    )
    persistence_raw_ok = core_persistence_raw_ok(
        persistence, commit, sd_history_mode
    )
    required_check_names = {
        "exact_candidate",
        "esp_idf_v5_5_4",
        "core_profile",
        "exact_sd_history_mode",
        "supported_commands_pass",
        "disabled_sd_nvs_authoritative",
        "pre_ui_crashlog_clean",
        "unavailable_mutations_rejected",
        "disabled_sd_status_probes_truthful",
        "health_ready",
        "persistence_pass",
        "no_public_rf",
        "no_sd_format",
    }
    checks = data.get("checks")
    checks_exact = (
        isinstance(checks, dict)
        and set(checks) == required_check_names
        and all(value is True for value in checks.values())
    )
    ok = (
        real_evidence(data)
        and data.get("kind") == "core_smoke"
        and data.get("mode") == "hardware"
        and data.get("ok") is True
        and data.get("closure_eligible") is True
        and data.get("physical_observed") is True
        and data.get("port") == D1L_CORE_PORT
        and data.get("release_profile") == CORE_RELEASE_PROFILE
        and data.get("sd_history_mode") == sd_history_mode
        and exact_candidate_fields(data, commit)
        and str(data.get("github_actions_run")) == str(run_id)
        and str(data.get("workflow_run_attempt")) == str(run_attempt)
        and checks_exact
        and probes_ok
        and status_probes_ok
        and commands_exact
        and no_public_send
        and raw_results_ok
        and disabled_storage_ok
        and crashlog_ok
        and idf_ok
        and persistence_raw_ok
        and data.get("public_rf_tx") is False
        and data.get("formats_sd") is False
    )
    return CoreGate(
        "exact_candidate_core_smoke",
        ok,
        "Exact-candidate Core smoke, unavailable mutation, and persistence checks pass",
        path_text(path, root),
        {
            "artifact_ok": data.get("ok"),
            "probe_plan_exact": probes_ok,
            "status_probe_plan_exact": status_probes_ok,
            "supported_commands_exact": commands_exact,
            "raw_supported_results_ok": raw_results_ok,
            "disabled_sd_nvs_authoritative": disabled_storage_ok,
            "pre_ui_crashlog_clean": crashlog_ok,
            "device_idf_exact": idf_ok,
            "persistence_ok": (
                persistence_raw_ok
            ),
        },
    )


def core_ui_gate(
    path: Path | None,
    root: Path,
    commit: str,
    sd_history_mode: str,
    run_id: str,
    run_attempt: str,
) -> CoreGate:
    data = read_json(path)
    checks = data.get("checks")
    scroll_events = data.get("scroll_events")
    compose_events = data.get("compose_events")
    scroll_ok = (
        data.get("scroll_surfaces") == list(CORE_SCROLL_SURFACES)
        and isinstance(scroll_events, list)
        and len(scroll_events) == len(CORE_SCROLL_SURFACES)
        and all(
            isinstance(event, dict)
            and event.get("surface") == surface
            and event.get("command") == f"ui scroll-probe {surface}"
            and core_scroll_result_ok(event.get("result"), surface)
            for event, surface in zip(scroll_events, CORE_SCROLL_SURFACES)
        )
    )
    compose_ok = (
        data.get("compose_targets") == list(CORE_COMPOSE_TARGETS)
        and isinstance(compose_events, list)
        and len(compose_events) == len(CORE_COMPOSE_TARGETS)
        and all(
            isinstance(event, dict)
            and event.get("target") == target
            and event.get("command") == f"ui compose-probe {target}"
            and core_compose_result_ok(event.get("result"), target)
            for event, target in zip(compose_events, CORE_COMPOSE_TARGETS)
        )
    )
    unavailable_events = data.get("unavailable_events")
    unavailable_ok = (
        data.get("unavailable_ui_probes")
        == unavailable_ui_probe_plan(sd_history_mode)
        and unavailable_ui_events_ok(
            unavailable_events,
            commit,
            sd_history_mode,
        )
    )
    events = data.get("events")
    events = events if isinstance(events, list) else []
    raw_sequence_ok = len(events) == data.get("rounds", 0) * (
        len(CORE_TAB_SEQUENCE) + 1
    )
    event_index = 0
    if raw_sequence_ok:
        for round_number in range(1, data["rounds"] + 1):
            for tab in CORE_TAB_SEQUENCE:
                event = events[event_index]
                event_index += 1
                raw_sequence_ok = raw_sequence_ok and (
                    isinstance(event, dict)
                    and event.get("round") == round_number
                    and event.get("kind") == "tab"
                    and event.get("tab") == tab
                    and event.get("request", {}).get("ok") is True
                    and event.get("active") is True
                    and _profile_ready_health_result(
                        event.get("health"), commit, sd_history_mode
                    )
                    and crashlog_clean(event.get("crashlog"))
                )
            refresh = events[event_index]
            event_index += 1
            raw_sequence_ok = raw_sequence_ok and (
                isinstance(refresh, dict)
                and refresh.get("round") == round_number
                and refresh.get("kind") == "data_refresh"
                and _profile_ready_health_result(
                    refresh.get("health"), commit, sd_history_mode
                )
                and crashlog_clean(refresh.get("crashlog"))
            )
    raw_events_pass = (
        raw_sequence_ok
        and ui_event_failures(events, skip_data_canary=False) == []
    )
    required_ui_check_names = {
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
    checks_exact = (
        isinstance(checks, dict)
        and set(checks) == required_ui_check_names
        and all(value is True for value in checks.values())
    )
    setup_events = data.get("setup_events")
    setup_events = (
        setup_events if isinstance(setup_events, list) else []
    )
    expected_setup_commands = [
        "version",
        "health",
        "ui status",
        "crashlog",
    ] + (
        ["crashlog clear"]
        if data.get("clear_crashlog_before_start") is True
        else []
    )
    setup_results = [
        event.get("result", {})
        for event in setup_events
        if isinstance(event, dict)
    ]
    setup_ok = (
        len(setup_events) == len(expected_setup_commands)
        and [
            event.get("command")
            for event in setup_events
            if isinstance(event, dict)
        ]
        == expected_setup_commands
        and len(setup_results) == len(expected_setup_commands)
        and _profile_version_result(
            setup_results[0], commit, sd_history_mode
        )
        and _profile_ready_health_result(
            setup_results[1], commit, sd_history_mode
        )
        and setup_results[2].get("ok") is True
        and setup_results[2].get("cmd") == "ui status"
        and crashlog_clean(setup_results[3])
        and (
            len(setup_results) == 4
            or (
                setup_results[4].get("ok") is True
                and setup_results[4].get("cmd") == "crashlog clear"
            )
        )
    )
    ok = (
        real_evidence(data)
        and data.get("kind") == "core_ui_corruption_probe"
        and data.get("mode") == "hardware"
        and data.get("ok") is True
        and data.get("closure_eligible") is True
        and data.get("physical_observed") is True
        and data.get("port") == D1L_CORE_PORT
        and data.get("release_profile") == CORE_RELEASE_PROFILE
        and data.get("sd_history_mode") == sd_history_mode
        and exact_candidate_fields(data, commit)
        and str(data.get("github_actions_run")) == str(run_id)
        and str(data.get("workflow_run_attempt")) == str(run_attempt)
        and isinstance(data.get("rounds"), int)
        and data["rounds"] >= RELEASE_MIN_ROUNDS
        and core_tab_sequence_ok(data.get("tabs"))
        and scroll_ok
        and compose_ok
        and unavailable_ok
        and data.get("skip_data_canary") is False
        and data.get("data_refresh_events") == data.get("rounds")
        and raw_events_pass
        and setup_ok
        and checks_exact
        and _profile_ready_health_result(
            data.get("final_health"), commit, sd_history_mode
        )
        and data.get("public_rf_tx") is False
        and data.get("network_tx") is False
        and data.get("map_network_requests") is False
        and data.get("formats_sd") is False
    )
    return CoreGate(
        "core_ui_20_rounds",
        ok,
        "Core Home/Messages/Nodes/Packets/Settings UI sequence passes 20 rounds",
        path_text(path, root),
        {
            "rounds": data.get("rounds"),
            "tabs": data.get("tabs"),
            "expected_tabs": list(CORE_TAB_SEQUENCE),
            "scroll_surfaces_exact": scroll_ok,
            "compose_targets_exact": compose_ok,
            "unavailable_destinations_rejected": unavailable_ok,
            "raw_round_sequence_exact": raw_sequence_ok,
            "raw_event_checks_pass": raw_events_pass,
            "pre_ui_setup_exact": setup_ok,
        },
    )


def manual_review_gate(
    path: Path | None,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
    core_ui_path: Path | None,
) -> CoreGate:
    strict_ok = False
    strict_reasons: list[str] = []
    strict_details: dict[str, Any] = {}
    if path is None or core_ui_path is None:
        strict_reasons = ["manual_or_automated_ui_receipt_missing"]
    else:
        try:
            strict_ok, strict_reasons, strict_details = (
                validate_core_manual_ui_review_receipt(
                    path,
                    root=root,
                    core_ui_path=core_ui_path,
                    commit=commit,
                    run_id=str(run_id),
                    run_attempt=str(run_attempt),
                )
            )
        except (OSError, RuntimeError, TypeError, ValueError) as exc:
            strict_reasons = [
                "strict_manual_ui_validator_failed:"
                f"{type(exc).__name__}"
            ]
    data = read_json(path)
    confirmations = data.get("confirmations")
    photo_receipts = data.get("photo_receipts")
    observed_photo_coverage: set[str] = set()
    observed_photo_hashes: set[str] = set()
    photos_ok = photo_receipts in (None, [])
    if isinstance(photo_receipts, list) and photo_receipts:
        photos_ok = True
        for row in photo_receipts:
            if not isinstance(row, dict):
                photos_ok = False
                break
            candidate = Path(str(row.get("path") or ""))
            if not candidate.is_absolute():
                candidate = root / candidate
            try:
                candidate = candidate.resolve(strict=True)
                candidate.relative_to(root.resolve())
                raw = candidate.read_bytes()
            except (OSError, RuntimeError, ValueError):
                photos_ok = False
                break
            png_ok = (
                candidate.suffix.lower() == ".png"
                and len(raw) > 100
                and raw.startswith(b"\x89PNG\r\n\x1a\n")
                and len(raw) >= 24
                and int.from_bytes(raw[16:20], "big") > 0
                and int.from_bytes(raw[20:24], "big") > 0
            )
            coverage = row.get("coverage")
            digest = row.get("sha256")
            if (
                not candidate.is_file()
                or is_link_or_reparse(candidate)
                or row.get("size") != candidate.stat().st_size
                or digest != sha256_file(candidate)
                or not png_ok
                or (
                    coverage is not None
                    and (
                        not isinstance(coverage, list)
                        or not coverage
                        or not all(
                            isinstance(label, str) and bool(label.strip())
                            for label in coverage
                        )
                    )
                )
                or digest in observed_photo_hashes
            ):
                photos_ok = False
                break
            observed_photo_hashes.add(digest)
            observed_photo_coverage.update(coverage or [])
    ui_receipt_path, ui_receipt_row_ok = _verified_file_row(
        data.get("automated_ui_receipt"), root
    )
    expected_ui_path = (
        core_ui_path.resolve()
        if core_ui_path is not None
        else None
    )
    ui_binding_ok = (
        ui_receipt_row_ok
        and ui_receipt_path is not None
        and expected_ui_path is not None
        and ui_receipt_path == expected_ui_path
        and core_ui_gate(
            ui_receipt_path,
            root,
            commit,
            "disabled",
            run_id,
            run_attempt,
        ).ok
    )
    confirmations_ok = (
        isinstance(confirmations, dict)
        and set(confirmations) == set(REQUIRED_MANUAL_CONFIRMATIONS)
        and all(
            confirmations.get(name) is True
            for name in REQUIRED_MANUAL_CONFIRMATIONS
        )
    )
    try:
        started = datetime.fromisoformat(
            str(data.get("started_at")).replace("Z", "+00:00")
        )
        ended = datetime.fromisoformat(
            str(data.get("ended_at")).replace("Z", "+00:00")
        )
        timestamps_ok = (
            started.tzinfo is not None
            and ended.tzinfo is not None
            and ended >= started
        )
    except (TypeError, ValueError):
        timestamps_ok = False
    ok = (
        strict_ok
        and real_evidence(data)
        and data.get("kind") == "core_manual_ui_review"
        and data.get("mode") == "manual-physical-ui-review"
        and data.get("ok") is True
        and data.get("closure_eligible") is True
        and data.get("physical_observed") is True
        and data.get("port") == D1L_CORE_PORT
        and data.get("release_profile") == CORE_RELEASE_PROFILE
        and data.get("sd_history_mode") == "disabled"
        and exact_sha(data.get("commit")) == commit
        and str(data.get("github_actions_run")) == str(run_id)
        and str(data.get("workflow_run_attempt")) == str(run_attempt)
        and isinstance(data.get("operator"), str)
        and bool(data["operator"].strip())
        and isinstance(data.get("reviewer"), str)
        and bool(data["reviewer"].strip())
        and data["operator"].strip() != data["reviewer"].strip()
        and confirmations_ok
        and ui_binding_ok
        and photos_ok
        and timestamps_ok
        and data.get("public_rf_tx") is False
        and data.get("dm_rf_tx") is False
        and data.get("formats_sd") is False
    )
    return CoreGate(
        "manual_display_touch_core_review",
        ok,
        "Manual display, touch, backlight, Core UI, and unavailable-surface review passes",
        path_text(path, root),
        {
            "confirmations_ok": confirmations_ok,
            "automated_ui_receipt_ok": ui_binding_ok,
            "photo_receipts_ok": photos_ok,
            "photos_optional": True,
            "photo_coverage": sorted(observed_photo_coverage),
            "timestamps_ok": timestamps_ok,
            "strict_validator_ok": strict_ok,
            "strict_validator_reasons": strict_reasons,
            "strict_validator_details": strict_details,
        },
    )


def _profile_identity_result(
    result: object, commit: str, sd_history_mode: str
) -> bool:
    return (
        isinstance(result, dict)
        and result.get("ok") is True
        and exact_sha(result.get("build_commit")) == commit
        and result.get("release_profile") == CORE_RELEASE_PROFILE
        and result.get("sd_history_mode") == sd_history_mode
    )


def _profile_ready_health_result(
    result: object, commit: str, sd_history_mode: str
) -> bool:
    return (
        _profile_identity_result(result, commit, sd_history_mode)
        and isinstance(result, dict)
        and result.get("cmd") == "health"
        and result.get("board_ready") is True
        and result.get("ui_ready") is True
    )


def _profile_version_result(
    result: object, commit: str, sd_history_mode: str
) -> bool:
    return (
        _profile_identity_result(result, commit, sd_history_mode)
        and isinstance(result, dict)
        and result.get("cmd") == "version"
        and result.get("idf") == EXPECTED_IDF_VERSION
    )


def _software_reboot_receipt_ok(
    receipt: dict, commit: str, sd_history_mode: str
) -> tuple[bool, int | None, int | None]:
    pre = receipt.get("pre")
    post = receipt.get("post")
    checks = receipt.get("checks")
    transition = receipt.get("transition")
    pre = pre if isinstance(pre, dict) else {}
    post = post if isinstance(post, dict) else {}
    checks = checks if isinstance(checks, dict) else {}
    transition = transition if isinstance(transition, dict) else {}
    before_health = pre.get("health")
    after_health = post.get("health")
    before_nonce = (
        before_health.get("boot_nonce")
        if isinstance(before_health, dict)
        else None
    )
    after_nonce = (
        after_health.get("boot_nonce")
        if isinstance(after_health, dict)
        else None
    )
    required_true = (
        "pre_complete",
        "reboot_attempted",
        "reboot_ack_ok",
        "boot_help_seen",
        "post_complete",
        "boot_nonce_changed",
        "post_reset_reason_sw",
        "firmware_identity_stable",
        "firmware_identity_expected",
        "raw_reset_reason_sw",
        "reset_scope_system",
        "raw_transition_complete",
    )
    required_false = (
        "multiple_boot_banners",
        "raw_crash_marker_seen",
        "raw_wdt_reset_seen",
    )
    raw_lines = transition.get("raw_lines")
    ok = (
        real_evidence(receipt)
        and receipt.get("schema") == 1
        and receipt.get("kind") == "d1l_reboot_transition_trace"
        and receipt.get("mode") == "hardware"
        and receipt.get("ok") is True
        and receipt.get("classification") == "single_sw_transition"
        and receipt.get("port") == D1L_CORE_PORT
        and exact_sha(receipt.get("expected_firmware_commit")) == commit
        and receipt.get("hardware_required") is True
        and receipt.get("serial_handle_reused_across_reboot") is True
        and receipt.get("public_rf_tx") is False
        and receipt.get("formats_sd") is False
        and _profile_version_result(
            pre.get("version"), commit, sd_history_mode
        )
        and _profile_identity_result(
            pre.get("health"), commit, sd_history_mode
        )
        and _profile_version_result(
            post.get("version"), commit, sd_history_mode
        )
        and _profile_identity_result(
            post.get("health"), commit, sd_history_mode
        )
        and isinstance(before_nonce, int)
        and not isinstance(before_nonce, bool)
        and isinstance(after_nonce, int)
        and not isinstance(after_nonce, bool)
        and before_nonce != 0
        and after_nonce != 0
        and before_nonce != after_nonce
        and all(checks.get(name) is True for name in required_true)
        and all(checks.get(name) is False for name in required_false)
        and isinstance(raw_lines, list)
        and bool(raw_lines)
        and transition.get("raw_line_count") == len(raw_lines)
        and transition.get("raw_lines_dropped") == 0
        and transition.get("raw_chars_dropped") == 0
        and transition.get("reset_events_dropped") == 0
    )
    return ok, before_nonce, after_nonce


def _cold_boot_receipt_ok(
    receipt: dict, commit: str, sd_history_mode: str
) -> tuple[bool, int | None, int | None]:
    before = receipt.get("before")
    after = receipt.get("after")
    before = before if isinstance(before, dict) else {}
    after = after if isinstance(after, dict) else {}
    before_health = before.get("health")
    after_health = after.get("health")
    before_nonce = (
        before_health.get("boot_nonce")
        if isinstance(before_health, dict)
        else None
    )
    after_nonce = (
        after_health.get("boot_nonce")
        if isinstance(after_health, dict)
        else None
    )
    raw_lines = receipt.get("raw_serial_lines")
    raw_boot_marker = isinstance(raw_lines, list) and any(
        isinstance(line, str)
        and any(marker in line for marker in ("ESP-ROM", "rst:", "boot:"))
        for line in raw_lines
    )
    before_state = receipt.get("retained_state_before_sha256")
    after_state = receipt.get("retained_state_after_sha256")
    crashlog = after.get("crashlog")
    crash_entries = (
        crashlog.get("entries") if isinstance(crashlog, dict) else None
    )
    crashlog_ok = (
        isinstance(crashlog, dict)
        and crashlog.get("ok") is True
        and isinstance(crash_entries, list)
        and not any(
            isinstance(entry, dict) and entry.get("crash_like") is True
            for entry in crash_entries
        )
    )
    ok = (
        real_evidence(receipt)
        and receipt.get("schema") == 1
        and receipt.get("kind") == "core_cold_boot_serial_capture"
        and receipt.get("mode") == "hardware"
        and receipt.get("ok") is True
        and receipt.get("physical_observed") is True
        and receipt.get("port") == D1L_CORE_PORT
        and exact_sha(receipt.get("expected_firmware_commit")) == commit
        and receipt.get("operator_power_removed") is True
        and receipt.get("serial_disconnect_observed") is True
        and isinstance(receipt.get("power_off_duration_sec"), (int, float))
        and not isinstance(receipt.get("power_off_duration_sec"), bool)
        and receipt["power_off_duration_sec"] >= 2
        and _profile_version_result(
            before.get("version"), commit, sd_history_mode
        )
        and _profile_identity_result(
            before.get("health"), commit, sd_history_mode
        )
        and _profile_version_result(
            after.get("version"), commit, sd_history_mode
        )
        and _profile_identity_result(
            after.get("health"), commit, sd_history_mode
        )
        and isinstance(before_nonce, int)
        and not isinstance(before_nonce, bool)
        and isinstance(after_nonce, int)
        and not isinstance(after_nonce, bool)
        and before_nonce != 0
        and after_nonce != 0
        and before_nonce != after_nonce
        and after_health.get("reset_reason") == "POWERON"
        and isinstance(before_state, str)
        and re.fullmatch(r"[0-9a-f]{64}", before_state) is not None
        and before_state == after_state
        and raw_boot_marker
        and crashlog_ok
        and receipt.get("public_rf_tx") is False
        and receipt.get("formats_sd") is False
    )
    return ok, before_nonce, after_nonce


def reboot_persistence_gate(
    path: Path | None,
    root: Path,
    commit: str,
    sd_history_mode: str,
    run_id: str,
    run_attempt: str,
) -> CoreGate:
    if (
        path is None
        or sd_history_mode != "disabled"
        or validate_core_reboot_persistence_receipt is None
    ):
        ok = False
        reasons = ["strict_r11_validator_or_receipt_missing"]
        matrix: dict[str, Any] = {}
    else:
        try:
            ok, reasons, matrix = (
                validate_core_reboot_persistence_receipt(
                    path,
                    root=root,
                    expected_commit=commit,
                    expected_run_id=str(run_id),
                    expected_run_attempt=str(run_attempt),
                )
            )
        except (OSError, RuntimeError, TypeError, ValueError) as exc:
            ok = False
            reasons = [
                "strict_r11_validator_failed:"
                f"{type(exc).__name__}"
            ]
            matrix = {}
    return CoreGate(
        "reboot_cold_boot_persistence",
        ok,
        "Five software reboots, three cold boots, and retained state pass",
        path_text(path, root),
        {
            "reasons": reasons,
            "software_reboots": matrix.get("software_cycle_count"),
            "cold_boots": matrix.get("cold_cycle_count"),
            "claim": matrix.get("claim"),
            "github_actions_run": matrix.get("github_actions_run"),
            "workflow_run_attempt": matrix.get(
                "workflow_run_attempt"
            ),
        },
    )


def protocol_migration_gate(
    path: Path | None,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
) -> CoreGate:
    data = read_json(path)
    reasons: list[str] = []
    validation_errors: list[str] = []
    contained = False
    receipt_sha256 = None
    if path is not None:
        try:
            resolved_root = root.resolve(strict=True)
            resolved_path = path.resolve(strict=True)
            resolved_path.relative_to(resolved_root)
            contained = (
                resolved_path.is_file()
                and not is_link_or_reparse(resolved_path)
            )
            if contained:
                receipt_sha256 = sha256_file(resolved_path)
        except (OSError, RuntimeError, ValueError):
            contained = False

    if path is None:
        reasons.append("protocol_migration_receipt_missing")
    elif not contained:
        reasons.append("protocol_migration_receipt_not_contained")
    elif not data:
        reasons.append("protocol_migration_receipt_invalid_json")

    validation_ok = False
    if contained and data:
        try:
            validation_ok, validation_errors = (
                time_protocol_migration.validate_receipt(
                    data,
                    root=root,
                )
            )
        except Exception as exc:  # strict evidence must fail closed
            validation_errors = [
                "strict_protocol_migration_validator_failed:"
                f"{type(exc).__name__}"
            ]
    if not validation_ok:
        reasons.append("strict_protocol_migration_validation_failed")

    real = real_evidence(data)
    if not real:
        reasons.append("protocol_migration_receipt_not_real_evidence")
    binding_ok = (
        exact_sha(data.get("commit")) == commit
        and data.get("github_actions_run") == str(run_id)
        and data.get("workflow_run_attempt") == str(run_attempt)
    )
    if not binding_ok:
        reasons.append("protocol_migration_exact_candidate_binding_failed")

    ok = contained and validation_ok and real and binding_ok
    return CoreGate(
        "protocol_timestamp_migration",
        ok,
        "Legacy protocol timestamp migration is exact and TX-safe",
        path_text(path, root),
        {
            "reasons": reasons,
            "validation_errors": validation_errors,
            "receipt_sha256": receipt_sha256,
            "receipt_commit": data.get("commit"),
            "github_actions_run": data.get("github_actions_run"),
            "workflow_run_attempt": data.get("workflow_run_attempt"),
        },
    )


def remote_rf_gate(
    data: dict,
    path: Path | None,
    root: Path,
    commit: str,
    sd_history_mode: str,
    run_id: str,
    run_attempt: str,
) -> CoreGate:
    peer = data.get("controlled_peer")
    peer = peer if isinstance(peer, dict) else {}
    control = data.get("controlled_peer_control")
    control = control if isinstance(control, dict) else {}
    remote = data.get("controlled_peer_remote")
    remote = remote if isinstance(remote, dict) else {}
    try:
        config = rf_acceptance.validate_remote_peer_config(
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
        config_ok = True
    except (TypeError, ValueError):
        config = None
        config_ok = False

    before_path, before_file_ok = _verified_file_row(
        data.get("controlled_peer_before_receipt"), root
    )
    after_path, after_file_ok = _verified_file_row(
        data.get("controlled_peer_after_receipt"), root
    )
    request_path, request_file_ok = _verified_file_row(
        control.get("request_receipt"), root
    )
    response_path, response_file_ok = _verified_file_row(
        control.get("response_receipt"), root
    )
    try:
        before_raw_bytes = (
            before_path.read_bytes() if before_path is not None else b""
        )
        after_raw_bytes = (
            after_path.read_bytes() if after_path is not None else b""
        )
        request_raw = (
            request_path.read_bytes() if request_path is not None else b""
        )
        response_raw = (
            response_path.read_bytes()
            if response_path is not None
            else b""
        )
        before_raw = rf_acceptance.strict_json_object(
            before_raw_bytes, "remote peer before status"
        )
        after_raw = rf_acceptance.strict_json_object(
            after_raw_bytes, "remote peer after status"
        )
        raw_parse_ok = True
    except (OSError, TypeError, ValueError):
        before_raw = {}
        after_raw = {}
        request_raw = b""
        response_raw = b""
        raw_parse_ok = False

    before_row = data.get("controlled_peer_before_receipt")
    after_row = data.get("controlled_peer_after_receipt")
    before_row = before_row if isinstance(before_row, dict) else {}
    after_row = after_row if isinstance(after_row, dict) else {}
    before_observed = rf_acceptance.parse_aware_timestamp(
        before_row.get("captured_at")
    )
    after_observed = rf_acceptance.parse_aware_timestamp(
        after_row.get("captured_at")
    )
    if config is not None and before_observed is not None:
        before_validation = rf_acceptance.validate_remote_peer_status(
            before_raw, config, observed_at=before_observed
        )
    else:
        before_validation = {"ok": False, "checks": {}}
    if config is not None and after_observed is not None:
        after_validation = rf_acceptance.validate_remote_peer_status(
            after_raw, config, observed_at=after_observed
        )
    else:
        after_validation = {"ok": False, "checks": {}}

    d1l_public_key = rf_acceptance.exact_public_key(
        data.get("d1l_public_key")
    )
    try:
        token = rf_acceptance.validate_safe_token(data.get("token"))
    except ValueError:
        token = ""
    outbound_token = f"{token}_out"
    inbound_token = f"{token}_in"
    fingerprint = str(data.get("target_fingerprint") or "").upper()
    peer_public_key = rf_acceptance.exact_public_key(
        rf_acceptance.get_path(before_raw, "serial", "public_key")
    )
    import_command = (
        rf_acceptance.contact_import_command(peer_public_key)
        if peer_public_key is not None
        else ""
    )
    outbound_text = f"core acceptance test {outbound_token}"
    outbound_command = (
        f"mesh send dm {fingerprint} {outbound_text}"
    )
    message_command = f"messages dm {fingerprint}"
    steps = data.get("steps")
    steps = steps if isinstance(steps, list) else []
    commands = [
        str(step.get("command") or "")
        for step in steps
        if isinstance(step, dict)
    ]
    structure_ok = (
        len(steps) >= 17
        and all(isinstance(step, dict) for step in steps)
        and commands[:10]
        == [
            "version",
            "identity status",
            "contacts",
            import_command,
            "contacts",
            message_command,
            "packets",
            f"routes trace {fingerprint}",
            outbound_command,
            f"packets search {outbound_token}",
        ]
        and commands[-6:]
        == [
            "packets",
            f"routes trace {fingerprint}",
            message_command,
            "packets",
            f"routes trace {fingerprint}",
            "health",
        ]
        and all(
            command == message_command for command in commands[10:-6]
        )
        and commands.count(outbound_command) == 1
        and sum(
            command.strip().lower().startswith("mesh send dm ")
            for command in commands
        )
        == 1
        and not any(
            command.strip().lower().startswith("mesh send public ")
            for command in commands
        )
    )
    results = [
        step.get("result", {}) if isinstance(step, dict) else {}
        for step in steps
    ]
    version = results[0] if len(results) > 0 else {}
    version_time = (
        version.get("time") if isinstance(version, dict) else None
    )
    protocol_tx_ready_before_rf = (
        isinstance(version_time, dict)
        and version_time.get("protocol_tx_ready") is True
        and version_time.get("protocol_tx_block") == "none"
    )
    identity = results[1] if len(results) > 1 else {}
    contacts_before = results[2] if len(results) > 2 else {}
    import_result = results[3] if len(results) > 3 else {}
    contacts_after = results[4] if len(results) > 4 else {}
    baseline_messages = results[5] if len(results) > 5 else {}
    baseline_packets = results[6] if len(results) > 6 else {}
    baseline_route = results[7] if len(results) > 7 else {}
    outbound_result = results[8] if len(results) > 8 else {}
    outbound_packets = results[9] if len(results) > 9 else {}
    final_messages = results[-4] if len(results) >= 4 else {}
    final_packets = results[-3] if len(results) >= 3 else {}
    final_route = results[-2] if len(results) >= 2 else {}
    final_health = results[-1] if results else {}

    control_validation = (
        rf_acceptance.validate_remote_control_exchange(
            request_raw,
            response_raw,
            d1l_public_key=d1l_public_key,
            token=inbound_token,
        )
        if d1l_public_key is not None and token
        else {"ok": False, "checks": {}}
    )
    control_exact = (
        request_file_ok
        and response_file_ok
        and request_path is not None
        and response_path is not None
        and request_path != response_path
        and control_validation.get("ok") is True
        and control.get("op") == "radio.send_dm"
        and config is not None
        and control.get("socket_path") == config["control_socket"]
        and control.get("request_id")
        == control_validation.get("request", {}).get("id")
        and control.get("request")
        == control_validation.get("request")
        and control.get("response")
        == control_validation.get("response")
        and control.get("request_sha256")
        == control_validation.get("request_sha256")
        and control.get("response_sha256")
        == control_validation.get("response_sha256")
        and control.get("validation") == control_validation
    )
    flow = rf_acceptance.remote_peer_flow_validation(
        before=before_raw,
        after=after_raw,
        before_validation=before_validation,
        after_validation=after_validation,
        d1l_public_key=d1l_public_key or "",
        control={
            **control,
            "validation": control_validation,
        },
    )
    remote_exact = (
        remote.get("before_validation") == before_validation
        and remote.get("after_validation") == after_validation
        and remote.get("flow") == flow
        and data.get("controlled_peer_counter_deltas")
        == flow.get("deltas")
    )

    def status_source_exact(row: dict) -> bool:
        return (
            config is not None
            and row.get("source_path") == config["status_path"]
            and row.get("source_host") == config["ssh_host"]
            and row.get("source_hostname") == config["hostname"]
            and row.get("transport") == "ssh"
            and row.get("remote_sha256") == row.get("sha256")
            and isinstance(row.get("remote_mtime_ns"), int)
            and not isinstance(row.get("remote_mtime_ns"), bool)
        )

    def control_source_exact(row: object, transport: str) -> bool:
        return (
            isinstance(row, dict)
            and config is not None
            and row.get("source_path") == config["control_socket"]
            and row.get("source_host") == config["ssh_host"]
            and row.get("source_hostname") == config["hostname"]
            and row.get("transport") == transport
        )

    source_rows_ok = (
        status_source_exact(before_row)
        and status_source_exact(after_row)
        and control_source_exact(
            control.get("request_receipt"),
            "ssh-unix-socket-request",
        )
        and control_source_exact(
            control.get("response_receipt"),
            "ssh-unix-socket-response",
        )
    )
    raw_status_ok = (
        raw_parse_ok
        and before_file_ok
        and after_file_ok
        and before_path is not None
        and after_path is not None
        and before_path != after_path
        and data.get("controlled_peer_before")
        == rf_acceptance.status_snapshot(before_raw)
        and data.get("controlled_peer_after")
        == rf_acceptance.status_snapshot(after_raw)
        and before_validation.get("ok") is True
        and after_validation.get("ok") is True
        and peer_public_key == rf_acceptance.REMOTE_PEER_PUBLIC_KEY
        and rf_acceptance.exact_public_key(
            rf_acceptance.get_path(
                after_raw, "serial", "public_key"
            )
        )
        == rf_acceptance.REMOTE_PEER_PUBLIC_KEY
    )
    contact_setup = data.get("controlled_peer_contact_setup")
    contact_setup = (
        contact_setup if isinstance(contact_setup, dict) else {}
    )
    contact_ok = (
        peer_public_key is not None
        and rf_acceptance.contact_import_ok(
            import_result, peer_public_key, fingerprint
        )
        and rf_acceptance.contacts_has_exact_peer(
            contacts_after, peer_public_key, fingerprint
        )
        and contact_setup
        == {
            "name": rf_acceptance.RADIO_LISTENER_CONTACT_NAME,
            "public_key": peer_public_key,
            "fingerprint": fingerprint,
            "import_command": import_command,
            "before": contacts_before,
            "import_result": import_result,
            "after": contacts_after,
        }
    )
    d1l_fingerprint = rf_acceptance.public_key_fingerprint(
        d1l_public_key or ""
    )
    identity_ok = (
        d1l_public_key is not None
        and d1l_fingerprint is not None
        and identity.get("ok") is True
        and rf_acceptance.exact_public_key(
            identity.get("public_key")
        )
        == d1l_public_key
        and str(identity.get("fingerprint") or "").upper()
        == d1l_fingerprint
        and data.get("expected_identity_fingerprint")
        == d1l_fingerprint
        and data.get("identity_fingerprint") == d1l_fingerprint
    )
    outbound_ok = (
        outbound_result.get("ok") is True
        and rf_acceptance.contains_token(
            outbound_packets, outbound_token
        )
    )
    inbound_ok = rf_acceptance.new_exact_listener_reply(
        baseline_messages,
        final_messages,
        fingerprint,
        inbound_token,
    )
    transaction_correlation = (
        rf_acceptance.correlated_listener_transaction(
            baseline_messages=baseline_messages,
            final_messages=final_messages,
            baseline_packets=baseline_packets,
            final_packets=final_packets,
            baseline_route=baseline_route,
            final_route=final_route,
            outbound_token=outbound_token,
            fingerprint=fingerprint,
            inbound_text=inbound_token,
        )
    )
    ack_ok = (
        transaction_correlation.get("ack_path_ok") is True
        and data.get("transaction_correlation")
        == transaction_correlation
    )
    route_ok = (
        transaction_correlation.get("direct_route_ok") is True
        and data.get("transaction_correlation")
        == transaction_correlation
    )
    derived_checks = {
        "identity_public_key_matches": identity_ok,
        "controlled_peer_observed": raw_status_ok
        and flow.get("ok") is True,
        "controlled_peer_status_connected": raw_status_ok
        and flow.get("ok") is True
        and control_exact,
        "controlled_peer_contact_ready": contact_ok,
        "outbound_dm": outbound_ok,
        "inbound_dm": inbound_ok,
        "ack_path": ack_ok,
        "direct_route": route_ok,
        "health_ready": _profile_ready_health_result(
            final_health, commit, sd_history_mode
        ),
        "no_public_commands": structure_ok,
        "protocol_tx_ready_before_rf": protocol_tx_ready_before_rf,
        "exact_candidate": _profile_version_result(
            version, commit, sd_history_mode
        ),
    }
    checks_ok = (
        data.get("checks") == derived_checks
        and all(derived_checks.values())
    )
    peer_binding_ok = (
        config_ok
        and config is not None
        and peer.get("evidence_source")
        == rf_acceptance.REMOTE_PEER_EVIDENCE_SOURCE
        and peer.get("port") is None
        and peer.get("fingerprint")
        == rf_acceptance.REMOTE_PEER_FINGERPRINT
        and peer.get("public_key")
        == rf_acceptance.REMOTE_PEER_PUBLIC_KEY
        and data.get("controlled_peer_adapter")
        == rf_acceptance.REMOTE_PEER_ADAPTER
    )
    ok = (
        real_evidence(data)
        and data.get("schema")
        == rf_acceptance.RF_FULL_ACCEPTANCE_SCHEMA
        and data.get("mode") == "rf-full-acceptance"
        and data.get("ok") is True
        and data.get("execution_complete") is True
        and data.get("closure_eligible") is True
        and data.get("physical_observed") is True
        and data.get("port") == D1L_CORE_PORT
        and exact_candidate_fields(data, commit)
        and str(data.get("github_actions_run")) == str(run_id)
        and str(data.get("workflow_run_attempt")) == str(run_attempt)
        and data.get("device_idf_version") == EXPECTED_IDF_VERSION
        and data.get("device_release_profile") == CORE_RELEASE_PROFILE
        and data.get("device_sd_history_mode") == sd_history_mode
        and checks_ok
        and structure_ok
        and source_rows_ok
        and raw_status_ok
        and remote_exact
        and flow.get("ok") is True
        and control_exact
        and contact_ok
        and transaction_correlation.get("ok") is True
        and peer_binding_ok
        and rf_acceptance.remote_peer_report_shape_ok(data)
        and data.get("outbound_token") == outbound_token
        and data.get("inbound_token") == inbound_token
        and isinstance(data.get("inbound_seen_at"), str)
        and bool(data["inbound_seen_at"])
        and data.get("dm_rf_tx") is True
        and data.get("public_rf_tx") is False
        and data.get("formats_sd") is False
    )
    evidence = (
        path_text(path, root)
        + path_text(before_path, root)
        + path_text(after_path, root)
        + path_text(request_path, root)
        + path_text(response_path, root)
    )
    return CoreGate(
        "controlled_bidirectional_rf_dm",
        ok,
        "Controlled-peer outbound/inbound DM, ACK/PATH, direct route, and health pass",
        evidence,
        {
            "checks_ok": checks_ok,
            "derived_checks": derived_checks,
            "command_structure_ok": structure_ok,
            "raw_status_ok": raw_status_ok,
            "peer_flow_ok": flow.get("ok") is True,
            "control_exchange_ok": control_exact,
            "contact_setup_ok": contact_ok,
            "source_rows_ok": source_rows_ok,
            "peer_binding_ok": peer_binding_ok,
            "counter_deltas": flow.get("deltas"),
            "transaction_correlation": transaction_correlation,
        },
    )


def rf_gate(
    path: Path | None,
    root: Path,
    commit: str,
    sd_history_mode: str,
    run_id: str,
    run_attempt: str,
) -> CoreGate:
    data = read_json(path)
    peer_row = data.get("controlled_peer")
    if (
        isinstance(peer_row, dict)
        and peer_row.get("evidence_source")
        == rf_acceptance.REMOTE_PEER_EVIDENCE_SOURCE
    ):
        return remote_rf_gate(
            data,
            path,
            root,
            commit,
            sd_history_mode,
            run_id,
            run_attempt,
        )
    before_path, before_file_ok = _verified_file_row(
        data.get("controlled_peer_before_receipt"), root
    )
    after_path, after_file_ok = _verified_file_row(
        data.get("controlled_peer_after_receipt"), root
    )
    before_raw = read_json(before_path)
    after_raw = read_json(after_path)
    peer = data.get("controlled_peer")
    peer = peer if isinstance(peer, dict) else {}
    fingerprint = str(data.get("target_fingerprint") or "").upper()
    peer_public_key = rf_acceptance.exact_public_key(
        rf_acceptance.get_path(before_raw, "serial", "public_key")
    )
    d1l_public_key = rf_acceptance.exact_public_key(
        data.get("d1l_public_key")
    )
    d1l_fingerprint = rf_acceptance.public_key_fingerprint(
        d1l_public_key or ""
    )
    token = data.get("token")
    token = token if isinstance(token, str) and token else ""
    outbound_token = f"{token}_out"
    outbound_text = f"core acceptance test {outbound_token}"
    import_command = (
        rf_acceptance.contact_import_command(peer_public_key)
        if peer_public_key is not None
        else ""
    )
    outbound_command = (
        f"mesh send dm {fingerprint} {outbound_text}"
    )
    message_command = f"messages dm {fingerprint}"
    steps = data.get("steps")
    steps = steps if isinstance(steps, list) else []
    commands = [
        str(step.get("command") or "")
        for step in steps
        if isinstance(step, dict)
    ]
    structure_ok = (
        len(steps) >= 17
        and all(isinstance(step, dict) for step in steps)
        and commands[:10]
        == [
            "version",
            "identity status",
            "contacts",
            import_command,
            "contacts",
            message_command,
            "packets",
            f"routes trace {fingerprint}",
            outbound_command,
            f"packets search {outbound_token}",
        ]
        and commands[-6:]
        == [
            "packets",
            f"routes trace {fingerprint}",
            message_command,
            "packets",
            f"routes trace {fingerprint}",
            "health",
        ]
        and all(
            command == message_command for command in commands[10:-6]
        )
        and commands.count(outbound_command) == 1
        and sum(
            command.strip().lower().startswith("mesh send dm ")
            for command in commands
        )
        == 1
        and not any(
            command.strip().lower().startswith("mesh send public ")
            for command in commands
        )
    )
    results = [
        step.get("result", {}) if isinstance(step, dict) else {}
        for step in steps
    ]
    version = results[0] if len(results) > 0 else {}
    version_time = (
        version.get("time") if isinstance(version, dict) else None
    )
    protocol_tx_ready_before_rf = (
        isinstance(version_time, dict)
        and version_time.get("protocol_tx_ready") is True
        and version_time.get("protocol_tx_block") == "none"
    )
    identity = results[1] if len(results) > 1 else {}
    contacts_before = results[2] if len(results) > 2 else {}
    import_result = results[3] if len(results) > 3 else {}
    contacts_after = results[4] if len(results) > 4 else {}
    baseline_messages = results[5] if len(results) > 5 else {}
    baseline_packets = results[6] if len(results) > 6 else {}
    baseline_route = results[7] if len(results) > 7 else {}
    outbound_result = results[8] if len(results) > 8 else {}
    outbound_packets = results[9] if len(results) > 9 else {}
    final_messages = results[-4] if len(results) >= 4 else {}
    final_packets = results[-3] if len(results) >= 3 else {}
    final_route = results[-2] if len(results) >= 2 else {}
    final_health = results[-1] if results else {}
    raw_status_ok = (
        before_file_ok
        and after_file_ok
        and data.get("controlled_peer_before")
        == rf_acceptance.status_snapshot(before_raw)
        and data.get("controlled_peer_after")
        == rf_acceptance.status_snapshot(after_raw)
        and rf_acceptance.radio_listener_connected(
            before_raw,
            rf_acceptance.RADIO_LISTENER_PORT,
            fingerprint,
        )
        and rf_acceptance.radio_listener_connected(
            after_raw,
            rf_acceptance.RADIO_LISTENER_PORT,
            fingerprint,
        )
        and before_raw.get("run_id") == after_raw.get("run_id")
        and peer_public_key is not None
        and rf_acceptance.exact_public_key(
            rf_acceptance.get_path(
                after_raw, "serial", "public_key"
            )
        )
        == peer_public_key
        and rf_acceptance.public_key_fingerprint(peer_public_key)
        == fingerprint
    )
    deltas = {
        name: rf_acceptance.counter_delta(before_raw, after_raw, name)
        for name in (
            "rx_dm_total",
            "tx_dm_total",
            "local_fast_reply_total",
            "tx_dm_ack_miss_total",
        )
    }
    peer_flow_ok = (
        deltas
        == {
            "rx_dm_total": 1,
            "tx_dm_total": 1,
            "local_fast_reply_total": 1,
            "tx_dm_ack_miss_total": 0,
        }
        and data.get("controlled_peer_counter_deltas") == deltas
        and rf_acceptance.listener_sender_matches(
            after_raw, d1l_public_key
        )
        and rf_acceptance.get_path(
            after_raw, "mesh", "last_rx_kind"
        )
        == "dm"
        and rf_acceptance.get_path(
            after_raw, "mesh", "last_tx_kind"
        )
        == "dm"
        and rf_acceptance.get_path(
            before_raw, "mesh", "last_rx_at"
        )
        != rf_acceptance.get_path(
            after_raw, "mesh", "last_rx_at"
        )
        and rf_acceptance.get_path(
            before_raw, "mesh", "last_tx_at"
        )
        != rf_acceptance.get_path(
            after_raw, "mesh", "last_tx_at"
        )
    )
    status_source = str(
        rf_acceptance.RADIO_LISTENER_STATUS_PATH.resolve()
    ).casefold()
    source_rows_ok = all(
        isinstance(row, dict)
        and str(Path(str(row.get("source_path") or "")).resolve()).casefold()
        == status_source
        for row in (
            data.get("controlled_peer_before_receipt"),
            data.get("controlled_peer_after_receipt"),
        )
    )
    try:
        peer_status_path_ok = (
            Path(str(peer.get("status_path") or "")).resolve()
            == rf_acceptance.RADIO_LISTENER_STATUS_PATH.resolve()
        )
    except (OSError, RuntimeError, ValueError):
        peer_status_path_ok = False
    contact_setup = data.get("controlled_peer_contact_setup")
    contact_setup = (
        contact_setup if isinstance(contact_setup, dict) else {}
    )
    contact_ok = (
        peer_public_key is not None
        and rf_acceptance.contact_import_ok(
            import_result, peer_public_key, fingerprint
        )
        and rf_acceptance.contacts_has_exact_peer(
            contacts_after, peer_public_key, fingerprint
        )
        and contact_setup
        == {
            "name": rf_acceptance.RADIO_LISTENER_CONTACT_NAME,
            "public_key": peer_public_key,
            "fingerprint": fingerprint,
            "import_command": import_command,
            "before": contacts_before,
            "import_result": import_result,
            "after": contacts_after,
        }
    )
    identity_ok = (
        d1l_public_key is not None
        and d1l_fingerprint is not None
        and identity.get("ok") is True
        and rf_acceptance.exact_public_key(
            identity.get("public_key")
        )
        == d1l_public_key
        and str(identity.get("fingerprint") or "").upper()
        == d1l_fingerprint
        and data.get("expected_identity_fingerprint")
        == d1l_fingerprint
        and data.get("identity_fingerprint") == d1l_fingerprint
    )
    outbound_ok = (
        outbound_result.get("ok") is True
        and rf_acceptance.contains_token(
            outbound_packets, outbound_token
        )
    )
    inbound_ok = rf_acceptance.new_exact_listener_reply(
        baseline_messages, final_messages, fingerprint
    )
    transaction_correlation = (
        rf_acceptance.correlated_listener_transaction(
            baseline_messages=baseline_messages,
            final_messages=final_messages,
            baseline_packets=baseline_packets,
            final_packets=final_packets,
            baseline_route=baseline_route,
            final_route=final_route,
            outbound_token=outbound_token,
            fingerprint=fingerprint,
        )
    )
    ack_ok = (
        transaction_correlation.get("ack_path_ok") is True
        and data.get("transaction_correlation")
        == transaction_correlation
    )
    route_ok = (
        transaction_correlation.get("direct_route_ok") is True
        and data.get("transaction_correlation")
        == transaction_correlation
    )
    derived_checks = {
        "identity_public_key_matches": identity_ok,
        "controlled_peer_observed": raw_status_ok and peer_flow_ok,
        "controlled_peer_status_connected": raw_status_ok
        and peer_flow_ok,
        "controlled_peer_contact_ready": contact_ok,
        "outbound_dm": outbound_ok,
        "inbound_dm": inbound_ok,
        "ack_path": ack_ok,
        "direct_route": route_ok,
        "health_ready": _profile_ready_health_result(
            final_health, commit, sd_history_mode
        ),
        "no_public_commands": structure_ok
        and not any(
            command.strip().lower().startswith("mesh send public ")
            for command in commands
        ),
        "protocol_tx_ready_before_rf": protocol_tx_ready_before_rf,
        "exact_candidate": _profile_version_result(
            version, commit, sd_history_mode
        ),
    }
    checks_ok = (
        data.get("checks") == derived_checks
        and all(derived_checks.values())
    )
    ok = (
        real_evidence(data)
        and data.get("schema")
        == rf_acceptance.RF_FULL_ACCEPTANCE_SCHEMA
        and data.get("mode") == "rf-full-acceptance"
        and data.get("ok") is True
        and data.get("execution_complete") is True
        and data.get("closure_eligible") is True
        and data.get("physical_observed") is True
        and data.get("port") == D1L_CORE_PORT
        and exact_candidate_fields(data, commit)
        and str(data.get("github_actions_run")) == str(run_id)
        and str(data.get("workflow_run_attempt")) == str(run_attempt)
        and data.get("device_idf_version") == EXPECTED_IDF_VERSION
        and data.get("device_release_profile") == CORE_RELEASE_PROFILE
        and data.get("device_sd_history_mode") == sd_history_mode
        and checks_ok
        and structure_ok
        and source_rows_ok
        and raw_status_ok
        and peer_flow_ok
        and contact_ok
        and transaction_correlation.get("ok") is True
        and peer.get("port") == rf_acceptance.RADIO_LISTENER_PORT
        and peer.get("fingerprint") == fingerprint
        and peer.get("public_key") == peer_public_key
        and peer_status_path_ok
        and data.get("controlled_peer_adapter")
        == rf_acceptance.RADIO_LISTENER_PROFILE
        and data.get("outbound_token") == outbound_token
        and data.get("inbound_token")
        == rf_acceptance.RADIO_LISTENER_REPLY
        and isinstance(data.get("inbound_seen_at"), str)
        and bool(data["inbound_seen_at"])
        and data.get("dm_rf_tx") is True
        and data.get("public_rf_tx") is False
        and data.get("formats_sd") is False
    )
    return CoreGate(
        "controlled_bidirectional_rf_dm",
        ok,
        "Controlled-peer outbound/inbound DM, ACK/PATH, direct route, and health pass",
        path_text(path, root)
        + path_text(before_path, root)
        + path_text(after_path, root),
        {
            "checks_ok": checks_ok,
            "derived_checks": derived_checks,
            "command_structure_ok": structure_ok,
            "raw_status_ok": raw_status_ok,
            "peer_flow_ok": peer_flow_ok,
            "contact_setup_ok": contact_ok,
            "source_rows_ok": source_rows_ok,
            "peer_status_path_ok": peer_status_path_ok,
            "peer_port": peer.get("port"),
            "counter_deltas": deltas,
            "transaction_correlation": transaction_correlation,
        },
    )


def _health_rows(data: dict) -> list[dict]:
    rows = []
    for sample in data.get("samples", []):
        if not isinstance(sample, dict):
            continue
        for result in sample.get("results", []):
            if isinstance(result, dict) and result.get("cmd") == "health":
                rows.append(result)
    return rows


def _mesh_runtime_rows(data: dict) -> list[dict]:
    rows = []
    for sample in data.get("samples", []):
        if not isinstance(sample, dict):
            continue
        for result in sample.get("results", []):
            if (
                isinstance(result, dict)
                and result.get("cmd") == "mesh status"
                and isinstance(result.get("runtime"), dict)
            ):
                rows.append(result["runtime"])
    return rows


def _queue_runtime_clean(rows: list[dict]) -> bool:
    if not rows:
        return False
    zero_counters = (
        "queue_drops",
        "callback_event_drops",
        "command_queue_saturation",
        "priority_queue_saturation",
    )
    if any(
        not isinstance(row.get(field), int)
        or isinstance(row.get(field), bool)
        or row.get(field) != 0
        for row in rows
        for field in zero_counters
    ):
        return False
    queue_limits = {
        "command_queue_depth": 6,
        "priority_queue_depth": 4,
        "event_queue_depth": 8,
    }
    return all(
        isinstance(row.get(field), int)
        and not isinstance(row.get(field), bool)
        and 0 <= row[field] < limit
        for row in rows
        for field, limit in queue_limits.items()
    )


def _bounded_memory(rows: list[dict], field: str) -> bool:
    values = [
        row.get(field)
        for row in rows
        if isinstance(row.get(field), int) and not isinstance(row.get(field), bool)
    ]
    if len(values) != len(rows) or not values or min(values) <= 0:
        return False
    permitted_drop = max(65536, int(values[0] * 0.25))
    return values[-1] >= values[0] - permitted_drop


def soak_artifact_ok(
    data: dict,
    *,
    commit: str,
    sd_history_mode: str,
    active: bool,
    run_id: str,
    run_attempt: str,
) -> bool:
    summary = data.get("summary")
    samples = data.get("samples")
    health = _health_rows(data)
    mesh_runtime = _mesh_runtime_rows(data)
    minimum_duration = 3600 if active else 1800
    duration = data.get("duration_sec")
    interval = data.get("sample_interval_sec")
    try:
        started_at = datetime.fromisoformat(
            str(data.get("started_at")).replace("Z", "+00:00")
        )
        ended_at = datetime.fromisoformat(
            str(data.get("ended_at")).replace("Z", "+00:00")
        )
        wall_duration_sec = (ended_at - started_at).total_seconds()
        wall_duration_ok = wall_duration_sec >= minimum_duration
    except (TypeError, ValueError):
        wall_duration_sec = None
        wall_duration_ok = False
    sample_span_ok = (
        isinstance(duration, (int, float))
        and not isinstance(duration, bool)
        and duration >= minimum_duration
        and isinstance(interval, (int, float))
        and not isinstance(interval, bool)
        and 0 < interval <= 300
        and isinstance(samples, list)
        and len(samples) >= int(minimum_duration // interval) + 1
        and isinstance(samples[-1].get("elapsed_sec"), (int, float))
        and samples[-1]["elapsed_sec"] >= minimum_duration
    )
    elapsed_values = [
        sample.get("elapsed_sec")
        for sample in samples
        if isinstance(sample, dict)
    ] if isinstance(samples, list) else []
    elapsed_sequence_ok = (
        len(elapsed_values) == len(samples)
        and all(
            isinstance(value, (int, float)) and not isinstance(value, bool)
            for value in elapsed_values
        )
        and elapsed_values
        and 0 <= elapsed_values[0] <= 60
        and all(
            0 < right - left <= interval + 60
            for left, right in zip(elapsed_values, elapsed_values[1:])
        )
    )
    per_sample_coverage_ok = bool(samples)
    if per_sample_coverage_ok:
        for sample in samples:
            results = sample.get("results") if isinstance(sample, dict) else None
            results = results if isinstance(results, list) else []
            by_command: dict[str, list[dict]] = {}
            for result in results:
                if isinstance(result, dict) and isinstance(
                    result.get("cmd"), str
                ):
                    by_command.setdefault(result["cmd"], []).append(result)
            required = ("health", "mesh status", "storage status", "crashlog")
            if any(len(by_command.get(command, [])) != 1 for command in required):
                per_sample_coverage_ok = False
                break
            if (
                not _profile_identity_result(
                    by_command["health"][0], commit, sd_history_mode
                )
                or not isinstance(
                    by_command["mesh status"][0].get("runtime"), dict
                )
                or (
                    sd_history_mode == "disabled"
                    and not disabled_storage_status_ok(
                        by_command["storage status"][0]
                    )
                )
                or not crashlog_clean(by_command["crashlog"][0])
            ):
                per_sample_coverage_ok = False
                break
    health_ok = bool(health) and all(
        row.get("ok") is True
        and exact_sha(row.get("build_commit")) == commit
        and row.get("release_profile") == CORE_RELEASE_PROFILE
        and row.get("sd_history_mode") == sd_history_mode
        and row.get("board_ready") is True
        and row.get("ui_ready") is True
        and isinstance(row.get("boot_nonce"), int)
        and not isinstance(row.get("boot_nonce"), bool)
        and row.get("boot_nonce") != 0
        and isinstance(row.get("uptime_ms"), int)
        and not isinstance(row.get("uptime_ms"), bool)
        and row.get("uptime_ms") >= 0
        for row in health
    )
    boot_nonce_stable = (
        bool(health)
        and len({row.get("boot_nonce") for row in health}) == 1
    )
    uptimes = [row.get("uptime_ms") for row in health]
    raw_uptime_monotonic = (
        bool(uptimes)
        and all(
            isinstance(value, int) and not isinstance(value, bool)
            for value in uptimes
        )
        and all(left <= right for left, right in zip(uptimes, uptimes[1:]))
    )
    summary_ok = (
        isinstance(summary, dict)
        and summary.get("ok") is True
        and summary.get("threshold_failures") == []
        and summary.get("command_timeout_seen") is False
        and summary.get("unexpected_console_restart_seen") is False
        and summary.get("board_ready_all") is True
        and summary.get("ui_ready_all") is True
        and summary.get("mesh_ready_all") is True
        and summary.get("uptime_monotonic") is True
        and isinstance(summary.get("retained_task_stack_free_bytes_floor"), int)
        and summary["retained_task_stack_free_bytes_floor"] >= 4096
        and summary.get("crashlog_crash_like_count") == 0
        and summary.get("storage_store_backend_stable_all") is True
    )
    try:
        recomputed_summary = soak_runner.summarize_soak(
            samples=samples if isinstance(samples, list) else [],
            active_events=(
                data.get("active_events")
                if isinstance(data.get("active_events"), list)
                else []
            ),
            require_rx_delta=data.get("require_rx_delta") is True,
            min_rx_delta=int(data.get("min_rx_delta")),
            min_tx_delta=int(data.get("min_tx_delta")),
            sample_storage=data.get("sample_storage") is True,
            sd_file_canary=data.get("sd_file_canary") is True,
            allow_sd_unavailable=data.get("allow_sd_unavailable") is True,
        )
        summary_raw_exact = summary == recomputed_summary
    except (TypeError, ValueError):
        summary_raw_exact = False
    setup_events = data.get("setup_events")
    setup_events = setup_events if isinstance(setup_events, list) else []
    setup_ok = (
        data.get("preflight_commands") == ["version"]
        and data.get("preflight_failure") is None
        and len(setup_events) == 1
        and setup_events[0].get("cmd") == "version"
        and setup_events[0].get("elapsed_sec") == 0.0
        and setup_events[0].get("result")
        == data.get("version_preflight")
        and _profile_version_result(
            data.get("version_preflight"), commit, sd_history_mode
        )
    )
    invocation_ok = (
        data.get("commands")
        == list(soak_runner.SOAK_COMMANDS)
        + [soak_runner.STORAGE_STATUS_COMMAND]
        and data.get("sample_storage") is True
        and data.get("sd_file_canary") is False
        and data.get("allow_sd_unavailable") is True
        and data.get("clear_crashlog_before_start") is False
        and data.get("release_profile") == CORE_RELEASE_PROFILE
        and data.get("sd_history_mode") == sd_history_mode
        and data.get("device_idf_version") == EXPECTED_IDF_VERSION
        and data.get("device_release_profile") == CORE_RELEASE_PROFILE
        and data.get("device_sd_history_mode") == sd_history_mode
    )
    storage_backends = []
    if isinstance(summary, dict):
        storage_backends.extend(summary.get("storage_data_backends") or [])
        storage_backends.extend(summary.get("storage_packet_log_backends") or [])
        for values in (summary.get("storage_store_backends") or {}).values():
            if isinstance(values, list):
                storage_backends.extend(values)
    if sd_history_mode == "disabled":
        expected_store_backends = {
            "settings": ["nvs"],
            "identity": ["nvs"],
            "messages": ["nvs"],
            "dm": ["nvs"],
            "packets": ["nvs"],
            "routes": ["nvs"],
            "contacts": ["nvs"],
            "read_state": ["nvs"],
            "crashlog": ["nvs"],
            "map_tiles": ["unavailable"],
            "exports": ["serial"],
        }
        storage_ok = (
            bool(storage_backends)
            and summary.get("storage_data_backends") == ["nvs"]
            and summary.get("storage_packet_log_backends") == ["nvs"]
            and summary.get("storage_store_backends")
            == expected_store_backends
            and data.get("allow_sd_unavailable") is True
        )
    else:
        storage_ok = bool(storage_backends) and all(
            str(value).lower() in {"nvs", "sd", "mixed"}
            for value in storage_backends
        )
    if active:
        active_events = data.get("active_events")
        active_events = (
            active_events if isinstance(active_events, list) else []
        )
        active_command = data.get("active_command")
        active_fingerprint = data.get("active_dm_fingerprint")
        active_elapsed = [
            event.get("elapsed_sec")
            for event in active_events
            if isinstance(event, dict)
        ]
        raw_active_events_ok = (
            len(active_events) >= 6
            and isinstance(active_command, str)
            and active_command.startswith("mesh send dm ")
            and isinstance(active_fingerprint, str)
            and re.fullmatch(r"[0-9A-F]{16}", active_fingerprint) is not None
            and len(active_elapsed) == len(active_events)
            and all(
                isinstance(value, (int, float))
                and not isinstance(value, bool)
                for value in active_elapsed
            )
            and all(
                left < right
                for left, right in zip(active_elapsed, active_elapsed[1:])
            )
            and all(
                isinstance(event, dict)
                and event.get("command") == active_command
                and event.get("fingerprint") == active_fingerprint
                and isinstance(event.get("text"), str)
                and bool(event.get("text"))
                and active_command
                == (
                    f"mesh send dm {active_fingerprint} "
                    f"{event.get('text')}"
                )
                and event.get("result", {}).get("schema") == 1
                and event.get("result", {}).get("ok") is True
                and event.get("result", {}).get("cmd") == "mesh send dm"
                for event in active_events
            )
        )
        traffic_ok = (
            isinstance(data.get("active_dm_fingerprint"), str)
            and bool(data["active_dm_fingerprint"])
            and data.get("active_command", "").startswith("mesh send dm ")
            and data.get("dm_rf_tx") is True
            and raw_active_events_ok
            and isinstance(summary, dict)
            and isinstance(summary.get("mesh_rx_packet_delta"), int)
            and summary["mesh_rx_packet_delta"] >= 1
            and isinstance(summary.get("mesh_tx_packet_delta"), int)
            and summary["mesh_tx_packet_delta"] >= 6
        )
    else:
        raw_active_events_ok = data.get("active_events") == []
        traffic_ok = (
            data.get("active_command") is None
            and data.get("active_events") == []
            and data.get("dm_rf_tx") is False
        )
    return (
        real_evidence(data)
        and data.get("mode") == "hardware"
        and data.get("ok") is True
        and data.get("closure_eligible") is True
        and data.get("physical_observed") is True
        and data.get("port") == D1L_CORE_PORT
        and exact_candidate_fields(data, commit)
        and str(data.get("github_actions_run")) == str(run_id)
        and str(data.get("workflow_run_attempt")) == str(run_attempt)
        and wall_duration_ok
        and setup_ok
        and invocation_ok
        and sample_span_ok
        and elapsed_sequence_ok
        and per_sample_coverage_ok
        and health_ok
        and boot_nonce_stable
        and raw_uptime_monotonic
        and summary_ok
        and summary_raw_exact
        and storage_ok
        and traffic_ok
        and raw_active_events_ok
        and _bounded_memory(health, "heap_free")
        and _bounded_memory(health, "psram_free")
        and _queue_runtime_clean(mesh_runtime)
        and data.get("public_rf_tx") is False
        and data.get("formats_sd") is False
        and data.get("aborted_after_timeout") is None
    )


def active_soak_peer_flow_ok(
    data: dict,
    rf: dict,
    root: Path,
) -> tuple[bool, dict[str, Any]]:
    before_path, before_file_ok = _verified_file_row(
        data.get("controlled_peer_before_receipt"), root
    )
    after_path, after_file_ok = _verified_file_row(
        data.get("controlled_peer_after_receipt"), root
    )
    before_row = data.get("controlled_peer_before_receipt")
    after_row = data.get("controlled_peer_after_receipt")
    before_row = before_row if isinstance(before_row, dict) else {}
    after_row = after_row if isinstance(after_row, dict) else {}
    peer = rf.get("controlled_peer")
    peer = peer if isinstance(peer, dict) else {}
    remote_mode = (
        peer.get("evidence_source")
        == rf_acceptance.REMOTE_PEER_EVIDENCE_SOURCE
    )
    remote_config: dict | None = None
    if remote_mode:
        try:
            remote_config = rf_acceptance.validate_remote_peer_config(
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
        except (TypeError, ValueError):
            remote_config = None

    try:
        if remote_mode:
            before = rf_acceptance.strict_json_object(
                before_path.read_bytes() if before_path else b"",
                "remote active-soak peer before status",
            )
            after = rf_acceptance.strict_json_object(
                after_path.read_bytes() if after_path else b"",
                "remote active-soak peer after status",
            )
        else:
            before = read_json(before_path)
            after = read_json(after_path)
        raw_parse_ok = True
    except (OSError, TypeError, ValueError):
        before = {}
        after = {}
        raw_parse_ok = False

    canonical_status = rf_acceptance.RADIO_LISTENER_STATUS_PATH.resolve()

    def canonical_source(row: object) -> bool:
        if not isinstance(row, dict):
            return False
        if remote_mode:
            return (
                remote_config is not None
                and row.get("source_path")
                == remote_config["status_path"]
                and row.get("source_host")
                == remote_config["ssh_host"]
                and row.get("source_hostname")
                == remote_config["hostname"]
                and row.get("transport") == "ssh"
                and row.get("remote_sha256") == row.get("sha256")
                and isinstance(row.get("remote_mtime_ns"), int)
                and not isinstance(row.get("remote_mtime_ns"), bool)
            )
        source = row.get("source_path")
        try:
            return (
                isinstance(source, str)
                and Path(source).resolve() == canonical_status
            )
        except (OSError, RuntimeError, ValueError):
            return False

    active_events = data.get("active_events")
    active_events = active_events if isinstance(active_events, list) else []
    successful_send_count = sum(
        1
        for event in active_events
        if isinstance(event, dict)
        and isinstance(event.get("result"), dict)
        and event["result"].get("ok") is True
    )
    expected_send_count = soak_runner.expected_active_send_count(
        data.get("duration_sec"), data.get("active_interval_sec")
    )
    before_observed = rf_acceptance.parse_aware_timestamp(
        before_row.get("captured_at")
    )
    after_observed = rf_acceptance.parse_aware_timestamp(
        after_row.get("captured_at")
    )
    if (
        remote_config is not None
        and before_observed is not None
        and after_observed is not None
    ):
        before_validation = rf_acceptance.validate_remote_peer_status(
            before,
            remote_config,
            observed_at=before_observed,
        )
        after_validation = rf_acceptance.validate_remote_peer_status(
            after,
            remote_config,
            observed_at=after_observed,
        )
        remote_validation_ok = (
            before_validation.get("ok") is True
            and after_validation.get("ok") is True
            and before_observed <= after_observed
        )
    else:
        before_validation = None
        after_validation = None
        remote_validation_ok = not remote_mode

    flow_ok, deltas = soak_runner.active_listener_flow_ok(
        before,
        after,
        successful_send_count=successful_send_count,
        d1l_public_key=rf.get("d1l_public_key"),
        peer_fingerprint=str(rf.get("target_fingerprint") or ""),
        peer_public_key=peer.get("public_key"),
        minimum_send_count=expected_send_count or sys.maxsize,
        remote_config=remote_config if remote_mode else None,
    )
    controlled_peer_remote = data.get("controlled_peer_remote")
    if remote_mode:
        remote_report_exact = controlled_peer_remote == {
            "before_validation": before_validation,
            "after_validation": after_validation,
        }
    else:
        remote_report_exact = controlled_peer_remote in (None, {})
    report_exact = (
        data.get("controlled_peer_before")
        == rf_acceptance.status_snapshot(before)
        and data.get("controlled_peer_after")
        == rf_acceptance.status_snapshot(after)
        and data.get("controlled_peer_counter_deltas") == deltas
        and data.get("controlled_peer_successful_send_count")
        == successful_send_count
        and data.get("controlled_peer_expected_send_count")
        == expected_send_count
        and data.get("controlled_peer_flow_ok") is True
        and remote_report_exact
    )
    files_exact = (
        raw_parse_ok
        and before_file_ok
        and after_file_ok
        and before_path is not None
        and after_path is not None
        and before_path != after_path
        and canonical_source(before_row)
        and canonical_source(after_row)
    )
    common_binding = (
        data.get("active_dm_fingerprint")
        == rf.get("target_fingerprint")
        and soak_runner.listener_test_text_ok(data.get("active_dm_text"))
        and rf_acceptance.exact_public_key(peer.get("public_key"))
        is not None
        and rf_acceptance.exact_public_key(rf.get("d1l_public_key"))
        is not None
    )
    if remote_mode:
        binding_exact = (
            common_binding
            and remote_config is not None
            and rf.get("controlled_peer_adapter")
            == rf_acceptance.REMOTE_PEER_ADAPTER
            and peer.get("port") is None
            and remote_config["device"]
            == rf_acceptance.REMOTE_PEER_DEVICE
            and remote_config["public_key"]
            == rf_acceptance.REMOTE_PEER_PUBLIC_KEY
            and peer.get("fingerprint")
            == rf_acceptance.REMOTE_PEER_FINGERPRINT
        )
    else:
        try:
            peer_status_exact = (
                Path(str(peer.get("status_path"))).resolve()
                == canonical_status
            )
        except (OSError, RuntimeError, ValueError):
            peer_status_exact = False
        binding_exact = (
            common_binding
            and rf.get("controlled_peer_adapter")
            == rf_acceptance.RADIO_LISTENER_PROFILE
            and peer.get("port") == rf_acceptance.RADIO_LISTENER_PORT
            and peer_status_exact
        )
    ok = (
        files_exact
        and binding_exact
        and report_exact
        and flow_ok
        and remote_validation_ok
        and expected_send_count is not None
        and successful_send_count >= max(6, expected_send_count)
    )
    return ok, {
        "before_file_ok": before_file_ok,
        "after_file_ok": after_file_ok,
        "canonical_status_sources": (
            canonical_source(before_row) and canonical_source(after_row)
        ),
        "successful_send_count": successful_send_count,
        "expected_send_count": expected_send_count,
        "counter_deltas": deltas,
        "report_exact": report_exact,
        "binding_exact": binding_exact,
        "flow_ok": flow_ok,
        "remote_mode": remote_mode,
        "remote_status_validation_ok": remote_validation_ok,
        "remote_before_validation": before_validation,
        "remote_after_validation": after_validation,
    }


def soak_gate(
    active_path: Path | None,
    idle_path: Path | None,
    root: Path,
    commit: str,
    sd_history_mode: str,
    run_id: str,
    run_attempt: str,
    rf_path: Path | None,
) -> CoreGate:
    active = read_json(active_path)
    idle = read_json(idle_path)
    active_ok = soak_artifact_ok(
        active,
        commit=commit,
        sd_history_mode=sd_history_mode,
        active=True,
        run_id=run_id,
        run_attempt=run_attempt,
    )
    idle_ok = soak_artifact_ok(
        idle,
        commit=commit,
        sd_history_mode=sd_history_mode,
        active=False,
        run_id=run_id,
        run_attempt=run_attempt,
    )
    rf = read_json(rf_path)
    peer_flow_ok, peer_flow_details = active_soak_peer_flow_ok(
        active, rf, root
    )
    linked_rf_path, linked_rf_ok = _verified_file_row(
        active.get("controlled_peer_receipt"), root
    )
    rf_binding_ok = (
        linked_rf_ok
        and linked_rf_path is not None
        and rf_path is not None
        and linked_rf_path == rf_path.resolve()
        and rf_gate(
            rf_path,
            root,
            commit,
            sd_history_mode,
            run_id,
            run_attempt,
        ).ok
        and active.get("active_dm_fingerprint")
        == rf.get("target_fingerprint")
        and isinstance(rf.get("controlled_peer"), dict)
        and active.get("active_dm_fingerprint")
        == rf["controlled_peer"].get("fingerprint")
        and idle.get("controlled_peer_receipt") is None
        and peer_flow_ok
    )
    active_health = _health_rows(active)
    idle_health = _health_rows(idle)
    seam_boot_nonce_ok = (
        bool(active_health)
        and bool(idle_health)
        and active_health[-1].get("boot_nonce") != 0
        and active_health[-1].get("boot_nonce")
        == idle_health[0].get("boot_nonce")
    )
    active_final_uptime = (
        active_health[-1].get("uptime_ms") if active_health else None
    )
    idle_first_uptime = (
        idle_health[0].get("uptime_ms") if idle_health else None
    )
    seam_uptime_ok = (
        isinstance(active_final_uptime, int)
        and not isinstance(active_final_uptime, bool)
        and isinstance(idle_first_uptime, int)
        and not isinstance(idle_first_uptime, bool)
        and idle_first_uptime >= active_final_uptime
    )
    try:
        active_end = datetime.fromisoformat(
            str(active.get("ended_at")).replace("Z", "+00:00")
        )
        idle_start = datetime.fromisoformat(
            str(idle.get("started_at")).replace("Z", "+00:00")
        )
        gap = (idle_start - active_end).total_seconds()
        contiguous = 0 <= gap <= 300
    except (TypeError, ValueError):
        gap = None
        contiguous = False
    return CoreGate(
        "core_90_minute_soak",
        active_ok
        and idle_ok
        and contiguous
        and seam_boot_nonce_ok
        and seam_uptime_ok
        and rf_binding_ok,
        "60-minute active DM soak followed by 30-minute idle soak passes",
        path_text(active_path, root) + path_text(idle_path, root),
        {
            "active_ok": active_ok,
            "idle_ok": idle_ok,
            "active_to_idle_gap_sec": gap,
            "active_idle_boot_nonce_continuity": seam_boot_nonce_ok,
            "active_idle_uptime_continuity": seam_uptime_ok,
            "controlled_peer_rf_receipt_binding": rf_binding_ok,
            "controlled_peer_flow": peer_flow_details,
            "active_final_uptime_ms": active_final_uptime,
            "idle_first_uptime_ms": idle_first_uptime,
        },
    )


def sd_decision_gate(
    path: Path | None,
    root: Path,
    commit: str,
    run_id: str,
    sd_history_mode: str,
    disabled_smoke_path: Path | None = None,
) -> CoreGate:
    if sd_history_mode == "disabled":
        smoke = read_json(disabled_smoke_path)
        results = smoke.get("results")
        results = results if isinstance(results, list) else []
        storage_rows = [
            result
            for result in results
            if isinstance(result, dict)
            and result.get("cmd") == "storage status"
        ]
        storage_ok = (
            len(storage_rows) == 1
            and disabled_storage_status_ok(storage_rows[0])
        )
        smoke_identity_ok = (
            real_evidence(smoke)
            and smoke.get("kind") == "core_smoke"
            and smoke.get("mode") == "hardware"
            and smoke.get("port") == D1L_CORE_PORT
            and smoke.get("sd_history_mode") == "disabled"
            and exact_candidate_fields(smoke, commit)
        )
        return CoreGate(
            "sd_history_decision",
            storage_ok and smoke_identity_ok,
            "Exact-candidate storage status proves SD disabled and NVS authoritative",
            path_text(disabled_smoke_path, root),
            {
                "sd_history_mode": "disabled",
                "rp2040_payload_required": False,
                "nvs_authoritative": True,
                "raw_storage_status_ok": storage_ok,
                "exact_candidate_smoke": smoke_identity_ok,
            },
        )
    data = read_json(path)
    checks = data.get("checks")
    required = (
        "paired_artifacts_exact",
        "fat32_present_mounted_root_ready",
        "file_operations",
        "atomic_rename",
        "reboot_remount",
        "physical_removal_truthful",
        "physical_reinsertion_fresh",
        "file_canary",
        "retained_readback",
        "stable_30_minutes",
        "no_format",
    )
    ok = (
        real_evidence(data)
        and data.get("kind") == "core_sd_qualification"
        and data.get("mode") == "hardware"
        and data.get("ok") is True
        and data.get("physical_observed") is True
        and exact_sha(data.get("commit")) == commit
        and str(data.get("github_actions_run")) == str(run_id)
        and data.get("d1l_port") == D1L_CORE_PORT
        and data.get("rp2040_port") == "COM16"
        and isinstance(data.get("stable_duration_sec"), (int, float))
        and data["stable_duration_sec"] >= 1800
        and isinstance(checks, dict)
        and all(checks.get(name) is True for name in required)
        and data.get("formats_sd") is False
    )
    return CoreGate(
        "sd_history_decision",
        ok,
        "Optional SD history exact-candidate qualification passes",
        path_text(path, root),
        {"required_checks": list(required)},
    )


def install_review_gate(
    path: Path | None,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
    package: Path | None,
) -> CoreGate:
    data = read_json(path)
    if path is None or package is None:
        ok = False
        reasons = ["install_review_or_package_missing"]
        details: dict[str, Any] = {}
    else:
        ok, reasons, details = validate_install_review_receipt(
            path,
            root=root,
            package_dir=package,
            commit=commit,
            run_id=str(run_id),
            run_attempt=str(run_attempt),
        )
    return CoreGate(
        "install_recovery_review",
        ok,
        "Checksum, COM12 install, and recovery instructions receive operator review",
        path_text(path, root)
        + (
            path_text(package / "manifest.json", root)
            + path_text(package / "SHA256SUMS.txt", root)
            if package is not None
            else []
        ),
        {
            "reasons": reasons,
            "validation": details,
            "operator": data.get("operator"),
            "reviewer": data.get("reviewer"),
        },
    )


def defect_gate(
    path: Path | None,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
) -> CoreGate:
    if path is None or validate_core_github_defect_receipt is None:
        ok = False
        reasons = ["strict_r8_defect_validator_or_receipt_missing"]
        details: dict[str, Any] = {}
    else:
        try:
            evidence_ok, reasons, details = (
                validate_core_github_defect_receipt(
                    path,
                    root=root,
                    commit=commit,
                    run_id=str(run_id),
                    run_attempt=str(run_attempt),
                    max_age_sec=DEFECT_RECEIPT_MAX_AGE_SEC,
                    max_future_skew_sec=(
                        DEFECT_RECEIPT_MAX_FUTURE_SKEW_SEC
                    ),
                )
            )
        except (OSError, RuntimeError, TypeError, ValueError) as exc:
            evidence_ok = False
            reasons = [
                "strict_r8_defect_validator_failed:"
                f"{type(exc).__name__}"
            ]
            details = {}
        ok = (
            evidence_ok
            and isinstance(details, dict)
            and details.get("release_gate_ok") is True
        )
        if evidence_ok and not ok:
            reasons = list(reasons) + [
                "live_defect_release_gate_not_green"
            ]
    return CoreGate(
        "zero_core_p0_and_critical_p1",
        ok,
        "No Core P0 or Core crash/data-loss/security P1 remains",
        path_text(path, root),
        {
            "reasons": reasons,
            "validation": details,
        },
    )


def _resolve(root: Path, value: str | None) -> Path | None:
    if not value:
        return None
    path = Path(value)
    return path.resolve() if path.is_absolute() else (root / path).resolve()


def newest(root: Path, *patterns: str) -> Path | None:
    candidates = []
    for pattern in patterns:
        candidates.extend(path for path in root.glob(pattern) if path.is_file())
    return max(candidates, key=lambda path: path.stat().st_mtime) if candidates else None


def evidence_path(
    root: Path,
    explicit: str | None,
    search_root: Path,
    *patterns: str,
) -> Path | None:
    return _resolve(root, explicit) if explicit else newest(search_root, *patterns)


def build_audit(args: argparse.Namespace) -> dict:
    root = Path(args.root).resolve()
    commit = exact_sha(args.commit)
    if commit is None:
        raise ValueError("--commit must be an exact 40-character hexadecimal SHA")
    if (
        not str(args.github_run_id).isdigit()
        or int(args.github_run_id) < 1
    ):
        raise ValueError("--github-run-id must be a positive integer")
    if (
        not str(args.github_run_attempt).isdigit()
        or int(args.github_run_attempt) < 1
    ):
        raise ValueError("--github-run-attempt must be a positive integer")
    if args.sd_history_mode not in FINAL_SD_HISTORY_MODES:
        raise ValueError(
            "Final Core audit requires SD history disabled with NVS authoritative"
        )
    if args.d1l_port.upper() != D1L_CORE_PORT:
        raise ValueError(f"Core audit requires {D1L_CORE_PORT}")

    github_run_dir = (
        _resolve(root, args.github_run_dir)
        if args.github_run_dir
        else root / "artifacts" / "github" / str(args.github_run_id)
    )
    hardware_dir = _resolve(root, args.hardware_dir) or root / "artifacts" / "hardware" / "com12"
    soak_dir = _resolve(root, args.soak_dir) or root / "artifacts" / "soak"
    package = find_release_package(github_run_dir)

    paths = {
        "actions_run": evidence_path(
            root,
            args.actions_run_receipt,
            github_run_dir / "core-actions-run-metadata",
            "core_actions_run*.json",
        ),
        "core_smoke": evidence_path(
            root, args.core_smoke, hardware_dir, "core_smoke*.json"
        ),
        "core_ui": evidence_path(
            root,
            args.core_ui,
            hardware_dir,
            "core_ui_corruption_probe*.json",
        ),
        "manual_review": evidence_path(
            root,
            args.manual_review,
            hardware_dir,
            "core_manual_ui_review*.json",
        ),
        "reboot_receipt": evidence_path(
            root,
            args.reboot_receipt,
            hardware_dir,
            "core_reboot_persistence*.json",
        ),
        "protocol_migration": evidence_path(
            root,
            args.protocol_migration_receipt,
            hardware_dir,
            "time_protocol_migration*.json",
        ),
        "rf_receipt": evidence_path(
            root,
            args.rf_receipt,
            hardware_dir,
            "rf_full_acceptance*.json",
        ),
        "active_soak": evidence_path(
            root, args.active_soak, soak_dir, "core_active_60m*.json"
        ),
        "idle_soak": evidence_path(
            root, args.idle_soak, soak_dir, "core_idle_30m*.json"
        ),
        "sd_receipt": evidence_path(
            root,
            args.sd_receipt,
            hardware_dir,
            "core_sd_qualification*.json",
        ),
        "install_review": evidence_path(
            root,
            args.install_review,
            hardware_dir,
            "core_install_recovery_review*.json",
        ),
        "defect_receipt": evidence_path(
            root,
            args.defect_receipt,
            hardware_dir,
            "core_github_defect_snapshot*.json",
        ),
    }

    gates: list[CoreGate] = [
        audit_runner_source_gate(root, commit),
        actions_inventory_gate(github_run_dir, root),
        actions_run_metadata_gate(
            paths["actions_run"],
            github_run_dir,
            root,
            commit,
            str(args.github_run_id),
            str(args.github_run_attempt),
        ),
        core_immutable_source_inputs_gate(
            github_run_dir,
            root,
            commit,
            str(args.github_run_id),
            args.sd_history_mode,
        ),
        imported_gate(
            host_checks_success_gate(
                github_run_dir, root, commit, str(args.github_run_id)
            )
        ),
        imported_gate(checksum_gate(github_run_dir, root)),
        imported_gate(
            meshcore_conformance_evidence_gate(
                github_run_dir,
                root,
                commit,
                expected_run_id=str(args.github_run_id),
            )
        ),
        imported_gate(
            meshcore_signed_advert_evidence_gate(
                github_run_dir,
                root,
                commit,
                expected_run_id=str(args.github_run_id),
            )
        ),
        package_gate(
            github_run_dir,
            root,
            commit,
            str(args.github_run_id),
            str(args.github_run_attempt),
            args.sd_history_mode,
        ),
        imported_gate(notices_gate(github_run_dir, root)),
        core_flash_receipt_gate(
            hardware_dir,
            github_run_dir,
            root,
            commit,
            str(args.github_run_id),
            str(args.github_run_attempt),
        ),
        core_smoke_gate(
            paths["core_smoke"],
            root,
            commit,
            args.sd_history_mode,
            str(args.github_run_id),
            str(args.github_run_attempt),
        ),
        core_ui_gate(
            paths["core_ui"],
            root,
            commit,
            args.sd_history_mode,
            str(args.github_run_id),
            str(args.github_run_attempt),
        ),
        manual_review_gate(
            paths["manual_review"],
            root,
            commit,
            str(args.github_run_id),
            str(args.github_run_attempt),
            paths["core_ui"],
        ),
        reboot_persistence_gate(
            paths["reboot_receipt"],
            root,
            commit,
            args.sd_history_mode,
            str(args.github_run_id),
            str(args.github_run_attempt),
        ),
        protocol_migration_gate(
            paths["protocol_migration"],
            root,
            commit,
            str(args.github_run_id),
            str(args.github_run_attempt),
        ),
        rf_gate(
            paths["rf_receipt"],
            root,
            commit,
            args.sd_history_mode,
            str(args.github_run_id),
            str(args.github_run_attempt),
        ),
        sd_decision_gate(
            paths["sd_receipt"],
            root,
            commit,
            str(args.github_run_id),
            args.sd_history_mode,
            paths["core_smoke"],
        ),
        soak_gate(
            paths["active_soak"],
            paths["idle_soak"],
            root,
            commit,
            args.sd_history_mode,
            str(args.github_run_id),
            str(args.github_run_attempt),
            paths["rf_receipt"],
        ),
        install_review_gate(
            paths["install_review"],
            root,
            commit,
            str(args.github_run_id),
            str(args.github_run_attempt),
            package,
        ),
        defect_gate(
            paths["defect_receipt"],
            root,
            commit,
            str(args.github_run_id),
            str(args.github_run_attempt),
        ),
    ]
    failed = [gate for gate in gates if not gate.ok]
    p0_failed = [gate for gate in failed if gate.severity == "P0"]
    core_release_ready = not failed
    return {
        "schema": CORE_AUDIT_SCHEMA,
        "kind": "core_release_gate_audit",
        "mode": "core-release-gate-audit",
        "created_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "release_profile": CORE_RELEASE_PROFILE,
        "commit": commit,
        "github_actions_run": str(args.github_run_id),
        "github_actions_run_attempt": int(args.github_run_attempt),
        "workflow_run_attempt": int(args.github_run_attempt),
        "github_run_dir": str(github_run_dir),
        "d1l_port": D1L_CORE_PORT,
        "sd_history_mode": args.sd_history_mode,
        "git": git_metadata(root),
        "core_release_ready": core_release_ready,
        "full_feature_release_ready": False,
        "gate_count": len(gates),
        "passed_count": len(gates) - len(failed),
        "failed_count": len(failed),
        "p0_failed_count": len(p0_failed),
        "gates": [gate.to_dict() for gate in gates],
        "evidence_paths": {
            name: str(path) if path is not None else None
            for name, path in paths.items()
        },
    }


def dry_run_report(args: argparse.Namespace) -> dict:
    """Return a CI-safe plan that can never close a physical release gate."""
    return {
        "schema": CORE_AUDIT_SCHEMA,
        "kind": "core_release_gate_audit",
        "mode": "dry-run",
        "dry_run": True,
        "planning_only": True,
        "ok": False,
        "closure_eligible": False,
        "hardware_required": True,
        "physical_observed": False,
        "release_profile": CORE_RELEASE_PROFILE,
        "commit": exact_sha(args.commit),
        "github_actions_run": (
            str(args.github_run_id) if args.github_run_id else None
        ),
        "github_actions_run_attempt": (
            int(str(args.github_run_attempt))
            if str(args.github_run_attempt or "").isdigit()
            and int(str(args.github_run_attempt)) >= 1
            else None
        ),
        "workflow_run_attempt": (
            int(str(args.github_run_attempt))
            if str(args.github_run_attempt or "").isdigit()
            and int(str(args.github_run_attempt)) >= 1
            else None
        ),
        "d1l_port": D1L_CORE_PORT,
        "sd_history_mode": args.sd_history_mode or "disabled",
        "core_release_ready": False,
        "full_feature_release_ready": False,
        "gate_count": 0,
        "passed_count": 0,
        "failed_count": 0,
        "p0_failed_count": 0,
        "physical_gates_executed": [],
        "note": (
            "Planning-only host CI invocation. Exact Actions artifacts and "
            "physical COM12 evidence are required for a closing audit."
        ),
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--github-run-id")
    parser.add_argument("--github-run-attempt")
    parser.add_argument("--github-run-dir")
    parser.add_argument("--commit")
    parser.add_argument("--d1l-port", default=D1L_CORE_PORT)
    parser.add_argument(
        "--sd-history-mode",
        choices=sorted(FINAL_SD_HISTORY_MODES),
    )
    parser.add_argument("--hardware-dir", default="artifacts/hardware/com12")
    parser.add_argument("--soak-dir", default="artifacts/soak")
    parser.add_argument("--actions-run-receipt")
    parser.add_argument("--core-smoke")
    parser.add_argument("--core-ui")
    parser.add_argument("--manual-review")
    parser.add_argument("--reboot-receipt")
    parser.add_argument("--protocol-migration-receipt")
    parser.add_argument("--rf-receipt")
    parser.add_argument("--active-soak")
    parser.add_argument("--idle-soak")
    parser.add_argument("--sd-receipt")
    parser.add_argument("--install-review")
    parser.add_argument("--defect-receipt")
    parser.add_argument("--out")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    if args.dry_run:
        report = dry_run_report(args)
    else:
        try:
            report = build_audit(args)
        except ValueError as exc:
            print(str(exc), file=sys.stderr)
            return 2
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        out = Path(args.out)
        if not out.is_absolute():
            out = Path(args.root).resolve() / out
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if args.dry_run or report["core_release_ready"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
