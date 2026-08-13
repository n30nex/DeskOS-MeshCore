import copy
import hashlib
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

from scripts import (
    compare_release_reproducibility_d1l,
    meshcore_conformance_d1l as conformance,
    meshcore_signed_advert_runtime_d1l as signed_runtime,
    package_release_d1l,
    sbom_d1l,
)
from scripts import verify_arduino_build_inputs
from tests.meshcore_conformance_fixture import completed_report


COMMIT = "a" * 40
ROOT = Path(__file__).resolve().parents[1]


def source_identity(commit: str = COMMIT) -> dict:
    return {
        "commit": commit,
        "created": "2026-07-13T18:01:56Z",
        "repository": sbom_d1l.PROJECT_REPOSITORY,
        "submodules": [
            {
                "path": "third_party/MeshCore",
                "url": "https://github.com/meshcore-dev/MeshCore.git",
                "commit": "1" * 40,
                "license": "MIT",
            },
            {
                "path": "third_party/sensecap_indicator_esp32",
                "url": "https://github.com/Seeed-Solution/sensecap_indicator_esp32.git",
                "commit": "2" * 40,
                "license": "Apache-2.0",
            },
        ],
    }


def write_source_tree(root: Path) -> None:
    for relative in sbom_d1l.REQUIRED_SOURCE_INPUTS:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"fixture for {relative}\n", encoding="utf-8")

    for relative in (
        "docs/USER_GUIDE_D1L.md",
        "docs/DESKOS_MESHCORE_FEATURE_PARITY.md",
        "docs/KNOWN_LIMITATIONS.md",
        "docs/D1L_SD_CARD_GUIDED_INSTALL.md",
        "docs/ADMIN_REMOTE_CLI_ALLOWLIST.md",
        "docs/ATTRIBUTIONS.md",
        "docs/RC1_SCOPE.md",
        "docs/RC2_SCOPE.md",
    ):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"fixture for {relative}\n", encoding="utf-8")

    (root / "main" / "d1l_config.h").write_text(
        '#define D1L_FIRMWARE_NAME "MeshCore DeskOS D1L"\n'
        '#define D1L_FIRMWARE_VERSION "1.0.0-test"\n',
        encoding="ascii",
    )
    requirements = root / "requirements" / "ci-host-windows.txt"
    requirements.parent.mkdir(parents=True, exist_ok=True)
    requirements.write_text("pytest==8.4.1\nruff==0.12.2\n", encoding="ascii")

    digest = "1" * 64
    committed_inputs = json.loads(
        (ROOT / ".github" / "d1l-build-inputs.json").read_text(encoding="utf-8")
    )
    build_inputs = {
        "schema": 1,
        "kind": "d1l_build_inputs",
        "esp_idf": {
            "version": "v5.5.4",
            "container": {
                "reference": f"docker.io/espressif/idf@sha256:{digest}",
                "index_digest": f"sha256:{digest}",
            },
        },
        "host_python": {
            "version": "3.13.6",
            "architecture": "x64",
            "requirements": {
                "path": "requirements/ci-host-windows.txt",
                "sha256": sbom_d1l.sha256_file(requirements),
            },
        },
        "ci_tools": committed_inputs["ci_tools"],
        "arduino": committed_inputs["arduino"],
        "submodules": {
            "third_party/MeshCore": "1" * 40,
            "third_party/sensecap_indicator_esp32": "2" * 40,
        },
    }
    build_lock = root / ".github" / "d1l-build-inputs.json"
    build_lock.parent.mkdir(parents=True, exist_ok=True)
    build_lock.write_text(
        json.dumps(build_inputs, indent=2, sort_keys=True) + "\n", encoding="ascii"
    )
    ledger = {
        "schema_version": 1,
        "snapshot_at": "2026-07-13T23:13:07Z",
        "release_posture": "not_ready_to_tag",
        "repository": {"main": {"commit": COMMIT}},
        "capabilities": [
            {
                "id": "public_messages",
                "runtime_available": True,
                "documentation_status": "hardware_proven",
            }
        ],
        "blockers": [],
        "work_packages": [
            {
                "id": "WP-03",
                "title": "Reproducible release inputs",
                "status": "in_progress",
                "required_evidence": [
                    "build_inputs_<sha>.json",
                    "sbom_<sha>.spdx.json",
                    "provenance_<sha>.json",
                ],
                "evidence": [],
            }
        ],
    }
    ledger_path = root / package_release_d1l.COMPLETION_LEDGER_SOURCE
    ledger_path.parent.mkdir(parents=True, exist_ok=True)
    ledger_path.write_text(
        json.dumps(ledger, indent=2, sort_keys=True) + "\n", encoding="ascii"
    )


