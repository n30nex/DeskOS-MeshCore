import copy
import json
import subprocess
from pathlib import Path

import pytest

from scripts import sbom_d1l


COMMIT = "a" * 40


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


def write_source_inputs(root: Path) -> None:
    for relative in sbom_d1l.REQUIRED_SOURCE_INPUTS:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"fixture for {relative}\n", encoding="utf-8")


def package_claim(path: Path, package_dir: Path, **metadata: object) -> dict:
    return {
        "path": path.relative_to(package_dir).as_posix(),
        "size": path.stat().st_size,
        "sha256": sbom_d1l.sha256_file(path),
        **metadata,
    }


def write_package_inputs(root: Path, commit: str = COMMIT) -> tuple[Path, dict]:
    package_dir = root / "release"
    firmware = package_dir / "firmware"
    notices = package_dir / "notices"
    firmware.mkdir(parents=True)
    notices.mkdir(parents=True)

    flash_files = []
    for name, role, payload in (
        ("bootloader.bin", "bootloader", b"BOOT"),
        ("partition-table.bin", "partition-table", b"PART"),
        ("meshcore_deskos_d1l.bin", "app", b"APP"),
    ):
        path = firmware / name
        path.write_bytes(payload)
        flash_files.append(package_claim(path, package_dir, role=role))

    notice_files = []
    for source in sbom_d1l.REQUIRED_NOTICE_SOURCES:
        destination = (
            "ORLP_ED25519_ZLIB_LICENSE.txt"
            if source == "overlays/meshcore_ed25519_defined/license.txt"
            else Path(source).name
        )
        path = notices / destination
        path.write_text(f"packaged {source}\n", encoding="utf-8")
        notice_files.append(package_claim(path, package_dir, source=source))

    manifest = {
        "package": "d1l-test",
        "app_version": "1.0.0-test",
        "git": {"commit": commit},
        "workflow": {"sha": commit},
        "flash_files": flash_files,
        "notice_files": notice_files,
    }
    return package_dir, manifest


def test_source_sbom_is_deterministic_and_bound_to_exact_identities(tmp_path):
    write_source_inputs(tmp_path)
    identity = source_identity()

    first = sbom_d1l.build_spdx_document(tmp_path, identity)
    second = sbom_d1l.build_spdx_document(tmp_path, identity)

    assert sbom_d1l.serialize_spdx(first) == sbom_d1l.serialize_spdx(second)
    assert sbom_d1l.validate_against_inputs(first, tmp_path, identity) == []
    assert first["spdxVersion"] == "SPDX-2.3"
    assert COMMIT in first["documentNamespace"]
    assert {item["versionInfo"] for item in first["packages"]} >= {
        COMMIT,
        "1" * 40,
        "2" * 40,
    }
    names = {item["fileName"] for item in first["files"]}
    assert "./source/LICENSE" in names
    assert "./source/THIRD_PARTY_NOTICES.md" in names
    assert {
        "./source/.github/d1l-build-inputs.json",
        "./source/requirements/ci-host-windows.txt",
        "./source/docs/archive/pre-rc1-authority-reset/COMPLETION_LEDGER.yaml",
        "./source/overlays/meshcore_ed25519_defined/fe.c",
        "./source/scripts/validate_ed25519_defined_overlay.py",
        "./source/tests/meshcore_signed_advert_runtime/manifest.json",
    }.issubset(names)
    overlay = next(
        item
        for item in first["files"]
        if item["fileName"] == "./source/overlays/meshcore_ed25519_defined/ge.c"
    )
    assert overlay["licenseConcluded"] == "Zlib"
    assert overlay["licenseInfoInFiles"] == ["Zlib"]


