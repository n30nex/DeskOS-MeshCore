#!/usr/bin/env python3
"""Evidence-based public release gate audit for MeshCore DeskOS D1L."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path, PurePosixPath
from typing import Any

try:
    from verify_checksums import is_link_or_reparse, verify_sha256_manifest
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.verify_checksums import (
        is_link_or_reparse,
        verify_sha256_manifest,
    )

try:
    from rf_full_acceptance_d1l import (
        LOCAL_PEER_EVIDENCE_SOURCE,
        MESHCOREBOT_PEER_CONTROL_SOCKET,
        MESHCOREBOT_PEER_DEVICE,
        MESHCOREBOT_PEER_FINGERPRINT,
        MESHCOREBOT_PEER_PROFILE,
        MESHCOREBOT_PEER_PUBLIC_KEY,
        MESHCOREBOT_PEER_STATUS_PATH,
        LOCAL_PEER_CONTROL_REQUEST_TRANSPORT,
        LOCAL_PEER_CONTROL_RESPONSE_TRANSPORT,
        REMOTE_PEER_HOSTNAME,
        remote_control_request,
        remote_control_semantic_ok,
        remote_peer_report_shape_ok as pinned_peer_report_shape_ok,
        validate_remote_control_exchange,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.rf_full_acceptance_d1l import (
        LOCAL_PEER_EVIDENCE_SOURCE,
        MESHCOREBOT_PEER_CONTROL_SOCKET,
        MESHCOREBOT_PEER_DEVICE,
        MESHCOREBOT_PEER_FINGERPRINT,
        MESHCOREBOT_PEER_PROFILE,
        MESHCOREBOT_PEER_PUBLIC_KEY,
        MESHCOREBOT_PEER_STATUS_PATH,
        LOCAL_PEER_CONTROL_REQUEST_TRANSPORT,
        LOCAL_PEER_CONTROL_RESPONSE_TRANSPORT,
        REMOTE_PEER_HOSTNAME,
        remote_control_request,
        remote_control_semantic_ok,
        remote_peer_report_shape_ok as pinned_peer_report_shape_ok,
        validate_remote_control_exchange,
    )

try:
    from d1l_serial_target import POSIX_D1L_TARGET
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.d1l_serial_target import POSIX_D1L_TARGET

try:
    from meshcore_conformance_d1l import (
        CANONICAL_EVIDENCE_PROFILE,
        canonicalize_release_report,
        validate_completed_report,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.meshcore_conformance_d1l import (
        CANONICAL_EVIDENCE_PROFILE,
        canonicalize_release_report,
        validate_completed_report,
    )

try:
    from meshcore_signed_advert_runtime_d1l import (
        SIGNED_ADVERT_ARTIFACT_TYPE,
        SIGNED_ADVERT_EVIDENCE_PROFILE,
        canonicalize_release_report as canonicalize_signed_advert_report,
        validate_completed_report as validate_signed_advert_report,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.meshcore_signed_advert_runtime_d1l import (
        SIGNED_ADVERT_ARTIFACT_TYPE,
        SIGNED_ADVERT_EVIDENCE_PROFILE,
        canonicalize_release_report as canonicalize_signed_advert_report,
        validate_completed_report as validate_signed_advert_report,
    )

try:
    from verify_arduino_build_inputs import validate_build_receipt
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.verify_arduino_build_inputs import validate_build_receipt

try:
    from sd_reboot_remount_acceptance_d1l import (
        crashlog_transition_passed,
        persistence_readback_commands,
        persistence_snapshot_clean,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.sd_reboot_remount_acceptance_d1l import (
        crashlog_transition_passed,
        persistence_readback_commands,
        persistence_snapshot_clean,
    )

try:
    from wp01_evidence_aggregate_d1l import (
        WP01_ARTIFACT_KINDS,
        WP01_ARTIFACT_PATTERNS,
        WP01_PROVENANCE_PATTERN,
        wp01_aggregate_artifact_ok,
        wp01_artifact_ok,
        wp01_provenance_receipt_ok,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.wp01_evidence_aggregate_d1l import (
        WP01_ARTIFACT_KINDS,
        WP01_ARTIFACT_PATTERNS,
        WP01_PROVENANCE_PATTERN,
        wp01_aggregate_artifact_ok,
        wp01_artifact_ok,
        wp01_provenance_receipt_ok,
    )


RELEASE_UI_CORRUPTION_MIN_ROUNDS = 20
RELEASE_GATE_AUDIT_SCHEMA = 2
BUILD_INPUTS_SCHEMA = 1
BUILD_INPUTS_KIND = "d1l_build_inputs"
ARDUINO_BUILD_INPUTS_KIND = "d1l_arduino_build_inputs"
SUPPORTED_ESP_IDF_IMAGE_TAG = "espressif/idf:v5.5.4"
SUPPORTED_ESP_IDF_IMAGE_DIGEST = (
    "sha256:b9f2d6ea1c19e0c9f7959bdb74a9e3c775642f9d0f3b841937c5fa3363db892b"
)
SUPPORTED_ESP_IDF_IMAGE = f"{SUPPORTED_ESP_IDF_IMAGE_TAG}@{SUPPORTED_ESP_IDF_IMAGE_DIGEST}"
SUPPORTED_ESP_IDF_BUILD_INPUTS = ".github/d1l-build-inputs.json"
SUPPORTED_ESP_IDF_VERSION = "v5.5.4"
SUPPORTED_ESP_IDF_LOCK_VERSION = "5.5.4"
ESP_IDF_MIGRATION_ISSUE = 63
MESHCORE_CONFORMANCE_ARTIFACT_TYPE = "d1l_meshcore_wire_conformance"
MESHCORE_CONFORMANCE_BOUNDARY = "wire_envelope_only"
MESHCORE_CONFORMANCE_MAX_AGE_DAYS = 14
MESHCORE_CONFORMANCE_CLOCK_SKEW_MINUTES = 5
MESHCORE_CONFORMANCE_ACTIONS_ARTIFACT = "d1l-meshcore-wire-conformance"
MESHCORE_SIGNED_ADVERT_ACTIONS_ARTIFACT = MESHCORE_CONFORMANCE_ACTIONS_ARTIFACT
HOST_CHECKS_ARTIFACT_TYPE = "d1l_host_checks_success"
REQUIRED_ESP32_FLASH_ROLES = {
    0x0: "build/bootloader/bootloader.bin",
    0x8000: "build/partition_table/partition-table.bin",
    0xF000: "build/ota_data_initial.bin",
    0x20000: "build/meshcore_deskos_d1l.bin",
}
REQUIRED_UI_TELEMETRY_FIELDS = {
    "heap_free",
    "heap_min_free",
    "heap_largest_free",
    "lvgl_free_bytes",
    "lvgl_largest_free_bytes",
    "lvgl_used_pct",
    "ui_task_stack_free_words",
    "reset_reason",
}
REQUIRED_SCROLL_SURFACES = {
    "home": "home",
    "public_messages": "messages",
    "dm_thread": "messages",
    "nodes": "nodes",
    "packets": "packets",
    "settings": "settings",
    "storage": "settings",
    "wifi": "settings",
    "map": "map",
    "map_options": "map",
    "map_location": "map",
    "map_cache": "map",
}
REQUIRED_SCROLL_SCREENS = set(REQUIRED_SCROLL_SURFACES)
SCROLL_MOVEMENT_OPTIONAL = {
    "home",
    "storage",
    "map",
    "map_options",
    "map_location",
    "map_cache",
}
REQUIRED_COMPOSE_CAPTURE_TARGETS = {
    "public",
    "public-long",
    "dm",
    "dm-long",
    "public-search",
    "dm-search",
    "packet-search",
    "contact-edit",
    "onboarding",
    "map-location",
    "wifi-ssid",
    "wifi-password",
}
REQUIRED_NOTICE_FILES = {
    "notices/LICENSE",
    "notices/THIRD_PARTY_NOTICES.md",
    "notices/ATTRIBUTIONS.md",
    "notices/SOURCE_AUDIT_AND_ATTRIBUTION.md",
    "notices/ORLP_ED25519_ZLIB_LICENSE.txt",
}
FULL_SOAK_SECONDS = 12 * 60 * 60
MAX_FULL_SOAK_SAMPLE_INTERVAL_SECONDS = 5 * 60
REQUIRED_FULL_SOAK_COMMANDS = {
    "health",
    "mesh status",
    "signal",
    "messages unread",
    "packets",
    "crashlog",
}
ALLOWED_FULL_SOAK_COMMANDS = REQUIRED_FULL_SOAK_COMMANDS | {
    "storage status",
    "storage filecanary",
}
REQUIRED_ROUTE_PROBE_CHECKS = {
    "trace_reports_active_probe",
    "probe_queued",
    "probe_is_dm_rf",
    "probe_not_public_rf",
    "token_generated",
    "packets_search_has_token",
    "messages_dm_has_token",
    "routes_trace_has_probe",
    "health_ready",
}
SAFE_SD_NEXT_ACTION = "confirm_fat32_card_or_inspect_rp2040_sd_mount_path"
OBSOLETE_SD_NEXT_ACTION_TOKENS = ("format", "mkfs", "erase", "wipe")
SD_BOOT_PREPARE_SCENARIOS = (
    "no-card",
    "correct-structure",
    "missing-structure",
    "unformatted",
    "existing-data",
    "rp2040-unavailable",
)
SD_BOOT_PREPARE_EXPECTED_CLASSIFICATIONS = {
    "no-card": {"no_card_fallback"},
    "correct-structure": {"ready_sd_file_gate"},
    "missing-structure": {"ready_sd_file_gate"},
    "unformatted": {"computer_fat32_required", "firmware_mount_error_fallback"},
    "existing-data": {
        "existing_data_preserved_ready",
        "existing_data_firmware_mount_fallback",
        "existing_data_not_wiped",
    },
    "rp2040-unavailable": {"bridge_unavailable_fallback"},
}
RAW_DIAG_PROBE_PREFIXES = ("hd", "hs", "ld", "ls", "bb", "bi", "bs", "bcm", "bsc")
RAW_DIAG_PROBE_SUFFIXES = (
    "_p",
    "_e",
    "_d",
    "_c0r",
    "_c8r",
    "_c0",
    "_c8",
    "_miso_pull",
    "_miso_spi",
    "_miso_idle",
)
UNSAFE_SD_COMMAND_TOKENS = ("setup confirm", "storage format", "format confirm", "mkfs", "erase sd", "wipe sd")
SD_FILE_LIMITS = {
    "file_line_max": 512,
    "file_chunk_max": 192,
    "path_max": 96,
}
TOP_LEVEL_COMMIT_FIELDS = (
    "commit",
    "commit_sha",
    "commitSha",
    "head_sha",
    "headSha",
    "head_sha256",
    "git_commit",
    "gitCommit",
    "git_sha",
    "gitSha",
    "source_commit",
    "sourceCommit",
    "source_head_sha",
    "sourceHeadSha",
    "firmware_commit",
    "firmwareCommit",
    "firmware_head_sha",
    "firmwareHeadSha",
    "firmware_git_sha",
    "firmwareGitSha",
    "build_commit",
    "buildCommit",
    "build_head_sha",
    "buildHeadSha",
    "workflow_sha",
    "workflowSha",
    "github_sha",
    "githubSha",
)
NESTED_COMMIT_FIELDS = (
    "commit",
    "commit_sha",
    "commitSha",
    "head_sha",
    "headSha",
    "head_sha256",
    "git_commit",
    "gitCommit",
    "git_sha",
    "gitSha",
    "source_commit",
    "sourceCommit",
    "source_head_sha",
    "sourceHeadSha",
    "firmware_commit",
    "firmwareCommit",
    "firmware_head_sha",
    "firmwareHeadSha",
    "firmware_git_sha",
    "firmwareGitSha",
    "build_commit",
    "buildCommit",
    "build_head_sha",
    "buildHeadSha",
)
COMMIT_METADATA_CONTAINERS = ("git", "artifact", "firmware", "build", "source", "workflow", "github", "metadata")
GENERIC_SHA_COMMIT_CONTAINERS = {"git", "workflow", "github"}
FORBIDDEN_PORTS = {"COM" + str(number) for number in (8, 11, 29)}
REQUIRED_DM_PROBE_CHECKS = {
    "controlled_peer_status_connected",
    "send_ok",
    "messages_dm_has_token",
    "packets_search_has_token",
    "route_trace_has_target",
    "controlled_peer_rx_contact_delta",
    "health_ready",
    "no_public_commands",
}
REQUIRED_FULL_RF_CHECKS = {
    "identity_public_key_matches",
    "controlled_peer_observed",
    "outbound_dm",
    "inbound_dm",
    "ack_path",
    "direct_route",
    "health_ready",
    "no_public_commands",
}


def default_d1l_port() -> str:
    return "COM" + "12"


def default_rp2040_port() -> str:
    return "COM" + "16"


def default_hardware_dir() -> str:
    return str(Path("artifacts") / "hardware" / default_d1l_port().lower())


def default_rp2040_hardware_dir() -> str:
    return str(Path("artifacts") / "hardware" / default_rp2040_port().lower())


def default_wp01_dir() -> str:
    return str(Path("artifacts") / "hardware" / "wp01")


def normalize_port(value: object) -> str | None:
    if not isinstance(value, str) or not value.strip():
        return None
    normalized = value.strip().upper().replace("/", "\\")
    for prefix in ("\\\\.\\", "\\\\?\\"):
        if normalized.startswith(prefix):
            normalized = normalized[len(prefix):]
            break
    return normalized


def allowed_port(value: object) -> bool:
    if value == POSIX_D1L_TARGET:
        return True
    normalized = normalize_port(value)
    return (
        normalized is not None
        and re.fullmatch(r"COM[1-9][0-9]*", normalized) is not None
        and normalized not in FORBIDDEN_PORTS
    )


@dataclass
class GateResult:
    gate_id: str
    severity: str
    ok: bool
    title: str
    evidence: list[str]
    message: str
    details: dict[str, Any] | None = None

    def to_dict(self) -> dict:
        payload = {
            "id": self.gate_id,
            "severity": self.severity,
            "ok": self.ok,
            "title": self.title,
            "evidence": self.evidence,
            "message": self.message,
        }
        if self.details:
            payload["details"] = self.details
        return payload


def read_json(path: Path | None) -> dict:
    if not path or not path.is_file():
        return {}
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def rel(path: Path, root: Path) -> str:
    try:
        return str(path.resolve().relative_to(root.resolve())).replace("\\", "/")
    except ValueError:
        return str(path)


def newest_file(root: Path, *patterns: str) -> Path | None:
    files: list[Path] = []
    for pattern in patterns:
        files.extend(path for path in root.glob(pattern) if path.is_file())
    if not files:
        return None
    return max(files, key=lambda path: path.stat().st_mtime)


def commit_file(root: Path, commit: str | None, *patterns: str) -> Path | None:
    if not commit:
        return newest_file(root, *patterns)
    short = commit[:7]
    commit_patterns = [pattern.replace("*", f"*{short}*", 1) for pattern in patterns]
    return newest_file(root, *commit_patterns)


def artifact_commit_values(data: dict) -> list[str]:
    if not isinstance(data, dict):
        return []
    values: list[Any] = [
        data.get(field)
        for field in TOP_LEVEL_COMMIT_FIELDS
    ]
    for container_name in COMMIT_METADATA_CONTAINERS:
        container = data.get(container_name)
        if isinstance(container, dict):
            values.extend(container.get(field) for field in NESTED_COMMIT_FIELDS)
            if container_name in GENERIC_SHA_COMMIT_CONTAINERS:
                values.append(container.get("sha"))
    return [value.strip() for value in values if isinstance(value, str) and value.strip()]


def commit_value_matches(value: str, commit: str) -> bool:
    full = commit.lower()
    lowered = value.lower()
    return (
        re.fullmatch(r"[0-9a-f]{40}", full) is not None
        and re.fullmatch(r"[0-9a-f]{7,40}", lowered) is not None
        and len(lowered) <= len(full)
        and full.startswith(lowered)
    )


def artifact_commit_matches(path: Path, data: dict, commit: str | None) -> bool:
    if not commit:
        return True
    metadata_values = artifact_commit_values(data)
    return bool(metadata_values) and all(
        commit_value_matches(value, commit) for value in metadata_values
    )


def newest_commit_json(root: Path, commit: str | None, *patterns: str) -> Path | None:
    files: list[Path] = []
    for pattern in patterns:
        files.extend(path for path in root.glob(pattern) if path.is_file())
    for candidate in sorted(set(files), key=lambda path: path.stat().st_mtime, reverse=True):
        if artifact_commit_matches(candidate, read_json(candidate), commit):
            return candidate
    return None


def unique_dirs(paths: list[Path]) -> list[Path]:
    seen: set[str] = set()
    unique: list[Path] = []
    for path in paths:
        try:
            key = str(path.resolve()).lower()
        except OSError:
            key = str(path).lower()
        if key in seen:
            continue
        seen.add(key)
        unique.append(path)
    return unique


def sd_artifact_roots(
    root: Path,
    github_run_dir: Path | None,
    hardware_dir: Path,
    *artifact_dirs: str,
) -> list[Path]:
    roots = [hardware_dir]
    for artifact_dir in artifact_dirs:
        roots.append(root / "artifacts" / artifact_dir)
        if github_run_dir:
            roots.append(github_run_dir / "d1l-host-artifacts" / artifact_dir)
    return unique_dirs(roots)


def newest_commit_json_from_roots(roots: list[Path], commit: str | None, *patterns: str) -> Path | None:
    files: list[Path] = []
    for root in roots:
        for pattern in patterns:
            files.extend(path for path in root.glob(pattern) if path.is_file())
    for candidate in sorted(set(files), key=lambda path: path.stat().st_mtime, reverse=True):
        if artifact_commit_matches(candidate, read_json(candidate), commit):
            return candidate
    return None


def find_release_package(github_run_dir: Path | None) -> Path | None:
    if not github_run_dir:
        return None
    package_root = github_run_dir / "d1l-release-package"
    if not package_root.is_dir():
        return None
    candidates = [path for path in package_root.iterdir() if path.is_dir() and path.name.startswith("d1l-release-")]
    if not candidates:
        return None
    return max(candidates, key=lambda path: path.stat().st_mtime)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_utc_timestamp(value: object) -> datetime | None:
    if not isinstance(value, str) or not value.strip():
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (ValueError, OverflowError):
        return None
    if parsed.tzinfo is None:
        return None
    try:
        return parsed.astimezone(timezone.utc)
    except (ValueError, OverflowError):
        return None


def meshcore_conformance_evidence_gate(
    github_run_dir: Path | None,
    root: Path,
    commit: str | None,
    now: datetime | None = None,
    expected_run_id: str | None = None,
) -> GateResult:
    """Bind a fresh Actions receipt to deterministic packaged evidence."""

    def resolve_inside(
        base: Path | None,
        relative: object,
        *,
        missing_code: str,
        outside_code: str,
        unresolvable_code: str,
    ) -> Path | None:
        if base is None or not isinstance(relative, str) or not relative:
            failures.append(missing_code)
            return None
        try:
            candidate = (base / relative).resolve()
            candidate.relative_to(base.resolve())
        except ValueError:
            failures.append(outside_code)
            return None
        except (OSError, RuntimeError):
            failures.append(unresolvable_code)
            return None
        return candidate

    def json_report(path: Path | None, invalid_code: str) -> dict[str, Any]:
        try:
            value = read_json(path)
        except (OSError, UnicodeError, json.JSONDecodeError):
            failures.append(invalid_code)
            return {}
        if not isinstance(value, dict):
            failures.append(invalid_code)
            return {}
        return value

    def file_identity(
        path: Path | None, hash_code: str, stat_code: str
    ) -> tuple[str | None, int | None]:
        if path is None or not path.is_file():
            return None, None
        try:
            digest = sha256_file(path)
        except OSError:
            failures.append(hash_code)
            digest = None
        try:
            size = path.stat().st_size
        except OSError:
            failures.append(stat_code)
            size = None
        return digest, size

    package = find_release_package(github_run_dir)
    manifest_path = package / "manifest.json" if package else None
    failures: list[str] = []
    manifest = json_report(manifest_path, "manifest_json_invalid")
    metadata = manifest.get("meshcore_conformance")
    metadata = metadata if isinstance(metadata, dict) else {}
    run_receipt = metadata.get("run_receipt")
    run_receipt = run_receipt if isinstance(run_receipt, dict) else {}
    if not package:
        failures.append("release_package_missing")
    if not commit:
        failures.append("expected_commit_missing")
    if github_run_dir is not None and not expected_run_id:
        failures.append("expected_run_id_missing")
    if not metadata:
        failures.append("manifest_metadata_missing")

    evidence_path = resolve_inside(
        package,
        metadata.get("path"),
        missing_code="evidence_path_missing",
        outside_code="evidence_path_outside_package",
        unresolvable_code="evidence_path_unresolvable",
    )
    if evidence_path and not evidence_path.is_file():
        failures.append("evidence_file_missing")

    receipt_artifact = run_receipt.get("artifact")
    receipt_root = (
        github_run_dir / receipt_artifact
        if github_run_dir is not None
        and receipt_artifact == MESHCORE_CONFORMANCE_ACTIONS_ARTIFACT
        else None
    )
    run_receipt_path = resolve_inside(
        receipt_root,
        run_receipt.get("path"),
        missing_code="run_receipt_path_missing",
        outside_code="run_receipt_path_outside_artifact",
        unresolvable_code="run_receipt_path_unresolvable",
    )
    if run_receipt_path and not run_receipt_path.is_file():
        failures.append("run_receipt_file_missing")

    canonical_report = json_report(evidence_path, "evidence_json_invalid")
    report = json_report(run_receipt_path, "run_receipt_json_invalid")
    signed_runtime_path = (
        receipt_root / f"meshcore_signed_advert_runtime_{commit}.json"
        if receipt_root is not None and commit
        else None
    )
    if signed_runtime_path is None or not signed_runtime_path.is_file():
        failures.append("signed_runtime_receipt_for_binding_missing")
        signed_runtime_report: dict[str, Any] = {}
    else:
        signed_runtime_report = json_report(
            signed_runtime_path,
            "signed_runtime_receipt_for_binding_invalid",
        )
    if report and commit:
        try:
            validate_completed_report(
                report,
                commit,
                build_inputs_path=root / SUPPORTED_ESP_IDF_BUILD_INPUTS,
                signed_advert_runtime_receipt=signed_runtime_report,
            )
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError):
            failures.append("run_receipt_semantics_incomplete")
    if canonical_report and commit:
        try:
            validate_completed_report(
                canonical_report,
                commit,
                build_inputs_path=root / SUPPORTED_ESP_IDF_BUILD_INPUTS,
                require_generated_at=False,
                signed_advert_runtime_receipt=signed_runtime_report,
            )
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError):
            failures.append("canonical_evidence_semantics_incomplete")
    source_verification = report.get("source_verification")
    source_commit = (
        source_verification.get("repository_commit")
        if isinstance(source_verification, dict)
        else None
    )
    package_git = manifest.get("git") if isinstance(manifest.get("git"), dict) else {}
    package_workflow = (
        manifest.get("workflow") if isinstance(manifest.get("workflow"), dict) else {}
    )
    expected_name = f"meshcore_conformance_{commit}.json" if commit else None
    if metadata:
        checks = {
            "metadata_artifact_type": metadata.get("artifact_type") == MESHCORE_CONFORMANCE_ARTIFACT_TYPE,
            "metadata_source_commit": metadata.get("source_commit") == commit,
            "metadata_evidence_profile": metadata.get("evidence_profile")
            == CANONICAL_EVIDENCE_PROFILE,
            "metadata_boundary": metadata.get("coverage_boundary") == MESHCORE_CONFORMANCE_BOUNDARY,
            "metadata_level": metadata.get("coverage_level") == MESHCORE_CONFORMANCE_BOUNDARY,
            "metadata_closure_ready_false": metadata.get("closure_ready") is False,
            "metadata_issue_65_false": metadata.get("issue_65_closure_eligible") is False,
            "metadata_passed": metadata.get("passed") is True,
            "metadata_execution_complete": metadata.get("execution_complete") is True,
            "metadata_max_age": metadata.get("max_age_days") == MESHCORE_CONFORMANCE_MAX_AGE_DAYS,
            "package_git_commit": package_git.get("commit") == commit,
            "package_git_clean": package_git.get("dirty") is False
            and package_git.get("dirty_entries") == [],
            "package_workflow_sha": package_workflow.get("sha") == commit,
            "package_workflow_run_id": bool(expected_run_id)
            and str(package_workflow.get("run_id")) == str(expected_run_id),
            "evidence_filename": bool(evidence_path and evidence_path.name == expected_name),
            "run_receipt_artifact": receipt_artifact
            == MESHCORE_CONFORMANCE_ACTIONS_ARTIFACT,
            "run_receipt_filename": bool(
                run_receipt_path and run_receipt_path.name == expected_name
            ),
        }
        failures.extend(name for name, ok in checks.items() if not ok)
    if report:
        checks = {
            "report_schema_version": report.get("schema_version") == 1,
            "report_artifact_type": report.get("artifact_type") == MESHCORE_CONFORMANCE_ARTIFACT_TYPE,
            "report_passed": report.get("passed") is True,
            "report_status": report.get("status") == "pass",
            "report_execution_complete": report.get("execution_complete") is True,
            "report_source_commit": source_commit == commit,
            "report_boundary": report.get("coverage_boundary") == MESHCORE_CONFORMANCE_BOUNDARY,
            "report_level": report.get("coverage_level") == MESHCORE_CONFORMANCE_BOUNDARY,
            "report_closure_ready_false": report.get("closure_ready") is False,
            "report_issue_65_false": report.get("issue_65_closure_eligible") is False,
        }
        failures.extend(name for name, ok in checks.items() if not ok)
    if canonical_report:
        checks = {
            "canonical_evidence_profile": canonical_report.get("evidence_profile")
            == CANONICAL_EVIDENCE_PROFILE,
            "canonical_evidence_matches_run_receipt": bool(report)
            and canonical_report == canonicalize_release_report(report),
        }
        failures.extend(name for name, ok in checks.items() if not ok)

    actual_sha, evidence_size = file_identity(
        evidence_path, "evidence_hash_unreadable", "evidence_stat_unreadable"
    )
    receipt_sha, receipt_size = file_identity(
        run_receipt_path,
        "run_receipt_hash_unreadable",
        "run_receipt_stat_unreadable",
    )
    if metadata and actual_sha != metadata.get("sha256"):
        failures.append("evidence_sha256_mismatch")
    if evidence_size is not None and metadata.get("size") != evidence_size:
        failures.append("evidence_size_mismatch")
    if run_receipt and receipt_sha != run_receipt.get("sha256"):
        failures.append("run_receipt_sha256_mismatch")
    if receipt_size is not None and run_receipt.get("size") != receipt_size:
        failures.append("run_receipt_size_mismatch")

    generated_at = parse_utc_timestamp(report.get("generated_at")) if report else None
    metadata_generated_at = (
        parse_utc_timestamp(run_receipt.get("generated_at")) if run_receipt else None
    )
    metadata_expires_at = (
        parse_utc_timestamp(run_receipt.get("expires_at")) if run_receipt else None
    )
    checked_at = (now or datetime.now(timezone.utc)).astimezone(timezone.utc)
    expected_expires_at = None
    expires_at_computable = generated_at is not None
    if generated_at is not None:
        try:
            expected_expires_at = generated_at + timedelta(
                days=MESHCORE_CONFORMANCE_MAX_AGE_DAYS
            )
        except OverflowError:
            expires_at_computable = False
    timestamp_checks = {
        "generated_at_valid": generated_at is not None,
        "expires_at_computable": expires_at_computable,
        "metadata_generated_at_matches": generated_at is not None and metadata_generated_at == generated_at,
        "metadata_expires_at_matches": expected_expires_at is not None
        and metadata_expires_at == expected_expires_at,
        "generated_at_not_future": generated_at is not None
        and generated_at <= checked_at + timedelta(minutes=MESHCORE_CONFORMANCE_CLOCK_SKEW_MINUTES),
        "evidence_not_expired": expected_expires_at is not None and checked_at < expected_expires_at,
    }
    failures.extend(name for name, ok in timestamp_checks.items() if not ok)
    failures = list(dict.fromkeys(failures))
    ok = not failures
    evidence = []
    if manifest_path and manifest_path.is_file():
        evidence.append(rel(manifest_path, root))
    if evidence_path and evidence_path.is_file():
        evidence.append(rel(evidence_path, root))
    if run_receipt_path and run_receipt_path.is_file():
        evidence.append(rel(run_receipt_path, root))
    return GateResult(
        "meshcore_wire_conformance_packaged",
        "P0",
        ok,
        "Current-commit MeshCore wire conformance packaged",
        evidence,
        (
            "Release package contains matching, unexpired wire_envelope_only evidence with "
            "closure_ready=false. This structural prerequisite does not close issue #65."
            if ok
            else "Release package is missing matching, unexpired MeshCore wire-envelope evidence; issue #65 remains open."
        ),
        {
            "failures": failures,
            "expected_commit": commit,
            "expected_run_id": expected_run_id,
            "package_run_id": package_workflow.get("run_id"),
            "package_git_dirty": package_git.get("dirty"),
            "package_git_dirty_entries": package_git.get("dirty_entries"),
            "source_commit": source_commit,
            "sha256": actual_sha,
            "run_receipt_sha256": receipt_sha,
            "generated_at": generated_at.isoformat() if generated_at else None,
            "expires_at": expected_expires_at.isoformat() if expected_expires_at else None,
            "checked_at": checked_at.isoformat(),
            "coverage_boundary": report.get("coverage_boundary") if report else None,
            "coverage_level": report.get("coverage_level") if report else None,
            "closure_ready": report.get("closure_ready") if report else None,
            "issue_65_closure_eligible": report.get("issue_65_closure_eligible") if report else None,
        },
    )


def meshcore_signed_advert_evidence_gate(
    github_run_dir: Path | None,
    root: Path,
    commit: str | None,
    now: datetime | None = None,
    expected_run_id: str | None = None,
) -> GateResult:
    """Bind the raw signed-advert Actions receipt to its packaged projection."""

    failures: list[str] = []

    def resolve_inside(
        base: Path | None,
        relative: object,
        *,
        missing_code: str,
        outside_code: str,
        unresolvable_code: str,
    ) -> Path | None:
        if base is None or not isinstance(relative, str) or not relative:
            failures.append(missing_code)
            return None
        try:
            candidate = (base / relative).resolve()
            candidate.relative_to(base.resolve())
        except ValueError:
            failures.append(outside_code)
            return None
        except (OSError, RuntimeError):
            failures.append(unresolvable_code)
            return None
        return candidate

    def json_report(path: Path | None, invalid_code: str) -> dict[str, Any]:
        try:
            value = read_json(path)
        except (OSError, UnicodeError, json.JSONDecodeError):
            failures.append(invalid_code)
            return {}
        if not isinstance(value, dict):
            failures.append(invalid_code)
            return {}
        return value

    def file_identity(
        path: Path | None, hash_code: str, stat_code: str
    ) -> tuple[str | None, int | None]:
        if path is None or not path.is_file():
            return None, None
        try:
            digest = sha256_file(path)
        except OSError:
            failures.append(hash_code)
            digest = None
        try:
            size = path.stat().st_size
        except OSError:
            failures.append(stat_code)
            size = None
        return digest, size

    package = find_release_package(github_run_dir)
    manifest_path = package / "manifest.json" if package else None
    manifest = json_report(manifest_path, "manifest_json_invalid")
    metadata = manifest.get("meshcore_signed_advert_runtime")
    metadata = metadata if isinstance(metadata, dict) else {}
    run_receipt = metadata.get("run_receipt")
    run_receipt = run_receipt if isinstance(run_receipt, dict) else {}
    if not package:
        failures.append("release_package_missing")
    if not commit:
        failures.append("expected_commit_missing")
    if github_run_dir is not None and not expected_run_id:
        failures.append("expected_run_id_missing")
    if not metadata:
        failures.append("manifest_metadata_missing")

    evidence_path = resolve_inside(
        package,
        metadata.get("path"),
        missing_code="evidence_path_missing",
        outside_code="evidence_path_outside_package",
        unresolvable_code="evidence_path_unresolvable",
    )
    if evidence_path and not evidence_path.is_file():
        failures.append("evidence_file_missing")

    receipt_artifact = run_receipt.get("artifact")
    receipt_root = (
        github_run_dir / receipt_artifact
        if github_run_dir is not None
        and receipt_artifact == MESHCORE_SIGNED_ADVERT_ACTIONS_ARTIFACT
        else None
    )
    run_receipt_path = resolve_inside(
        receipt_root,
        run_receipt.get("path"),
        missing_code="run_receipt_path_missing",
        outside_code="run_receipt_path_outside_artifact",
        unresolvable_code="run_receipt_path_unresolvable",
    )
    if run_receipt_path and not run_receipt_path.is_file():
        failures.append("run_receipt_file_missing")

    canonical_report = json_report(evidence_path, "evidence_json_invalid")
    report = json_report(run_receipt_path, "run_receipt_json_invalid")
    if report and commit:
        try:
            validate_signed_advert_report(report, commit)
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError):
            failures.append("run_receipt_semantics_incomplete")
    if canonical_report and commit:
        try:
            validate_signed_advert_report(
                canonical_report,
                commit,
                require_generated_at=False,
                require_commands=False,
            )
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError):
            failures.append("canonical_evidence_semantics_incomplete")

    repository = report.get("repository")
    repository = repository if isinstance(repository, dict) else {}
    source_commit = repository.get("repository_commit")
    package_git = (
        manifest.get("git") if isinstance(manifest.get("git"), dict) else {}
    )
    package_workflow = (
        manifest.get("workflow")
        if isinstance(manifest.get("workflow"), dict)
        else {}
    )
    expected_name = (
        f"meshcore_signed_advert_runtime_{commit}.json" if commit else None
    )
    if metadata:
        checks = {
            "metadata_artifact_type": metadata.get("artifact_type")
            == SIGNED_ADVERT_ARTIFACT_TYPE,
            "metadata_source_commit": metadata.get("source_commit") == commit,
            "metadata_evidence_profile": metadata.get("evidence_profile")
            == SIGNED_ADVERT_EVIDENCE_PROFILE,
            "metadata_boundary": metadata.get("coverage_boundary")
            == report.get("coverage_boundary"),
            "metadata_closure_ready_false": metadata.get("closure_ready") is False,
            "metadata_wp04_closure_eligible_false": metadata.get(
                "wp04_closure_eligible"
            )
            is False,
            "metadata_full_ubsan_clean": metadata.get("full_ubsan_clean") is True,
            "metadata_passed": metadata.get("passed") is True,
            "metadata_execution_complete": metadata.get("execution_complete")
            is True,
            "metadata_max_age": metadata.get("max_age_days")
            == MESHCORE_CONFORMANCE_MAX_AGE_DAYS,
            "package_git_commit": package_git.get("commit") == commit,
            "package_git_clean": package_git.get("dirty") is False
            and package_git.get("dirty_entries") == [],
            "package_workflow_sha": package_workflow.get("sha") == commit,
            "package_workflow_run_id": bool(expected_run_id)
            and str(package_workflow.get("run_id")) == str(expected_run_id),
            "evidence_filename": bool(
                evidence_path and evidence_path.name == expected_name
            ),
            "run_receipt_artifact": receipt_artifact
            == MESHCORE_SIGNED_ADVERT_ACTIONS_ARTIFACT,
            "run_receipt_filename": bool(
                run_receipt_path and run_receipt_path.name == expected_name
            ),
        }
        failures.extend(name for name, ok in checks.items() if not ok)
    if report:
        checks = {
            "report_schema_version": report.get("schema_version") == 1,
            "report_artifact_type": report.get("artifact_type")
            == SIGNED_ADVERT_ARTIFACT_TYPE,
            "report_passed": report.get("passed") is True,
            "report_status": report.get("status") == "pass",
            "report_execution_complete": report.get("execution_complete") is True,
            "report_source_commit": source_commit == commit,
            "report_closure_ready_false": report.get("closure_ready") is False,
            "report_wp04_closure_eligible_false": report.get(
                "wp04_closure_eligible"
            )
            is False,
            "report_full_ubsan_clean": report.get("full_ubsan_clean") is True,
        }
        failures.extend(name for name, ok in checks.items() if not ok)
    if canonical_report:
        canonical_matches = False
        if report and commit:
            try:
                canonical_matches = canonical_report == canonicalize_signed_advert_report(
                    report, commit
                )
            except (OSError, UnicodeError, json.JSONDecodeError, ValueError):
                canonical_matches = False
        checks = {
            "canonical_evidence_profile": canonical_report.get("evidence_profile")
            == SIGNED_ADVERT_EVIDENCE_PROFILE,
            "canonical_evidence_matches_run_receipt": canonical_matches,
        }
        failures.extend(name for name, ok in checks.items() if not ok)

    actual_sha, evidence_size = file_identity(
        evidence_path, "evidence_hash_unreadable", "evidence_stat_unreadable"
    )
    receipt_sha, receipt_size = file_identity(
        run_receipt_path,
        "run_receipt_hash_unreadable",
        "run_receipt_stat_unreadable",
    )
    if metadata and actual_sha != metadata.get("sha256"):
        failures.append("evidence_sha256_mismatch")
    if evidence_size is not None and metadata.get("size") != evidence_size:
        failures.append("evidence_size_mismatch")
    if run_receipt and receipt_sha != run_receipt.get("sha256"):
        failures.append("run_receipt_sha256_mismatch")
    if receipt_size is not None and run_receipt.get("size") != receipt_size:
        failures.append("run_receipt_size_mismatch")

    generated_at = parse_utc_timestamp(report.get("generated_at")) if report else None
    metadata_generated_at = (
        parse_utc_timestamp(run_receipt.get("generated_at")) if run_receipt else None
    )
    metadata_expires_at = (
        parse_utc_timestamp(run_receipt.get("expires_at")) if run_receipt else None
    )
    checked_at = (now or datetime.now(timezone.utc)).astimezone(timezone.utc)
    expected_expires_at = None
    expires_at_computable = generated_at is not None
    if generated_at is not None:
        try:
            expected_expires_at = generated_at + timedelta(
                days=MESHCORE_CONFORMANCE_MAX_AGE_DAYS
            )
        except OverflowError:
            expires_at_computable = False
    timestamp_checks = {
        "generated_at_valid": generated_at is not None,
        "expires_at_computable": expires_at_computable,
        "metadata_generated_at_matches": generated_at is not None
        and metadata_generated_at == generated_at,
        "metadata_expires_at_matches": expected_expires_at is not None
        and metadata_expires_at == expected_expires_at,
        "generated_at_not_future": generated_at is not None
        and generated_at
        <= checked_at + timedelta(minutes=MESHCORE_CONFORMANCE_CLOCK_SKEW_MINUTES),
        "evidence_not_expired": expected_expires_at is not None
        and checked_at < expected_expires_at,
    }
    failures.extend(name for name, ok in timestamp_checks.items() if not ok)
    failures = list(dict.fromkeys(failures))
    ok = not failures
    evidence = []
    if manifest_path and manifest_path.is_file():
        evidence.append(rel(manifest_path, root))
    if evidence_path and evidence_path.is_file():
        evidence.append(rel(evidence_path, root))
    if run_receipt_path and run_receipt_path.is_file():
        evidence.append(rel(run_receipt_path, root))
    return GateResult(
        "meshcore_signed_advert_runtime_packaged",
        "P0",
        ok,
        "Current-commit MeshCore signed-advert runtime packaged",
        evidence,
        (
            "Release package contains matching, unexpired signed-advert runtime "
            "evidence with exception-free sanitizer proof. This prerequisite "
            "does not close WP-04 or issue #65."
            if ok
            else "Release package is missing matching, unexpired MeshCore "
            "signed-advert runtime evidence; WP-04 and issue #65 remain open."
        ),
        {
            "failures": failures,
            "expected_commit": commit,
            "expected_run_id": expected_run_id,
            "package_run_id": package_workflow.get("run_id"),
            "package_git_dirty": package_git.get("dirty"),
            "package_git_dirty_entries": package_git.get("dirty_entries"),
            "source_commit": source_commit,
            "sha256": actual_sha,
            "run_receipt_sha256": receipt_sha,
            "generated_at": generated_at.isoformat() if generated_at else None,
            "expires_at": (
                expected_expires_at.isoformat() if expected_expires_at else None
            ),
            "checked_at": checked_at.isoformat(),
            "coverage_boundary": report.get("coverage_boundary") if report else None,
            "closure_ready": report.get("closure_ready") if report else None,
            "wp04_closure_eligible": (
                report.get("wp04_closure_eligible") if report else None
            ),
            "full_ubsan_clean": report.get("full_ubsan_clean") if report else None,
        },
    )


def host_checks_success_gate(
    github_run_dir: Path | None,
    root: Path,
    commit: str | None,
    expected_run_id: str | None,
) -> GateResult:
    failures: list[str] = []
    commit_valid = isinstance(commit, str) and re.fullmatch(r"[0-9a-fA-F]{40}", commit) is not None
    run_id_valid = (
        isinstance(expected_run_id, str)
        and re.fullmatch(r"[1-9][0-9]*", expected_run_id) is not None
    )
    if github_run_dir is None:
        failures.append("github_run_dir_missing")
    if not commit_valid:
        failures.append("expected_commit_invalid")
    if not run_id_valid:
        failures.append("expected_run_id_missing_or_invalid")

    marker: Path | None = None
    if github_run_dir is not None and commit_valid:
        marker = (
            github_run_dir
            / "d1l-host-artifacts"
            / "host-checks"
            / f"d1l_host_checks_success_{commit}.json"
        )
        if not marker.is_file():
            failures.append("host_checks_marker_missing")

    try:
        payload = read_json(marker)
    except (OSError, UnicodeError, json.JSONDecodeError):
        payload = {}
        failures.append("host_checks_marker_invalid")
    if not isinstance(payload, dict):
        payload = {}
        failures.append("host_checks_marker_invalid")

    attempt = payload.get("workflow_run_attempt")
    attempt_valid = (
        isinstance(attempt, (str, int))
        and not isinstance(attempt, bool)
        and str(attempt).isdigit()
        and int(attempt) >= 1
    )
    checks = {
        "marker_schema": payload.get("schema") == 1,
        "marker_artifact_type": payload.get("artifact_type") == HOST_CHECKS_ARTIFACT_TYPE,
        "marker_status": payload.get("status") == "pass",
        "marker_passed": payload.get("passed") is True,
        "marker_prior_steps": payload.get("all_prior_steps_completed") is True,
        "marker_job": payload.get("job") == "host-checks",
        "marker_commit": commit_valid and payload.get("repository_commit") == commit,
        "marker_run_id": run_id_valid
        and str(payload.get("workflow_run_id")) == expected_run_id,
        "marker_run_attempt": attempt_valid,
    }
    failures.extend(name for name, ok in checks.items() if not ok)
    failures = list(dict.fromkeys(failures))
    ok = not failures
    return GateResult(
        "same_run_host_checks_passed",
        "P0",
        ok,
        "Same-run host checks completed",
        [rel(marker, root)] if marker and marker.is_file() else [],
        (
            "The selected Actions run contains exact-commit host-check success evidence emitted after all host steps passed."
            if ok
            else "The selected Actions run lacks valid exact-commit host-check success evidence."
        ),
        {
            "failures": failures,
            "expected_commit": commit,
            "expected_run_id": expected_run_id,
            "marker_commit": payload.get("repository_commit"),
            "marker_run_id": payload.get("workflow_run_id"),
            "marker_run_attempt": attempt,
        },
    )


def meshcore_full_conformance_gate() -> GateResult:
    """Keep release fail-closed until issue #65 has a dedicated closure artifact.

    The currently packaged report deliberately covers only the structural wire
    envelope.  A later #65 slice must replace this pending gate with validation
    of a distinct full-surface artifact; allowing the structural prerequisite
    to satisfy release readiness would make the audit contradict its evidence.
    """
    return GateResult(
        "meshcore_full_conformance_complete",
        "P0",
        False,
        "Full MeshCore 1.0 conformance complete",
        [],
        (
            "Issue #65 remains open: wire-envelope evidence does not prove the "
            "semantic, cryptographic, retained-state, duplicate/replay, or real-peer surface."
        ),
        {
            "issue": 65,
            "closure_artifact_required": True,
            "closure_artifact_present": False,
            "closure_ready": False,
            "wire_envelope_evidence_is_sufficient": False,
        },
    )


def checksum_gate(github_run_dir: Path | None, root: Path) -> GateResult:
    manifests: list[Path] = []
    required_labels = ["d1l-firmware-artifacts/SHA256SUMS.txt"]
    optional_rp2040_labels = [
        "rp2040-sd-bridge-firmware/SHA256SUMS.txt",
        "rp2040-seeed-official-sd-smoke-firmware/SHA256SUMS.txt",
    ]
    package = None
    package_manifest: dict = {}
    if github_run_dir:
        package = find_release_package(github_run_dir)
        package_manifest = read_json(package / "manifest.json" if package else None)
        if package_manifest.get("rp2040_artifacts"):
            required_labels.extend(optional_rp2040_labels)
        manifests.extend(github_run_dir / label for label in required_labels)
        if package:
            manifests.append(package / "SHA256SUMS.txt")
        optional_rp2040_manifests = [github_run_dir / label for label in optional_rp2040_labels]
    else:
        optional_rp2040_manifests = []
    present = [path for path in manifests if path.is_file()]
    missing = [path for path in manifests if not path.is_file()]
    optional_present = [path for path in optional_rp2040_manifests if path.is_file() and path not in present]
    optional_missing = [path for path in optional_rp2040_manifests if not path.is_file()]
    failed = [path for path in present if not verify_sha256_manifest(path)]
    failed.extend(path for path in optional_present if not verify_sha256_manifest(path))
    expected_count = len(required_labels) + (1 if package else 0)
    ok = bool(present) and not missing and not failed and len(present) == expected_count
    return GateResult(
        "ci_artifacts_checksums",
        "P0",
        ok,
        "GitHub Actions artifacts and checksums",
        [rel(path, root) for path in present],
        "All required downloaded Actions checksum manifests verify."
        if ok else "Downloaded Actions artifacts are missing or have a checksum failure.",
        {
            "missing": [rel(path, root) for path in missing],
            "optional_missing": [rel(path, root) for path in optional_missing],
            "optional_present": [rel(path, root) for path in optional_present],
            "failed": [rel(path, root) for path in failed],
        },
    )


def notices_gate(github_run_dir: Path | None, root: Path) -> GateResult:
    package = find_release_package(github_run_dir) if github_run_dir else None
    manifest = read_json(package / "manifest.json" if package else None)
    notice_entries = manifest.get("notice_files") if isinstance(manifest.get("notice_files"), list) else []
    listed = {entry.get("path") for entry in notice_entries if isinstance(entry, dict)}
    existing = {path for path in REQUIRED_NOTICE_FILES if package and (package / path).is_file()}
    missing = sorted(REQUIRED_NOTICE_FILES - listed)
    missing_files = sorted(REQUIRED_NOTICE_FILES - existing)
    ok = package is not None and not missing and not missing_files
    evidence = [rel(package / path, root) for path in sorted(existing)] if package else []
    if package and (package / "manifest.json").is_file():
        evidence.append(rel(package / "manifest.json", root))
    return GateResult(
        "release_notices_included",
        "P0",
        ok,
        "Third-party notices and attributions packaged",
        evidence,
        "Release package includes all required notice and attribution documents."
        if ok else "Release package is missing one or more required notice/attribution files.",
        {"missing_manifest_entries": missing, "missing_files": missing_files},
    )


def simple_json_ok_gate(
    gate_id: str,
    title: str,
    path: Path | None,
    root: Path,
    predicate,
    success: str,
    failure: str,
    severity: str = "P0",
) -> GateResult:
    data = read_json(path)
    ok = bool(path and data and predicate(data))
    return GateResult(
        gate_id,
        severity,
        ok,
        title,
        [rel(path, root)] if path else [],
        success if ok else failure,
        {"path_found": bool(path), "artifact_ok": data.get("ok") if data else None},
    )


def exact_full_commit(value: object, commit: str | None) -> bool:
    return (
        isinstance(value, str)
        and isinstance(commit, str)
        and re.fullmatch(r"[0-9a-fA-F]{40}", value) is not None
        and value.lower() == commit.lower()
    )


def smoke_device_identity_ok(data: dict, expected_port: str, commit: str | None) -> bool:
    results = data.get("results")
    version = next(
        (
            row
            for row in results
            if isinstance(row, dict) and row.get("cmd") == "version"
        ),
        None,
    ) if isinstance(results, list) else None
    return (
        data.get("mode") == "hardware"
        and data.get("ok") is True
        and data.get("port") == expected_port
        and data.get("firmware_identity_required") is True
        and data.get("firmware_identity_ok") is True
        and exact_full_commit(data.get("expected_firmware_commit"), commit)
        and exact_full_commit(data.get("device_build_commit"), commit)
        and isinstance(version, dict)
        and version.get("ok") is True
        and exact_full_commit(version.get("build_commit"), commit)
    )


def checksum_manifest_entries(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeError):
        return {}
    entries: dict[str, str] = {}
    for raw in lines:
        line = raw.strip()
        if not line:
            continue
        try:
            digest, relative = line.split(maxsplit=1)
        except ValueError:
            return {}
        if re.fullmatch(r"[0-9a-fA-F]{64}", digest) is None:
            return {}
        if relative.startswith("./"):
            relative = relative[2:]
        relative = relative.replace("\\", "/")
        candidate = Path(relative)
        if (
            not relative
            or candidate.is_absolute()
            or any(part in {"", ".", ".."} for part in candidate.parts)
            or relative in entries
        ):
            return {}
        entries[relative] = digest.lower()
    return entries


def flash_command_matches_exact_files(
    command: list,
    write_flash_args: list[str],
    expected_targets: dict[int, Path],
) -> bool:
    if command.count("write-flash") != 1:
        return False
    write_index = command.index("write-flash")
    tail = [str(token) for token in command[write_index + 1 :]]
    if tail[: len(write_flash_args)] != write_flash_args:
        return False
    file_tokens = tail[len(write_flash_args) :]
    if len(file_tokens) != len(expected_targets) * 2:
        return False

    actual_pairs: list[tuple[int, Path]] = []
    for index in range(0, len(file_tokens), 2):
        try:
            offset = int(file_tokens[index], 0)
        except ValueError:
            return False
        candidate_path = Path(file_tokens[index + 1])
        if not candidate_path.is_absolute():
            return False
        try:
            candidate_file = candidate_path.resolve(strict=True)
        except (OSError, RuntimeError):
            return False
        actual_pairs.append((offset, candidate_file))

    expected_pairs: list[tuple[int, Path]] = []
    for offset, artifact_file in sorted(expected_targets.items()):
        try:
            expected_file = artifact_file.resolve(strict=True)
        except (OSError, RuntimeError):
            return False
        expected_pairs.append((offset, expected_file))
    return actual_pairs == expected_pairs


def command_uses_exact_port(command: list, expected_port: str) -> bool:
    ports = [
        str(command[index + 1])
        for index, token in enumerate(command[:-1])
        if str(token) in {"-p", "--port"}
    ]
    return ports == [expected_port]


def esp32_flash_receipt_gate(
    hardware_dir: Path,
    github_run_dir: Path | None,
    root: Path,
    commit: str | None,
    expected_run_id: str | None,
    expected_port: str,
) -> GateResult:
    receipt = newest_commit_json(hardware_dir, commit, "esp32_flash_*.json")
    data = read_json(receipt)
    failures: list[str] = []
    if receipt is None:
        failures.append("flash_receipt_missing")
    if github_run_dir is None:
        failures.append("github_run_dir_missing")
    if not commit:
        failures.append("expected_commit_missing")
    if not expected_run_id:
        failures.append("expected_run_id_missing")

    artifact_root = github_run_dir / "d1l-firmware-artifacts" if github_run_dir else None
    manifest_path = artifact_root / "SHA256SUMS.txt" if artifact_root else None
    manifest_entries = checksum_manifest_entries(manifest_path) if manifest_path else {}
    manifest_verified = bool(
        manifest_path
        and manifest_path.is_file()
        and verify_sha256_manifest(manifest_path)
    )
    actual_manifest_sha = None
    if manifest_path and manifest_path.is_file():
        try:
            actual_manifest_sha = sha256_file(manifest_path)
        except OSError:
            failures.append("manifest_hash_unreadable")

    verification = data.get("artifact_verification") if isinstance(data, dict) else None
    verification = verification if isinstance(verification, dict) else {}
    result = data.get("result") if isinstance(data, dict) else None
    result = result if isinstance(result, dict) else {}
    command = data.get("command") if isinstance(data, dict) else None
    command = command if isinstance(command, list) else []
    flash_rows = verification.get("flash_files")
    flash_rows = flash_rows if isinstance(flash_rows, list) else []

    flasher_path = artifact_root / "build" / "flasher_args.json" if artifact_root else None
    flasher: dict = {}
    if flasher_path and flasher_path.is_file():
        try:
            candidate = json.loads(flasher_path.read_text(encoding="utf-8"))
            flasher = candidate if isinstance(candidate, dict) else {}
        except (OSError, UnicodeError, json.JSONDecodeError):
            failures.append("flasher_args_invalid")
    else:
        failures.append("flasher_args_missing")

    expected_flash: dict[int, str] = {}
    expected_targets: dict[int, Path] = {}
    write_flash_args: list[str] = []
    raw_flash_files = flasher.get("flash_files")
    raw_write_flash_args = flasher.get("write_flash_args", [])
    if isinstance(raw_write_flash_args, list):
        write_flash_args = [
            str(item).replace("_", "-") if str(item).startswith("--") else str(item)
            for item in raw_write_flash_args
        ]
    else:
        failures.append("flasher_write_flash_args_invalid")
    if artifact_root and isinstance(raw_flash_files, dict) and raw_flash_files:
        try:
            artifact_root_resolved = artifact_root.resolve(strict=True)
            build_root = (artifact_root_resolved / "build").resolve(strict=True)
            build_root.relative_to(artifact_root_resolved)
            for raw_offset, raw_path in raw_flash_files.items():
                offset = int(str(raw_offset), 0)
                raw_relative = str(raw_path)
                relative_path = Path(raw_relative)
                if (
                    not raw_relative
                    or relative_path.is_absolute()
                    or any(part in {"", ".", ".."} for part in relative_path.parts)
                    or offset < 0
                    or offset in expected_flash
                ):
                    raise ValueError
                target = (build_root / relative_path).resolve(strict=True)
                target.relative_to(build_root)
                artifact_relative = target.relative_to(artifact_root_resolved).as_posix()
                expected_flash[offset] = artifact_relative
                expected_targets[offset] = target
        except (TypeError, ValueError, OSError, RuntimeError):
            expected_flash = {}
            expected_targets = {}
            failures.append("flasher_flash_files_invalid")
    else:
        failures.append("flasher_flash_files_missing")

    row_offsets: set[int] = set()
    rows_ok = bool(flash_rows) and len(flash_rows) == len(expected_flash)
    for row in flash_rows:
        if not isinstance(row, dict):
            rows_ok = False
            continue
        offset = row.get("offset")
        if isinstance(offset, bool) or not isinstance(offset, int) or offset < 0 or offset in row_offsets:
            rows_ok = False
            continue
        row_offsets.add(offset)
        relative = row.get("path")
        expected_relative = expected_flash.get(offset)
        expected_target = expected_targets.get(offset)
        digest = row.get("sha256")
        target_hash = None
        target_size = None
        target_ok = False
        if isinstance(relative, str) and relative == expected_relative and expected_target:
            try:
                target_ok = expected_target.is_file()
                if target_ok:
                    target_size = expected_target.stat().st_size
                    target_hash = sha256_file(expected_target)
            except OSError:
                target_ok = False
        rows_ok = rows_ok and (
            relative == expected_relative
            and row.get("offset_hex") == hex(offset)
            and isinstance(row.get("size"), int)
            and not isinstance(row.get("size"), bool)
            and target_ok
            and row.get("size") == target_size
            and isinstance(digest, str)
            and re.fullmatch(r"[0-9a-fA-F]{64}", digest) is not None
            and manifest_entries.get(str(relative)) == digest.lower()
            and target_hash == digest.lower()
        )

    flasher_hash = None
    if flasher_path and flasher_path.is_file():
        try:
            flasher_hash = sha256_file(flasher_path)
        except OSError:
            pass
    safe_command = (
        bool(command)
        and command == result.get("args")
        and "write-flash" in command
        and command_uses_exact_port(command, expected_port)
        and not any("erase" in str(token).lower() for token in command)
    )
    command_files_exact = flash_command_matches_exact_files(
        command,
        write_flash_args,
        expected_targets,
    )
    required_roles_present = expected_flash == REQUIRED_ESP32_FLASH_ROLES
    checks = {
        "receipt_hardware_mode": data.get("mode") == "hardware",
        "receipt_kind": data.get("kind") == "esp32_flash",
        "receipt_ok": data.get("ok") is True,
        "receipt_port": data.get("port") == expected_port,
        "receipt_commit": exact_full_commit(data.get("commit"), commit),
        "receipt_run_id": bool(expected_run_id)
        and str(data.get("github_actions_run")) == str(expected_run_id),
        "receipt_no_public_rf": data.get("public_rf_tx") is False,
        "receipt_no_format": data.get("formats_sd") is False,
        "esptool_result": result.get("name") == "esp32_flash"
        and result.get("ok") is True
        and result.get("returncode") == 0,
        "safe_exact_flash_command": safe_command,
        "artifact_verification": verification.get("ok") is True,
        "manifest_complete": verification.get("manifest_complete") is True,
        "manifest_verified": manifest_verified,
        "manifest_entry_count": verification.get("manifest_entry_count") == len(manifest_entries)
        and len(manifest_entries) > 0,
        "manifest_sha256": isinstance(verification.get("manifest_sha256"), str)
        and actual_manifest_sha == verification.get("manifest_sha256").lower(),
        "flasher_args_sha256": isinstance(verification.get("flasher_args_sha256"), str)
        and flasher_hash == verification.get("flasher_args_sha256").lower(),
        "flash_files_exact": rows_ok and set(expected_flash) == row_offsets,
        "flash_command_files_exact": command_files_exact,
        "required_flash_roles": required_roles_present,
    }
    failures.extend(name for name, ok in checks.items() if not ok)
    failures = list(dict.fromkeys(failures))
    ok = not failures
    return GateResult(
        "exact_actions_esp32_flash",
        "P0",
        ok,
        "Exact Actions ESP32 artifact flashed to the selected D1L port",
        [rel(receipt, root)] if receipt else [],
        (
            "A successful non-erasing D1L flash receipt proves the selected run's complete checksummed project image was written."
            if ok
            else "No valid successful D1L flash receipt matches the selected commit, run, and complete Actions artifact."
        ),
        {
            "failures": failures,
            "expected_commit": commit,
            "expected_run_id": expected_run_id,
            "expected_port": expected_port,
            "receipt_run_id": data.get("github_actions_run") if data else None,
            "receipt_port": data.get("port") if data else None,
            "manifest_sha256": actual_manifest_sha,
            "flash_file_count": len(flash_rows),
        },
    )


def all_checks_true(data: dict) -> bool:
    checks = data.get("checks")
    return isinstance(checks, dict) and bool(checks) and all(value is True for value in checks.values())


def map_network_counter_evidence_ok(data: dict) -> bool:
    evidence = data.get("map_network_evidence")
    if not isinstance(evidence, dict):
        return False
    before = evidence.get("before")
    after = evidence.get("after")
    if not isinstance(before, dict) or not isinstance(after, dict):
        return False
    before_count = before.get("network_requests")
    after_count = after.get("network_requests")
    if (
        isinstance(before_count, bool)
        or isinstance(after_count, bool)
        or not isinstance(before_count, int)
        or not isinstance(after_count, int)
        or before_count < 0
        or after_count < 0
    ):
        return False
    return (
        evidence.get("command") == "map tiles status"
        and evidence.get("measured") is True
        and evidence.get("samples_valid") is True
        and evidence.get("unchanged") is True
        and evidence.get("before_count") == before_count
        and evidence.get("after_count") == after_count
        and evidence.get("delta") == 0
        and before.get("ok") is True
        and after.get("ok") is True
        and before_count == after_count
    )


def ui_corruption_probe_ok(data: dict, expected_port: str) -> bool:
    telemetry = data.get("telemetry") if isinstance(data.get("telemetry"), dict) else {}
    telemetry_fields = set(telemetry.get("telemetry_fields") or data.get("telemetry_fields") or [])
    checks = data.get("checks") if isinstance(data.get("checks"), dict) else {}
    return (
        data.get("ok") is True
        and data.get("port") == expected_port
        and data.get("public_rf_tx") is False
        and data.get("network_tx") is False
        and data.get("map_network_requests") is False
        and map_network_counter_evidence_ok(data)
        and data.get("formats_sd") is False
        and data.get("skip_data_canary") is not True
        and int(data.get("rounds") or 0) >= RELEASE_UI_CORRUPTION_MIN_ROUNDS
        and int(data.get("data_refresh_events") or 0) >= int(data.get("rounds") or 0)
        and int(data.get("failure_count") or 0) == 0
        and int(telemetry.get("health_sample_count") or 0) > 0
        and telemetry.get("uptime_monotonic") is True
        and telemetry.get("final_pending") is False
        and telemetry.get("final_active_tab") in REQUIRED_SCROLL_SURFACES.values()
        and REQUIRED_UI_TELEMETRY_FIELDS.issubset(telemetry_fields)
        and checks.get("tab_switches_settle") is True
        and checks.get("data_refresh_exercised") is True
        and checks.get("data_refreshes_pass") is True
        and checks.get("no_public_rf") is True
        and checks.get("no_map_network_requests") is True
        and checks.get("no_formatting") is True
        and checks.get("no_stuck_pending") is True
        and checks.get("final_active_tab_known") is True
    )


def ui_pixel_capture_ok(data: dict, expected_port: str) -> bool:
    simulator_diff = data.get("simulator_diff") if isinstance(data.get("simulator_diff"), dict) else {}
    return (
        data.get("ok") is True
        and data.get("kind") == "ui_pixel_capture"
        and data.get("mode") == "hardware"
        and data.get("port") == expected_port
        and data.get("public_rf_tx") is False
        and data.get("formats_sd") is False
        and data.get("width") == 480
        and data.get("height") == 480
        and data.get("bytes_per_pixel") == 2
        and data.get("pixel_format") == "rgb565-le"
        and data.get("total_bytes") == 480 * 480 * 2
        and data.get("captured_bytes") == 480 * 480 * 2
        and int(data.get("chunk_count") or 0) > 0
        and bool(data.get("crc32"))
        and bool(data.get("png_path"))
        and bool(data.get("raw_path"))
        and bool(data.get("reference_png_path"))
        and bool(data.get("simulator_diff_path"))
        and data.get("simulator_diff_ok") is True
        and simulator_diff.get("ok") is True
        and simulator_diff.get("kind") == "ui_capture_simulator_diff"
        and simulator_diff.get("size_match") is True
        and simulator_diff.get("width") == 480
        and simulator_diff.get("height") == 480
        and simulator_diff.get("public_rf_tx") is False
        and simulator_diff.get("formats_sd") is False
    )


def compose_capture_expected_onboarding(expected_target: str) -> bool:
    return expected_target == "onboarding"


def compose_capture_requires_hidden_dock(expected_target: str) -> bool:
    return expected_target in {
        "public",
        "public-long",
        "dm",
        "dm-long",
        "public-search",
        "dm-search",
        "packet-search",
        "contact-edit",
        "onboarding",
        "map-location",
        "wifi-ssid",
        "wifi-password",
    }


def compose_capture_min_keyboard_size(expected_target: str) -> tuple[int, int]:
    if expected_target in {"public", "public-long", "dm", "dm-long"}:
        return (440, 250)
    if expected_target in {"public-search", "dm-search", "packet-search"}:
        return (400, 180)
    if expected_target == "contact-edit":
        return (400, 170)
    if expected_target in {"onboarding", "map-location"}:
        return (400, 150)
    return (440, 80)


def compose_capture_probe_ok(probe: dict, expected_target: str) -> bool:
    if not isinstance(probe, dict):
        return False
    keyboard = probe.get("keyboard") if isinstance(probe.get("keyboard"), dict) else {}
    textarea = probe.get("textarea") if isinstance(probe.get("textarea"), dict) else {}
    sheet = probe.get("sheet") if isinstance(probe.get("sheet"), dict) else {}
    min_w, min_h = compose_capture_min_keyboard_size(expected_target)
    try:
        keyboard_inside_sheet = (
            int(keyboard.get("x")) >= 0
            and int(keyboard.get("y")) >= 0
            and int(keyboard.get("x")) + int(keyboard.get("w")) <= int(sheet.get("w"))
            and int(keyboard.get("y")) + int(keyboard.get("h")) <= int(sheet.get("h"))
        )
        textarea_above_keyboard = (
            int(textarea.get("x")) >= 0
            and int(textarea.get("y")) >= 0
            and int(textarea.get("x")) + int(textarea.get("w")) <= int(sheet.get("w"))
            and int(textarea.get("y")) + int(textarea.get("h")) <= int(keyboard.get("y"))
        )
        keyboard_size_ok = int(keyboard.get("w")) >= min_w and int(keyboard.get("h")) >= min_h
    except (TypeError, ValueError):
        return False
    expected_onboarding = compose_capture_expected_onboarding(expected_target)
    dock_ok = (
        probe.get("dock_hidden") is True
        if compose_capture_requires_hidden_dock(expected_target)
        else isinstance(probe.get("dock_hidden"), bool)
    )
    return (
        probe.get("ok") is True
        and probe.get("cmd") == "ui compose-probe"
        and probe.get("target") == expected_target.replace("-", "_")
        and probe.get("target_supported") is True
        and probe.get("sheet_visible") is True
        and probe.get("textarea_visible") is True
        and probe.get("keyboard_visible") is True
        and probe.get("onboarding_visible") is expected_onboarding
        and dock_ok
        and probe.get("dm_mode") is (expected_target.startswith("dm"))
        and probe.get("public_rf_tx") is False
        and probe.get("formats_sd") is False
        and keyboard_inside_sheet
        and textarea_above_keyboard
        and keyboard_size_ok
    )


def compose_keyboard_capture_ok(data: dict, expected_port: str) -> bool:
    captures = data.get("captures") if isinstance(data.get("captures"), list) else []
    captures_by_target = {
        item.get("target"): item
        for item in captures
        if isinstance(item, dict) and isinstance(item.get("target"), str)
    }
    if not REQUIRED_COMPOSE_CAPTURE_TARGETS.issubset(set(captures_by_target)):
        return False
    return (
        data.get("ok") is True
        and data.get("kind") == "ui_compose_keyboard_capture"
        and data.get("mode") == "hardware"
        and data.get("port") == expected_port
        and data.get("public_rf_tx") is False
        and data.get("formats_sd") is False
        and all(
            capture.get("ok") is True
            and capture.get("public_rf_tx") is False
            and capture.get("network_tx") is False
            and capture.get("map_network_requests") is False
            and capture.get("formats_sd") is False
            and bool(capture.get("png_path"))
            and bool(capture.get("raw_path"))
            and isinstance(capture.get("capture"), dict)
            and capture["capture"].get("ok") is True
            and capture["capture"].get("kind") == "ui_pixel_capture"
            and capture["capture"].get("port") == expected_port
            and capture["capture"].get("width") == 480
            and capture["capture"].get("height") == 480
            and capture["capture"].get("onboarding_visible") is compose_capture_expected_onboarding(target)
            and capture.get("target_visible") is True
            and compose_capture_probe_ok(capture.get("compose_probe"), target)
            for target, capture in captures_by_target.items()
            if target in REQUIRED_COMPOSE_CAPTURE_TARGETS
        )
    )


def scroll_probe_results_by_screen(data: dict) -> dict:
    probes: dict = {}
    probe_results = data.get("probe_results")
    if isinstance(probe_results, dict):
        for screen, probe in probe_results.items():
            if isinstance(screen, str) and isinstance(probe, dict):
                probes[screen] = probe
    events = data.get("events")
    if isinstance(events, list):
        for event in events:
            if not isinstance(event, dict):
                continue
            screen = event.get("screen")
            probe = event.get("probe")
            if isinstance(screen, str) and isinstance(probe, dict):
                probes[screen] = probe
    return probes


def scroll_probe_screen_ok(screen: str, tab: str, probe: dict) -> bool:
    movement_ok = screen in SCROLL_MOVEMENT_OPTIONAL or (
        probe.get("scrollable") is True and probe.get("moved") is True
    )
    probe_ok = probe.get("ok") is True or (screen in SCROLL_MOVEMENT_OPTIONAL and movement_ok)
    return (
        probe_ok
        and probe.get("surface") == screen
        and probe.get("tab") == tab
        and probe.get("target_found") is True
        and movement_ok
    )


def scroll_probe_ok(data: dict, expected_port: str) -> bool:
    screens = set(data.get("screens") or [])
    plan = data.get("surface_plan") if isinstance(data.get("surface_plan"), list) else []
    tabs_by_screen = {
        item.get("screen"): item.get("tab")
        for item in plan
        if isinstance(item, dict) and isinstance(item.get("screen"), str)
    }
    probes = scroll_probe_results_by_screen(data)
    return (
        data.get("ok") is True
        and data.get("port") == expected_port
        and int(data.get("failure_count") or 0) == 0
        and REQUIRED_SCROLL_SCREENS.issubset(screens)
        and all(tabs_by_screen.get(screen) == tab for screen, tab in REQUIRED_SCROLL_SURFACES.items())
        and all(
            isinstance(probes.get(screen), dict)
            and scroll_probe_screen_ok(screen, tab, probes[screen])
            for screen, tab in REQUIRED_SCROLL_SURFACES.items()
        )
        and data.get("network_tx") is False
        and data.get("map_network_requests") is False
        and map_network_counter_evidence_ok(data)
        and data.get("background_download") is False
        and data.get("area_download") is False
        and int(data.get("visible_tile_limit") or 0) == 9
        and int(data.get("zoom_batch_limit") or 0) == 1
        and data.get("wifi_mutation") is False
        and data.get("storage_mutation") is False
    )


def real_hardware_rf_boundary_ok(data: dict, expected_port: str) -> bool:
    git = data.get("git") if isinstance(data.get("git"), dict) else {}
    return (
        data.get("schema") == 2
        and data.get("hardware_required") is True
        and data.get("physical_observed") is True
        and data.get("dry_run") is False
        and data.get("simulated") is False
        and data.get("simulation") is False
        and data.get("source_inspection") is False
        and data.get("execution_complete") is True
        and data.get("dm_rf_tx") is True
        and data.get("public_rf_tx") is False
        and data.get("public_rf_transmit") is False
        and data.get("formats_sd") is False
        and allowed_port(expected_port)
        and normalize_port(data.get("port")) == normalize_port(expected_port)
        and git.get("dirty") is False
    )


def _strict_control_sidecar_bytes(
    receipt: object,
    evidence_root: Path,
    *,
    source_path: str,
    transport: str,
) -> tuple[bytes, Path] | None:
    if not isinstance(receipt, dict):
        return None
    try:
        root = Path(evidence_root).resolve(strict=True)
        if not root.is_dir() or is_link_or_reparse(root):
            return None
        relative_text = receipt.get("path")
        if (
            not isinstance(relative_text, str)
            or not relative_text
            or "\\" in relative_text
        ):
            return None
        relative = PurePosixPath(relative_text)
        if (
            relative.is_absolute()
            or str(relative) != relative_text
            or any(part in {"", ".", ".."} for part in relative.parts)
        ):
            return None
        candidate = root.joinpath(*relative.parts)
        cursor = root
        for part in relative.parts[:-1]:
            cursor /= part
            if not cursor.is_dir() or is_link_or_reparse(cursor):
                return None
        if is_link_or_reparse(candidate):
            return None
        before = os.lstat(candidate)
        if not stat.S_ISREG(before.st_mode):
            return None
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(root)
        raw = candidate.read_bytes()
        after = os.lstat(candidate)
        if (
            before.st_dev != after.st_dev
            or before.st_ino != after.st_ino
            or before.st_size != after.st_size
            or before.st_mtime_ns != after.st_mtime_ns
        ):
            return None
        digest = hashlib.sha256(raw).hexdigest()
        peer_pid = receipt.get("source_peer_pid")
        if not (
            receipt.get("size") == len(raw)
            and receipt.get("sha256") == digest
            and receipt.get("source_path") == source_path
            and receipt.get("source_host") == REMOTE_PEER_HOSTNAME
            and receipt.get("source_hostname") == REMOTE_PEER_HOSTNAME
            and receipt.get("transport") == transport
            and isinstance(peer_pid, int)
            and not isinstance(peer_pid, bool)
            and peer_pid > 0
            and receipt.get("source_peer_uid") == 0
            and receipt.get("source_peer_gid") == 0
        ):
            return None
        return raw, resolved
    except (OSError, RuntimeError, TypeError, ValueError):
        return None


def strict_control_sidecars_ok(
    data: dict,
    evidence_root: Path | None,
    *,
    control_socket: str,
    request_transport: str,
    response_transport: str,
) -> bool:
    if evidence_root is None:
        return False
    control = data.get("controlled_peer_control")
    if not isinstance(control, dict):
        return False
    request_capture = _strict_control_sidecar_bytes(
        control.get("request_receipt"),
        evidence_root,
        source_path=control_socket,
        transport=request_transport,
    )
    response_capture = _strict_control_sidecar_bytes(
        control.get("response_receipt"),
        evidence_root,
        source_path=control_socket,
        transport=response_transport,
    )
    if request_capture is None or response_capture is None:
        return False
    request_raw, request_path = request_capture
    response_raw, response_path = response_capture
    if request_path == response_path:
        return False
    try:
        expected_request, expected_raw = remote_control_request(
            data.get("d1l_public_key"),
            data.get("inbound_token"),
        )
        response = json.loads(response_raw.decode("utf-8"))
        validation = validate_remote_control_exchange(
            request_raw,
            response_raw,
            d1l_public_key=data.get("d1l_public_key"),
            token=data.get("inbound_token"),
        )
    except (UnicodeDecodeError, ValueError):
        return False
    return bool(
        request_raw == expected_raw
        and isinstance(response, dict)
        and control.get("op") == "radio.send_dm"
        and control.get("socket_path") == control_socket
        and control.get("request_id") == expected_request["id"]
        and control.get("request") == expected_request
        and control.get("response") == response
        and control.get("request_sha256")
        == hashlib.sha256(request_raw).hexdigest()
        and control.get("response_sha256")
        == hashlib.sha256(response_raw).hexdigest()
        and control.get("validation") == validation
        and validation.get("ok") is True
        and remote_control_semantic_ok(
            control,
            d1l_public_key=data.get("d1l_public_key"),
            token=data.get("inbound_token"),
            control_socket=control_socket,
        )
    )


def controlled_peer_evidence_ok(
    data: dict,
    expected_port: str,
    expected_peer_port: str | None,
    *,
    require_status: bool,
    evidence_root: Path | None = None,
) -> bool:
    peer = data.get("controlled_peer")
    if not isinstance(peer, dict):
        return False
    fingerprint = data.get("target_fingerprint")
    if (
        not isinstance(fingerprint, str)
        or re.fullmatch(r"[0-9A-Fa-f]{16}", fingerprint) is None
        or peer.get("fingerprint") != fingerprint
    ):
        return False
    source = peer.get("evidence_source")
    peer_identity = peer.get("port")
    peer_port = normalize_port(peer_identity)
    expected_peer = normalize_port(expected_peer_port)
    if source == "d1l_bidirectional_rf":
        return (
            not require_status
            and peer_port is None
            and expected_peer is None
        )
    if source == LOCAL_PEER_EVIDENCE_SOURCE:
        return (
            require_status
            and peer_port is None
            and expected_peer is None
            and pinned_peer_report_shape_ok(data)
        )
    if source != "explicit_peer_status":
        return False
    if peer_identity == MESHCOREBOT_PEER_DEVICE:
        return (
            require_status
            and expected_peer_port in (None, MESHCOREBOT_PEER_DEVICE)
            and expected_port != MESHCOREBOT_PEER_DEVICE
            and peer.get("fingerprint")
            == MESHCOREBOT_PEER_FINGERPRINT
            and peer.get("profile") == MESHCOREBOT_PEER_PROFILE
            and peer.get("status_path")
            == str(MESHCOREBOT_PEER_STATUS_PATH)
            and peer.get("control_socket")
            == MESHCOREBOT_PEER_CONTROL_SOCKET
            and peer.get("public_key")
            == MESHCOREBOT_PEER_PUBLIC_KEY
            and peer.get("device_access")
            == "opaque_status_identity_only"
            and remote_control_semantic_ok(
                data.get("controlled_peer_control"),
                d1l_public_key=data.get("d1l_public_key"),
                token=data.get("inbound_token"),
                control_socket=MESHCOREBOT_PEER_CONTROL_SOCKET,
            )
            and strict_control_sidecars_ok(
                data,
                evidence_root,
                control_socket=MESHCOREBOT_PEER_CONTROL_SOCKET,
                request_transport=LOCAL_PEER_CONTROL_REQUEST_TRANSPORT,
                response_transport=LOCAL_PEER_CONTROL_RESPONSE_TRANSPORT,
            )
        )
    return (
        peer_port is not None
        and allowed_port(peer_port)
        and peer_port != normalize_port(expected_port)
        and (expected_peer is None or peer_port == expected_peer)
        and isinstance(peer.get("status_path"), str)
        and bool(peer["status_path"].strip())
    )


def completed_hardware_steps_ok(data: dict, commands: list[str]) -> bool:
    steps = data.get("steps")
    return (
        isinstance(steps, list)
        and bool(steps)
        and len(steps) == len(commands)
        and all(
            isinstance(step, dict)
            and step.get("command") == command
            and isinstance(step.get("result"), dict)
            and step["result"].get("ok") is True
            for step, command in zip(steps, commands, strict=True)
        )
    )


def dm_probe_ok(
    data: dict,
    expected_port: str,
    expected_peer_port: str | None = None,
    *,
    evidence_root: Path | None = None,
) -> bool:
    checks = data.get("checks") if isinstance(data.get("checks"), dict) else {}
    commands = data.get("commands_sent")
    return (
        data.get("ok") is True
        and data.get("mode") == "hardware-dm-probe"
        and real_hardware_rf_boundary_ok(data, expected_port)
        and controlled_peer_evidence_ok(
            data,
            expected_port,
            expected_peer_port,
            require_status=True,
            evidence_root=evidence_root,
        )
        and isinstance(commands, list)
        and bool(commands)
        and all(isinstance(command, str) for command in commands)
        and not any(command.strip().lower().startswith("mesh send public ") for command in commands)
        and completed_hardware_steps_ok(data, commands)
        and isinstance(data.get("token"), str)
        and bool(data["token"])
        and data["token"] in json.dumps(data["steps"], sort_keys=True)
        and all(checks.get(name) is True for name in REQUIRED_DM_PROBE_CHECKS)
    )


def outbound_dm_gate(
    hardware_dir: Path,
    root: Path,
    commit: str | None,
    expected_port: str,
    expected_peer_port: str | None,
) -> GateResult:
    artifact = newest_commit_json(
        hardware_dir,
        commit,
        "rf_full_acceptance*.json",
        "dm_probe_*.json",
    )
    data = read_json(artifact)
    mode = data.get("mode") if data else None
    if mode == "rf-full-acceptance":
        valid = full_rf_acceptance_ok(
            data,
            expected_port,
            expected_peer_port,
            evidence_root=root,
        )
    else:
        valid = dm_probe_ok(
            data,
            expected_port,
            expected_peer_port,
            evidence_root=root,
        )
    ok = bool(artifact and data and valid)
    return GateResult(
        "outbound_dm_controlled_peer",
        "P0",
        ok,
        "D1L outbound DM proof with a controlled peer",
        [rel(artifact, root)] if artifact else [],
        "Exact-commit outbound D1L DM proof with a controlled peer passes."
        if ok
        else "No exact-commit, real-hardware outbound D1L DM proof with an allowed controlled peer was found.",
        {
            "path_found": bool(artifact),
            "artifact_ok": data.get("ok") if data else None,
            "mode": mode,
            "port": data.get("port") if data else None,
            "peer_evidence_source": (
                data.get("controlled_peer", {}).get("evidence_source")
                if isinstance(data.get("controlled_peer"), dict)
                else None
            ),
        },
    )


def full_rf_acceptance_ok(
    data: dict,
    expected_port: str,
    expected_peer_port: str | None = None,
    *,
    evidence_root: Path | None = None,
) -> bool:
    checks = data.get("checks") if isinstance(data.get("checks"), dict) else {}
    steps = data.get("steps")
    commands = [
        step.get("command")
        for step in steps
        if isinstance(step, dict) and isinstance(step.get("command"), str)
    ] if isinstance(steps, list) else []
    source = (
        data.get("controlled_peer", {}).get("evidence_source")
        if isinstance(data.get("controlled_peer"), dict)
        else None
    )
    require_status = source in {
        "explicit_peer_status",
        LOCAL_PEER_EVIDENCE_SOURCE,
    }
    tokens = [
        data.get("outbound_token"),
        data.get("inbound_token"),
        data.get("direct_token"),
    ]
    return (
        data.get("ok") is True
        and data.get("mode") == "rf-full-acceptance"
        and real_hardware_rf_boundary_ok(data, expected_port)
        and controlled_peer_evidence_ok(
            data,
            expected_port,
            expected_peer_port,
            require_status=require_status,
            evidence_root=evidence_root,
        )
        and isinstance(steps, list)
        and bool(steps)
        and len(commands) == len(steps)
        and completed_hardware_steps_ok(data, commands)
        and not any(command.strip().lower().startswith("mesh send public ") for command in commands)
        and all(
            any(command.startswith(prefix) for command in commands)
            for prefix in (
                "identity status",
                "mesh send dm ",
                "messages dm ",
                "packets",
                "routes trace ",
                "health",
            )
        )
        and all(isinstance(token, str) and bool(token) for token in tokens)
        and len(set(tokens)) == len(tokens)
        and all(token in json.dumps(steps, sort_keys=True) for token in tokens)
        and isinstance(data.get("inbound_seen_at"), str)
        and bool(data["inbound_seen_at"].strip())
        and all(checks.get(name) is True for name in REQUIRED_FULL_RF_CHECKS)
    )


def route_probe_ok(data: dict, expected_port: str) -> bool:
    checks = data.get("checks") if isinstance(data.get("checks"), dict) else {}
    return (
        data.get("ok") is True
        and data.get("mode") == "hardware-route-probe"
        and data.get("port") == expected_port
        and data.get("dm_rf_tx") is True
        and data.get("public_rf_tx") is False
        and data.get("formats_sd") is False
        and all(checks.get(name) is True for name in REQUIRED_ROUTE_PROBE_CHECKS)
    )


def route_probe_gate(hardware_dir: Path, root: Path, commit: str | None, expected_port: str) -> GateResult:
    probe = newest_commit_json(hardware_dir, commit, "routes_probe_*.json")
    data = read_json(probe)
    checks = data.get("checks") if isinstance(data.get("checks"), dict) else {}
    missing_or_failed = sorted(name for name in REQUIRED_ROUTE_PROBE_CHECKS if checks.get(name) is not True)
    ok = bool(probe and data and route_probe_ok(data, expected_port))
    return GateResult(
        "supplemental_dm_route_probe",
        "P1",
        ok,
        "Supplemental DM-only route active probe",
        [rel(probe, root)] if probe else [],
        "Supplemental DM-only active route probe evidence is present; full RF acceptance still requires inbound DM, ACK/PATH, and direct-route proof."
        if ok else "No passing supplemental DM-only route probe artifact was found.",
        {
            "path_found": bool(probe),
            "artifact_ok": data.get("ok") if data else None,
            "mode": data.get("mode") if data else None,
            "dm_rf_tx": data.get("dm_rf_tx") if data else None,
            "public_rf_tx": data.get("public_rf_tx") if data else None,
            "formats_sd": data.get("formats_sd") if data else None,
            "missing_or_failed_checks": missing_or_failed,
            "scope": "supplementary_dm_only_not_full_rf_acceptance",
        },
    )


def iter_nested_dicts(value: Any):
    if isinstance(value, dict):
        yield value
        for nested in value.values():
            yield from iter_nested_dicts(nested)
    elif isinstance(value, list):
        for nested in value:
            yield from iter_nested_dicts(nested)
    elif isinstance(value, str):
        text = value.strip()
        if text.startswith("{") and text.endswith("}"):
            try:
                parsed = json.loads(text)
            except json.JSONDecodeError:
                return
            yield from iter_nested_dicts(parsed)


def first_official_seeed_smoke_payload(data: dict) -> dict:
    for candidate in iter_nested_dicts(data):
        if candidate.get("test") == "seeed_official_sd_smoke":
            return candidate
    return {}


def int_value(value: Any) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def first_number(value: Any) -> float | None:
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        match = re.search(r"-?\d+(?:\.\d+)?", value)
        if match:
            try:
                return float(match.group(0))
            except ValueError:
                return None
    return None


def bool_field(data: dict, *names: str) -> bool:
    containers = [data]
    for key in ("checks", "measurements", "evidence"):
        nested = data.get(key)
        if isinstance(nested, dict):
            containers.append(nested)
    return any(container.get(name) is True for container in containers for name in names)


def first_numeric_field(data: dict, *names: str) -> float | None:
    containers = [data]
    for key in ("checks", "measurements", "evidence"):
        nested = data.get(key)
        if isinstance(nested, dict):
            containers.append(nested)
    for container in containers:
        for name in names:
            number = first_number(container.get(name))
            if number is not None:
                return number
    return None


def path_for_details(path: Path | None, root: Path) -> str | None:
    return rel(path, root) if path else None


def exact_port_ok(data: dict, expected_port: str) -> bool:
    return data.get("port") == expected_port


def optional_port_ok(data: dict, expected_port: str) -> bool:
    port = data.get("port")
    return port in (None, expected_port)


def nested_flag_true(data: dict, *flags: str) -> bool:
    for candidate in iter_nested_dicts(data):
        for flag in flags:
            if candidate.get(flag) is True:
                return True
    return False


def nested_terminal_host_timeout(data: dict) -> bool:
    return any(
        candidate.get("code")
        in {
            "TIMEOUT",
            "UNEXPECTED_RESTART",
            "SKIPPED_AFTER_TIMEOUT",
            "SKIPPED_AFTER_UNEXPECTED_RESTART",
        }
        for candidate in iter_nested_dicts(data)
    )


EXPECTED_REBOOT_BOOT_HELP_FIELD = "expected_boot_help_after_reboot"


def command_result_transcript_aligned(data: dict) -> bool:
    commands = data.get("commands")
    results = data.get("results")
    if (
        not isinstance(commands, list)
        or not isinstance(results, list)
        or len(commands) != len(results)
        or not commands
    ):
        return False
    for command, result in zip(commands, results):
        if not isinstance(command, str) or not isinstance(result, dict):
            return False
        result_command = result.get("cmd")
        if not isinstance(result_command, str) or not result_command:
            return False
        if command != result_command and not command.startswith(result_command + " "):
            return False
    return True


def expected_reboot_boot_help_dict_ids(data: dict) -> set[int]:
    """Return the only boot-help dictionaries allowed at a planned reboot.

    The evidence producer may annotate the first successful storage response
    immediately after its own successful reboot command.  The raw boot marker
    remains present, but no annotation elsewhere (or without complete reboot
    proof) is trusted by the release gate.
    """
    results = data.get("results")
    if not isinstance(results, list) or not command_result_transcript_aligned(data):
        return set()
    commands = data["commands"]
    marked = [
        (index, result)
        for index, result in enumerate(results)
        if isinstance(result, dict)
        and result.get(EXPECTED_REBOOT_BOOT_HELP_FIELD) is True
    ]
    if len(marked) != 1:
        return set()
    index, result = marked[0]
    if index == 0 or not isinstance(results[index - 1], dict):
        return set()
    reboot = results[index - 1]
    ignored_count = result.get("ignored_json_count")
    if not (
        data.get("pre_sequence_complete") is True
        and data.get("post_sequence_complete") is True
        and data.get("reboot_command_passed") is True
        and data.get("reboot_reset_scope") == "system"
        and data.get("reboot_connectivity_prepare") == "ESP_OK"
        and data.get("reboot_route_flush") == "ESP_OK"
        and data.get("reboot_storage_manager_quiesced") is True
        and data.get("reboot_retained_worker_quiesced") is True
        and data.get("reboot_rp2040_bridge_quiesced") is True
        and data.get("reboot_nonce_proven") is True
        and data.get("reboot_proven") is True
        and data.get("post_reboot_reset_reason") == "SW"
        and commands[index - 1] == "reboot"
        and commands[index] == "storage status"
        and reboot.get("ok") is True
        and reboot.get("cmd") == "reboot"
        and reboot.get("rebooting") is True
        and reboot.get("reset_scope") == "system"
        and reboot.get("storage_manager_quiesced") is True
        and reboot.get("retained_worker_quiesced") is True
        and reboot.get("rp2040_bridge_quiesced") is True
        and reboot.get("connectivity_prepare") == "ESP_OK"
        and reboot.get("retained_flush") == "ESP_OK"
        and reboot.get("route_flush") == "ESP_OK"
        and result.get("ok") is True
        and result.get("cmd") == "storage status"
        and result.get("ignored_boot_help_seen") is True
        and type(ignored_count) is int
        and ignored_count == 1
        and result.get("ignored_json") == [{"cmd": "help", "ok": True}]
        and result.get("code") not in {
            "TIMEOUT",
            "UNEXPECTED_RESTART",
            "SKIPPED_AFTER_TIMEOUT",
            "SKIPPED_AFTER_UNEXPECTED_RESTART",
        }
    ):
        return set()
    allowed = {id(result)}
    allowed.update(
        id(ignored)
        for ignored in (result.get("ignored_json") or [])
        if isinstance(ignored, dict) and ignored.get("cmd") == "help"
    )
    return allowed


def nested_unexpected_console_restart(
    data: dict, *, allow_expected_planned_reboot: bool = False
) -> bool:
    allowed_expected_ids = (
        expected_reboot_boot_help_dict_ids(data)
        if allow_expected_planned_reboot
        else set()
    )
    for candidate in iter_nested_dicts(data):
        if EXPECTED_REBOOT_BOOT_HELP_FIELD in candidate:
            if (
                candidate.get(EXPECTED_REBOOT_BOOT_HELP_FIELD) is not True
                or id(candidate) not in allowed_expected_ids
            ):
                return True
        if (
            candidate.get("ignored_boot_help_seen") is True
            or candidate.get("cmd") == "help"
        ) and id(candidate) not in allowed_expected_ids:
            return True
        if "ignored_json_count" in candidate:
            count = candidate.get("ignored_json_count")
            if isinstance(count, bool) or not isinstance(count, int) or count < 0:
                return True
            if count > 0 and not isinstance(
                candidate.get("ignored_boot_help_seen"), bool
            ):
                # Current evidence producers preserve this boolean even when
                # the boot marker has fallen out of the last-five JSON tail.
                # Older/malformed evidence cannot prove that did not happen.
                return True
    return False


def nested_command_startswith(data: dict, prefix: str) -> bool:
    normalized = prefix.strip().lower()
    commands = data.get("commands")
    if isinstance(commands, list) and any(
        isinstance(command, str)
        and command.strip().lower().startswith(normalized)
        for command in commands
    ):
        return True
    return any(
        isinstance(candidate.get(field), str)
        and candidate[field].strip().lower().startswith(normalized)
        for candidate in iter_nested_dicts(data)
        for field in ("cmd", "command")
    )


def report_is_no_rf_no_format(data: dict) -> bool:
    return (
        data.get("public_rf_tx") is False
        and data.get("formats_sd") is False
        and not nested_flag_true(data, "public_rf_tx", "formats_sd", "format_performed", "format_confirmed")
    )


def unsafe_sd_commands(data: dict) -> list[str]:
    commands = data.get("commands")
    if not isinstance(commands, list):
        return []
    unsafe: list[str] = []
    for command in commands:
        if not isinstance(command, str):
            continue
        lowered = command.lower()
        if any(token in lowered for token in UNSAFE_SD_COMMAND_TOKENS):
            unsafe.append(command)
    return unsafe


def storage_file_gate_ready_status(storage_status: dict) -> bool:
    if not isinstance(storage_status, dict):
        return False
    sd = storage_status.get("sd")
    if not isinstance(sd, dict):
        return False
    return (
        sd.get("status_stale") is False
        and sd.get("presence_stale") is False
        and int_value(sd.get("refresh_failures")) == 0
        and sd.get("state") == "ready"
        and sd.get("filesystem") == "fat32"
        and sd.get("present") is True
        and sd.get("mounted") is True
        and sd.get("data_root_ready") is True
        and sd.get("rp2040_protocol_supported") is True
        and sd.get("file_ops") is True
        and sd.get("atomic_rename") is True
        and all(int_value(sd.get(key)) is not None and int_value(sd.get(key)) >= minimum for key, minimum in SD_FILE_LIMITS.items())
    )


def retained_history_sd_ready_status(storage_status: dict) -> bool:
    if not isinstance(storage_status, dict):
        return False
    manager = storage_status.get("manager")
    stores = storage_status.get("stores")
    retained_nvs = storage_status.get("retained_nvs")
    retained_sd = storage_status.get("retained_sd")
    retained_store_stats = retained_sd.get("stores") if isinstance(retained_sd, dict) else None
    return (
        storage_file_gate_ready_status(storage_status)
        and storage_status.get("ok") is True
        and storage_status.get("data_enabled") is True
        and storage_status.get("data_backend") == "mixed"
        and all(
            storage_status.get(field) == "sd"
            for field in (
                "message_store_backend",
                "dm_store_backend",
                "route_store_backend",
                "packet_log_backend",
            )
        )
        and isinstance(stores, dict)
        and all(stores.get(name) == "sd" for name in ("messages", "dm", "routes", "packets"))
        and isinstance(retained_nvs, dict)
        and retained_nvs.get("partition") == "d1l_retained"
        and retained_nvs.get("marker_ready") is True
        and retained_nvs.get("markers_complete") is True
        and retained_nvs.get("anchor_ready") is True
        and retained_nvs.get("sentinel_ready") is True
        and retained_nvs.get("external_init_required") is False
        and retained_nvs.get("ready") is True
        and retained_nvs.get("init_error") == "ESP_OK"
        and retained_nvs.get("migration_error") == "ESP_OK"
        and isinstance(retained_sd, dict)
        and retained_sd.get("degraded") is False
        and retained_sd.get("backup_degraded") is False
        and isinstance(retained_store_stats, dict)
        and all(
            isinstance(retained_store_stats.get(name), dict)
            and retained_store_stats[name].get("nvs_mirror_last_error") == "ESP_OK"
            for name in ("messages", "dm", "routes", "packets")
        )
        and isinstance(manager, dict)
        and manager.get("running") is True
        and manager.get("state") == "READY_SD"
    )


def storage_status_fresh_status(storage_status: dict) -> bool:
    sd = storage_status.get("sd") if isinstance(storage_status, dict) else None
    return (
        isinstance(sd, dict)
        and sd.get("status_stale") is False
        and sd.get("presence_stale") is False
        and int_value(sd.get("refresh_failures")) == 0
    )


def storage_setup_no_format_policy(storage_setup: dict | None) -> bool:
    if not isinstance(storage_setup, dict):
        return False
    return (
        storage_setup.get("policy") == "no_device_format"
        and storage_setup.get("will_format") is False
        and storage_setup.get("format_performed") is False
    )


def sd_file_canary_artifact_ok(data: dict, expected_port: str) -> bool:
    return (
        data.get("mode") == "hardware"
        and exact_port_ok(data, expected_port)
        and report_is_no_rf_no_format(data)
        and not unsafe_sd_commands(data)
        and data.get("ok") is True
        and data.get("sequence_completed") is True
        and data.get("timed_out_command") is None
        and data.get("unexpected_console_restart") is False
        and not nested_terminal_host_timeout(data)
        and not nested_unexpected_console_restart(data)
        and data.get("canary_passed") is True
        and data.get("canary_unavailable_ok") is not True
        and data.get("allow_unavailable") is not True
        and data.get("storage_file_gate_ready_before") is True
        and data.get("storage_file_gate_ready_after") is True
        and storage_status_fresh_status(data.get("storage_before"))
        and storage_status_fresh_status(data.get("storage_after"))
    )


def reboot_transition_proven(data: dict) -> bool:
    before = data.get("pre_reboot_boot_nonce")
    after = data.get("post_reboot_boot_nonce")
    results = data.get("results")
    if not isinstance(results, list) or not command_result_transcript_aligned(data):
        return False
    reboot_indices = [
        index
        for index, result in enumerate(results)
        if isinstance(result, dict)
        and result.get("cmd") == "reboot"
        and result.get("ok") is True
        and result.get("rebooting") is True
        and result.get("reset_scope") == "system"
        and result.get("storage_manager_quiesced") is True
        and result.get("retained_worker_quiesced") is True
        and result.get("rp2040_bridge_quiesced") is True
        and result.get("connectivity_prepare") == "ESP_OK"
        and result.get("retained_flush") == "ESP_OK"
        and result.get("route_flush") == "ESP_OK"
    ]
    if len(reboot_indices) != 1:
        return False
    reboot_index = reboot_indices[0]
    pre_health = [
        result
        for result in results[:reboot_index]
        if isinstance(result, dict) and result.get("cmd") == "health"
    ]
    post_health = [
        result
        for result in results[reboot_index + 1 :]
        if isinstance(result, dict) and result.get("cmd") == "health"
    ]
    return (
        data.get("reboot_command_passed") is True
        and data.get("reboot_reset_scope") == "system"
        and data.get("reboot_connectivity_prepare") == "ESP_OK"
        and data.get("reboot_route_flush") == "ESP_OK"
        and data.get("reboot_storage_manager_quiesced") is True
        and data.get("reboot_retained_worker_quiesced") is True
        and data.get("reboot_rp2040_bridge_quiesced") is True
        and data.get("reboot_nonce_proven") is True
        and data.get("reboot_proven") is True
        and data.get("post_reboot_reset_reason") == "SW"
        and isinstance(before, int)
        and not isinstance(before, bool)
        and before != 0
        and isinstance(after, int)
        and not isinstance(after, bool)
        and after != 0
        and before != after
        and bool(pre_health)
        and bool(post_health)
        and pre_health[-1].get("ok") is True
        and post_health[-1].get("ok") is True
        and pre_health[-1].get("boot_nonce") == before
        and post_health[-1].get("boot_nonce") == after
        and post_health[-1].get("reset_reason") == "SW"
    )


def sd_retained_canary_artifact_ok(data: dict, expected_port: str) -> bool:
    return (
        data.get("mode") == "hardware"
        and exact_port_ok(data, expected_port)
        and report_is_no_rf_no_format(data)
        and not unsafe_sd_commands(data)
        and data.get("ok") is True
        and data.get("pre_sequence_complete") is True
        and data.get("post_sequence_complete") is True
        and data.get("timed_out_command") is None
        and data.get("unexpected_restart_before_reboot") is False
        and not nested_terminal_host_timeout(data)
        and not nested_unexpected_console_restart(
            data, allow_expected_planned_reboot=True
        )
        and data.get("allow_unavailable") is not True
        and data.get("retained_canary_passed") is True
        and data.get("filecanary_passed") is True
        and data.get("pre_reboot_readbacks_ok") is True
        and data.get("post_reboot_readbacks_ok") is True
        and data.get("health_ok") is True
        and data.get("storage_file_gate_ready_before") is True
        and data.get("storage_file_gate_ready_after") is True
        and data.get("retained_history_sd_ready_before") is True
        and data.get("retained_history_sd_ready_after_canary") is True
        and data.get("retained_history_sd_ready_after") is True
        and reboot_transition_proven(data)
        and retained_history_sd_ready_status(data.get("storage_before"))
        and retained_history_sd_ready_status(data.get("storage_after_canary"))
        and retained_history_sd_ready_status(data.get("storage_after"))
        and "reboot" in (data.get("commands") or [])
    )


def strict_reboot_remount_evidence_ok(
    data: dict, expected_commit: str | None
) -> bool:
    require_runner_source = expected_commit is not None
    commit = expected_commit or data.get("expected_firmware_commit")
    if not isinstance(commit, str) or re.fullmatch(r"[0-9a-fA-F]{40}", commit) is None:
        return False
    if not command_result_transcript_aligned(data):
        return False
    commands = data["commands"]
    results = data["results"]
    reboot_indices = [
        index
        for index, result in enumerate(results)
        if isinstance(result, dict) and result.get("cmd") == "reboot"
    ]
    if len(reboot_indices) != 1:
        return False
    reboot_index = reboot_indices[0]
    pre_commands = commands[:reboot_index]
    pre_results = results[:reboot_index]
    post_commands = commands[reboot_index + 1 :]
    post_results = results[reboot_index + 1 :]
    pre_versions = [
        result
        for result in pre_results
        if isinstance(result, dict) and result.get("cmd") == "version"
    ]
    post_versions = [
        result
        for result in post_results
        if isinstance(result, dict) and result.get("cmd") == "version"
    ]
    pre_crashlogs = [
        result
        for result in pre_results
        if isinstance(result, dict) and result.get("cmd") == "crashlog"
    ]
    post_crashlogs = [
        result
        for result in post_results
        if isinstance(result, dict) and result.get("cmd") == "crashlog"
    ]
    if not all(
        len(rows) == 1
        for rows in (pre_versions, post_versions, pre_crashlogs, post_crashlogs)
    ):
        return False
    version_results = [pre_versions[0], post_versions[0]]
    crashlog_results = [pre_crashlogs[0], post_crashlogs[0]]
    if not all(
        result.get("ok") is True
        and exact_full_commit(result.get("build_commit"), commit)
        for result in version_results
    ):
        return False
    token = data.get("token")
    if not isinstance(token, str) or not token:
        return False
    persistence_plan = persistence_readback_commands(token)
    pre_health_indices = [
        index for index, command in enumerate(pre_commands) if command == "health"
    ]
    post_version_indices = [
        index for index, command in enumerate(post_commands) if command == "version"
    ]
    post_crashlog_indices = [
        index for index, command in enumerate(post_commands) if command == "crashlog"
    ]
    if (
        len(pre_health_indices) != 1
        or len(post_version_indices) != 1
        or len(post_crashlog_indices) != 1
        or post_crashlog_indices[0] != post_version_indices[0] + 1
    ):
        return False
    pre_persistence_tail = pre_commands[pre_health_indices[0] + 1 :]
    post_persistence_tail = post_commands[post_crashlog_indices[0] + 1 :]
    if not all(
        len(tail) >= len(persistence_plan)
        and len(tail) % len(persistence_plan) == 0
        and all(
            tail[index : index + len(persistence_plan)] == persistence_plan
            for index in range(0, len(tail), len(persistence_plan))
        )
        for tail in (pre_persistence_tail, post_persistence_tail)
    ):
        return False
    pre_poll_cohorts = len(pre_persistence_tail) // len(persistence_plan)
    post_poll_cohorts = len(post_persistence_tail) // len(persistence_plan)
    final_pre_persistence = dict(
        zip(persistence_plan, pre_results[-len(persistence_plan) :])
    )
    final_persistence = dict(
        zip(persistence_plan, results[-len(persistence_plan) :])
    )
    git = data.get("git")
    return (
        exact_full_commit(data.get("expected_firmware_commit"), commit)
        and (
            not require_runner_source
            or (
                data.get("kind") == "sd_reboot_remount_acceptance_d1l"
                and data.get("physical_observed") is True
                and data.get("dry_run") is False
                and data.get("simulated") is False
                and data.get("manual_only") is False
                and exact_full_commit(data.get("commit"), commit)
                and isinstance(git, dict)
                and exact_full_commit(git.get("commit"), commit)
                and git.get("status_ok") is True
                and git.get("dirty") is False
                and git.get("dirty_entries") == []
            )
        )
        and exact_full_commit(data.get("pre_device_build_commit"), commit)
        and exact_full_commit(data.get("device_build_commit"), commit)
        and data.get("firmware_identity_required") is True
        and data.get("pre_firmware_identity_ok") is True
        and data.get("post_firmware_identity_ok") is True
        and data.get("firmware_identity_ok") is True
        and data.get("persistence_clean_required") is True
        and data.get("pre_reboot_persistence_checked") is True
        and data.get("pre_reboot_persistence_clean") is True
        and data.get("pre_reboot_pending_dirty") is False
        and type(data.get("pre_reboot_persistence_poll_attempts_used")) is int
        and data.get("pre_reboot_persistence_poll_attempts_used") == pre_poll_cohorts
        and set(final_pre_persistence) == set(persistence_plan)
        and persistence_snapshot_clean(final_pre_persistence, token)
        and data.get("pre_reboot_persistence") == final_pre_persistence
        and data.get("pre_reboot_gate_passed") is True
        and data.get("reboot_attempted") is True
        and data.get("reboot_skipped_reason") is None
        and data.get("post_reboot_persistence_checked") is True
        and data.get("post_reboot_persistence_clean") is True
        and data.get("post_reboot_pending_dirty") is False
        and type(data.get("persistence_poll_attempts_used")) is int
        and data.get("persistence_poll_attempts_used") == post_poll_cohorts
        and set(final_persistence) == set(persistence_plan)
        and persistence_snapshot_clean(final_persistence, token)
        and data.get("post_reboot_persistence") == final_persistence
        and data.get("crashlog_transition_required") is True
        and data.get("crashlog_transition_ok") is True
        and data.get("crashlog_before_reboot") == crashlog_results[0]
        and data.get("crashlog_after_reboot") == crashlog_results[1]
        and crashlog_transition_passed(crashlog_results[0], crashlog_results[1])
        and data.get("reboot_retained_flush") == "ESP_OK"
    )


def sd_reboot_remount_artifact_ok(
    data: dict, expected_port: str, expected_commit: str | None = None
) -> bool:
    return (
        data.get("mode") == "hardware"
        and exact_port_ok(data, expected_port)
        and report_is_no_rf_no_format(data)
        and not unsafe_sd_commands(data)
        and data.get("ok") is True
        and data.get("pre_sequence_complete") is True
        and data.get("post_sequence_complete") is True
        and data.get("timed_out_command") is None
        and data.get("unexpected_restart_before_reboot") is False
        and not nested_terminal_host_timeout(data)
        and not nested_unexpected_console_restart(
            data, allow_expected_planned_reboot=True
        )
        and data.get("pre_remount_ready") is True
        and data.get("post_remount_ready") is True
        and data.get("pre_remount_command_passed") is True
        and data.get("post_remount_command_passed") is True
        and data.get("pre_remount_retained_worker_quiesce_acquired") is True
        and data.get("post_remount_retained_worker_quiesce_acquired") is True
        and data.get("retained_history_sd_ready_before") is True
        and data.get("retained_history_sd_ready_after") is True
        and data.get("filecanary_passed") is True
        and data.get("retained_canary_passed") is True
        and data.get("pre_reboot_readbacks_ok") is True
        and data.get("post_reboot_readbacks_ok") is True
        and data.get("health_ok") is True
        and data.get("pre_map_tile_canary_passed") is True
        and data.get("post_map_tile_canary_passed") is True
        and reboot_transition_proven(data)
        and retained_history_sd_ready_status(data.get("storage_before_reboot"))
        and retained_history_sd_ready_status(data.get("storage_after_reboot"))
        and storage_status_fresh_status(data.get("storage_before_reboot"))
        and storage_status_fresh_status(data.get("storage_after_reboot"))
        and "reboot" in (data.get("commands") or [])
        and strict_reboot_remount_evidence_ok(data, expected_commit)
    )


def sd_map_tile_canary_artifact_ok(data: dict, expected_port: str) -> bool:
    return (
        data.get("mode") == "hardware"
        and exact_port_ok(data, expected_port)
        and report_is_no_rf_no_format(data)
        and not unsafe_sd_commands(data)
        and data.get("ok") is True
        and data.get("canary_passed") is True
        and data.get("canary_unavailable_ok") is not True
        and data.get("allow_unavailable") is not True
        and data.get("storage_file_gate_ready_before") is True
        and data.get("storage_file_gate_ready_after") is True
        and data.get("map_tile_backend_ready_before") is True
        and data.get("map_tile_backend_ready_after") is True
        and storage_status_fresh_status(data.get("storage_before"))
        and storage_status_fresh_status(data.get("storage_after"))
    )


def sd_export_artifact_common_ok(data: dict, expected_port: str, passed_field: str, unavailable_field: str) -> bool:
    return (
        data.get("mode") == "hardware"
        and exact_port_ok(data, expected_port)
        and report_is_no_rf_no_format(data)
        and not unsafe_sd_commands(data)
        and data.get("ok") is True
        and data.get("allow_unavailable") is not True
        and data.get(passed_field) is True
        and data.get(unavailable_field) is not True
        and data.get("storage_file_gate_ready_before") is True
        and data.get("storage_file_gate_ready_after") is True
        and data.get("export_backend_ready_before") is True
        and data.get("export_backend_ready_after") is True
        and storage_status_fresh_status(data.get("storage_before"))
        and storage_status_fresh_status(data.get("storage_after"))
    )


def sd_export_canary_artifact_ok(data: dict, expected_port: str) -> bool:
    return sd_export_artifact_common_ok(data, expected_port, "canary_passed", "canary_unavailable_ok")


def sd_diagnostic_export_artifact_ok(data: dict, expected_port: str) -> bool:
    export = data.get("export") if isinstance(data.get("export"), dict) else {}
    return (
        sd_export_artifact_common_ok(
            data,
            expected_port,
            "diagnostic_export_passed",
            "diagnostic_export_unavailable_ok",
        )
        and int_value(export.get("bytes")) is not None
        and int_value(export.get("bytes")) > 192
        and int_value(export.get("chunks_written")) is not None
        and int_value(export.get("chunks_written")) >= 2
        and int_value(export.get("final_verified_bytes")) == int_value(export.get("bytes"))
    )


def sd_data_export_artifact_ok(data: dict, expected_port: str) -> bool:
    export = data.get("export") if isinstance(data.get("export"), dict) else {}
    return (
        sd_export_artifact_common_ok(data, expected_port, "data_export_passed", "data_export_unavailable_ok")
        and export.get("private_identity_exported") is False
        and export.get("sampled") is True
        and int_value(export.get("bytes")) is not None
        and int_value(export.get("bytes")) > 192
        and int_value(export.get("chunks_written")) is not None
        and int_value(export.get("chunks_written")) >= 2
        and int_value(export.get("final_verified_bytes")) == int_value(export.get("bytes"))
    )


def raw_diag_line(data: dict) -> str:
    for key in ("raw_line", "diag"):
        value = data.get(key)
        if isinstance(value, str) and "DESKOS_SD_DIAG" in value:
            return value
    lines = data.get("lines")
    if isinstance(lines, list):
        for line in lines:
            if isinstance(line, str) and "DESKOS_SD_DIAG" in line:
                return line
    storage_diag = data.get("storage_diag")
    if isinstance(storage_diag, dict):
        value = storage_diag.get("raw_line")
        if isinstance(value, str) and "DESKOS_SD_DIAG" in value:
            return value
    return ""


def raw_diag_fields(data: dict) -> dict[str, str]:
    fields = data.get("fields")
    if isinstance(fields, dict):
        return {str(key): str(value) for key, value in fields.items()}
    line = raw_diag_line(data)
    parsed: dict[str, str] = {}
    for token in line.split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        parsed[key] = value
    return parsed


def raw_diag_complete(data: dict, expected_port: str) -> bool:
    fields = raw_diag_fields(data)
    required_fields = {"pins", "hz", "pin_sck", "pin_mosi", "pin_miso", "pin_cs", "selected_power", "selected_mode"}
    required_fields.update(f"{prefix}{suffix}" for prefix in RAW_DIAG_PROBE_PREFIXES for suffix in RAW_DIAG_PROBE_SUFFIXES)
    return (
        data.get("ok") is True
        and optional_port_ok(data, expected_port)
        and report_is_no_rf_no_format(data)
        and data.get("response_truncated") is not True
        and raw_diag_line(data).startswith("DESKOS_SD_DIAG")
        and required_fields.issubset(fields)
    )


def sd_boot_prepare_artifact_ok(data: dict, scenario: str, expected_port: str) -> bool:
    classification = data.get("classification")
    storage_after = data.get("storage_after") if isinstance(data.get("storage_after"), dict) else {}
    storage_remount = (
        data.get("storage_remount")
        if isinstance(data.get("storage_remount"), dict)
        else {}
    )
    remount_receipt_ok = scenario == "rp2040-unavailable" or (
        data.get("retained_worker_quiesce_acquired") is True
        and storage_remount.get("ok") is True
        and storage_remount.get("cmd") == "storage remount"
        and storage_remount.get("retained_worker_quiesce_acquired") is True
    )
    file_gate_ok = storage_file_gate_ready_status(storage_after)
    format_policy_ok = (
        scenario not in {"unformatted", "existing-data"}
        or data.get("format_allowed") is False
        or storage_setup_no_format_policy(data.get("storage_setup") if isinstance(data.get("storage_setup"), dict) else None)
    )
    if scenario in {"correct-structure", "missing-structure"}:
        format_policy_ok = format_policy_ok and file_gate_ok and data.get("retained_store_gate_ready") is True
        format_policy_ok = format_policy_ok and data.get("filecanary_passed") is True
    if scenario == "unformatted":
        format_policy_ok = format_policy_ok and storage_setup_no_format_policy(
            data.get("storage_setup") if isinstance(data.get("storage_setup"), dict) else None
        )
    if scenario == "rp2040-unavailable":
        sd = storage_after.get("sd") if isinstance(storage_after.get("sd"), dict) else {}
        stores = storage_after.get("stores") if isinstance(storage_after.get("stores"), dict) else {}
        prerequisite = (
            data.get("scenario_prerequisite")
            if isinstance(data.get("scenario_prerequisite"), dict)
            else {}
        )
        format_policy_ok = format_policy_ok and (
            prerequisite.get("satisfied") is True
            and sd.get("state") in {"rp2040_unavailable", "bridge_unavailable", "protocol_pending"}
            and sd.get("rp2040_protocol_supported") is False
            and sd.get("mounted") is False
            and sd.get("data_root_ready") is False
            and sd.get("file_ops") is False
            and sd.get("atomic_rename") is False
            and storage_after.get("data_enabled") is False
            and storage_after.get("data_backend") == "nvs"
            and all(stores.get(name) == "nvs" for name in ("messages", "dm", "routes", "packets"))
            and data.get("storage_file_gate_ready") is False
            and data.get("retained_store_gate_ready") is False
        )
    return (
        data.get("mode") == "hardware"
        and exact_port_ok(data, expected_port)
        and data.get("scenario") == scenario
        and data.get("ok") is True
        and report_is_no_rf_no_format(data)
        and data.get("commands_safe") is True
        and not unsafe_sd_commands(data)
        and classification in SD_BOOT_PREPARE_EXPECTED_CLASSIFICATIONS[scenario]
        and remount_receipt_ok
        and format_policy_ok
    )


def sd_power_rail_evidence_ok(data: dict, expected_port: str) -> bool:
    vcc = first_numeric_field(
        data,
        "vcc_at_socket_volts",
        "sd_vcc_volts",
        "vcc_after_gpio18_high_volts",
        "sd_power_rail_volts",
    )
    vcc_ok = vcc is not None and 3.0 <= vcc <= 3.6
    return (
        data.get("ok") is True
        and optional_port_ok(data, expected_port)
        and report_is_no_rf_no_format(data)
        and (data.get("mode") in (None, "hardware", "hardware-electrical", "electrical"))
        and (data.get("sd_power_rail_measured") is True or bool_field(data, "sd_power_rail_measured", "power_rail_measured"))
        and vcc_ok
        and bool_field(data, "gpio18_high", "gpio18_asserted")
        and bool_field(data, "gnd_continuity", "ground_continuity_ok")
        and bool_field(data, "cs_toggles_at_socket", "cs_toggle_seen")
        and bool_field(data, "sck_clocks_at_socket", "sck_clocks_seen")
        and bool_field(data, "mosi_commands_at_socket", "mosi_commands_seen")
        and bool_field(data, "miso_response_or_idle_high", "miso_idle_high", "miso_response_seen")
        and bool(data.get("sku") or data.get("board_revision") or data.get("board"))
    )


def card_capacity_gb(card: dict) -> float | None:
    for key in ("capacity_gb", "card_gb", "size_gb", "max_card_gb"):
        value = first_number(card.get(key))
        if value is not None:
            return value
    return None


def card_is_fat32(card: dict) -> bool:
    fat_type = int_value(card.get("fat_type"))
    if fat_type == 32:
        return True
    for key in ("fs", "format", "filesystem"):
        value = card.get(key)
        if isinstance(value, str) and value.lower() == "fat32":
            return True
    return False


def card_matrix_entries(data: dict) -> list[dict]:
    for key in ("cards", "matrix", "results"):
        value = data.get(key)
        if isinstance(value, list):
            return [item for item in value if isinstance(item, dict)]
    return []


def sd_matrix_artifact_ok(data: dict, expected_port: str) -> bool:
    cards = card_matrix_entries(data)
    capacities = [card_capacity_gb(card) for card in cards]
    return (
        data.get("ok") is True
        and optional_port_ok(data, expected_port)
        and report_is_no_rf_no_format(data)
        and not unsafe_sd_commands(data)
        and len(cards) >= 3
        and all(capacity is not None and capacity <= 32 for capacity in capacities)
        and any(capacity == 32 for capacity in capacities)
        and all(card_is_fat32(card) for card in cards)
        and all(card.get("ok") is True or card.get("official_seeed_smoke_passed") is True for card in cards)
        and all(card.get("formats_sd") is not True and card.get("public_rf_tx") is not True for card in cards)
    )


def official_seeed_sd_smoke_ok(data: dict, expected_port: str) -> bool:
    payload = first_official_seeed_smoke_payload(data)
    required_steps = ("mount", "root_open", "mkdir", "write", "read", "rename", "stat", "delete")
    max_card_gb = int_value(payload.get("max_card_gb"))
    fat_type = int_value(payload.get("fat_type"))
    port = data.get("port")
    port_ok = port in (None, expected_port)
    return (
        port_ok
        and payload.get("ok") is True
        and all(payload.get(step) is True for step in required_steps)
        and payload.get("public_rf_tx") is False
        and payload.get("formats_sd") is False
        and payload.get("format") == "non_destructive"
        and max_card_gb is not None
        and max_card_gb <= 32
        and fat_type == 32
    )


def official_seeed_sd_smoke_gate(
    artifact_roots: list[Path],
    root: Path,
    commit: str | None,
    expected_port: str,
) -> GateResult:
    smoke = newest_commit_json_from_roots(
        artifact_roots,
        commit,
        "seeed_official_sd_smoke_*.json",
        "sd_official_seeed_smoke_*.json",
        "rp2040_seeed_official_sd_smoke_*.json",
    )
    data = read_json(smoke)
    payload = first_official_seeed_smoke_payload(data)
    ok = bool(smoke and data and official_seeed_sd_smoke_ok(data, expected_port))
    required_steps = ("mount", "root_open", "mkdir", "write", "read", "rename", "stat", "delete")
    raw_fields = (
        "diag_ran",
        "raw_present",
        "raw_cmd8_echo_ok",
        "raw_acmd41_ready",
        "raw_err",
        "raw_data",
        "raw_miso_pullup",
        "raw_miso_idle",
        "raw_idle_ff",
        "raw_cmd0_ready",
        "raw_cmd0",
        "raw_cmd8_ready",
        "raw_cmd8",
        "raw_r70",
        "raw_r71",
        "raw_r72",
        "raw_r73",
        "raw_cmd55",
        "raw_acmd41",
        "raw_acmd41_attempts",
        "raw_ocr0",
        "raw_ocr1",
        "raw_ocr2",
        "raw_ocr3",
    )
    return GateResult(
        "sd_official_seeed_smoke_passed",
        "P0",
        ok,
        "Official Seeed RP2040 SD smoke",
        [rel(smoke, root)] if smoke else [],
        "Official Seeed SD smoke proves mount, root open, write/read/rename/stat/delete, FAT32, <=32GB, and no formatting."
        if ok else "No passing current-commit official Seeed SD smoke artifact was found.",
        {
            "path_found": bool(smoke),
            "artifact_ok": data.get("ok") if data else None,
            "inner_test": payload.get("test") if payload else None,
            "inner_ok": payload.get("ok") if payload else None,
            "port": data.get("port") if data else None,
            "required_steps": {step: payload.get(step) for step in required_steps} if payload else {},
            "fat_type": payload.get("fat_type") if payload else None,
            "max_card_gb": payload.get("max_card_gb") if payload else None,
            "fat32": payload.get("fat32") if payload else None,
            "needs_fat32": payload.get("needs_fat32") if payload else None,
            "will_format": payload.get("will_format") if payload else None,
            "format_performed": payload.get("format_performed") if payload else None,
            "detect_used_for_ok": payload.get("detect_used_for_ok") if payload else None,
            "power_measured": payload.get("power_measured") if payload else None,
            "power_state": payload.get("power_state") if payload else None,
            "detect": payload.get("detect") if payload else None,
            "detect_pullup": payload.get("detect_pullup") if payload else None,
            "detect_pulldown": payload.get("detect_pulldown") if payload else None,
            "raw_diagnostics": {field: payload.get(field) for field in raw_fields} if payload else {},
            "formats_sd": payload.get("formats_sd") if payload else data.get("formats_sd") if data else None,
            "public_rf_tx": payload.get("public_rf_tx") if payload else data.get("public_rf_tx") if data else None,
        },
    )


def sd_next_action(preflight: dict, classification: dict) -> str:
    value = classification.get("next_action")
    if value is None:
        value = preflight.get("next_action")
    return str(value or "")


def sd_next_action_is_obsolete(next_action: str) -> bool:
    lowered = next_action.lower()
    return any(token in lowered for token in OBSOLETE_SD_NEXT_ACTION_TOKENS)


def sanitized_sd_classification(preflight: dict, classification: dict) -> tuple[dict, bool]:
    sanitized = dict(classification)
    raw_next_action = sd_next_action(preflight, classification)
    obsolete = sd_next_action_is_obsolete(raw_next_action)
    if obsolete:
        sanitized["next_action"] = SAFE_SD_NEXT_ACTION
        sanitized["policy"] = "no_device_format"
        sanitized["obsolete_format_action_blocked"] = True
    return sanitized, obsolete


def sd_gate(preflight_path: Path | None, root: Path) -> GateResult:
    preflight = read_json(preflight_path)
    classification = preflight.get("classification") if isinstance(preflight.get("classification"), dict) else {}
    classification, obsolete_format_action = sanitized_sd_classification(preflight, classification)
    ready = (
        preflight.get("ready_for_sd_acceptance") is True
        and classification.get("storage_file_gate_ready") is True
        and storage_file_gate_ready_status(preflight.get("storage_status"))
    )
    no_device_format_policy_ok = preflight.get("formats_sd") is False and not obsolete_format_action
    ok = bool(preflight_path and preflight.get("ok") is True and ready and no_device_format_policy_ok)
    details = {
        "ready_for_sd_acceptance": preflight.get("ready_for_sd_acceptance"),
        "classification": classification,
        "candidate_volume_count": len(preflight.get("candidate_volumes") or []),
        "artifact_sha256": (preflight.get("artifact") or {}).get("sha256"),
        "formats_sd": preflight.get("formats_sd"),
        "public_rf_tx": preflight.get("public_rf_tx"),
        "copies_uf2": preflight.get("copies_uf2"),
        "no_device_format_policy_ok": no_device_format_policy_ok,
        "obsolete_format_action_blocked": obsolete_format_action,
    }
    if obsolete_format_action:
        message = (
            "SD matrix is not release-ready; stale format-capable SD evidence is obsolete. "
            "Refresh under the no-device-format policy with a FAT32 card prepared on a computer."
        )
    elif ok:
        message = "SD preflight is ready for acceptance matrix; run and attach the full matrix artifacts."
    else:
        message = "SD matrix is not release-ready; RP2040/SD file gate or follow-up matrix evidence is missing."
    return GateResult(
        "sd_acceptance_matrix",
        "P0",
        ok,
        "SD auto-prepare, retained stores, exports, map tiles, and reboot/remount",
        [rel(preflight_path, root)] if preflight_path else [],
        message,
        details,
    )


def sd_filecanary_gate(artifact_roots: list[Path], root: Path, commit: str | None, expected_port: str) -> GateResult:
    canary = newest_commit_json_from_roots(
        artifact_roots,
        commit,
        "d1l-sd-file-canary-*.json",
        "sd_file_canary_*.json",
        "sd_filecanary_*.json",
        "filecanary_*.json",
    )
    data = read_json(canary)
    ok = bool(canary and data and sd_file_canary_artifact_ok(data, expected_port))
    return GateResult(
        "sd_filecanary_independent",
        "P0",
        ok,
        "Independent RP2040 SD file-operation canary",
        [rel(canary, root)] if canary else [],
        "Raw RP2040 file-operation canary passes without requiring retained-store migration."
        if ok else "No passing current-commit hardware SD file-operation canary artifact was found.",
        {
            "path_found": bool(canary),
            "artifact_ok": data.get("ok") if data else None,
            "mode": data.get("mode") if data else None,
            "port": data.get("port") if data else None,
            "canary_passed": data.get("canary_passed") if data else None,
            "storage_file_gate_ready_before": data.get("storage_file_gate_ready_before") if data else None,
            "storage_file_gate_ready_after": data.get("storage_file_gate_ready_after") if data else None,
            "retained_history_sd_ready_before": data.get("retained_history_sd_ready_before") if data else None,
            "retained_history_sd_ready_after": data.get("retained_history_sd_ready_after") if data else None,
        },
    )


def sd_retained_canary_gate(artifact_roots: list[Path], root: Path, commit: str | None, expected_port: str) -> GateResult:
    retained = newest_commit_json_from_roots(
        artifact_roots,
        commit,
        "d1l-sd-retained-history-*.json",
        "sd_retained_history_*.json",
        "sd_retained_canary_*.json",
        "retained_history_*.json",
    )
    data = read_json(retained)
    ok = bool(retained and data and sd_retained_canary_artifact_ok(data, expected_port))
    return GateResult(
        "sd_retained_canary_passed",
        "P0",
        ok,
        "SD retained-history canary",
        [rel(retained, root)] if retained else [],
        "Retained Public/DM/routes/packet canary survives reboot and readback without Public RF or formatting."
        if ok else "No passing current-commit hardware retained-history SD canary artifact was found.",
        {
            "path_found": bool(retained),
            "artifact_ok": data.get("ok") if data else None,
            "mode": data.get("mode") if data else None,
            "port": data.get("port") if data else None,
            "retained_canary_passed": data.get("retained_canary_passed") if data else None,
            "filecanary_passed": data.get("filecanary_passed") if data else None,
            "storage_file_gate_ready_before": data.get("storage_file_gate_ready_before") if data else None,
            "storage_file_gate_ready_after": data.get("storage_file_gate_ready_after") if data else None,
            "retained_history_sd_ready_before": data.get("retained_history_sd_ready_before") if data else None,
            "retained_history_sd_ready_after": data.get("retained_history_sd_ready_after") if data else None,
            "pre_reboot_readbacks_ok": data.get("pre_reboot_readbacks_ok") if data else None,
            "post_reboot_readbacks_ok": data.get("post_reboot_readbacks_ok") if data else None,
            "reboot_command_passed": data.get("reboot_command_passed") if data else None,
            "reboot_reset_scope": data.get("reboot_reset_scope") if data else None,
            "reboot_connectivity_prepare": data.get("reboot_connectivity_prepare") if data else None,
            "reboot_route_flush": data.get("reboot_route_flush") if data else None,
            "reboot_retained_worker_quiesced": data.get("reboot_retained_worker_quiesced") if data else None,
            "pre_reboot_boot_nonce": data.get("pre_reboot_boot_nonce") if data else None,
            "post_reboot_boot_nonce": data.get("post_reboot_boot_nonce") if data else None,
            "reboot_proven": data.get("reboot_proven") if data else None,
        },
    )


def sd_reboot_remount_gate(artifact_roots: list[Path], root: Path, commit: str | None, expected_port: str) -> GateResult:
    remount = newest_commit_json_from_roots(
        artifact_roots,
        commit,
        "d1l-sd-reboot-remount-*.json",
        "sd_reboot_remount_*.json",
        "reboot_remount_*.json",
    )
    data = read_json(remount)
    ok = bool(
        remount
        and data
        and sd_reboot_remount_artifact_ok(data, expected_port, commit)
    )
    return GateResult(
        "sd_reboot_remount_passed",
        "P0",
        ok,
        "SD reboot/remount convergence",
        [rel(remount, root)] if remount else [],
        "SD retained data and map-tile canary survive reboot/remount without Public RF or formatting."
        if ok else "No passing current-commit hardware SD reboot/remount artifact was found.",
        {
            "path_found": bool(remount),
            "artifact_ok": data.get("ok") if data else None,
            "mode": data.get("mode") if data else None,
            "port": data.get("port") if data else None,
            "pre_remount_ready": data.get("pre_remount_ready") if data else None,
            "post_remount_ready": data.get("post_remount_ready") if data else None,
            "pre_remount_command_passed": data.get("pre_remount_command_passed") if data else None,
            "post_remount_command_passed": data.get("post_remount_command_passed") if data else None,
            "pre_remount_retained_worker_quiesce_acquired": data.get("pre_remount_retained_worker_quiesce_acquired") if data else None,
            "post_remount_retained_worker_quiesce_acquired": data.get("post_remount_retained_worker_quiesce_acquired") if data else None,
            "retained_history_sd_ready_before": data.get("retained_history_sd_ready_before") if data else None,
            "retained_history_sd_ready_after": data.get("retained_history_sd_ready_after") if data else None,
            "reboot_command_passed": data.get("reboot_command_passed") if data else None,
            "reboot_reset_scope": data.get("reboot_reset_scope") if data else None,
            "reboot_connectivity_prepare": data.get("reboot_connectivity_prepare") if data else None,
            "reboot_route_flush": data.get("reboot_route_flush") if data else None,
            "reboot_retained_flush": data.get("reboot_retained_flush") if data else None,
            "reboot_retained_worker_quiesced": data.get("reboot_retained_worker_quiesced") if data else None,
            "pre_reboot_boot_nonce": data.get("pre_reboot_boot_nonce") if data else None,
            "post_reboot_boot_nonce": data.get("post_reboot_boot_nonce") if data else None,
            "reboot_proven": data.get("reboot_proven") if data else None,
            "firmware_identity_ok": data.get("firmware_identity_ok") if data else None,
            "pre_reboot_persistence_checked": data.get("pre_reboot_persistence_checked") if data else None,
            "pre_reboot_persistence_clean": data.get("pre_reboot_persistence_clean") if data else None,
            "pre_reboot_pending_dirty": data.get("pre_reboot_pending_dirty") if data else None,
            "post_reboot_persistence_clean": data.get("post_reboot_persistence_clean") if data else None,
            "post_reboot_pending_dirty": data.get("post_reboot_pending_dirty") if data else None,
            "crashlog_transition_ok": data.get("crashlog_transition_ok") if data else None,
            "pre_map_tile_canary_passed": data.get("pre_map_tile_canary_passed") if data else None,
            "post_map_tile_canary_passed": data.get("post_map_tile_canary_passed") if data else None,
        },
    )


def wp01_evidence_gate(
    wp01_dir: Path,
    root: Path,
    github_run_dir: Path | None,
    commit: str | None,
    github_run_id: str | None,
    d1l_port: str,
    rp2040_port: str,
) -> GateResult:
    paths: dict[str, Path | None] = {}
    checks: dict[str, bool] = {}
    hashes: dict[str, str] = {}
    details: dict[str, Any] = {}
    provenance_path = newest_commit_json(wp01_dir, commit, WP01_PROVENANCE_PATTERN)
    provenance = read_json(provenance_path)
    provenance_sha256 = sha256_file(provenance_path) if provenance_path else None
    provenance_ok = bool(
        provenance_path
        and provenance_sha256
        and github_run_dir
        and commit
        and github_run_id
        and wp01_provenance_receipt_ok(
            provenance,
            commit,
            github_run_id,
            d1l_port,
            rp2040_port,
            root=root,
            github_run_dir=github_run_dir,
        )
    )
    details["provenance_receipt"] = {
        "path_found": bool(provenance_path),
        "artifact_ok": provenance_ok,
        "path": rel(provenance_path, root) if provenance_path else None,
        "sha256": provenance_sha256,
    }

    for kind in WP01_ARTIFACT_KINDS:
        path = newest_commit_json(wp01_dir, commit, WP01_ARTIFACT_PATTERNS[kind])
        data = read_json(path)
        valid = bool(
            path
            and commit
            and github_run_id
            and wp01_artifact_ok(
                data,
                kind,
                commit,
                github_run_id,
                d1l_port,
                rp2040_port,
                provenance if provenance_ok else None,
                provenance_sha256 if provenance_ok else None,
                root,
            )
        )
        paths[kind] = path
        checks[kind] = valid
        if path:
            hashes[kind] = sha256_file(path)
        details[kind] = {
            "path_found": bool(path),
            "artifact_ok": valid,
            "path": rel(path, root) if path else None,
        }

    aggregate = newest_commit_json(wp01_dir, commit, "wp01_acceptance_*.json")
    aggregate_data = read_json(aggregate)
    aggregate_ok = bool(
        aggregate
        and commit
        and github_run_id
        and all(checks.values())
        and wp01_aggregate_artifact_ok(
            aggregate_data,
            commit,
            github_run_id,
            d1l_port,
            rp2040_port,
            hashes,
            provenance_sha256,
            provenance if provenance_ok else None,
        )
    )
    details["aggregate"] = {
        "path_found": bool(aggregate),
        "artifact_ok": aggregate_ok,
        "path": rel(aggregate, root) if aggregate else None,
    }
    ok = provenance_ok and all(checks.values()) and aggregate_ok
    evidence = [rel(path, root) for path in paths.values() if path]
    if provenance_path:
        evidence.insert(0, rel(provenance_path, root))
    if aggregate:
        evidence.append(rel(aggregate, root))
    return GateResult(
        "wp01_exact_pair_storage_reboot",
        "P0",
        ok,
        "WP-01 exact-pair storage, reboot, removal, and soak qualification",
        evidence,
        "All four exact-commit WP-01 physical artifacts and their hash-bound aggregate pass."
        if ok
        else "WP-01 remains open; one or more exact-commit physical artifacts or the hash-bound aggregate is missing or invalid.",
        details,
    )


def sd_map_tile_canary_gate(artifact_roots: list[Path], root: Path, commit: str | None, expected_port: str) -> GateResult:
    canary = newest_commit_json_from_roots(
        artifact_roots,
        commit,
        "d1l-sd-map-tile-canary-*.json",
        "sd_map_tile_canary_*.json",
        "map_tile_canary_*.json",
    )
    data = read_json(canary)
    ok = bool(canary and data and sd_map_tile_canary_artifact_ok(data, expected_port))
    return GateResult(
        "sd_map_tile_canary_passed",
        "P0",
        ok,
        "SD map-tile cache canary",
        [rel(canary, root)] if canary else [],
        "Map-tile cache canary writes through temp/read/atomic-rename/final-read on SD."
        if ok else "No passing current-commit hardware SD map-tile canary artifact was found.",
        {
            "path_found": bool(canary),
            "artifact_ok": data.get("ok") if data else None,
            "mode": data.get("mode") if data else None,
            "port": data.get("port") if data else None,
            "canary_passed": data.get("canary_passed") if data else None,
            "map_tile_backend_ready_before": data.get("map_tile_backend_ready_before") if data else None,
            "map_tile_backend_ready_after": data.get("map_tile_backend_ready_after") if data else None,
        },
    )


def sd_export_canary_gate(artifact_roots: list[Path], root: Path, commit: str | None, expected_port: str) -> GateResult:
    canary = newest_commit_json_from_roots(
        artifact_roots,
        commit,
        "d1l-sd-export-canary-*.json",
        "sd_export_canary_*.json",
        "sd_export_canary*.json",
        "export_canary_*.json",
    )
    data = read_json(canary)
    ok = bool(canary and data and sd_export_canary_artifact_ok(data, expected_port))
    return GateResult(
        "sd_export_canary_passed",
        "P0",
        ok,
        "SD export-store canary",
        [rel(canary, root)] if canary else [],
        "Export-store canary writes through temp/read/atomic-rename/final-read on SD."
        if ok else "No passing current-commit hardware SD export canary artifact was found.",
        {
            "path_found": bool(canary),
            "artifact_ok": data.get("ok") if data else None,
            "mode": data.get("mode") if data else None,
            "port": data.get("port") if data else None,
            "canary_passed": data.get("canary_passed") if data else None,
            "storage_file_gate_ready_before": data.get("storage_file_gate_ready_before") if data else None,
            "storage_file_gate_ready_after": data.get("storage_file_gate_ready_after") if data else None,
            "export_backend_ready_before": data.get("export_backend_ready_before") if data else None,
            "export_backend_ready_after": data.get("export_backend_ready_after") if data else None,
        },
    )


def sd_diagnostic_export_gate(artifact_roots: list[Path], root: Path, commit: str | None, expected_port: str) -> GateResult:
    export = newest_commit_json_from_roots(
        artifact_roots,
        commit,
        "d1l-sd-diagnostic-export-*.json",
        "sd_diagnostic_export_*.json",
        "sd_diagnostic_export*.json",
        "diagnostic_export_*.json",
    )
    data = read_json(export)
    inner = data.get("export") if isinstance(data.get("export"), dict) else {}
    ok = bool(export and data and sd_diagnostic_export_artifact_ok(data, expected_port))
    return GateResult(
        "sd_diagnostic_export_passed",
        "P0",
        ok,
        "SD diagnostic export",
        [rel(export, root)] if export else [],
        "Diagnostic export writes a multi-chunk SD JSON export through atomic rename and verifies final bytes."
        if ok else "No passing current-commit hardware SD diagnostic export artifact was found.",
        {
            "path_found": bool(export),
            "artifact_ok": data.get("ok") if data else None,
            "mode": data.get("mode") if data else None,
            "port": data.get("port") if data else None,
            "diagnostic_export_passed": data.get("diagnostic_export_passed") if data else None,
            "bytes": inner.get("bytes") if inner else None,
            "chunks_written": inner.get("chunks_written") if inner else None,
            "final_verified_bytes": inner.get("final_verified_bytes") if inner else None,
        },
    )


def sd_data_export_gate(artifact_roots: list[Path], root: Path, commit: str | None, expected_port: str) -> GateResult:
    export = newest_commit_json_from_roots(
        artifact_roots,
        commit,
        "d1l-sd-data-export-*.json",
        "sd_data_export_*.json",
        "sd_data_export*.json",
        "data_export_*.json",
    )
    data = read_json(export)
    inner = data.get("export") if isinstance(data.get("export"), dict) else {}
    ok = bool(export and data and sd_data_export_artifact_ok(data, expected_port))
    return GateResult(
        "sd_data_export_passed",
        "P0",
        ok,
        "SD sampled data export",
        [rel(export, root)] if export else [],
        "Sampled data export writes a multi-chunk SD JSON export without private identity material."
        if ok else "No passing current-commit hardware SD sampled data export artifact was found.",
        {
            "path_found": bool(export),
            "artifact_ok": data.get("ok") if data else None,
            "mode": data.get("mode") if data else None,
            "port": data.get("port") if data else None,
            "data_export_passed": data.get("data_export_passed") if data else None,
            "private_identity_exported": inner.get("private_identity_exported") if inner else None,
            "sampled": inner.get("sampled") if inner else None,
            "bytes": inner.get("bytes") if inner else None,
            "chunks_written": inner.get("chunks_written") if inner else None,
            "final_verified_bytes": inner.get("final_verified_bytes") if inner else None,
        },
    )


def sd_raw_diag_gate(artifact_roots: list[Path], root: Path, commit: str | None, expected_port: str) -> GateResult:
    diag = newest_commit_json_from_roots(
        artifact_roots,
        commit,
        "storage_diag_raw_*.json",
        "sd_diag_raw_*.json",
        "rp2040_raw_diag_*.json",
        "rp2040_direct_diag_*.json",
        "rp2040_direct_diag_poll_*.json",
        "rp2040_direct_preflight_*.json",
    )
    data = read_json(diag)
    fields = raw_diag_fields(data)
    missing_prefixes = [
        prefix
        for prefix in RAW_DIAG_PROBE_PREFIXES
        if not all(f"{prefix}{suffix}" in fields for suffix in RAW_DIAG_PROBE_SUFFIXES)
    ]
    ok = bool(diag and data and raw_diag_complete(data, expected_port))
    return GateResult(
        "sd_raw_diag_complete",
        "P0",
        ok,
        "Raw RP2040 SD diagnostic matrix",
        [rel(diag, root)] if diag else [],
        "Raw SD diagnostic artifact includes normal, power, bitbang, inverted-CS, swapped-pin, command, and MISO fields."
        if ok else "No complete current-commit raw SD diagnostic artifact was found.",
        {
            "path_found": bool(diag),
            "artifact_ok": data.get("ok") if data else None,
            "port": data.get("port") if data else None,
            "response_truncated": data.get("response_truncated") if data else None,
            "field_count": len(fields),
            "missing_probe_prefixes": missing_prefixes,
        },
    )


def newest_boot_prepare_artifact(
    artifact_roots: list[Path],
    commit: str | None,
    scenario: str,
) -> Path | None:
    underscored = scenario.replace("-", "_")
    return newest_commit_json_from_roots(
        artifact_roots,
        commit,
        f"d1l-sd-boot-prepare-{scenario}-*.json",
        f"d1l-sd-boot-prepare-{underscored}-*.json",
        f"sd_boot_prepare_{scenario}_*.json",
        f"sd_boot_prepare_{underscored}_*.json",
        f"*boot*prepare*{scenario}*.json",
        f"*boot*prepare*{underscored}*.json",
    )


def sd_boot_prepare_gate(artifact_roots: list[Path], root: Path, commit: str | None, expected_port: str) -> GateResult:
    missing: list[str] = []
    failed: list[str] = []
    evidence: list[str] = []
    details: dict[str, Any] = {}
    for scenario in SD_BOOT_PREPARE_SCENARIOS:
        path = newest_boot_prepare_artifact(artifact_roots, commit, scenario)
        data = read_json(path)
        if path:
            evidence.append(rel(path, root))
        ok = bool(path and data and sd_boot_prepare_artifact_ok(data, scenario, expected_port))
        if not path:
            missing.append(scenario)
        elif not ok:
            failed.append(scenario)
        details[scenario] = {
            "path": path_for_details(path, root),
            "ok": ok,
            "artifact_ok": data.get("ok") if data else None,
            "mode": data.get("mode") if data else None,
            "port": data.get("port") if data else None,
            "classification": data.get("classification") if data else None,
            "retained_worker_quiesce_acquired": data.get("retained_worker_quiesce_acquired") if data else None,
            "storage_remount_ok": (
                data.get("storage_remount", {}).get("ok")
                if data and isinstance(data.get("storage_remount"), dict)
                else None
            ),
            "storage_remount_retained_worker_quiesce_acquired": (
                data.get("storage_remount", {}).get("retained_worker_quiesce_acquired")
                if data and isinstance(data.get("storage_remount"), dict)
                else None
            ),
            "formats_sd": data.get("formats_sd") if data else None,
            "public_rf_tx": data.get("public_rf_tx") if data else None,
        }
    ok = not missing and not failed
    details["missing_scenarios"] = missing
    details["failed_scenarios"] = failed
    return GateResult(
        "sd_boot_retry_manager",
        "P0",
        ok,
        "SD boot/retry manager scenario matrix",
        evidence,
        "All SD boot/retry manager scenarios pass on current hardware evidence."
        if ok else "SD boot/retry manager scenario evidence is missing or incomplete.",
        details,
    )


def sd_fat32_policy_gate(unformatted_path: Path | None, root: Path, expected_port: str) -> GateResult:
    data = read_json(unformatted_path)
    storage_after = data.get("storage_after") if isinstance(data.get("storage_after"), dict) else {}
    storage_setup = data.get("storage_setup") if isinstance(data.get("storage_setup"), dict) else {}
    sd = storage_after.get("sd") if isinstance(storage_after.get("sd"), dict) else {}
    ok = bool(
        unformatted_path
        and data
        and data.get("mode") == "hardware"
        and exact_port_ok(data, expected_port)
        and data.get("scenario") == "unformatted"
        and report_is_no_rf_no_format(data)
        and data.get("classification") in SD_BOOT_PREPARE_EXPECTED_CLASSIFICATIONS["unformatted"]
        and storage_setup_no_format_policy(storage_setup)
        and (sd.get("needs_fat32") is True or storage_setup.get("needs_fat32") is True)
        and data.get("storage_file_gate_ready") is not True
    )
    return GateResult(
        "sd_fat32_only_enforced",
        "P0",
        ok,
        "FAT32-only SD policy enforcement",
        [rel(unformatted_path, root)] if unformatted_path else [],
        "Unformatted/non-FAT32 evidence keeps NVS fallback active and instructs computer-side FAT32 preparation."
        if ok else "No current hardware evidence proves non-FAT32 cards are rejected without enabling SD file ops.",
        {
            "path_found": bool(unformatted_path),
            "artifact_ok": data.get("ok") if data else None,
            "mode": data.get("mode") if data else None,
            "classification": data.get("classification") if data else None,
            "sd_state": sd.get("state") if sd else None,
            "needs_fat32": sd.get("needs_fat32") if sd else storage_setup.get("needs_fat32") if storage_setup else None,
            "storage_file_gate_ready": data.get("storage_file_gate_ready") if data else None,
        },
    )


def sd_no_format_policy_gate(preflight_path: Path | None, unformatted_path: Path | None, root: Path) -> GateResult:
    preflight = read_json(preflight_path)
    classification = preflight.get("classification") if isinstance(preflight.get("classification"), dict) else {}
    _, obsolete_format_action = sanitized_sd_classification(preflight, classification)
    unformatted = read_json(unformatted_path)
    storage_setup = unformatted.get("storage_setup") if isinstance(unformatted.get("storage_setup"), dict) else {}
    preflight_ok = bool(
        preflight_path
        and preflight
        and preflight.get("formats_sd") is False
        and preflight.get("copies_uf2") is not True
        and not obsolete_format_action
    )
    unformatted_ok = bool(
        unformatted_path
        and unformatted
        and report_is_no_rf_no_format(unformatted)
        and storage_setup_no_format_policy(storage_setup)
        and unformatted.get("format_command_sent") is False
        and unformatted.get("format_confirmed") is False
        and unformatted.get("format_allowed") is False
        and not unsafe_sd_commands(unformatted)
    )
    ok = preflight_ok and unformatted_ok
    evidence = []
    if preflight_path:
        evidence.append(rel(preflight_path, root))
    if unformatted_path:
        evidence.append(rel(unformatted_path, root))
    return GateResult(
        "sd_no_format_language",
        "P0",
        ok,
        "No-device-format SD evidence policy",
        evidence,
        "Preflight and setup evidence preserve no-device-format language and never request formatting."
        if ok else "Current SD evidence is missing no-device-format policy proof or contains stale format guidance.",
        {
            "preflight_path_found": bool(preflight_path),
            "preflight_formats_sd": preflight.get("formats_sd") if preflight else None,
            "preflight_copies_uf2": preflight.get("copies_uf2") if preflight else None,
            "obsolete_format_action_blocked": obsolete_format_action,
            "unformatted_path_found": bool(unformatted_path),
            "format_command_sent": unformatted.get("format_command_sent") if unformatted else None,
            "format_confirmed": unformatted.get("format_confirmed") if unformatted else None,
            "format_allowed": unformatted.get("format_allowed") if unformatted else None,
            "storage_setup_policy": storage_setup.get("policy") if storage_setup else None,
            "unsafe_commands": unsafe_sd_commands(unformatted) if unformatted else [],
        },
    )


def sd_power_rail_gate(artifact_roots: list[Path], root: Path, commit: str | None, expected_port: str) -> GateResult:
    power = newest_commit_json_from_roots(
        artifact_roots,
        commit,
        "sd_electrical_*.json",
        "sd_power_rail_*.json",
        "sd_slot_electrical_*.json",
        "sd_power_matrix_*.json",
    )
    data = read_json(power)
    ok = bool(power and data and sd_power_rail_evidence_ok(data, expected_port))
    return GateResult(
        "sd_power_rail_measured",
        "P0",
        ok,
        "SD slot power-rail and signal measurement",
        [rel(power, root)] if power else [],
        "Measured SD socket VCC/GND/SPI evidence is present for the release hardware."
        if ok else "No current hardware SD power-rail/electrical measurement artifact was found.",
        {
            "path_found": bool(power),
            "artifact_ok": data.get("ok") if data else None,
            "mode": data.get("mode") if data else None,
            "port": data.get("port") if data else None,
            "vcc_at_socket_volts": first_numeric_field(
                data,
                "vcc_at_socket_volts",
                "sd_vcc_volts",
                "vcc_after_gpio18_high_volts",
                "sd_power_rail_volts",
            ) if data else None,
            "sd_power_rail_measured": data.get("sd_power_rail_measured") if data else None,
        },
    )


def sd_32gb_matrix_gate(artifact_roots: list[Path], root: Path, commit: str | None, expected_port: str) -> GateResult:
    matrix = newest_commit_json_from_roots(
        artifact_roots,
        commit,
        "sd_32gb_matrix_*.json",
        "sd_fat32_matrix_*.json",
        "sd_card_matrix_*.json",
        "sd_matrix_*.json",
    )
    data = read_json(matrix)
    cards = card_matrix_entries(data)
    capacities = [card_capacity_gb(card) for card in cards]
    ok = bool(matrix and data and sd_matrix_artifact_ok(data, expected_port))
    return GateResult(
        "sd_32gb_max_matrix_passed",
        "P0",
        ok,
        "FAT32 <=32GB SD card matrix",
        [rel(matrix, root)] if matrix else [],
        "At least three FAT32 cards, including a 32GB card, pass non-formatting SD acceptance."
        if ok else "No passing current-commit FAT32 <=32GB SD matrix artifact was found.",
        {
            "path_found": bool(matrix),
            "artifact_ok": data.get("ok") if data else None,
            "port": data.get("port") if data else None,
            "card_count": len(cards),
            "capacities_gb": capacities,
            "all_fat32": all(card_is_fat32(card) for card in cards) if cards else False,
            "has_32gb_card": any(capacity == 32 for capacity in capacities),
        },
    )


def raw_full_soak_evidence(data: dict) -> tuple[bool, int | None]:
    samples = data.get("samples")
    commands = data.get("commands")
    interval = first_number(data.get("sample_interval_sec"))
    if (
        not isinstance(samples, list)
        or not samples
        or not isinstance(commands, list)
        or not all(isinstance(command, str) for command in commands)
        or len(commands) != len(set(commands))
        or not REQUIRED_FULL_SOAK_COMMANDS.issubset(set(commands))
        or not set(commands).issubset(ALLOWED_FULL_SOAK_COMMANDS)
        or interval is None
        or interval <= 0
        or interval > MAX_FULL_SOAK_SAMPLE_INTERVAL_SECONDS
    ):
        return False, None

    required_samples = int(FULL_SOAK_SECONDS // interval) + 1
    if len(samples) < required_samples:
        return False, None

    elapsed_values: list[float] = []
    uptime_values: list[int] = []
    boot_nonces: list[int] = []
    retained_stack_values: list[int] = []
    for sample in samples:
        if (
            not isinstance(sample, dict)
            or "aborted_after_timeout" not in sample
            or sample.get("aborted_after_timeout") is not None
        ):
            return False, None
        elapsed = first_number(sample.get("elapsed_sec"))
        results = sample.get("results")
        if elapsed is None or not isinstance(results, list):
            return False, None
        if (
            [result.get("cmd") for result in results if isinstance(result, dict)]
            != commands
            or len(results) != len(commands)
            or any(
                not isinstance(result, dict) or result.get("ok") is not True
                for result in results
            )
        ):
            return False, None
        health_rows = [
            result
            for result in results
            if isinstance(result, dict) and result.get("cmd") == "health"
        ]
        if (
            len(health_rows) != 1
            or health_rows[0].get("board_ready") is not True
            or health_rows[0].get("ui_ready") is not True
        ):
            return False, None
        mesh_rows = [
            result
            for result in results
            if isinstance(result, dict) and result.get("cmd") == "mesh status"
        ]
        if (
            len(mesh_rows) != 1
            or mesh_rows[0].get("state") != "ready"
            or mesh_rows[0].get("identity_ready") is not True
            or mesh_rows[0].get("radio_ready") is not True
        ):
            return False, None
        crashlog_rows = [
            result
            for result in results
            if isinstance(result, dict) and result.get("cmd") == "crashlog"
        ]
        if len(crashlog_rows) != 1 or nested_flag_true(
            crashlog_rows[0], "crash_like"
        ):
            return False, None
        uptime = int_value(health_rows[0].get("uptime_ms"))
        boot_nonce = int_value(health_rows[0].get("boot_nonce"))
        retained_stack = int_value(
            health_rows[0].get("retained_task_stack_free_bytes")
        )
        if (
            uptime is None
            or uptime < 0
            or boot_nonce is None
            or boot_nonce == 0
            or retained_stack is None
            or retained_stack < 4096
        ):
            return False, None
        elapsed_values.append(elapsed)
        uptime_values.append(uptime)
        boot_nonces.append(boot_nonce)
        retained_stack_values.append(retained_stack)

    elapsed_gaps = [
        later - earlier
        for earlier, later in zip(elapsed_values, elapsed_values[1:])
    ]
    uptime_gaps_seconds = [
        (later - earlier) / 1000.0
        for earlier, later in zip(uptime_values, uptime_values[1:])
    ]
    timing_ok = (
        0 <= elapsed_values[0] <= interval
        and all(
            0 < gap <= (interval * 1.5) + 5.0
            for gap in elapsed_gaps
        )
        and elapsed_values[-1] >= FULL_SOAK_SECONDS
        and uptime_values == sorted(uptime_values)
        and len(set(boot_nonces)) == 1
        and all(
            abs(uptime_gap - elapsed_gap)
            <= max(10.0, elapsed_gap * 0.15)
            for elapsed_gap, uptime_gap in zip(
                elapsed_gaps, uptime_gaps_seconds
            )
        )
    )
    return timing_ok, min(retained_stack_values)


def full_soak_ok(data: dict) -> bool:
    summary = data.get("summary") if isinstance(data.get("summary"), dict) else {}
    duration = first_number(data.get("duration_sec"))
    retained_stack_floor = int_value(
        summary.get("retained_task_stack_free_bytes_floor")
    )
    raw_evidence_ok, raw_retained_stack_floor = raw_full_soak_evidence(data)
    return (
        data.get("ok") is True
        and data.get("mode") == "hardware"
        and duration is not None
        and duration >= FULL_SOAK_SECONDS
        and report_is_no_rf_no_format(data)
        and "active_public_text" in data
        and data.get("active_public_text") is None
        and data.get("dm_rf_tx") is False
        and data.get("active_command") is None
        and data.get("active_events") == []
        and isinstance(data.get("setup_events"), list)
        and all(
            isinstance(event, dict)
            and isinstance(event.get("result"), dict)
            and event["result"].get("ok") is True
            for event in data["setup_events"]
        )
        and not nested_command_startswith(data, "mesh send")
        and not nested_terminal_host_timeout(data)
        and not nested_unexpected_console_restart(data)
        and "aborted_after_timeout" in data
        and data.get("aborted_after_timeout") is None
        and raw_evidence_ok
        and summary.get("ok") is True
        and not summary.get("threshold_failures")
        and summary.get("command_timeout_seen") is False
        and summary.get("unexpected_console_restart_seen") is False
        and retained_stack_floor is not None
        and retained_stack_floor >= 4096
        and retained_stack_floor == raw_retained_stack_floor
        and summary.get("crashlog_crash_like_count") == 0
        and summary.get("board_ready_all") is True
        and summary.get("ui_ready_all") is True
        and summary.get("mesh_ready_all") is True
        and summary.get("uptime_monotonic") is True
    )


def full_soak_gate(soak_root: Path, root: Path, commit: str | None) -> GateResult:
    candidates = [
        path for path in sorted(soak_root.glob("*.json"), key=lambda path: path.stat().st_mtime, reverse=True)
        if artifact_commit_matches(path, read_json(path), commit)
    ]
    for candidate in candidates:
        data = read_json(candidate)
        if full_soak_ok(data):
            return GateResult(
                "full_duration_idle_soak",
                "P0",
                True,
                "12-hour idle/listening soak",
                [rel(candidate, root)],
                "12-hour idle/listening soak artifact passes.",
                {"duration_sec": data.get("duration_sec")},
            )
    newest = candidates[0] if candidates else None
    newest_data = read_json(newest)
    return GateResult(
        "full_duration_idle_soak",
        "P0",
        False,
        "12-hour idle/listening soak",
        [rel(newest, root)] if newest else [],
        "No passing 12-hour idle/listening hardware soak artifact was found.",
        {"latest_duration_sec": newest_data.get("duration_sec") if newest_data else None},
    )


def manual_evidence_gate(hardware_dir: Path, root: Path, commit: str | None) -> GateResult:
    review = newest_commit_json(hardware_dir, commit, "manual_touch_review*.json", "*manual*ui*review*.json")
    photos = [path for pattern in ("photos/*", "screen_photos/*", "*screen_photo*") for path in hardware_dir.glob(pattern) if path.is_file()]
    data = read_json(review)
    ok = bool(review and data.get("ok") is True and photos)
    evidence = []
    if review:
        evidence.append(rel(review, root))
    evidence.extend(rel(path, root) for path in sorted(photos)[:10])
    return GateResult(
        "manual_physical_ui_review",
        "P0",
        ok,
        "Manual physical UI/touch review and screen photos",
        evidence,
        "Manual physical UI/touch review and screen photos are present."
        if ok else "Manual physical UI/touch review and physical screen photos are still missing.",
        {"photo_count": len(photos), "review_found": bool(review)},
    )


def full_rf_gate(
    hardware_dir: Path,
    root: Path,
    commit: str | None,
    expected_port: str,
    expected_peer_port: str | None,
) -> GateResult:
    candidates = [
        newest_commit_json(hardware_dir, commit, "rf_full_acceptance*.json", "*ack_path*.json", "*direct_route*.json", "*inbound_dm*.json")
    ]
    evidence_paths = [path for path in candidates if path]
    passing = False
    for path in evidence_paths:
        data = read_json(path)
        passing = full_rf_acceptance_ok(
            data,
            expected_port,
            expected_peer_port,
            evidence_root=root,
        )
        if passing:
            break
    return GateResult(
        "full_rf_dm_acceptance",
        "P0",
        passing,
        "Inbound DM, ACK/PATH, and direct-route RF proof",
        [rel(path, root) for path in evidence_paths],
        "Full RF/DM acceptance evidence is present."
        if passing else "Only partial RF/DM evidence is present; inbound DM, ACK/PATH, and direct-route proof remain open.",
        {
            "candidate_count": len(evidence_paths),
            "strict_schema": 2,
            "expected_port": normalize_port(expected_port),
            "expected_peer_port": normalize_port(expected_peer_port),
        },
    )


def component_lock_idf_version(text: str) -> str | None:
    in_dependencies = False
    in_idf = False
    for raw_line in text.splitlines():
        content = raw_line.lstrip(" ")
        if not content or content.startswith("#"):
            continue
        indent = len(raw_line) - len(content)
        stripped = content.strip()
        if indent == 0:
            in_dependencies = stripped == "dependencies:"
            in_idf = False
            continue
        if not in_dependencies:
            continue
        if indent == 2:
            in_idf = stripped == "idf:"
            continue
        if in_idf and indent == 4 and stripped.startswith("version:"):
            version = stripped.partition(":")[2].strip().strip("\"'")
            return version or None
    return None


def supported_sdk_gate(root: Path) -> GateResult:
    workflow = root / ".github" / "workflows" / "d1l-ci.yml"
    build_inputs_path = root / SUPPORTED_ESP_IDF_BUILD_INPUTS
    component_lock = root / "dependencies.lock"
    workflow_text = workflow.read_text(encoding="utf-8") if workflow.is_file() else ""
    lock_text = component_lock.read_text(encoding="utf-8") if component_lock.is_file() else ""
    try:
        build_inputs = read_json(build_inputs_path)
    except (OSError, json.JSONDecodeError, UnicodeError):
        build_inputs = {}
    esp_idf_inputs = build_inputs.get("esp_idf")
    if not isinstance(esp_idf_inputs, dict):
        esp_idf_inputs = {}
    container_inputs = esp_idf_inputs.get("container")
    if not isinstance(container_inputs, dict):
        container_inputs = {}
    locked_idf_version = component_lock_idf_version(lock_text)
    job_match = re.search(
        r"(?ms)^  firmware-build:\s*\n(?P<body>.*?)(?=^  [A-Za-z0-9_-]+:\s*\n|\Z)",
        workflow_text,
    )
    job_text = job_match.group("body") if job_match else ""
    configured_images = re.findall(r"(?m)^\s{4}container:\s*([^\s#]+)\s*(?:#.*)?$", job_text)
    moving_images = [
        image
        for image in configured_images
        if image.endswith(":latest") or ":release-v" in image
    ]
    exact_release_pin = configured_images == [SUPPORTED_ESP_IDF_IMAGE]
    exact_build_inputs = (
        build_inputs.get("schema") == 1
        and build_inputs.get("kind") == "d1l_build_inputs"
        and esp_idf_inputs.get("version") == SUPPORTED_ESP_IDF_VERSION
        and container_inputs.get("reference") == SUPPORTED_ESP_IDF_IMAGE
        and container_inputs.get("index_digest") == SUPPORTED_ESP_IDF_IMAGE_DIGEST
    )
    exact_component_lock = locked_idf_version == SUPPORTED_ESP_IDF_LOCK_VERSION
    ok = (
        workflow.is_file()
        and job_match is not None
        and exact_release_pin
        and not moving_images
        and exact_build_inputs
        and exact_component_lock
    )
    evidence = [
        rel(path, root)
        for path in (workflow, build_inputs_path, component_lock)
        if path.is_file()
    ]
    return GateResult(
        "supported_sdk_baseline",
        "P0",
        ok,
        "Supported ESP-IDF release baseline",
        evidence,
        f"D1L firmware CI configuration and component lock target ESP-IDF {SUPPORTED_ESP_IDF_VERSION}."
        if ok else (
            f"D1L firmware CI and build-input metadata must pin exactly {SUPPORTED_ESP_IDF_IMAGE}, "
            f"and its solver-generated component lock must record IDF {SUPPORTED_ESP_IDF_LOCK_VERSION}; "
            "mutable, missing, malformed, EOL, or unapproved SDK inputs are release-blocking."
        ),
        {
            "issue": ESP_IDF_MIGRATION_ISSUE,
            "expected_image": SUPPORTED_ESP_IDF_IMAGE,
            "expected_image_tag": SUPPORTED_ESP_IDF_IMAGE_TAG,
            "expected_image_digest": SUPPORTED_ESP_IDF_IMAGE_DIGEST,
            "expected_version": SUPPORTED_ESP_IDF_VERSION,
            "workflow_found": workflow.is_file(),
            "firmware_job_found": job_match is not None,
            "configured_images": configured_images,
            "moving_images": moving_images,
            "exact_release_pin": exact_release_pin,
            "build_inputs_found": build_inputs_path.is_file(),
            "build_inputs_path": SUPPORTED_ESP_IDF_BUILD_INPUTS,
            "recorded_image": container_inputs.get("reference"),
            "recorded_image_digest": container_inputs.get("index_digest"),
            "exact_build_inputs": exact_build_inputs,
            "component_lock_found": component_lock.is_file(),
            "expected_lock_version": SUPPORTED_ESP_IDF_LOCK_VERSION,
            "locked_idf_version": locked_idf_version,
            "exact_component_lock": exact_component_lock,
        },
    )


def immutable_release_source_inputs_gate(
    github_run_dir: Path | None,
    root: Path,
    commit: str | None,
    expected_run_id: str | None,
) -> GateResult:
    """Bind the selected run's host, IDF, and Arduino receipts to one lock."""

    def json_object(path: Path | None) -> dict[str, Any]:
        try:
            value = read_json(path)
        except (OSError, UnicodeError, json.JSONDecodeError):
            return {}
        return value if isinstance(value, dict) else {}

    def digest(path: Path | None) -> str | None:
        try:
            return sha256_file(path) if path and path.is_file() else None
        except OSError:
            return None

    def text(path: Path | None) -> str | None:
        try:
            return path.read_text(encoding="utf-8").strip() if path and path.is_file() else None
        except (OSError, UnicodeError):
            return None

    def normalized_packages(value: object) -> dict[str, str] | None:
        if not isinstance(value, list):
            return None
        packages: dict[str, str] = {}
        for item in value:
            if not isinstance(item, dict):
                return None
            name = item.get("name")
            version = item.get("version")
            if not isinstance(name, str) or not name.strip() or not isinstance(version, str):
                return None
            normalized = re.sub(r"[-_.]+", "-", name.strip()).lower()
            if normalized in packages or not version.strip():
                return None
            packages[normalized] = version.strip()
        return packages

    metadata_path = root / SUPPORTED_ESP_IDF_BUILD_INPUTS
    requirements_path = root / "requirements" / "ci-host-windows.txt"
    dependency_lock = root / "dependencies.lock"
    metadata = json_object(metadata_path)
    metadata_sha = digest(metadata_path)

    host_dir = github_run_dir / "d1l-host-artifacts" / "build-inputs" if github_run_dir else None
    host_metadata = host_dir / "d1l-build-inputs.json" if host_dir else None
    host_requirements = host_dir / "ci-host-windows.txt" if host_dir else None
    host_installed = host_dir / "ci-host-windows-installed.json" if host_dir else None
    host_checksums = host_dir / "SHA256SUMS.txt" if host_dir else None

    idf_dir = github_run_dir / "d1l-idf55-migration-state" if github_run_dir else None
    idf_metadata = idf_dir / "build-inputs.json" if idf_dir else None
    idf_container = idf_dir / "container-image.txt" if idf_dir else None
    idf_version = idf_dir / "idf-version.txt" if idf_dir else None
    idf_dependency_lock = idf_dir / "dependencies.lock" if idf_dir else None
    idf_dependency_patch = idf_dir / "dependencies.lock.patch" if idf_dir else None

    arduino_dir = github_run_dir / "rp2040-sd-bridge-firmware" if github_run_dir else None
    arduino_receipt_path = arduino_dir / "build-inputs.json" if arduino_dir else None
    arduino_checksums = arduino_dir / "SHA256SUMS.txt" if arduino_dir else None
    arduino_receipt = json_object(arduino_receipt_path)
    try:
        validate_build_receipt(
            arduino_receipt,
            metadata,
            commit or "",
            metadata_sha or "",
            root,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError):
        arduino_receipt_semantics_valid = False
    else:
        arduino_receipt_semantics_valid = True

    esp_idf = metadata.get("esp_idf") if isinstance(metadata.get("esp_idf"), dict) else {}
    container = esp_idf.get("container") if isinstance(esp_idf.get("container"), dict) else {}
    host_python = (
        metadata.get("host_python") if isinstance(metadata.get("host_python"), dict) else {}
    )
    requirements = (
        host_python.get("requirements")
        if isinstance(host_python.get("requirements"), dict)
        else {}
    )
    expected_packages_raw = requirements.get("packages")
    expected_packages = (
        {
            re.sub(r"[-_.]+", "-", str(name)).lower(): str(version)
            for name, version in expected_packages_raw.items()
        }
        if isinstance(expected_packages_raw, dict)
        and expected_packages_raw
        and all(
            isinstance(name, str)
            and name.strip()
            and isinstance(version, str)
            and version.strip()
            for name, version in expected_packages_raw.items()
        )
        else None
    )
    expected_package_metadata_valid = (
        expected_packages is not None
        and isinstance(expected_packages_raw, dict)
        and len(expected_packages) == len(expected_packages_raw)
    )
    try:
        installed_value = (
            json.loads(host_installed.read_text(encoding="utf-8"))
            if host_installed and host_installed.is_file()
            else None
        )
    except (OSError, UnicodeError, json.JSONDecodeError):
        installed_value = None
    installed_packages = normalized_packages(installed_value)

    arduino = metadata.get("arduino") if isinstance(metadata.get("arduino"), dict) else {}
    arduino_cli = arduino.get("cli") if isinstance(arduino.get("cli"), dict) else {}
    rp2040 = arduino.get("rp2040") if isinstance(arduino.get("rp2040"), dict) else {}
    platform_archive = (
        rp2040.get("platform_archive")
        if isinstance(rp2040.get("platform_archive"), dict)
        else {}
    )
    tools = rp2040.get("tools") if isinstance(rp2040.get("tools"), list) else []
    expected_archive_rows = [platform_archive, *tools] if platform_archive else []
    expected_archives: dict[str, tuple[str, int]] = {}
    archive_metadata_valid = bool(expected_archive_rows)
    for item in expected_archive_rows:
        if not isinstance(item, dict):
            archive_metadata_valid = False
            continue
        filename = item.get("filename")
        archive_sha = item.get("sha256")
        size = item.get("size")
        if (
            not isinstance(filename, str)
            or Path(filename).name != filename
            or filename in expected_archives
            or not isinstance(archive_sha, str)
            or re.fullmatch(r"[0-9a-f]{64}", archive_sha) is None
            or isinstance(size, bool)
            or not isinstance(size, int)
            or size <= 0
        ):
            archive_metadata_valid = False
            continue
        expected_archives[filename] = (archive_sha, size)

    receipt_archives: dict[str, tuple[str, int]] = {}
    receipt_archive_paths_valid = True
    raw_receipt_archives = arduino_receipt.get("archives")
    if isinstance(raw_receipt_archives, list):
        for item in raw_receipt_archives:
            if not isinstance(item, dict):
                receipt_archive_paths_valid = False
                continue
            filename = item.get("filename")
            relative_path = item.get("relative_path")
            archive_sha = item.get("sha256")
            size = item.get("size")
            relative = PurePosixPath(relative_path) if isinstance(relative_path, str) else None
            if (
                not isinstance(filename, str)
                or filename in receipt_archives
                or relative is None
                or relative.is_absolute()
                or any(part in {"", ".", ".."} for part in relative.parts)
                or relative.name != filename
                or not isinstance(archive_sha, str)
                or re.fullmatch(r"[0-9a-f]{64}", archive_sha) is None
                or isinstance(size, bool)
                or not isinstance(size, int)
                or size <= 0
            ):
                receipt_archive_paths_valid = False
                continue
            receipt_archives[filename] = (archive_sha, size)
    else:
        receipt_archive_paths_valid = False

    receipt_metadata = (
        arduino_receipt.get("metadata")
        if isinstance(arduino_receipt.get("metadata"), dict)
        else {}
    )
    expected_submodules = metadata.get("submodules")
    submodule_metadata_valid = (
        isinstance(expected_submodules, dict)
        and bool(expected_submodules)
        and all(
            isinstance(path, str)
            and path.strip()
            and isinstance(value, str)
            and re.fullmatch(r"[0-9a-f]{40}", value) is not None
            for path, value in expected_submodules.items()
        )
    )

    host_entries = checksum_manifest_entries(host_checksums) if host_checksums else {}
    arduino_entries = checksum_manifest_entries(arduino_checksums) if arduino_checksums else {}
    expected_host_entries = {
        "d1l-build-inputs.json",
        "ci-host-windows.txt",
        "ci-host-windows-installed.json",
    }
    checks = {
        "github_run_dir_present": github_run_dir is not None and github_run_dir.is_dir(),
        "expected_commit_exact": isinstance(commit, str)
        and re.fullmatch(r"[0-9a-f]{40}", commit) is not None,
        "expected_run_id_numeric": isinstance(expected_run_id, str)
        and re.fullmatch(r"[1-9][0-9]*", expected_run_id) is not None,
        "metadata_schema": metadata.get("schema") == BUILD_INPUTS_SCHEMA
        and metadata.get("kind") == BUILD_INPUTS_KIND,
        "metadata_hash_readable": metadata_sha is not None,
        "esp_idf_lock_exact": esp_idf.get("version") == SUPPORTED_ESP_IDF_VERSION
        and container.get("reference") == SUPPORTED_ESP_IDF_IMAGE
        and container.get("index_digest") == SUPPORTED_ESP_IDF_IMAGE_DIGEST,
        "requirements_lock_exact": requirements.get("path")
        == "requirements/ci-host-windows.txt"
        and digest(requirements_path) == requirements.get("sha256"),
        "host_metadata_copy_exact": metadata_sha is not None
        and digest(host_metadata) == metadata_sha,
        "host_requirements_copy_exact": digest(requirements_path) is not None
        and digest(host_requirements) == digest(requirements_path),
        "host_installed_packages_exact": expected_package_metadata_valid
        and installed_packages == expected_packages,
        "host_checksum_manifest_complete": bool(host_checksums)
        and verify_sha256_manifest(host_checksums)
        and set(host_entries) == expected_host_entries,
        "idf_metadata_copy_exact": metadata_sha is not None
        and digest(idf_metadata) == metadata_sha,
        "idf_container_exact": text(idf_container) == SUPPORTED_ESP_IDF_IMAGE,
        "idf_version_exact": text(idf_version) == f"ESP-IDF {SUPPORTED_ESP_IDF_VERSION}",
        "idf_dependency_lock_exact": digest(dependency_lock) is not None
        and digest(idf_dependency_lock) == digest(dependency_lock),
        "idf_dependency_patch_empty": digest(idf_dependency_patch)
        == hashlib.sha256(b"").hexdigest(),
        "arduino_receipt_schema": arduino_receipt.get("schema") == BUILD_INPUTS_SCHEMA
        and arduino_receipt.get("kind") == ARDUINO_BUILD_INPUTS_KIND
        and arduino_receipt.get("ok") is True,
        "arduino_receipt_semantics_complete": arduino_receipt_semantics_valid,
        "arduino_receipt_source_commit": isinstance(commit, str)
        and arduino_receipt.get("source_commit") == commit,
        "arduino_receipt_metadata_exact": receipt_metadata.get("path")
        == SUPPORTED_ESP_IDF_BUILD_INPUTS
        and metadata_sha is not None
        and receipt_metadata.get("sha256") == metadata_sha,
        "arduino_cli_version_exact": isinstance(arduino_cli.get("version"), str)
        and re.fullmatch(r"\d+\.\d+\.\d+", arduino_cli["version"]) is not None
        and arduino_receipt.get("arduino_cli_version") == arduino_cli["version"],
        "rp2040_core_version_exact": isinstance(rp2040.get("version"), str)
        and re.fullmatch(r"\d+\.\d+\.\d+", rp2040["version"]) is not None
        and arduino_receipt.get("rp2040_core_version") == rp2040["version"],
        "submodule_gitlinks_exact": submodule_metadata_valid
        and arduino_receipt.get("submodules") == expected_submodules,
        "arduino_archives_verified": arduino_receipt.get("archives_verified") is True,
        "arduino_archive_inventory_exact": archive_metadata_valid
        and receipt_archive_paths_valid
        and receipt_archives == expected_archives,
        "arduino_checksum_manifest_complete": bool(arduino_checksums)
        and verify_sha256_manifest(arduino_checksums)
        and digest(arduino_receipt_path) is not None
        and arduino_entries.get("build-inputs.json") == digest(arduino_receipt_path),
    }
    failures = [name for name, passed in checks.items() if not passed]
    evidence_paths = [
        metadata_path,
        requirements_path,
        dependency_lock,
        host_checksums,
        host_metadata,
        host_requirements,
        host_installed,
        idf_metadata,
        idf_container,
        idf_version,
        idf_dependency_lock,
        idf_dependency_patch,
        arduino_receipt_path,
        arduino_checksums,
    ]
    evidence = [rel(path, root) for path in evidence_paths if path and path.is_file()]
    ok = not failures
    return GateResult(
        "immutable_release_source_inputs",
        "P0",
        ok,
        "Immutable release source and toolchain inputs",
        evidence,
        (
            "The selected Actions run binds host Python, ESP-IDF, Arduino/RP2040 archives, and submodules to the committed build-input lock."
            if ok
            else "The selected Actions run is missing or contradicts immutable host, ESP-IDF, Arduino/RP2040, or submodule input receipts."
        ),
        {
            "failures": failures,
            "checks": checks,
            "expected_commit": commit,
            "expected_run_id": expected_run_id,
            "metadata_sha256": metadata_sha,
            "host_package_count": len(installed_packages or {}),
            "arduino_archive_count": len(receipt_archives),
            "submodule_count": len(expected_submodules or {})
            if isinstance(expected_submodules, dict)
            else 0,
        },
    )