def deterministic_build_payload(role: str, source_payload: bytes) -> bytes:
    identity = hashlib.sha256(
        role.encode("ascii") + b"\0" + COMMIT.encode("ascii") + b"\0" + source_payload
    ).digest()
    return b"D1L-DETERMINISTIC\0" + role.encode("ascii") + b"\0" + identity


def write_build(build: Path, app_source: bytes = b"app-source-v1") -> None:
    (build / "bootloader").mkdir(parents=True)
    (build / "partition_table").mkdir(parents=True)
    (build / "bootloader" / "bootloader.bin").write_bytes(
        deterministic_build_payload("bootloader", b"boot-source-v1")
    )
    (build / "partition_table" / "partition-table.bin").write_bytes(
        deterministic_build_payload("partition-table", b"partition-source-v1")
    )
    (build / "meshcore_deskos_d1l.bin").write_bytes(
        deterministic_build_payload("app", app_source)
    )
    (build / "meshcore_deskos_d1l.elf").write_bytes(
        deterministic_build_payload("elf", app_source)
    )
    (build / "meshcore_deskos_d1l.map").write_bytes(
        deterministic_build_payload("map", app_source)
    )
    (build / "flasher_args.json").write_text(
        json.dumps(
            {
                "flash_settings": {
                    "flash_mode": "dio",
                    "flash_size": "8MB",
                    "flash_freq": "80m",
                },
                "flash_files": {
                    "0x0": "bootloader/bootloader.bin",
                    "0x10000": "meshcore_deskos_d1l.bin",
                    "0x8000": "partition_table/partition-table.bin",
                },
            }
        ),
        encoding="ascii",
    )


def write_conformance(
    root: Path,
    run_index: int,
    commit: str = COMMIT,
    *,
    completed_runs: int = 100000,
) -> Path:
    path = root / f"meshcore_conformance_{commit}.json"
    generated_at = datetime.now(timezone.utc) - timedelta(seconds=run_index)
    temporary = f"/tmp/d1l-meshcore-conformance-run-{run_index}"
    report = completed_report(
        commit,
        root / package_release_d1l.BUILD_INPUTS_SOURCE,
        generated_at=generated_at,
    )
    source_root = str(conformance.ROOT).replace("\\", "/")
    actions_root = f"/actions/run-{run_index}/checkout"
    planned_commands = conformance.command_plan(
        "clang-18",
        "clang++-18",
        temporary,
    )
    report["commands"] = [
        [
            (
                actions_root + normalized[len(source_root) :]
                if normalized == source_root
                or normalized.startswith(source_root + "/")
                else normalized
            )
            for argument in command
            for normalized in [str(argument).replace("\\", "/")]
        ]
        for command in planned_commands
    ]
    report["fuzz_command"] = [
                    f"{temporary}/meshcore_wire_fuzz",
                    "-runs=100000",
                    "-seed=13746277",
                    f"-artifact_prefix=/tmp/findings-run-{run_index}/",
                    f"{temporary}/corpus",
                ]
    report["fuzz_result"]["completed_runs"] = completed_runs
    report["fuzz_result"]["passed"] = completed_runs == 100000
    report["fuzz_result"]["duration_ms"] = 1000 + run_index
    report["fuzz_result"]["artifact_prefix"] = f"/tmp/findings-run-{run_index}"
    path.write_text(json.dumps(report, sort_keys=True), encoding="ascii")
    return path


