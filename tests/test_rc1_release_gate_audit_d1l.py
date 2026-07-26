import hashlib
import io
import json
import zipfile
from pathlib import Path

import pytest

from scripts import capture_core_actions_run_d1l as actions_capture
from scripts import rc1_release_gate_audit_d1l as audit


COMMIT = "a" * 40
RUN = "123456789"
ATTEMPT = "1"
APP_BYTES = b"exact rc1 application image"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: dict) -> None:
    path.write_text(
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="ascii",
    )


def write_checksums(package: Path) -> None:
    checksum_path = package / "SHA256SUMS.txt"
    checksum_path.unlink(missing_ok=True)
    rows = []
    for path in sorted(item for item in package.rglob("*") if item.is_file()):
        relative = path.relative_to(package).as_posix()
        rows.append(f"{sha256(path)}  ./{relative}")
    checksum_path.write_text("\n".join(rows) + "\n", encoding="ascii")


def write_sd_preparation(package: Path) -> dict:
    payloads = {
        audit.SD_PREPARATION_SCRIPT: b"#!/usr/bin/env python3\n",
        "sdcard/offline-tile-provider.example.json": b'{"schema":1}\n',
        "sdcard/deskos/manifest.json": b'{"schema":1}\n',
        "sdcard/deskos/README.txt": b"DeskOS SD fixture\n",
        "sdcard/deskos/map/manifest.json": b'{"schema":1}\n',
        "sdcard/deskos/map/tiles/README.txt": b"Tile fixture\n",
    }
    rows = []
    for relative, payload in payloads.items():
        path = package / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        rows.append(
            {
                "path": relative,
                "size": path.stat().st_size,
                "sha256": sha256(path),
            }
        )
    return {
        "schema": 1,
        "script": audit.SD_PREPARATION_SCRIPT,
        "bundle_root": audit.SD_BUNDLE_ROOT,
        "minimum_card_bytes": audit.SD_MINIMUM_CARD_BYTES,
        "filesystem": "FAT32",
        "formats_sd": False,
        "files": rows,
    }


def valid_manifest(
    app_sha: str,
    app_size: int,
    *,
    package_name: str,
    sd_preparation: dict,
) -> dict:
    return {
        "schema": 2,
        "project": audit.PROJECT,
        "package": package_name,
        "release_profile": audit.RELEASE_PROFILE,
        "sd_history_mode": audit.SD_HISTORY_MODE,
        "sd_history_state": audit.SD_HISTORY_STATE,
        "storage_authority": audit.STORAGE_AUTHORITY,
        "supported_capabilities": ["sd_history"],
        "unavailable_capabilities": [],
        "sd_preparation": sd_preparation,
        "firmware_commit": COMMIT,
        "actions_run": RUN,
        "actions_run_attempt": ATTEMPT,
        "git": {
            "commit": COMMIT,
            "dirty": False,
            "dirty_entries": [],
        },
        "workflow": {
            "sha": COMMIT,
            "run_id": RUN,
            "run_attempt": ATTEMPT,
            "repository": audit.REPOSITORY,
        },
        "flash_files": [
            {
                "role": "app",
                "source": audit.APP_NAME,
                "path": f"firmware/{audit.APP_NAME}",
                "size": app_size,
                "sha256": app_sha,
            }
        ],
        "install_recovery_guide": {
            "normal_install_targets": {
                "posix": {
                    "requested_path": audit.PI_SERIAL_PATH,
                    "target_kind": "posix_by_id",
                    "vid": int(audit.USB_VID, 16),
                    "pid": int(audit.USB_PID, 16),
                }
            },
            "target_policy": {
                "stable_requested_path_only": True,
                "resolved_tty_observational_only": True,
                "hardware_identity_required": True,
                "raw_posix_tty_forbidden": True,
            },
            "normal_install_preserves_unrelated_nvs": True,
            "normal_install_checksum_verified": True,
            "no_on_device_sd_format": True,
        },
    }