def docs_freshness_gate(root: Path, commit: str | None, run_id: str | None) -> GateResult:
    docs = [
        root / "README.md",
        root / "docs" / "ROADMAP.md",
        root / "docs" / "RELEASE_CHECKLIST.md",
        root / "docs" / "KNOWN_LIMITATIONS.md",
    ]
    texts = {path: path.read_text(encoding="utf-8") for path in docs if path.is_file()}
    del commit, run_id
    combined = "\n".join(texts.values())
    needle_values = [
        "release_gate_audit_d1l.py",
        "ready_for_public_release=false",
        "No release tag should be cut until",
    ]
    missing = []
    for needle in needle_values:
        if needle not in combined:
            missing.append(needle)
    ok = not missing and len(texts) == len(docs)
    return GateResult(
        "docs_current_evidence",
        "P1",
        ok,
        "Release docs include fail-closed audit gate",
        [rel(path, root) for path in texts],
        "Release docs include the fail-closed audit gate and no-release-tag policy."
        if ok else "Release docs do not yet include the fail-closed audit gate and no-release-tag policy.",
        {"missing_needles": missing},
    )


def build_audit(args: argparse.Namespace) -> dict:
    root = Path(args.root).resolve()
    if args.github_run_id and not args.github_run_dir:
        args.github_run_dir = str(root / "artifacts" / "github" / args.github_run_id)
    github_run_dir = Path(args.github_run_dir).resolve() if args.github_run_dir else None
    hardware_dir = Path(args.hardware_dir).resolve()
    rp2040_hardware_dir = Path(args.rp2040_hardware_dir).resolve()
    soak_dir = Path(args.soak_dir).resolve()
    wp01_dir_arg = Path(args.wp01_dir)
    wp01_dir = (
        wp01_dir_arg.resolve()
        if wp01_dir_arg.is_absolute()
        else (root / wp01_dir_arg).resolve()
    )
    gates: list[GateResult] = []
    smoke_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "smoke")
    ui_corruption_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "ui-corruption-probe")
    ui_capture_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "ui-capture")
    scroll_probe_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "scroll-probe")
    official_smoke_roots = unique_dirs(
        sd_artifact_roots(root, github_run_dir, rp2040_hardware_dir, "rp2040-official-sd-smoke")
        + [hardware_dir]
    )
    preflight_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "rp2040-preflight")
    boot_prepare_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "sd-boot-prepare")
    file_canary_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "sd-canary")
    retained_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "sd-retained-history")
    reboot_remount_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "sd-reboot-remount")
    map_tile_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "sd-map-tile-canary")
    export_canary_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "sd-export-canary")
    diagnostic_export_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "sd-diagnostic-export")
    data_export_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "sd-data-export")
    raw_diag_roots = unique_dirs(
        sd_artifact_roots(root, github_run_dir, rp2040_hardware_dir, "rp2040-preflight")
        + sd_artifact_roots(root, github_run_dir, hardware_dir, "rp2040-preflight")
    )
    sd_matrix_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "sd-matrix")
    sd_power_roots = sd_artifact_roots(root, github_run_dir, hardware_dir, "sd-electrical", "sd-matrix")
    preflight_path = newest_commit_json_from_roots(
        preflight_roots,
        args.commit,
        "rp2040_preflight_*.json",
        "rp2040_sd_preflight_*.json",
        "d1l-rp2040-sd-bridge-preflight-*.json",
    )
    unformatted_boot_path = newest_boot_prepare_artifact(boot_prepare_roots, args.commit, "unformatted")

    gates.append(supported_sdk_gate(root))
    gates.append(
        immutable_release_source_inputs_gate(
            github_run_dir,
            root,
            args.commit,
            args.github_run_id,
        )
    )
    gates.append(
        host_checks_success_gate(
            github_run_dir,
            root,
            args.commit,
            args.github_run_id,
        )
    )
    gates.append(checksum_gate(github_run_dir, root))
    gates.append(
        meshcore_conformance_evidence_gate(
            github_run_dir,
            root,
            args.commit,
            expected_run_id=args.github_run_id,
        )
    )
    gates.append(
        meshcore_signed_advert_evidence_gate(
            github_run_dir,
            root,
            args.commit,
            expected_run_id=args.github_run_id,
        )
    )
    gates.append(meshcore_full_conformance_gate())
    gates.append(notices_gate(github_run_dir, root))
    gates.append(
        esp32_flash_receipt_gate(
            hardware_dir,
            github_run_dir,
            root,
            args.commit,
            args.github_run_id,
            args.d1l_port,
        )
    )
    gates.append(
        simple_json_ok_gate(
            "com12_smoke",
            "D1L hardware smoke",
            newest_commit_json_from_roots(smoke_roots, args.commit, "smoke_*.json", "d1l-smoke-*.json"),
            root,
            lambda data: smoke_device_identity_ok(data, args.d1l_port, args.commit),
            "Current-commit D1L smoke artifact passes.",
            "No passing current-commit D1L smoke artifact was found.",
        )
    )
    gates.append(
        simple_json_ok_gate(
            "ui_corruption_probe",
            "Targeted D1L UI corruption probe",
            newest_commit_json_from_roots(
                ui_corruption_roots,
                args.commit,
                "ui_corruption_probe_*.json",
                "d1l-ui-corruption-probe-*.json",
            ),
            root,
            lambda data: ui_corruption_probe_ok(data, args.d1l_port),
            "Targeted D1L UI corruption probe artifact passes.",
            "No passing targeted D1L UI corruption probe artifact was found.",
        )
    )
    gates.append(
        simple_json_ok_gate(
            "ui_pixel_capture",
            "D1L hardware pixel capture",
            newest_commit_json_from_roots(
                ui_capture_roots,
                args.commit,
                "ui_pixel_capture_*.json",
                "d1l-ui-capture-*.json",
            ),
            root,
            lambda data: ui_pixel_capture_ok(data, args.d1l_port),
            "Current-commit D1L hardware pixel capture reconstructs a 480x480 RGB565 frame and passes the simulator/reference diff.",
            "No passing current-commit D1L hardware pixel capture artifact was found.",
        )
    )
    gates.append(
        simple_json_ok_gate(
            "ui_compose_keyboard_capture",
            "D1L compose keyboard hardware capture",
            newest_commit_json_from_roots(
                ui_capture_roots,
                args.commit,
                "ui_compose_keyboard_capture*.json",
                "d1l-compose-keyboard-capture-*.json",
            ),
            root,
            lambda data: compose_keyboard_capture_ok(data, args.d1l_port),
            "Current-commit D1L compose/input keyboard capture proves all release-blocking keyboard callers fit the 480x480 hardware frame.",
            "No passing current-commit D1L compose/input keyboard hardware capture artifact was found.",
        )
    )
    gates.append(
        simple_json_ok_gate(
            "ui_scroll_probe",
            "D1L scroll probe",
            newest_commit_json_from_roots(
                scroll_probe_roots,
                args.commit,
                "scroll_probe_*.json",
                "d1l-scroll-probe-*.json",
            ),
            root,
            lambda data: scroll_probe_ok(data, args.d1l_port),
            "D1L scroll probe artifact passes.",
            "No passing D1L scroll probe artifact was found.",
        )
    )
    gates.append(
        outbound_dm_gate(
            hardware_dir,
            root,
            args.commit,
            args.d1l_port,
            args.mesh_peer_port,
        )
    )
    gates.append(route_probe_gate(hardware_dir, root, args.commit, args.d1l_port))
    gates.append(official_seeed_sd_smoke_gate(official_smoke_roots, root, args.commit, args.rp2040_port))
    gates.append(sd_gate(preflight_path, root))
    gates.append(sd_raw_diag_gate(raw_diag_roots, root, args.commit, args.d1l_port))
    gates.append(sd_boot_prepare_gate(boot_prepare_roots, root, args.commit, args.d1l_port))
    gates.append(sd_filecanary_gate(file_canary_roots, root, args.commit, args.d1l_port))
    gates.append(sd_retained_canary_gate(retained_roots, root, args.commit, args.d1l_port))
    gates.append(sd_reboot_remount_gate(reboot_remount_roots, root, args.commit, args.d1l_port))
    gates.append(
        wp01_evidence_gate(
            wp01_dir,
            root,
            github_run_dir,
            args.commit,
            args.github_run_id,
            args.d1l_port,
            args.rp2040_port,
        )
    )
    gates.append(sd_map_tile_canary_gate(map_tile_roots, root, args.commit, args.d1l_port))
    gates.append(sd_export_canary_gate(export_canary_roots, root, args.commit, args.d1l_port))
    gates.append(sd_diagnostic_export_gate(diagnostic_export_roots, root, args.commit, args.d1l_port))
    gates.append(sd_data_export_gate(data_export_roots, root, args.commit, args.d1l_port))
    gates.append(sd_fat32_policy_gate(unformatted_boot_path, root, args.d1l_port))
    gates.append(sd_no_format_policy_gate(preflight_path, unformatted_boot_path, root))
    gates.append(sd_power_rail_gate(sd_power_roots, root, args.commit, args.d1l_port))
    gates.append(sd_32gb_matrix_gate(sd_matrix_roots, root, args.commit, args.d1l_port))
    gates.append(full_soak_gate(soak_dir, root, args.commit))
    gates.append(manual_evidence_gate(hardware_dir, root, args.commit))
    gates.append(
        full_rf_gate(
            hardware_dir,
            root,
            args.commit,
            args.d1l_port,
            args.mesh_peer_port,
        )
    )
    gates.append(docs_freshness_gate(root, args.commit, args.github_run_id))

    p0_gates = [gate for gate in gates if gate.severity == "P0"]
    p1_gates = [gate for gate in gates if gate.severity == "P1"]
    p0_failed = [gate for gate in p0_gates if not gate.ok]
    failed = [gate for gate in gates if not gate.ok]
    return {
        "schema": RELEASE_GATE_AUDIT_SCHEMA,
        "mode": "release-gate-audit",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "root": str(root),
        "github_run_id": args.github_run_id,
        "github_run_dir": str(github_run_dir) if github_run_dir else None,
        "commit": args.commit,
        "hardware_dir": str(hardware_dir),
        "wp01_dir": str(wp01_dir),
        "rp2040_port": args.rp2040_port,
        "rp2040_hardware_dir": str(rp2040_hardware_dir),
        "ready_for_public_release": not p0_failed,
        "gate_count": len(gates),
        "p0_gate_count": len(p0_gates),
        "p1_gate_count": len(p1_gates),
        "passed_count": len(gates) - len(failed),
        "p0_failed_count": len(p0_failed),
        "failed_count": len(failed),
        "gates": [gate.to_dict() for gate in gates],
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--github-run-id")
    parser.add_argument("--github-run-dir")
    parser.add_argument("--commit")
    parser.add_argument("--d1l-port", default=default_d1l_port())
    parser.add_argument("--rp2040-port", default=default_rp2040_port())
    parser.add_argument(
        "--mesh-peer-port",
        "--meshbot-port",
        dest="mesh_peer_port",
        default=os.environ.get("MESH_PEER_PORT"),
        help="Optional explicit, allowed local serial port for controlled-peer status evidence.",
    )
    parser.add_argument("--hardware-dir", default=default_hardware_dir())
    parser.add_argument("--rp2040-hardware-dir", default=default_rp2040_hardware_dir())
    parser.add_argument("--wp01-dir", default=default_wp01_dir())
    parser.add_argument("--soak-dir", default="artifacts/soak")
    parser.add_argument("--out")
    parser.add_argument("--fail-on-open-p0", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    if args.github_run_id and not args.github_run_dir:
        args.github_run_dir = str(Path(args.root) / "artifacts" / "github" / args.github_run_id)
    report = build_audit(args)
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text + "\n", encoding="utf-8")
    print(text)
    if args.fail_on_open_p0 and not report["ready_for_public_release"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
