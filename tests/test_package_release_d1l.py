import json
import os
import subprocess
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

from scripts import meshcore_signed_advert_runtime_d1l as signed_runtime
from scripts import package_release_d1l
from scripts.verify_checksums import verify_sha256_manifest
from tests.meshcore_conformance_fixture import completed_report


ROOT = Path(__file__).resolve().parents[1]


def run_git(cwd: Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args],
        cwd=cwd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    ).stdout


def create_exact_bsp_patch_fixture(root: Path) -> Path:
    submodule = root / package_release_d1l.EXPECTED_BSP_SUBMODULE
    submodule.mkdir(parents=True)
    run_git(submodule, "init")
    run_git(submodule, "config", "user.email", "tests@example.invalid")
    run_git(submodule, "config", "user.name", "D1L Tests")
    run_git(submodule, "config", "core.autocrlf", "false")
    tracked = {
        "touch.c": (b"touch base\n", b"touch patched\n"),
        "compat.c": (b"compat base\n", b"compat patched\n"),
        "origin.c": (b"origin base\n", b"origin patched\n"),
    }
    for name, (base, _patched) in tracked.items():
        (submodule / name).write_bytes(base)
    run_git(submodule, "add", ".")
    run_git(submodule, "commit", "-m", "base")

    patch_dir = root / "patches"
    patch_dir.mkdir(parents=True)
    for relative_patch, (name, (base, patched)) in zip(
        package_release_d1l.EXPECTED_BSP_PATCHES,
        tracked.items(),
    ):
        (submodule / name).write_bytes(patched)
        patch_text = run_git(
            submodule,
            "diff",
            "--binary",
            "--no-ext-diff",
            "--src-prefix=a/",
            "--dst-prefix=b/",
            "--",
            name,
        )
        (submodule / name).write_bytes(base)
        (root / relative_patch).write_text(patch_text, encoding="utf-8")

    run_git(root, "init")
    run_git(root, "config", "user.email", "tests@example.invalid")
    run_git(root, "config", "user.name", "D1L Tests")
    run_git(root, "config", "core.autocrlf", "false")
    run_git(root, "add", package_release_d1l.EXPECTED_BSP_SUBMODULE.as_posix(), "patches")
    run_git(root, "commit", "-m", "root")
    for relative_patch in package_release_d1l.EXPECTED_BSP_PATCHES:
        run_git(
            submodule,
            "apply",
            "--unidiff-zero",
            "--ignore-space-change",
            str(root / relative_patch),
        )
    return submodule


def write_fake_build(build: Path) -> None:
    (build / "bootloader").mkdir(parents=True)
    (build / "partition_table").mkdir(parents=True)
    (build / "bootloader" / "bootloader.bin").write_bytes(b"BOOT")
    (build / "partition_table" / "partition-table.bin").write_bytes(b"PART")
    (build / "meshcore_deskos_d1l.bin").write_bytes(b"APP")
    (build / "meshcore_deskos_d1l.elf").write_bytes(b"ELF")
    (build / "meshcore_deskos_d1l.map").write_text("MAP", encoding="ascii")
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


def write_fake_notices(root: Path) -> None:
    (root / "docs").mkdir(exist_ok=True)
    (root / "LICENSE").write_text("project license\n", encoding="ascii")
    (root / "THIRD_PARTY_NOTICES.md").write_text("third party notices\n", encoding="ascii")
    (root / "docs" / "ATTRIBUTIONS.md").write_text("attributions\n", encoding="ascii")
    (root / "docs" / "SOURCE_AUDIT_AND_ATTRIBUTION.md").write_text("source audit\n", encoding="ascii")
    (root / "docs" / "USER_GUIDE_D1L.md").write_text("user guide\n", encoding="ascii")
    (root / "docs" / "DEVELOPER_GUIDE_D1L.md").write_text("developer guide\n", encoding="ascii")
    (root / "docs" / "FLASH_RECOVERY_D1L.md").write_text("flash recovery\n", encoding="ascii")
    (root / "docs" / "RP2040_SD_BRIDGE_FLASH_D1L.md").write_text("rp2040 guide\n", encoding="ascii")
    overlay = root / "overlays" / "meshcore_ed25519_defined"
    overlay.mkdir(parents=True, exist_ok=True)
    (overlay / "license.txt").write_text("orlp zlib license\n", encoding="ascii")
    source_inputs = {
        ".gitmodules": "[submodule \"MeshCore\"]\n\tpath = third_party/MeshCore\n\turl = https://github.com/meshcore-dev/MeshCore.git\n",
        ".github/workflows/d1l-ci.yml": "name: d1l-ci\n",
        "CMakeLists.txt": "cmake_minimum_required(VERSION 3.16)\n",
        "dependencies.lock": "dependencies: []\n",
        "docs/BUILD_PROVENANCE_D1L.md": "# fixture build type\n",
        "main/CMakeLists.txt": "idf_component_register(SRCS main.c)\n",
        "overlays/meshcore_ed25519_defined/README.md": "# overlay fixture\n",
        "overlays/meshcore_ed25519_defined/fe.c": "/* altered fe fixture */\n",
        "overlays/meshcore_ed25519_defined/ge.c": "/* altered ge fixture */\n",
        "overlays/meshcore_ed25519_defined/sc.c": "/* altered sc fixture */\n",
        "partitions_d1l.csv": "nvs,data,nvs,0x9000,0x6000\n",
        "patches/sensecap_indicator_idf55_compat.patch": "compat patch\n",
        "patches/sensecap_indicator_touch_fix.patch": "touch patch\n",
        "patches/sensecap_indicator_tx_origin.patch": "origin patch\n",
        "scripts/compare_release_reproducibility_d1l.py": "# comparator fixture\n",
        "scripts/meshcore_conformance_d1l.py": "# conformance fixture\n",
        "scripts/meshcore_signed_advert_runtime_d1l.py": "# signed runtime fixture\n",
        "scripts/d1l_serial_target.py": (
            ROOT / "scripts" / "d1l_serial_target.py"
        ).read_text(encoding="ascii"),
        "scripts/package_release_d1l.py": "# package fixture\n",
        "scripts/provenance_d1l.py": "# provenance fixture\n",
        "scripts/sbom_d1l.py": "# sbom fixture\n",
        "scripts/verify_arduino_build_inputs.py": "# Arduino input verifier fixture\n",
        "scripts/verify_ci_tool_inputs.py": "# CI tool input verifier fixture\n",
        "scripts/validate_ed25519_defined_overlay.py": "# overlay validator fixture\n",
        "tests/ed25519_defined_overlay/driver.c": "/* overlay driver fixture */\n",
        "tests/meshcore_oracle/manifest.json": "{}\n",
        "tests/meshcore_signed_advert_runtime/manifest.json": "{}\n",
        "sdkconfig.defaults": "CONFIG_IDF_TARGET=\"esp32s3\"\n",
    }
    for relative, contents in source_inputs.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="ascii")
    requirements = root / package_release_d1l.HOST_REQUIREMENTS_SOURCE
    requirements.parent.mkdir(parents=True, exist_ok=True)
    requirements.write_text("pytest==8.4.1\nruff==0.12.2\n", encoding="ascii")
    build_inputs = {
        "schema": 1,
        "kind": "d1l_build_inputs",
        "ci_tools": json.loads(
            (ROOT / ".github" / "d1l-build-inputs.json").read_text(encoding="utf-8")
        )["ci_tools"],
        "host_python": {
            "requirements": {
                "path": package_release_d1l.HOST_REQUIREMENTS_SOURCE.as_posix(),
                "sha256": package_release_d1l.sha256_file(requirements),
            }
        },
    }
    build_inputs_path = root / package_release_d1l.BUILD_INPUTS_SOURCE
    build_inputs_path.parent.mkdir(parents=True, exist_ok=True)
    build_inputs_path.write_text(
        json.dumps(build_inputs, indent=2, sort_keys=True) + "\n", encoding="ascii"
    )
    ledger = {
        "schema_version": 1,
        "snapshot_at": "2026-07-13T23:13:07Z",
        "release_posture": "not_ready_to_tag",
        "repository": {"main": {"commit": "a" * 40}},
        "capabilities": [
            {
                "id": "public_messages",
                "runtime_available": True,
                "documentation_status": "hardware_proven",
            },
            {
                "id": "authenticated_remote_admin",
                "runtime_available": False,
                "documentation_status": "not_started",
            },
        ],
        "blockers": [],
        "work_packages": [
            {
                "id": "WP-03",
                "title": "Reproducible release inputs",
                "status": "in_progress",
                "required_evidence": ["build_inputs_<sha>.json"],
                "evidence": [],
            }
        ],
    }
    ledger_path = root / package_release_d1l.COMPLETION_LEDGER_SOURCE
    ledger_path.parent.mkdir(parents=True, exist_ok=True)
    ledger_path.write_text(
        json.dumps(ledger, indent=2, sort_keys=True) + "\n", encoding="ascii"
    )