def write_signed_advert_runtime(
    root: Path,
    run_index: int,
    commit: str = COMMIT,
) -> Path:
    manifest = signed_runtime.load_manifest()
    dependency = manifest["external_dependency"]
    repository_files = {
        path: {
            "expected_sha256": digest,
            "actual_sha256": digest,
            "matched": True,
        }
        for group in signed_runtime._source_groups(manifest)
        for path, digest in group.items()
    }
    report = {
        "schema_version": 1,
        "artifact_type": signed_runtime.SIGNED_ADVERT_ARTIFACT_TYPE,
        "status": "pass",
        "passed": True,
        "execution_complete": True,
        "generated_at": (
            datetime.now(timezone.utc) - timedelta(seconds=run_index)
        ).isoformat(),
        "work_package": "WP-04",
        "capability": manifest["capability"],
        "coverage_boundary": manifest["coverage_boundary"],
        "wp04_closure_eligible": False,
        "closure_ready": False,
        "repository": {
            "verified": True,
            "repository_commit": commit,
            "expected_repository_commit": commit,
            "upstream_commit": manifest["upstream"]["commit"],
            "gitlink_commit": manifest["upstream"]["commit"],
            "source_hash_mode": manifest["source_hash_mode"],
            "files": repository_files,
        },
        "external_archive": {
            "verified": True,
            "source": dependency["archive_url"],
            "url": dependency["archive_url"],
            "size": dependency["archive_size"],
            "sha256": dependency["archive_sha256"],
            "version": dependency["version"],
            "registry_version_id": dependency["registry_version_id"],
            "release_commit": dependency["release_commit"],
        },
        "external_sources": {
            "verified": True,
            "files": {
                path: {"sha256": digest, "size": 1}
                for path, digest in dependency["sources"].items()
            },
        },
        "sanitizers_enabled": True,
        "sanitizer_policy": manifest["sanitizer_policy"],
        "full_ubsan_clean": True,
        "commands": signed_runtime.command_plan(
            "clang-18",
            "clang++-18",
            f"/tmp/signed-advert-crypto-{run_index}",
            f"/tmp/signed-advert-build-{run_index}",
            sanitize=True,
        ),
        "result": manifest["expected_result"],
        "assertions": manifest["assertions"],
        "residual_gaps": manifest["residual_gaps"],
    }
    path = root / f"meshcore_signed_advert_runtime_{commit}.json"
    path.write_text(json.dumps(report, sort_keys=True), encoding="ascii")
    return path


def write_rp2040_artifacts(root: Path, run_index: int) -> Path:
    artifact_root = root / "artifacts" / f"rp2040-build-{run_index}"
    for name in compare_release_reproducibility_d1l.RP2040_ARTIFACT_NAMES:
        artifact_dir = artifact_root / name
        artifact_dir.mkdir(parents=True)
        uf2_name = (
            "deskos_sd_bridge.ino.uf2"
            if name == "rp2040-sd-bridge-firmware"
            else f"{name}.uf2"
        )
        uf2 = artifact_dir / uf2_name
        uf2.write_bytes(
            deterministic_build_payload(f"rp2040:{name}", b"rp2040-source-v1")
        )
        if name == "rp2040-sd-bridge-firmware":
            metadata_path = root / package_release_d1l.BUILD_INPUTS_SOURCE
            metadata = verify_arduino_build_inputs.load_metadata(metadata_path)
            validated = verify_arduino_build_inputs.validate_metadata(
                metadata, root, verify_repository=False
            )
            build_inputs = artifact_dir / "build-inputs.json"
            build_inputs.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "kind": "d1l_arduino_build_inputs",
                        "ok": True,
                        "source_commit": COMMIT,
                        "metadata": {
                            "path": package_release_d1l.BUILD_INPUTS_SOURCE.as_posix(),
                            "sha256": sbom_d1l.sha256_file(metadata_path),
                        },
                        "arduino_cli_version": validated["cli_version"],
                        "arduino_cli": {
                            "version": validated["cli_version"],
                            "archive": validated["cli_archive"],
                            "executable": {
                                **validated["cli_executable"],
                                "verified": True,
                            },
                            "bytes_verified": True,
                        },
                        "arduino_cli_bytes_verified": True,
                        "rp2040_core_version": validated["core_version"],
                        "submodules": validated["submodules"],
                        "archives_verified": True,
                        "archives": [
                            {
                                **item,
                                "relative_path": f"staging/{item['filename']}",
                            }
                            for item in validated["archive_inventory"]
                        ],
                    },
                    indent=2,
                    sort_keys=True,
                )
                + "\n",
                encoding="ascii",
            )
        rows = [
            f"{sbom_d1l.sha256_file(path)}  ./{path.name}"
            for path in sorted(artifact_dir.iterdir())
            if path.is_file() and path.name != "SHA256SUMS.txt"
        ]
        (artifact_dir / "SHA256SUMS.txt").write_text(
            "\n".join(rows) + "\n", encoding="ascii"
        )
    return artifact_root


