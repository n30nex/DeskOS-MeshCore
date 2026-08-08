#!/usr/bin/env python3
"""Compare two exact-SHA D1L Actions release packages reproducibly.

The current package has two explicitly permitted byte-variable metadata files:

* ``manifest.json`` may differ only at ``created_at``, the GitHub Actions run
  id/attempt/URL, ``source_build_dir``, and the bound raw wire-conformance and
  signed-advert runtime receipt identities whose semantic projections are
  byte-stable in the package;
* ``README_RELEASE.md`` carries the same exact run identity for operators;
* ``SHA256SUMS.txt`` may consequently differ only in those two rows.

Every other file, resolved build input, toolchain lock, package identity, and
SHA-256 digest must match. The receipt contains no current timestamp or absolute
input path, so the same pair produces the same receipt bytes.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
from datetime import datetime, timedelta, timezone
from pathlib import Path, PurePosixPath
from typing import Any

if __package__:
    from .meshcore_conformance_d1l import validate_completed_report
    from .meshcore_signed_advert_runtime_d1l import (
        SIGNED_ADVERT_ARTIFACT_TYPE,
        SIGNED_ADVERT_EVIDENCE_PROFILE,
        validate_completed_report as validate_signed_advert_report,
    )
    from .package_release_d1l import (
        FULL_FEATURE_PACKAGE_SCHEMA,
        RELEASE_DOC_SPECS,
        git_info,
        package_inventory_payloads,
        validate_generated_package_metadata,
    )
    from .provenance_d1l import (
        CANONICAL_REPOSITORY,
        GITHUB_HOSTED_BUILDER,
        validate_against_inputs as validate_provenance_against_inputs,
        validate_profile as validate_provenance_profile,
    )
    from .sbom_d1l import (
        discover_source_identity,
        exact_sha,
        normalize_source_identity,
        sha256_file,
        validate_against_inputs as validate_sbom_against_inputs,
        validate_manifest_inputs,
        validate_spdx_profile,
    )
    from .verify_arduino_build_inputs import (
        load_metadata as load_build_inputs,
        validate_build_receipt,
    )
else:
    from meshcore_conformance_d1l import (  # type: ignore[no-redef]
        validate_completed_report,
    )
    from meshcore_signed_advert_runtime_d1l import (  # type: ignore[no-redef]
        SIGNED_ADVERT_ARTIFACT_TYPE,
        SIGNED_ADVERT_EVIDENCE_PROFILE,
        validate_completed_report as validate_signed_advert_report,
    )
    from package_release_d1l import (  # type: ignore[no-redef]
        FULL_FEATURE_PACKAGE_SCHEMA,
        RELEASE_DOC_SPECS,
        git_info,
        package_inventory_payloads,
        validate_generated_package_metadata,
    )
    from provenance_d1l import (  # type: ignore[no-redef]
        CANONICAL_REPOSITORY,
        GITHUB_HOSTED_BUILDER,
        validate_against_inputs as validate_provenance_against_inputs,
        validate_profile as validate_provenance_profile,
    )
    from sbom_d1l import (  # type: ignore[no-redef]
        discover_source_identity,
        exact_sha,
        normalize_source_identity,
        sha256_file,
        validate_against_inputs as validate_sbom_against_inputs,
        validate_manifest_inputs,
        validate_spdx_profile,
    )
    from verify_arduino_build_inputs import (  # type: ignore[no-redef]
        load_metadata as load_build_inputs,
        validate_build_receipt,
    )


SCHEMA = 1
ARTIFACT_TYPE = "d1l_release_reproducibility_comparison"
PROJECT = "MeshCore DeskOS D1L"
WORKFLOW_NAME = "d1l-ci"
REPOSITORY = CANONICAL_REPOSITORY
PROFILE_FULL_RELEASE = "full-release"
PROFILE_ESP32_ONLY = "esp32-only"
COMPARISON_PROFILES = (PROFILE_FULL_RELEASE, PROFILE_ESP32_ONLY)
RP2040_ARTIFACT_NAMES = (
    "rp2040-sd-bridge-firmware",
    "rp2040-sd-smoke-firmware",
    "rp2040-seeed-official-sd-smoke-firmware",
)
CANONICAL_CONFORMANCE_PROFILE = "d1l_meshcore_wire_conformance_package_v1"
CONFORMANCE_ACTIONS_ARTIFACT = "d1l-meshcore-wire-conformance"
CONFORMANCE_MAX_AGE_DAYS = 14
SIGNED_ADVERT_ACTIONS_ARTIFACT = CONFORMANCE_ACTIONS_ARTIFACT
PERMITTED_MANIFEST_PATHS = (
    "/actions_run",
    "/actions_run_attempt",
    "/created_at",
    "/meshcore_conformance/run_receipt/expires_at",
    "/meshcore_conformance/run_receipt/generated_at",
    "/meshcore_conformance/run_receipt/sha256",
    "/meshcore_conformance/run_receipt/size",
    "/meshcore_signed_advert_runtime/run_receipt/expires_at",
    "/meshcore_signed_advert_runtime/run_receipt/generated_at",
    "/meshcore_signed_advert_runtime/run_receipt/sha256",
    "/meshcore_signed_advert_runtime/run_receipt/size",
    "/source_build_dir",
    "/workflow/run_attempt",
    "/workflow/run_id",
    "/workflow/run_url",
)
PERMITTED_BYTE_VARIABLE_FILES = (
    "README_RELEASE.md",
    "SHA256SUMS.txt",
    "manifest.json",
)
PROVENANCE_EXCLUSIONS = {
    "README_RELEASE.md",
    "SHA256SUMS.txt",
    "manifest.json",
}
REQUIRED_TOOLCHAIN_MATERIALS = (
    ".github/d1l-build-inputs.json",
    "dependencies.lock",
    "requirements/ci-host-windows.txt",
)
REQUIRED_INPUT_MATERIALS = (
    "SIGUI source",
    ".github/workflows/d1l-ci.yml",
    "docs/archive/pre-rc1-authority-reset/COMPLETION_LEDGER.yaml",
    "partitions_d1l.csv",
    "third_party/MeshCore",
    "third_party/sensecap_indicator_esp32",
    *REQUIRED_TOOLCHAIN_MATERIALS,
)


class ComparisonError(ValueError):
    def __init__(self, code: str, detail: str):
        super().__init__(detail)
        self.code = code
        self.detail = detail


def canonical_json(value: object) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def canonical_sha256(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def load_json_object(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ComparisonError("invalid_json", f"{label} is not readable JSON") from exc
    if not isinstance(value, dict):
        raise ComparisonError("invalid_json", f"{label} must contain an object")
    return value


def safe_relative_path(value: str) -> bool:
    if not value or "\\" in value or value.startswith("/"):
        return False
    path = PurePosixPath(value)
    return not path.is_absolute() and all(
        part not in {"", ".", ".."} for part in path.parts
    )


def package_inventory(package_dir: Path) -> dict[str, str]:
    package_dir = package_dir.resolve()
    if not package_dir.is_dir():
        raise ComparisonError("missing_package", "release package directory is missing")
    inventory: dict[str, str] = {}
    try:
        candidates = sorted(package_dir.rglob("*"))
    except OSError as exc:
        raise ComparisonError(
            "unreadable_package", "release package inventory is unreadable"
        ) from exc
    for path in candidates:
        if path.is_symlink():
            raise ComparisonError(
                "unsafe_package_path", "release package contains a symbolic link"
            )
        if not path.is_file():
            continue
        relative = path.relative_to(package_dir).as_posix()
        if not safe_relative_path(relative):
            raise ComparisonError(
                "unsafe_package_path", f"unsafe package path: {relative}"
            )
        try:
            resolved = path.resolve()
            resolved.relative_to(package_dir)
            digest = sha256_file(resolved)
        except (OSError, RuntimeError, ValueError) as exc:
            raise ComparisonError(
                "unsafe_package_path",
                f"package path escapes or is unreadable: {relative}",
            ) from exc
        if relative in inventory:
            raise ComparisonError(
                "duplicate_package_path", f"duplicate package path: {relative}"
            )
        inventory[relative] = digest
    if not inventory:
        raise ComparisonError("empty_package", "release package contains no files")
    return inventory


def checksum_manifest(package_dir: Path, inventory: dict[str, str]) -> dict[str, str]:
    path = package_dir / "SHA256SUMS.txt"
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeError) as exc:
        raise ComparisonError(
            "invalid_checksum_manifest", "SHA256SUMS.txt is unreadable"
        ) from exc
    if not lines or any(not line for line in lines):
        raise ComparisonError(
            "invalid_checksum_manifest", "SHA256SUMS.txt is empty or malformed"
        )

    parsed: dict[str, str] = {}
    order: list[str] = []
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  \./(.+)", line)
        if match is None or not safe_relative_path(match.group(2)):
            raise ComparisonError(
                "invalid_checksum_manifest", "SHA256SUMS.txt has a malformed row"
            )
        digest, relative = match.groups()
        if relative in parsed:
            raise ComparisonError(
                "invalid_checksum_manifest", f"SHA256SUMS.txt repeats {relative}"
            )
        parsed[relative] = digest
        order.append(relative)
    if order != sorted(order):
        raise ComparisonError(
            "invalid_checksum_manifest", "SHA256SUMS.txt is not sorted"
        )

    expected_paths = set(inventory) - {"SHA256SUMS.txt"}
    if set(parsed) != expected_paths:
        missing = sorted(expected_paths - set(parsed))
        extra = sorted(set(parsed) - expected_paths)
        raise ComparisonError(
            "checksum_inventory_mismatch",
            f"SHA256SUMS.txt inventory mismatch; missing={missing}, extra={extra}",
        )
    mismatched = sorted(
        path for path, digest in parsed.items() if inventory[path] != digest
    )
    if mismatched:
        raise ComparisonError(
            "checksum_digest_mismatch",
            "SHA256SUMS.txt digest mismatch: " + ", ".join(mismatched),
        )
    return parsed


def parse_utc(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ComparisonError("invalid_manifest", f"manifest {field} is missing")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (ValueError, OverflowError) as exc:
        raise ComparisonError(
            "invalid_manifest", f"manifest {field} is invalid"
        ) from exc
    if parsed.tzinfo is None:
        raise ComparisonError("invalid_manifest", f"manifest {field} lacks a timezone")
    try:
        return parsed.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
    except (ValueError, OverflowError) as exc:
        raise ComparisonError(
            "invalid_manifest", f"manifest {field} is out of range"
        ) from exc


def normalize_manifest(manifest: dict[str, Any]) -> dict[str, Any]:
    normalized = copy.deepcopy(manifest)
    normalized["created_at"] = "<permitted-created-at>"
    normalized["source_build_dir"] = "<permitted-source-build-dir>"
    normalized["actions_run"] = "<permitted-run-id>"
    normalized["actions_run_attempt"] = "<permitted-run-attempt>"
    workflow = normalized["workflow"]
    workflow["run_id"] = "<permitted-run-id>"
    workflow["run_attempt"] = "<permitted-run-attempt>"
    workflow["run_url"] = "<permitted-run-url>"
    conformance = normalized.get("meshcore_conformance")
    receipt = conformance.get("run_receipt") if isinstance(conformance, dict) else None
    if isinstance(receipt, dict):
        receipt["generated_at"] = "<permitted-conformance-generated-at>"
        receipt["expires_at"] = "<permitted-conformance-expires-at>"
        receipt["sha256"] = "<permitted-conformance-run-receipt-sha256>"
        receipt["size"] = "<permitted-conformance-run-receipt-size>"
    signed_advert = normalized.get("meshcore_signed_advert_runtime")
    signed_receipt = (
        signed_advert.get("run_receipt")
        if isinstance(signed_advert, dict)
        else None
    )
    if isinstance(signed_receipt, dict):
        signed_receipt["generated_at"] = (
            "<permitted-signed-advert-generated-at>"
        )
        signed_receipt["expires_at"] = "<permitted-signed-advert-expires-at>"
        signed_receipt["sha256"] = (
            "<permitted-signed-advert-run-receipt-sha256>"
        )
        signed_receipt["size"] = "<permitted-signed-advert-run-receipt-size>"
    return normalized


def pointer_token(value: object) -> str:
    return str(value).replace("~", "~0").replace("/", "~1")


def json_difference_paths(first: object, second: object, prefix: str = "") -> list[str]:
    if type(first) is not type(second):
        return [prefix or "/"]
    if isinstance(first, dict):
        differences: list[str] = []
        for key in sorted(set(first) | set(second)):
            path = f"{prefix}/{pointer_token(key)}"
            if key not in first or key not in second:
                differences.append(path)
            else:
                differences.extend(json_difference_paths(first[key], second[key], path))
        return differences
    if isinstance(first, list):
        if len(first) != len(second):
            return [prefix or "/"]
        differences = []
        for index, (left, right) in enumerate(zip(first, second)):
            differences.extend(
                json_difference_paths(left, right, f"{prefix}/{pointer_token(index)}")
            )
        return differences
    return [] if first == second else [prefix or "/"]


def required_package_paths(source_commit: str, profile: str) -> set[str]:
    required = {
        "README_RELEASE.md",
        "SHA256SUMS.txt",
        "debug/meshcore_deskos_d1l.elf",
        "debug/meshcore_deskos_d1l.map",
        *(f"docs/{destination}" for _source, destination in RELEASE_DOC_SPECS),
        f"evidence/meshcore_conformance_{source_commit}.json",
        f"evidence/meshcore_signed_advert_runtime_{source_commit}.json",
        f"build_inputs_{source_commit}.json",
        f"capability_manifest_{source_commit}.json",
        "firmware/bootloader.bin",
        "firmware/flasher_args.json",
        "firmware/meshcore_deskos_d1l.bin",
        "firmware/partition-table.bin",
        "flash_full_8mb.ps1",
        "flash_project.ps1",
        "flash_project.sh",
        "full-flash/meshcore_deskos_d1l-full-8mb.bin",
        "manifest.json",
        "notices/ATTRIBUTIONS.md",
        "notices/LICENSE",
        "notices/SOURCE_AUDIT_AND_ATTRIBUTION.md",
        "notices/THIRD_PARTY_NOTICES.md",
        "notices/ORLP_ED25519_ZLIB_LICENSE.txt",
        f"provenance_{source_commit}.json",
        f"release_evidence_index_{source_commit}.json",
        f"sbom_{source_commit}.spdx.json",
        "update/d1l-update.bin",
        "update/d1l-update.manifest",
        "update/d1l-update.sig",
    }
    if profile == PROFILE_FULL_RELEASE:
        required.update(
            f"rp2040/{name}/SHA256SUMS.txt" for name in RP2040_ARTIFACT_NAMES
        )
        required.add(
            "rp2040/rp2040-sd-bridge-firmware/build-inputs.json"
        )
    return required


def validate_rp2040_profile(
    root: Path,
    manifest: dict[str, Any],
    package_dir: Path,
    source_commit: str,
    profile: str,
) -> None:
    if profile not in COMPARISON_PROFILES:
        raise ComparisonError(
            "invalid_comparison_profile", f"unsupported comparison profile: {profile}"
        )
    groups = manifest.get("rp2040_artifacts")
    groups = groups if isinstance(groups, list) else []
    if profile == PROFILE_ESP32_ONLY:
        if groups:
            raise ComparisonError(
                "package_profile_mismatch",
                "esp32-only comparison received RP2040 release artifacts",
            )
        return

    names = [group.get("name") for group in groups if isinstance(group, dict)]
    if names != list(RP2040_ARTIFACT_NAMES) or len(groups) != len(
        RP2040_ARTIFACT_NAMES
    ):
        raise ComparisonError(
            "missing_rp2040_artifacts",
            "full-release profile requires all three RP2040 artifact roots",
        )

    for group, name in zip(groups, RP2040_ARTIFACT_NAMES, strict=True):
        expected_root = f"rp2040/{name}"
        if group.get("path") != expected_root:
            raise ComparisonError(
                "invalid_rp2040_artifact",
                f"RP2040 artifact root is invalid for {name}",
            )
        group_dir = (package_dir / expected_root).resolve()
        try:
            group_dir.relative_to(package_dir.resolve())
        except ValueError as exc:
            raise ComparisonError(
                "invalid_rp2040_artifact", f"RP2040 artifact escapes package: {name}"
            ) from exc
        inventory = package_inventory(group_dir)
        checksum_manifest(group_dir, inventory)
        expected_files = {f"{expected_root}/{path}" for path in inventory}
        raw_files = group.get("files")
        declared_files = {
            item.get("path")
            for item in raw_files
            if isinstance(item, dict) and isinstance(item.get("path"), str)
        } if isinstance(raw_files, list) else set()
        if declared_files != expected_files or len(declared_files) != len(
            raw_files if isinstance(raw_files, list) else []
        ):
            raise ComparisonError(
                "invalid_rp2040_artifact",
                f"RP2040 manifest inventory is incomplete for {name}",
            )
        uf2_files = group.get("uf2_files")
        if (
            not isinstance(uf2_files, list)
            or not uf2_files
            or any(
                not isinstance(path, str)
                or path not in expected_files
                or not path.lower().endswith(".uf2")
                for path in uf2_files
            )
        ):
            raise ComparisonError(
                "invalid_rp2040_artifact", f"RP2040 UF2 inventory is invalid for {name}"
            )

    receipt_path = (
        package_dir
        / "rp2040"
        / "rp2040-sd-bridge-firmware"
        / "build-inputs.json"
    )
    receipt = load_json_object(receipt_path, "RP2040 build-input receipt")
    metadata_path = root / ".github" / "d1l-build-inputs.json"
    try:
        metadata = load_build_inputs(metadata_path)
        validate_build_receipt(
            receipt,
            metadata,
            source_commit,
            sha256_file(metadata_path),
            root,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise ComparisonError(
            "invalid_rp2040_build_inputs",
            f"full-release RP2040 build-input receipt is incomplete or stale: {exc}",
        ) from exc


def validate_manifest_shape(
    root: Path,
    manifest: dict[str, Any],
    package_dir: Path,
    source_commit: str,
    profile: str,
) -> dict[str, str]:
    if (
        manifest.get("schema") != FULL_FEATURE_PACKAGE_SCHEMA
        or manifest.get("project") != PROJECT
    ):
        raise ComparisonError(
            "invalid_manifest", "manifest schema or project identity is invalid"
        )
    if manifest.get("package") != f"d1l-release-{source_commit}":
        raise ComparisonError(
            "package_identity_mismatch", "manifest package name is not exact-SHA bound"
        )
    parse_utc(manifest.get("created_at"), "created_at")
    build_dir = manifest.get("source_build_dir")
    if not isinstance(build_dir, str) or not build_dir.strip():
        raise ComparisonError(
            "invalid_manifest", "manifest source_build_dir is missing"
        )

    git = manifest.get("git")
    if (
        not isinstance(git, dict)
        or exact_sha(git.get("commit"), "manifest git commit") != source_commit
    ):
        raise ComparisonError(
            "source_sha_mismatch", "manifest Git commit does not match source SHA"
        )
    if git.get("dirty") is not False or git.get("dirty_entries") != []:
        raise ComparisonError(
            "dirty_source", "manifest does not prove a clean source worktree"
        )

    workflow = manifest.get("workflow")
    if not isinstance(workflow, dict):
        raise ComparisonError(
            "invalid_actions_identity", "manifest workflow identity is missing"
        )
    if exact_sha(workflow.get("sha"), "manifest workflow SHA") != source_commit:
        raise ComparisonError(
            "source_sha_mismatch", "workflow SHA does not match source SHA"
        )
    if (
        workflow.get("repository") != REPOSITORY
        or workflow.get("workflow") != WORKFLOW_NAME
    ):
        raise ComparisonError(
            "invalid_actions_identity", "workflow repository or name is invalid"
        )
    ref = workflow.get("ref")
    if not isinstance(ref, str) or not ref.strip():
        raise ComparisonError("invalid_actions_identity", "workflow ref is missing")
    run_id = str(workflow.get("run_id") or "")
    run_attempt = str(workflow.get("run_attempt") or "")
    if not run_id.isdigit() or not run_attempt.isdigit() or int(run_attempt) < 1:
        raise ComparisonError(
            "invalid_actions_identity", "workflow run identity is invalid"
        )
    expected_url = f"https://github.com/{REPOSITORY}/actions/runs/{run_id}"
    if workflow.get("run_url") != expected_url:
        raise ComparisonError("invalid_actions_identity", "workflow run URL is invalid")

    try:
        validate_manifest_inputs(manifest, package_dir, source_commit)
    except (FileNotFoundError, OSError, ValueError) as exc:
        raise ComparisonError("invalid_manifest_binding", str(exc)) from exc
    try:
        expected_inventory_payloads = package_inventory_payloads(root, source_commit)
        for contract_name, expected_payload in expected_inventory_payloads.items():
            payload = validate_generated_package_metadata(
                package_dir,
                manifest.get(contract_name),
                source_commit,
                contract_name,
            )
            if payload != expected_payload:
                raise ValueError(
                    f"Packaged {contract_name} does not match its exact source inputs"
                )
    except (FileNotFoundError, OSError, ValueError) as exc:
        raise ComparisonError("invalid_package_metadata", str(exc)) from exc
    validate_rp2040_profile(root, manifest, package_dir, source_commit, profile)

    conformance = manifest.get("meshcore_conformance")
    expected_conformance = f"evidence/meshcore_conformance_{source_commit}.json"
    if (
        not isinstance(conformance, dict)
        or conformance.get("path") != expected_conformance
    ):
        raise ComparisonError(
            "missing_required_payload", "exact-SHA conformance evidence is missing"
        )
    run_receipt = conformance.get("run_receipt")
    if (
        conformance.get("evidence_profile") != CANONICAL_CONFORMANCE_PROFILE
        or not isinstance(run_receipt, dict)
        or run_receipt.get("artifact") != CONFORMANCE_ACTIONS_ARTIFACT
        or run_receipt.get("path") != f"meshcore_conformance_{source_commit}.json"
        or isinstance(run_receipt.get("size"), bool)
        or not isinstance(run_receipt.get("size"), int)
        or run_receipt["size"] <= 0
        or re.fullmatch(r"[0-9a-f]{64}", str(run_receipt.get("sha256") or ""))
        is None
    ):
        raise ComparisonError(
            "invalid_conformance_binding",
            "canonical conformance evidence has no valid raw Actions receipt binding",
        )
    try:
        canonical_report = load_json_object(
            package_dir / expected_conformance,
            "canonical MeshCore conformance evidence",
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise ComparisonError(
            "invalid_conformance_semantics",
            f"canonical conformance evidence is incomplete: {exc}",
        ) from exc
    generated_at = parse_utc(run_receipt.get("generated_at"), "conformance generated_at")
    expires_at = parse_utc(run_receipt.get("expires_at"), "conformance expires_at")
    generated_time = datetime.fromisoformat(generated_at.replace("Z", "+00:00"))
    expires_time = datetime.fromisoformat(expires_at.replace("Z", "+00:00"))
    if expires_time != generated_time + timedelta(days=CONFORMANCE_MAX_AGE_DAYS):
        raise ComparisonError(
            "invalid_conformance_binding",
            "raw conformance receipt expiry does not match the release policy",
        )

    signed_advert = manifest.get("meshcore_signed_advert_runtime")
    expected_signed_advert = (
        f"evidence/meshcore_signed_advert_runtime_{source_commit}.json"
    )
    if (
        not isinstance(signed_advert, dict)
        or signed_advert.get("path") != expected_signed_advert
    ):
        raise ComparisonError(
            "missing_required_payload",
            "exact-SHA signed-advert runtime evidence is missing",
        )
    signed_receipt = signed_advert.get("run_receipt")
    if (
        signed_advert.get("artifact_type") != SIGNED_ADVERT_ARTIFACT_TYPE
        or signed_advert.get("source_commit") != source_commit
        or signed_advert.get("evidence_profile")
        != SIGNED_ADVERT_EVIDENCE_PROFILE
        or signed_advert.get("max_age_days") != CONFORMANCE_MAX_AGE_DAYS
        or signed_advert.get("wp04_closure_eligible") is not False
        or signed_advert.get("closure_ready") is not False
        or signed_advert.get("full_ubsan_clean") is not True
        or signed_advert.get("passed") is not True
        or signed_advert.get("execution_complete") is not True
        or not isinstance(signed_receipt, dict)
        or signed_receipt.get("artifact") != SIGNED_ADVERT_ACTIONS_ARTIFACT
        or signed_receipt.get("path")
        != f"meshcore_signed_advert_runtime_{source_commit}.json"
        or isinstance(signed_receipt.get("size"), bool)
        or not isinstance(signed_receipt.get("size"), int)
        or signed_receipt["size"] <= 0
        or re.fullmatch(
            r"[0-9a-f]{64}", str(signed_receipt.get("sha256") or "")
        )
        is None
    ):
        raise ComparisonError(
            "invalid_signed_advert_binding",
            "canonical signed-advert evidence has no valid raw Actions receipt binding",
        )
    try:
        canonical_signed_advert = load_json_object(
            package_dir / expected_signed_advert,
            "canonical MeshCore signed-advert runtime evidence",
        )
        validate_signed_advert_report(
            canonical_signed_advert,
            source_commit,
            require_generated_at=False,
            require_commands=False,
        )
        if (
            canonical_signed_advert.get("evidence_profile")
            != SIGNED_ADVERT_EVIDENCE_PROFILE
            or signed_advert.get("coverage_boundary")
            != canonical_signed_advert.get("coverage_boundary")
        ):
            raise ValueError("signed-advert evidence profile or boundary drifted")
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise ComparisonError(
            "invalid_signed_advert_semantics",
            f"canonical signed-advert evidence is incomplete: {exc}",
        ) from exc
    try:
        validate_completed_report(
            canonical_report,
            source_commit,
            build_inputs_path=root / ".github" / "d1l-build-inputs.json",
            require_generated_at=False,
            signed_advert_runtime_receipt=canonical_signed_advert,
            signed_advert_runtime_receipt_is_canonical=True,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise ComparisonError(
            "invalid_conformance_semantics",
            f"canonical conformance evidence is incomplete: {exc}",
        ) from exc
    signed_generated_at = parse_utc(
        signed_receipt.get("generated_at"), "signed-advert generated_at"
    )
    signed_expires_at = parse_utc(
        signed_receipt.get("expires_at"), "signed-advert expires_at"
    )
    signed_generated_time = datetime.fromisoformat(
        signed_generated_at.replace("Z", "+00:00")
    )
    signed_expires_time = datetime.fromisoformat(
        signed_expires_at.replace("Z", "+00:00")
    )
    if signed_expires_time != signed_generated_time + timedelta(
        days=CONFORMANCE_MAX_AGE_DAYS
    ):
        raise ComparisonError(
            "invalid_signed_advert_binding",
            "raw signed-advert receipt expiry does not match the release policy",
        )

    sbom = manifest.get("sbom")
    provenance = manifest.get("provenance")
    expected_sbom = f"sbom_{source_commit}.spdx.json"
    expected_provenance = f"provenance_{source_commit}.json"
    if (
        not isinstance(sbom, dict)
        or sbom.get("path") != expected_sbom
        or sbom.get("source_commit") != source_commit
        or sbom.get("valid") is not True
    ):
        raise ComparisonError(
            "invalid_sbom_binding", "manifest SBOM binding is invalid"
        )
    if (
        not isinstance(provenance, dict)
        or provenance.get("path") != expected_provenance
        or provenance.get("source_commit") != source_commit
        or provenance.get("valid") is not True
        or provenance.get("authenticated") is not False
    ):
        raise ComparisonError(
            "invalid_provenance_binding", "manifest provenance binding is invalid"
        )
    return {"run_id": run_id, "run_attempt": run_attempt, "ref": ref}


def descriptor_maps(
    statement: dict[str, Any],
) -> tuple[dict[str, dict], dict[str, str]]:
    definition = statement["predicate"]["buildDefinition"]
    materials = definition["resolvedDependencies"]
    by_name: dict[str, dict] = {}
    for item in materials:
        name = item.get("name")
        if not isinstance(name, str) or name in by_name:
            raise ComparisonError(
                "invalid_provenance",
                "provenance material names are missing or duplicated",
            )
        by_name[name] = item
    missing = sorted(set(REQUIRED_INPUT_MATERIALS) - set(by_name))
    if missing:
        raise ComparisonError(
            "missing_build_inputs",
            "provenance omits required inputs: " + ", ".join(missing),
        )
    subjects: dict[str, str] = {}
    for item in statement["subject"]:
        name = item["name"]
        if name in subjects:
            raise ComparisonError(
                "invalid_provenance", "provenance subject names are duplicated"
            )
        subjects[name] = item["digest"]["sha256"]
    return by_name, subjects


def validate_toolchain_lock(root: Path, materials: dict[str, dict]) -> str:
    path = root / ".github" / "d1l-build-inputs.json"
    build_inputs = load_json_object(path, "D1L build-input lock")
    if (
        build_inputs.get("schema") != 1
        or build_inputs.get("kind") != "d1l_build_inputs"
    ):
        raise ComparisonError(
            "invalid_toolchain_lock", "D1L build-input lock identity is invalid"
        )
    material = materials[".github/d1l-build-inputs.json"]
    if material.get("digest") != {"sha256": sha256_file(path)}:
        raise ComparisonError(
            "toolchain_material_mismatch", "build-input lock digest is stale"
        )

    esp_idf = build_inputs.get("esp_idf")
    container = esp_idf.get("container") if isinstance(esp_idf, dict) else None
    reference = container.get("reference") if isinstance(container, dict) else None
    digest = container.get("index_digest") if isinstance(container, dict) else None
    if (
        not isinstance(reference, str)
        or not isinstance(digest, str)
        or re.fullmatch(r"sha256:[0-9a-f]{64}", digest) is None
        or not reference.endswith("@" + digest)
    ):
        raise ComparisonError(
            "invalid_toolchain_lock", "ESP-IDF container digest is not immutable"
        )

    host_python = build_inputs.get("host_python")
    requirements = (
        host_python.get("requirements") if isinstance(host_python, dict) else None
    )
    requirements_path = (
        requirements.get("path") if isinstance(requirements, dict) else None
    )
    requirements_sha = (
        requirements.get("sha256") if isinstance(requirements, dict) else None
    )
    if requirements_path != "requirements/ci-host-windows.txt":
        raise ComparisonError(
            "invalid_toolchain_lock", "host requirements path is invalid"
        )
    locked_requirements = root / requirements_path
    if (
        not locked_requirements.is_file()
        or requirements_sha != sha256_file(locked_requirements)
        or materials[requirements_path].get("digest") != {"sha256": requirements_sha}
    ):
        raise ComparisonError(
            "toolchain_material_mismatch", "host requirements lock is stale"
        )
    return canonical_sha256(
        {name: materials[name] for name in sorted(REQUIRED_TOOLCHAIN_MATERIALS)}
    )


def load_snapshot(
    root: Path,
    package_dir: Path,
    source_commit: str,
    source_identity: dict,
    label: str,
    profile: str,
) -> dict[str, Any]:
    package_dir = package_dir.resolve()
    inventory = package_inventory(package_dir)
    checksums = checksum_manifest(package_dir, inventory)
    missing_paths = sorted(
        required_package_paths(source_commit, profile) - set(inventory)
    )
    if missing_paths:
        raise ComparisonError(
            "missing_required_payload",
            "required package files are missing: " + ", ".join(missing_paths),
        )

    manifest = load_json_object(package_dir / "manifest.json", "release manifest")
    actions = validate_manifest_shape(root, manifest, package_dir, source_commit, profile)
    normalized_manifest = normalize_manifest(manifest)

    sbom_path = package_dir / f"sbom_{source_commit}.spdx.json"
    sbom = load_json_object(sbom_path, "release SBOM")
    sbom_errors = validate_spdx_profile(sbom, source_commit)
    sbom_errors.extend(
        validate_sbom_against_inputs(
            sbom,
            root,
            source_identity,
            package_dir=package_dir,
            package_manifest=manifest,
        )
    )
    if sbom_errors:
        raise ComparisonError("invalid_sbom", "; ".join(sorted(set(sbom_errors))))
    if manifest["sbom"].get("sha256") != inventory[sbom_path.name]:
        raise ComparisonError("invalid_sbom_binding", "SBOM manifest checksum is stale")

    provenance_path = package_dir / f"provenance_{source_commit}.json"
    statement = load_json_object(provenance_path, "release provenance")
    provenance_errors = validate_provenance_profile(statement, source_commit)
    provenance_errors.extend(
        validate_provenance_against_inputs(
            statement, root, package_dir, manifest, source_identity
        )
    )
    if provenance_errors:
        raise ComparisonError(
            "invalid_provenance", "; ".join(sorted(set(provenance_errors)))
        )
    if manifest["provenance"].get("sha256") != inventory[provenance_path.name]:
        raise ComparisonError(
            "invalid_provenance_binding", "provenance manifest checksum is stale"
        )
    builder_id = statement["predicate"]["runDetails"]["builder"]["id"]
    if builder_id != GITHUB_HOSTED_BUILDER:
        raise ComparisonError(
            "invalid_actions_identity",
            "provenance was not generated by the Actions package context",
        )

    materials, subjects = descriptor_maps(statement)
    source = materials["SIGUI source"]
    if source.get("digest") != {"gitCommit": source_commit}:
        raise ComparisonError(
            "source_sha_mismatch", "provenance source material SHA is invalid"
        )
    expected_subjects = {
        path: digest
        for path, digest in inventory.items()
        if path not in PROVENANCE_EXCLUSIONS
        and path != f"provenance_{source_commit}.json"
    }
    if subjects != expected_subjects:
        raise ComparisonError(
            "provenance_subject_mismatch",
            "provenance subjects do not cover exact payload inventory",
        )
    toolchain_fingerprint = validate_toolchain_lock(root, materials)

    stable_inventory = {
        path: digest
        for path, digest in inventory.items()
        if path not in PERMITTED_BYTE_VARIABLE_FILES
    }
    comparable_checksums = {
        path: digest
        for path, digest in checksums.items()
        if path not in {"manifest.json", "README_RELEASE.md"}
    }
    return {
        "label": label,
        "manifest": manifest,
        "normalized_manifest": normalized_manifest,
        "inventory": inventory,
        "stable_inventory": stable_inventory,
        "comparable_checksums": comparable_checksums,
        "materials": materials,
        "actions": actions,
        "toolchain_fingerprint": toolchain_fingerprint,
        "receipt": {
            "label": label,
            "valid": True,
            "package": manifest["package"],
            "app_version": manifest.get("app_version"),
            "workflow_run_id": actions["run_id"],
            "workflow_run_attempt": actions["run_attempt"],
            "workflow_ref": actions["ref"],
            "file_count": len(inventory),
            "manifest_sha256": inventory["manifest.json"],
            "normalized_manifest_sha256": canonical_sha256(normalized_manifest),
            "checksum_manifest_sha256": inventory["SHA256SUMS.txt"],
            "stable_payload_fingerprint": canonical_sha256(stable_inventory),
            "input_fingerprint": canonical_sha256(materials),
            "toolchain_fingerprint": toolchain_fingerprint,
        },
    }


def failure(code: str, detail: str, package: str | None = None) -> dict[str, str]:
    result = {"code": code, "detail": detail}
    if package is not None:
        result["package"] = package
    return result


def base_receipt(source_commit: str, profile: str) -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "artifact_type": ARTIFACT_TYPE,
        "source_commit": source_commit,
        "comparison_profile": profile,
        "status": "fail",
        "reproducible": False,
        "packages": [],
        "permitted_nonreproducible_metadata": {
            "files": {
                "manifest.json": list(PERMITTED_MANIFEST_PATHS),
                "README_RELEASE.md": ["exact Actions run identity only"],
                "SHA256SUMS.txt": [
                    "manifest.json and README_RELEASE.md checksum rows only"
                ],
            },
            "observed_manifest_differences": [],
        },
        "comparison": {
            "distinct_actions_runs": False,
            "required_inventory_match": False,
            "payload_sha256_match": False,
            "checksum_payload_map_match": False,
            "normalized_manifest_match": False,
            "resolved_inputs_match": False,
            "toolchain_match": False,
            "package_identity_match": False,
        },
        "inventory": {
            "file_count": None,
            "stable_payload_count": None,
            "stable_payload_fingerprint": None,
        },
        "failures": [],
    }


def compare_with_identity(
    root: Path,
    first_package: Path,
    second_package: Path,
    source_commit: str,
    source_identity: dict,
    profile: str,
) -> dict[str, Any]:
    receipt = base_receipt(source_commit, profile)
    snapshots = []
    for label, package_dir in (("first", first_package), ("second", second_package)):
        try:
            snapshot = load_snapshot(
                root, package_dir, source_commit, source_identity, label, profile
            )
        except ComparisonError as exc:
            receipt["packages"].append(
                {"label": label, "valid": False, "error_code": exc.code}
            )
            receipt["failures"].append(failure(exc.code, exc.detail, label))
        except (FileNotFoundError, OSError, TypeError, ValueError) as exc:
            receipt["packages"].append(
                {"label": label, "valid": False, "error_code": "invalid_package"}
            )
            receipt["failures"].append(failure("invalid_package", str(exc), label))
        else:
            snapshots.append(snapshot)
            receipt["packages"].append(snapshot["receipt"])
    if len(snapshots) != 2:
        receipt["failures"].sort(
            key=lambda item: (item.get("package", ""), item["code"], item["detail"])
        )
        return receipt

    first, second = snapshots
    checks = receipt["comparison"]
    checks["distinct_actions_runs"] = (
        first["actions"]["run_id"] != second["actions"]["run_id"]
    )
    if not checks["distinct_actions_runs"]:
        receipt["failures"].append(
            failure(
                "same_actions_run",
                "packages must come from distinct GitHub Actions runs",
            )
        )

    first_paths = set(first["inventory"])
    second_paths = set(second["inventory"])
    checks["required_inventory_match"] = first_paths == second_paths
    if not checks["required_inventory_match"]:
        receipt["failures"].append(
            failure(
                "package_inventory_mismatch",
                f"first_only={sorted(first_paths - second_paths)}, "
                f"second_only={sorted(second_paths - first_paths)}",
            )
        )

    payload_mismatches = sorted(
        path
        for path in set(first["stable_inventory"]) | set(second["stable_inventory"])
        if first["stable_inventory"].get(path) != second["stable_inventory"].get(path)
    )
    checks["payload_sha256_match"] = not payload_mismatches
    if payload_mismatches:
        receipt["failures"].append(
            failure("payload_sha256_mismatch", ", ".join(payload_mismatches))
        )

    checks["checksum_payload_map_match"] = (
        first["comparable_checksums"] == second["comparable_checksums"]
    )
    if not checks["checksum_payload_map_match"]:
        receipt["failures"].append(
            failure(
                "checksum_payload_map_mismatch",
                "SHA256SUMS payload rows differ beyond the permitted manifest row",
            )
        )

    observed = json_difference_paths(first["manifest"], second["manifest"])
    receipt["permitted_nonreproducible_metadata"]["observed_manifest_differences"] = (
        observed
    )
    unpermitted = sorted(set(observed) - set(PERMITTED_MANIFEST_PATHS))
    checks["normalized_manifest_match"] = (
        first["normalized_manifest"] == second["normalized_manifest"]
        and not unpermitted
    )
    if not checks["normalized_manifest_match"]:
        receipt["failures"].append(
            failure(
                "manifest_semantic_mismatch",
                "unpermitted manifest differences: " + ", ".join(unpermitted),
            )
        )

    checks["resolved_inputs_match"] = first["materials"] == second["materials"]
    if not checks["resolved_inputs_match"]:
        receipt["failures"].append(
            failure(
                "resolved_input_mismatch", "provenance resolved dependencies differ"
            )
        )
    checks["toolchain_match"] = (
        first["toolchain_fingerprint"] == second["toolchain_fingerprint"]
    )
    if not checks["toolchain_match"]:
        receipt["failures"].append(
            failure("toolchain_mismatch", "toolchain material fingerprints differ")
        )

    first_identity = (
        first["manifest"].get("project"),
        first["manifest"].get("package"),
        first["manifest"].get("app_version"),
    )
    second_identity = (
        second["manifest"].get("project"),
        second["manifest"].get("package"),
        second["manifest"].get("app_version"),
    )
    checks["package_identity_match"] = first_identity == second_identity
    if not checks["package_identity_match"]:
        receipt["failures"].append(
            failure(
                "package_identity_mismatch", "project/package/version identity differs"
            )
        )

    if checks["required_inventory_match"]:
        receipt["inventory"] = {
            "file_count": len(first["inventory"]),
            "stable_payload_count": len(first["stable_inventory"]),
            "stable_payload_fingerprint": canonical_sha256(first["stable_inventory"]),
        }
    receipt["failures"].sort(
        key=lambda item: (item.get("package", ""), item["code"], item["detail"])
    )
    receipt["reproducible"] = not receipt["failures"] and all(checks.values())
    receipt["status"] = "pass" if receipt["reproducible"] else "fail"
    return receipt


def compare_release_packages(
    root: Path,
    first_package: Path,
    second_package: Path,
    expected_source_sha: str,
    *,
    source_identity: dict | None = None,
    enforce_clean_source: bool = True,
    profile: str = PROFILE_FULL_RELEASE,
) -> dict[str, Any]:
    source_commit = exact_sha(expected_source_sha, "expected source SHA")
    if profile not in COMPARISON_PROFILES:
        receipt = base_receipt(source_commit, profile)
        receipt["failures"] = [
            failure(
                "invalid_comparison_profile",
                f"unsupported comparison profile: {profile}",
            )
        ]
        return receipt
    root = root.resolve()
    if source_identity is None:
        info = git_info(root)
        if info.get("dirty"):
            receipt = base_receipt(source_commit, profile)
            receipt["failures"] = [
                failure("dirty_source", "comparison requires a clean source worktree")
            ]
            return receipt
        repository_commit = info.get("commit")
        if (
            repository_commit is None
            or exact_sha(repository_commit, "repository source commit") != source_commit
        ):
            receipt = base_receipt(source_commit, profile)
            receipt["failures"] = [
                failure(
                    "source_sha_mismatch",
                    "repository HEAD does not match expected source SHA",
                )
            ]
            return receipt
        try:
            identity = discover_source_identity(root, source_commit)
        except (FileNotFoundError, OSError, ValueError) as exc:
            receipt = base_receipt(source_commit, profile)
            receipt["failures"] = [failure("invalid_source_identity", str(exc))]
            return receipt
    else:
        identity = normalize_source_identity(source_identity)
        if identity["commit"] != source_commit:
            receipt = base_receipt(source_commit, profile)
            receipt["failures"] = [
                failure(
                    "source_sha_mismatch",
                    "supplied source identity does not match expected SHA",
                )
            ]
            return receipt
        if enforce_clean_source:
            receipt = base_receipt(source_commit, profile)
            receipt["failures"] = [
                failure(
                    "invalid_source_identity",
                    "injected source identity is allowed only for isolated host tests",
                )
            ]
            return receipt
    return compare_with_identity(
        root, first_package, second_package, source_commit, identity, profile
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=Path(__file__).resolve().parents[1])
    parser.add_argument("--first-package", required=True)
    parser.add_argument("--second-package", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument(
        "--profile", choices=COMPARISON_PROFILES, default=PROFILE_FULL_RELEASE
    )
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    first = Path(args.first_package).resolve()
    second = Path(args.second_package).resolve()
    out = Path(args.out).resolve()
    for package in (first, second):
        try:
            out.relative_to(package)
        except ValueError:
            continue
        raise SystemExit("--out must be outside both compared package directories")

    try:
        receipt = compare_release_packages(
            root, first, second, args.source_sha, profile=args.profile
        )
    except (OSError, TypeError, ValueError) as exc:
        try:
            source_commit = exact_sha(args.source_sha, "expected source SHA")
        except ValueError:
            source_commit = str(args.source_sha)
        receipt = base_receipt(source_commit, args.profile)
        receipt["failures"] = [failure("comparison_error", str(exc))]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(canonical_json(receipt), encoding="ascii")
    print(json.dumps({"ok": receipt["reproducible"], "path": str(out)}))
    return 0 if receipt["reproducible"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