def fake_source_identity(commit: str) -> dict:
    return {
        "commit": commit,
        "created": "2026-07-13T18:01:56Z",
        "repository": "https://github.com/n30nex/SIGUI",
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


def install_fake_source_identity(monkeypatch, commit: str) -> None:
    monkeypatch.setenv("GITHUB_SHA", commit)
    monkeypatch.setattr(
        package_release_d1l,
        "discover_source_identity",
        lambda _root, _expected_commit: fake_source_identity(commit),
    )


def write_fake_config(root: Path) -> None:
    (root / "main").mkdir(exist_ok=True)
    (root / "main" / "d1l_config.h").write_text(
        '#define D1L_FIRMWARE_NAME "MeshCore DeskOS D1L"\n'
        '#define D1L_FIRMWARE_VERSION "1.0.0-rc1"\n',
        encoding="ascii",
    )


def write_fake_rp2040_artifacts(root: Path, *, include_manifests: bool = True) -> Path:
    artifacts = root / "artifacts" / "rp2040-release-inputs"
    for name, payload in {
        "rp2040-sd-bridge-firmware": b"BRIDGE",
        "rp2040-sd-smoke-firmware": b"SMOKE",
        "rp2040-seeed-official-sd-smoke-firmware": b"OFFICIAL",
    }.items():
        artifact_dir = artifacts / name
        artifact_dir.mkdir(parents=True)
        uf2 = artifact_dir / f"{name}.uf2"
        uf2.write_bytes(payload)
        if include_manifests:
            (artifact_dir / "SHA256SUMS.txt").write_text(
                f"{package_release_d1l.sha256_file(uf2)}  ./{uf2.name}\n",
                encoding="ascii",
            )
    return artifacts


def write_meshcore_conformance(
    root: Path,
    commit: str,
    *,
    generated_at: datetime | None = None,
    **overrides: object,
) -> Path:
    payload = completed_report(
        commit,
        root / package_release_d1l.BUILD_INPUTS_SOURCE,
        generated_at=generated_at,
    )
    payload.update(overrides)
    path = root / f"meshcore_conformance_{commit}.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def write_meshcore_signed_advert_runtime(
    root: Path,
    commit: str,
    *,
    generated_at: datetime | None = None,
    **overrides: object,
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
    payload = {
        "schema_version": 1,
        "artifact_type": signed_runtime.SIGNED_ADVERT_ARTIFACT_TYPE,
        "status": "pass",
        "passed": True,
        "execution_complete": True,
        "generated_at": (generated_at or datetime.now(timezone.utc)).isoformat(),
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
            "clang-18", "clang++-18", "/tmp/crypto", "/tmp/build", sanitize=True
        ),
        "result": manifest["expected_result"],
        "assertions": manifest["assertions"],
        "residual_gaps": manifest["residual_gaps"],
    }
    payload.update(overrides)
    path = root / f"meshcore_signed_advert_runtime_{commit}.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


def test_full_feature_package_requires_signing_key_at_callable_boundary(
    tmp_path,
) -> None:
    with pytest.raises(ValueError, match="requires an update signing key"):
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=tmp_path / "build",
            out_dir=tmp_path / "release",
            package_name="unsigned-full-feature",
            full_size=0x20000,
            release_profile=package_release_d1l.FULL_FEATURE_RELEASE_PROFILE,
        )