def clean_git_info(commit: str = COMMIT) -> dict:
    return {
        "commit": commit,
        "short_commit": commit[:7],
        "branch": "main",
        "dirty": False,
        "dirty_entries": [],
        "source_patches": [],
    }


def rewrite_package_checksums(package_dir: Path) -> None:
    package_release_d1l.write_sha256sums(package_dir)


def write_fixture_signed_update_bundle(
    root: Path,
    package_dir: Path,
    update_image: dict,
    source_commit: str,
    version: str,
    security_sequence: int,
    _signing_key: Path,
) -> dict:
    image_path = package_dir / update_image["path"]
    update_dir = image_path.parent
    manifest_path = update_dir / "d1l-update.manifest"
    signature_path = update_dir / "d1l-update.sig"
    manifest_path.write_text(
        (
            f"{package_release_d1l.UPDATE_MANIFEST_HEADER}\n"
            f"product={package_release_d1l.UPDATE_PRODUCT}\n"
            f"target={package_release_d1l.UPDATE_TARGET}\n"
            f"version={version}\n"
            f"source_sha={source_commit}\n"
            "partition_table_sha256="
            f"{package_release_d1l.sha256_file(root / 'partitions_d1l.csv')}\n"
            f"image_sha256={package_release_d1l.sha256_file(image_path)}\n"
            f"image_size={image_path.stat().st_size}\n"
            f"security_sequence={security_sequence}\n"
            f"signer_key_id={package_release_d1l.UPDATE_SIGNER_KEY_ID}\n"
        ),
        encoding="ascii",
        newline="\n",
    )
    signature_path.write_bytes(hashlib.sha512(manifest_path.read_bytes()).digest())
    return {
        "schema": 1,
        "signed": True,
        "product": package_release_d1l.UPDATE_PRODUCT,
        "target": package_release_d1l.UPDATE_TARGET,
        "version": version,
        "source_commit": source_commit,
        "security_sequence": security_sequence,
        "signer_key_id": package_release_d1l.UPDATE_SIGNER_KEY_ID,
        "partition_table_sha256": package_release_d1l.sha256_file(
            root / "partitions_d1l.csv"
        ),
        "image": {
            **update_image,
            "sd_destination": "updates/d1l-update.bin",
        },
        "manifest": {
            "path": manifest_path.relative_to(package_dir).as_posix(),
            "sd_destination": "updates/d1l-update.manifest",
            "size": manifest_path.stat().st_size,
            "sha256": package_release_d1l.sha256_file(manifest_path),
        },
        "signature": {
            "path": signature_path.relative_to(package_dir).as_posix(),
            "sd_destination": "updates/d1l-update.sig",
            "size": signature_path.stat().st_size,
            "sha256": package_release_d1l.sha256_file(signature_path),
            "algorithm": "Ed25519",
        },
        "trigger_policy": "local_ui_or_usb_confirmation_only",
        "rf_trigger_allowed": False,
        "partition_table_replacement_in_firmware": False,
        "rollback_required": True,
    }


