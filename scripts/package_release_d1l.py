#!/usr/bin/env python3
"""Package MeshCore DeskOS D1L firmware artifacts for release handoff."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

try:
    from verify_checksums import is_link_or_reparse, verify_checksum_tree
except ModuleNotFoundError:
    from scripts.verify_checksums import is_link_or_reparse, verify_checksum_tree

try:
    from d1l_serial_target import (
        EXPECTED_PID,
        EXPECTED_VID,
        POSIX_D1L_TARGET,
    )
except ModuleNotFoundError:
    from scripts.d1l_serial_target import (
        EXPECTED_PID,
        EXPECTED_VID,
        POSIX_D1L_TARGET,
    )

if __package__:
    from .meshcore_conformance_d1l import (
        CANONICAL_EVIDENCE_PROFILE,
        canonicalize_release_report,
        validate_completed_report,
    )
    from .meshcore_signed_advert_runtime_d1l import (
        SIGNED_ADVERT_ARTIFACT_TYPE,
        SIGNED_ADVERT_EVIDENCE_PROFILE,
        canonicalize_release_report as canonicalize_signed_advert_report,
        validate_completed_report as validate_signed_advert_report,
    )
    from .provenance_d1l import write_package_provenance
    from .sbom_d1l import (
        discover_source_identity,
        exact_sha,
        write_package_sbom,
    )
else:
    from meshcore_conformance_d1l import (  # type: ignore[no-redef]
        CANONICAL_EVIDENCE_PROFILE,
        canonicalize_release_report,
        validate_completed_report,
    )
    from meshcore_signed_advert_runtime_d1l import (  # type: ignore[no-redef]
        SIGNED_ADVERT_ARTIFACT_TYPE,
        SIGNED_ADVERT_EVIDENCE_PROFILE,
        canonicalize_release_report as canonicalize_signed_advert_report,
        validate_completed_report as validate_signed_advert_report,
    )
    from provenance_d1l import write_package_provenance  # type: ignore[no-redef]
    from sbom_d1l import (  # type: ignore[no-redef]
        discover_source_identity,
        exact_sha,
        write_package_sbom,
    )


PROJECT = "MeshCore DeskOS D1L"
DEFAULT_FLASH_SIZE = 8 * 1024 * 1024
FLASH_BAUD = 460800
PACKAGE_METADATA_SCHEMA = 1
UPDATE_MANIFEST_HEADER = "D1L-UPDATE-MANIFEST-V1"
UPDATE_PRODUCT = "MeshCore DeskOS D1L"
UPDATE_TARGET = "seeed_indicator_d1l"
UPDATE_SIGNER_KEY_ID = "d1l-prod-8241789a002d0b50"
UPDATE_SIGNING_PUBLIC_KEY_HEX = (
    "e048dd4ebb613fb55378714ae527d7de"
    "9374270c7a18b29565c2715b17e5b26c"
)
BUILD_INPUTS_SOURCE = Path(".github/d1l-build-inputs.json")
HOST_REQUIREMENTS_SOURCE = Path("requirements/ci-host-windows.txt")
COMPLETION_LEDGER_SOURCE = Path(
    "docs/archive/pre-rc1-authority-reset/COMPLETION_LEDGER.yaml"
)
PACKAGE_METADATA_CONTRACTS = {
    "build_inputs": {
        "prefix": "build_inputs",
        "artifact_type": "d1l_build_inputs_package_metadata",
    },
    "capability_manifest": {
        "prefix": "capability_manifest",
        "artifact_type": "d1l_capability_manifest_package_metadata",
    },
    "release_evidence_index": {
        "prefix": "release_evidence_index",
        "artifact_type": "d1l_release_evidence_index_package_metadata",
    },
}
EXPECTED_BSP_PATCHES = (
    Path("patches/sensecap_indicator_touch_fix.patch"),
    Path("patches/sensecap_indicator_idf55_compat.patch"),
    Path("patches/sensecap_indicator_tx_origin.patch"),
)
EXPECTED_BSP_SUBMODULE = Path("third_party/sensecap_indicator_esp32")
RELEASE_DOC_SPECS = [
    ("docs/USER_GUIDE_D1L.md", "USER_GUIDE_D1L.md"),
    (
        "docs/DESKOS_MESHCORE_FEATURE_PARITY.md",
        "DESKOS_MESHCORE_FEATURE_PARITY.md",
    ),
    ("docs/KNOWN_LIMITATIONS.md", "KNOWN_LIMITATIONS.md"),
    ("docs/D1L_SD_CARD_GUIDED_INSTALL.md", "D1L_SD_CARD_GUIDED_INSTALL.md"),
    ("docs/ADMIN_REMOTE_CLI_ALLOWLIST.md", "ADMIN_REMOTE_CLI_ALLOWLIST.md"),
    ("docs/ATTRIBUTIONS.md", "ATTRIBUTIONS.md"),
    ("docs/RC1_SCOPE.md", "RC1_SCOPE.md"),
]
PRODUCTION_RELEASE_DOC_SPECS = [
    ("docs/USER_GUIDE_D1L.md", "USER_GUIDE_D1L.md"),
    ("docs/D1L_SD_CARD_GUIDED_INSTALL.md", "D1L_SD_CARD_GUIDED_INSTALL.md"),
    ("docs/ADMIN_REMOTE_CLI_ALLOWLIST.md", "ADMIN_REMOTE_CLI_ALLOWLIST.md"),
    ("docs/RC1_SCOPE.md", "RC1_SCOPE.md"),
    ("docs/ATTRIBUTIONS.md", "ATTRIBUTIONS.md"),
]
NOTICE_FILE_SPECS = [
    ("LICENSE", "LICENSE"),
    ("THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md"),
    ("docs/ATTRIBUTIONS.md", "ATTRIBUTIONS.md"),
    ("docs/SOURCE_AUDIT_AND_ATTRIBUTION.md", "SOURCE_AUDIT_AND_ATTRIBUTION.md"),
    (
        "overlays/meshcore_ed25519_defined/license.txt",
        "ORLP_ED25519_ZLIB_LICENSE.txt",
    ),
]
PRODUCTION_NOTICE_FILE_SPECS = [
    ("LICENSE", "LICENSE"),
    ("THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md"),
    (
        "overlays/meshcore_ed25519_defined/license.txt",
        "ORLP_ED25519_ZLIB_LICENSE.txt",
    ),
]
PRODUCTION_FORBIDDEN_PATH_WORDS = frozenset(
    {"audit", "debug", "evidence", "gate", "smoke", "test", "testing", "tests"}
)
PRODUCTION_FORBIDDEN_READER_PATTERNS = (
    # The interoperable user channel is literally named "#test"; reject
    # qualification prose while allowing that exact hashtag in instructions.
    re.compile(r"(?<!#)\btests?\b", re.IGNORECASE),
    re.compile(r"\bsmoke\b", re.IGNORECASE),
    re.compile(r"\brelease[-_ ]evidence\b", re.IGNORECASE),
    re.compile(r"\brelease[-_ ]gate\b", re.IGNORECASE),
    re.compile(r"\bdry[-_ ]run\b", re.IGNORECASE),
    re.compile(r"\bsimulat(?:e|ed|ion)\b", re.IGNORECASE),
)
PRODUCTION_FORBIDDEN_ESP_PAYLOAD_MARKERS = (
    b"display test",
    b"touch test",
    b"ui scroll-probe",
    b"ui compose-probe",
    b"ui data-canary",
    b"ui capture status",
    b"ui capture begin",
    b"ui capture chunk",
    b"ui capture end",
    b"map acceptance status",
    b"map acceptance open",
    b"storage filecanary",
    b"storage map-tile-canary",
    b"storage map-tile-check",
    b"storage export-canary",
    b"storage retained-canary",
    b"core retained-witness",
    b"synthetic ui refresh canary",
    b"synthetic retained-history canary",
    b"test from deskos d1l",
    b"ui-data-canary",
    b"ui_canary",
    b"ui canary",
    b"sd-retained-canary",
    b"sd_canary",
    b"sd canary",
    b"canary/fc-",
    b"d1l desk",
    b"c0dec0dec0dec0de",
    b"contact probe",
    b"scroll probe dm",
    b"probe dm",
    b"probe contact",
    b"deskos probe",
    b"43.6532",
    b"probenet",
    b"probe-pass",
)
PRODUCTION_FORBIDDEN_RP2040_PAYLOAD_MARKERS = (
    b"/deskos/canary",
    b"deskos_canary_dir_unavailable",
    b"/deskos/probe.tmp",
    b"/deskos/probe.json",
    b"d1l-sd-file-ops-ready",
    b'{"schema":1,"probe":"d1l"}',
    b"deskos_sd_bridge_smoke",
    b"deskos_sd_bridge_official_smoke",
)
RP2040_ARTIFACT_NAMES = [
    "rp2040-sd-bridge-firmware",
    "rp2040-sd-smoke-firmware",
    "rp2040-seeed-official-sd-smoke-firmware",
]
PRODUCTION_RP2040_ARTIFACT_NAMES = (
    "rp2040-sd-bridge-firmware",
)
MESHCORE_CONFORMANCE_ARTIFACT_TYPE = "d1l_meshcore_wire_conformance"
MESHCORE_CONFORMANCE_BOUNDARY = "wire_envelope_only"
MESHCORE_CONFORMANCE_MAX_AGE_DAYS = 14
MESHCORE_CONFORMANCE_CLOCK_SKEW_MINUTES = 5
MESHCORE_CONFORMANCE_ACTIONS_ARTIFACT = "d1l-meshcore-wire-conformance"
MESHCORE_SIGNED_ADVERT_ACTIONS_ARTIFACT = MESHCORE_CONFORMANCE_ACTIONS_ARTIFACT
CORE_RELEASE_PROFILE = "core_1_0"
FULL_FEATURE_RELEASE_PROFILE = "full_feature"
CORE_PACKAGE_SCHEMA = 2
FULL_FEATURE_PACKAGE_SCHEMA = 2
CORE_INSTALL_CONTRACT_SCHEMA = 2
CORE_GENERATED_INSTALL_FILES = (
    "d1l_serial_target.py",
    "flash_project.py",
    "flash_project.ps1",
    "flash_project.sh",
    "flash_full_8mb.ps1",
    "docs/CORE_INSTALL_RECOVERY.md",
)
PRODUCTION_USER_INSTALL_FILES = (
    "START_HERE.md",
    "prepare_sd_card.ps1",
    "prepare_sd_card.sh",
    "flash_rp2040.ps1",
    "flash_rp2040.sh",
    "scripts/flash_rp2040_sd_bridge_uf2.py",
    "scripts/verify_package.py",
)
RELEASE_PROFILES = frozenset(
    {"development", CORE_RELEASE_PROFILE, FULL_FEATURE_RELEASE_PROFILE}
)
PRODUCTION_RELEASE_PROFILES = frozenset(
    {CORE_RELEASE_PROFILE, FULL_FEATURE_RELEASE_PROFILE}
)
SD_HISTORY_MODES = frozenset({"disabled", "conditional", "supported_optional"})
CORE_SUPPORTED_CAPABILITIES_BASE = (
    "board_initialization",
    "display_touch_backlight",
    "home_core_navigation",
    "public_messages",
    "direct_messages",
    "basic_contacts",
    "nodes",
    "packets",
    "route_signal_read_only",
    "radio_settings",
    "identity",
    "retained_nvs",
    "diagnostics",
    "time_truth",
    "map",
    "wifi_user_control",
    "multi_channel_management",
    "admin",
    "observer_mqtt",
    "mutable_terminal",
    "location",
    "user_trace",
)
CORE_UNAVAILABLE_CAPABILITIES_BASE = (
    "usb_recovery",
    "ble",
    "signed_update",
    "advanced_qr_emoji",
)
FULL_FEATURE_SUPPORTED_CAPABILITIES = (
    "board_initialization",
    "display_touch_backlight",
    "display_preferences_accessibility",
    "home_navigation",
    "public_messages",
    "direct_messages",
    "contacts",
    "heard_nodes",
    "packets",
    "multi_channel_management",
    "routes_and_trace",
    "radio_settings",
    "identity_and_adverts",
    "retained_nvs",
    "sd_history",
    "map",
    "wifi_user_control",
    "secure_ble_companion_core_protocol",
    "authenticated_repeater_room_admin",
    "observer_mqtt_tls",
    "signed_sd_ota_update",
    "usb_terminal",
    "location",
    "qr_sharing",
    "curated_glyph_palette",
    "notifications",
    "event_log",
    "service_control_sheets",
    "diagnostics_and_recovery",
    "time_truth",
)


def utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def parse_offset(value: str) -> int:
    return int(value, 0)


def validate_release_settings(
    release_profile: str, sd_history_mode: str
) -> tuple[str, str]:
    if release_profile not in RELEASE_PROFILES:
        raise ValueError(f"Unsupported release profile: {release_profile}")
    if sd_history_mode not in SD_HISTORY_MODES:
        raise ValueError(f"Unsupported SD history mode: {sd_history_mode}")
    return release_profile, sd_history_mode


def source_security_sequence(source_identity: dict) -> int:
    """Return the exact source-commit epoch used by the firmware build."""
    created = source_identity.get("created")
    if not isinstance(created, str) or not created.endswith("Z"):
        raise ValueError("D1L source commit timestamp is missing or non-canonical")
    try:
        timestamp = datetime.fromisoformat(created.replace("Z", "+00:00"))
        epoch = int(timestamp.timestamp())
    except (OverflowError, OSError, ValueError) as exc:
        raise ValueError("D1L source commit timestamp is invalid") from exc
    canonical = datetime.fromtimestamp(epoch, tz=timezone.utc).isoformat().replace(
        "+00:00", "Z"
    )
    if created != canonical or epoch < 1767225600 or epoch > 3978743295:
        raise ValueError(
            "D1L source commit timestamp is outside the release-safe sequence range"
        )
    return epoch


def build_release_settings(
    build_dir: Path,
    release_profile: str | None = None,
    sd_history_mode: str | None = None,
) -> tuple[str, str]:
    """Resolve the immutable firmware settings from the configured build."""
    values: dict[str, str] = {}
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        for raw_line in cache.read_text(encoding="utf-8", errors="strict").splitlines():
            if raw_line.startswith("D1L_RELEASE_PROFILE:"):
                values["release_profile"] = raw_line.split("=", 1)[-1].strip()
            elif raw_line.startswith("D1L_SD_HISTORY_MODE:"):
                values["sd_history_mode"] = raw_line.split("=", 1)[-1].strip()
    if (
        release_profile is not None
        and values.get("release_profile") is not None
        and release_profile != values["release_profile"]
    ):
        raise ValueError(
            "Explicit release profile does not match configured firmware"
        )
    if (
        sd_history_mode is not None
        and values.get("sd_history_mode") is not None
        and sd_history_mode != values["sd_history_mode"]
    ):
        raise ValueError(
            "Explicit SD history mode does not match configured firmware"
        )

    # The callable API keeps its historical full-feature defaults. The CLI
    # resolves real Actions packages from CMakeCache unless explicitly bound.
    profile = release_profile or values.get("release_profile") or "full_feature"
    sd_mode = sd_history_mode or values.get("sd_history_mode") or "conditional"
    return validate_release_settings(profile, sd_mode)


def core_capability_truth(sd_history_mode: str) -> dict:
    if sd_history_mode not in SD_HISTORY_MODES:
        raise ValueError(f"Unsupported SD history mode: {sd_history_mode}")
    supported = list(CORE_SUPPORTED_CAPABILITIES_BASE)
    unavailable = list(CORE_UNAVAILABLE_CAPABILITIES_BASE)
    if sd_history_mode != "disabled":
        supported.append("sd_history")
        sd_state = (
            "qualified_sd_primary"
            if sd_history_mode == "supported_optional"
            else "runtime_conditional_sd_primary"
        )
    else:
        unavailable.append("sd_history")
        sd_state = "disabled_nvs_authoritative"
    return {
        "supported_capabilities": supported,
        "unavailable_capabilities": unavailable,
        "sd_history_state": sd_state,
        "storage_authority": (
            "nvs"
            if sd_history_mode == "disabled"
            else "sd_primary_live_only_without_sd"
        ),
    }


def full_feature_capability_truth(sd_history_mode: str) -> dict:
    if sd_history_mode not in SD_HISTORY_MODES:
        raise ValueError(f"Unsupported SD history mode: {sd_history_mode}")
    return {
        "supported_capabilities": list(FULL_FEATURE_SUPPORTED_CAPABILITIES),
        "unavailable_capabilities": [],
        "sd_history_state": (
            "disabled_nvs_authoritative"
            if sd_history_mode == "disabled"
            else (
                "qualified_optional"
                if sd_history_mode == "supported_optional"
                else "runtime_conditional_on_verified_bridge"
            )
        ),
        "storage_authority": (
            "nvs"
            if sd_history_mode == "disabled"
            else "nvs_with_runtime_verified_sd_history"
        ),
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json(value: object) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def load_required_json_object(path: Path, label: str) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"{label} is missing or unreadable JSON: {path}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{label} must contain a JSON object")
    return value


def package_metadata_filename(contract_name: str, source_commit: str) -> str:
    source_commit = exact_sha(source_commit, "package metadata source commit")
    try:
        prefix = PACKAGE_METADATA_CONTRACTS[contract_name]["prefix"]
    except KeyError as exc:
        raise ValueError(f"Unknown package metadata contract: {contract_name}") from exc
    return f"{prefix}_{source_commit}.json"


def package_metadata_common(contract_name: str, source_commit: str) -> dict:
    source_commit = exact_sha(source_commit, "package metadata source commit")
    try:
        artifact_type = PACKAGE_METADATA_CONTRACTS[contract_name]["artifact_type"]
    except KeyError as exc:
        raise ValueError(f"Unknown package metadata contract: {contract_name}") from exc
    return {
        "schema_version": PACKAGE_METADATA_SCHEMA,
        "artifact_type": artifact_type,
        "source_commit": source_commit,
        "generated_package_metadata": True,
        "release_evidence": False,
        "physical_closure_claimed": False,
    }


def canonical_records(value: object, label: str) -> list[dict]:
    if not isinstance(value, list):
        raise ValueError(f"Completion ledger {label} must be a list")
    records: list[dict] = []
    identifiers: set[str] = set()
    for item in value:
        if not isinstance(item, dict):
            raise ValueError(f"Completion ledger {label} entries must be objects")
        identifier = item.get("id")
        if not isinstance(identifier, str) or not identifier.strip():
            raise ValueError(f"Completion ledger {label} entries require an id")
        if identifier in identifiers:
            raise ValueError(f"Completion ledger {label} contains duplicate id {identifier}")
        identifiers.add(identifier)
        records.append(json.loads(json.dumps(item)))
    return sorted(records, key=lambda item: item["id"])


def normalize_capabilities(ledger: dict) -> list[dict]:
    capabilities = canonical_records(ledger.get("capabilities"), "capabilities")
    if not capabilities:
        raise ValueError("Completion ledger capabilities must not be empty")
    for capability in capabilities:
        if not isinstance(capability.get("runtime_available"), bool):
            raise ValueError(
                f"Completion ledger capability {capability['id']} has invalid runtime_available"
            )
        status = capability.get("documentation_status")
        if not isinstance(status, str) or not status.strip():
            raise ValueError(
                f"Completion ledger capability {capability['id']} has invalid documentation_status"
            )
    return capabilities


def normalize_work_packages(ledger: dict) -> list[dict]:
    work_packages = canonical_records(ledger.get("work_packages"), "work_packages")
    if not work_packages:
        raise ValueError("Completion ledger work_packages must not be empty")
    normalized = []
    for work_package in work_packages:
        required_evidence = work_package.get("required_evidence")
        evidence = work_package.get("evidence")
        if (
            not isinstance(required_evidence, list)
            or any(not isinstance(item, str) or not item.strip() for item in required_evidence)
        ):
            raise ValueError(
                f"Completion ledger work package {work_package['id']} has invalid required_evidence"
            )
        if not isinstance(evidence, list) or any(not isinstance(item, dict) for item in evidence):
            raise ValueError(
                f"Completion ledger work package {work_package['id']} has invalid evidence"
            )
        status = work_package.get("status")
        if not isinstance(status, str) or not status.strip():
            raise ValueError(
                f"Completion ledger work package {work_package['id']} has invalid status"
            )
        normalized.append(
            {
                "id": work_package["id"],
                "title": work_package.get("title"),
                "status": status,
                "required_evidence": sorted(required_evidence),
                "evidence": sorted(
                    (json.loads(json.dumps(item)) for item in evidence),
                    key=lambda item: canonical_json(item),
                ),
            }
        )
    return normalized


def completion_ledger_binding(root: Path, ledger: dict, source_commit: str) -> dict:
    ledger_path = root / COMPLETION_LEDGER_SOURCE
    schema_version = ledger.get("schema_version")
    if schema_version != 1:
        raise ValueError("Completion ledger schema_version must be 1")
    snapshot_at = ledger.get("snapshot_at")
    release_posture = ledger.get("release_posture")
    if not isinstance(snapshot_at, str) or not snapshot_at.strip():
        raise ValueError("Completion ledger snapshot_at is missing")
    if not isinstance(release_posture, str) or not release_posture.strip():
        raise ValueError("Completion ledger release_posture is missing")
    repository = ledger.get("repository")
    main = repository.get("main") if isinstance(repository, dict) else None
    declared_commit = main.get("commit") if isinstance(main, dict) else None
    declared_commit = exact_sha(declared_commit, "completion ledger main commit")
    return {
        "path": COMPLETION_LEDGER_SOURCE.as_posix(),
        "sha256": sha256_file(ledger_path),
        "schema_version": schema_version,
        "snapshot_at": snapshot_at,
        "release_posture": release_posture,
        "declared_main_commit": declared_commit,
        "declared_main_matches_package": declared_commit == source_commit,
    }


def validate_generated_package_metadata(
    package_dir: Path,
    metadata: object,
    source_commit: str,
    contract_name: str,
) -> dict:
    source_commit = exact_sha(source_commit, "package metadata source commit")
    if not isinstance(metadata, dict):
        raise ValueError(f"Package manifest {contract_name} binding must be an object")
    contract = PACKAGE_METADATA_CONTRACTS.get(contract_name)
    if contract is None:
        raise ValueError(f"Unknown package metadata contract: {contract_name}")
    expected_path = package_metadata_filename(contract_name, source_commit)
    required_metadata = {
        "schema_version": PACKAGE_METADATA_SCHEMA,
        "artifact_type": contract["artifact_type"],
        "path": expected_path,
        "source_commit": source_commit,
        "generated_package_metadata": True,
        "release_evidence": False,
        "physical_closure_claimed": False,
        "valid": True,
    }
    failed = [name for name, value in required_metadata.items() if metadata.get(name) != value]
    if failed:
        raise ValueError(
            f"Package manifest {contract_name} binding is stale or invalid: "
            + ", ".join(failed)
        )
    target = (package_dir / expected_path).resolve()
    try:
        target.relative_to(package_dir.resolve())
    except ValueError as exc:
        raise ValueError(f"Package metadata path escapes the package: {expected_path}") from exc
    if not target.is_file():
        raise FileNotFoundError(f"Missing exact-SHA package metadata {expected_path}")
    size = metadata.get("size")
    digest = str(metadata.get("sha256") or "").lower()
    if isinstance(size, bool) or not isinstance(size, int) or size <= 0:
        raise ValueError(f"Package manifest {contract_name} size is invalid")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError(f"Package manifest {contract_name} SHA256 is invalid")
    if target.stat().st_size != size or sha256_file(target) != digest:
        raise ValueError(f"Package manifest {contract_name} checksum or size is stale")
    payload = load_required_json_object(target, f"Packaged {contract_name}")
    required_payload = package_metadata_common(contract_name, source_commit)
    failed = [name for name, value in required_payload.items() if payload.get(name) != value]
    if failed:
        raise ValueError(
            f"Packaged {contract_name} identity is stale or invalid: " + ", ".join(failed)
        )
    return payload


def write_package_metadata_artifact(
    package_dir: Path, contract_name: str, source_commit: str, payload: dict
) -> dict:
    filename = package_metadata_filename(contract_name, source_commit)
    path = package_dir / filename
    path.write_text(canonical_json(payload), encoding="ascii")
    metadata = {
        **package_metadata_common(contract_name, source_commit),
        "path": filename,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
        "valid": True,
    }
    validate_generated_package_metadata(package_dir, metadata, source_commit, contract_name)
    return metadata


def package_inventory_payloads(
    root: Path,
    source_commit: str,
    release_profile: str = "full_feature",
    sd_history_mode: str = "conditional",
) -> dict[str, dict]:
    source_commit = exact_sha(source_commit, "package metadata source commit")
    build_inputs_path = root / BUILD_INPUTS_SOURCE
    build_inputs = load_required_json_object(build_inputs_path, "D1L build-input lock")
    if build_inputs.get("schema") != 1 or build_inputs.get("kind") != "d1l_build_inputs":
        raise ValueError("D1L build-input lock identity is invalid")
    host_python = build_inputs.get("host_python")
    requirements = host_python.get("requirements") if isinstance(host_python, dict) else None
    requirements_path = requirements.get("path") if isinstance(requirements, dict) else None
    requirements_digest = requirements.get("sha256") if isinstance(requirements, dict) else None
    host_requirements_path = root / HOST_REQUIREMENTS_SOURCE
    if (
        requirements_path != HOST_REQUIREMENTS_SOURCE.as_posix()
        or not host_requirements_path.is_file()
        or requirements_digest != sha256_file(host_requirements_path)
    ):
        raise ValueError("D1L build-input lock host requirements binding is stale")
    build_payload = {
        **package_metadata_common("build_inputs", source_commit),
        "source": {
            "path": BUILD_INPUTS_SOURCE.as_posix(),
            "sha256": sha256_file(build_inputs_path),
        },
        "build_inputs": build_inputs,
    }

    ledger_path = root / COMPLETION_LEDGER_SOURCE
    ledger = load_required_json_object(ledger_path, "Completion ledger")
    ledger_binding = completion_ledger_binding(root, ledger, source_commit)
    capabilities = normalize_capabilities(ledger)
    work_packages = normalize_work_packages(ledger)
    blockers = canonical_records(ledger.get("blockers"), "blockers")
    capability_payload = {
        **package_metadata_common("capability_manifest", source_commit),
        "ledger_source": ledger_binding,
        "release_posture": ledger_binding["release_posture"],
        "capabilities": capabilities,
        "note": (
            "Generated from the completion ledger for package inventory only; "
            "it is not new release evidence or physical closure."
        ),
    }
    evidence_payload = {
        **package_metadata_common("release_evidence_index", source_commit),
        "ledger_source": ledger_binding,
        "release_posture": ledger_binding["release_posture"],
        "release_ready": False,
        "readiness_evaluated_by_packaging": False,
        "work_packages": work_packages,
        "blockers": blockers,
        "note": (
            "This deterministically indexes ledger claims without validating, replacing, "
            "or creating release evidence."
        ),
    }
    if release_profile == CORE_RELEASE_PROFILE:
        truth = core_capability_truth(sd_history_mode)
        capability_payload.update(
            {
                "release_profile": release_profile,
                "sd_history_mode": sd_history_mode,
                "supported_capabilities": truth["supported_capabilities"],
                "unavailable_capabilities": truth["unavailable_capabilities"],
                "full_feature_release_ready": False,
                "capabilities": [
                    {"id": capability, "core_state": "supported"}
                    for capability in truth["supported_capabilities"]
                ]
                + [
                    {"id": capability, "core_state": "unavailable"}
                    for capability in truth["unavailable_capabilities"]
                ],
                "note": (
                    "Core profile capability truth generated from the compiled "
                    "core_1_0 contract. Archived ledger capability "
                    "claims are intentionally not projected into this package."
                ),
            }
        )
        evidence_payload.update(
            {
                "release_profile": release_profile,
                "sd_history_mode": sd_history_mode,
                "core_release_ready": False,
                "full_feature_release_ready": False,
                "work_packages": [],
                "blockers": [],
                "core_evidence_requirements": [
                    "exact_actions_core_1_0_candidate",
                    "checksums_provenance_sbom",
                    "stable_pi5_by_id_non_erasing_flash",
                    "boot_ui_navigation_and_boot_advert_public",
                    "dm_ack_path_trace_ping",
                    "repeater_login_authenticated_query",
                    "wifi_reconnect",
                    "sd_write_remount_and_degraded_mode",
                    "authorized_map_download_offline_cache_revisit",
                    "zero_rc1_release_blockers",
                ],
                "note": (
                    "Packaging does not evaluate Core release readiness. "
                    "scripts/rc1_release_gate_audit_d1l.py evaluates exact "
                    "candidate evidence separately."
                ),
            }
        )
    elif release_profile == FULL_FEATURE_RELEASE_PROFILE:
        truth = full_feature_capability_truth(sd_history_mode)
        capability_payload.update(
            {
                "release_profile": release_profile,
                "sd_history_mode": sd_history_mode,
                "supported_capabilities": truth["supported_capabilities"],
                "unavailable_capabilities": truth["unavailable_capabilities"],
                "full_feature_release_ready": False,
                "capabilities": [
                    {"id": capability, "full_feature_state": "supported"}
                    for capability in truth["supported_capabilities"]
                ],
                "note": (
                    "Full Feature capability truth generated from the immutable "
                    "production profile. Packaging does not itself close the "
                    "exact-candidate or physical release gates."
                ),
            }
        )
        evidence_payload.update(
            {
                "release_profile": release_profile,
                "sd_history_mode": sd_history_mode,
                "full_feature_release_ready": False,
                "full_feature_evidence_requirements": [
                    "exact_actions_candidate",
                    "signed_update_bundle",
                    "checksums_provenance_sbom",
                    "stable_by_id_flash",
                    "automated_device_acceptance",
                    "controlled_rf_acceptance",
                    "ble_companion_acceptance",
                    "wifi_map_observer_acceptance",
                    "sd_update_recovery_acceptance",
                    "final_physical_ui_confirmation",
                    "zero_release_blocking_defects",
                ],
                "note": (
                    "Packaging indexes the current ledger but does not evaluate "
                    "Full Feature readiness or create physical evidence."
                ),
            }
        )
    return {
        "build_inputs": build_payload,
        "capability_manifest": capability_payload,
        "release_evidence_index": evidence_payload,
    }


def write_package_inventory_metadata(
    root: Path,
    package_dir: Path,
    source_commit: str,
    release_profile: str = "full_feature",
    sd_history_mode: str = "conditional",
    include_release_evidence_index: bool = True,
    include_internal_metadata: bool = True,
) -> dict[str, dict]:
    if not include_internal_metadata:
        return {}
    payloads = package_inventory_payloads(
        root,
        source_commit,
        release_profile=release_profile,
        sd_history_mode=sd_history_mode,
    )
    if not include_release_evidence_index:
        payloads.pop("release_evidence_index", None)
    return {
        contract_name: write_package_metadata_artifact(
            package_dir, contract_name, source_commit, payload
        )
        for contract_name, payload in payloads.items()
    }


def parse_utc_timestamp(value: object, field: str) -> datetime:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"MeshCore conformance {field} is missing")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (ValueError, OverflowError) as exc:
        raise ValueError(f"MeshCore conformance {field} is not a valid timestamp") from exc
    if parsed.tzinfo is None:
        raise ValueError(f"MeshCore conformance {field} must include a timezone")
    try:
        return parsed.astimezone(timezone.utc)
    except (ValueError, OverflowError) as exc:
        raise ValueError(f"MeshCore conformance {field} is outside the supported range") from exc


def copy_meshcore_conformance_evidence(
    source: Path | None,
    signed_advert_runtime_source: Path | None,
    root: Path,
    package_dir: Path,
    expected_commit: str | None,
    *,
    include_in_package: bool = True,
) -> dict | None:
    if source is None:
        return None
    source = source.resolve()
    if not source.is_file():
        raise FileNotFoundError(f"Missing MeshCore conformance JSON {source}")
    if not expected_commit:
        raise ValueError("Cannot verify MeshCore conformance without an expected release commit")
    try:
        report = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"MeshCore conformance JSON is unreadable: {source}") from exc
    if not isinstance(report, dict):
        raise ValueError("MeshCore conformance JSON must contain an object")
    validate_completed_report(
        report,
        expected_commit,
        build_inputs_path=root / BUILD_INPUTS_SOURCE,
        require_signed_advert_runtime_receipt=False,
    )
    if signed_advert_runtime_source is None:
        raise ValueError(
            "Cannot verify MeshCore conformance without signed-advert runtime evidence"
        )
    signed_advert_runtime_source = signed_advert_runtime_source.resolve()
    if not signed_advert_runtime_source.is_file():
        raise FileNotFoundError(
            "Missing MeshCore signed-advert runtime JSON "
            f"{signed_advert_runtime_source}"
        )
    try:
        signed_advert_runtime_report = json.loads(
            signed_advert_runtime_source.read_text(encoding="utf-8")
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(
            "MeshCore signed-advert runtime JSON is unreadable: "
            f"{signed_advert_runtime_source}"
        ) from exc
    validate_completed_report(
        report,
        expected_commit,
        build_inputs_path=root / BUILD_INPUTS_SOURCE,
        signed_advert_runtime_receipt=signed_advert_runtime_report,
    )

    source_verification = report.get("source_verification")
    source_commit = (
        source_verification.get("repository_commit")
        if isinstance(source_verification, dict)
        else None
    )
    required = {
        "schema_version": report.get("schema_version") == 1,
        "artifact_type": report.get("artifact_type") == MESHCORE_CONFORMANCE_ARTIFACT_TYPE,
        "passed": report.get("passed") is True,
        "status": report.get("status") == "pass",
        "execution_complete": report.get("execution_complete") is True,
        "coverage_boundary": report.get("coverage_boundary") == MESHCORE_CONFORMANCE_BOUNDARY,
        "coverage_level": report.get("coverage_level") == MESHCORE_CONFORMANCE_BOUNDARY,
        "closure_ready_false": report.get("closure_ready") is False,
        "issue_65_closure_eligible_false": report.get("issue_65_closure_eligible") is False,
        "source_commit": source_commit == expected_commit,
    }
    failed = [name for name, ok in required.items() if not ok]
    if failed:
        raise ValueError("MeshCore conformance validation failed: " + ", ".join(failed))

    generated_at = parse_utc_timestamp(report.get("generated_at"), "generated_at")
    now = datetime.now(timezone.utc)
    if generated_at > now + timedelta(minutes=MESHCORE_CONFORMANCE_CLOCK_SKEW_MINUTES):
        raise ValueError("MeshCore conformance generated_at is in the future")
    try:
        expires_at = generated_at + timedelta(days=MESHCORE_CONFORMANCE_MAX_AGE_DAYS)
    except OverflowError as exc:
        raise ValueError("MeshCore conformance generated_at is outside the supported range") from exc
    if now >= expires_at:
        raise ValueError("MeshCore conformance evidence is expired")

    expected_name = f"meshcore_conformance_{expected_commit}.json"
    if source.name != expected_name:
        raise ValueError(
            f"MeshCore conformance filename must be {expected_name}, got {source.name}"
        )
    raw_size = source.stat().st_size
    raw_sha256 = sha256_file(source)
    canonical_report = canonicalize_release_report(report)
    if canonical_report.get("evidence_profile") != CANONICAL_EVIDENCE_PROFILE:
        raise ValueError("MeshCore conformance canonical evidence profile is invalid")
    if not include_in_package:
        return None

    evidence_dir = package_dir / "evidence"
    evidence_dir.mkdir(parents=True, exist_ok=True)
    dest = evidence_dir / expected_name
    dest.write_text(
        json.dumps(canonical_report, indent=2, sort_keys=True, ensure_ascii=True) + "\n",
        encoding="ascii",
    )
    return {
        "artifact_type": MESHCORE_CONFORMANCE_ARTIFACT_TYPE,
        "path": dest.relative_to(package_dir).as_posix(),
        "size": dest.stat().st_size,
        "sha256": sha256_file(dest),
        "source_commit": source_commit,
        "evidence_profile": CANONICAL_EVIDENCE_PROFILE,
        "run_receipt": {
            "artifact": MESHCORE_CONFORMANCE_ACTIONS_ARTIFACT,
            "path": expected_name,
            "size": raw_size,
            "sha256": raw_sha256,
            "generated_at": generated_at.isoformat().replace("+00:00", "Z"),
            "expires_at": expires_at.isoformat().replace("+00:00", "Z"),
        },
        "max_age_days": MESHCORE_CONFORMANCE_MAX_AGE_DAYS,
        "coverage_boundary": MESHCORE_CONFORMANCE_BOUNDARY,
        "coverage_level": MESHCORE_CONFORMANCE_BOUNDARY,
        "closure_ready": False,
        "issue_65_closure_eligible": False,
        "passed": True,
        "execution_complete": True,
        "note": "Structural wire-envelope prerequisite only; this evidence does not close issue #65.",
    }


def copy_meshcore_signed_advert_evidence(
    source: Path | None,
    package_dir: Path,
    expected_commit: str | None,
    *,
    include_in_package: bool = True,
) -> dict | None:
    """Validate a raw Actions receipt and package its deterministic projection."""

    if source is None:
        return None
    source = source.resolve()
    if not source.is_file():
        raise FileNotFoundError(f"Missing MeshCore signed-advert runtime JSON {source}")
    if not expected_commit:
        raise ValueError(
            "Cannot verify MeshCore signed-advert runtime without an expected release commit"
        )
    try:
        report = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(
            f"MeshCore signed-advert runtime JSON is unreadable: {source}"
        ) from exc
    validate_signed_advert_report(report, expected_commit)

    generated_at = parse_utc_timestamp(report.get("generated_at"), "generated_at")
    now = datetime.now(timezone.utc)
    if generated_at > now + timedelta(minutes=MESHCORE_CONFORMANCE_CLOCK_SKEW_MINUTES):
        raise ValueError("MeshCore signed-advert runtime generated_at is in the future")
    try:
        expires_at = generated_at + timedelta(days=MESHCORE_CONFORMANCE_MAX_AGE_DAYS)
    except OverflowError as exc:
        raise ValueError(
            "MeshCore signed-advert runtime generated_at is outside the supported range"
        ) from exc
    if now >= expires_at:
        raise ValueError("MeshCore signed-advert runtime evidence is expired")

    expected_name = f"meshcore_signed_advert_runtime_{expected_commit}.json"
    if source.name != expected_name:
        raise ValueError(
            f"MeshCore signed-advert runtime filename must be {expected_name}, "
            f"got {source.name}"
        )
    canonical_report = canonicalize_signed_advert_report(report, expected_commit)
    if canonical_report.get("evidence_profile") != SIGNED_ADVERT_EVIDENCE_PROFILE:
        raise ValueError("MeshCore signed-advert canonical evidence profile is invalid")
    if not include_in_package:
        return None

    evidence_dir = package_dir / "evidence"
    evidence_dir.mkdir(parents=True, exist_ok=True)
    destination = evidence_dir / expected_name
    destination.write_text(
        json.dumps(canonical_report, indent=2, sort_keys=True, ensure_ascii=True) + "\n",
        encoding="ascii",
    )
    repository = report["repository"]
    return {
        "artifact_type": SIGNED_ADVERT_ARTIFACT_TYPE,
        "path": destination.relative_to(package_dir).as_posix(),
        "size": destination.stat().st_size,
        "sha256": sha256_file(destination),
        "source_commit": repository["repository_commit"],
        "evidence_profile": SIGNED_ADVERT_EVIDENCE_PROFILE,
        "run_receipt": {
            "artifact": MESHCORE_SIGNED_ADVERT_ACTIONS_ARTIFACT,
            "path": expected_name,
            "size": source.stat().st_size,
            "sha256": sha256_file(source),
            "generated_at": generated_at.isoformat().replace("+00:00", "Z"),
            "expires_at": expires_at.isoformat().replace("+00:00", "Z"),
        },
        "max_age_days": MESHCORE_CONFORMANCE_MAX_AGE_DAYS,
        "coverage_boundary": report["coverage_boundary"],
        "wp04_closure_eligible": False,
        "closure_ready": False,
        "full_ubsan_clean": True,
        "passed": True,
        "execution_complete": True,
        "note": (
            "Pinned signed-advert semantic runtime prerequisite only; broader "
            "WP-04 protocol and physical closure remain open."
        ),
    }


def git_value(root: Path, *args: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None
    # Porcelain status uses the first two columns as semantic XY state. Keep
    # leading spaces so a worktree-only submodule edit cannot be confused with
    # a staged gitlink change.
    return result.stdout.rstrip("\r\n") or None


def command_succeeds(cwd: Path, args: list[str]) -> bool:
    try:
        subprocess.run(
            args,
            cwd=cwd,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return False
    return True


def expected_bsp_patches_applied(root: Path) -> bool:
    root = root.resolve()
    submodule = root / EXPECTED_BSP_SUBMODULE
    if not submodule.exists():
        return False

    for relative_patch in EXPECTED_BSP_PATCHES:
        patch = root / relative_patch
        if not patch.exists() or not command_succeeds(
            submodule,
            [
                "git",
                "apply",
                "--unidiff-zero",
                "--reverse",
                "--check",
                "--ignore-space-change",
                str(patch),
            ],
        ):
            return False
    return True


def run_git_command(
    cwd: Path,
    args: list[str],
    *,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str] | None:
    try:
        return subprocess.run(
            ["git", *args],
            cwd=cwd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            env=env,
        )
    except (FileNotFoundError, subprocess.CalledProcessError, OSError):
        return None


def exact_expected_bsp_patch_state(root: Path) -> bool:
    """Prove the BSP worktree is exactly HEAD plus the two tracked patches."""
    root = root.resolve()
    submodule = root / EXPECTED_BSP_SUBMODULE
    patches = [root / relative for relative in EXPECTED_BSP_PATCHES]
    if not submodule.is_dir() or any(not patch.is_file() for patch in patches):
        return False

    gitlink = run_git_command(
        root, ["ls-tree", "HEAD", "--", EXPECTED_BSP_SUBMODULE.as_posix()]
    )
    submodule_head = run_git_command(submodule, ["rev-parse", "HEAD"])
    if gitlink is None or submodule_head is None:
        return False
    match = re.fullmatch(
        r"160000 commit ([0-9a-f]{40})\t" + re.escape(EXPECTED_BSP_SUBMODULE.as_posix()),
        gitlink.stdout.strip(),
    )
    if match is None or submodule_head.stdout.strip() != match.group(1):
        return False

    # The expected build patches are unstaged worktree edits. Reject staged or
    # untracked content before comparing the exact tracked tree.
    if run_git_command(submodule, ["diff", "--cached", "--quiet", "--exit-code"]) is None:
        return False
    untracked = run_git_command(
        submodule, ["ls-files", "--others", "--exclude-standard"]
    )
    if untracked is None or untracked.stdout.strip():
        return False

    with tempfile.TemporaryDirectory(prefix="d1l-bsp-patch-state-") as temporary:
        temp = Path(temporary)
        expected_env = os.environ.copy()
        expected_env["GIT_INDEX_FILE"] = str(temp / "expected.index")
        actual_env = os.environ.copy()
        actual_env["GIT_INDEX_FILE"] = str(temp / "actual.index")

        if run_git_command(submodule, ["read-tree", "HEAD"], env=expected_env) is None:
            return False
        for patch in patches:
            if run_git_command(
                submodule,
                [
                    "apply",
                    "--cached",
                    "--unidiff-zero",
                    "--ignore-space-change",
                    str(patch),
                ],
                env=expected_env,
            ) is None:
                return False
        expected_tree = run_git_command(submodule, ["write-tree"], env=expected_env)

        if run_git_command(submodule, ["read-tree", "HEAD"], env=actual_env) is None:
            return False
        if run_git_command(submodule, ["add", "-u", "--", "."], env=actual_env) is None:
            return False
        actual_tree = run_git_command(submodule, ["write-tree"], env=actual_env)
        return (
            expected_tree is not None
            and actual_tree is not None
            and expected_tree.stdout.strip() == actual_tree.stdout.strip()
        )


def clean_release_status_entries(root: Path, status: str) -> tuple[list[str], list[str]]:
    entries = [line for line in status.splitlines() if line.strip()]
    if not entries:
        return [], []

    expected_submodule = EXPECTED_BSP_SUBMODULE.as_posix()
    expected_entries = [line for line in entries if status_path(line) == expected_submodule]
    other_entries = [line for line in entries if status_path(line) != expected_submodule]
    expected_entry_is_worktree_only = (
        len(expected_entries) == 1
        and len(expected_entries[0]) >= 3
        and expected_entries[0][0] == " "
        and expected_entries[0][1] in {"M", "m"}
    )
    if (
        expected_entry_is_worktree_only
        and exact_expected_bsp_patch_state(root)
    ):
        return other_entries, [patch.as_posix() for patch in EXPECTED_BSP_PATCHES]

    return entries, []


def status_path(status_line: str) -> str:
    parts = status_line.split(maxsplit=1)
    return parts[1] if len(parts) == 2 else ""


def git_info(root: Path) -> dict:
    root = root.resolve()
    status = git_value(root, "status", "--porcelain") or ""
    dirty_entries, source_patches = clean_release_status_entries(root, status)
    return {
        "commit": git_value(root, "rev-parse", "HEAD"),
        "short_commit": git_value(root, "rev-parse", "--short", "HEAD"),
        "branch": git_value(root, "branch", "--show-current"),
        "dirty": bool(dirty_entries),
        "dirty_entries": dirty_entries,
        "source_patches": source_patches,
    }


def load_flasher_args(build_dir: Path) -> dict:
    path = build_dir / "flasher_args.json"
    if not path.exists():
        raise FileNotFoundError(f"Missing {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def ordered_flash_files(flasher_args: dict) -> list[tuple[int, str]]:
    files = flasher_args.get("flash_files", {})
    if not isinstance(files, dict) or not files:
        raise ValueError("flasher_args.json has no flash_files map")
    return sorted((parse_offset(offset), rel_path) for offset, rel_path in files.items())


def flash_role_for_path(path: str) -> str:
    name = Path(path).name
    if "bootloader" in path.replace("\\", "/"):
        return "bootloader"
    if name == "partition-table.bin":
        return "partition-table"
    if name == "ota_data_initial.bin":
        return "ota-data"
    if name.endswith(".bin"):
        return "app"
    return "artifact"


def copy_flash_files(build_dir: Path, firmware_dir: Path, flasher_args: dict) -> list[dict]:
    firmware_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    for offset, rel_path in ordered_flash_files(flasher_args):
        source = build_dir / rel_path
        if not source.exists():
            raise FileNotFoundError(f"Missing flash file {source}")
        dest = firmware_dir / Path(rel_path).name
        shutil.copy2(source, dest)
        entries.append(
            {
                "role": flash_role_for_path(rel_path),
                "offset": f"0x{offset:x}",
                "source": rel_path.replace("\\", "/"),
                "path": dest.relative_to(firmware_dir.parent).as_posix(),
                "size": dest.stat().st_size,
                "sha256": sha256_file(dest),
            }
        )
    flasher_dest = firmware_dir / "flasher_args.json"
    shutil.copy2(build_dir / "flasher_args.json", flasher_dest)
    return entries


def copy_optional_debug_files(build_dir: Path, package_dir: Path) -> list[dict]:
    debug_dir = package_dir / "debug"
    copied = []
    for pattern in ["*.elf", "*.map"]:
        for source in sorted(build_dir.glob(pattern)):
            debug_dir.mkdir(parents=True, exist_ok=True)
            dest = debug_dir / source.name
            shutil.copy2(source, dest)
            copied.append(
                {
                    "path": dest.relative_to(package_dir).as_posix(),
                    "size": dest.stat().st_size,
                    "sha256": sha256_file(dest),
                }
            )
    return copied


def copy_notice_files(
    root: Path,
    package_dir: Path,
    specs: list[tuple[str, str]] = NOTICE_FILE_SPECS,
) -> list[dict]:
    notices_dir = package_dir / "notices"
    copied = []
    for source_rel, dest_name in specs:
        source = root / source_rel
        if not source.exists():
            continue
        notices_dir.mkdir(parents=True, exist_ok=True)
        dest = notices_dir / dest_name
        shutil.copy2(source, dest)
        copied.append({
            "path": dest.relative_to(package_dir).as_posix(),
            "source": source_rel,
            "sha256": sha256_file(dest),
        })
    return copied


def copy_release_docs(
    root: Path,
    package_dir: Path,
    specs: list[tuple[str, str]] = RELEASE_DOC_SPECS,
) -> list[dict]:
    docs_dir = package_dir / "docs"
    copied = []
    for source_rel, dest_name in specs:
        source = root / source_rel
        if not source.exists():
            continue
        docs_dir.mkdir(parents=True, exist_ok=True)
        dest = docs_dir / dest_name
        shutil.copy2(source, dest)
        copied.append({
            "path": dest.relative_to(package_dir).as_posix(),
            "source": source_rel,
            "sha256": sha256_file(dest),
        })
    return copied


def validate_production_package_surface(package_dir: Path) -> None:
    """Reject internal qualification material from the customer package."""

    for path in sorted(package_dir.rglob("*")):
        if is_link_or_reparse(path):
            raise ValueError(f"Production package contains an unsafe link: {path}")
        relative = path.relative_to(package_dir)
        if path.is_file() and path.suffix.lower() in {".elf", ".map"}:
            raise ValueError(
                "Production package contains a debug-only file: "
                f"{relative.as_posix()}"
            )
        if path.is_file() and path.suffix.lower() in {".bin", ".uf2"}:
            payload = path.read_bytes().lower()
            markers = (
                PRODUCTION_FORBIDDEN_RP2040_PAYLOAD_MARKERS
                if path.suffix.lower() == ".uf2"
                else PRODUCTION_FORBIDDEN_ESP_PAYLOAD_MARKERS
            )
            for marker in markers:
                if marker in payload:
                    raise ValueError(
                        "Production firmware payload contains an internal "
                        f"qualification marker: {relative.as_posix()}: "
                        f"{marker.decode('ascii', errors='replace')}"
                    )
        for component in relative.parts:
            words = {
                word
                for word in re.split(r"[^a-z0-9]+", component.lower())
                if word
            }
            forbidden = sorted(words & PRODUCTION_FORBIDDEN_PATH_WORDS)
            if forbidden:
                raise ValueError(
                    "Production package contains internal-only path "
                    f"{relative.as_posix()}: {', '.join(forbidden)}"
                )
        if not path.is_file() or path.suffix.lower() not in {".md", ".txt"}:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise ValueError(
                f"Production reader document is not valid UTF-8: {relative.as_posix()}"
            ) from exc
        for pattern in PRODUCTION_FORBIDDEN_READER_PATTERNS:
            if pattern.search(text):
                raise ValueError(
                    "Production reader document contains internal qualification "
                    f"language: {relative.as_posix()}"
                )


def copy_sd_preparation_bundle(root: Path, package_dir: Path) -> dict:
    sources = (
        root / "scripts" / "prepare_deskos_sd.py",
        root / "sdcard",
    )
    for source in sources:
        if not source.exists() or is_link_or_reparse(source):
            raise FileNotFoundError(
                f"Missing or unsafe SD preparation source: {source}"
            )

    files = [
        sources[0],
        *sorted(path for path in sources[1].rglob("*") if path.is_file()),
    ]
    copied = []
    for source in files:
        if is_link_or_reparse(source):
            raise ValueError(f"SD preparation source must not be linked: {source}")
        relative = source.relative_to(root)
        destination = package_dir / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if relative.as_posix() == "scripts/prepare_deskos_sd.py":
            text = source.read_text(encoding="utf-8")
            bypass_option = """    parser.add_argument(
        "--skip-filesystem-check",
        action="store_true",
        help="staging/test directories only; never use this to bypass a real card check",
    )