def test_update_security_sequence_is_exact_source_commit_epoch() -> None:
    identity = fake_source_identity("a" * 40)
    assert package_release_d1l.source_security_sequence(identity) == 1783965716


def test_release_package_contains_flash_set_update_and_full_image(tmp_path, monkeypatch):
    root = tmp_path
    build = root / "build"
    out = root / "artifacts" / "release"
    write_fake_build(build)
    (build / "ota_data_initial.bin").write_bytes(b"OTA-DATA")
    flasher_args_path = build / "flasher_args.json"
    flasher_args = json.loads(flasher_args_path.read_text(encoding="ascii"))
    flasher_args["flash_files"]["0xf000"] = "ota_data_initial.bin"
    flasher_args_path.write_text(json.dumps(flasher_args), encoding="ascii")
    write_fake_notices(root)
    write_fake_config(root)
    rp2040_artifacts = write_fake_rp2040_artifacts(root)
    commit = "a" * 40
    conformance = write_meshcore_conformance(root, commit)
    signed_advert = write_meshcore_signed_advert_runtime(root, commit)
    monkeypatch.setenv("GITHUB_SHA", commit)
    install_fake_source_identity(monkeypatch, commit)
    signed_advert = write_meshcore_signed_advert_runtime(tmp_path, commit)

    manifest = package_release_d1l.create_release_package(
        root=root,
        build_dir=build,
        out_dir=out,
        package_name="d1l-test",
        full_size=0x20000,
        release_profile="development",
        rp2040_artifact_root=rp2040_artifacts,
        meshcore_conformance_json=conformance,
        meshcore_signed_advert_runtime_json=signed_advert,
    )

    package_dir = out / "d1l-test"
    assert manifest["schema"] == 1
    assert manifest["project"] == package_release_d1l.PROJECT
    assert manifest["app_version"] == "1.0.0-rc1"
    assert "workflow" in manifest
    assert (package_dir / "firmware" / "bootloader.bin").read_bytes() == b"BOOT"
    assert (package_dir / "firmware" / "partition-table.bin").read_bytes() == b"PART"
    assert (package_dir / "firmware" / "ota_data_initial.bin").read_bytes() == b"OTA-DATA"
    assert (package_dir / "firmware" / "meshcore_deskos_d1l.bin").read_bytes() == b"APP"
    assert (package_dir / "update" / "d1l-update.bin").read_bytes() == b"APP"
    assert next(
        entry
        for entry in manifest["flash_files"]
        if entry["source"] == "ota_data_initial.bin"
    )["role"] == "ota-data"
    assert (package_dir / "notices" / "LICENSE").read_text(encoding="ascii") == "project license\n"
    assert (package_dir / "notices" / "THIRD_PARTY_NOTICES.md").read_text(encoding="ascii") == "third party notices\n"
    assert (package_dir / "notices" / "ATTRIBUTIONS.md").read_text(encoding="ascii") == "attributions\n"
    assert (
        package_dir / "notices" / "SOURCE_AUDIT_AND_ATTRIBUTION.md"
    ).read_text(encoding="ascii") == "source audit\n"
    assert (
        package_dir / "notices" / "ORLP_ED25519_ZLIB_LICENSE.txt"
    ).read_text(encoding="ascii") == "orlp zlib license\n"
    assert [item["path"] for item in manifest["notice_files"]] == [
        "notices/LICENSE",
        "notices/THIRD_PARTY_NOTICES.md",
        "notices/ATTRIBUTIONS.md",
        "notices/SOURCE_AUDIT_AND_ATTRIBUTION.md",
        "notices/ORLP_ED25519_ZLIB_LICENSE.txt",
    ]
    assert [item["path"] for item in manifest["release_docs"]] == [
        "docs/USER_GUIDE_D1L.md",
        "docs/DEVELOPER_GUIDE_D1L.md",
        "docs/FLASH_RECOVERY_D1L.md",
        "docs/RP2040_SD_BRIDGE_FLASH_D1L.md",
    ]
    assert [item["name"] for item in manifest["rp2040_artifacts"]] == [
        "rp2040-sd-bridge-firmware",
        "rp2040-sd-smoke-firmware",
        "rp2040-seeed-official-sd-smoke-firmware",
    ]
    for artifact in manifest["rp2040_artifacts"]:
        assert artifact["uf2_files"]
        assert (package_dir / artifact["uf2_files"][0]).is_file()
    conformance_metadata = manifest["meshcore_conformance"]
    packaged_conformance = package_dir / conformance_metadata["path"]
    packaged_report = json.loads(packaged_conformance.read_text(encoding="ascii"))
    assert packaged_report == package_release_d1l.canonicalize_release_report(
        json.loads(conformance.read_text(encoding="utf-8"))
    )
    assert packaged_conformance.read_bytes() != conformance.read_bytes()
    assert "generated_at" not in packaged_report
    assert conformance_metadata["evidence_profile"] == (
        package_release_d1l.CANONICAL_EVIDENCE_PROFILE
    )
    run_receipt = conformance_metadata["run_receipt"]
    assert run_receipt["artifact"] == (
        package_release_d1l.MESHCORE_CONFORMANCE_ACTIONS_ARTIFACT
    )
    assert run_receipt["path"] == conformance.name
    assert run_receipt["size"] == conformance.stat().st_size
    assert run_receipt["sha256"] == package_release_d1l.sha256_file(conformance)
    assert run_receipt["generated_at"] == json.loads(
        conformance.read_text(encoding="utf-8")
    )["generated_at"]
    assert datetime.fromisoformat(run_receipt["expires_at"].replace("Z", "+00:00")) == (
        datetime.fromisoformat(run_receipt["generated_at"].replace("Z", "+00:00"))
        + timedelta(days=package_release_d1l.MESHCORE_CONFORMANCE_MAX_AGE_DAYS)
    )
    assert conformance_metadata["source_commit"] == commit
    assert conformance_metadata["coverage_level"] == "wire_envelope_only"
    assert conformance_metadata["closure_ready"] is False
    assert conformance_metadata["issue_65_closure_eligible"] is False
    assert conformance_metadata["sha256"] == package_release_d1l.sha256_file(packaged_conformance)

    signed_metadata = manifest["meshcore_signed_advert_runtime"]
    packaged_signed = package_dir / signed_metadata["path"]
    packaged_signed_report = json.loads(packaged_signed.read_text(encoding="ascii"))
    raw_signed_report = json.loads(signed_advert.read_text(encoding="utf-8"))
    assert packaged_signed_report == signed_runtime.canonicalize_release_report(
        raw_signed_report, commit
    )
    assert "generated_at" not in packaged_signed_report
    assert "commands" not in packaged_signed_report
    assert "source" not in packaged_signed_report["external_archive"]
    assert signed_metadata["source_commit"] == commit
    assert signed_metadata["full_ubsan_clean"] is True
    assert signed_metadata["closure_ready"] is False
    assert signed_metadata["wp04_closure_eligible"] is False
    assert signed_metadata["run_receipt"]["path"] == signed_advert.name
    assert signed_metadata["run_receipt"]["sha256"] == (
        package_release_d1l.sha256_file(signed_advert)
    )

    full = package_dir / manifest["full_flash_image"]["path"]
    image = full.read_bytes()
    assert len(image) == 0x20000
    assert image[0:4] == b"BOOT"
    assert image[0x8000 : 0x8004] == b"PART"
    assert image[0x10000 : 0x10003] == b"APP"
    assert image[0x9000] == 0xFF

    sha_text = (package_dir / "SHA256SUMS.txt").read_text(encoding="ascii")
    assert "./firmware/meshcore_deskos_d1l.bin" in sha_text
    assert "./full-flash/meshcore_deskos_d1l-full-8mb.bin" in sha_text
    assert "./manifest.json" in sha_text
    assert "./rp2040/rp2040-sd-bridge-firmware/rp2040-sd-bridge-firmware.uf2" in sha_text
    assert "./rp2040/rp2040-seeed-official-sd-smoke-firmware/rp2040-seeed-official-sd-smoke-firmware.uf2" in sha_text
    assert "./rp2040/rp2040-sd-bridge-firmware/SHA256SUMS.txt" in sha_text
    assert "./rp2040/rp2040-sd-smoke-firmware/SHA256SUMS.txt" in sha_text
    assert "./rp2040/rp2040-seeed-official-sd-smoke-firmware/SHA256SUMS.txt" in sha_text
    assert "./docs/USER_GUIDE_D1L.md" in sha_text
    assert "./notices/LICENSE" in sha_text
    assert "./notices/THIRD_PARTY_NOTICES.md" in sha_text
    assert "./notices/ORLP_ED25519_ZLIB_LICENSE.txt" in sha_text
    assert f"./evidence/meshcore_conformance_{commit}.json" in sha_text
    assert f"./evidence/meshcore_signed_advert_runtime_{commit}.json" in sha_text
    assert f"./build_inputs_{commit}.json" in sha_text
    assert f"./capability_manifest_{commit}.json" in sha_text
    assert f"./release_evidence_index_{commit}.json" in sha_text
    assert f"./sbom_{commit}.spdx.json" in sha_text
    assert f"./provenance_{commit}.json" in sha_text
    sbom_path = package_dir / manifest["sbom"]["path"]
    assert manifest["sbom"]["valid"] is True
    assert manifest["sbom"]["source_commit"] == commit
    assert manifest["sbom"]["sha256"] == package_release_d1l.sha256_file(sbom_path)
    sbom = json.loads(sbom_path.read_text(encoding="ascii"))
    assert sbom["spdxVersion"] == "SPDX-2.3"
    assert sbom["name"] == f"n30nex-SIGUI-{commit}"
    assert any(item["versionInfo"] == "1" * 40 for item in sbom["packages"])
    assert any(item["versionInfo"] == "2" * 40 for item in sbom["packages"])
    assert any(item["fileName"] == "./package/firmware/meshcore_deskos_d1l.bin" for item in sbom["files"])
    assert any(item["fileName"] == "./source/THIRD_PARTY_NOTICES.md" for item in sbom["files"])
    overlay_sbom = next(
        item
        for item in sbom["files"]
        if item["fileName"] == "./source/overlays/meshcore_ed25519_defined/fe.c"
    )
    assert overlay_sbom["licenseConcluded"] == "Zlib"
    assert overlay_sbom["licenseInfoInFiles"] == ["Zlib"]
    packaged_license_sbom = next(
        item
        for item in sbom["files"]
        if item["fileName"] == "./package/notices/ORLP_ED25519_ZLIB_LICENSE.txt"
    )
    assert packaged_license_sbom["licenseConcluded"] == "Zlib"
    for source_name in (
        ".github/d1l-build-inputs.json",
        "requirements/ci-host-windows.txt",
        "docs/COMPLETION_LEDGER.yaml",
    ):
        assert any(item["fileName"] == f"./source/{source_name}" for item in sbom["files"])
    for contract_name in package_release_d1l.PACKAGE_METADATA_CONTRACTS:
        metadata = manifest[contract_name]
        payload = package_release_d1l.validate_generated_package_metadata(
            package_dir, metadata, commit, contract_name
        )
        assert metadata["generated_package_metadata"] is True
        assert metadata["release_evidence"] is False
        assert metadata["physical_closure_claimed"] is False
        assert payload["source_commit"] == commit
    build_inputs_payload = json.loads(
        (package_dir / f"build_inputs_{commit}.json").read_text(encoding="ascii")
    )
    assert build_inputs_payload["source"]["path"] == ".github/d1l-build-inputs.json"
    capability_payload = json.loads(
        (package_dir / f"capability_manifest_{commit}.json").read_text(encoding="ascii")
    )
    assert [item["id"] for item in capability_payload["capabilities"]] == [
        "authenticated_remote_admin",
        "public_messages",
    ]
    evidence_payload = json.loads(
        (package_dir / f"release_evidence_index_{commit}.json").read_text(
            encoding="ascii"
        )
    )
    assert evidence_payload["release_ready"] is False
    assert evidence_payload["readiness_evaluated_by_packaging"] is False
    provenance_path = package_dir / manifest["provenance"]["path"]
    assert manifest["provenance"]["valid"] is True
    assert manifest["provenance"]["authenticated"] is False
    assert manifest["provenance"]["source_commit"] == commit
    assert manifest["provenance"]["sha256"] == package_release_d1l.sha256_file(
        provenance_path
    )
    provenance = json.loads(provenance_path.read_text(encoding="ascii"))
    assert provenance["_type"] == "https://in-toto.io/Statement/v1"
    assert provenance["predicateType"] == "https://slsa.dev/provenance/v1"
    assert provenance["predicate"]["sigui_attestation"]["authenticated"] is False
    assert any(
        item["name"] == "firmware/meshcore_deskos_d1l.bin"
        for item in provenance["subject"]
    )
    assert any(item["name"] == f"sbom_{commit}.spdx.json" for item in provenance["subject"])
    for path in (
        f"build_inputs_{commit}.json",
        f"capability_manifest_{commit}.json",
        f"release_evidence_index_{commit}.json",
    ):
        assert any(item["name"] == path for item in provenance["subject"])
    material_names = {
        item["name"]
        for item in provenance["predicate"]["buildDefinition"]["resolvedDependencies"]
    }
    assert {
        ".github/d1l-build-inputs.json",
        "requirements/ci-host-windows.txt",
        "docs/COMPLETION_LEDGER.yaml",
    }.issubset(material_names)
    assert verify_sha256_manifest(package_dir / "SHA256SUMS.txt")
    for artifact in manifest["rp2040_artifacts"]:
        nested_manifest = package_dir / "rp2040" / artifact["name"] / "SHA256SUMS.txt"
        assert verify_sha256_manifest(nested_manifest)
    readme = (package_dir / "README_RELEASE.md").read_text(encoding="ascii")
    assert "App image: `firmware/meshcore_deskos_d1l.bin`" in readme
    assert "`rp2040/` contains the Actions-built RP2040 SD bridge" in readme
    assert "`docs/` contains the user guide" in readme
    assert "`notices/` contains the project license" in readme
    assert f"`sbom_{commit}.spdx.json` is the deterministic SPDX 2.3 SBOM" in readme
    assert f"`provenance_{commit}.json` is deterministic unsigned SLSA v1 provenance" in readme
    assert "package metadata, not new release evidence or physical closure" in readme
    assert "neither projection alone closes WP-04 or issue #65" in readme