def build_pair(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    *,
    run_ids: tuple[str, str] = ("1001", "1002"),
    app_payloads: tuple[bytes, bytes] = (b"APP", b"APP"),
    second_package_name: str | None = None,
    include_rp2040: bool = True,
    conformance_completed_runs: tuple[int, int] = (100000, 100000),
) -> tuple[Path, Path, Path, dict]:
    root = tmp_path / "source"
    root.mkdir()
    write_source_tree(root)
    identity = source_identity()
    monkeypatch.setattr(package_release_d1l, "git_info", lambda _root: clean_git_info())
    monkeypatch.setattr(
        package_release_d1l,
        "discover_source_identity",
        lambda _root, _expected: identity,
    )
    monkeypatch.setenv("GITHUB_SHA", COMMIT)
    monkeypatch.setenv("GITHUB_REPOSITORY", "n30nex/DeskOS-MeshCore")
    monkeypatch.setenv("GITHUB_WORKFLOW", "d1l-ci")
    monkeypatch.setenv("GITHUB_REF", "refs/heads/main")
    monkeypatch.setenv("GITHUB_RUN_ATTEMPT", "1")
    monkeypatch.setenv("GITHUB_SERVER_URL", "https://github.com")

    # This suite exercises the internal evidence-rich reproducibility shape.
    # Customer production-package surface enforcement is covered separately;
    # keep this fixture out of that path so its comparator evidence remains
    # available without changing what production packages contain.
    monkeypatch.setattr(
        package_release_d1l,
        "PRODUCTION_RELEASE_PROFILES",
        frozenset({package_release_d1l.CORE_RELEASE_PROFILE}),
    )
    monkeypatch.setattr(
        sbom_d1l,
        "PRODUCTION_RELEASE_PROFILES",
        frozenset({"core_1_0"}),
    )

    signing_key = root / "test-update-signing-key.pem"
    signing_key.write_text("fixture key; signing is covered by release CI\n", encoding="ascii")
    monkeypatch.setattr(
        package_release_d1l,
        "write_signed_update_bundle",
        write_fixture_signed_update_bundle,
    )

    packages = []
    for index, (run_id, app_payload) in enumerate(
        zip(run_ids, app_payloads, strict=True), start=1
    ):
        build = root / f"build-{index}"
        write_build(build, app_payload)
        conformance = write_conformance(
            root, index, completed_runs=conformance_completed_runs[index - 1]
        )
        signed_advert = write_signed_advert_runtime(root, index)
        rp2040_artifacts = (
            write_rp2040_artifacts(root, index) if include_rp2040 else None
        )
        monkeypatch.setenv("GITHUB_RUN_ID", run_id)
        package_name = (
            second_package_name
            if index == 2 and second_package_name is not None
            else f"d1l-release-{COMMIT}"
        )
        package_release_d1l.create_release_package(
            root=root,
            build_dir=build,
            out_dir=root / "artifacts" / f"run-{index}",
            package_name=package_name,
            full_size=0x20000,
            rp2040_artifact_root=rp2040_artifacts,
            meshcore_conformance_json=conformance,
            meshcore_signed_advert_runtime_json=signed_advert,
            update_signing_key=signing_key,
        )
        packages.append(root / "artifacts" / f"run-{index}" / package_name)
    return root, packages[0], packages[1], identity


def compare(root: Path, first: Path, second: Path, identity: dict) -> dict:
    return compare_release_reproducibility_d1l.compare_release_packages(
        root,
        first,
        second,
        COMMIT,
        source_identity=identity,
        enforce_clean_source=False,
    )


def test_two_distinct_actions_packages_are_reproducible_and_receipt_is_deterministic(
    tmp_path, monkeypatch
):
    root, first, second, identity = build_pair(tmp_path, monkeypatch)

    receipt = compare(root, first, second, identity)
    repeated = compare(root, first, second, identity)

    assert receipt == repeated
    assert receipt["status"] == "pass"
    assert receipt["reproducible"] is True
    assert receipt["comparison_profile"] == (
        compare_release_reproducibility_d1l.PROFILE_FULL_RELEASE
    )
    assert receipt["failures"] == []
    assert all(receipt["comparison"].values())
    assert (
        receipt["packages"][0]["manifest_sha256"]
        != receipt["packages"][1]["manifest_sha256"]
    )
    assert (
        receipt["packages"][0]["checksum_manifest_sha256"]
        != receipt["packages"][1]["checksum_manifest_sha256"]
    )
    assert (
        receipt["packages"][0]["stable_payload_fingerprint"]
        == receipt["packages"][1]["stable_payload_fingerprint"]
    )
    observed = set(
        receipt["permitted_nonreproducible_metadata"]["observed_manifest_differences"]
    )
    assert {
        "/meshcore_conformance/run_receipt/expires_at",
        "/meshcore_conformance/run_receipt/generated_at",
        "/meshcore_conformance/run_receipt/sha256",
        "/meshcore_signed_advert_runtime/run_receipt/expires_at",
        "/meshcore_signed_advert_runtime/run_receipt/generated_at",
        "/meshcore_signed_advert_runtime/run_receipt/sha256",
        "/source_build_dir",
        "/workflow/run_id",
        "/workflow/run_url",
    } <= observed
    assert observed <= set(compare_release_reproducibility_d1l.PERMITTED_MANIFEST_PATHS)
    first_evidence = json.loads(
        (first / f"evidence/meshcore_conformance_{COMMIT}.json").read_text(
            encoding="ascii"
        )
    )
    second_evidence = json.loads(
        (second / f"evidence/meshcore_conformance_{COMMIT}.json").read_text(
            encoding="ascii"
        )
    )
    assert first_evidence == second_evidence
    assert "generated_at" not in first_evidence
    assert "duration_ms" not in first_evidence["fuzz_result"]
    assert "artifact_prefix" not in first_evidence["fuzz_result"]
    assert "run-1" not in json.dumps(first_evidence)
    assert "run-2" not in json.dumps(second_evidence)
    first_signed_evidence = json.loads(
        (
            first
            / f"evidence/meshcore_signed_advert_runtime_{COMMIT}.json"
        ).read_text(encoding="ascii")
    )
    second_signed_evidence = json.loads(
        (
            second
            / f"evidence/meshcore_signed_advert_runtime_{COMMIT}.json"
        ).read_text(encoding="ascii")
    )
    assert first_signed_evidence == second_signed_evidence
    assert "generated_at" not in first_signed_evidence
    assert "commands" not in first_signed_evidence
    serialized = compare_release_reproducibility_d1l.canonical_json(receipt)
    assert str(root) not in serialized
    assert serialized == compare_release_reproducibility_d1l.canonical_json(repeated)