"""
            bypass_call = "ensure_target(args.target, args.skip_filesystem_check)"
            has_bypass_option = bypass_option in text
            has_bypass_call = bypass_call in text
            if has_bypass_option != has_bypass_call:
                raise ValueError(
                    "Production SD preparer source does not match the reviewed "
                    "filesystem-bypass boundary"
                )
            destination.write_text(
                (
                    text.replace(bypass_option, "").replace(
                        bypass_call,
                        "ensure_target(args.target, False)",
                    )
                    if has_bypass_option
                    else text
                ),
                encoding="utf-8",
            )
        else:
            shutil.copy2(source, destination)
        copied.append(
            {
                "path": relative.as_posix(),
                "size": destination.stat().st_size,
                "sha256": sha256_file(destination),
            }
        )

    return {
        "schema": 1,
        "script": "scripts/prepare_deskos_sd.py",
        "bundle_root": "sdcard",
        "minimum_card_bytes": 28_000_000_000,
        "filesystem": "FAT32",
        "formats_sd": False,
        "files": copied,
    }


def write_production_user_install_bundle(
    root: Path,
    package_dir: Path,
    *,
    source_commit: str,
    sd_history_mode: str,
) -> dict:
    """Write checksum-bound Windows/Linux production install helpers."""

    source_scripts = {
        "scripts/flash_rp2040_sd_bridge_uf2.py": (
            root / "scripts" / "flash_rp2040_sd_bridge_uf2.py"
        ),
        "scripts/verify_package.py": root / "scripts" / "verify_checksums.py",
    }
    for relative, source in source_scripts.items():
        if (
            not source.is_file()
            or is_link_or_reparse(source)
            or source.stat().st_size <= 0
        ):
            raise ValueError(f"Production user helper source is invalid: {source}")
        destination = package_dir / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if relative == "scripts/flash_rp2040_sd_bridge_uf2.py":
            text = source.read_text(encoding="utf-8")
            if '"dry-run"' not in text or "default is dry-run" not in text:
                raise ValueError(
                    "Production RP2040 helper source does not match the reviewed "
                    "preview boundary"
                )
            destination.write_text(
                text.replace('"dry-run"', '"preview"').replace(
                    "default is dry-run", "default is preview"
                ),
                encoding="utf-8",
            )
        else:
            shutil.copy2(source, destination)

    bridge_relative = (
        "rp2040/rp2040-sd-bridge-firmware/deskos_sd_bridge.ino.uf2"
    )
    bridge_path = package_dir / bridge_relative
    if (
        not bridge_path.is_file()
        or is_link_or_reparse(bridge_path)
        or bridge_path.stat().st_size <= 0
    ):
        raise ValueError(
            "DeskOS 1.0 install requires the production RP2040 bridge UF2"
        )

    prepare_ps1 = package_dir / "prepare_sd_card.ps1"
    prepare_ps1.write_text(
        "\n".join(
            (
                "param([Parameter(Mandatory=$true)][string]$Target)",
                '$ErrorActionPreference = "Stop"',
                '$Root = Split-Path -Parent $MyInvocation.MyCommand.Path',
                'python (Join-Path $Root "scripts/verify_package.py") $Root',
                'if ($LASTEXITCODE -ne 0) { throw "Package verification failed." }',
                'python (Join-Path $Root "scripts/prepare_deskos_sd.py") --target $Target',
                'if ($LASTEXITCODE -ne 0) { throw "SD card preflight failed." }',
                '$Confirm = Read-Host "Review the target above. Type PREPARE-SD to copy the DeskOS files without formatting or deleting"',
                'if ($Confirm -ne "PREPARE-SD") { throw "SD preparation cancelled." }',
                'python (Join-Path $Root "scripts/prepare_deskos_sd.py") --target $Target --apply',
                'if ($LASTEXITCODE -ne 0) { throw "SD card preparation failed." }',
                'Write-Host "DeskOS SD preparation and byte verification passed."',
                "",
            )
        ),
        encoding="ascii",
    )
    prepare_sh = package_dir / "prepare_sd_card.sh"
    prepare_sh.write_text(
        "\n".join(
            (
                "#!/usr/bin/env sh",
                "set -eu",
                'if [ "$#" -ne 1 ]; then',
                '  printf "%s\\n" "Usage: $0 /path/to/mounted-sd-card" >&2',
                "  exit 2",
                "fi",
                'ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"',
                'PYTHON_BIN="${D1L_PYTHON:-python3}"',
                'TARGET="$1"',
                '"$PYTHON_BIN" "$ROOT/scripts/verify_package.py" "$ROOT"',
                '"$PYTHON_BIN" "$ROOT/scripts/prepare_deskos_sd.py" --target "$TARGET"',
                'printf "%s" "Review the target above. Type PREPARE-SD to copy without formatting or deleting: "',
                "IFS= read -r CONFIRM",
                'if [ "$CONFIRM" != "PREPARE-SD" ]; then',
                '  printf "%s\\n" "SD preparation cancelled." >&2',
                "  exit 2",
                "fi",
                '"$PYTHON_BIN" "$ROOT/scripts/prepare_deskos_sd.py" --target "$TARGET" --apply',
                'printf "%s\\n" "DeskOS SD preparation and byte verification passed."',
                "",
            )
        ),
        encoding="ascii",
    )
    prepare_sh.chmod(0o755)

    rp2040_ps1 = package_dir / "flash_rp2040.ps1"
    rp2040_ps1.write_text(
        "\n".join(
            (
                "param([Parameter(Mandatory=$true)][string]$Volume)",
                '$ErrorActionPreference = "Stop"',
                '$Root = Split-Path -Parent $MyInvocation.MyCommand.Path',
                '$Helper = Join-Path $Root "scripts/flash_rp2040_sd_bridge_uf2.py"',
                '$Artifact = Join-Path $Root "rp2040/rp2040-sd-bridge-firmware"',
                'python (Join-Path $Root "scripts/verify_package.py") $Root',
                'if ($LASTEXITCODE -ne 0) { throw "Package verification failed." }',
                "python $Helper --artifact-dir $Artifact --volume $Volume",
                'if ($LASTEXITCODE -ne 0) { throw "RP2040 UF2 volume preflight failed." }',
                '$Confirm = Read-Host "Type FLASH-RP2040 to copy the verified UF2 to the validated bootloader volume"',
                'if ($Confirm -ne "FLASH-RP2040") { throw "RP2040 flash cancelled." }',
                "python $Helper --artifact-dir $Artifact --volume $Volume --copy",
                'if ($LASTEXITCODE -ne 0) { throw "RP2040 UF2 copy failed." }',
                'Write-Host "RP2040 UF2 copied. The bootloader volume should disconnect while the RP2040 reboots."',
                "",
            )
        ),
        encoding="ascii",
    )
    rp2040_sh = package_dir / "flash_rp2040.sh"
    rp2040_sh.write_text(
        "\n".join(
            (
                "#!/usr/bin/env sh",
                "set -eu",
                'if [ "$#" -ne 1 ]; then',
                '  printf "%s\\n" "Usage: $0 /path/to/mounted-uf2-volume" >&2',
                "  exit 2",
                "fi",
                'ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"',
                'PYTHON_BIN="${D1L_PYTHON:-python3}"',
                'VOLUME="$1"',
                'HELPER="$ROOT/scripts/flash_rp2040_sd_bridge_uf2.py"',
                'ARTIFACT="$ROOT/rp2040/rp2040-sd-bridge-firmware"',
                '"$PYTHON_BIN" "$ROOT/scripts/verify_package.py" "$ROOT"',
                '"$PYTHON_BIN" "$HELPER" --artifact-dir "$ARTIFACT" --volume "$VOLUME"',
                'printf "%s" "Type FLASH-RP2040 to copy the verified UF2 to this bootloader volume: "',
                "IFS= read -r CONFIRM",
                'if [ "$CONFIRM" != "FLASH-RP2040" ]; then',
                '  printf "%s\\n" "RP2040 flash cancelled." >&2',
                "  exit 2",
                "fi",
                '"$PYTHON_BIN" "$HELPER" --artifact-dir "$ARTIFACT" --volume "$VOLUME" --copy',
                'printf "%s\\n" "RP2040 UF2 copied. The volume should disconnect while the RP2040 reboots."',
                "",
            )
        ),
        encoding="ascii",
    )
    rp2040_sh.chmod(0o755)

    guide = package_dir / "START_HERE.md"
    guide.write_text(
        f"""# DeskOS D1L 1.0 RC1 Candidate - Windows and Linux Install