@pytest.mark.parametrize(
    "relative",
    (
        ".github/d1l-build-inputs.json",
        "requirements/ci-host-windows.txt",
        "dependencies.lock",
        "docs/archive/pre-rc1-authority-reset/COMPLETION_LEDGER.yaml",
        "overlays/meshcore_ed25519_defined/license.txt",
        "scripts/validate_ed25519_defined_overlay.py",
    ),
)
def test_source_sbom_requires_every_release_inventory_lock(tmp_path, relative):
    write_source_inputs(tmp_path)
    (tmp_path / relative).unlink()

    with pytest.raises(FileNotFoundError, match="Missing required SBOM input"):
        sbom_d1l.build_spdx_document(tmp_path, source_identity())


def test_package_sbom_round_trips_and_detects_input_tampering(tmp_path):
    write_source_inputs(tmp_path)
    identity = source_identity()
    package_dir, manifest = write_package_inputs(tmp_path)

    metadata = sbom_d1l.write_package_sbom(
        tmp_path,
        package_dir,
        manifest,
        source_identity=identity,
        expected_source_sha=COMMIT,
    )
    path = package_dir / metadata["path"]
    original = path.read_bytes()
    loaded = json.loads(original)

    assert metadata["valid"] is True
    assert metadata["sha256"] == sbom_d1l.sha256_file(path)
    assert sbom_d1l.validate_against_inputs(
        loaded,
        tmp_path,
        identity,
        package_dir=package_dir,
        package_manifest=manifest,
    ) == []
    package_names = {item["fileName"] for item in loaded["files"]}
    assert "./package/firmware/meshcore_deskos_d1l.bin" in package_names
    assert "./package/notices/THIRD_PARTY_NOTICES.md" in package_names
    assert "./package/notices/ORLP_ED25519_ZLIB_LICENSE.txt" in package_names
    packaged_license = next(
        item
        for item in loaded["files"]
        if item["fileName"] == "./package/notices/ORLP_ED25519_ZLIB_LICENSE.txt"
    )
    assert packaged_license["licenseConcluded"] == "Zlib"
    release = next(
        item
        for item in loaded["packages"]
        if item["SPDXID"] == sbom_d1l.RELEASE_PACKAGE_ID
    )
    assert "notices/ORLP_ED25519_ZLIB_LICENSE.txt" in release["attributionTexts"][0]

    sbom_d1l.write_package_sbom(
        tmp_path,
        package_dir,
        manifest,
        source_identity=identity,
        expected_source_sha=COMMIT,
    )
    assert path.read_bytes() == original

    (package_dir / "firmware" / "meshcore_deskos_d1l.bin").write_bytes(b"TAMPERED")
    errors = sbom_d1l.validate_against_inputs(
        loaded,
        tmp_path,
        identity,
        package_dir=package_dir,
        package_manifest=manifest,
    )
    assert any("manifest checksum does not match" in error for error in errors)


def test_full_feature_package_uses_production_sbom_surface(tmp_path):
    write_source_inputs(tmp_path)
    internal_source = tmp_path / ".github/d1l-build-inputs.json"
    internal_source.unlink()
    identity = source_identity()
    package_dir, manifest = write_package_inputs(tmp_path)
    manifest["release_profile"] = "full_feature"

    allowed_notices = set(sbom_d1l.PRODUCTION_REQUIRED_NOTICE_SOURCES)
    removed = [
        item
        for item in manifest["notice_files"]
        if item["source"] not in allowed_notices
    ]
    manifest["notice_files"] = [
        item
        for item in manifest["notice_files"]
        if item["source"] in allowed_notices
    ]
    for item in removed:
        (package_dir / item["path"]).unlink()

    document = sbom_d1l.build_spdx_document(
        tmp_path,
        identity,
        package_dir=package_dir,
        package_manifest=manifest,
    )

    filenames = {item["fileName"] for item in document["files"]}
    assert "./source/.github/d1l-build-inputs.json" not in filenames
    release = next(
        item
        for item in document["packages"]
        if item["SPDXID"] == sbom_d1l.RELEASE_PACKAGE_ID
    )
    attribution = release["attributionTexts"][0]
    assert "docs/ATTRIBUTIONS.md" in attribution
    assert "notices/SOURCE_AUDIT_AND_ATTRIBUTION.md" not in attribution