def test_full_release_profile_rejects_packages_without_rp2040_roots(
    tmp_path, monkeypatch
):
    root, first, second, identity = build_pair(
        tmp_path, monkeypatch, include_rp2040=False
    )

    receipt = compare(root, first, second, identity)

    assert receipt["reproducible"] is False
    assert receipt["comparison_profile"] == (
        compare_release_reproducibility_d1l.PROFILE_FULL_RELEASE
    )
    assert receipt["packages"] == [
        {"label": "first", "valid": False, "error_code": "missing_required_payload"},
        {"label": "second", "valid": False, "error_code": "missing_required_payload"},
    ]
    assert all(
        "rp2040/" in item["detail"]
        for item in receipt["failures"]
        if item["code"] == "missing_required_payload"
    )


def test_full_release_profile_rejects_rechecksummed_unverified_arduino_cli_bytes(
    tmp_path, monkeypatch
):
    root, first, _second, _identity = build_pair(tmp_path, monkeypatch)
    receipt_path = (
        first / "rp2040" / "rp2040-sd-bridge-firmware" / "build-inputs.json"
    )
    receipt = json.loads(receipt_path.read_text(encoding="ascii"))
    receipt["arduino_cli"]["executable"]["sha256"] = "0" * 64
    receipt_path.write_text(json.dumps(receipt, sort_keys=True), encoding="ascii")
    package_release_d1l.write_sha256sums(receipt_path.parent)
    manifest = json.loads((first / "manifest.json").read_text(encoding="ascii"))

    with pytest.raises(
        compare_release_reproducibility_d1l.ComparisonError,
        match="RP2040 build-input receipt is incomplete or stale",
    ):
        compare_release_reproducibility_d1l.validate_rp2040_profile(
            root,
            manifest,
            first,
            COMMIT,
            compare_release_reproducibility_d1l.PROFILE_FULL_RELEASE,
        )


def test_semantic_conformance_drift_is_not_hidden_by_volatile_normalization(
    tmp_path, monkeypatch
):
    with pytest.raises(ValueError, match="fuzz_passed, fuzz_complete"):
        build_pair(
            tmp_path,
            monkeypatch,
            conformance_completed_runs=(100000, 99999),
        )


def test_signed_advert_semantic_drift_is_rejected_after_rebinding_metadata(
    tmp_path, monkeypatch
):
    root, first, _second, _identity = build_pair(tmp_path, monkeypatch)
    manifest = json.loads((first / "manifest.json").read_text(encoding="ascii"))
    evidence = (
        first / f"evidence/meshcore_signed_advert_runtime_{COMMIT}.json"
    )
    report = json.loads(evidence.read_text(encoding="ascii"))
    report["result"]["contact_name"] = "tampered-contact"
    evidence.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
    )
    metadata = manifest["meshcore_signed_advert_runtime"]
    metadata["size"] = evidence.stat().st_size
    metadata["sha256"] = sbom_d1l.sha256_file(evidence)

    with pytest.raises(
        compare_release_reproducibility_d1l.ComparisonError
    ) as failure:
        compare_release_reproducibility_d1l.validate_manifest_shape(
            root,
            manifest,
            first,
            COMMIT,
            compare_release_reproducibility_d1l.PROFILE_FULL_RELEASE,
        )

    assert failure.value.code == "invalid_signed_advert_semantics"