def test_release_package_rejects_signed_advert_receipt_from_other_commit(
    tmp_path, monkeypatch
):
    build = tmp_path / "build"
    write_fake_build(build)
    write_fake_notices(tmp_path)
    commit = "b" * 40
    install_fake_source_identity(monkeypatch, commit)
    signed_advert = write_meshcore_signed_advert_runtime(tmp_path, commit)
    report = json.loads(signed_advert.read_text(encoding="utf-8"))
    report["repository"]["repository_commit"] = "c" * 40
    signed_advert.write_text(json.dumps(report), encoding="utf-8")

    with pytest.raises(ValueError, match="repository_commit"):
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=tmp_path / "artifacts" / "release",
            package_name="signed-advert-wrong-commit",
            full_size=0x20000,
            release_profile="development",
            meshcore_signed_advert_runtime_json=signed_advert,
        )


def test_release_package_rejects_substituted_signed_runtime_binding_hash(
    tmp_path, monkeypatch
):
    build = tmp_path / "build"
    write_fake_build(build)
    write_fake_notices(tmp_path)
    commit = "b" * 40
    monkeypatch.setenv("GITHUB_SHA", commit)
    install_fake_source_identity(monkeypatch, commit)
    conformance = write_meshcore_conformance(tmp_path, commit)
    signed_advert = write_meshcore_signed_advert_runtime(tmp_path, commit)
    report = json.loads(conformance.read_text(encoding="utf-8"))
    report["signed_advert_runtime"]["canonical_sha256"] = "0" * 64
    conformance.write_text(json.dumps(report), encoding="utf-8")

    with pytest.raises(ValueError, match="signed_advert_runtime_receipt_binding"):
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=tmp_path / "artifacts" / "release",
            package_name="substituted-signed-runtime-binding",
            full_size=0x20000,
            release_profile="development",
            meshcore_conformance_json=conformance,
            meshcore_signed_advert_runtime_json=signed_advert,
        )


