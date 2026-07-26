#!/usr/bin/env python3
"""Fail-closed aggregation for the four WP-01 physical evidence artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
from typing import Any

try:
    from autonomous_hardware_validate_d1l import (
        RunContext,
        verify_actions_artifact_provenance,
        verify_esp32_flash_inputs,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.autonomous_hardware_validate_d1l import (
        RunContext,
        verify_actions_artifact_provenance,
        verify_esp32_flash_inputs,
    )

try:
    from wp01_evidence_sources_d1l import (
        SOURCE_BOUND_KINDS,
        rebuild_source_bound_artifact,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.wp01_evidence_sources_d1l import (
        SOURCE_BOUND_KINDS,
        rebuild_source_bound_artifact,
    )


WP01_ARTIFACT_KINDS = (
    "sd_inserted_stability",
    "sd_remove_reinsert",
    "retained_reboot_matrix",
    "storage_active_soak",
)
WP01_ARTIFACT_PATTERNS = {
    "sd_inserted_stability": "sd_inserted_stability_*.json",
    "sd_remove_reinsert": "sd_remove_reinsert_*.json",
    "retained_reboot_matrix": "retained_reboot_matrix_*.json",
    "storage_active_soak": "storage_active_soak_*.json",
}
WP01_PROVENANCE_PATTERN = "wp01_exact_pair_provenance_*.json"
WP01_REPOSITORY = "n30nex/SIGUI"
WP01_INSERTED_STABILITY_MIN_SECONDS = 5 * 60
WP01_REMOVE_REINSERT_MIN_CYCLES = 1
WP01_REBOOT_MIN_CYCLES = 5
WP01_STORAGE_ACTIVE_SOAK_MIN_SECONDS = 2 * 60 * 60
WP01_RETAINED_STACK_MIN_BYTES = 4096
FORBIDDEN_PORTS = {"COM" + str(number) for number in (8, 11, 29)}
SHA256_RE = re.compile(r"[0-9a-f]{64}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
RUN_ID_RE = re.compile(r"[1-9][0-9]*")
COM_PORT_RE = re.compile(r"COM[1-9][0-9]*")


def _port(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().upper()
    return normalized or None


def _count(value: object, *, minimum: int = 0) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= minimum


def _duration(value: object, *, minimum: float) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and float(value) >= minimum
    )


def _zero_counts(data: dict, *names: str) -> bool:
    return all(_count(data.get(name)) and data.get(name) == 0 for name in names)


def _context_ok(
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
) -> bool:
    normalized_d1l = _port(d1l_port)
    normalized_rp2040 = _port(rp2040_port)
    return bool(
        isinstance(commit, str)
        and COMMIT_RE.fullmatch(commit.lower()) is not None
        and RUN_ID_RE.fullmatch(str(github_actions_run)) is not None
        and normalized_d1l
        and normalized_rp2040
        and COM_PORT_RE.fullmatch(normalized_d1l)
        and COM_PORT_RE.fullmatch(normalized_rp2040)
        and normalized_d1l not in FORBIDDEN_PORTS
        and normalized_rp2040 not in FORBIDDEN_PORTS
        and normalized_d1l != normalized_rp2040
    )


def _within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def _canonical_sha256(data: dict) -> str:
    encoded = json.dumps(data, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _verification_context(
    root: Path,
    github_run_dir: Path,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
) -> RunContext:
    root = root.resolve()
    github_run_dir = github_run_dir.resolve()
    return RunContext(
        root=root,
        commit=commit.lower(),
        short_commit=commit[:7].lower(),
        github_run_id=str(github_actions_run),
        github_run_dir=github_run_dir,
        d1l_port=_port(d1l_port) or "",
        rp2040_port=_port(rp2040_port) or "",
        hardware_dir=root / "artifacts" / "hardware" / (_port(d1l_port) or "").lower(),
        rp2040_hardware_dir=root / "artifacts" / "hardware" / (_port(rp2040_port) or "").lower(),
        baud=115200,
        esp32_flash_baud=460800,
    )


def _verified_actions_inputs(
    root: Path,
    github_run_dir: Path,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
    *,
    include_official_smoke: bool = False,
) -> tuple[RunContext, dict, dict]:
    if not _context_ok(commit, github_actions_run, d1l_port, rp2040_port):
        raise ValueError("invalid exact-pair commit, run, or COM-port context")
    root = root.resolve()
    github_run_dir = github_run_dir.resolve()
    if not _within(github_run_dir, root):
        raise ValueError("GitHub Actions artifact directory must be within the repository root")
    ctx = _verification_context(
        root,
        github_run_dir,
        commit,
        github_actions_run,
        d1l_port,
        rp2040_port,
    )
    verifier_args = SimpleNamespace(
        refresh_rp2040_smoke=include_official_smoke,
        skip_rp2040_official_smoke=not include_official_smoke,
        skip_sd_suite=False,
    )
    actions_provenance = verify_actions_artifact_provenance(ctx, verifier_args)
    esp32_artifact_verification = verify_esp32_flash_inputs(ctx)
    return ctx, actions_provenance, esp32_artifact_verification


def _derived_firmware_hashes(actions_provenance: dict) -> tuple[dict[str, str], dict]:
    expected_roles = {
        "bootloader": "0x0",
        "partition-table": "0x8000",
        "app": "0x10000",
    }
    flash_files = actions_provenance.get("esp32_flash_files")
    if not isinstance(flash_files, list):
        raise ValueError("verified Actions provenance has no ESP32 flash files")
    by_role = {
        item.get("role"): item
        for item in flash_files
        if isinstance(item, dict) and item.get("role") in expected_roles
    }
    if set(by_role) != set(expected_roles):
        raise ValueError("verified Actions provenance has incomplete ESP32 flash roles")
    esp32_hashes = {
        offset: str(by_role[role].get("sha256") or "").lower()
        for role, offset in expected_roles.items()
    }
    if not all(SHA256_RE.fullmatch(value) for value in esp32_hashes.values()):
        raise ValueError("verified Actions provenance has invalid ESP32 hashes")

    groups = actions_provenance.get("rp2040_artifact_groups")
    bridge_groups = [
        group
        for group in groups if isinstance(group, dict)
        and group.get("name") == "rp2040-sd-bridge-firmware"
    ] if isinstance(groups, list) else []
    if len(bridge_groups) != 1:
        raise ValueError("verified Actions provenance has no unique RP2040 bridge group")
    bridge_group = bridge_groups[0]
    uf2_files = [
        item for item in bridge_group.get("files", [])
        if isinstance(item, dict) and item.get("path") == "deskos_sd_bridge.ino.uf2"
    ]
    if len(uf2_files) != 1:
        raise ValueError("verified Actions provenance has no unique RP2040 bridge UF2")
    rp2040_uf2 = uf2_files[0]
    rp2040_sha = str(rp2040_uf2.get("sha256") or "").lower()
    if SHA256_RE.fullmatch(rp2040_sha) is None:
        raise ValueError("verified Actions provenance has an invalid RP2040 UF2 hash")
    return esp32_hashes, {
        **rp2040_uf2,
        "sha256": rp2040_sha,
        "artifact_dir": bridge_group.get("artifact_dir"),
    }


def _last_run(runs: list, kind: str) -> tuple[int, dict]:
    matches = [
        (index, step)
        for index, step in enumerate(runs)
        if isinstance(step, dict) and step.get("kind") == kind
    ]
    if not matches:
        raise ValueError(f"hardware validation report has no {kind} step")
    return matches[-1]


def _esp32_install_ok(
    step: dict,
    expected_artifact_verification: dict,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
) -> bool:
    result = step.get("result") if isinstance(step.get("result"), dict) else {}
    return bool(
        step.get("schema") == 1
        and step.get("kind") == "esp32_flash"
        and step.get("mode") == "hardware"
        and step.get("ok") is True
        and step.get("commit", "").lower() == commit.lower()
        and str(step.get("github_actions_run")) == str(github_actions_run)
        and _port(step.get("port")) == _port(d1l_port)
        and step.get("artifact_verification") == expected_artifact_verification
        and step.get("public_rf_tx") is False
        and step.get("formats_sd") is False
        and result.get("name") == "esp32_flash"
        and result.get("ok") is True
        and result.get("returncode") == 0
    )


def _rp2040_install_ok(
    step: dict,
    expected_uf2: dict,
    commit: str,
    github_actions_run: str,
    rp2040_port: str,
) -> bool:
    copy_result = step.get("copy") if isinstance(step.get("copy"), dict) else {}
    artifact = copy_result.get("artifact") if isinstance(copy_result.get("artifact"), dict) else {}
    ping = step.get("ping") if isinstance(step.get("ping"), dict) else {}
    expected_path = (
        Path(str(expected_uf2.get("artifact_dir"))) / str(expected_uf2.get("path"))
    ).resolve()
    artifact_path = Path(str(artifact.get("path") or "")).resolve()
    return bool(
        step.get("schema") == 1
        and step.get("kind") == "rp2040_bridge_restore"
        and step.get("mode") == "hardware"
        and step.get("ok") is True
        and step.get("commit", "").lower() == commit.lower()
        and str(step.get("github_actions_run")) == str(github_actions_run)
        and _port(step.get("port")) == _port(rp2040_port)
        and step.get("public_rf_tx") is False
        and step.get("formats_sd") is False
        and copy_result.get("ok") is True
        and copy_result.get("copied") is True
        and artifact_path == expected_path
        and artifact.get("size") == expected_uf2.get("size")
        and str(artifact.get("sha256") or "").lower() == expected_uf2.get("sha256")
        and ping.get("ok") is True
        and ping.get("protocol_supported") is True
    )


def build_wp01_provenance_receipt(
    source_validation_report: Path,
    *,
    root: Path,
    github_run_dir: Path,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
) -> dict:
    """Derive exact-pair provenance from recomputed Actions hashes and flash receipts."""
    root = root.resolve()
    source_validation_report = source_validation_report.resolve()
    github_run_dir = github_run_dir.resolve()
    receipt: dict[str, Any] = {
        "schema": 1,
        "kind": "wp01_exact_pair_provenance",
        "mode": "hardware",
        "ok": False,
        "source_commit": commit.lower() if isinstance(commit, str) else commit,
        "github_actions_run": str(github_actions_run),
        "ports": {"d1l": _port(d1l_port), "rp2040": _port(rp2040_port)},
        "public_rf_tx": False,
        "formats_sd": False,
        "failures": [],
    }
    try:
        if not _within(source_validation_report, root):
            raise ValueError("hardware validation report must be within the repository root")
        source_report = read_json(source_validation_report)
        if not source_report:
            raise ValueError("missing or invalid hardware validation report")
        runs = source_report.get("runs")
        if not isinstance(runs, list):
            raise ValueError("hardware validation report has no runs list")
        input_index, input_step = _last_run(runs, "input_artifact_check")
        claimed_provenance = (
            input_step.get("provenance")
            if isinstance(input_step.get("provenance"), dict)
            else {}
        )
        claimed_groups = claimed_provenance.get("rp2040_artifact_groups")
        claimed_group_names = {
            group.get("name")
            for group in claimed_groups
            if isinstance(group, dict) and isinstance(group.get("name"), str)
        } if isinstance(claimed_groups, list) else set()
        bridge_group = "rp2040-sd-bridge-firmware"
        official_group = "rp2040-seeed-official-sd-smoke-firmware"
        if claimed_group_names not in ({bridge_group}, {bridge_group, official_group}):
            raise ValueError("hardware validation report claims an unsupported RP2040 artifact group set")
        ctx, actions_provenance, esp32_artifact_verification = _verified_actions_inputs(
            root,
            github_run_dir,
            commit,
            github_actions_run,
            d1l_port,
            rp2040_port,
            include_official_smoke=official_group in claimed_group_names,
        )
        esp32_hashes, rp2040_uf2 = _derived_firmware_hashes(actions_provenance)
        manifest = read_json(Path(actions_provenance["release_manifest"]))
        workflow = manifest.get("workflow") if isinstance(manifest.get("workflow"), dict) else {}
        workflow_ref = workflow.get("ref")
        if not isinstance(workflow_ref, str) or not workflow_ref.startswith(("refs/heads/", "refs/tags/")):
            raise ValueError("verified release manifest has no canonical workflow ref")

        esp32_index, esp32_step = _last_run(runs, "esp32_flash")
        rp2040_index, rp2040_step = _last_run(runs, "rp2040_bridge_restore")
        source_ports = source_report.get("ports")
        if not (
            source_report.get("schema") == 1
            and source_report.get("kind") == "d1l_autonomous_hardware_validation"
            and source_report.get("mode") == "hardware"
            and source_report.get("commit", "").lower() == commit.lower()
            and str(source_report.get("github_actions_run")) == str(github_actions_run)
            and isinstance(source_ports, dict)
            and _port(source_ports.get("d1l")) == _port(d1l_port)
            and _port(source_ports.get("rp2040")) == _port(rp2040_port)
            and source_report.get("public_rf_tx") is False
            and source_report.get("formats_sd") is False
            and input_step.get("schema") == 1
            and input_step.get("mode") == "check"
            and input_step.get("ok") is True
            and input_step.get("provenance") == actions_provenance
        ):
            raise ValueError("hardware validation report is not bound to the verified Actions inputs")
        if not _esp32_install_ok(
            esp32_step,
            esp32_artifact_verification,
            commit,
            github_actions_run,
            d1l_port,
        ):
            raise ValueError("hardware validation report has no valid final ESP32 flash receipt")
        if not _rp2040_install_ok(
            rp2040_step,
            rp2040_uf2,
            commit,
            github_actions_run,
            rp2040_port,
        ):
            raise ValueError("hardware validation report has no valid final RP2040 flash receipt")

        receipt.update(
            {
                "ok": True,
                "repository": actions_provenance.get("repository"),
                "workflow_ref": workflow_ref,
                "github_run_attempt": int(actions_provenance["workflow_run_attempt"]),
                "esp32_device_build_commit": commit.lower(),
                "rp2040_device_build_commit": commit.lower(),
                "github_run_dir": str(ctx.github_run_dir),
                "source_validation_report": {
                    "path": str(source_validation_report),
                    "sha256": sha256_file(source_validation_report),
                },
                "artifact_provenance": actions_provenance,
                "release_manifest_sha256": str(actions_provenance["release_manifest_sha256"]).lower(),
                "esp32_flash_sha256": esp32_hashes,
                "rp2040_uf2_sha256": rp2040_uf2["sha256"],
                "installation_receipts": {
                    "input_artifact_check": {
                        "run_index": input_index,
                        "step_sha256": _canonical_sha256(input_step),
                    },
                    "esp32_flash": {
                        "run_index": esp32_index,
                        "step_sha256": _canonical_sha256(esp32_step),
                    },
                    "rp2040_bridge_restore": {
                        "run_index": rp2040_index,
                        "step_sha256": _canonical_sha256(rp2040_step),
                    },
                },
                "checksums_verified": True,
                "release_manifest_verified": True,
                "esp32_flash_verified": True,
                "rp2040_flash_verified": True,
                "exact_pair_installed": True,
                "failures": [],
            }
        )
    except Exception as exc:
        receipt["failures"] = [str(exc)]
    return receipt


def wp01_provenance_receipt_ok(
    data: dict,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
    *,
    root: Path | None = None,
    github_run_dir: Path | None = None,
) -> bool:
    """Rebuild the receipt from source files; never trust claimed hashes or booleans."""
    if not isinstance(data, dict) or root is None or github_run_dir is None:
        return False
    source = data.get("source_validation_report")
    if not isinstance(source, dict) or not isinstance(source.get("path"), str):
        return False
    expected = build_wp01_provenance_receipt(
        Path(source["path"]),
        root=root,
        github_run_dir=github_run_dir,
        commit=commit,
        github_actions_run=github_actions_run,
        d1l_port=d1l_port,
        rp2040_port=rp2040_port,
    )
    return expected.get("ok") is True and data == expected


def _common_artifact_ok(
    data: dict,
    kind: str,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
) -> bool:
    ports = data.get("ports")
    normalized_d1l = _port(d1l_port)
    normalized_rp2040 = _port(rp2040_port)
    return (
        kind in WP01_ARTIFACT_KINDS
        and _context_ok(
            commit,
            github_actions_run,
            d1l_port,
            rp2040_port,
        )
        and data.get("schema") == 1
        and data.get("kind") == kind
        and data.get("mode") == "hardware"
        and data.get("ok") is True
        and isinstance(data.get("commit"), str)
        and data["commit"].lower() == commit.lower()
        and str(data.get("github_actions_run")) == str(github_actions_run)
        and isinstance(ports, dict)
        and _port(ports.get("d1l")) == normalized_d1l
        and _port(ports.get("rp2040")) == normalized_rp2040
        and data.get("public_rf_tx") is False
        and data.get("dm_rf_tx") is False
        and data.get("formats_sd") is False
        and isinstance(data.get("provenance"), dict)
        and isinstance(data.get("provenance_receipt_sha256"), str)
        and SHA256_RE.fullmatch(data["provenance_receipt_sha256"].lower()) is not None
        and _zero_counts(
            data,
            "wdt_count",
            "panic_count",
            "brownout_count",
            "terminal_timeout_count",
        )
    )


def _inserted_stability_ok(data: dict) -> bool:
    sample_count = data.get("sample_count")
    return (
        _duration(data.get("duration_sec"), minimum=WP01_INSERTED_STABILITY_MIN_SECONDS)
        and _count(sample_count, minimum=2)
        and data.get("status_poll_count") == sample_count
        and data.get("card_inserted_all_samples") is True
        and data.get("final_ready_sd") is True
        and _zero_counts(
            data,
            "false_no_card_count",
            "unintended_backend_generation_count",
            "retained_failure_count",
            "reset_count",
        )
    )


REMOVE_REINSERT_CYCLE_TRUE_FIELDS = (
    "true_removal_detected",
    "nvs_fallback_active",
    "deterministic_remount",
    "generation_changed",
    "read_merge_before_overwrite",
    "all_stores_reconciled",
    "write_before_edge",
    "write_during_edge",
    "write_after_edge",
)


def _remove_reinsert_cycle_ok(cycle: object) -> bool:
    return (
        isinstance(cycle, dict)
        and cycle.get("ok") is True
        and all(cycle.get(field) is True for field in REMOVE_REINSERT_CYCLE_TRUE_FIELDS)
        and cycle.get("reset_storm") is False
        and cycle.get("delete_or_rename_against_replacement") is False
        and cycle.get("user_data_loss") is False
    )


def _remove_reinsert_ok(data: dict) -> bool:
    cycles = data.get("cycles")
    cycle_count = data.get("cycle_count")
    return (
        isinstance(cycles, list)
        and _count(cycle_count, minimum=WP01_REMOVE_REINSERT_MIN_CYCLES)
        and len(cycles) == cycle_count
        and all(_remove_reinsert_cycle_ok(cycle) for cycle in cycles)
        and data.get("all_cycles_passed") is True
        and _zero_counts(data, "reset_storm_count", "cross_media_mutation_count", "data_loss_count")
    )


REBOOT_CYCLE_TRUE_FIELDS = (
    "reboot_proven",
    "storage_manager_quiesced",
    "retained_worker_quiesced",
    "rp2040_bridge_quiesced",
)


def _reboot_cycle_ok(cycle: object) -> bool:
    return (
        isinstance(cycle, dict)
        and cycle.get("ok") is True
        and cycle.get("post_reboot_reset_reason") == "SW"
        and cycle.get("reset_scope") == "system"
        and cycle.get("connectivity_prepare") == "ESP_OK"
        and all(cycle.get(field) is True for field in REBOOT_CYCLE_TRUE_FIELDS)
        and cycle.get("retained_flush") == "ESP_OK"
        and cycle.get("route_flush") == "ESP_OK"
        and cycle.get("pending_dirty") is False
        and cycle.get("wdt") is False
        and cycle.get("panic") is False
        and cycle.get("brownout") is False
        and _count(
            cycle.get("retained_task_stack_free_bytes"),
            minimum=WP01_RETAINED_STACK_MIN_BYTES,
        )
    )


def _retained_reboot_matrix_ok(data: dict) -> bool:
    cycles = data.get("cycles")
    cycle_count = data.get("cycle_count")
    return (
        isinstance(cycles, list)
        and _count(cycle_count, minimum=WP01_REBOOT_MIN_CYCLES)
        and len(cycles) == cycle_count
        and all(_reboot_cycle_ok(cycle) for cycle in cycles)
        and data.get("all_cycles_passed") is True
        and _zero_counts(data, "dirty_recovery_failure_count")
    )


def _storage_active_soak_ok(data: dict) -> bool:
    dirty_events = data.get("dirty_event_counts")
    segments = data.get("segments")
    segment_count = data.get("segment_count")
    reboots = data.get("controlled_reboots")
    reboot_count = data.get("controlled_reboot_count")
    return (
        _duration(data.get("duration_sec"), minimum=WP01_STORAGE_ACTIVE_SOAK_MIN_SECONDS)
        and _count(data.get("sample_count"), minimum=2)
        and isinstance(segments, list)
        and _count(segment_count, minimum=2)
        and len(segments) == segment_count
        and all(isinstance(segment, dict) and segment.get("ok") is True for segment in segments)
        and data.get("status_poll_count") == data.get("sample_count")
        and data.get("storage_active") is True
        and isinstance(dirty_events, dict)
        and all(
            dirty_events.get(name) == reboot_count
            for name in ("public", "dm", "routes", "packets")
        )
        and isinstance(reboots, list)
        and _count(reboot_count, minimum=1)
        and reboot_count == segment_count - 1
        and len(reboots) == reboot_count
        and all(_reboot_cycle_ok(reboot) for reboot in reboots)
        and data.get("all_segments_passed") is True
        and data.get("all_controlled_reboots_passed") is True
        and data.get("final_ready_sd") is True
        and _count(
            data.get("retained_task_stack_free_bytes_floor"),
            minimum=WP01_RETAINED_STACK_MIN_BYTES,
        )
        and _zero_counts(
            data,
            "false_no_card_count",
            "unintended_backend_generation_count",
            "retained_failure_count",
            "command_retry_count",
            "unexpected_reset_count",
        )
    )


WP01_KIND_VALIDATORS = {
    "sd_inserted_stability": _inserted_stability_ok,
    "sd_remove_reinsert": _remove_reinsert_ok,
    "retained_reboot_matrix": _retained_reboot_matrix_ok,
    "storage_active_soak": _storage_active_soak_ok,
}


def wp01_artifact_ok(
    data: dict,
    kind: str,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
    expected_provenance: dict | None = None,
    expected_provenance_sha256: str | None = None,
    root: Path | None = None,
) -> bool:
    """Validate one canonical WP-01 physical artifact without trusting its filename."""
    rebuilt = (
        rebuild_source_bound_artifact(
            data,
            root=root,
            commit=commit,
            github_actions_run=github_actions_run,
            d1l_port=d1l_port,
            rp2040_port=rp2040_port,
        )
        if kind in SOURCE_BOUND_KINDS and isinstance(root, Path)
        else None
    )
    return (
        isinstance(data, dict)
        and isinstance(expected_provenance, dict)
        and isinstance(expected_provenance_sha256, str)
        and SHA256_RE.fullmatch(expected_provenance_sha256.lower()) is not None
        and _common_artifact_ok(
            data,
            kind,
            commit,
            github_actions_run,
            d1l_port,
            rp2040_port,
        )
        and WP01_KIND_VALIDATORS[kind](data)
        and data.get("provenance") == expected_provenance
        and data.get("provenance_receipt_sha256", "").lower()
        == expected_provenance_sha256.lower()
        and (
            kind not in SOURCE_BOUND_KINDS
            or (isinstance(rebuilt, dict) and rebuilt.get("ok") is True and data == rebuilt)
        )
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def wp01_aggregate_artifact_ok(
    data: dict,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
    expected_hashes: dict[str, str] | None = None,
    expected_provenance_sha256: str | None = None,
    expected_provenance: dict | None = None,
) -> bool:
    ports = data.get("ports")
    artifacts = data.get("artifacts")
    checks = data.get("checks")
    provenance_receipt = data.get("provenance_receipt")
    if not (
        isinstance(data, dict)
        and data.get("schema") == 1
        and data.get("kind") == "wp01_evidence_aggregate"
        and data.get("mode") == "evidence-aggregation"
        and data.get("ok") is True
        and isinstance(data.get("commit"), str)
        and data["commit"].lower() == commit.lower()
        and str(data.get("github_actions_run")) == str(github_actions_run)
        and isinstance(ports, dict)
        and _port(ports.get("d1l")) == _port(d1l_port)
        and _port(ports.get("rp2040")) == _port(rp2040_port)
        and data.get("public_rf_tx") is False
        and data.get("dm_rf_tx") is False
        and data.get("formats_sd") is False
        and data.get("failures") == []
        and isinstance(artifacts, dict)
        and isinstance(checks, dict)
        and set(artifacts) == set(WP01_ARTIFACT_KINDS)
        and set(checks) == set(WP01_ARTIFACT_KINDS)
        and all(checks.get(kind) is True for kind in WP01_ARTIFACT_KINDS)
        and isinstance(provenance_receipt, dict)
        and isinstance(provenance_receipt.get("path"), str)
        and provenance_receipt.get("path")
        and isinstance(provenance_receipt.get("sha256"), str)
        and SHA256_RE.fullmatch(provenance_receipt["sha256"].lower()) is not None
        and provenance_receipt.get("valid") is True
        and isinstance(expected_provenance, dict)
        and provenance_receipt.get("provenance") == expected_provenance
        and (
            expected_provenance_sha256 is None
            or provenance_receipt["sha256"].lower()
            == expected_provenance_sha256.lower()
        )
    ):
        return False

    for kind in WP01_ARTIFACT_KINDS:
        entry = artifacts.get(kind)
        if not (
            isinstance(entry, dict)
            and entry.get("kind") == kind
            and isinstance(entry.get("path"), str)
            and entry.get("path")
            and isinstance(entry.get("sha256"), str)
            and SHA256_RE.fullmatch(entry["sha256"].lower()) is not None
            and entry.get("valid") is True
        ):
            return False
        if expected_hashes is not None and entry["sha256"].lower() != expected_hashes.get(kind, "").lower():
            return False
    return True


def build_aggregate(
    paths: dict[str, Path],
    provenance_path: Path,
    *,
    commit: str,
    github_actions_run: str,
    d1l_port: str,
    rp2040_port: str,
    root: Path,
    github_run_dir: Path,
) -> dict:
    entries: dict[str, dict[str, Any]] = {}
    checks: dict[str, bool] = {}
    failures: list[str] = []
    resolved_paths: list[str] = []
    provenance = read_json(provenance_path)
    provenance_sha256 = sha256_file(provenance_path) if provenance_path.is_file() else None
    provenance_valid = bool(
        provenance_sha256
        and wp01_provenance_receipt_ok(
            provenance,
            commit,
            github_actions_run,
            d1l_port,
            rp2040_port,
            root=root,
            github_run_dir=github_run_dir,
        )
    )
    if not provenance_valid:
        failures.append("missing_or_invalid:exact_pair_provenance")

    for kind in WP01_ARTIFACT_KINDS:
        path = paths[kind]
        data = read_json(path)
        valid = wp01_artifact_ok(
            data,
            kind,
            commit,
            github_actions_run,
            d1l_port,
            rp2040_port,
            provenance if provenance_valid else None,
            provenance_sha256 if provenance_valid else None,
            root,
        )
        resolved = str(path.resolve())
        resolved_paths.append(resolved.lower())
        entries[kind] = {
            "kind": kind,
            "path": resolved,
            "sha256": sha256_file(path) if path.is_file() else None,
            "valid": valid,
        }
        checks[kind] = valid
        if not valid:
            failures.append(f"missing_or_invalid:{kind}")

    if len(set(resolved_paths)) != len(resolved_paths):
        failures.append("duplicate_source_artifact")

    return {
        "schema": 1,
        "kind": "wp01_evidence_aggregate",
        "mode": "evidence-aggregation",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "commit": commit,
        "github_actions_run": str(github_actions_run),
        "ports": {"d1l": _port(d1l_port), "rp2040": _port(rp2040_port)},
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "checks": checks,
        "artifacts": entries,
        "provenance_receipt": {
            "path": str(provenance_path.resolve()),
            "sha256": provenance_sha256,
            "valid": provenance_valid,
            "provenance": provenance,
        },
        "failures": failures,
        "ok": not failures,
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--github-actions-run", required=True)
    parser.add_argument("--github-run-dir", required=True)
    parser.add_argument("--d1l-port", required=True)
    parser.add_argument("--rp2040-port", required=True)
    parser.add_argument("--exact-pair-provenance", required=True)
    parser.add_argument("--hardware-validation-report", required=True)
    parser.add_argument("--provenance-only", action="store_true")
    parser.add_argument("--sd-inserted-stability")
    parser.add_argument("--sd-remove-reinsert")
    parser.add_argument("--retained-reboot-matrix")
    parser.add_argument("--storage-active-soak")
    parser.add_argument("--out")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = Path(args.root).resolve()
    github_run_dir = Path(args.github_run_dir).resolve()
    provenance_path = Path(args.exact_pair_provenance)
    provenance = build_wp01_provenance_receipt(
        Path(args.hardware_validation_report),
        root=root,
        github_run_dir=github_run_dir,
        commit=args.commit,
        github_actions_run=args.github_actions_run,
        d1l_port=args.d1l_port,
        rp2040_port=args.rp2040_port,
    )
    provenance_path.parent.mkdir(parents=True, exist_ok=True)
    provenance_path.write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    if args.provenance_only:
        print(json.dumps(provenance, indent=2, sort_keys=True))
        return 0 if provenance["ok"] else 1

    missing = [
        option
        for option in (
            "sd_inserted_stability",
            "sd_remove_reinsert",
            "retained_reboot_matrix",
            "storage_active_soak",
        )
        if not getattr(args, option)
    ]
    if missing:
        raise SystemExit(
            "full aggregation requires: "
            + ", ".join("--" + option.replace("_", "-") for option in missing)
        )
    paths = {
        "sd_inserted_stability": Path(args.sd_inserted_stability),
        "sd_remove_reinsert": Path(args.sd_remove_reinsert),
        "retained_reboot_matrix": Path(args.retained_reboot_matrix),
        "storage_active_soak": Path(args.storage_active_soak),
    }
    report = build_aggregate(
        paths,
        provenance_path,
        commit=args.commit,
        github_actions_run=args.github_actions_run,
        d1l_port=args.d1l_port,
        rp2040_port=args.rp2040_port,
        root=root,
        github_run_dir=github_run_dir,
    )
    out = Path(args.out) if args.out else Path("artifacts") / "hardware" / "wp01" / (
        f"wp01_acceptance_{args.commit[:7]}_{_port(args.d1l_port)}_{_port(args.rp2040_port)}.json"
    )
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