def test_substituted_signed_runtime_binding_hash_is_rejected_after_rebinding_metadata(
    tmp_path, monkeypatch
):
    root, first, _second, _identity = build_pair(tmp_path, monkeypatch)
    manifest = json.loads((first / "manifest.json").read_text(encoding="ascii"))
    evidence = first / f"evidence/meshcore_conformance_{COMMIT}.json"
    report = json.loads(evidence.read_text(encoding="ascii"))
    report["signed_advert_runtime"]["canonical_sha256"] = "0" * 64
    evidence.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
    )
    metadata = manifest["meshcore_conformance"]
    metadata["size"] = evidence.stat().st_size
    metadata["sha256"] = sbom_d1l.sha256_file(evidence)

    with pytest.raises(
        compare_release_reproducibility_d1l.ComparisonError
    ) as failure:
        compare_release_reproducibility_d1l.validate_manifest_shape(
            root,
            manifest,
            first,
            COMMIT,
            compare_release_reproducibility_d1l.PROFILE_FULL_RELEASE,
        )

    assert failure.value.code == "invalid_conformance_semantics"


def test_signed_advert_evidence_is_a_required_exact_sha_payload(
    tmp_path, monkeypatch
):
    root, first, _second, _identity = build_pair(tmp_path, monkeypatch)
    manifest = json.loads((first / "manifest.json").read_text(encoding="ascii"))
    manifest.pop("meshcore_signed_advert_runtime")

    with pytest.raises(
        compare_release_reproducibility_d1l.ComparisonError
    ) as failure:
        compare_release_reproducibility_d1l.validate_manifest_shape(
            root,
            manifest,
            first,
            COMMIT,
            compare_release_reproducibility_d1l.PROFILE_FULL_RELEASE,
        )

    assert failure.value.code == "missing_required_payload"


def test_same_actions_run_is_rejected(tmp_path, monkeypatch):
    root, first, second, identity = build_pair(
        tmp_path, monkeypatch, run_ids=("1001", "1001")
    )

    receipt = compare(root, first, second, identity)

    assert receipt["reproducible"] is False
    assert receipt["comparison"]["distinct_actions_runs"] is False
    assert {item["code"] for item in receipt["failures"]} == {"same_actions_run"}


def test_payload_digest_mismatch_is_rejected(tmp_path, monkeypatch):
    root, first, second, identity = build_pair(
        tmp_path, monkeypatch, app_payloads=(b"APP-A", b"APP-B")
    )

    receipt = compare(root, first, second, identity)

    codes = {item["code"] for item in receipt["failures"]}
    assert receipt["reproducible"] is False
    assert receipt["comparison"]["payload_sha256_match"] is False
    assert "payload_sha256_mismatch" in codes
    assert "firmware/meshcore_deskos_d1l.bin" in next(
        item["detail"]
        for item in receipt["failures"]
        if item["code"] == "payload_sha256_mismatch"
    )


def test_expected_source_sha_mismatch_is_rejected(tmp_path, monkeypatch):
    root, first, second, identity = build_pair(tmp_path, monkeypatch)

    receipt = compare_release_reproducibility_d1l.compare_release_packages(
        root,
        first,
        second,
        "b" * 40,
        source_identity=identity,
        enforce_clean_source=False,
    )

    assert receipt["reproducible"] is False
    assert receipt["failures"] == [
        {
            "code": "source_sha_mismatch",
            "detail": "supplied source identity does not match expected SHA",
        }
    ]


def test_changed_source_input_invalidates_both_packages(tmp_path, monkeypatch):
    root, first, second, identity = build_pair(tmp_path, monkeypatch)
    (root / ".github" / "workflows" / "d1l-ci.yml").write_text(
        "changed after package generation\n", encoding="ascii"
    )

    receipt = compare(root, first, second, identity)

    assert receipt["reproducible"] is False
    assert receipt["packages"] == [
        {"label": "first", "valid": False, "error_code": "invalid_sbom"},
        {"label": "second", "valid": False, "error_code": "invalid_sbom"},
    ]
    assert {item["code"] for item in receipt["failures"]} == {"invalid_sbom"}


