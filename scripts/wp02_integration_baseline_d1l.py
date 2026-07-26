#!/usr/bin/env python3
"""Build and validate the exact-merged-main WP-02 integration baseline.

The artifact is intentionally source-bound.  A passing report requires a clean
local ``main`` ref at the selected 40-hex commit, every required PR head as an
ancestor, a checksum-complete downloaded Actions run for that same commit, and
five role-specific hardware receipts tied to the verified firmware payloads.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable

try:
    from verify_checksums import (
        sha256_file,
        verify_sha256_file,
        verify_sha256_manifest,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.verify_checksums import (
        sha256_file,
        verify_sha256_file,
        verify_sha256_manifest,
    )


SCHEMA = 1
REPOSITORY = "n30nex/SIGUI"
MAIN_REF = "refs/heads/main"
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
RUN_ID_RE = re.compile(r"[1-9][0-9]*")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
COM_PORT_RE = re.compile(r"COM[1-9][0-9]*")
FORBIDDEN_PORTS = {"COM" + str(number) for number in (8, 11, 29)}
TRUSTED_PR_LAYERS = {
    62: "7a6ff86493042cc5617ef88c4765312cea46150d",
    64: "15f2a9ed99541fa059445ff3d1b06a40b4c42bee",
    80: "ab3e7d82b6f3c4b38fd80d833e155aa941dee045",
}
REQUIRED_PR_LAYERS = tuple(TRUSTED_PR_LAYERS)
RP2040_GROUPS = (
    "rp2040-sd-bridge-firmware",
    "rp2040-sd-smoke-firmware",
    "rp2040-seeed-official-sd-smoke-firmware",
)
ROLE_CHECKS = {
    "board": ("board_ready", "ui_ready"),
    "ui": ("ui_opened", "ui_responsive"),
    "sd": ("sd_ready", "storage_clean"),
    "reboot": ("reboot_completed", "post_reboot_ready"),
    "map_open": ("map_opened", "map_rendered"),
}
CANONICAL_PHYSICAL_FIELDS = {
    "physical_observed": True,
    "dry_run": False,
    "simulated": False,
    "simulation": False,
    "source_inspection": False,
}


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _canonical_sha256(value: object) -> str:
    return hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _exact_commit(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    return normalized if COMMIT_RE.fullmatch(normalized) else None


def _exact_sha256(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    return normalized if SHA256_RE.fullmatch(normalized) else None


def _port(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().upper()
    return normalized if COM_PORT_RE.fullmatch(normalized) else None


def _safe_ports(d1l_port: object, rp2040_port: object) -> bool:
    d1l = _port(d1l_port)
    rp2040 = _port(rp2040_port)
    return bool(
        d1l
        and rp2040
        and d1l != rp2040
        and d1l not in FORBIDDEN_PORTS
        and rp2040 not in FORBIDDEN_PORTS
    )


def _within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except (OSError, RuntimeError, ValueError):
        return False
    return True


def _resolve_inside(root: Path, value: str | Path) -> Path:
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = root / candidate
    resolved = candidate.resolve()
    if not _within(resolved, root):
        raise ValueError("path_outside_repository")
    return resolved


def _relative(path: Path, root: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def _git(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )


def _git_value(root: Path, *args: str) -> str | None:
    result = _git(root, *args)
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def _repository_evidence(
    root: Path,
    main_sha: str,
    layers: dict[int, str],
) -> tuple[dict[str, Any], list[str]]:
    failures: list[str] = []
    top = _git_value(root, "rev-parse", "--show-toplevel")
    repository_root = False
    if top:
        try:
            repository_root = Path(top).resolve() == root.resolve()
        except (OSError, RuntimeError):
            repository_root = False
    if not repository_root:
        failures.append("repository:not_git_root")

    head = _git_value(root, "rev-parse", "HEAD")
    main_ref_sha = _git_value(root, "rev-parse", "--verify", f"{MAIN_REF}^{{commit}}")
    status_result = _git(root, "status", "--porcelain", "--untracked-files=all")
    clean = status_result.returncode == 0 and status_result.stdout.strip() == ""
    if head != main_sha:
        failures.append("repository:head_not_exact_main_sha")
    if main_ref_sha != main_sha:
        failures.append("repository:main_ref_not_exact_main_sha")
    if not clean:
        failures.append("repository:dirty")

    layer_rows: list[dict[str, Any]] = []
    if set(layers) != set(REQUIRED_PR_LAYERS):
        failures.append("repository:required_pr_layers_incomplete")
    for pr in REQUIRED_PR_LAYERS:
        trusted_commit = TRUSTED_PR_LAYERS[pr]
        commit = _exact_commit(layers.get(pr))
        trusted = commit == trusted_commit
        exists = bool(
            commit
            and _git(root, "cat-file", "-e", f"{commit}^{{commit}}").returncode == 0
        )
        ancestor = bool(
            exists
            and _git(
                root, "merge-base", "--is-ancestor", commit or "", main_sha
            ).returncode
            == 0
        )
        if not commit:
            failures.append(f"repository:pr_{pr}_commit_invalid")
        elif not trusted:
            failures.append(f"repository:pr_{pr}_commit_untrusted")
        elif not exists:
            failures.append(f"repository:pr_{pr}_commit_missing")
        elif not ancestor:
            failures.append(f"repository:pr_{pr}_not_landed")
        layer_rows.append(
            {
                "pr": pr,
                "commit": commit or str(layers.get(pr) or "").lower(),
                "trusted_commit": trusted_commit,
                "trusted": trusted,
                "exists": exists,
                "ancestor_of_main": ancestor,
            }
        )

    checks = {
        "repository_root": repository_root,
        "head_is_main_sha": head == main_sha,
        "main_ref_is_main_sha": main_ref_sha == main_sha,
        "working_tree_clean": clean,
        "required_layers_present": set(layers) == set(REQUIRED_PR_LAYERS),
        "required_layers_exact": all(row["trusted"] for row in layer_rows),
        "required_layers_unique": len({row["commit"] for row in layer_rows})
        == len(REQUIRED_PR_LAYERS),
        "required_layers_landed": all(
            row["trusted"] and row["ancestor_of_main"] for row in layer_rows
        ),
    }
    return (
        {
            "repository": REPOSITORY,
            "main_ref": MAIN_REF,
            "main_sha": main_sha,
            "head_sha": head,
            "main_ref_sha": main_ref_sha,
            "clean": clean,
            "layers": layer_rows,
            "checks": checks,
            "valid": all(checks.values()),
        },
        failures,
    )


def _unique_file(root: Path, pattern: str) -> Path | None:
    matches = sorted(path for path in root.glob(pattern) if path.is_file())
    return matches[0] if len(matches) == 1 else None


def _checksum_entry_count(path: Path) -> int:
    try:
        return len(
            [
                line
                for line in path.read_text(encoding="ascii").splitlines()
                if line.strip()
            ]
        )
    except (OSError, UnicodeError):
        return 0


def _tree_receipt(root: Path) -> tuple[list[dict[str, Any]], str]:
    rows = [
        {
            "path": path.relative_to(root).as_posix(),
            "size": path.stat().st_size,
            "sha256": sha256_file(path).lower(),
        }
        for path in sorted(
            candidate for candidate in root.rglob("*") if candidate.is_file()
        )
    ]
    return rows, _canonical_sha256(rows)


def _actions_evidence(
    root: Path,
    run_dir: Path,
    main_sha: str,
    run_id: str,
) -> tuple[dict[str, Any], list[str]]:
    failures: list[str] = []
    base: dict[str, Any] = {
        "repository": REPOSITORY,
        "run_id": run_id,
        "run_dir": _relative(run_dir, root) if _within(run_dir, root) else None,
        "valid": False,
    }
    if not _within(run_dir, root):
        return base, ["actions:run_dir_outside_repository"]
    if not run_dir.is_dir():
        return base, ["actions:run_dir_missing"]

    marker_path = _unique_file(
        run_dir,
        "d1l-host-artifacts/host-checks/d1l_host_checks_success_*.json",
    )
    package_manifest_path = _unique_file(
        run_dir,
        f"d1l-release-package/d1l-release-{main_sha}/manifest.json",
    )
    marker = _read_json(marker_path) if marker_path else {}
    package_manifest = (
        _read_json(package_manifest_path) if package_manifest_path else {}
    )

    marker_valid = bool(
        marker_path
        and marker.get("schema") == 1
        and marker.get("artifact_type") == "d1l_host_checks_success"
        and marker.get("status") == "pass"
        and marker.get("passed") is True
        and marker.get("all_prior_steps_completed") is True
        and marker.get("job") == "host-checks"
        and marker.get("repository_commit") == main_sha
        and str(marker.get("workflow_run_id")) == run_id
        and str(marker.get("workflow_run_attempt")).isdigit()
    )
    if not marker_valid:
        failures.append("actions:host_success_marker_invalid")

    git_data = package_manifest.get("git")
    workflow = package_manifest.get("workflow")
    manifest_valid = bool(
        package_manifest_path
        and package_manifest.get("schema") == 1
        and package_manifest.get("package") == f"d1l-release-{main_sha}"
        and isinstance(git_data, dict)
        and git_data.get("commit") == main_sha
        and git_data.get("dirty") is False
        and isinstance(workflow, dict)
        and str(workflow.get("run_id")) == run_id
        and workflow.get("sha") == main_sha
        and workflow.get("repository") == REPOSITORY
        and str(workflow.get("run_attempt")).isdigit()
        and marker_valid
        and str(marker.get("workflow_run_attempt")) == str(workflow.get("run_attempt"))
    )
    if not manifest_valid:
        failures.append("actions:release_manifest_identity_invalid")

    package_root = package_manifest_path.parent if package_manifest_path else None
    required_manifests = {
        "d1l-firmware-artifacts/SHA256SUMS.txt",
        *(f"{name}/SHA256SUMS.txt" for name in RP2040_GROUPS),
    }
    if package_root:
        package_rel = _relative(package_root, run_dir)
        required_manifests.add(f"{package_rel}/SHA256SUMS.txt")
        required_manifests.update(
            f"{package_rel}/rp2040/{name}/SHA256SUMS.txt" for name in RP2040_GROUPS
        )

    manifest_paths = sorted(run_dir.rglob("SHA256SUMS.txt"))
    sha_paths = sorted(run_dir.rglob("*.sha256"))
    manifest_rows: list[dict[str, Any]] = []
    checksum_files_valid = True
    for path in manifest_paths:
        valid = verify_sha256_manifest(path)
        checksum_files_valid = checksum_files_valid and valid
        manifest_rows.append(
            {
                "path": _relative(path, run_dir),
                "sha256": sha256_file(path).lower(),
                "entry_count": _checksum_entry_count(path),
                "valid": valid,
            }
        )
    scalar_rows: list[dict[str, Any]] = []
    for path in sha_paths:
        valid = verify_sha256_file(path)
        checksum_files_valid = checksum_files_valid and valid
        scalar_rows.append(
            {
                "path": _relative(path, run_dir),
                "sha256": sha256_file(path).lower(),
                "valid": valid,
            }
        )
    present_manifests = {row["path"] for row in manifest_rows}
    required_manifests_present = required_manifests <= present_manifests
    if not required_manifests_present:
        failures.append("actions:required_checksum_manifests_missing")
    if not checksum_files_valid:
        failures.append("actions:checksum_verification_failed")

    flash_rows = package_manifest.get("flash_files")
    flash_rows = flash_rows if isinstance(flash_rows, list) else []
    flash_by_role = {
        str(row.get("role")): row for row in flash_rows if isinstance(row, dict)
    }
    expected_sources = {
        "bootloader": "bootloader/bootloader.bin",
        "partition-table": "partition_table/partition-table.bin",
        "ota-data": "ota_data_initial.bin",
        "app": "meshcore_deskos_d1l.bin",
    }
    firmware_rows: list[dict[str, Any]] = []
    firmware_valid = (
        set(flash_by_role) == set(expected_sources) and package_root is not None
    )
    for role, source_rel in expected_sources.items():
        row = flash_by_role.get(role, {})
        source = run_dir / "d1l-firmware-artifacts" / "build" / source_rel
        packaged = package_root / str(row.get("path") or "") if package_root else Path()
        expected_sha = _exact_sha256(row.get("sha256"))
        valid = bool(
            row
            and row.get("source") == source_rel
            and expected_sha
            and source.is_file()
            and packaged.is_file()
            and _within(packaged, package_root or run_dir)
            and source.stat().st_size == row.get("size")
            and packaged.stat().st_size == row.get("size")
            and sha256_file(source).lower() == expected_sha
            and sha256_file(packaged).lower() == expected_sha
        )
        firmware_valid = firmware_valid and valid
        firmware_rows.append(
            {
                "role": role,
                "source": f"d1l-firmware-artifacts/build/{source_rel}",
                "sha256": expected_sha,
                "valid": valid,
            }
        )
    if not firmware_valid:
        failures.append("actions:firmware_payload_identity_invalid")

    group_rows = package_manifest.get("rp2040_artifacts")
    group_rows = group_rows if isinstance(group_rows, list) else []
    group_by_name = {
        str(row.get("name")): row for row in group_rows if isinstance(row, dict)
    }
    rp2040_rows: list[dict[str, Any]] = []
    rp2040_valid = set(RP2040_GROUPS) <= set(group_by_name) and package_root is not None
    bridge_sha256: str | None = None
    for name in RP2040_GROUPS:
        group = group_by_name.get(name, {})
        files = group.get("files") if isinstance(group, dict) else None
        files = files if isinstance(files, list) else []
        direct_root = run_dir / name
        verified_files: list[dict[str, Any]] = []
        group_valid = bool(group and direct_root.is_dir() and files)
        for row in sorted(
            (item for item in files if isinstance(item, dict)),
            key=lambda item: str(item.get("path")),
        ):
            packaged = (
                package_root / str(row.get("path") or "") if package_root else Path()
            )
            package_prefix = f"rp2040/{name}/"
            raw_path = str(row.get("path") or "")
            local_name = (
                raw_path[len(package_prefix) :]
                if raw_path.startswith(package_prefix)
                else ""
            )
            direct = direct_root / local_name
            expected_sha = _exact_sha256(row.get("sha256"))
            valid = bool(
                local_name
                and expected_sha
                and direct.is_file()
                and packaged.is_file()
                and _within(direct, direct_root)
                and _within(packaged, package_root or run_dir)
                and direct.stat().st_size == row.get("size")
                and packaged.stat().st_size == row.get("size")
                and sha256_file(direct).lower() == expected_sha
                and sha256_file(packaged).lower() == expected_sha
            )
            group_valid = group_valid and valid
            verified_files.append(
                {"path": local_name, "sha256": expected_sha, "valid": valid}
            )
        direct_names = {
            path.relative_to(direct_root).as_posix()
            for path in direct_root.rglob("*")
            if path.is_file()
        }
        listed_names = {row["path"] for row in verified_files}
        group_valid = group_valid and direct_names == listed_names
        uf2_rows = [
            row for row in verified_files if row["path"].lower().endswith(".uf2")
        ]
        group_valid = group_valid and len(uf2_rows) == 1
        if name == "rp2040-sd-bridge-firmware" and len(uf2_rows) == 1:
            bridge_sha256 = uf2_rows[0]["sha256"]
        rp2040_valid = rp2040_valid and group_valid
        rp2040_rows.append(
            {"name": name, "files": verified_files, "valid": group_valid}
        )
    if not rp2040_valid or not bridge_sha256:
        failures.append("actions:rp2040_payload_identity_invalid")

    tree_rows, tree_sha256 = _tree_receipt(run_dir)
    app_row = next((row for row in firmware_rows if row["role"] == "app"), {})
    checks = {
        "host_success_marker": marker_valid,
        "release_manifest_identity": manifest_valid,
        "required_checksum_manifests": required_manifests_present,
        "all_checksum_files_verified": checksum_files_valid,
        "firmware_payload_identity": firmware_valid,
        "rp2040_payload_identity": rp2040_valid and bridge_sha256 is not None,
    }
    base.update(
        {
            "workflow_run_attempt": str(workflow.get("run_attempt"))
            if isinstance(workflow, dict)
            else None,
            "host_success_marker": {
                "path": _relative(marker_path, run_dir) if marker_path else None,
                "sha256": sha256_file(marker_path).lower() if marker_path else None,
            },
            "release_manifest": {
                "path": _relative(package_manifest_path, run_dir)
                if package_manifest_path
                else None,
                "sha256": sha256_file(package_manifest_path).lower()
                if package_manifest_path
                else None,
            },
            "checksum_manifests": manifest_rows,
            "checksum_files": scalar_rows,
            "checksum_manifest_count": len(manifest_rows),
            "checksum_entry_count": sum(row["entry_count"] for row in manifest_rows),
            "firmware": firmware_rows,
            "rp2040": rp2040_rows,
            "esp32_app_sha256": app_row.get("sha256"),
            "rp2040_bridge_sha256": bridge_sha256,
            "tree_file_count": len(tree_rows),
            "tree_sha256": tree_sha256,
            "checks": checks,
            "valid": all(checks.values()),
        }
    )
    if not base["valid"] and not failures:
        failures.append("actions:invalid")
    return base, failures


def _receipt_evidence(
    role: str,
    path: Path,
    *,
    root: Path,
    main_sha: str,
    run_id: str,
    d1l_port: str,
    rp2040_port: str,
    actions: dict[str, Any],
) -> tuple[dict[str, Any], list[str]]:
    failures: list[str] = []
    inside = _within(path, root)
    if not inside or not path.is_file():
        return (
            {
                "path": _relative(path, root) if inside else None,
                "sha256": None,
                "valid": False,
            },
            [f"receipt:{role}:missing_or_outside_repository"],
        )
    data = _read_json(path)
    identity = data.get("artifact_identity")
    identity = identity if isinstance(identity, dict) else {}
    checks = data.get("checks")
    checks = checks if isinstance(checks, dict) else {}
    ports = data.get("ports")
    ports = ports if isinstance(ports, dict) else {}
    expected_checks = ROLE_CHECKS[role]
    common_valid = bool(
        data.get("schema") == SCHEMA
        and data.get("kind") == f"wp02_{role}_qualification"
        and data.get("mode") == "hardware"
        and all(
            data.get(name) is expected
            for name, expected in CANONICAL_PHYSICAL_FIELDS.items()
        )
        and data.get("ok") is True
        and data.get("commit") == main_sha
        and str(data.get("github_actions_run")) == run_id
        and data.get("device_build_commit") == main_sha
        and data.get("firmware_identity_ok") is True
        and _port(ports.get("d1l")) == d1l_port
        and _port(ports.get("rp2040")) == rp2040_port
        and data.get("public_rf_tx") is False
        and data.get("dm_rf_tx") is False
        and data.get("formats_sd") is False
        and data.get("failures") == []
        and all(checks.get(name) is True for name in expected_checks)
        and _exact_sha256(identity.get("release_manifest_sha256"))
        == actions.get("release_manifest", {}).get("sha256")
        and _exact_sha256(identity.get("esp32_app_sha256"))
        == actions.get("esp32_app_sha256")
    )
    pair_valid = True
    if role in {"sd", "reboot"}:
        pair_valid = bool(
            data.get("rp2040_device_build_commit") == main_sha
            and _exact_sha256(identity.get("rp2040_bridge_sha256"))
            == actions.get("rp2040_bridge_sha256")
        )
    valid = common_valid and pair_valid and actions.get("valid") is True
    if not valid:
        failures.append(f"receipt:{role}:invalid_or_stale")
    entry = {
        "path": _relative(path, root),
        "sha256": sha256_file(path).lower(),
        "kind": data.get("kind"),
        "mode": data.get("mode"),
        "commit": data.get("commit"),
        "github_actions_run": str(data.get("github_actions_run")),
        "device_build_commit": data.get("device_build_commit"),
        "rp2040_device_build_commit": data.get("rp2040_device_build_commit"),
        "ports": {"d1l": _port(ports.get("d1l")), "rp2040": _port(ports.get("rp2040"))},
        "artifact_identity": identity,
        "required_checks": {name: checks.get(name) for name in expected_checks},
        "safety": {
            "public_rf_tx": data.get("public_rf_tx"),
            "dm_rf_tx": data.get("dm_rf_tx"),
            "formats_sd": data.get("formats_sd"),
        },
        "physical": {name: data.get(name) for name in CANONICAL_PHYSICAL_FIELDS},
        "valid": valid,
    }
    return entry, failures


def build_integration_baseline(
    *,
    root: Path,
    main_sha: str,
    github_actions_run: str,
    github_run_dir: Path,
    d1l_port: str,
    rp2040_port: str,
    layers: dict[int, str],
    receipts: dict[str, Path],
) -> dict[str, Any]:
    root = root.resolve()
    main_sha = str(main_sha).lower()
    github_actions_run = str(github_actions_run)
    d1l_port = _port(d1l_port) or str(d1l_port).upper()
    rp2040_port = _port(rp2040_port) or str(rp2040_port).upper()
    failures: list[str] = []

    context_valid = bool(
        _exact_commit(main_sha)
        and RUN_ID_RE.fullmatch(github_actions_run)
        and _safe_ports(d1l_port, rp2040_port)
    )
    if not _exact_commit(main_sha):
        failures.append("context:main_sha_invalid")
    if not RUN_ID_RE.fullmatch(github_actions_run):
        failures.append("context:github_actions_run_invalid")
    if not _safe_ports(d1l_port, rp2040_port):
        failures.append("context:ports_invalid_or_forbidden")

    repository, repository_failures = _repository_evidence(root, main_sha, layers)
    failures.extend(repository_failures)
    actions, actions_failures = _actions_evidence(
        root, github_run_dir.resolve(), main_sha, github_actions_run
    )
    failures.extend(actions_failures)

    qualification: dict[str, Any] = {}
    receipt_paths: list[str] = []
    for role in ROLE_CHECKS:
        path = receipts.get(role)
        if path is None:
            qualification[role] = {"path": None, "sha256": None, "valid": False}
            failures.append(f"receipt:{role}:missing")
            continue
        resolved = path.resolve()
        entry, role_failures = _receipt_evidence(
            role,
            resolved,
            root=root,
            main_sha=main_sha,
            run_id=github_actions_run,
            d1l_port=d1l_port,
            rp2040_port=rp2040_port,
            actions=actions,
        )
        qualification[role] = entry
        failures.extend(role_failures)
        if entry.get("path"):
            receipt_paths.append(str(entry["path"]))
    unique_receipts = len(receipt_paths) == len(set(receipt_paths)) == len(ROLE_CHECKS)
    if not unique_receipts:
        failures.append("receipt:paths_not_unique")

    receipt_safety = bool(
        qualification
        and all(
            entry.get("safety")
            == {"public_rf_tx": False, "dm_rf_tx": False, "formats_sd": False}
            for entry in qualification.values()
        )
    )
    receipt_physical = bool(
        qualification
        and all(
            entry.get("physical") == CANONICAL_PHYSICAL_FIELDS
            for entry in qualification.values()
        )
    )
    checks = {
        "context_valid": context_valid,
        "repository_exact_clean_main": repository.get("valid") is True,
        "required_pr_layers_landed": repository.get("checks", {}).get(
            "required_layers_landed"
        )
        is True,
        "actions_exact_run_and_commit": actions.get("checks", {}).get(
            "release_manifest_identity"
        )
        is True,
        "actions_checksums_verified": actions.get("checks", {}).get(
            "all_checksum_files_verified"
        )
        is True
        and actions.get("checks", {}).get("required_checksum_manifests") is True,
        "actions_payloads_verified": actions.get("checks", {}).get(
            "firmware_payload_identity"
        )
        is True
        and actions.get("checks", {}).get("rp2040_payload_identity") is True,
        "receipt_paths_unique": unique_receipts,
        "receipt_safety_fields": receipt_safety,
        "receipt_physical_fields": receipt_physical,
        **{
            f"{role}_receipt": qualification.get(role, {}).get("valid") is True
            for role in ROLE_CHECKS
        },
    }
    failures = sorted(set(failures))
    return {
        "schema": SCHEMA,
        "kind": "wp02_integration_baseline",
        "mode": "exact-merged-main",
        "ok": not failures and all(checks.values()),
        "main_sha": main_sha,
        "github_actions_run": github_actions_run,
        "ports": {"d1l": d1l_port, "rp2040": rp2040_port},
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "repository": repository,
        "actions": actions,
        "qualification": qualification,
        "checks": checks,
        "failures": failures,
    }


def validate_integration_baseline(data: dict[str, Any], root: Path) -> bool:
    if not (
        isinstance(data, dict)
        and data.get("schema") == SCHEMA
        and data.get("kind") == "wp02_integration_baseline"
        and data.get("mode") == "exact-merged-main"
        and data.get("ok") is True
        and data.get("public_rf_tx") is False
        and data.get("dm_rf_tx") is False
        and data.get("formats_sd") is False
    ):
        return False
    root = root.resolve()
    repository = data.get("repository")
    actions = data.get("actions")
    qualification = data.get("qualification")
    ports = data.get("ports")
    if not all(
        isinstance(value, dict) for value in (repository, actions, qualification, ports)
    ):
        return False
    try:
        layers = {
            int(row["pr"]): str(row["commit"])
            for row in repository.get("layers", [])
            if isinstance(row, dict)
        }
        receipts = {
            role: _resolve_inside(root, qualification[role]["path"])
            for role in ROLE_CHECKS
        }
        run_dir = _resolve_inside(root, actions["run_dir"])
    except (KeyError, TypeError, ValueError):
        return False
    rebuilt = build_integration_baseline(
        root=root,
        main_sha=str(data.get("main_sha")),
        github_actions_run=str(data.get("github_actions_run")),
        github_run_dir=run_dir,
        d1l_port=str(ports.get("d1l")),
        rp2040_port=str(ports.get("rp2040")),
        layers=layers,
        receipts=receipts,
    )
    return rebuilt.get("ok") is True and data == rebuilt


def _parse_layers(values: Iterable[str]) -> dict[int, str]:
    layers: dict[int, str] = {}
    for value in values:
        match = re.fullmatch(r"(?:PR)?([0-9]+)=([0-9a-fA-F]{40})", value.strip())
        if not match:
            raise ValueError("--layer must use PR=40-hex-sha, for example 62=<sha>")
        pr = int(match.group(1))
        if pr in layers:
            raise ValueError(f"duplicate --layer for PR {pr}")
        layers[pr] = match.group(2).lower()
    return layers


def _add_generate_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--root", default=".")
    parser.add_argument("--main-sha", required=True)
    parser.add_argument("--github-actions-run", required=True)
    parser.add_argument("--github-run-dir", required=True)
    parser.add_argument("--d1l-port", required=True)
    parser.add_argument("--rp2040-port", required=True)
    parser.add_argument("--layer", action="append", required=True)
    for role in ROLE_CHECKS:
        parser.add_argument(f"--{role.replace('_', '-')}-receipt", required=True)
    parser.add_argument("--out")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    _add_generate_args(subparsers.add_parser("generate"))
    validate = subparsers.add_parser("validate")
    validate.add_argument("--root", default=".")
    validate.add_argument("--artifact", required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = Path(args.root).resolve()
    if args.command == "validate":
        try:
            artifact_path = _resolve_inside(root, args.artifact)
        except ValueError:
            print(json.dumps({"ok": False, "error": "artifact_outside_repository"}))
            return 1
        data = _read_json(artifact_path)
        valid = validate_integration_baseline(data, root)
        print(json.dumps({"ok": valid, "artifact": _relative(artifact_path, root)}))
        return 0 if valid else 1

    try:
        layers = _parse_layers(args.layer)
        run_dir = _resolve_inside(root, args.github_run_dir)
        receipts = {
            role: _resolve_inside(root, getattr(args, f"{role}_receipt"))
            for role in ROLE_CHECKS
        }
        out = _resolve_inside(
            root,
            args.out
            or f"artifacts/release/integration_baseline_{str(args.main_sha).lower()}.json",
        )
    except ValueError as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    if _within(out, run_dir) or out in set(receipts.values()):
        print(json.dumps({"ok": False, "error": "output_overwrites_input"}))
        return 2
    report = build_integration_baseline(
        root=root,
        main_sha=args.main_sha,
        github_actions_run=args.github_actions_run,
        github_run_dir=run_dir,
        d1l_port=args.d1l_port,
        rp2040_port=args.rp2040_port,
        layers=layers,
        receipts=receipts,
    )
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(
        (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8")
    )
    print(
        json.dumps(
            {
                "ok": report["ok"],
                "artifact": _relative(out, root),
                "sha256": sha256_file(out).lower(),
                "failures": report["failures"],
            },
            sort_keys=True,
        )
    )
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