This is the DeskOS D1L 1.0 RC1 candidate package:

- firmware commit: `{source_commit}`
- GitHub Actions run and attempt: see `manifest.json` and `README_RELEASE.md`
- release profile: `{CORE_RELEASE_PROFILE}`
- SD history mode: `{sd_history_mode}`

Do not mix files from another download or run these tools from inside an
archive. Extract the complete package first. Every installer verifies the complete
`SHA256SUMS.txt` inventory before it changes a device or card.

## What you need

- a D1L and its data-capable USB cable;
- a 32 GB-class or larger microSD card formatted as FAT32, not exFAT;
- Python 3.10 or newer; and
- for the ESP32 installer, `esptool` and `pyserial`.

Windows PowerShell setup:

```powershell
py -3 -m venv .deskos-venv
.\\.deskos-venv\\Scripts\\Activate.ps1
python -m pip install esptool pyserial
Set-ExecutionPolicy -Scope Process Bypass
```

Linux setup:

```sh
python3 -m venv .deskos-venv
. ./.deskos-venv/bin/activate
python -m pip install esptool pyserial
chmod +x ./*.sh
```

## 1. Prepare the microSD card

The included helper does not format, erase, or overwrite a different file.
If the card is not already FAT32, use the Windows Format dialog or a Linux disk
utility to format only the confirmed removable card as FAT32. Formatting erases
that card, so verify the device and drive letter yourself before doing it.