def test_release_package_rejects_signed_advert_receipt_without_sanitizer_commands(
    tmp_path, monkeypatch
):
    build = tmp_path / "build"
    write_fake_build(build)
    write_fake_notices(tmp_path)
    commit = "b" * 40
    install_fake_source_identity(monkeypatch, commit)
    signed_advert = write_meshcore_signed_advert_runtime(tmp_path, commit)
    report = json.loads(signed_advert.read_text(encoding="utf-8"))
    report["commands"] = [
        [argument for argument in command if argument != "-fsanitize=address,undefined"]
        for command in report["commands"]
    ]
    signed_advert.write_text(json.dumps(report), encoding="utf-8")

    with pytest.raises(ValueError, match="commands"):
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=tmp_path / "artifacts" / "release",
            package_name="signed-advert-without-sanitizers",
            full_size=0x20000,
            release_profile="development",
            meshcore_signed_advert_runtime_json=signed_advert,
        )


def test_release_package_accepts_signed_advert_commands_from_other_checkout_root(
    tmp_path, monkeypatch
):
    build = tmp_path / "build"
    write_fake_build(build)
    write_fake_notices(tmp_path)
    commit = "b" * 40
    install_fake_source_identity(monkeypatch, commit)
    signed_advert = write_meshcore_signed_advert_runtime(tmp_path, commit)
    report = json.loads(signed_advert.read_text(encoding="utf-8"))
    local_root = str(signed_runtime.ROOT).replace("\\", "/").rstrip("/")
    producer_root = "/home/runner/work/SIGUI/SIGUI"
    external_suffix = f"/{signed_runtime.EXTERNAL_CPP_SOURCES[0]}"
    external_command = next(
        [argument.replace("\\", "/") for argument in command]
        for command in report["commands"]
        if any(
            argument.replace("\\", "/").endswith(external_suffix)
            for argument in command
        )
    )
    external_source = external_command[external_command.index("-c") + 1]
    local_external = external_source[: -len(external_suffix)].rstrip("/")
    first_command = [argument.replace("\\", "/") for argument in report["commands"][0]]
    local_build = first_command[first_command.index("-o") + 1].rsplit("/", 1)[0]
    relocated_roots = (
        (local_root, producer_root),
        (local_external, "/tmp/sigui-producer/crypto"),
        (local_build, "/tmp/sigui-producer/build"),
    )

    def relocate(argument: str) -> str:
        normalized = argument.replace("\\", "/")
        for local, producer in sorted(
            relocated_roots, key=lambda item: len(item[0]), reverse=True
        ):
            if normalized == local or normalized.startswith(f"{local}/"):
                return f"{producer}{normalized[len(local):]}"
        return normalized

    report["commands"] = [
        [relocate(argument) for argument in command]
        for command in report["commands"]
    ]
    executable = report["commands"][-1][0]
    relocated_executable = (
        executable.removesuffix(".exe")
        if executable.endswith(".exe")
        else f"{executable}.exe"
    )
    link_output_index = report["commands"][-2].index("-o") + 1
    report["commands"][-2][link_output_index] = relocated_executable
    report["commands"][-1][0] = relocated_executable
    signed_advert.write_text(json.dumps(report), encoding="utf-8")

    manifest = package_release_d1l.create_release_package(
        root=tmp_path,
        build_dir=build,
        out_dir=tmp_path / "artifacts" / "release",
        package_name="signed-advert-relocated-checkout",
        full_size=0x20000,
        release_profile="development",
        meshcore_signed_advert_runtime_json=signed_advert,
    )

    assert manifest["meshcore_signed_advert_runtime"]["passed"] is True