def zip_bytes(files: dict[str, bytes]) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, payload in files.items():
            archive.writestr(name, payload)
    return output.getvalue()


def write_actions_capture(
    root: Path,
    package: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> Path:
    scope = {
        "schema": 1,
        "kind": "d1l_candidate_scope",
        "source_commit": COMMIT,
        "workflow_run_id": RUN,
        "workflow_run_attempt": ATTEMPT,
        "repository": audit.REPOSITORY,
        "workflow": "d1l-ci",
        "event": "push",
        "include_sd_bridge": True,
        "scope_reason": "rc1_candidate",
        "release_profile": audit.RELEASE_PROFILE,
        "sd_history_mode": audit.SD_HISTORY_MODE,
    }
    archives: dict[int, bytes] = {}
    artifact_rows = []
    for artifact_id, name in enumerate(
        actions_capture.EXPECTED_ACTIONS_ARTIFACTS, 1
    ):
        if name == "d1l-host-artifacts":
            files = {
                "build-inputs/d1l-candidate-scope.json": (
                    json.dumps(scope, sort_keys=True).encode("ascii")
                )
            }
        elif name == "d1l-release-package":
            files = {
                (
                    package.name
                    + "/"
                    + path.relative_to(package).as_posix()
                ): path.read_bytes()
                for path in package.rglob("*")
                if path.is_file()
            }
        else:
            files = {f"{name}.txt": name.encode("ascii")}
        archive = zip_bytes(files)
        archives[artifact_id] = archive
        artifact_rows.append(
            {
                "id": artifact_id,
                "name": name,
                "expired": False,
                "size_in_bytes": len(archive),
                "digest": "sha256:" + hashlib.sha256(archive).hexdigest(),
                "workflow_run": {
                    "id": int(RUN),
                    "head_sha": COMMIT,
                    "head_branch": "main",
                },
            }
        )
    run_payload = {
        "id": int(RUN),
        "status": "completed",
        "conclusion": "success",
        "head_sha": COMMIT,
        "head_branch": "main",
        "event": "push",
        "path": ".github/workflows/d1l-ci.yml",
        "name": "d1l-ci",
        "run_attempt": int(ATTEMPT),
        "repository": {"full_name": audit.REPOSITORY},
    }
    artifacts_payload = {
        "total_count": len(artifact_rows),
        "artifacts": artifact_rows,
    }

    def fake_api(_root: Path, endpoint: str) -> bytes:
        if endpoint.endswith(f"/actions/runs/{RUN}"):
            return json.dumps(run_payload).encode("utf-8")
        if endpoint.endswith(
            f"/actions/runs/{RUN}/artifacts?per_page=100"
        ):
            return json.dumps(artifacts_payload).encode("utf-8")
        artifact_id = int(endpoint.split("/")[-2])
        return archives[artifact_id]

    monkeypatch.setattr(actions_capture, "_api", fake_api)
    monkeypatch.setattr(
        actions_capture,
        "git_metadata",
        lambda _root: {
            "commit": COMMIT,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    run_dir = root / "artifacts" / "github" / RUN
    return actions_capture.capture(
        root=root,
        run_id=RUN,
        commit=COMMIT,
        out_dir=run_dir / "core-actions-run-metadata",
        github_run_dir=run_dir,
    )


def valid_receipt(package: Path) -> dict:
    manifest = json.loads(
        (package / "manifest.json").read_text(encoding="ascii")
    )
    app = manifest["flash_files"][0]
    return {
        "schema": audit.RECEIPT_SCHEMA,
        "kind": audit.RECEIPT_KIND,
        "mode": "physical",
        "simulated": False,
        "dry_run": False,
        "candidate": {
            "firmware_commit": COMMIT,
            "actions_run": RUN,
            "actions_run_attempt": ATTEMPT,
            "manifest_sha256": sha256(package / "manifest.json"),
            "checksum_manifest_sha256": sha256(
                package / "SHA256SUMS.txt"
            ),
            "app_path": app["path"],
            "app_sha256": app["sha256"],
        },
        "target": {
            "host": audit.PI_HOST,
            "path": audit.PI_SERIAL_PATH,
            "vid": audit.USB_VID,
            "pid": audit.USB_PID,
        },
        "flash": {
            "performed": True,
            "method": "project_write_flash",
            "erase_flash": False,
            "non_erasing": True,
            "formats_sd": False,
            "settings_preserved": True,
            "artifact_app_sha256": app["sha256"],
            "written_app_sha256": app["sha256"],
        },
        "bounded_gate": {
            "bounded": True,
            "soak_required": False,
            "duration_requirement_seconds": None,
        },
        "outcomes": {
            "boot": True,
            "ui_navigation": True,
            "boot_advert": True,
            "public_send_count": 1,
            "dm_ack": True,
            "path": True,
            "trace": True,
            "ping": True,
            "repeater_login": True,
            "repeater_query": True,
            "wifi_reconnect": True,
            "sd_write": True,
            "sd_remount": True,
            "sd_degraded_notice": True,
            "authorized_map_download": True,
            "map_cache_revisit": True,
            "no_panic": True,
            "no_unexpected_reset": True,
        },
    }


def write_physical_evidence(receipt_path: Path) -> Path:
    receipt = json.loads(receipt_path.read_text(encoding="ascii"))
    sources = {}
    for index, (role, kind) in enumerate(
        audit.PHYSICAL_SOURCE_KINDS.items(), 1
    ):
        relative = f"physical-sources/{role}.json"
        path = receipt_path.parent / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "schema": 1,
            "kind": kind,
            "role": role,
            "sequence": index,
            "ok": True,
            "mode": "hardware",
            "physical_observed": True,
            "dry_run": False,
            "simulated": False,
            "simulation": False,
            "source_inspection": False,
            "manual_only": False,
        }
        if role == "ui":
            payload["manual_touch"] = False
        elif role == "rf":
            payload.update(
                {
                    "mode": "rf-full-acceptance",
                    "dry_run": False,
                    "simulated": False,
                    "simulation": False,
                    "source_inspection": False,
                }
            )
        elif role in {"protocol", "map"}:
            payload.update(
                {
                    "dry_run": False,
                    "simulated": False,
                    "manual_only": False,
                }
            )
        elif role == "wifi":
            payload["truth"] = {
                "physical_observed": True,
                "simulated": False,
                "dry_run": False,
                "source_inspection": False,
            }
        elif role in {"sd", "sd_degraded"}:
            payload["events"] = [{"event": role, "observed": True}]
            if role == "sd_degraded":
                payload.update(
                    {
                        "port": audit.PI_SERIAL_PATH,
                        "expected_firmware_commit": COMMIT,
                        "cycles": [
                            {
                                "absent": {
                                    "mode": "live_only_no_card",
                                    "degraded_notice_visible": True,
                                }
                            }
                        ],
                    }
                )
        write_json(
            path,
            payload,
        )
        sources[role] = {
            "path": relative,
            "sha256": sha256(path),
            "kind": kind,
        }
    evidence_path = receipt_path.with_name(
        receipt_path.stem + ".evidence.json"
    )
    write_json(
        evidence_path,
        {
            "schema": 1,
            "kind": audit.PHYSICAL_EVIDENCE_KIND,
            "receipt": {
                "path": receipt_path.name,
                "sha256": sha256(receipt_path),
            },
            "candidate": receipt["candidate"],
            "sources": sources,
            "coverage": audit.PHYSICAL_EVIDENCE_COVERAGE,
        },
    )
    return evidence_path


def release_fixture(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> tuple[Path, Path, Path, Path]:
    monkeypatch.setattr(
        audit,
        "sd_reboot_remount_artifact_ok",
        lambda _data, _port, _commit: True,
    )
    monkeypatch.setattr(
        audit,
        "validate_sd_remove_reinsert_report",
        lambda _data: True,
    )
    package = tmp_path / f"d1l-release-{COMMIT}"
    firmware = package / "firmware"
    firmware.mkdir(parents=True)
    app = firmware / audit.APP_NAME
    app.write_bytes(APP_BYTES)
    sd_preparation = write_sd_preparation(package)
    write_json(
        package / "manifest.json",
        valid_manifest(
            sha256(app),
            app.stat().st_size,
            package_name=package.name,
            sd_preparation=sd_preparation,
        ),
    )
    write_checksums(package)
    actions_receipt = write_actions_capture(tmp_path, package, monkeypatch)
    receipt_path = tmp_path / "physical-receipt.json"
    write_json(receipt_path, valid_receipt(package))
    evidence_path = write_physical_evidence(receipt_path)
    return package, actions_receipt, receipt_path, evidence_path


def test_exact_package_and_single_bounded_receipt_are_release_ready(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    package, actions_receipt, receipt, evidence = release_fixture(
        tmp_path, monkeypatch
    )

    first = audit.audit(
        package,
        actions_receipt,
        receipt,
        evidence,
        repository_root=tmp_path,
    )
    second = audit.audit(
        package,
        actions_receipt,
        receipt,
        evidence,
        repository_root=tmp_path,
    )

    assert first == second
    assert first["ready_for_public_release"] is True
    assert first["failures"] == []
    assert all(first["checks"].values())
    assert first["identity"] == {
        "firmware_commit": COMMIT,
        "actions_run": RUN,
        "actions_run_attempt": ATTEMPT,
        "actions_capture_receipt_sha256": sha256(actions_receipt),
        "physical_evidence_sha256": sha256(evidence),
        "manifest_sha256": sha256(package / "manifest.json"),
        "checksum_manifest_sha256": sha256(
            package / "SHA256SUMS.txt"
        ),
        "app_path": f"firmware/{audit.APP_NAME}",
        "app_sha256": sha256(package / "firmware" / audit.APP_NAME),
    }


@pytest.mark.parametrize(
    ("section", "field", "bad_value", "failed_check"),
    [
        (
            "target",
            "path",
            "/dev/ttyUSB2",
            "stable_pi_path_and_vid_pid",
        ),
        ("target", "vid", "ffff", "stable_pi_path_and_vid_pid"),
        ("flash", "erase_flash", True, "non_erasing_exact_app_flash"),
        (
            "flash",
            "formats_sd",
            True,
            "formats_sd_false_and_settings_preserved",
        ),
        (
            "flash",
            "settings_preserved",
            False,
            "formats_sd_false_and_settings_preserved",
        ),
        (
            "bounded_gate",
            "soak_required",
            True,
            "bounded_gate_without_soak_or_duration_requirement",
        ),
        (
            "bounded_gate",
            "duration_requirement_seconds",
            60,
            "bounded_gate_without_soak_or_duration_requirement",
        ),
        ("outcomes", "dm_ack", False, "dm_ack"),
        (
            "outcomes",
            "public_send_count",
            2,
            "boot_advert_and_one_public_send",
        ),
        (
            "outcomes",
            "repeater_query",
            False,
            "repeater_login_and_query",
        ),
        (
            "outcomes",
            "map_cache_revisit",
            False,
            "authorized_map_download_and_cache_revisit",
        ),
        (
            "outcomes",
            "no_unexpected_reset",
            False,
            "no_panic_or_reset_regression",
        ),
    ],
)
def test_each_physical_safety_or_outcome_requirement_fails_closed(
    tmp_path: Path,
    section: str,
    field: str,
    bad_value: object,
    failed_check: str,
    monkeypatch: pytest.MonkeyPatch,
):
    package, actions_receipt, receipt_path, evidence = release_fixture(
        tmp_path, monkeypatch
    )
    receipt = json.loads(receipt_path.read_text(encoding="ascii"))
    receipt[section][field] = bad_value
    write_json(receipt_path, receipt)

    report = audit.audit(
        package,
        actions_receipt,
        receipt_path,
        evidence,
        repository_root=tmp_path,
    )

    assert report["ready_for_public_release"] is False
    assert report["checks"][failed_check] is False
    assert failed_check in report["failures"]


def test_transplanted_receipt_and_corrupt_package_both_fail_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    package, actions_receipt, receipt_path, evidence = release_fixture(
        tmp_path, monkeypatch
    )
    receipt = json.loads(receipt_path.read_text(encoding="ascii"))
    receipt["candidate"]["actions_run_attempt"] = "2"
    write_json(receipt_path, receipt)
    (package / "firmware" / audit.APP_NAME).write_bytes(b"tampered")

    report = audit.audit(
        package,
        actions_receipt,
        receipt_path,
        evidence,
        repository_root=tmp_path,
    )

    assert report["ready_for_public_release"] is False
    assert report["checks"]["package_checksum_tree_and_manifest"] is False
    assert report["checks"]["package_exact_app_artifact"] is False
    assert report["checks"]["receipt_exact_package_binding"] is False


def test_missing_actions_capture_fails_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    package, _actions_receipt, receipt, evidence = release_fixture(
        tmp_path, monkeypatch
    )

    report = audit.audit(
        package,
        tmp_path / "missing-actions-receipt.json",
        receipt,
        evidence,
        repository_root=tmp_path,
    )

    assert report["ready_for_public_release"] is False
    assert (
        report["checks"][
            "actions_successful_main_push_exact_eight_artifacts_and_package"
        ]
        is False
    )


def test_non_sd_primary_manifest_fails_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    package, actions_receipt, receipt, _evidence = release_fixture(
        tmp_path, monkeypatch
    )
    manifest_path = package / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="ascii"))
    manifest["storage_authority"] = "nvs"
    write_json(manifest_path, manifest)
    write_checksums(package)
    write_json(receipt, valid_receipt(package))
    evidence = write_physical_evidence(receipt)

    report = audit.audit(
        package,
        actions_receipt,
        receipt,
        evidence,
        repository_root=tmp_path,
    )

    assert report["ready_for_public_release"] is False
    assert (
        report["checks"]["package_sd_primary_truth_and_preparation"] is False
    )


def test_manual_physical_source_sidecar_fails_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    package, actions_receipt, receipt, evidence = release_fixture(
        tmp_path, monkeypatch
    )
    sidecar = json.loads(evidence.read_text(encoding="ascii"))
    source_path = tmp_path / sidecar["sources"]["ui"]["path"]
    source = json.loads(source_path.read_text(encoding="ascii"))
    source["manual"] = True
    write_json(source_path, source)
    sidecar["sources"]["ui"]["sha256"] = sha256(source_path)
    write_json(evidence, sidecar)

    report = audit.audit(
        package,
        actions_receipt,
        receipt,
        evidence,
        repository_root=tmp_path,
    )

    assert report["ready_for_public_release"] is False
    assert (
        report["checks"]["physical_evidence_sidecar_machine_sources"] is False
    )


def test_cli_writes_the_same_canonical_report_and_returns_gate_status(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    monkeypatch: pytest.MonkeyPatch,
):
    package, actions_receipt, receipt, evidence = release_fixture(
        tmp_path, monkeypatch
    )
    output = tmp_path / "audit.json"

    exit_code = audit.main(
        [
            "--root",
            str(tmp_path),
            "--package-dir",
            str(package),
            "--actions-receipt",
            str(actions_receipt),
            "--physical-receipt",
            str(receipt),
            "--physical-evidence",
            str(evidence),
            "--output",
            str(output),
        ]
    )

    assert exit_code == 0
    assert output.read_text(encoding="ascii") == capsys.readouterr().out
    assert (
        json.loads(output.read_text(encoding="ascii"))[
            "ready_for_public_release"
        ]
        is True
    )