On Windows, replace `E:\\` with the mounted SD card:

```powershell
.\\prepare_sd_card.ps1 -Target E:\\
```

On Linux, replace the example with the mounted SD card root:

```sh
./prepare_sd_card.sh /media/$USER/DESKOS
```

The first pass is read-only. The wrapper applies the payload only after you type
`PREPARE-SD`, verifies every copied byte, and writes
`deskos/card-preparation-receipt.json` on the card. Safely eject it when done.
The included payload installs the authorized Natural Resources Canada provider
manifest at `deskos/map/offline-provider.json`; first setup verifies both the
prepared card and this manifest before it can finish.

## 2. Flash the RP2040 SD-bridge side

Power the D1L off. Put its RP2040 into physical BOOTSEL/UF2 mode using the
procedure for your hardware revision. Continue only after the computer mounts a
small UF2 bootloader volume containing `INFO_UF2.TXT`, `CURRENT.UF2`, or both.
This is not the microSD card volume.

On Windows, replace `R:\\` with the UF2 bootloader volume:

```powershell
.\\flash_rp2040.ps1 -Volume R:\\
```

On Linux, replace the example with the mounted UF2 bootloader volume:

```sh
./flash_rp2040.sh /media/$USER/RPI-RP2
```

The helper verifies the package, production bridge UF2, and UF2 volume metadata,
then requires `FLASH-RP2040`. A successful copy normally makes the
bootloader volume disconnect as the RP2040 reboots. It never formats the
microSD card.

## 3. Insert the prepared card

With the D1L powered off, insert the prepared microSD card fully. Then connect
the normal ESP32 USB/serial port.

## 4. Flash the ESP32 main GUI side

The normal project flash writes the exact Actions-built ESP-IDF images at their
declared offsets. It does not erase the flash and preserves unrelated settings,
contacts, messages, and other NVS state.

Windows: find the D1L COM port in Device Manager. It must be the USB device with
VID:PID `1A86:7523`. Enter that explicit port when prompted:

```powershell
$D1LPort = Read-Host "Enter the D1L COM port"
.\\flash_project.ps1 -Port $D1LPort
```

Linux: the D1L must be selected through its stable by-id path, never a raw
`/dev/ttyUSB` path:

```sh
ls -l /dev/serial/by-id/
export D1L_PORT="{POSIX_D1L_TARGET}"
./flash_project.sh
```

If Linux reports permission denied, add your account to the serial-device group
used by your distribution (commonly `dialout`), sign out and back in, and retry.
Do not run the flasher against a guessed port.

`flash_full_8mb.ps1` is destructive recovery, not a normal install. Use it only
when you intentionally accept loss of retained state.

## 5. Complete first setup on the screen

On every boot, wait for the full-screen readiness progress to show Display,
Identity, Radio, Storage & maps, and UI as ready. A fresh device then asks you
to:

1. enter the MeshCore name other people will see;
2. optionally enter manual decimal latitude and longitude;
3. optionally save Wi-Fi, or continue offline;
4. confirm the Canadian 910.525 MHz / 62.5 kHz / SF7 / CR5 preset;
5. verify the prepared FAT32 card and included NRCan provider; and
6. review Public, #bot, and #test before finishing.

The normal dock is Home, Channels, Contacts, Map, and Settings. DeskOS does not
ship a local identity, position, nearby-node list, or qualification data; those
are established only from your setup and real MeshCore traffic.

## Reporting a problem