def test_package_sbom_uses_posix_order_for_mixed_case_paths(tmp_path):
    source_root = tmp_path / "source"
    write_source_inputs(source_root)
    identity = source_identity()
    first_dir, first_manifest = write_package_inputs(tmp_path / "first")
    second_dir, second_manifest = write_package_inputs(tmp_path / "second")
    mixed_case_files = (
        ("Zeta.bin", b"ZETA"),
        ("alpha.bin", b"ALPHA"),
        ("Beta.bin", b"BETA"),
        ("omega.bin", b"OMEGA"),
    )
    for package_dir, entries in (
        (first_dir, mixed_case_files),
        (second_dir, reversed(mixed_case_files)),
    ):
        for relative, payload in entries:
            (package_dir / relative).write_bytes(payload)

    first = sbom_d1l.build_spdx_document(
        source_root,
        identity,
        package_dir=first_dir,
        package_manifest=first_manifest,
    )
    second = sbom_d1l.build_spdx_document(
        source_root,
        identity,
        package_dir=second_dir,
        package_manifest=second_manifest,
    )
    package_names = [
        item["fileName"]
        for item in first["files"]
        if item["fileName"].startswith("./package/")
    ]

    assert package_names == sorted(package_names)
    assert sbom_d1l.serialize_spdx(first) == sbom_d1l.serialize_spdx(second)
    assert sbom_d1l.validate_against_inputs(
        first,
        source_root,
        identity,
        package_dir=first_dir,
        package_manifest=first_manifest,
    ) == []
    assert sbom_d1l.validate_against_inputs(
        second,
        source_root,
        identity,
        package_dir=second_dir,
        package_manifest=second_manifest,
    ) == []


def test_validator_rejects_modified_document(tmp_path):
    write_source_inputs(tmp_path)
    identity = source_identity()
    document = sbom_d1l.build_spdx_document(tmp_path, identity)
    modified = copy.deepcopy(document)
    modified["files"][0]["checksums"][1]["checksumValue"] = "0" * 64

    errors = sbom_d1l.validate_against_inputs(modified, tmp_path, identity)

    assert "SPDX document does not match the deterministic source and package inputs" in errors


def test_generator_fails_closed_on_missing_source_or_identity(tmp_path):
    write_source_inputs(tmp_path)
    (tmp_path / "LICENSE").unlink()
    with pytest.raises(FileNotFoundError, match="Missing required SBOM input"):
        sbom_d1l.build_spdx_document(tmp_path, source_identity())

    write_source_inputs(tmp_path)
    malformed = source_identity()
    malformed["commit"] = "abc123"
    with pytest.raises(ValueError, match="exact 40-character"):
        sbom_d1l.build_spdx_document(tmp_path, malformed)

    missing_submodule = source_identity()
    missing_submodule["submodules"] = missing_submodule["submodules"][:1]
    with pytest.raises(ValueError, match="missing submodules"):
        sbom_d1l.build_spdx_document(tmp_path, missing_submodule)

    wrong_url = source_identity()
    wrong_url["submodules"][0]["url"] = "https://example.invalid/not-meshcore.git"
    with pytest.raises(ValueError, match="URL is missing or unexpected"):
        sbom_d1l.build_spdx_document(tmp_path, wrong_url)


def test_repository_identity_uses_head_and_gitlinks():
    root = Path(__file__).resolve().parents[1]
    identity = sbom_d1l.discover_source_identity(root)
    head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()

    assert identity["commit"] == head
    assert {item["path"] for item in identity["submodules"]} == set(
        sbom_d1l.SUBMODULE_LICENSES
    )
    for item in identity["submodules"]:
        row = subprocess.run(
            ["git", "ls-tree", head, "--", item["path"]],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        assert item["commit"] in row
