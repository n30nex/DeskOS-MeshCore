#!/usr/bin/env python3
"""Capture and validate the two-person Core install/recovery package review."""

from __future__ import annotations

import argparse
import json
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from artifact_metadata import git_metadata
    from verify_checksums import (
        is_link_or_reparse,
        sha256_file,
        verify_checksum_tree,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import git_metadata
    from scripts.verify_checksums import (
        is_link_or_reparse,
        sha256_file,
        verify_checksum_tree,
    )


try:
    from d1l_serial_target import (
        EXPECTED_PID,
        EXPECTED_VID,
        POSIX_D1L_TARGET,
        WINDOWS_D1L_TARGET,
        safe_slug,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.d1l_serial_target import (
        EXPECTED_PID,
        EXPECTED_VID,
        POSIX_D1L_TARGET,
        WINDOWS_D1L_TARGET,
        safe_slug,
    )

CORE_RELEASE_PROFILE = "core_1_0"
CORE_SD_HISTORY_MODE = "disabled"
CORE_PACKAGE_SCHEMA = 2
CORE_INSTALL_CONTRACT_SCHEMA = 2
CORE_TARGET_ARTIFACT_SLUG = safe_slug("core-windows-posix-usb")
INSTALL_REVIEW_CONFIRMATIONS = (
    "checksums_reviewed",
    "normal_usb_install_reviewed",
    "generated_entrypoints_reviewed",
    "windows_com12_normal_install_reviewed",
    "posix_stable_by_id_normal_install_reviewed",
    "usb_identity_policy_reviewed",
    "raw_tty_observational_only_reviewed",
    "non_erasing_normal_flash",
    "recovery_steps_reviewed",
    "windows_only_recovery_reviewed",
    "full_flash_data_loss_warning",
    "no_on_device_sd_format",
    "supported_features_reviewed",
)
REQUIRED_INSTALL_CONFIRMATIONS = INSTALL_REVIEW_CONFIRMATIONS
GENERATED_INSTALL_FILES = (
    "d1l_serial_target.py",
    "flash_project.py",
    "flash_project.ps1",
    "flash_project.sh",
    "flash_full_8mb.ps1",
    "docs/CORE_INSTALL_RECOVERY.md",
)
REVIEWED_PACKAGE_FILES = (
    "manifest.json",
    "SHA256SUMS.txt",
    "d1l_serial_target.py",
    "flash_project.py",
    "flash_project.ps1",
    "flash_project.sh",
    "flash_full_8mb.ps1",
    "README_RELEASE.md",
    "SUPPORTED_FEATURES.md",
    "docs/CORE_INSTALL_RECOVERY.md",
)
CORE_INSTALL_CONTRACT_KEYS = frozenset(
    {
        "schema",
        "usb_only",
        "normal_install_script",
        "normal_install_scripts",
        "normal_install_port",
        "normal_install_targets",
        "target_policy",
        "normal_install_preserves_unrelated_nvs",
        "normal_install_package_root_only",
        "normal_install_checksum_verified",
        "recovery_script",
        "recovery_platform",
        "posix_recovery_script",
        "recovery_requires_typed_confirmation",
        "recovery_checksum_verified",
        "recovery_target_identity_verified",
        "install_guide",
        "recovery_guide",
        "no_on_device_sd_format",
        "generated_files",
    }
)
INSTALL_REVIEW_RECEIPT_KEYS = frozenset(
    {
        "schema",
        "kind",
        "mode",
        "ok",
        "closure_eligible",
        "hardware_required",
        "physical_observed",
        "dry_run",
        "simulated",
        "fabricated",
        "edited",
        "source_only",
        "predecessor_evidence_used",
        "reused",
        "public_rf_tx",
        "formats_sd",
        "captured_at",
        "commit",
        "github_actions_run",
        "github_actions_run_attempt",
        "workflow_run_attempt",
        "release_profile",
        "sd_history_mode",
        "target_artifact_slug",
        "normal_install_targets",
        "target_policy",
        "recovery_policy",
        "generated_files",
        "operator",
        "reviewer",
        "confirmations",
        "package_root",
        "package_manifest_sha256",
        "checksum_manifest_sha256",
        "reviewed_files",
        "package_contract",
        "git",
    }
)


def expected_normal_install_targets() -> dict[str, dict[str, Any]]:
    return {
        "windows": {
            "requested_path": WINDOWS_D1L_TARGET,
            "target_kind": "windows_com",
            "vid": EXPECTED_VID,
            "pid": EXPECTED_PID,
        },
        "posix": {
            "requested_path": POSIX_D1L_TARGET,
            "target_kind": "posix_by_id",
            "vid": EXPECTED_VID,
            "pid": EXPECTED_PID,
        },
    }


def expected_target_policy() -> dict[str, bool]:
    return {
        "stable_requested_path_only": True,
        "resolved_tty_observational_only": True,
        "hardware_identity_required": True,
        "raw_posix_tty_forbidden": True,
    }


def expected_recovery_policy() -> dict[str, Any]:
    return {
        "platform": "windows_only",
        "script": "flash_full_8mb.ps1",
        "posix_script": None,
        "typed_confirmation_required": True,
        "checksum_verified": True,
        "target_identity_verified": True,
    }

def exact_sha(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    return (
        normalized
        if re.fullmatch(r"[0-9a-f]{40}", normalized)
        else None
    )


def positive_integer_text(value: object) -> str | None:
    normalized = str(value)
    return (
        normalized
        if re.fullmatch(r"[1-9][0-9]*", normalized)
        else None
    )


def _inside(path: Path, root: Path, label: str) -> Path:
    root = root.resolve(strict=True)
    resolved = path.resolve(strict=False)
    try:
        relative = resolved.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"{label} must stay inside the repository") from exc
    cursor = root
    for part in relative.parts:
        cursor /= part
        if cursor.exists() and is_link_or_reparse(cursor):
            raise ValueError(f"{label} cannot traverse a link/reparse point")
    return resolved


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def reviewed_file_rows(package_dir: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    package_dir = package_dir.resolve(strict=True)
    for relative in REVIEWED_PACKAGE_FILES:
        cursor = package_dir
        for part in Path(relative).parts:
            cursor /= part
            if cursor.exists() and is_link_or_reparse(cursor):
                raise ValueError(
                    f"reviewed package path is linked: {relative}"
                )
        path = cursor.resolve(strict=True)
        path.relative_to(package_dir)
        if (
            not path.is_file()
            or is_link_or_reparse(path)
            or path.stat().st_size <= 0
        ):
            raise ValueError(f"reviewed package file is missing or linked: {relative}")
        rows.append(
            {
                "path": relative,
                "size": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    return rows


def generated_file_bindings(
    rows: list[dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    by_path = {row["path"]: row for row in rows}
    if not all(relative in by_path for relative in GENERATED_INSTALL_FILES):
        raise ValueError("generated install file bindings are incomplete")
    return {
        relative: {
            "size": by_path[relative]["size"],
            "sha256": by_path[relative]["sha256"],
        }
        for relative in GENERATED_INSTALL_FILES
    }


def validate_package_review_inputs(
    package_dir: Path,
    *,
    root: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
) -> tuple[bool, list[str], dict[str, Any]]:
    reasons: list[str] = []
    try:
        root = root.resolve(strict=True)
        package_dir = _inside(package_dir, root, "package directory")
        package_dir = package_dir.resolve(strict=True)
    except (OSError, RuntimeError, ValueError) as exc:
        return False, ["package_root_invalid"], {"error": str(exc)}
    if not package_dir.is_dir() or is_link_or_reparse(package_dir):
        reasons.append("package_root_invalid")
    checksum_tree_ok = verify_checksum_tree(package_dir)
    if not checksum_tree_ok:
        reasons.append("checksum_tree_invalid")

    try:
        rows = reviewed_file_rows(package_dir)
        generated_bindings = generated_file_bindings(rows)
    except (OSError, RuntimeError, ValueError) as exc:
        rows = []
        generated_bindings = {}
        reasons.append("reviewed_package_files_invalid")
        reviewed_error = str(exc)
    else:
        reviewed_error = None

    manifest_path = package_dir / "manifest.json"
    manifest = read_json(manifest_path)
    workflow = manifest.get("workflow")
    install = manifest.get("install_recovery_guide")
    scripts = manifest.get("scripts")
    manifest_ok = (
        type(manifest.get("schema")) is int
        and manifest.get("schema") == CORE_PACKAGE_SCHEMA
        and manifest.get("release_profile") == CORE_RELEASE_PROFILE
        and exact_sha(manifest.get("firmware_commit")) == commit
        and str(manifest.get("actions_run")) == run_id
        and str(manifest.get("actions_run_attempt")) == run_attempt
        and manifest.get("sd_history_mode") == CORE_SD_HISTORY_MODE
        and manifest.get("full_feature_release_ready") is False
        and manifest.get("rp2040_artifacts") == []
        and manifest.get("update_image") is None
        and isinstance(manifest.get("git"), dict)
        and exact_sha(manifest["git"].get("commit")) == commit
        and manifest["git"].get("dirty") is False
        and manifest["git"].get("dirty_entries") == []
        and isinstance(workflow, dict)
        and exact_sha(workflow.get("sha")) == commit
        and str(workflow.get("run_id")) == run_id
        and str(workflow.get("run_attempt")) == run_attempt
        and workflow.get("repository") == "n30nex/SIGUI"
        and scripts == {
            "shared_project_flash": "flash_project.py",
            "serial_target_resolver": "d1l_serial_target.py",
            "windows_project_flash": "flash_project.ps1",
            "posix_project_flash": "flash_project.sh",
            "windows_full_flash": "flash_full_8mb.ps1",
            "posix_full_flash": None,
        }
        and isinstance(install, dict)
        and set(install) == CORE_INSTALL_CONTRACT_KEYS
        and type(install.get("schema")) is int
        and install.get("schema") == CORE_INSTALL_CONTRACT_SCHEMA
        and install.get("usb_only") is True
        and install.get("normal_install_script") == "flash_project.py"
        and install.get("normal_install_scripts")
        == {
            "windows": "flash_project.ps1",
            "posix": "flash_project.sh",
        }
        and install.get("normal_install_port") == WINDOWS_D1L_TARGET
        and install.get("normal_install_targets")
        == expected_normal_install_targets()
        and install.get("target_policy") == expected_target_policy()
        and install.get("normal_install_preserves_unrelated_nvs") is True
        and install.get("normal_install_package_root_only") is True
        and install.get("normal_install_checksum_verified") is True
        and install.get("recovery_script") == "flash_full_8mb.ps1"
        and install.get("recovery_platform") == "windows_only"
        and install.get("posix_recovery_script") is None
        and install.get("recovery_requires_typed_confirmation") is True
        and install.get("recovery_checksum_verified") is True
        and install.get("recovery_target_identity_verified") is True
        and install.get("install_guide") == "docs/CORE_INSTALL_RECOVERY.md"
        and install.get("recovery_guide") == "docs/CORE_INSTALL_RECOVERY.md"
        and install.get("no_on_device_sd_format") is True
        and install.get("generated_files") == generated_bindings
    )
    if not manifest_ok:
        reasons.append("package_manifest_identity_invalid")
    return not reasons, reasons, {
        "package_root": package_dir.relative_to(root).as_posix(),
        "checksum_tree_ok": checksum_tree_ok,
        "package_manifest_ok": manifest_ok,
        "package_schema": manifest.get("schema"),
        "install_contract_schema": (
            install.get("schema") if isinstance(install, dict) else None
        ),
        "package_manifest_sha256": (
            sha256_file(manifest_path) if manifest_path.is_file() else None
        ),
        "checksum_manifest_sha256": (
            sha256_file(package_dir / "SHA256SUMS.txt")
            if (package_dir / "SHA256SUMS.txt").is_file()
            else None
        ),
        "normal_install_targets": expected_normal_install_targets(),
        "target_policy": expected_target_policy(),
        "recovery_policy": expected_recovery_policy(),
        "generated_files": generated_bindings,
        "reviewed_files": rows,
        "reviewed_error": reviewed_error,
    }


def validate_install_review_receipt(
    path: Path,
    *,
    root: Path,
    package_dir: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
) -> tuple[bool, list[str], dict[str, Any]]:
    reasons: list[str] = []
    commit = exact_sha(commit) or ""
    run_id = positive_integer_text(run_id) or ""
    run_attempt = positive_integer_text(run_attempt) or ""
    try:
        path = _inside(path, root, "install review receipt")
        path = path.resolve(strict=True)
        receipt_file_ok = path.is_file() and not is_link_or_reparse(path)
    except (OSError, RuntimeError, ValueError):
        receipt_file_ok = False
    data = read_json(path) if receipt_file_ok else {}
    if not receipt_file_ok:
        reasons.append("receipt_file_invalid")
    package_ok, package_reasons, package_details = (
        validate_package_review_inputs(
            package_dir,
            root=root,
            commit=commit,
            run_id=run_id,
            run_attempt=run_attempt,
        )
    )
    reasons.extend(package_reasons)
    confirmations = data.get("confirmations")
    confirmations_ok = (
        isinstance(confirmations, dict)
        and set(confirmations) == set(INSTALL_REVIEW_CONFIRMATIONS)
        and all(confirmations[name] is True for name in INSTALL_REVIEW_CONFIRMATIONS)
    )
    if not confirmations_ok:
        reasons.append("confirmations_not_exact")
    operator = data.get("operator")
    reviewer = data.get("reviewer")
    people_ok = (
        isinstance(operator, str)
        and bool(operator.strip())
        and isinstance(reviewer, str)
        and bool(reviewer.strip())
        and operator.strip().casefold() != reviewer.strip().casefold()
    )
    if not people_ok:
        reasons.append("operator_reviewer_invalid")
    source = data.get("git")
    source_ok = (
        isinstance(source, dict)
        and exact_sha(source.get("commit")) == commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    )
    if not source_ok:
        reasons.append("source_git_invalid")
    contract_ok = (
        set(data) == INSTALL_REVIEW_RECEIPT_KEYS
        and type(data.get("schema")) is int
        and data.get("schema") == 2
        and data.get("kind") == "core_install_recovery_review"
        and data.get("mode") == "manual-package-review"
        and data.get("ok") is True
        and data.get("closure_eligible") is True
        and data.get("hardware_required") is False
        and data.get("physical_observed") is False
        and data.get("dry_run") is False
        and data.get("simulated") is False
        and data.get("fabricated") is False
        and data.get("edited") is False
        and data.get("source_only") is False
        and data.get("predecessor_evidence_used") is False
        and data.get("reused") is False
        and data.get("public_rf_tx") is False
        and data.get("formats_sd") is False
        and "port" not in data
        and exact_sha(data.get("commit")) == commit
        and data.get("github_actions_run") == run_id
        and data.get("github_actions_run_attempt") == int(run_attempt or 0)
        and data.get("workflow_run_attempt") == run_attempt
        and data.get("release_profile") == CORE_RELEASE_PROFILE
        and data.get("sd_history_mode") == CORE_SD_HISTORY_MODE
        and data.get("target_artifact_slug") == CORE_TARGET_ARTIFACT_SLUG
        and data.get("normal_install_targets")
        == package_details.get("normal_install_targets")
        and data.get("target_policy") == package_details.get("target_policy")
        and data.get("recovery_policy") == package_details.get("recovery_policy")
        and data.get("generated_files") == package_details.get("generated_files")
        and data.get("package_root") == package_details.get("package_root")
        and data.get("package_manifest_sha256")
        == package_details.get("package_manifest_sha256")
        and data.get("checksum_manifest_sha256")
        == package_details.get("checksum_manifest_sha256")
        and data.get("reviewed_files") == package_details.get("reviewed_files")
        and data.get("package_contract") == {
            "checksum_tree_ok": True,
            "package_manifest_ok": True,
            "package_schema": CORE_PACKAGE_SCHEMA,
            "install_contract_schema": CORE_INSTALL_CONTRACT_SCHEMA,
        }
    )
    if not contract_ok:
        reasons.append("receipt_contract_invalid")
    reasons = list(dict.fromkeys(reasons))
    return (
        receipt_file_ok
        and package_ok
        and confirmations_ok
        and people_ok
        and source_ok
        and contract_ok,
        reasons,
        {
            **package_details,
            "confirmations_ok": confirmations_ok,
            "operator_reviewer_distinct": people_ok,
            "source_git_ok": source_ok,
            "receipt_contract_ok": contract_ok,
            "receipt_file_ok": receipt_file_ok,
        },
    )


def capture(
    *,
    root: Path,
    package_dir: Path,
    out_path: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
    operator: str,
    reviewer: str,
    confirmations: dict[str, bool],
) -> Path:
    root = root.resolve(strict=True)
    commit = exact_sha(commit)
    run_id = positive_integer_text(run_id)
    run_attempt = positive_integer_text(run_attempt)
    if commit is None or run_id is None or run_attempt is None:
        raise ValueError("commit, run id, and run attempt must be exact positive values")
    if (
        not operator.strip()
        or not reviewer.strip()
        or operator.strip().casefold() == reviewer.strip().casefold()
    ):
        raise ValueError("operator and reviewer must be distinct non-empty people")
    if (
        set(confirmations) != set(INSTALL_REVIEW_CONFIRMATIONS)
        or any(confirmations[name] is not True for name in INSTALL_REVIEW_CONFIRMATIONS)
    ):
        raise ValueError("every exact install/recovery confirmation is required")
    source = git_metadata(root)
    if not (
        exact_sha(source.get("commit")) == commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    ):
        raise ValueError("install review must run from the exact clean candidate")
    package_ok, reasons, details = validate_package_review_inputs(
        package_dir,
        root=root,
        commit=commit,
        run_id=run_id,
        run_attempt=run_attempt,
    )
    if not package_ok:
        raise ValueError(f"package review inputs are invalid: {', '.join(reasons)}")
    out_path = _inside(out_path, root, "output path")
    if out_path.exists():
        raise ValueError(f"refusing to overwrite install review: {out_path}")
    receipt = {
        "schema": 2,
        "kind": "core_install_recovery_review",
        "mode": "manual-package-review",
        "ok": True,
        "closure_eligible": True,
        "hardware_required": False,
        "physical_observed": False,
        "dry_run": False,
        "simulated": False,
        "fabricated": False,
        "edited": False,
        "source_only": False,
        "predecessor_evidence_used": False,
        "reused": False,
        "public_rf_tx": False,
        "formats_sd": False,
        "captured_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "commit": commit,
        "github_actions_run": run_id,
        "github_actions_run_attempt": int(run_attempt),
        "workflow_run_attempt": run_attempt,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": CORE_SD_HISTORY_MODE,
        "target_artifact_slug": CORE_TARGET_ARTIFACT_SLUG,
        "normal_install_targets": details["normal_install_targets"],
        "target_policy": details["target_policy"],
        "recovery_policy": details["recovery_policy"],
        "generated_files": details["generated_files"],
        "operator": operator.strip(),
        "reviewer": reviewer.strip(),
        "confirmations": {
            name: confirmations[name] for name in INSTALL_REVIEW_CONFIRMATIONS
        },
        "package_root": details["package_root"],
        "package_manifest_sha256": details["package_manifest_sha256"],
        "checksum_manifest_sha256": details["checksum_manifest_sha256"],
        "reviewed_files": details["reviewed_files"],
        "package_contract": {
            "checksum_tree_ok": True,
            "package_manifest_ok": True,
            "package_schema": CORE_PACKAGE_SCHEMA,
            "install_contract_schema": CORE_INSTALL_CONTRACT_SCHEMA,
        },
        "git": source,
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("x", encoding="ascii", newline="\n") as handle:
        json.dump(receipt, handle, indent=2, sort_keys=True)
        handle.write("\n")
    return out_path


def default_out_path(
    root: Path, commit: str, run_id: str, run_attempt: str
) -> Path:
    return (
        root
        / "artifacts"
        / "hardware"
        / CORE_TARGET_ARTIFACT_SLUG
        / (
            f"core_install_recovery_review_{commit[:12]}_"
            f"run_{run_id}_attempt_{run_attempt}.json"
        )
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument(
        "--package-root", "--package-dir", dest="package_dir", required=True
    )
    parser.add_argument("--out")
    parser.add_argument(
        "--expected-firmware-commit",
        "--commit",
        dest="commit",
        required=True,
    )
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-attempt", required=True)
    parser.add_argument("--operator", required=True)
    parser.add_argument("--reviewer", required=True)
    parser.add_argument(
        "--confirm",
        action="append",
        choices=INSTALL_REVIEW_CONFIRMATIONS,
        default=[],
        help="repeat once for each completed review confirmation",
    )
    for name in INSTALL_REVIEW_CONFIRMATIONS:
        parser.add_argument(
            "--confirm-" + name.replace("_", "-"),
            action="store_true",
            dest=f"confirm_{name}",
        )
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    package_dir = Path(args.package_dir)
    if not package_dir.is_absolute():
        package_dir = root / package_dir
    out_path = (
        Path(args.out)
        if args.out
        else default_out_path(
            root,
            str(args.commit),
            str(args.github_run_id),
            str(args.github_run_attempt),
        )
    )
    if not out_path.is_absolute():
        out_path = root / out_path
    selected_confirmations = set(args.confirm)
    selected_confirmations.update(
        name
        for name in INSTALL_REVIEW_CONFIRMATIONS
        if getattr(args, f"confirm_{name}")
    )
    try:
        path = capture(
            root=root,
            package_dir=package_dir,
            out_path=out_path,
            commit=args.commit,
            run_id=args.github_run_id,
            run_attempt=args.github_run_attempt,
            operator=args.operator,
            reviewer=args.reviewer,
            confirmations={
                name: True for name in selected_confirmations
            },
        )
    except (OSError, RuntimeError, ValueError) as exc:
        parser.error(str(exc))
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