Open https://github.com/n30nex/SIGUI/issues/new. Include commit
`{source_commit}`, your operating system, and the operation that failed. Do not
post private messages, Wi-Fi passwords, or credentials.
""",
        encoding="ascii",
    )

    bindings: dict[str, dict[str, Any]] = {}
    for relative in PRODUCTION_USER_INSTALL_FILES:
        path = package_dir / relative
        if (
            not path.is_file()
            or is_link_or_reparse(path)
            or path.stat().st_size <= 0
        ):
            raise ValueError(f"Production user install file is invalid: {relative}")
        bindings[relative] = {
            "size": path.stat().st_size,
            "sha256": sha256_file(path),
        }
    return {
        "schema": 1,
        "guide": "START_HERE.md",
        "windows": {
            "prepare_sd": "prepare_sd_card.ps1",
            "flash_rp2040": "flash_rp2040.ps1",
            "flash_esp32": "flash_project.ps1",
        },
        "linux": {
            "prepare_sd": "prepare_sd_card.sh",
            "flash_rp2040": "flash_rp2040.sh",
            "flash_esp32": "flash_project.sh",
        },
        "shared": {
            "prepare_sd": "scripts/prepare_deskos_sd.py",
            "flash_rp2040": "scripts/flash_rp2040_sd_bridge_uf2.py",
            "verify_package": "scripts/verify_package.py",
        },
        "rp2040_artifact": {
            "directory": "rp2040/rp2040-sd-bridge-firmware",
            "uf2": bridge_relative,
        },
        "no_sd_format": True,
        "normal_esp32_flash_erases_flash": False,
        "files": bindings,
    }


def copy_rp2040_artifacts(
    artifact_root: Path | None,
    package_dir: Path,
    *,
    include_names: tuple[str, ...] | list[str] | None = None,
    production_only: bool = False,
) -> list[dict]:
    if artifact_root is None:
        return []
    try:
        artifact_root = artifact_root.resolve(strict=True)
    except (OSError, RuntimeError):
        raise FileNotFoundError(f"Missing RP2040 artifact root {artifact_root}") from None
    if not artifact_root.is_dir():
        raise FileNotFoundError(f"Missing RP2040 artifact root {artifact_root}")

    missing = [name for name in RP2040_ARTIFACT_NAMES if not (artifact_root / name).is_dir()]
    if missing:
        raise FileNotFoundError("Missing RP2040 release artifacts: " + ", ".join(missing))
    selected = set(RP2040_ARTIFACT_NAMES if include_names is None else include_names)
    unknown = selected.difference(RP2040_ARTIFACT_NAMES)
    if unknown:
        raise ValueError(
            "Unknown RP2040 release artifacts requested: " + ", ".join(sorted(unknown))
        )

    copied = []
    rp2040_dir = package_dir / "rp2040"
    for artifact_name in RP2040_ARTIFACT_NAMES:
        source_dir = artifact_root / artifact_name
        try:
            source_resolved = source_dir.resolve(strict=True)
            source_resolved.relative_to(artifact_root)
        except (OSError, RuntimeError, ValueError):
            raise ValueError(
                f"{artifact_name} source directory must be a real direct child "
                "of the RP2040 artifact root"
            ) from None
        if (
            is_link_or_reparse(source_dir)
            or not source_dir.is_dir()
            or source_resolved != source_dir
        ):
            raise ValueError(
                f"{artifact_name} source directory must be a real direct child "
                "of the RP2040 artifact root"
            )

        source_manifests = sorted(source_dir.rglob("SHA256SUMS.txt"))
        source_manifest = source_dir / "SHA256SUMS.txt"
        if source_manifests != [source_manifest] or not verify_checksum_tree(source_dir):
            raise ValueError(
                f"{artifact_name} must contain exactly one valid root SHA256SUMS.txt"
            )
        if artifact_name not in selected:
            continue
        dest_dir = rp2040_dir / artifact_name
        if production_only:
            source_uf2_files = sorted(source_dir.glob("*.uf2"))
            if (
                artifact_name != "rp2040-sd-bridge-firmware"
                or len(source_uf2_files) != 1
                or source_uf2_files[0].name != "deskos_sd_bridge.ino.uf2"
            ):
                raise ValueError(
                    "Production RP2040 package requires exactly "
                    "deskos_sd_bridge.ino.uf2"
                )
            dest_dir.mkdir(parents=True, exist_ok=True)
            destination = dest_dir / source_uf2_files[0].name
            shutil.copy2(source_uf2_files[0], destination)
            (dest_dir / "SHA256SUMS.txt").write_text(
                f"{sha256_file(destination)}  ./{destination.name}\n",
                encoding="ascii",
            )
        else:
            shutil.copytree(source_dir, dest_dir)
        dest_manifests = sorted(dest_dir.rglob("SHA256SUMS.txt"))
        dest_manifest = dest_dir / "SHA256SUMS.txt"
        if dest_manifests != [dest_manifest] or not verify_checksum_tree(dest_dir):
            raise ValueError(f"{artifact_name} checksum verification changed after copy")
        files = []
        uf2_files = []
        for path in sorted(dest_dir.rglob("*")):
            if not path.is_file():
                continue
            rel_path = path.relative_to(package_dir).as_posix()
            entry = {
                "path": rel_path,
                "size": path.stat().st_size,
                "sha256": sha256_file(path),
            }
            files.append(entry)
            if path.suffix.lower() == ".uf2":
                uf2_files.append(rel_path)
        if not uf2_files:
            raise ValueError(f"{artifact_name} does not contain a UF2 file")
        copied.append({
            "name": artifact_name,
            "path": dest_dir.relative_to(package_dir).as_posix(),
            "uf2_files": uf2_files,
            "files": files,
        })
    return copied


def d1l_firmware_version(root: Path) -> str:
    config = root / "main" / "d1l_config.h"
    if not config.exists():
        return "unknown"
    match = re.search(r'#define\s+D1L_FIRMWARE_VERSION\s+"([^"]+)"', config.read_text(encoding="utf-8"))
    return match.group(1) if match else "unknown"


def workflow_info() -> dict:
    run_id = os.environ.get("GITHUB_RUN_ID")
    repository = os.environ.get("GITHUB_REPOSITORY")
    server_url = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    return {
        "run_id": run_id,
        "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "workflow": os.environ.get("GITHUB_WORKFLOW"),
        "sha": os.environ.get("GITHUB_SHA"),
        "ref": os.environ.get("GITHUB_REF"),
        "repository": repository,
        "run_url": f"{server_url}/{repository}/actions/runs/{run_id}" if repository and run_id else None,
    }


def app_entry(entries: list[dict]) -> dict:
    matches = [
        entry
        for entry in entries
        if entry["role"] == "app"
        and Path(entry["source"]).name == "meshcore_deskos_d1l.bin"
    ]
    if len(matches) != 1:
        raise ValueError(
            "Expected exactly one meshcore_deskos_d1l.bin app image in flash files"
        )
    return matches[0]


def copy_update_image(package_dir: Path, firmware_dir: Path, app: dict) -> dict:
    update_dir = package_dir / "update"
    update_dir.mkdir(parents=True, exist_ok=True)
    source = firmware_dir.parent / app["path"]
    dest = update_dir / "d1l-update.bin"
    shutil.copy2(source, dest)
    return {
        "path": dest.relative_to(package_dir).as_posix(),
        "size": dest.stat().st_size,
        "sha256": sha256_file(dest),
        "note": (
            "Application image for the fixed local SD update path. Serial "
            "project flashing remains the partition-migration and USB recovery path."
        ),
    }


def write_signed_update_bundle(
    root: Path,
    package_dir: Path,
    update_image: dict,
    source_commit: str,
    version: str,
    security_sequence: int,
    signing_key: Path,
) -> dict:
    """Create the exact canonical manifest consumed by update_manager.c."""
    source_commit = exact_sha(source_commit, "signed update source commit")
    if (
        isinstance(security_sequence, bool)
        or not isinstance(security_sequence, int)
        or security_sequence <= 0
        or security_sequence > 0xFFFFFFFF
    ):
        raise ValueError("D1L update security sequence is invalid")
    signing_key = signing_key.resolve(strict=True)
    if not signing_key.is_file() or signing_key.stat().st_size == 0:
        raise ValueError("D1L update signing key is missing or empty")
    image_path = package_dir / update_image["path"]
    if not image_path.is_file():
        raise FileNotFoundError("D1L update image is missing")
    partition_table = root / "partitions_d1l.csv"
    if not partition_table.is_file():
        raise FileNotFoundError("D1L partition table is missing")
    update_dir = image_path.parent
    manifest_path = update_dir / "d1l-update.manifest"
    signature_path = update_dir / "d1l-update.sig"
    public_der_path = update_dir / ".signer-public.der"
    public_pem_path = update_dir / ".signer-public.pem"

    manifest_text = (
        f"{UPDATE_MANIFEST_HEADER}\n"
        f"product={UPDATE_PRODUCT}\n"
        f"target={UPDATE_TARGET}\n"
        f"version={version}\n"
        f"source_sha={source_commit}\n"
        f"partition_table_sha256={sha256_file(partition_table)}\n"
        f"image_sha256={sha256_file(image_path)}\n"
        f"image_size={image_path.stat().st_size}\n"
        f"security_sequence={security_sequence}\n"
        f"signer_key_id={UPDATE_SIGNER_KEY_ID}\n"
    )
    manifest_path.write_text(manifest_text, encoding="ascii", newline="\n")
    try:
        subprocess.run(
            [
                "openssl",
                "pkey",
                "-in",
                str(signing_key),
                "-pubout",
                "-outform",
                "DER",
                "-out",
                str(public_der_path),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        public_der = public_der_path.read_bytes()
        if (
            len(public_der) != 44
            or public_der[-32:].hex() != UPDATE_SIGNING_PUBLIC_KEY_HEX
        ):
            raise ValueError(
                "D1L update signing secret does not match the firmware public key"
            )
        subprocess.run(
            [
                "openssl",
                "pkeyutl",
                "-sign",
                "-rawin",
                "-inkey",
                str(signing_key),
                "-in",
                str(manifest_path),
                "-out",
                str(signature_path),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        if signature_path.stat().st_size != 64:
            raise ValueError("D1L Ed25519 update signature is not 64 bytes")
        subprocess.run(
            [
                "openssl",
                "pkey",
                "-in",
                str(signing_key),
                "-pubout",
                "-out",
                str(public_pem_path),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        subprocess.run(
            [
                "openssl",
                "pkeyutl",
                "-verify",
                "-pubin",
                "-inkey",
                str(public_pem_path),
                "-rawin",
                "-in",
                str(manifest_path),
                "-sigfile",
                str(signature_path),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
    except FileNotFoundError as exc:
        raise ValueError("OpenSSL is required to sign D1L updates") from exc
    except subprocess.CalledProcessError as exc:
        raise ValueError("D1L update signing or verification failed") from exc
    finally:
        public_der_path.unlink(missing_ok=True)
        public_pem_path.unlink(missing_ok=True)

    return {
        "schema": 1,
        "signed": True,
        "product": UPDATE_PRODUCT,
        "target": UPDATE_TARGET,
        "version": version,
        "source_commit": source_commit,
        "security_sequence": security_sequence,
        "signer_key_id": UPDATE_SIGNER_KEY_ID,
        "partition_table_sha256": sha256_file(partition_table),
        "image": {
            **update_image,
            "sd_destination": "updates/d1l-update.bin",
        },
        "manifest": {
            "path": manifest_path.relative_to(package_dir).as_posix(),
            "sd_destination": "updates/d1l-update.manifest",
            "size": manifest_path.stat().st_size,
            "sha256": sha256_file(manifest_path),
        },
        "signature": {
            "path": signature_path.relative_to(package_dir).as_posix(),
            "sd_destination": "updates/d1l-update.sig",
            "size": signature_path.stat().st_size,
            "sha256": sha256_file(signature_path),
            "algorithm": "Ed25519",
        },
        "trigger_policy": "local_ui_or_usb_confirmation_only",
        "rf_trigger_allowed": False,
        "partition_table_replacement_in_firmware": False,
        "rollback_required": True,
    }


def write_full_flash_image(build_dir: Path, package_dir: Path, flasher_args: dict, size: int) -> dict:
    image_dir = package_dir / "full-flash"
    image_dir.mkdir(parents=True, exist_ok=True)
    image = image_dir / "meshcore_deskos_d1l-full-8mb.bin"
    with image.open("wb") as out:
        out.write(b"\xff" * size)
    with image.open("r+b") as out:
        for offset, rel_path in ordered_flash_files(flasher_args):
            data = (build_dir / rel_path).read_bytes()
            end = offset + len(data)
            if end > size:
                raise ValueError(f"{rel_path} at 0x{offset:x} exceeds full image size")
            out.seek(offset)
            out.write(data)
    return {
        "path": image.relative_to(package_dir).as_posix(),
        "size": image.stat().st_size,
        "sha256": sha256_file(image),
        "flash_offset": "0x0",
        "warning": "Factory/full-flash image is 0xff padded and intended for full-image recovery or factory flows, not NVS-preserving app updates.",
    }


def write_sha256sums(package_dir: Path) -> None:
    rows = []
    manifest = package_dir / "SHA256SUMS.txt"
    paths = sorted(
        package_dir.rglob("*"),
        key=lambda path: path.relative_to(package_dir).as_posix(),
    )
    for path in paths:
        if not path.is_file() or path == manifest:
            continue
        rel = path.relative_to(package_dir).as_posix()
        rows.append(f"{sha256_file(path)}  ./{rel}")
    manifest.write_text("\n".join(rows) + "\n", encoding="ascii")


def command_flash_files(entries: list[dict]) -> list[str]:
    args: list[str] = []
    for entry in sorted(entries, key=lambda item: parse_offset(item["offset"])):
        args.extend([entry["offset"], entry["path"]])
    return args


def powershell_checksum_guard_lines() -> list[str]:
    """Inline package-root checksum verification for generated flash scripts."""
    return [
        "function Assert-PackageChecksums {",
        "    param([string]$PackageRoot)",
        "    $Manifest = Join-Path $PackageRoot 'SHA256SUMS.txt'",
        '    if (!(Test-Path -LiteralPath $Manifest -PathType Leaf)) { throw "Missing package SHA256SUMS.txt." }',
        "    $Prefix = [IO.Path]::GetFullPath($PackageRoot).TrimEnd('\\', '/') + [IO.Path]::DirectorySeparatorChar",
        "    $ManifestPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)",
        "    foreach ($Line in Get-Content -LiteralPath $Manifest) {",
        "        if ($Line -notmatch '^([0-9A-Fa-f]{64})  \\./(.+)$') { throw \"Invalid SHA256SUMS.txt row: $Line\" }",
        "        $Expected = $Matches[1].ToLowerInvariant()",
        "        $Relative = $Matches[2]",
        "        if ($Relative.Contains('\\') -or $Relative.Split('/') -contains '..') { throw \"Unsafe checksum path: $Relative\" }",
        "        if (!$ManifestPaths.Add($Relative)) { throw \"Duplicate checksum path: $Relative\" }",
        "        $Target = [IO.Path]::GetFullPath((Join-Path $PackageRoot $Relative))",
        "        if (!$Target.StartsWith($Prefix, [StringComparison]::OrdinalIgnoreCase)) { throw \"Checksum path escapes package: $Relative\" }",
        "        if (!(Test-Path -LiteralPath $Target -PathType Leaf)) { throw \"Missing checksummed file: $Relative\" }",
        "        if (((Get-Item -LiteralPath $Target).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw \"Reparse-point file rejected: $Relative\" }",
        "        $Actual = (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash.ToLowerInvariant()",
        "        if ($Actual -ne $Expected) { throw \"SHA256 mismatch: $Relative\" }",
        "    }",
        "    $AllEntries = @(Get-ChildItem -LiteralPath $PackageRoot -Recurse -Force)",
        "    foreach ($Entry in $AllEntries) {",
        "        if (($Entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw \"Reparse-point package entry rejected: $($Entry.FullName)\" }",
        "    }",
        "    $PackageFiles = @($AllEntries | Where-Object { !$_.PSIsContainer -and $_.FullName -ne $Manifest })",
        "    foreach ($File in $PackageFiles) {",
        "        if (!$File.FullName.StartsWith($Prefix, [StringComparison]::OrdinalIgnoreCase)) { throw \"Package file escapes package root: $($File.FullName)\" }",
        "        $Relative = $File.FullName.Substring($Prefix.Length).Replace('\\', '/')",
        "        if (!$ManifestPaths.Contains($Relative)) { throw \"Unchecksummed package file: $Relative\" }",
        "    }",
        "    if ($ManifestPaths.Count -ne $PackageFiles.Count) { throw \"SHA256SUMS.txt is not a complete one-to-one package file inventory.\" }",
        "}",
        "Assert-PackageChecksums -PackageRoot $Root",
    ]


def copy_core_serial_target_resolver(
    root: Path,
    package_dir: Path,
) -> dict:
    root = root.resolve(strict=True)
    source_candidate = root / "scripts" / "d1l_serial_target.py"
    if is_link_or_reparse(source_candidate):
        raise ValueError("Core serial target resolver source is linked")
    source = source_candidate.resolve(strict=True)
    try:
        source.relative_to(root)
    except ValueError as exc:
        raise ValueError("Core serial target resolver escaped the source root") from exc
    if (
        not source.is_file()
        or is_link_or_reparse(source)
        or source.stat().st_size <= 0
    ):
        raise ValueError("Core serial target resolver source is invalid")
    target = package_dir / "d1l_serial_target.py"
    shutil.copyfile(source, target)
    if is_link_or_reparse(target) or target.stat().st_size <= 0:
        raise ValueError("Packaged Core serial target resolver is invalid")
    return {
        "path": target.name,
        "size": target.stat().st_size,
        "sha256": sha256_file(target),
    }


def core_flash_runner_source(
    entries: list[dict],
    *,
    flash_mode: str,
    flash_size: str,
    flash_freq: str,
) -> str:
    flash_plan = [
        [entry["offset"], entry["path"]]
        for entry in sorted(entries, key=lambda item: parse_offset(item["offset"]))
    ]
    template = r'''#!/usr/bin/env python3
"""Checksum- and identity-guarded Core 1.0 project flasher."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import stat
import subprocess
import sys
import types
from pathlib import Path, PurePosixPath
from typing import Any, Callable


POSIX_D1L_TARGET = __POSIX_TARGET__
EXPECTED_VID = __EXPECTED_VID__
EXPECTED_PID = __EXPECTED_PID__
FLASH_BAUD = __FLASH_BAUD__
FLASH_MODE = __FLASH_MODE__
FLASH_SIZE = __FLASH_SIZE__
FLASH_FREQ = __FLASH_FREQ__
FLASH_PLAN = __FLASH_PLAN__
_CHECKSUM_ROW = re.compile(r"([0-9A-Fa-f]{64})  \./(.+)\Z")
_WINDOWS_COM_PORT = re.compile(r"COM[1-9][0-9]*\Z", re.IGNORECASE)


def _is_link_or_reparse(path: Path) -> bool:
    try:
        info = path.stat(follow_symlinks=False)
    except OSError:
        return True
    attributes = getattr(info, "st_file_attributes", 0)
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    return path.is_symlink() or bool(attributes & reparse_flag)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _checked_package_file(root: Path, relative: str) -> Path:
    if (
        not relative
        or "\\" in relative
        or any(ord(char) < 0x20 or ord(char) == 0x7F for char in relative)
    ):
        raise ValueError(f"Unsafe checksum path: {relative!r}")
    posix_path = PurePosixPath(relative)
    if posix_path.is_absolute() or any(
        part in {"", ".", ".."} for part in posix_path.parts
    ):
        raise ValueError(f"Unsafe checksum path: {relative!r}")
    cursor = root
    for part in posix_path.parts:
        cursor /= part
        if _is_link_or_reparse(cursor):
            raise ValueError(f"Linked/reparse package path rejected: {relative}")
    resolved = cursor.resolve(strict=True)
    try:
        resolved.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"Checksum path escapes package: {relative}") from exc
    if not resolved.is_file():
        raise ValueError(f"Missing checksummed file: {relative}")
    return resolved