def test_release_package_rejects_mismatched_runtime_link_and_execution_paths(
    tmp_path, monkeypatch
):
    build = tmp_path / "build"
    write_fake_build(build)
    write_fake_notices(tmp_path)
    commit = "b" * 40
    install_fake_source_identity(monkeypatch, commit)
    signed_advert = write_meshcore_signed_advert_runtime(tmp_path, commit)
    report = json.loads(signed_advert.read_text(encoding="utf-8"))
    executable = report["commands"][-1][0]
    report["commands"][-1][0] = (
        executable.removesuffix(".exe")
        if executable.endswith(".exe")
        else f"{executable}.exe"
    )
    signed_advert.write_text(json.dumps(report), encoding="utf-8")

    with pytest.raises(ValueError, match="commands"):
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=tmp_path / "artifacts" / "release",
            package_name="signed-advert-mismatched-runtime-output",
            full_size=0x20000,
            release_profile="development",
            meshcore_signed_advert_runtime_json=signed_advert,
        )


def test_exact_sha_package_metadata_contract_rejects_missing_stale_and_mismatched(
    tmp_path,
):
    write_fake_notices(tmp_path)
    package_dir = tmp_path / "package"
    package_dir.mkdir()
    commit = "a" * 40
    payloads = package_release_d1l.package_inventory_payloads(tmp_path, commit)

    metadata = package_release_d1l.write_package_metadata_artifact(
        package_dir, "build_inputs", commit, payloads["build_inputs"]
    )
    target = package_dir / metadata["path"]
    target.unlink()
    with pytest.raises(FileNotFoundError, match="Missing exact-SHA package metadata"):
        package_release_d1l.validate_generated_package_metadata(
            package_dir, metadata, commit, "build_inputs"
        )

    metadata = package_release_d1l.write_package_metadata_artifact(
        package_dir, "build_inputs", commit, payloads["build_inputs"]
    )
    stale = dict(metadata)
    stale["path"] = f"build_inputs_{'b' * 40}.json"
    with pytest.raises(ValueError, match="stale or invalid: path"):
        package_release_d1l.validate_generated_package_metadata(
            package_dir, stale, commit, "build_inputs"
        )

    mismatched_payload = dict(payloads["build_inputs"])
    mismatched_payload["source_commit"] = "b" * 40
    target.write_text(
        package_release_d1l.canonical_json(mismatched_payload), encoding="ascii"
    )
    mismatched = {
        **metadata,
        "size": target.stat().st_size,
        "sha256": package_release_d1l.sha256_file(target),
    }
    with pytest.raises(ValueError, match="identity is stale or invalid: source_commit"):
        package_release_d1l.validate_generated_package_metadata(
            package_dir, mismatched, commit, "build_inputs"
        )


def test_package_inventory_metadata_fails_closed_on_missing_sources_and_bad_ledger(
    tmp_path,
):
    write_fake_notices(tmp_path)
    commit = "a" * 40
    build_inputs = tmp_path / package_release_d1l.BUILD_INPUTS_SOURCE
    build_inputs.unlink()
    with pytest.raises(ValueError, match="build-input lock is missing or unreadable JSON"):
        package_release_d1l.package_inventory_payloads(tmp_path, commit)

    write_fake_notices(tmp_path)
    ledger_path = tmp_path / package_release_d1l.COMPLETION_LEDGER_SOURCE
    ledger = json.loads(ledger_path.read_text(encoding="ascii"))
    ledger["capabilities"].append(dict(ledger["capabilities"][0]))
    ledger_path.write_text(json.dumps(ledger), encoding="ascii")
    with pytest.raises(ValueError, match="duplicate id public_messages"):
        package_release_d1l.package_inventory_payloads(tmp_path, commit)


