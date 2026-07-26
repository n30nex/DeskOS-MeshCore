import copy
import json
import shutil
from pathlib import Path

import pytest

from scripts import release_gate_audit_d1l as audit
from scripts import autonomous_hardware_validate_d1l as runner
from scripts import wp01_evidence_aggregate_d1l as wp01
from scripts import wp01_evidence_produce_d1l as wp01_produce
from scripts import wp01_evidence_sources_d1l as wp01_sources
from tests.wp01_source_fixtures import (
    write_inserted_soak,
    write_reboot_source,
    write_storage_active_soak_source,
)
from tests.test_sd_remove_reinsert_acceptance_d1l import (
    COMMIT as REMOVE_FIXTURE_COMMIT,
    valid_report as valid_remove_reinsert_report,
)


COMMIT = "ad0aa5bda21435846f6e7fcdde8fd87a85c5da5c"
RUN_ID = "29222903210"


def write_actions_fixture(root: Path) -> Path:
    run_dir = root / "artifacts" / "github" / f"{RUN_ID}-current"
    build = run_dir / "d1l-firmware-artifacts" / "build"
    source_files = {
        "bootloader/bootloader.bin": b"bootloader",
        "partition_table/partition-table.bin": b"partition-table",
        "ota_data_initial.bin": b"ota-data",
        "meshcore_deskos_d1l.bin": b"application",
    }
    for relative, content in source_files.items():
        path = build / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    (build / "flasher_args.json").write_text(
        json.dumps(
            {
                "flash_files": {
                    "0x0": "bootloader/bootloader.bin",
                    "0x8000": "partition_table/partition-table.bin",
                    "0xf000": "ota_data_initial.bin",
                    "0x20000": "meshcore_deskos_d1l.bin",
                }
            }
        ),
        encoding="utf-8",
    )
    artifact_root = build.parent
    checksum_rows = []
    for path in sorted(item for item in artifact_root.rglob("*") if item.is_file()):
        checksum_rows.append(
            f"{wp01.sha256_file(path)}  {path.relative_to(artifact_root).as_posix()}"
        )
    (artifact_root / "SHA256SUMS.txt").write_text(
        "\n".join(checksum_rows) + "\n",
        encoding="ascii",
    )

    host = run_dir / "d1l-host-artifacts" / "host-checks"
    host.mkdir(parents=True, exist_ok=True)
    (host / f"d1l_host_checks_success_{COMMIT}.json").write_text(
        json.dumps(
            {
                "schema": 1,
                "artifact_type": "d1l_host_checks_success",
                "status": "pass",
                "passed": True,
                "all_prior_steps_completed": True,
                "job": "host-checks",
                "repository_commit": COMMIT,
                "workflow_run_id": RUN_ID,
                "workflow_run_attempt": "1",
            }
        ),
        encoding="utf-8",
    )

    bridge = run_dir / "rp2040-sd-bridge-firmware"
    bridge.mkdir(parents=True, exist_ok=True)
    bridge_uf2 = bridge / runner.BRIDGE_UF2
    bridge_uf2.write_bytes(b"exact-rp2040-bridge-uf2")
    (bridge / "SHA256SUMS.txt").write_text(
        f"{wp01.sha256_file(bridge_uf2)}  {runner.BRIDGE_UF2}\n",
        encoding="ascii",
    )

    package = run_dir / "d1l-release-package" / f"d1l-release-{COMMIT}"
    flash_files = []
    roles = (
        ("bootloader", "bootloader/bootloader.bin", "firmware/bootloader.bin", "0x0"),
        (
            "partition-table",
            "partition_table/partition-table.bin",
            "firmware/partition-table.bin",
            "0x8000",
        ),
        ("ota-data", "ota_data_initial.bin", "firmware/ota_data_initial.bin", "0xf000"),
        ("app", "meshcore_deskos_d1l.bin", "firmware/meshcore_deskos_d1l.bin", "0x20000"),
    )
    for role, source_name, package_name, offset in roles:
        source = build / source_name
        packaged = package / package_name
        packaged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, packaged)
        flash_files.append(
            {
                "role": role,
                "source": source_name,
                "path": package_name,
                "offset": offset,
                "size": source.stat().st_size,
                "sha256": wp01.sha256_file(source),
            }
        )

    group_files = []
    for source in sorted(path for path in bridge.iterdir() if path.is_file()):
        package_name = f"rp2040/rp2040-sd-bridge-firmware/{source.name}"
        packaged = package / package_name
        packaged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, packaged)
        group_files.append(
            {
                "path": package_name,
                "size": source.stat().st_size,
                "sha256": wp01.sha256_file(source),
            }
        )
    package.mkdir(parents=True, exist_ok=True)
    (package / "manifest.json").write_text(
        json.dumps(
            {
                "schema": 1,
                "package": f"d1l-release-{COMMIT}",
                "git": {"commit": COMMIT, "dirty": False},
                "workflow": {
                    "run_id": RUN_ID,
                    "run_attempt": "1",
                    "sha": COMMIT,
                    "ref": "refs/heads/codex/d1l-retained-merge",
                    "repository": "n30nex/SIGUI",
                },
                "flash_files": flash_files,
                "rp2040_artifacts": [
                    {
                        "name": "rp2040-sd-bridge-firmware",
                        "path": "rp2040/rp2040-sd-bridge-firmware",
                        "files": group_files,
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    return run_dir


def add_official_smoke_fixture(run_dir: Path) -> None:
    direct = run_dir / "rp2040-seeed-official-sd-smoke-firmware"
    direct.mkdir(parents=True, exist_ok=True)
    uf2 = direct / runner.OFFICIAL_SMOKE_UF2
    uf2.write_bytes(b"exact-official-sd-smoke-uf2")
    (direct / "SHA256SUMS.txt").write_text(
        f"{wp01.sha256_file(uf2)}  {runner.OFFICIAL_SMOKE_UF2}\n",
        encoding="ascii",
    )
    package = run_dir / "d1l-release-package" / f"d1l-release-{COMMIT}"
    files = []
    prefix = "rp2040/rp2040-seeed-official-sd-smoke-firmware"
    for source in sorted(path for path in direct.iterdir() if path.is_file()):
        package_name = f"{prefix}/{source.name}"
        packaged = package / package_name
        packaged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, packaged)
        files.append(
            {
                "path": package_name,
                "size": source.stat().st_size,
                "sha256": wp01.sha256_file(source),
            }
        )
    manifest_path = package / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["rp2040_artifacts"].append(
        {
            "name": "rp2040-seeed-official-sd-smoke-firmware",
            "path": prefix,
            "files": files,
        }
    )
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")


def write_hardware_validation_report(
    root: Path,
    run_dir: Path,
    *,
    include_official_smoke: bool = False,
) -> Path:
    ctx, actions_provenance, esp32_verification = wp01._verified_actions_inputs(
        root,
        run_dir,
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        include_official_smoke=include_official_smoke,
    )
    bridge_uf2 = run_dir / "rp2040-sd-bridge-firmware" / runner.BRIDGE_UF2
    runs = [
        {
            "schema": 1,
            "kind": "input_artifact_check",
            "mode": "check",
            "ok": True,
            "provenance": actions_provenance,
        },
        {
            "schema": 1,
            "kind": "esp32_flash",
            "mode": "hardware",
            "ok": True,
            "commit": COMMIT,
            "github_actions_run": RUN_ID,
            "port": "COM12",
            "artifact_verification": esp32_verification,
            "public_rf_tx": False,
            "formats_sd": False,
            "result": {"name": "esp32_flash", "ok": True, "returncode": 0},
        },
        {
            "schema": 1,
            "kind": "rp2040_bridge_restore",
            "mode": "hardware",
            "ok": True,
            "commit": COMMIT,
            "github_actions_run": RUN_ID,
            "port": "COM16",
            "public_rf_tx": False,
            "formats_sd": False,
            "copy": {
                "ok": True,
                "copied": True,
                "artifact": {
                    "path": str(bridge_uf2.resolve()),
                    "size": bridge_uf2.stat().st_size,
                    "sha256": wp01.sha256_file(bridge_uf2).upper(),
                },
            },
            "ping": {"ok": True, "protocol_supported": True},
        },
    ]
    report = {
        "schema": 1,
        "kind": "d1l_autonomous_hardware_validation",
        "mode": "hardware",
        "ok": False,
        "commit": COMMIT,
        "github_actions_run": RUN_ID,
        "ports": {"d1l": "COM12", "rp2040": "COM16"},
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "runs": runs,
    }
    path = root / "artifacts" / "hardware" / "com12" / (
        f"d1l_autonomous_hardware_validation_{COMMIT[:7]}.json"
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report), encoding="utf-8")
    return path


def write_provenance(root: Path) -> tuple[dict, Path, Path, Path]:
    run_dir = write_actions_fixture(root)
    validation_report = write_hardware_validation_report(root, run_dir)
    provenance = wp01.build_wp01_provenance_receipt(
        validation_report,
        root=root,
        github_run_dir=run_dir,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert provenance["ok"] is True
    path = root / f"wp01_exact_pair_provenance_{COMMIT[:7]}.json"
    path.write_text(json.dumps(provenance, sort_keys=True), encoding="utf-8")
    return provenance, path, run_dir, validation_report


def common(
    kind: str,
    provenance_sha256: str = "a" * 64,
    provenance: dict | None = None,
) -> dict:
    return {
        "schema": 1,
        "kind": kind,
        "mode": "hardware",
        "ok": True,
        "commit": COMMIT,
        "github_actions_run": RUN_ID,
        "ports": {"d1l": "COM12", "rp2040": "COM16"},
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "provenance": copy.deepcopy(provenance or {}),
        "provenance_receipt_sha256": provenance_sha256,
        "wdt_count": 0,
        "panic_count": 0,
        "brownout_count": 0,
        "terminal_timeout_count": 0,
    }


def reboot_cycle() -> dict:
    return {
        "ok": True,
        "post_reboot_reset_reason": "SW",
        "reboot_proven": True,
        "reset_scope": "system",
        "connectivity_prepare": "ESP_OK",
        "storage_manager_quiesced": True,
        "retained_worker_quiesced": True,
        "rp2040_bridge_quiesced": True,
        "retained_flush": "ESP_OK",
        "route_flush": "ESP_OK",
        "pending_dirty": False,
        "wdt": False,
        "panic": False,
        "brownout": False,
        "retained_task_stack_free_bytes": 8192,
    }


def payloads(
    provenance_sha256: str = "a" * 64,
    provenance: dict | None = None,
) -> dict[str, dict]:
    inserted = {
        **common("sd_inserted_stability", provenance_sha256, provenance),
        "duration_sec": 300,
        "sample_count": 6,
        "status_poll_count": 6,
        "card_inserted_all_samples": True,
        "final_ready_sd": True,
        "false_no_card_count": 0,
        "unintended_backend_generation_count": 0,
        "retained_failure_count": 0,
        "reset_count": 0,
    }
    remove_cycle = {
        "ok": True,
        "true_removal_detected": True,
        "nvs_fallback_active": True,
        "deterministic_remount": True,
        "generation_changed": True,
        "read_merge_before_overwrite": True,
        "all_stores_reconciled": True,
        "write_before_edge": True,
        "write_during_edge": True,
        "write_after_edge": True,
        "reset_storm": False,
        "delete_or_rename_against_replacement": False,
        "user_data_loss": False,
    }
    remove = {
        **common("sd_remove_reinsert", provenance_sha256, provenance),
        "cycle_count": 10,
        "cycles": [copy.deepcopy(remove_cycle) for _ in range(10)],
        "all_cycles_passed": True,
        "reset_storm_count": 0,
        "cross_media_mutation_count": 0,
        "data_loss_count": 0,
    }
    reboots = {
        **common("retained_reboot_matrix", provenance_sha256, provenance),
        "cycle_count": 5,
        "cycles": [reboot_cycle() for _ in range(5)],
        "all_cycles_passed": True,
        "dirty_recovery_failure_count": 0,
    }
    soak = {
        **common("storage_active_soak", provenance_sha256, provenance),
        "duration_sec": 7200,
        "segment_count": 3,
        "segments": [{"ok": True}, {"ok": True}, {"ok": True}],
        "sample_count": 6,
        "status_poll_count": 6,
        "storage_active": True,
        "dirty_event_counts": {"public": 2, "dm": 2, "routes": 2, "packets": 2},
        "controlled_reboot_count": 2,
        "controlled_reboots": [reboot_cycle(), reboot_cycle()],
        "all_segments_passed": True,
        "all_controlled_reboots_passed": True,
        "final_ready_sd": True,
        "retained_task_stack_free_bytes_floor": 8192,
        "false_no_card_count": 0,
        "unintended_backend_generation_count": 0,
        "retained_failure_count": 0,
        "command_retry_count": 0,
        "unexpected_reset_count": 0,
    }
    return {
        "sd_inserted_stability": inserted,
        "sd_remove_reinsert": remove,
        "retained_reboot_matrix": reboots,
        "storage_active_soak": soak,
    }


@pytest.mark.parametrize("kind", wp01.WP01_ARTIFACT_KINDS)
def test_wp01_artifact_validators_accept_exact_complete_evidence(tmp_path: Path, kind: str):
    paths, provenance_path, _ = write_inputs(tmp_path)
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    provenance_sha256 = wp01.sha256_file(provenance_path)
    assert wp01.wp01_artifact_ok(
        json.loads(paths[kind].read_text(encoding="utf-8")),
        kind,
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        provenance,
        provenance_sha256,
        tmp_path,
    ) is True


@pytest.mark.parametrize(
    ("kind", "mutate"),
    [
        ("sd_inserted_stability", lambda value: value.update(duration_sec=299)),
        (
            "sd_remove_reinsert",
            lambda value: value["cycles"][0].update(delete_or_rename_against_replacement=True),
        ),
        (
            "retained_reboot_matrix",
            lambda value: value["cycles"][0].update(post_reboot_reset_reason="WDT"),
        ),
        (
            "storage_active_soak",
            lambda value: value["dirty_event_counts"].update(packets=0),
        ),
    ],
)
def test_wp01_artifact_validators_fail_closed_on_missing_closure_condition(
    tmp_path: Path, kind, mutate
):
    paths, provenance_path, _ = write_inputs(tmp_path)
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    provenance_sha256 = wp01.sha256_file(provenance_path)
    payload = json.loads(paths[kind].read_text(encoding="utf-8"))
    mutate(payload)
    assert wp01.wp01_artifact_ok(
        payload,
        kind,
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        provenance,
        provenance_sha256,
        tmp_path,
    ) is False


def test_wp01_artifact_validator_rejects_stale_run_forbidden_port_and_safety_claims(
    tmp_path: Path,
):
    paths, provenance_path, _ = write_inputs(tmp_path)
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    provenance_sha256 = wp01.sha256_file(provenance_path)
    payload = json.loads(paths["sd_inserted_stability"].read_text(encoding="utf-8"))
    payload["github_actions_run"] = "29222903209"
    assert wp01.wp01_artifact_ok(
        payload,
        "sd_inserted_stability",
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        provenance,
        provenance_sha256,
        tmp_path,
    ) is False


def test_wp01_artifact_validator_rejects_absent_or_mismatched_exact_pair_provenance(
    tmp_path: Path,
):
    paths, provenance_path, run_dir = write_inputs(tmp_path)
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    provenance_sha256 = wp01.sha256_file(provenance_path)
    valid_inserted = json.loads(
        paths["sd_inserted_stability"].read_text(encoding="utf-8")
    )
    payload = copy.deepcopy(valid_inserted)
    payload.pop("provenance")
    assert wp01.wp01_artifact_ok(
        payload,
        "sd_inserted_stability",
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        provenance,
        provenance_sha256,
        tmp_path,
    ) is False

    payload = copy.deepcopy(valid_inserted)
    payload["provenance"]["rp2040_device_build_commit"] = "0" * 40
    assert wp01.wp01_artifact_ok(
        payload,
        "sd_inserted_stability",
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        provenance,
        provenance_sha256,
        tmp_path,
    ) is False

    fake_hash = copy.deepcopy(provenance)
    fake_hash["release_manifest_sha256"] = "1" * 64
    assert wp01.wp01_provenance_receipt_ok(
        fake_hash,
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        root=tmp_path,
        github_run_dir=run_dir,
    ) is False

    payload = copy.deepcopy(valid_inserted)
    payload["ports"]["d1l"] = "COM11"
    assert wp01.wp01_artifact_ok(
        payload,
        "sd_inserted_stability",
        COMMIT,
        RUN_ID,
        "COM11",
        "COM16",
        provenance,
        provenance_sha256,
        tmp_path,
    ) is False

    payload = copy.deepcopy(valid_inserted)
    payload["formats_sd"] = True
    assert wp01.wp01_artifact_ok(
        payload,
        "sd_inserted_stability",
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        provenance,
        provenance_sha256,
        tmp_path,
    ) is False


def test_wp01_provenance_recomputes_checksums_and_rejects_invalid_ports(tmp_path: Path):
    provenance, _, run_dir, _ = write_provenance(tmp_path)
    assert wp01.wp01_provenance_receipt_ok(
        provenance,
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        root=tmp_path,
        github_run_dir=run_dir,
    ) is True
    assert wp01.wp01_provenance_receipt_ok(
        provenance,
        COMMIT,
        RUN_ID,
        "",
        "COM16",
        root=tmp_path,
        github_run_dir=run_dir,
    ) is False

    bridge = run_dir / "rp2040-sd-bridge-firmware" / runner.BRIDGE_UF2
    bridge.write_bytes(b"checksum-mismatch-with-manifest")
    assert wp01.wp01_provenance_receipt_ok(
        provenance,
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        root=tmp_path,
        github_run_dir=run_dir,
    ) is False


def test_wp01_provenance_recomputes_real_two_rp2040_group_shape(tmp_path: Path):
    run_dir = write_actions_fixture(tmp_path)
    add_official_smoke_fixture(run_dir)
    validation_report = write_hardware_validation_report(
        tmp_path,
        run_dir,
        include_official_smoke=True,
    )

    provenance = wp01.build_wp01_provenance_receipt(
        validation_report,
        root=tmp_path,
        github_run_dir=run_dir,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )

    assert provenance["ok"] is True
    assert [
        group["name"] for group in provenance["artifact_provenance"]["rp2040_artifact_groups"]
    ] == [
        "rp2040-sd-bridge-firmware",
        "rp2040-seeed-official-sd-smoke-firmware",
    ]


def write_inputs(root: Path) -> tuple[dict[str, Path], Path, Path]:
    provenance, provenance_path, run_dir, _ = write_provenance(root)
    provenance_sha256 = wp01.sha256_file(provenance_path)
    paths = {}
    for kind, payload in payloads(provenance_sha256, provenance).items():
        if kind in wp01_sources.SOURCE_BOUND_KINDS:
            continue
        path = root / f"{kind}_{COMMIT[:7]}.json"
        path.write_text(json.dumps(payload), encoding="utf-8")
        paths[kind] = path
    inserted_source = write_inserted_soak(
        root / "sources" / "inserted-soak.json", commit=COMMIT
    )
    inserted = wp01_sources.build_sd_inserted_stability_artifact(
        inserted_source,
        provenance_path,
        root=root,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert inserted["ok"] is True, inserted.get("failures")
    inserted_path = root / f"sd_inserted_stability_{COMMIT[:7]}.json"
    inserted_path.write_text(json.dumps(inserted), encoding="utf-8")
    paths["sd_inserted_stability"] = inserted_path

    remove_source_payload = json.loads(
        json.dumps(valid_remove_reinsert_report()).replace(
            REMOVE_FIXTURE_COMMIT, COMMIT
        )
    )
    remove_source_payload["commit"] = COMMIT
    remove_source_payload["git"]["dirty_entries"] = []
    remove_source = root / "sources" / "remove-reinsert.json"
    remove_source.write_text(json.dumps(remove_source_payload), encoding="utf-8")
    remove = wp01_sources.build_sd_remove_reinsert_artifact(
        remove_source,
        provenance_path,
        root=root,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert remove["ok"] is True, remove.get("failures")
    remove_path = root / f"sd_remove_reinsert_{COMMIT[:7]}.json"
    remove_path.write_text(json.dumps(remove), encoding="utf-8")
    paths["sd_remove_reinsert"] = remove_path

    reboot_sources = []
    for index in range(5):
        reboot_sources.append(
            write_reboot_source(
                root / "sources" / f"reboot-{index + 1}.json",
                commit=COMMIT,
                token=f"cycle{index + 1}",
                before_nonce=100 + index,
                after_nonce=101 + index,
                crash_total=20 + index,
            )
        )
    reboots = wp01_sources.build_retained_reboot_matrix_artifact(
        reboot_sources,
        provenance_path,
        root=root,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert reboots["ok"] is True, reboots.get("failures")
    reboot_path = root / f"retained_reboot_matrix_{COMMIT[:7]}.json"
    reboot_path.write_text(json.dumps(reboots), encoding="utf-8")
    paths["retained_reboot_matrix"] = reboot_path

    active_source = write_storage_active_soak_source(
        root / "sources" / "storage-active-soak.json", commit=COMMIT
    )
    active = wp01_sources.build_storage_active_soak_artifact(
        active_source,
        provenance_path,
        root=root,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert active["ok"] is True, active.get("failures")
    active_path = root / f"storage_active_soak_{COMMIT[:7]}.json"
    active_path.write_text(json.dumps(active), encoding="utf-8")
    paths["storage_active_soak"] = active_path
    return paths, provenance_path, run_dir


def test_wp01_aggregate_hash_binds_all_four_exact_artifacts(tmp_path: Path):
    paths, provenance_path, run_dir = write_inputs(tmp_path)
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    report = wp01.build_aggregate(
        paths,
        provenance_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
        root=tmp_path,
        github_run_dir=run_dir,
    )
    hashes = {kind: wp01.sha256_file(path) for kind, path in paths.items()}
    provenance_sha256 = wp01.sha256_file(provenance_path)

    assert report["ok"] is True
    assert wp01.wp01_aggregate_artifact_ok(
        report,
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        hashes,
        provenance_sha256,
        provenance,
    ) is True

    hashes["storage_active_soak"] = "0" * 64
    assert wp01.wp01_aggregate_artifact_ok(
        report,
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        hashes,
        provenance_sha256,
        provenance,
    ) is False

    assert wp01.wp01_aggregate_artifact_ok(
        report,
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        hashes,
        "0" * 64,
        provenance,
    ) is False


def test_wp01_aggregate_preserves_invalid_input_and_fails_closed(tmp_path: Path):
    paths, provenance_path, run_dir = write_inputs(tmp_path)
    invalid = json.loads(paths["retained_reboot_matrix"].read_text(encoding="utf-8"))
    invalid["cycles"][0]["retained_worker_quiesced"] = False
    paths["retained_reboot_matrix"].write_text(json.dumps(invalid), encoding="utf-8")

    report = wp01.build_aggregate(
        paths,
        provenance_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
        root=tmp_path,
        github_run_dir=run_dir,
    )

    assert report["ok"] is False
    assert report["checks"]["retained_reboot_matrix"] is False
    assert report["failures"] == ["missing_or_invalid:retained_reboot_matrix"]


def test_release_audit_recognizes_only_hash_bound_complete_wp01_evidence(tmp_path: Path):
    paths, provenance_path, run_dir = write_inputs(tmp_path)
    report = wp01.build_aggregate(
        paths,
        provenance_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
        root=tmp_path,
        github_run_dir=run_dir,
    )
    aggregate = tmp_path / f"wp01_acceptance_{COMMIT[:7]}_COM12_COM16.json"
    aggregate.write_text(json.dumps(report), encoding="utf-8")

    gate = audit.wp01_evidence_gate(
        tmp_path, tmp_path, run_dir, COMMIT, RUN_ID, "COM12", "COM16"
    ).to_dict()
    assert gate["ok"] is True
    assert len(gate["evidence"]) == 6

    stale = json.loads(paths["sd_inserted_stability"].read_text(encoding="utf-8"))
    stale["github_actions_run"] = "29222903209"
    paths["sd_inserted_stability"].write_text(json.dumps(stale), encoding="utf-8")
    gate = audit.wp01_evidence_gate(
        tmp_path, tmp_path, run_dir, COMMIT, RUN_ID, "COM12", "COM16"
    ).to_dict()
    assert gate["ok"] is False
    assert gate["details"]["sd_inserted_stability"]["artifact_ok"] is False


def test_source_bound_inserted_artifact_rejects_raw_source_tamper(tmp_path: Path):
    paths, provenance_path, _ = write_inputs(tmp_path)
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    provenance_sha256 = wp01.sha256_file(provenance_path)
    artifact = json.loads(
        paths["sd_inserted_stability"].read_text(encoding="utf-8")
    )
    assert wp01.wp01_artifact_ok(
        artifact,
        "sd_inserted_stability",
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        provenance,
        provenance_sha256,
        tmp_path,
    ) is True

    source_path = Path(artifact["source_receipts"][0]["path"])
    source = json.loads(source_path.read_text(encoding="utf-8"))
    storage = next(
        row
        for row in source["samples"][-1]["results"]
        if row.get("cmd") == "storage status"
    )
    storage["sd"]["present"] = False
    source_path.write_text(json.dumps(source), encoding="utf-8")

    assert wp01.wp01_artifact_ok(
        artifact,
        "sd_inserted_stability",
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        provenance,
        provenance_sha256,
        tmp_path,
    ) is False


def test_source_bound_reboot_matrix_rejects_dirty_raw_and_broken_nonce_chain(
    tmp_path: Path,
):
    paths, provenance_path, _ = write_inputs(tmp_path)
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    provenance_sha256 = wp01.sha256_file(provenance_path)
    artifact = json.loads(
        paths["retained_reboot_matrix"].read_text(encoding="utf-8")
    )
    assert wp01.wp01_artifact_ok(
        artifact,
        "retained_reboot_matrix",
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        provenance,
        provenance_sha256,
        tmp_path,
    ) is True

    source_path = Path(artifact["source_receipts"][0]["path"])
    source = json.loads(source_path.read_text(encoding="utf-8"))
    dm = [row for row in source["results"] if row.get("cmd") == "messages dm"][-1]
    dm["persistence"]["sd"]["reconcile_pending"] = True
    dm["persisted"] = False
    source_path.write_text(json.dumps(source), encoding="utf-8")
    assert wp01.wp01_artifact_ok(
        artifact,
        "retained_reboot_matrix",
        COMMIT,
        RUN_ID,
        "COM12",
        "COM16",
        provenance,
        provenance_sha256,
        tmp_path,
    ) is False

    broken_sources = []
    for index in range(5):
        broken_sources.append(
            write_reboot_source(
                tmp_path / "broken" / f"cycle-{index + 1}.json",
                commit=COMMIT,
                token=f"broken{index + 1}",
                before_nonce=500 + index + (10 if index >= 3 else 0),
                after_nonce=501 + index + (10 if index >= 3 else 0),
                crash_total=50 + index,
            )
        )
    broken = wp01_sources.build_retained_reboot_matrix_artifact(
        broken_sources,
        provenance_path,
        root=tmp_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert broken["ok"] is False
    assert broken["failures"] == ["reboot nonce chain is not consecutive"]


def test_source_bound_reboot_matrix_rejects_forged_poll_attempt_counts(
    tmp_path: Path,
):
    paths, provenance_path, _ = write_inputs(tmp_path)
    canonical = json.loads(
        paths["retained_reboot_matrix"].read_text(encoding="utf-8")
    )
    source_paths = [Path(receipt["path"]) for receipt in canonical["source_receipts"]]
    first_source = source_paths[0]
    original = first_source.read_text(encoding="utf-8")

    for field, expected_failure in (
        (
            "pre_reboot_persistence_poll_attempts_used",
            "reboot source pre-reboot persistence attempt count is inconsistent",
        ),
        (
            "persistence_poll_attempts_used",
            "reboot source post-reboot persistence attempt count is inconsistent",
        ),
    ):
        source = json.loads(original)
        source[field] = 999
        first_source.write_text(json.dumps(source), encoding="utf-8")
        rebuilt = wp01_sources.build_retained_reboot_matrix_artifact(
            source_paths,
            provenance_path,
            root=tmp_path,
            commit=COMMIT,
            github_actions_run=RUN_ID,
            d1l_port="COM12",
            rp2040_port="COM16",
        )
        assert rebuilt["ok"] is False
        assert rebuilt["failures"] == [expected_failure]
        first_source.write_text(original, encoding="utf-8")


def test_source_bound_storage_active_soak_rejects_missing_canary_safety(
    tmp_path: Path,
):
    paths, provenance_path, _ = write_inputs(tmp_path)
    canonical = json.loads(paths["storage_active_soak"].read_text(encoding="utf-8"))
    source_path = Path(canonical["source_receipts"][0]["path"])
    source = json.loads(source_path.read_text(encoding="utf-8"))
    reboot = next(event for event in source["events"] if event["kind"] == "reboot")
    canary = next(
        result
        for result in reboot["report"]["results"]
        if result.get("cmd") == "storage retained-canary"
    )
    canary.pop("dm_rf_tx")
    source_path.write_text(json.dumps(source), encoding="utf-8")

    rebuilt = wp01_sources.build_storage_active_soak_artifact(
        source_path,
        provenance_path,
        root=tmp_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert rebuilt["ok"] is False
    assert rebuilt["failures"] == [
        "storage-active retained canary is unsafe or inconsistent"
    ]


def test_source_bound_storage_active_soak_accepts_generation_reset_after_reboot(
    tmp_path: Path,
):
    paths, provenance_path, _ = write_inputs(tmp_path)
    canonical = json.loads(paths["storage_active_soak"].read_text(encoding="utf-8"))
    source_path = Path(canonical["source_receipts"][0]["path"])
    source = json.loads(source_path.read_text(encoding="utf-8"))
    segments = [event for event in source["events"] if event["kind"] == "segment"]
    for sample in segments[1]["report"]["samples"]:
        packets = next(
            result for result in sample["results"] if result.get("cmd") == "packets"
        )
        packets["persistence"]["sd"]["generation"] = 7
    source_path.write_text(json.dumps(source), encoding="utf-8")

    rebuilt = wp01_sources.build_storage_active_soak_artifact(
        source_path,
        provenance_path,
        root=tmp_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )

    assert rebuilt["ok"] is True, rebuilt.get("failures")
    assert rebuilt["unintended_backend_generation_count"] == 0
    assert rebuilt["controlled_reboot_backend_generation_transition_count"] == 2


def test_source_bound_producer_cli_rebuilds_all_canonical_artifacts(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
):
    paths, provenance_path, _ = write_inputs(tmp_path)
    for kind in (
        "sd_inserted_stability",
        "retained_reboot_matrix",
        "storage_active_soak",
    ):
        canonical = json.loads(paths[kind].read_text(encoding="utf-8"))
        out = tmp_path / "produced" / f"{kind}.json"
        argv = [
            "--kind",
            kind,
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-actions-run",
            RUN_ID,
            "--d1l-port",
            "COM12",
            "--rp2040-port",
            "COM16",
            "--exact-pair-provenance",
            str(provenance_path),
            "--out",
            str(out),
        ]
        for receipt in canonical["source_receipts"]:
            argv.extend(("--source", receipt["path"]))

        assert wp01_produce.main(argv) == 0
        assert json.loads(out.read_text(encoding="utf-8")) == canonical

    summaries = [json.loads(line) for line in capsys.readouterr().out.splitlines()]
    assert [summary["kind"] for summary in summaries] == [
        "sd_inserted_stability",
        "retained_reboot_matrix",
        "storage_active_soak",
    ]
    assert all(summary["ok"] is True for summary in summaries)


def test_source_bound_artifact_rejects_provenance_outside_root(tmp_path: Path):
    paths, provenance_path, _ = write_inputs(tmp_path)
    canonical = json.loads(
        paths["sd_inserted_stability"].read_text(encoding="utf-8")
    )
    source_path = Path(canonical["source_receipts"][0]["path"])
    outside_dir = tmp_path.parent / f"{tmp_path.name}-outside"
    outside_dir.mkdir()
    outside_provenance = outside_dir / provenance_path.name
    shutil.copy2(provenance_path, outside_provenance)

    escaped = wp01_sources.build_sd_inserted_stability_artifact(
        source_path,
        outside_provenance,
        root=tmp_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert escaped["ok"] is False
    assert escaped["failures"] == [
        "exact-pair provenance is outside repository root"
    ]


def test_inserted_source_rejects_degraded_backends_and_failed_commands(
    tmp_path: Path,
):
    paths, provenance_path, _ = write_inputs(tmp_path)
    canonical = json.loads(
        paths["sd_inserted_stability"].read_text(encoding="utf-8")
    )
    source_path = Path(canonical["source_receipts"][0]["path"])
    source = json.loads(source_path.read_text(encoding="utf-8"))
    for sample in source["samples"]:
        for row in sample["results"]:
            if row.get("cmd") == "storage status":
                row["retained_sd"]["degraded"] = True
                row["retained_sd"]["backup_degraded"] = True
                for name in ("messages", "dm", "routes", "packets"):
                    row["stores"][name] = "nvs"
            elif row.get("cmd") == "packets":
                row["ok"] = False
    source_path.write_text(json.dumps(source), encoding="utf-8")

    rebuilt = wp01_sources.build_sd_inserted_stability_artifact(
        source_path,
        provenance_path,
        root=tmp_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert rebuilt["ok"] is False
    assert rebuilt["failures"] == [
        "inserted-card soak sample command cohort failed"
    ]


@pytest.mark.parametrize("tamper", ["identity", "preflight_order"])
def test_reboot_source_rejects_identity_or_late_preflight(
    tmp_path: Path, tamper: str
):
    paths, provenance_path, _ = write_inputs(tmp_path)
    canonical = json.loads(paths["retained_reboot_matrix"].read_text(encoding="utf-8"))
    source_path = Path(canonical["source_receipts"][0]["path"])
    source = json.loads(source_path.read_text(encoding="utf-8"))
    if tamper == "identity":
        source["expected_firmware_commit"] = "0" * 40
        source["pre_device_build_commit"] = "0" * 40
        source["device_build_commit"] = "0" * 40
        source["pre_firmware_identity_ok"] = False
        source["post_firmware_identity_ok"] = False
        source["firmware_identity_ok"] = False
    else:
        preflight = list(zip(source["commands"][:2], source["results"][:2]))
        remaining = list(zip(source["commands"][2:], source["results"][2:]))
        reboot_index = next(
            index for index, (command, _result) in enumerate(remaining) if command == "reboot"
        )
        reordered = [
            *remaining[:reboot_index],
            *preflight,
            *remaining[reboot_index:],
        ]
        source["commands"] = [command for command, _result in reordered]
        source["results"] = [result for _command, result in reordered]
    source_path.write_text(json.dumps(source), encoding="utf-8")

    rebuilt = wp01_sources.build_retained_reboot_matrix_artifact(
        [Path(receipt["path"]) for receipt in canonical["source_receipts"]],
        provenance_path,
        root=tmp_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert rebuilt["ok"] is False
    assert rebuilt["cycle_count"] == 0


def test_reboot_matrix_rejects_discontinuous_crashlog_chain(tmp_path: Path):
    _paths, provenance_path, _ = write_inputs(tmp_path)
    crash_totals = (20, 100, 50, 200, 1)
    sources = [
        write_reboot_source(
            tmp_path / "disjoint" / f"cycle-{index + 1}.json",
            commit=COMMIT,
            token=f"disjoint{index + 1}",
            before_nonce=700 + index,
            after_nonce=701 + index,
            crash_total=crash_total,
        )
        for index, crash_total in enumerate(crash_totals)
    ]
    rebuilt = wp01_sources.build_retained_reboot_matrix_artifact(
        sources,
        provenance_path,
        root=tmp_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert rebuilt["ok"] is False
    assert rebuilt["failures"] == ["reboot crashlog chain is not consecutive"]


@pytest.mark.parametrize(
    "tamper", ["intermediate_crash", "flat_elapsed", "infinite_elapsed"]
)
def test_inserted_source_rejects_hidden_crash_or_zero_observed_span(
    tmp_path: Path, tamper: str
):
    paths, provenance_path, _ = write_inputs(tmp_path)
    canonical = json.loads(
        paths["sd_inserted_stability"].read_text(encoding="utf-8")
    )
    source_path = Path(canonical["source_receipts"][0]["path"])
    source = json.loads(source_path.read_text(encoding="utf-8"))
    if tamper == "flat_elapsed":
        for sample in source["samples"]:
            sample["elapsed_sec"] = 300.0
    elif tamper == "infinite_elapsed":
        source["samples"][-1]["elapsed_sec"] = float("inf")
    else:
        crashlog = next(
            row
            for row in source["samples"][3]["results"]
            if row.get("cmd") == "crashlog"
        )
        crashlog["total_written"] = 2
        crashlog["entries"].append(
            {"seq": 2, "reset_reason": "WDT_TASK", "crash_like": True}
        )
    source_path.write_text(json.dumps(source), encoding="utf-8")

    rebuilt = wp01_sources.build_sd_inserted_stability_artifact(
        source_path,
        provenance_path,
        root=tmp_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert rebuilt["ok"] is False


def test_reboot_matrix_rejects_failed_health_boundary(tmp_path: Path):
    paths, provenance_path, _ = write_inputs(tmp_path)
    canonical = json.loads(paths["retained_reboot_matrix"].read_text(encoding="utf-8"))
    source_path = Path(canonical["source_receipts"][0]["path"])
    source = json.loads(source_path.read_text(encoding="utf-8"))
    for row in source["results"]:
        if row.get("cmd") == "health":
            row["ok"] = False
    source_path.write_text(json.dumps(source), encoding="utf-8")

    rebuilt = wp01_sources.build_retained_reboot_matrix_artifact(
        [Path(receipt["path"]) for receipt in canonical["source_receipts"]],
        provenance_path,
        root=tmp_path,
        commit=COMMIT,
        github_actions_run=RUN_ID,
        d1l_port="COM12",
        rp2040_port="COM16",
    )
    assert rebuilt["ok"] is False
    assert rebuilt["failures"] == ["reboot source health boundary is missing"]


def test_source_bound_producer_refuses_to_overwrite_raw_source(tmp_path: Path):
    paths, provenance_path, _ = write_inputs(tmp_path)
    canonical = json.loads(
        paths["sd_inserted_stability"].read_text(encoding="utf-8")
    )
    source_path = Path(canonical["source_receipts"][0]["path"])
    source_sha256 = wp01.sha256_file(source_path)
    with pytest.raises(
        SystemExit, match="output must not overwrite a source or provenance receipt"
    ):
        wp01_produce.main(
            [
                "--kind",
                "sd_inserted_stability",
                "--root",
                str(tmp_path),
                "--commit",
                COMMIT,
                "--github-actions-run",
                RUN_ID,
                "--d1l-port",
                "COM12",
                "--rp2040-port",
                "COM16",
                "--exact-pair-provenance",
                str(provenance_path),
                "--source",
                str(source_path),
                "--out",
                str(source_path),
            ]
        )
    assert wp01.sha256_file(source_path) == source_sha256