def verify_complete_package(package_root: Path) -> None:
    root = package_root.resolve(strict=True)
    manifest = root / "SHA256SUMS.txt"
    if (
        not manifest.is_file()
        or _is_link_or_reparse(manifest)
        or manifest.stat().st_size <= 0
    ):
        raise ValueError("Missing or linked package SHA256SUMS.txt")

    expected: dict[str, tuple[str, str]] = {}
    for raw_line in manifest.read_text(encoding="ascii").splitlines():
        match = _CHECKSUM_ROW.fullmatch(raw_line)
        if match is None:
            raise ValueError(f"Invalid SHA256SUMS.txt row: {raw_line}")
        digest, relative = match.groups()
        folded = relative.casefold()
        if folded in expected:
            raise ValueError(f"Duplicate checksum path: {relative}")
        target = _checked_package_file(root, relative)
        actual = _sha256(target)
        if actual != digest.lower():
            raise ValueError(f"SHA256 mismatch: {relative}")
        expected[folded] = (relative, actual)

    actual_paths: dict[str, str] = {}
    for candidate in sorted(root.rglob("*")):
        if _is_link_or_reparse(candidate):
            raise ValueError(f"Linked/reparse package entry rejected: {candidate}")
        if not candidate.is_file() or candidate == manifest:
            continue
        relative = candidate.relative_to(root).as_posix()
        folded = relative.casefold()
        if folded in actual_paths:
            raise ValueError(f"Ambiguous package path: {relative}")
        actual_paths[folded] = relative
    if not expected or set(expected) != set(actual_paths):
        missing = sorted(set(actual_paths) - set(expected))
        extra = sorted(set(expected) - set(actual_paths))
        raise ValueError(
            "SHA256SUMS.txt is not a complete one-to-one package file "
            f"inventory (unchecksummed={missing}, missing={extra})"
        )
    for folded, (relative, _digest) in expected.items():
        if actual_paths[folded] != relative:
            raise ValueError(f"Checksum path spelling mismatch: {relative}")


def _load_resolver(root: Path) -> types.ModuleType:
    path = root / "d1l_serial_target.py"
    source = path.read_text(encoding="utf-8")
    module = types.ModuleType("_packaged_d1l_serial_target")
    module.__file__ = str(path)
    exec(compile(source, str(path), "exec"), module.__dict__)
    if (
        module.POSIX_D1L_TARGET != POSIX_D1L_TARGET
        or module.EXPECTED_VID != EXPECTED_VID
        or module.EXPECTED_PID != EXPECTED_PID
    ):
        raise ValueError("Packaged D1L serial target policy is inconsistent")
    return module


def _default_port_lister() -> list[Any]:
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise ValueError("pyserial is required to identify the D1L target") from exc
    return list(list_ports.comports())


def _row_value(row: object, field: str) -> Any:
    if isinstance(row, dict):
        return row.get(field)
    return getattr(row, field, None)


def _windows_platform(platform_name: str | None) -> bool:
    value = os.name if platform_name is None else platform_name
    if not isinstance(value, str):
        raise ValueError("platform_name must be text")
    normalized = value.strip().lower()
    if normalized in {"nt", "win32", "windows"}:
        return True
    if normalized in {"posix", "linux", "darwin"}:
        return False
    raise ValueError(f"unsupported serial-target platform: {value!r}")


def _manual_windows_target(
    requested_port: str,
    *,
    port_lister: Callable[[], list[Any]],
) -> dict[str, Any]:
    if not isinstance(requested_port, str) or not requested_port.strip():
        raise ValueError("Windows manual install requires an explicit COM port")
    if any(
        ord(char) < 0x20 or ord(char) == 0x7F
        for char in requested_port
    ):
        raise ValueError("Windows COM port contains a control character")
    canonical = requested_port.strip().upper()
    if canonical.startswith("\\\\.\\"):
        canonical = canonical[4:]
    if not _WINDOWS_COM_PORT.fullmatch(canonical):
        raise ValueError(
            "Windows manual install requires an explicit canonical COM port"
        )
    try:
        rows = list(port_lister())
    except Exception as exc:
        raise ValueError("serial target enumeration failed") from exc
    matches = [
        row
        for row in rows
        if str(_row_value(row, "device") or "").strip().upper() == canonical
    ]
    if len(matches) != 1:
        raise ValueError(
            "the explicitly requested Windows COM port must be present exactly once"
        )
    row = matches[0]
    vid = _row_value(row, "vid")
    pid = _row_value(row, "pid")
    if type(vid) is not int or vid != EXPECTED_VID:
        raise ValueError(
            f"D1L VID must be 0x{EXPECTED_VID:04X}; got {vid!r}"
        )
    if type(pid) is not int or pid != EXPECTED_PID:
        raise ValueError(
            f"D1L PID must be 0x{EXPECTED_PID:04X}; got {pid!r}"
        )
    return {
        "schema": 1,
        "kind": "d1l_manual_windows_target",
        "target_kind": "windows_com_operator_supplied",
        "requested_path": canonical,
        "resolved_tty": canonical,
        "vid": vid,
        "pid": pid,
        "hwid": _row_value(row, "hwid"),
        "qualification_route": False,
        "operator_supplied": True,
    }