def test_release_package_rejects_mismatched_or_expired_meshcore_evidence(tmp_path, monkeypatch):
    build = tmp_path / "build"
    out = tmp_path / "artifacts" / "release"
    write_fake_build(build)
    write_fake_notices(tmp_path)
    commit = "b" * 40
    monkeypatch.setenv("GITHUB_SHA", commit)
    install_fake_source_identity(monkeypatch, commit)
    signed_advert = write_meshcore_signed_advert_runtime(tmp_path, commit)

    mismatched = write_meshcore_conformance(
        tmp_path,
        commit,
        source_verification={"repository_commit": "c" * 40},
    )
    try:
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=out,
            package_name="mismatch",
            full_size=0x20000,
            release_profile="development",
            meshcore_conformance_json=mismatched,
            meshcore_signed_advert_runtime_json=signed_advert,
        )
    except ValueError as exc:
        assert "source_commit" in str(exc)
    else:
        raise AssertionError("mismatched MeshCore commit was accepted")

    expired = write_meshcore_conformance(
        tmp_path,
        commit,
        generated_at=datetime.now(timezone.utc)
        - timedelta(days=package_release_d1l.MESHCORE_CONFORMANCE_MAX_AGE_DAYS + 1),
    )
    try:
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=out,
            package_name="expired",
            full_size=0x20000,
            release_profile="development",
            meshcore_conformance_json=expired,
            meshcore_signed_advert_runtime_json=signed_advert,
        )
    except ValueError as exc:
        assert "expired" in str(exc)
    else:
        raise AssertionError("expired MeshCore evidence was accepted")

    far_future = write_meshcore_conformance(
        tmp_path,
        commit,
        generated_at=datetime(9999, 12, 31, tzinfo=timezone.utc),
    )
    try:
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=out,
            package_name="future-overflow",
            full_size=0x20000,
            release_profile="development",
            meshcore_conformance_json=far_future,
            meshcore_signed_advert_runtime_json=signed_advert,
        )
    except ValueError as exc:
        assert "future" in str(exc) or "supported range" in str(exc)
    else:
        raise AssertionError("out-of-range MeshCore evidence was accepted")


def test_release_package_rejects_top_level_green_but_incomplete_conformance(
    tmp_path, monkeypatch
):
    build = tmp_path / "build"
    write_fake_build(build)
    write_fake_notices(tmp_path)
    commit = "d" * 40
    monkeypatch.setenv("GITHUB_SHA", commit)
    install_fake_source_identity(monkeypatch, commit)
    conformance_path = write_meshcore_conformance(tmp_path, commit)
    report = json.loads(conformance_path.read_text(encoding="utf-8"))
    report["vector_result"] = None
    conformance_path.write_text(json.dumps(report), encoding="utf-8")

    with pytest.raises(ValueError, match="vector_passed"):
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=tmp_path / "artifacts" / "release",
            package_name="incomplete-conformance",
            full_size=0x20000,
            release_profile="development",
            meshcore_conformance_json=conformance_path,
        )


def test_release_package_requires_each_rp2040_checksum_manifest(tmp_path, monkeypatch):
    install_fake_source_identity(monkeypatch, "c" * 40)
    build = tmp_path / "build"
    out = tmp_path / "artifacts" / "release"
    write_fake_build(build)
    rp2040_artifacts = write_fake_rp2040_artifacts(
        tmp_path, include_manifests=False
    )

    with pytest.raises(
        ValueError, match="must contain exactly one valid root SHA256SUMS.txt"
    ):
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=out,
            package_name="missing-rp2040-manifests",
            full_size=0x20000,
            release_profile="development",
            rp2040_artifact_root=rp2040_artifacts,
        )

    invalid_root = tmp_path / "invalid"
    invalid_artifacts = write_fake_rp2040_artifacts(invalid_root)
    invalid_manifest = (
        invalid_artifacts / package_release_d1l.RP2040_ARTIFACT_NAMES[0]
        / "SHA256SUMS.txt"
    )
    invalid_manifest.write_text("not a valid checksum row\n", encoding="ascii")
    with pytest.raises(
        ValueError, match="must contain exactly one valid root SHA256SUMS.txt"
    ):
        package_release_d1l.create_release_package(
            root=invalid_root,
            build_dir=build,
            out_dir=out,
            package_name="invalid-rp2040-manifest",
            full_size=0x20000,
            release_profile="development",
            rp2040_artifact_root=invalid_artifacts,
        )


def test_copy_rp2040_artifacts_rejects_linked_source_directory(tmp_path):
    artifacts = write_fake_rp2040_artifacts(tmp_path)
    artifact_name = package_release_d1l.RP2040_ARTIFACT_NAMES[0]
    source = artifacts / artifact_name
    outside = tmp_path / "outside-rp2040-source"
    source.rename(outside)
    try:
        os.symlink(outside, source, target_is_directory=True)
    except (NotImplementedError, OSError) as exc:
        pytest.skip(f"directory symlinks unavailable: {exc}")

    package_dir = tmp_path / "package"
    package_dir.mkdir()
    with pytest.raises(ValueError, match="real direct child"):
        package_release_d1l.copy_rp2040_artifacts(artifacts, package_dir)


def test_copy_rp2040_artifacts_rejects_reparse_source_directory(
    tmp_path, monkeypatch
):
    artifacts = write_fake_rp2040_artifacts(tmp_path)
    source = artifacts / package_release_d1l.RP2040_ARTIFACT_NAMES[0]
    original = package_release_d1l.is_link_or_reparse
    monkeypatch.setattr(
        package_release_d1l,
        "is_link_or_reparse",
        lambda path: path == source or original(path),
    )

    package_dir = tmp_path / "package"
    package_dir.mkdir()
    with pytest.raises(ValueError, match="real direct child"):
        package_release_d1l.copy_rp2040_artifacts(artifacts, package_dir)


def test_copy_rp2040_artifacts_rechecks_complete_destination_tree(
    tmp_path, monkeypatch
):
    artifacts = write_fake_rp2040_artifacts(tmp_path)
    package_dir = tmp_path / "package"
    package_dir.mkdir()
    original_copytree = package_release_d1l.shutil.copytree
    injected = False

    def copytree_with_uncovered_manifest(source, destination):
        nonlocal injected
        result = original_copytree(source, destination)
        if not injected:
            injected = True
            nested = Path(destination) / "unexpected"
            nested.mkdir()
            (nested / "SHA256SUMS.txt").write_text(
                f"{'0' * 64}  ./missing.bin\n", encoding="ascii"
            )
        return result

    monkeypatch.setattr(package_release_d1l.shutil, "copytree", copytree_with_uncovered_manifest)
    with pytest.raises(ValueError, match="verification changed after copy"):
        package_release_d1l.copy_rp2040_artifacts(artifacts, package_dir)