@pytest.mark.parametrize(
    ("contract_name", "mutation"),
    [
        ("build_inputs", "missing"),
        ("capability_manifest", "stale_filename"),
        ("release_evidence_index", "mismatched_sha"),
    ],
)
def test_exact_sha_package_metadata_contract_is_fail_closed(
    tmp_path, monkeypatch, contract_name, mutation
):
    root, first, second, identity = build_pair(tmp_path, monkeypatch)
    manifest_path = first / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="ascii"))
    metadata = manifest[contract_name]
    target = first / metadata["path"]

    if mutation == "missing":
        target.unlink()
    elif mutation == "stale_filename":
        stale_name = package_release_d1l.package_metadata_filename(
            contract_name, "b" * 40
        )
        target.rename(first / stale_name)
        metadata["path"] = stale_name
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="ascii")
    else:
        payload = json.loads(target.read_text(encoding="ascii"))
        payload["source_commit"] = "b" * 40
        target.write_text(
            package_release_d1l.canonical_json(payload), encoding="ascii"
        )
        metadata["size"] = target.stat().st_size
        metadata["sha256"] = package_release_d1l.sha256_file(target)
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="ascii")
    rewrite_package_checksums(first)

    receipt = compare(root, first, second, identity)

    assert receipt["reproducible"] is False
    assert receipt["packages"][0] == {
        "label": "first",
        "valid": False,
        "error_code": (
            "missing_required_payload"
            if mutation != "mismatched_sha"
            else "invalid_package_metadata"
        ),
    }


def test_invalid_toolchain_lock_is_rejected(tmp_path, monkeypatch):
    root, first, _second, _identity = build_pair(tmp_path, monkeypatch)
    statement = json.loads(
        (first / f"provenance_{COMMIT}.json").read_text(encoding="ascii")
    )
    materials, _subjects = compare_release_reproducibility_d1l.descriptor_maps(
        statement
    )
    modified_materials = copy.deepcopy(materials)
    lock = root / ".github" / "d1l-build-inputs.json"
    lock_data = json.loads(lock.read_text(encoding="ascii"))
    lock_data["esp_idf"]["container"]["reference"] = "docker.io/espressif/idf:mutable"
    lock.write_text(
        json.dumps(lock_data, indent=2, sort_keys=True) + "\n", encoding="ascii"
    )
    modified_materials[".github/d1l-build-inputs.json"]["digest"] = {
        "sha256": sbom_d1l.sha256_file(lock)
    }

    with pytest.raises(
        compare_release_reproducibility_d1l.ComparisonError,
        match="ESP-IDF container digest is not immutable",
    ) as error:
        compare_release_reproducibility_d1l.validate_toolchain_lock(
            root, modified_materials
        )

    assert error.value.code == "invalid_toolchain_lock"


def test_wrong_package_identity_is_rejected(tmp_path, monkeypatch):
    root, first, second, identity = build_pair(
        tmp_path, monkeypatch, second_package_name="d1l-release-not-the-source-sha"
    )

    receipt = compare(root, first, second, identity)

    assert receipt["reproducible"] is False
    assert receipt["packages"][1] == {
        "label": "second",
        "valid": False,
        "error_code": "package_identity_mismatch",
    }
    assert any(
        item["code"] == "package_identity_mismatch" for item in receipt["failures"]
    )


def test_production_wrapper_rejects_dirty_source_before_reading_packages(
    tmp_path, monkeypatch
):
    monkeypatch.setattr(
        compare_release_reproducibility_d1l,
        "git_info",
        lambda _root: {"commit": COMMIT, "dirty": True},
    )

    receipt = compare_release_reproducibility_d1l.compare_release_packages(
        tmp_path,
        tmp_path / "missing-first",
        tmp_path / "missing-second",
        COMMIT,
    )

    assert receipt["reproducible"] is False
    assert receipt["failures"] == [
        {
            "code": "dirty_source",
            "detail": "comparison requires a clean source worktree",
        }
    ]