def run_flash(
    port: str,
    *,
    package_root: Path | None = None,
    platform_name: str | None = None,
    port_lister: Callable[[], list[Any]] | None = None,
    command_runner: Callable[..., Any] | None = None,
    resolver_module: Any | None = None,
    resolver_hooks: dict[str, Any] | None = None,
    validate_only: bool = False,
) -> dict[str, Any]:
    raw_root = (
        Path(__file__).parent
        if package_root is None
        else Path(package_root)
    )
    if _is_link_or_reparse(raw_root):
        raise ValueError("Linked/reparse package root rejected")
    root = raw_root.resolve(strict=True)
    verify_complete_package(root)
    list_ports = port_lister or _default_port_lister
    if _windows_platform(platform_name):
        if resolver_hooks:
            raise ValueError(
                "resolver hooks are not accepted for manual Windows install"
            )
        snapshot = _manual_windows_target(
            port,
            port_lister=list_ports,
        )
    else:
        resolver = resolver_module or _load_resolver(root)
        hooks = dict(resolver_hooks or {})
        snapshot = resolver.resolve_target(
            port,
            port_lister=list_ports,
            platform_name=platform_name,
            **hooks,
        )
        resolver.validate_snapshot(snapshot, snapshot["requested_path"])
    authorized_port = snapshot["requested_path"]
    if snapshot["target_kind"] == "windows_com_operator_supplied":
        if not _WINDOWS_COM_PORT.fullmatch(authorized_port):
            raise ValueError("Resolver returned an invalid Windows COM port")
    elif authorized_port != POSIX_D1L_TARGET:
        raise ValueError("Resolver returned an unauthorized D1L target")
    if validate_only:
        return {"target": snapshot, "command": None}

    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--port",
        authorized_port,
        "--baud",
        str(FLASH_BAUD),
        "--before",
        "default-reset",
        "--after",
        "hard-reset",
        "write-flash",
        "--flash-mode",
        FLASH_MODE,
        "--flash-size",
        FLASH_SIZE,
        "--flash-freq",
        FLASH_FREQ,
    ]
    for offset, relative in FLASH_PLAN:
        command.extend((offset, str(root / relative)))
    runner = command_runner or subprocess.run
    result = runner(command, cwd=root)
    returncode = getattr(result, "returncode", result if type(result) is int else None)
    if returncode != 0:
        raise RuntimeError(f"Project flash failed with exit code {returncode!r}")
    return {"target": snapshot, "command": command}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT"))
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args(argv)
    if not isinstance(args.port, str) or not args.port.strip():
        parser.error("No D1L port supplied. Set D1L_PORT or pass --port.")
    try:
        run_flash(args.port, validate_only=args.validate_only)
    except (OSError, RuntimeError, ValueError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
'''
    replacements = {
        "__POSIX_TARGET__": repr(POSIX_D1L_TARGET),
        "__EXPECTED_VID__": str(EXPECTED_VID),
        "__EXPECTED_PID__": str(EXPECTED_PID),
        "__FLASH_BAUD__": str(FLASH_BAUD),
        "__FLASH_MODE__": repr(str(flash_mode)),
        "__FLASH_SIZE__": repr(str(flash_size)),
        "__FLASH_FREQ__": repr(str(flash_freq)),
        "__FLASH_PLAN__": json.dumps(flash_plan, separators=(",", ":")),
    }
    for marker, replacement in replacements.items():
        template = template.replace(marker, replacement)
    return template


def core_install_file_bindings(package_dir: Path) -> dict[str, dict[str, Any]]:
    bindings: dict[str, dict[str, Any]] = {}
    for relative in CORE_GENERATED_INSTALL_FILES:
        path = package_dir / relative
        if (
            not path.is_file()
            or is_link_or_reparse(path)
            or path.stat().st_size <= 0
        ):
            raise ValueError(f"Core generated install file is invalid: {relative}")
        bindings[relative] = {
            "size": path.stat().st_size,
            "sha256": sha256_file(path),
        }
    return bindings


def write_flash_scripts(
    root: Path,
    package_dir: Path,
    entries: list[dict],
    flasher_args: dict,
    full_image: dict,
    release_profile: str = "full_feature",
) -> dict:
    flash_settings = flasher_args.get("flash_settings", {})
    flash_mode = flash_settings.get("flash_mode", "dio")
    flash_size = flash_settings.get("flash_size", "8MB")
    flash_freq = flash_settings.get("flash_freq", "80m")
    project_args = command_flash_files(entries)

    if release_profile == CORE_RELEASE_PROFILE:
        resolver = copy_core_serial_target_resolver(root, package_dir)
        py_project = package_dir / "flash_project.py"
        py_project.write_text(
            core_flash_runner_source(
                entries,
                flash_mode=str(flash_mode),
                flash_size=str(flash_size),
                flash_freq=str(flash_freq),
            ),
            encoding="ascii",
        )

        ps_project = package_dir / "flash_project.ps1"
        ps_project.write_text(
            "\n".join(
                (
                    "param([Parameter(Mandatory=$true)][string]$Port)",
                    '$ErrorActionPreference = "Stop"',
                    'if ([string]::IsNullOrWhiteSpace($Port)) { throw "Pass the operator-confirmed D1L COM port with -Port." }',
                    "$ValidatedPort = $Port.Trim().ToUpperInvariant()",
                    'if ($ValidatedPort -notmatch "^COM[1-9][0-9]*$") { throw "Pass one explicit canonical COM port; automatic port selection is forbidden." }',
                    'Write-Host "Installing DeskOS D1L 1.0 on the explicitly selected USB device."',
                    "$Root = Split-Path -Parent $MyInvocation.MyCommand.Path",
                    "python (Join-Path $Root 'flash_project.py') --port $ValidatedPort",
                    'if ($LASTEXITCODE -ne 0) { throw "Project flash failed with exit code $LASTEXITCODE" }',
                    "",
                )
            ),
            encoding="ascii",
        )

        sh_project = package_dir / "flash_project.sh"
        sh_project.write_text(
            "\n".join(
                (
                    "#!/usr/bin/env sh",
                    "set -eu",
                    ': "${D1L_PORT:?Set D1L_PORT to the stable D1L by-id path.}"',
                    f'if [ "$D1L_PORT" != "{POSIX_D1L_TARGET}" ]; then',
                    f'  printf "%s\\n" "Core 1.0 POSIX D1L flashing requires {POSIX_D1L_TARGET}." >&2',
                    "  exit 2",
                    "fi",
                    'ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"',
                    'PYTHON_BIN="${D1L_PYTHON:-python3}"',
                    '"$PYTHON_BIN" "$ROOT/flash_project.py" --port "$D1L_PORT"',
                    "",
                )
            ),
            encoding="ascii",
        )
        sh_project.chmod(0o755)

        ps_full = package_dir / "flash_full_8mb.ps1"
        ps_full_lines = [
            "param([Parameter(Mandatory=$true)][string]$Port)",
            '$ErrorActionPreference = "Stop"',
            'if ([string]::IsNullOrWhiteSpace($Port)) { throw "Pass the operator-confirmed D1L COM port with -Port." }',
            "$ValidatedPort = $Port.Trim().ToUpperInvariant()",
            'if ($ValidatedPort -notmatch "^COM[1-9][0-9]*$") { throw "Pass one explicit canonical COM port; automatic port selection is forbidden." }',
            'Write-Warning "DESTRUCTIVE WINDOWS RECOVERY: use only when a normal install cannot recover the device."',
            "$Root = Split-Path -Parent $MyInvocation.MyCommand.Path",
            *powershell_checksum_guard_lines(),
            "python (Join-Path $Root 'flash_project.py') --port $ValidatedPort --validate-only",
            'if ($LASTEXITCODE -ne 0) { throw "D1L identity validation failed with exit code $LASTEXITCODE" }',
            'Write-Warning "WINDOWS-ONLY RECOVERY: this writes the full 8MB image at 0x0 and can overwrite persisted settings/logs."',
            '$Confirm = Read-Host "Type FULL-FLASH-$ValidatedPort to continue"',
            'if ($Confirm -ne "FULL-FLASH-$ValidatedPort") { throw "Full flash confirmation failed." }',
            "python -m esptool --chip esp32s3 --port $ValidatedPort --baud "
            f"{FLASH_BAUD} --before default-reset --after hard-reset write-flash "
            f"--flash-mode {flash_mode} --flash-size {flash_size} --flash-freq {flash_freq} "
            f"{full_image['flash_offset']} (Join-Path $Root '{full_image['path']}')",
            'if ($LASTEXITCODE -ne 0) { throw "Full flash failed with exit code $LASTEXITCODE" }',
            "",
        ]
        ps_full.write_text("\n".join(ps_full_lines), encoding="ascii")
        return {
            "shared_project_flash": py_project.name,
            "serial_target_resolver": resolver["path"],
            "windows_project_flash": ps_project.name,
            "posix_project_flash": sh_project.name,
            "windows_full_flash": ps_full.name,
            "posix_full_flash": None,
        }

    ps_project = package_dir / "flash_project.ps1"
    ps_project_lines = [
        "param([string]$Port = $env:D1L_PORT)",
        '$ErrorActionPreference = "Stop"',
        'if ([string]::IsNullOrWhiteSpace($Port)) { throw "No D1L port supplied. Set D1L_PORT or pass -Port." }',
        "$Root = Split-Path -Parent $MyInvocation.MyCommand.Path",
        *powershell_checksum_guard_lines(),
        "$Firmware = Join-Path $Root 'firmware'",
        "python -m esptool --chip esp32s3 --port $Port --baud "
        f"{FLASH_BAUD} --before default-reset --after hard-reset write-flash "
        f"--flash-mode {flash_mode} --flash-size {flash_size} --flash-freq {flash_freq} "
        + " ".join(
            f"{project_args[i]} (Join-Path $Root '{project_args[i + 1]}')"
            for i in range(0, len(project_args), 2)
        ),
        'if ($LASTEXITCODE -ne 0) { throw "Project flash failed with exit code $LASTEXITCODE" }',
        "",
    ]
    ps_project.write_text("\n".join(ps_project_lines), encoding="ascii")

    ps_full = package_dir / "flash_full_8mb.ps1"
    ps_full_lines = [
        "param([string]$Port = $env:D1L_PORT)",
        '$ErrorActionPreference = "Stop"',
        'if ([string]::IsNullOrWhiteSpace($Port)) { throw "No D1L port supplied. Set D1L_PORT or pass -Port." }',
        "$Root = Split-Path -Parent $MyInvocation.MyCommand.Path",
        *powershell_checksum_guard_lines(),
        'Write-Warning "This writes the full 8MB image at 0x0 and can overwrite persisted settings/logs."',
        '$Confirm = Read-Host "Type FULL-FLASH-$Port to continue"',
        'if ($Confirm -ne "FULL-FLASH-$Port") { throw "Full flash confirmation failed." }',
        "python -m esptool --chip esp32s3 --port $Port --baud "
        f"{FLASH_BAUD} --before default-reset --after hard-reset write-flash "
        f"--flash-mode {flash_mode} --flash-size {flash_size} --flash-freq {flash_freq} "
        f"{full_image['flash_offset']} (Join-Path $Root '{full_image['path']}')",
        'if ($LASTEXITCODE -ne 0) { throw "Full flash failed with exit code $LASTEXITCODE" }',
        "",
    ]
    ps_full.write_text("\n".join(ps_full_lines), encoding="ascii")

    sh_project = package_dir / "flash_project.sh"
    sh_project.write_text(
        "\n".join(
            (
                "#!/usr/bin/env sh",
                "set -eu",
                ': "${D1L_PORT:?Set D1L_PORT to the D1L serial port.}"',
                'ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"',
                "python -m esptool --chip esp32s3 --port \"$D1L_PORT\" --baud "
                f"{FLASH_BAUD} --before default-reset --after hard-reset write-flash "
                f"--flash-mode {flash_mode} --flash-size {flash_size} --flash-freq {flash_freq} "
                + " ".join(
                    f'{project_args[i]} "$ROOT/{project_args[i + 1]}"'
                    for i in range(0, len(project_args), 2)
                ),
                "",
            )
        ),
        encoding="ascii",
    )
    sh_project.chmod(0o755)
    return {
        "windows_project_flash": ps_project.name,
        "windows_full_flash": ps_full.name,
        "posix_project_flash": sh_project.name,
    }


def write_supported_features(
    package_dir: Path,
    *,
    source_commit: str,
    actions_run: str,
    actions_run_attempt: str,
    sd_history_mode: str,
    supported_capabilities: list[str],
    unavailable_capabilities: list[str],
) -> dict:
    path = package_dir / "SUPPORTED_FEATURES.md"
    supported_lines = "\n".join(
        f"- `{capability}`" for capability in supported_capabilities
    )
    unavailable_lines = "\n".join(
        f"- `{capability}`" for capability in unavailable_capabilities
    )
    if sd_history_mode == "disabled":
        sd_text = (
            "SD history is disabled and deferred. NVS is authoritative. "
            "No RP2040 payload is included."
        )
    elif sd_history_mode == "supported_optional":
        sd_text = (
            "SD is the qualified primary retained-data store for the paired "
            "artifacts in this exact package."
        )
    else:
        sd_text = (
            "SD is the primary retained-data store when the paired bridge and "
            "prepared FAT32 card and authorized NRCan provider are ready. "
            "Without required media, operation is visibly live-only and retained "
            "history is not redirected into default NVS."
        )
    path.write_text(
        f"""# MeshCore DeskOS D1L 1.0 RC1 Candidate Supported Features

Release profile: `{CORE_RELEASE_PROFILE}`

Firmware commit: `{source_commit}`

GitHub Actions run: `{actions_run}`

GitHub Actions run attempt: `{actions_run_attempt}`

SD history mode: `{sd_history_mode}`

{sd_text}

## Supported

{supported_lines}

## Unavailable

{unavailable_lines}

Unavailable capabilities are intentionally hidden or rejected before side
effects. BLE companion transport, signed update/recovery, advanced QR sharing,
and the on-device USB recovery service are not part of the RC1 candidate. The
package still contains a checksum-verified host-side factory recovery image.

## Current known limitations

- SD history is `{sd_history_mode}`. In the production conditional mode, SD is
  primary and missing/unusable media produces visible live-only operation.
- Signed OTA/SD update and the on-device USB recovery service are unavailable.
  A checksum-verified host-side factory recovery image remains in the package.
- This package contains the bounded RC1 candidate product surface only.

## Support and reporting

Report defects with the firmware commit, GitHub Actions run, and run attempt
shown above at https://github.com/n30nex/SIGUI/issues/new.

Use USB installation and recovery only. Never format an SD card on the device.
""",
        encoding="ascii",
    )
    return {
        "path": path.name,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def write_core_install_recovery_guide(
    package_dir: Path,
    *,
    source_commit: str,
    actions_run: str,
    actions_run_attempt: str,
    sd_history_mode: str,
) -> dict:
    docs_dir = package_dir / "docs"
    docs_dir.mkdir(parents=True, exist_ok=True)
    path = docs_dir / "CORE_INSTALL_RECOVERY.md"
    path.write_text(
        f"""# MeshCore DeskOS D1L 1.0 RC1 Candidate Install and Recovery

Firmware commit: `{source_commit}`

GitHub Actions run: `{actions_run}`

GitHub Actions run attempt: `{actions_run_attempt}`

Release profile: `{CORE_RELEASE_PROFILE}`

SD history mode: `{sd_history_mode}`

## Before installing

1. On Linux, select the stable D1L target:
   `{POSIX_D1L_TARGET}`.
2. The device must report USB VID:PID `{EXPECTED_VID:04X}:{EXPECTED_PID:04X}`.
3. On POSIX, `/dev/ttyUSB<number>` is unstable. Never pass
   a raw tty path to a package flash command.
4. On Windows, the operator must pass the exact COM port explicitly. The
   package never chooses
   or guesses a Windows port and rejects any device without the exact VID:PID.
5. The generated entrypoint verifies every file against `SHA256SUMS.txt` and
   rejects missing, extra, linked, duplicate, or mismatched files.
6. Read `SUPPORTED_FEATURES.md`. BLE companion transport, signed OTA/recovery,
   and advanced QR sharing are unavailable in the RC1 candidate.
7. Never format an SD card on the device.
8. Prepare a FAT32 card with `scripts/prepare_deskos_sd.py`; the checked-in
   payload under `sdcard/` includes the required authorized NRCan provider
   manifest.

## Normal non-erasing USB install

The normal project flash writes the Actions-built bootloader, partition table,
OTA selection data, and application at their declared ESP-IDF offsets. It does not issue an erase
and preserves unrelated NVS regions. Both wrappers invoke the package-root
`flash_project.py`, which verifies the complete checksum inventory, resolves
the exact USB identity, and gives esptool only the stable requested target.

### Linux

```sh
export D1L_PORT="{POSIX_D1L_TARGET}"
./flash_project.sh
```

### Windows

```powershell
$PackageRoot = (Get-Location).Path
$OperatorPort = Read-Host "Enter the operator-confirmed D1L COM port"
& (Join-Path $PackageRoot "flash_project.ps1") -Port $OperatorPort
```

This Windows helper verifies the explicitly supplied port and its exact VID:PID
`{EXPECTED_VID:04X}:{EXPECTED_PID:04X}`. It never scans for a fallback port.

## Recovery

Try the normal project flash first. The full 8MB recovery image is a last
resort: it can overwrite settings, contacts, messages, and other retained
state. Core recovery is Windows-only and manual. No POSIX
recovery wrapper is shipped. `flash_full_8mb.ps1` verifies the complete
checksum inventory plus the explicitly supplied port's exact USB identity
before it asks for port-bound confirmation.

```powershell
$PackageRoot = (Get-Location).Path
$OperatorPort = Read-Host "Enter the operator-confirmed D1L COM port"
& (Join-Path $PackageRoot "flash_full_8mb.ps1") -Port $OperatorPort
```

The RC1 candidate supports USB install/recovery only; it does not support OTA
or signed SD update.
""",
        encoding="ascii",
    )
    return {
        "path": path.relative_to(package_dir).as_posix(),
        "source": "generated_core_profile",
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }

def write_release_readme(package_dir: Path, package_name: str, manifest: dict) -> None:
    readme = package_dir / "README_RELEASE.md"
    app = app_entry(manifest["flash_files"])
    if manifest.get("release_profile") == CORE_RELEASE_PROFILE:
        sd_mode = manifest["sd_history_mode"]
        supported_lines = "\n".join(
            f"- `{capability}`"
            for capability in manifest["supported_capabilities"]
        )
        unavailable_lines = "\n".join(
            f"- `{capability}`"
            for capability in manifest["unavailable_capabilities"]
        )
        checksum_rows = [
            (
                str(entry["path"]),
                str(entry["sha256"]),
            )
            for entry in manifest["flash_files"]
        ]
        full_image = manifest.get("full_flash_image")
        if isinstance(full_image, dict):
            checksum_rows.append(
                (str(full_image["path"]), str(full_image["sha256"]))
            )
        supported_features = manifest.get("supported_features")
        if isinstance(supported_features, dict):
            checksum_rows.append(
                (
                    str(supported_features["path"]),
                    str(supported_features["sha256"]),
                )
            )
        for release_doc in manifest.get("release_docs", []):
            if isinstance(release_doc, dict):
                checksum_rows.append(
                    (
                        str(release_doc["path"]),
                        str(release_doc["sha256"]),
                    )
                )
        checksum_lines = "\n".join(
            f"- `{path}`: `{digest}`" for path, digest in checksum_rows
        )
        sd_note = (
            "SD history is disabled/deferred; NVS is authoritative and no "
            "RP2040 payload is included."
            if sd_mode == "disabled"
            else (
                "SD history is qualified optional and is bound to the paired "
                "RP2040 artifacts in this package."
                if sd_mode == "supported_optional"
                else (
                    "SD is primary when the paired bridge, prepared FAT32 card, "
                    "and authorized NRCan provider are ready. Without required "
                    "media, operation is visibly live-only and retained history "
                    "is not redirected into default NVS."
                )
            )
        )
        readme.write_text(
            f"""# {PROJECT} 1.0 RC1 Candidate Package

Package: `{package_name}`

Release profile: `{manifest['release_profile']}`

Git commit: `{manifest['firmware_commit']}`

GitHub Actions run: `{manifest['actions_run']}`

GitHub Actions run attempt: `{manifest['actions_run_attempt']}`

SD history mode: `{sd_mode}`

{sd_note}

Start with `START_HERE.md` for the complete Windows or Linux candidate install:
prepare the FAT32 card, flash the RP2040 bridge, and flash the ESP32 GUI.

`SUPPORTED_FEATURES.md` is the authoritative package capability summary.

## Supported matrix

{supported_lines}

## Unavailable matrix

{unavailable_lines}

Unavailable means hidden or rejected before side effects. BLE companion
transport, signed OTA/recovery, advanced QR sharing, and the on-device USB
recovery service are not part of the RC1 candidate. The package still
contains a checksum-verified host-side factory recovery image.

## SHA-256 values

{checksum_lines}

`SHA256SUMS.txt` contains the authoritative SHA-256 value for every other file
in the package except itself. The generated Python entrypoint and both
normal-install wrappers verify that complete checksum tree before writing. The
Windows-only recovery wrapper also performs the same inventory and USB identity
checks before its warning. The SD and RP2040 installers call the same complete
package verifier.

## Normal USB Install

Normal project flashing writes the exact Actions-built bootloader, partition
table, OTA selection data, and app at their ESP-IDF offsets without erasing
unrelated NVS regions.
Linux installation uses the stable by-id target and requires VID:PID
`1A86:7523`.

```sh
export D1L_PORT="/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
./flash_project.sh
```

On POSIX, `/dev/ttyUSB<number>` is observational only and is never an
authorized open target.

The Windows wrapper requires an operator-supplied `-Port`, verifies that exact
enumerated port has VID:PID
`1A86:7523`, and never chooses or probes a default port:

```powershell
$PackageRoot = (Get-Location).Path
$OperatorPort = Read-Host "Enter the operator-confirmed D1L COM port"
& (Join-Path $PackageRoot "flash_project.ps1") -Port $OperatorPort
```

## Prepare the SD card

Use a 32GB-class or larger FAT32 card. The preparation command is read-only
unless `--apply` is supplied, never formats or deletes files, and verifies
every copied byte:

```sh
python scripts/prepare_deskos_sd.py --target <mounted-card-root>
python scripts/prepare_deskos_sd.py --target <mounted-card-root> --apply
```

The checked-in payload is under `sdcard/` and installs the required authorized
Natural Resources Canada provider manifest. Any replacement provider must
explicitly permit offline storage and background prefetch.

## Flash the RP2040 bridge

Put the RP2040 into physical BOOTSEL/UF2 mode and pass the mounted UF2 volume
explicitly. The helpers reject a volume without UF2 metadata, verify the
production `deskos_sd_bridge.ino.uf2`, and require typed confirmation before
copying:

```powershell
.\\flash_rp2040.ps1 -Volume R:\\
```

```sh
./flash_rp2040.sh /media/$USER/RPI-RP2
```

## Recovery

Read `docs/CORE_INSTALL_RECOVERY.md` before recovery. The full 8MB recovery image
requires an explicit typed confirmation and can overwrite retained state.
Recovery remains Windows-only; no POSIX full-flash wrapper is included.
USB install/recovery is the only supported update path in the RC1 candidate.

Never format an SD card on the device.

## Current known limitations

- {sd_note}
- Only the supported matrix above is available; Full Feature remains
  unreleased.
- Installation and recovery are USB-only; OTA and signed SD update are
  unavailable.

## Support and reporting

Report defects at https://github.com/n30nex/SIGUI/issues/new. Include firmware
commit `{manifest['firmware_commit']}`, Actions run
`{manifest['actions_run']}`, run attempt
`{manifest['actions_run_attempt']}`, this package name, and what went wrong.

App image: `{app['path']}`

App SHA256: `{app['sha256']}`
""",
            encoding="ascii",
        )
        return
    if manifest.get("release_profile") == FULL_FEATURE_RELEASE_PROFILE:
        supported_lines = "\n".join(
            f"- `{capability}`"
            for capability in manifest["supported_capabilities"]
        )
        signed_update = manifest["signed_update"]
        readme.write_text(
            f"""# {PROJECT} Full Feature Release Package

Package: `{package_name}`

Release profile: `{manifest['release_profile']}`

Firmware commit: `{manifest['firmware_commit']}`

GitHub Actions run: `{manifest['actions_run']}`

GitHub Actions run attempt: `{manifest['actions_run_attempt']}`

SD history mode: `{manifest['sd_history_mode']}`

This package contains the production Full Feature firmware and a verified
Ed25519-signed local update bundle. `full_feature_release_ready` remains
`false` until the exact package is flashed and its physical release gates are
recorded; packaging never manufactures that evidence.

## Supported capabilities

{supported_lines}

The BLE companion deliberately rejects factory reset, remote reboot, and
private-key import/export. Those are production security boundaries, not
missing user features. RF-triggered firmware update is also impossible.

## Contents

- `firmware/` contains the bootloader, partition table, OTA selection data,
  application image, and `flasher_args.json`.
- `update/d1l-update.bin`, `update/d1l-update.manifest`, and
  `update/d1l-update.sig` are one exact signed update set.
- `full-flash/meshcore_deskos_d1l-full-8mb.bin` is the destructive
  factory/recovery image.
- `docs/`, `notices/`, `evidence/`, the SBOM, provenance, and package metadata
  are bound by `SHA256SUMS.txt`.

Update signer: `{signed_update['signer_key_id']}`

Update security sequence: `{signed_update['security_sequence']}`

## Normal project flash

On the current Pi 5 development route, open only the stable D1L identity:

```sh
export D1L_PORT="/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
./flash_project.sh
```

The target must enumerate as VID:PID `1A86:7523`. Never substitute
`/dev/ttyUSB<number>` and never probe another Pi serial device. On Windows,
resolve the D1L by the same VID:PID immediately before use; do not reuse an
old COM assignment.

Normal project flashing writes bootloader, partition table, OTA selection
data, and app at the ESP-IDF offsets. Use the full 8MB image only for reviewed
recovery because it can overwrite settings, logs, contacts, and message state.

## Signed local update

Copy this exact set to the mounted card without renaming:

- `update/d1l-update.bin` -> `updates/d1l-update.bin`
- `update/d1l-update.manifest` -> `updates/d1l-update.manifest`
- `update/d1l-update.sig` -> `updates/d1l-update.sig`

Then use the on-device Update sheet, or the local USB console:

```text
update status
update install CONFIRM-SIGNED-UPDATE
update status
update reboot CONFIRM-REBOOT-UPDATE
```

The device verifies product, target, exact image size/hash, partition-table
hash, signer identity, signature, and monotonic security sequence before
writing the inactive OTA slot. It confirms the new image after a healthy boot
and otherwise retains ESP-IDF rollback behavior. Never format the SD card.

## Checksums and reporting

Verify `SHA256SUMS.txt` before flashing. Report defects at
https://github.com/n30nex/SIGUI/issues/new with firmware commit
`{manifest['firmware_commit']}`, Actions run `{manifest['actions_run']}`,
attempt `{manifest['actions_run_attempt']}`, and the relevant device receipt.

App image: `{app['path']}`

App SHA256: `{app['sha256']}`
""",
            encoding="ascii",
        )
        return
    if manifest.get("rp2040_artifacts"):
        rp2040_contents = "- `rp2040/` contains the Actions-built RP2040 SD bridge, legacy smoke, and official Seeed SD smoke UF2 artifacts."
    else:
        rp2040_contents = (
            "- `rp2040/` is omitted from this ESP32-only package. Re-run `d1l-ci` with "
            "`include_sd_bridge=true`, or change SD/RP2040 sources, when bridge UF2 artifacts are required."
        )
    readme.write_text(
        f"""# {PROJECT} Release Package

Package: `{package_name}`

Git commit: `{manifest['git'].get('commit') or 'unknown'}`

## Contents

- `firmware/` contains the bootloader, partition table, OTA selection data,
  app binary, and `flasher_args.json`.
{rp2040_contents}
- `update/d1l-update.bin` is the application image for development update flows.
- `full-flash/meshcore_deskos_d1l-full-8mb.bin` is an 8MB factory/recovery image padded with `0xff`.
- `docs/` contains the current RC1 user guide, feature-parity matrix,
  limitations, SD-card setup, admin allowlist, attributions, and RC1 scope.
- `notices/` contains the project license, third-party notices, source audit notes, attributions, and the verbatim orlp Ed25519 zlib license for public distribution.
- `evidence/` contains deterministic projections of current-commit MeshCore wire-envelope and signed-advert runtime receipts when supplied by CI. Their manifest entries bind the raw Actions receipt hashes; neither projection alone closes WP-04 or issue #65.
- `{manifest['build_inputs']['path']}` records the exact build-input lock copied into package metadata.
- `{manifest['capability_manifest']['path']}` deterministically projects the completion-ledger capability matrix.
- `{manifest['release_evidence_index']['path']}` indexes completion-ledger evidence and blockers. These three generated files are package metadata, not new release evidence or physical closure.
- `{manifest['sbom']['path']}` is the deterministic SPDX 2.3 SBOM bound to the exact source, submodule, and package inputs.
- `{manifest['provenance']['path']}` is deterministic unsigned SLSA v1 provenance. Its checksums are verifiable, but authenticity requires a separately signed attestation.
- `SHA256SUMS.txt` covers every file in this package except itself.

## Normal Flash

Normal project flashing writes bootloader, partition table, OTA selection
data, and app at their ESP-IDF offsets while preserving unrelated flash
regions.

```powershell
$env:D1L_PORT = "COMx"
.\\flash_project.ps1 -Port $env:D1L_PORT
```

Do not use a bot, bridge, or other non-D1L serial port for D1L flashing/testing unless the operator explicitly reassigns the hardware.

## Full Flash Image

The full 8MB image is for factory/recovery workflows. It can overwrite persisted settings, logs, contacts, and message state.

```powershell
$env:D1L_PORT = "COMx"
.\\flash_full_8mb.ps1 -Port $env:D1L_PORT
```

## Checksums

```powershell
Get-FileHash -Algorithm SHA256 firmware\\meshcore_deskos_d1l.bin
Get-Content .\\SHA256SUMS.txt
```

App image: `{app['path']}`

App SHA256: `{app['sha256']}`
""",
        encoding="ascii",
    )


def create_release_package(
    root: Path,
    build_dir: Path,
    out_dir: Path,
    package_name: str,
    full_size: int,
    rp2040_artifact_root: Path | None = None,
    meshcore_conformance_json: Path | None = None,
    meshcore_signed_advert_runtime_json: Path | None = None,
    release_profile: str = "full_feature",
    sd_history_mode: str = "conditional",
    update_signing_key: Path | None = None,
) -> dict:
    release_profile, sd_history_mode = validate_release_settings(
        release_profile, sd_history_mode
    )
    if (
        release_profile == FULL_FEATURE_RELEASE_PROFILE
        and update_signing_key is None
    ):
        raise ValueError(
            "full_feature release packaging requires an update signing key"
        )
    flasher_args = load_flasher_args(build_dir)
    package_dir = out_dir / package_name
    if package_dir.exists():
        shutil.rmtree(package_dir)
    package_dir.mkdir(parents=True, exist_ok=True)

    source_git = git_info(root)
    if source_git.get("dirty"):
        raise ValueError("Release packaging requires a clean source worktree")
    requested_commit = os.environ.get("GITHUB_SHA") or source_git.get("commit")
    source_identity = discover_source_identity(root, requested_commit)
    expected_commit = source_identity["commit"]
    repository_commit = source_git.get("commit")
    if repository_commit is not None and exact_sha(
        repository_commit, "repository source commit"
    ) != expected_commit:
        raise ValueError("release package source identity does not match repository HEAD")
    source_git["commit"] = expected_commit
    source_git["short_commit"] = expected_commit[:7]
    workflow = workflow_info()
    if release_profile in PRODUCTION_RELEASE_PROFILES:
        profile_label = (
            "Core"
            if release_profile == CORE_RELEASE_PROFILE
            else "Full Feature"
        )
        workflow_sha = workflow.get("sha")
        workflow_run_id = workflow.get("run_id")
        workflow_run_attempt = workflow.get("run_attempt")
        if exact_sha(
            workflow_sha, f"{profile_label} package Actions SHA"
        ) != expected_commit:
            raise ValueError(
                f"{profile_label} package requires GITHUB_SHA to match the "
                "exact firmware commit"
            )
        if (
            not isinstance(workflow_run_id, str)
            or re.fullmatch(r"[1-9][0-9]*", workflow_run_id) is None
        ):
            raise ValueError(
                f"{profile_label} package requires the exact numeric GitHub "
                "Actions run ID"
            )
        if (
            not isinstance(workflow_run_attempt, str)
            or re.fullmatch(r"[1-9][0-9]*", workflow_run_attempt)
            is None
        ):
            raise ValueError(
                f"{profile_label} package requires the exact positive GitHub Actions "
                "run attempt"
            )
        if workflow.get("repository") != "n30nex/SIGUI":
            raise ValueError(
                f"{profile_label} package requires the canonical n30nex/SIGUI "
                "Actions repository"
            )
    meshcore_conformance = copy_meshcore_conformance_evidence(
        meshcore_conformance_json,
        meshcore_signed_advert_runtime_json,
        root,
        package_dir,
        expected_commit,
        include_in_package=release_profile not in PRODUCTION_RELEASE_PROFILES,
    )
    meshcore_signed_advert_runtime = copy_meshcore_signed_advert_evidence(
        meshcore_signed_advert_runtime_json,
        package_dir,
        expected_commit,
        include_in_package=release_profile not in PRODUCTION_RELEASE_PROFILES,
    )

    firmware_dir = package_dir / "firmware"
    entries = copy_flash_files(build_dir, firmware_dir, flasher_args)
    app = app_entry(entries)
    update_image = (
        None
        if release_profile == CORE_RELEASE_PROFILE
        else copy_update_image(package_dir, firmware_dir, app)
    )
    signed_update = (
        write_signed_update_bundle(
            root,
            package_dir,
            update_image,
            expected_commit,
            d1l_firmware_version(root),
            source_security_sequence(source_identity),
            update_signing_key,
        )
        if update_image is not None and update_signing_key is not None
        else None
    )
    full_image = write_full_flash_image(build_dir, package_dir, flasher_args, full_size)
    debug_files = (
        []
        if release_profile in PRODUCTION_RELEASE_PROFILES
        else copy_optional_debug_files(build_dir, package_dir)
    )
    notice_files = copy_notice_files(
        root,
        package_dir,
        (
            PRODUCTION_NOTICE_FILE_SPECS
            if release_profile in PRODUCTION_RELEASE_PROFILES
            else NOTICE_FILE_SPECS
        ),
    )
    release_docs = (
        []
        if release_profile == CORE_RELEASE_PROFILE and sd_history_mode == "disabled"
        else copy_release_docs(
            root,
            package_dir,
            (
                PRODUCTION_RELEASE_DOC_SPECS
                if release_profile in PRODUCTION_RELEASE_PROFILES
                else RELEASE_DOC_SPECS
            ),
        )
    )
    if release_profile == CORE_RELEASE_PROFILE and sd_history_mode == "disabled":
        rp2040_artifacts = []
    else:
        rp2040_artifacts = copy_rp2040_artifacts(
            rp2040_artifact_root,
            package_dir,
            include_names=(
                PRODUCTION_RP2040_ARTIFACT_NAMES
                if release_profile in PRODUCTION_RELEASE_PROFILES
                else None
            ),
            production_only=release_profile in PRODUCTION_RELEASE_PROFILES,
        )
    if (
        release_profile == CORE_RELEASE_PROFILE
        and sd_history_mode != "disabled"
        and not rp2040_artifacts
    ):
        raise ValueError(
            "Core SD support requires the exact paired RP2040 artifacts"
        )
    scripts = write_flash_scripts(
        root,
        package_dir,
        entries,
        flasher_args,
        full_image,
        release_profile=release_profile,
    )
    if release_profile == CORE_RELEASE_PROFILE:
        release_docs.append(
            write_core_install_recovery_guide(
                package_dir,
                source_commit=expected_commit,
                actions_run=str(workflow["run_id"]),
                actions_run_attempt=str(workflow["run_attempt"]),
                sd_history_mode=sd_history_mode,
            )
        )

    sd_preparation = (
        copy_sd_preparation_bundle(root, package_dir)
        if release_profile == CORE_RELEASE_PROFILE
        and sd_history_mode != "disabled"
        else None
    )
    user_install = (
        write_production_user_install_bundle(
            root,
            package_dir,
            source_commit=expected_commit,
            sd_history_mode=sd_history_mode,
        )
        if release_profile == CORE_RELEASE_PROFILE
        and sd_history_mode != "disabled"
        else None
    )

    manifest = {
        "schema": (
            CORE_PACKAGE_SCHEMA
            if release_profile == CORE_RELEASE_PROFILE
            else (
                FULL_FEATURE_PACKAGE_SCHEMA
                if release_profile == FULL_FEATURE_RELEASE_PROFILE
                else 1
            )
        ),
        "project": PROJECT,
        "app_version": d1l_firmware_version(root),
        "package": package_name,
        "created_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "git": source_git,
        "workflow": workflow,
        "flash_settings": flasher_args.get("flash_settings", {}),
        "flash_files": entries,
        "rp2040_artifacts": rp2040_artifacts,
        "update_image": update_image,
        "signed_update": signed_update,
        "full_flash_image": full_image,
        "debug_files": debug_files,
        "release_docs": release_docs,
        "sd_preparation": sd_preparation,
        "user_install": user_install,
        "notice_files": notice_files,
        "scripts": scripts,
        "notes": [
            "Project flash scripts require D1L_PORT or an explicit -Port.",
            "Full 8MB flash script requires a typed confirmation because it can overwrite persisted state.",
        ],
    }
    if release_profile not in PRODUCTION_RELEASE_PROFILES:
        manifest["source_build_dir"] = str(build_dir)
        manifest["notes"].append(
            "Flash backup may be skipped only when the operator explicitly requests that for hardware validation."
        )
    if release_profile not in PRODUCTION_RELEASE_PROFILES:
        manifest["meshcore_conformance"] = meshcore_conformance
        manifest["meshcore_signed_advert_runtime"] = (
            meshcore_signed_advert_runtime
        )
    if release_profile == CORE_RELEASE_PROFILE:
        capability_truth = core_capability_truth(sd_history_mode)
        manifest.update(
            {
                "release_profile": release_profile,
                "firmware_commit": expected_commit,
                "actions_run": str(workflow["run_id"]),
                "actions_run_attempt": str(workflow["run_attempt"]),
                "supported_capabilities": capability_truth[
                    "supported_capabilities"
                ],
                "unavailable_capabilities": capability_truth[
                    "unavailable_capabilities"
                ],
                "sd_history_mode": sd_history_mode,
                "sd_history_state": capability_truth["sd_history_state"],
                "storage_authority": capability_truth["storage_authority"],
                "full_feature_release_ready": False,
                "install_recovery_guide": {
                    "schema": CORE_INSTALL_CONTRACT_SCHEMA,
                    "usb_only": True,
                    "normal_install_script": "flash_project.py",
                    "normal_install_scripts": {
                        "windows": "flash_project.ps1",
                        "posix": "flash_project.sh",
                    },
                    "normal_install_port": POSIX_D1L_TARGET,
                    "normal_install_targets": {
                        "windows": {
                            "requested_path": None,
                            "target_kind": "windows_com_operator_supplied",
                            "vid": EXPECTED_VID,
                            "pid": EXPECTED_PID,
                            "qualifying": False,
                            "explicit_operator_port_required": True,
                            "port_probe_forbidden": True,
                        },
                        "posix": {
                            "requested_path": POSIX_D1L_TARGET,
                            "target_kind": "posix_by_id",
                            "vid": EXPECTED_VID,
                            "pid": EXPECTED_PID,
                            "qualifying": True,
                        },
                    },
                    "target_policy": {
                        "stable_requested_path_only": True,
                        "resolved_tty_observational_only": True,
                        "hardware_identity_required": True,
                        "raw_posix_tty_forbidden": True,
                        "qualification_platform": "posix",
                        "qualification_target": POSIX_D1L_TARGET,
                        "windows_manual_non_qualifying": True,
                        "windows_explicit_operator_port_required": True,
                        "windows_port_probe_forbidden": True,
                    },
                    "normal_install_preserves_unrelated_nvs": True,
                    "normal_install_package_root_only": True,
                    "normal_install_checksum_verified": True,
                    "recovery_script": "flash_full_8mb.ps1",
                    "recovery_platform": "windows_only",
                    "posix_recovery_script": None,
                    "recovery_requires_typed_confirmation": True,
                    "recovery_checksum_verified": True,
                    "recovery_target_identity_verified": True,
                    "install_guide": "docs/CORE_INSTALL_RECOVERY.md",
                    "recovery_guide": "docs/CORE_INSTALL_RECOVERY.md",
                    "no_on_device_sd_format": True,
                    "generated_files": core_install_file_bindings(package_dir),
                },
            }
        )
        manifest["supported_features"] = write_supported_features(
            package_dir,
            source_commit=expected_commit,
            actions_run=str(workflow["run_id"]),
            actions_run_attempt=str(workflow["run_attempt"]),
            sd_history_mode=sd_history_mode,
            supported_capabilities=manifest["supported_capabilities"],
            unavailable_capabilities=manifest["unavailable_capabilities"],
        )
        if sd_history_mode == "disabled":
            manifest["notes"].extend(
                (
                    "SD history is deferred and disabled; NVS is authoritative.",
                    "RP2040 release payloads are intentionally omitted.",
                )
            )
    elif release_profile == FULL_FEATURE_RELEASE_PROFILE:
        capability_truth = full_feature_capability_truth(sd_history_mode)
        manifest.update(
            {
                "release_profile": release_profile,
                "firmware_commit": expected_commit,
                "actions_run": str(workflow["run_id"]),
                "actions_run_attempt": str(workflow["run_attempt"]),
                "supported_capabilities": capability_truth[
                    "supported_capabilities"
                ],
                "unavailable_capabilities": capability_truth[
                    "unavailable_capabilities"
                ],
                "sd_history_mode": sd_history_mode,
                "sd_history_state": capability_truth["sd_history_state"],
                "storage_authority": capability_truth["storage_authority"],
                "full_feature_release_ready": False,
                "signed_update_required": True,
                "security_constraints": [
                    "No RF-triggered firmware update",
                    "No BLE private-key import or export",
                    "No BLE factory reset or remote reboot",
                    "Admin mutations require authenticated capability gates and local confirmation",
                    "Room administration never synchronizes room history",
                ],
            }
        )
        if not isinstance(signed_update, dict) or not signed_update.get("signed"):
            raise ValueError(
                "Full Feature package requires a verified signed update bundle"
            )
    manifest.update(
        write_package_inventory_metadata(
            root,
            package_dir,
            expected_commit,
            release_profile=release_profile,
            sd_history_mode=sd_history_mode,
            include_release_evidence_index=(
                release_profile not in PRODUCTION_RELEASE_PROFILES
            ),
            include_internal_metadata=(
                release_profile not in PRODUCTION_RELEASE_PROFILES
            ),
        )
    )
    manifest["sbom"] = write_package_sbom(
        root,
        package_dir,
        manifest,
        source_identity=source_identity,
        expected_source_sha=expected_commit,
    )
    manifest["provenance"] = write_package_provenance(
        root,
        package_dir,
        manifest,
        source_identity=source_identity,
        expected_source_sha=expected_commit,
    )
    (package_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="ascii")
    write_release_readme(package_dir, package_name, manifest)
    if release_profile in PRODUCTION_RELEASE_PROFILES:
        validate_production_package_surface(package_dir)
    write_sha256sums(package_dir)
    manifest["package_dir"] = str(package_dir)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--out-dir", default="artifacts/release")
    parser.add_argument("--package-name", default=None)
    parser.add_argument("--full-size", type=lambda value: int(value, 0), default=DEFAULT_FLASH_SIZE)
    parser.add_argument("--rp2040-artifact-root", default=None)
    parser.add_argument("--meshcore-conformance-json", default=None)
    parser.add_argument("--meshcore-signed-advert-runtime-json", default=None)
    parser.add_argument("--release-profile", choices=sorted(RELEASE_PROFILES))
    parser.add_argument("--sd-history-mode", choices=sorted(SD_HISTORY_MODES))
    parser.add_argument("--update-signing-key", default=None)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = root / build_dir
    out_dir = Path(args.out_dir)
    if not out_dir.is_absolute():
        out_dir = root / out_dir
    release_profile, sd_history_mode = build_release_settings(
        build_dir,
        release_profile=args.release_profile,
        sd_history_mode=args.sd_history_mode,
    )
    info = git_info(root)
    package_name = args.package_name
    if not package_name:
        suffix = info.get("short_commit") or utc_stamp()
        package_name = f"d1l-release-{suffix}"

    rp2040_artifact_root = Path(args.rp2040_artifact_root) if args.rp2040_artifact_root else None
    if rp2040_artifact_root and not rp2040_artifact_root.is_absolute():
        rp2040_artifact_root = root / rp2040_artifact_root

    meshcore_conformance_json = (
        Path(args.meshcore_conformance_json) if args.meshcore_conformance_json else None
    )
    if meshcore_conformance_json and not meshcore_conformance_json.is_absolute():
        meshcore_conformance_json = root / meshcore_conformance_json

    meshcore_signed_advert_runtime_json = (
        Path(args.meshcore_signed_advert_runtime_json)
        if args.meshcore_signed_advert_runtime_json
        else None
    )
    if (
        meshcore_signed_advert_runtime_json
        and not meshcore_signed_advert_runtime_json.is_absolute()
    ):
        meshcore_signed_advert_runtime_json = (
            root / meshcore_signed_advert_runtime_json
        )

    update_signing_key = (
        Path(args.update_signing_key) if args.update_signing_key else None
    )
    if update_signing_key and not update_signing_key.is_absolute():
        update_signing_key = root / update_signing_key
    if (
        release_profile == FULL_FEATURE_RELEASE_PROFILE
        and update_signing_key is None
    ):
        parser.error(
            "full_feature release packaging requires --update-signing-key"
        )

    manifest = create_release_package(
        root,
        build_dir,
        out_dir,
        package_name,
        args.full_size,
        rp2040_artifact_root=rp2040_artifact_root,
        meshcore_conformance_json=meshcore_conformance_json,
        meshcore_signed_advert_runtime_json=meshcore_signed_advert_runtime_json,
        release_profile=release_profile,
        sd_history_mode=sd_history_mode,
        update_signing_key=update_signing_key,
    )
    print(json.dumps(manifest))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