def test_generated_flash_scripts_require_explicit_port(tmp_path, monkeypatch):
    root = tmp_path
    build = root / "build"
    out = root / "artifacts" / "release"
    write_fake_build(build)
    write_fake_notices(root)
    write_fake_config(root)
    commit = "d" * 40
    install_fake_source_identity(monkeypatch, commit)

    package_release_d1l.create_release_package(
        root=root,
        build_dir=build,
        out_dir=out,
        package_name="d1l-test",
        full_size=0x20000,
        release_profile="development",
    )

    ps1 = (out / "d1l-test" / "flash_project.ps1").read_text(encoding="ascii")
    sh = (out / "d1l-test" / "flash_project.sh").read_text(encoding="ascii")
    full_ps1 = (out / "d1l-test" / "flash_full_8mb.ps1").read_text(encoding="ascii")

    assert "$env:D1L_PORT" in ps1
    assert "No D1L port supplied" in ps1
    assert "${D1L_PORT:?" in sh
    assert os.access(out / "d1l-test" / "flash_project.sh", os.X_OK)
    assert "FULL-FLASH-$Port" in full_ps1
    assert "COM7" not in ps1
    assert "COM11" not in ps1


def test_esp32_only_release_package_omits_rp2040_artifacts(tmp_path, monkeypatch):
    root = tmp_path
    build = root / "build"
    out = root / "artifacts" / "release"
    write_fake_build(build)
    write_fake_notices(root)
    write_fake_config(root)
    commit = "e" * 40
    install_fake_source_identity(monkeypatch, commit)

    manifest = package_release_d1l.create_release_package(
        root=root,
        build_dir=build,
        out_dir=out,
        package_name="d1l-esp32-only",
        full_size=0x20000,
        release_profile="development",
    )

    package_dir = out / "d1l-esp32-only"
    assert manifest["rp2040_artifacts"] == []
    assert not (package_dir / "rp2040").exists()
    sha_text = (package_dir / "SHA256SUMS.txt").read_text(encoding="ascii")
    assert "./firmware/meshcore_deskos_d1l.bin" in sha_text
    assert "./rp2040/" not in sha_text
    readme = (package_dir / "README_RELEASE.md").read_text(encoding="ascii")
    assert "`rp2040/` is omitted from this ESP32-only package" in readme
    assert "`include_sd_bridge=true`" in readme


def test_release_package_rejects_dirty_source_worktree(tmp_path, monkeypatch):
    build = tmp_path / "build"
    write_fake_build(build)
    monkeypatch.setattr(
        package_release_d1l,
        "git_info",
        lambda _root: {
            "commit": "a" * 40,
            "short_commit": "a" * 7,
            "branch": "test",
            "dirty": True,
            "dirty_entries": [" M scripts/package_release_d1l.py"],
            "source_patches": [],
        },
    )

    with pytest.raises(ValueError, match="clean source worktree"):
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=tmp_path / "release",
            package_name="dirty",
            full_size=0x20000,
            release_profile="development",
        )


def test_git_info_treats_expected_bsp_patches_as_clean(monkeypatch, tmp_path):
    def fake_git_value(root, *args):
        if args == ("status", "--porcelain"):
            return " m third_party/sensecap_indicator_esp32"
        if args == ("rev-parse", "HEAD"):
            return "abc123"
        if args == ("rev-parse", "--short", "HEAD"):
            return "abc123"
        if args == ("branch", "--show-current"):
            return "feature/test"
        return None

    monkeypatch.setattr(package_release_d1l, "git_value", fake_git_value)
    monkeypatch.setattr(package_release_d1l, "exact_expected_bsp_patch_state", lambda root: True)

    info = package_release_d1l.git_info(tmp_path)

    assert info["dirty"] is False
    assert info["dirty_entries"] == []
    assert info["source_patches"] == [
        "patches/sensecap_indicator_touch_fix.patch",
        "patches/sensecap_indicator_idf55_compat.patch",
        "patches/sensecap_indicator_tx_origin.patch",
    ]


def test_exact_bsp_patch_state_accepts_only_the_tracked_patch_tree(tmp_path):
    create_exact_bsp_patch_fixture(tmp_path)

    assert package_release_d1l.exact_expected_bsp_patch_state(tmp_path) is True
    info = package_release_d1l.git_info(tmp_path)
    assert info["dirty"] is False
    assert info["dirty_entries"] == []
    assert info["source_patches"] == [path.as_posix() for path in package_release_d1l.EXPECTED_BSP_PATCHES]


def test_exact_bsp_patch_state_rejects_extra_tracked_delta(tmp_path):
    submodule = create_exact_bsp_patch_fixture(tmp_path)
    with (submodule / "touch.c").open("ab") as stream:
        stream.write(b"unexpected tracked delta\n")

    assert package_release_d1l.exact_expected_bsp_patch_state(tmp_path) is False
    assert package_release_d1l.git_info(tmp_path)["dirty"] is True


def test_exact_bsp_patch_state_rejects_untracked_content(tmp_path):
    submodule = create_exact_bsp_patch_fixture(tmp_path)
    (submodule / "unexpected.c").write_bytes(b"unexpected untracked content\n")

    assert package_release_d1l.exact_expected_bsp_patch_state(tmp_path) is False
    assert package_release_d1l.git_info(tmp_path)["dirty"] is True


def test_expected_bsp_patch_set_fails_closed_when_any_reverse_check_fails(monkeypatch, tmp_path):
    submodule = tmp_path / package_release_d1l.EXPECTED_BSP_SUBMODULE
    submodule.mkdir(parents=True)
    for relative_patch in package_release_d1l.EXPECTED_BSP_PATCHES:
        patch = tmp_path / relative_patch
        patch.parent.mkdir(parents=True, exist_ok=True)
        patch.write_text("patch", encoding="utf-8")

    calls = []

    def fake_command_succeeds(cwd, args):
        calls.append((cwd, args))
        return len(calls) == 1

    monkeypatch.setattr(package_release_d1l, "command_succeeds", fake_command_succeeds)

    assert package_release_d1l.expected_bsp_patches_applied(tmp_path) is False
    assert len(calls) == 2
