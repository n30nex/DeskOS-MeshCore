import copy
import json
from pathlib import Path

import pytest

from scripts import provenance_d1l, sbom_d1l


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


def claim(path: Path, package_dir: Path, **metadata: object) -> dict:
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
        flash_files.append(claim(path, package_dir, role=role))

    notice_files = []
    for source in sbom_d1l.REQUIRED_NOTICE_SOURCES:
        path = notices / Path(source).name
        path.write_text(f"packaged {source}\n", encoding="utf-8")
        notice_files.append(claim(path, package_dir, source=source))

    manifest = {
        "package": "d1l-test",
        "app_version": "1.0.0-test",
        "git": {"commit": commit},
        "workflow": {
            "sha": commit,
            "ref": "refs/tags/v1.0.0-test",
            "repository": "n30nex/SIGUI",
            "workflow": "d1l-ci",
            "run_id": "123456",
            "run_attempt": "1",
            "run_url": "https://github.com/n30nex/SIGUI/actions/runs/123456",
        },
        "flash_files": flash_files,
        "notice_files": notice_files,
    }
    manifest["sbom"] = sbom_d1l.write_package_sbom(
        root,
        package_dir,
        manifest,
        source_identity=source_identity(commit),
        expected_source_sha=commit,
    )
    return package_dir, manifest


def as_core_manifest(
    manifest: dict,
    *,
    run_id: str = "123456",
    run_attempt: str = "1",
) -> dict:
    core = copy.deepcopy(manifest)
    core.update(
        {
            "release_profile": "core_1_0",
            "firmware_commit": COMMIT,
            "actions_run": run_id,
            "actions_run_attempt": run_attempt,
        }
    )
    core["workflow"].update(
        {
            "workflow": "d1l-ci",
            "run_id": run_id,
            "run_attempt": run_attempt,
            "run_url": (
                "https://github.com/n30nex/SIGUI/actions/runs/" + run_id
            ),
        }
    )
    return core


def test_statement_is_deterministic_and_binds_subjects_materials_and_builder(tmp_path):
    write_source_inputs(tmp_path)
    package_dir, manifest = write_package_inputs(tmp_path)
    identity = source_identity()

    first = provenance_d1l.build_statement(tmp_path, package_dir, manifest, identity)
    second = provenance_d1l.build_statement(tmp_path, package_dir, manifest, identity)

    assert provenance_d1l.canonical_json(first) == provenance_d1l.canonical_json(second)
    assert provenance_d1l.validate_against_inputs(
        first, tmp_path, package_dir, manifest, identity
    ) == []
    assert first["_type"] == "https://in-toto.io/Statement/v1"
    assert first["predicateType"] == "https://slsa.dev/provenance/v1"
    predicate = first["predicate"]
    assert predicate["buildDefinition"]["externalParameters"] == {
        "sourceRepository": sbom_d1l.PROJECT_REPOSITORY,
        "sourceRevision": COMMIT,
        "releaseProfile": "d1l",
    }
    assert predicate["runDetails"] == {
        "builder": {"id": provenance_d1l.GITHUB_HOSTED_BUILDER}
    }
    assert predicate["sigui_attestation"] == {
        "authenticated": False,
        "format": "unsigned-json",
        "slsaBuildLevel": "not-claimed",
    }
    assert "metadata" not in predicate["runDetails"]

    subject_names = {item["name"] for item in first["subject"]}
    assert "firmware/meshcore_deskos_d1l.bin" in subject_names
    assert f"sbom_{COMMIT}.spdx.json" in subject_names
    assert "manifest.json" not in subject_names
    assert f"provenance_{COMMIT}.json" not in subject_names

    materials = predicate["buildDefinition"]["resolvedDependencies"]
    material_names = {item["name"] for item in materials}
    assert "SIGUI source" in material_names
    assert "third_party/MeshCore" in material_names
    assert "third_party/sensecap_indicator_esp32" in material_names
    assert "dependencies.lock" in material_names
    assert ".github/d1l-build-inputs.json" in material_names
    assert "requirements/ci-host-windows.txt" in material_names
    assert "docs/COMPLETION_LEDGER.yaml" in material_names
    assert "partitions_d1l.csv" in material_names
    assert "scripts/provenance_d1l.py" in material_names


@pytest.mark.parametrize(
    "relative",
    (
        ".github/d1l-build-inputs.json",
        "requirements/ci-host-windows.txt",
        "dependencies.lock",
        "docs/COMPLETION_LEDGER.yaml",
    ),
)
def test_provenance_requires_every_release_inventory_lock(tmp_path, relative):
    write_source_inputs(tmp_path)
    (tmp_path / relative).unlink()

    with pytest.raises(FileNotFoundError, match="Missing provenance material"):
        provenance_d1l.collect_materials(tmp_path, source_identity(), COMMIT)


def test_written_provenance_round_trips_without_creating_sbom_cycle(tmp_path):
    write_source_inputs(tmp_path)
    package_dir, manifest = write_package_inputs(tmp_path)
    identity = source_identity()

    metadata = provenance_d1l.write_package_provenance(
        tmp_path,
        package_dir,
        manifest,
        source_identity=identity,
        expected_source_sha=COMMIT,
    )
    manifest["provenance"] = metadata
    path = package_dir / metadata["path"]
    original = path.read_bytes()
    loaded = json.loads(original)

    assert metadata["valid"] is True
    assert metadata["authenticated"] is False
    assert metadata["sha256"] == provenance_d1l.sha256_file(path)
    assert provenance_d1l.validate_against_inputs(
        loaded, tmp_path, package_dir, manifest, identity
    ) == []
    sbom_document = json.loads(
        (package_dir / manifest["sbom"]["path"]).read_text(encoding="ascii")
    )
    assert sbom_d1l.validate_against_inputs(
        sbom_document,
        tmp_path,
        identity,
        package_dir=package_dir,
        package_manifest=manifest,
    ) == []

    provenance_d1l.write_package_provenance(
        tmp_path,
        package_dir,
        manifest,
        source_identity=identity,
        expected_source_sha=COMMIT,
    )
    assert path.read_bytes() == original


def test_validator_rejects_document_and_payload_tampering(tmp_path):
    write_source_inputs(tmp_path)
    package_dir, manifest = write_package_inputs(tmp_path)
    identity = source_identity()
    statement = provenance_d1l.build_statement(tmp_path, package_dir, manifest, identity)

    modified = copy.deepcopy(statement)
    modified["subject"][0]["digest"]["sha256"] = "0" * 64
    errors = provenance_d1l.validate_against_inputs(
        modified, tmp_path, package_dir, manifest, identity
    )
    assert "provenance does not match deterministic source and package inputs" in errors

    (package_dir / "firmware" / "meshcore_deskos_d1l.bin").write_bytes(b"TAMPERED")
    errors = provenance_d1l.validate_against_inputs(
        statement, tmp_path, package_dir, manifest, identity
    )
    assert any("manifest checksum does not match" in error for error in errors)


def test_generator_fails_closed_on_missing_or_mismatched_sbom(tmp_path):
    write_source_inputs(tmp_path)
    package_dir, manifest = write_package_inputs(tmp_path)
    identity = source_identity()

    missing = copy.deepcopy(manifest)
    missing.pop("sbom")
    with pytest.raises(ValueError, match="no SBOM binding"):
        provenance_d1l.build_statement(tmp_path, package_dir, missing, identity)

    mismatched = copy.deepcopy(manifest)
    mismatched["sbom"]["source_commit"] = "b" * 40
    with pytest.raises(ValueError, match="identity is missing or invalid"):
        provenance_d1l.build_statement(tmp_path, package_dir, mismatched, identity)

    mismatched_workflow = copy.deepcopy(manifest)
    mismatched_workflow["workflow"]["sha"] = "b" * 40
    with pytest.raises(ValueError, match="manifest workflow commit"):
        provenance_d1l.build_statement(
            tmp_path, package_dir, mismatched_workflow, identity
        )


def test_local_package_context_uses_allowlisted_self_asserted_builder(tmp_path):
    write_source_inputs(tmp_path)
    package_dir, manifest = write_package_inputs(tmp_path)
    manifest["workflow"] = {
        "sha": COMMIT,
        "ref": None,
        "repository": None,
        "run_id": None,
    }

    statement = provenance_d1l.build_statement(
        tmp_path, package_dir, manifest, source_identity()
    )

    assert statement["predicate"]["runDetails"]["builder"]["id"] == (
        provenance_d1l.LOCAL_BUILDER
    )


def test_core_statement_binds_exact_canonical_actions_invocation(tmp_path):
    write_source_inputs(tmp_path)
    package_dir, manifest = write_package_inputs(tmp_path)
    manifest = as_core_manifest(manifest)

    statement = provenance_d1l.build_statement(
        tmp_path, package_dir, manifest, source_identity()
    )

    assert statement["predicate"]["buildDefinition"]["externalParameters"] == {
        "sourceRepository": sbom_d1l.PROJECT_REPOSITORY,
        "sourceRevision": COMMIT,
        "releaseProfile": "core_1_0",
        "workflowRepository": "n30nex/SIGUI",
        "workflowName": "d1l-ci",
        "workflowPath": ".github/workflows/d1l-ci.yml",
        "workflowRunId": "123456",
        "workflowRunAttempt": "1",
    }
    assert statement["predicate"]["runDetails"] == {
        "builder": {"id": provenance_d1l.GITHUB_HOSTED_BUILDER}
    }
    assert provenance_d1l.validate_core_actions_binding(
        statement, COMMIT, "123456", "1"
    ) == []

    metadata = provenance_d1l.write_package_provenance(
        tmp_path,
        package_dir,
        manifest,
        source_identity=source_identity(),
        expected_source_sha=COMMIT,
    )
    assert metadata["release_profile"] == "core_1_0"
    assert metadata["workflow_repository"] == "n30nex/SIGUI"
    assert metadata["workflow_name"] == "d1l-ci"
    assert metadata["workflow_path"] == ".github/workflows/d1l-ci.yml"
    assert metadata["workflow_run_id"] == "123456"
    assert metadata["workflow_run_attempt"] == "1"


@pytest.mark.parametrize(
    ("other_run_id", "other_run_attempt"),
    (("654321", "1"), ("123456", "2")),
)
def test_core_validator_rejects_same_commit_other_actions_invocation(
    tmp_path, other_run_id, other_run_attempt
):
    write_source_inputs(tmp_path)
    package_dir, manifest = write_package_inputs(tmp_path)
    expected_manifest = as_core_manifest(manifest)
    other_manifest = as_core_manifest(
        manifest,
        run_id=other_run_id,
        run_attempt=other_run_attempt,
    )
    transplanted = provenance_d1l.build_statement(
        tmp_path, package_dir, other_manifest, source_identity()
    )

    assert provenance_d1l.validate_profile(transplanted, COMMIT) == []
    assert provenance_d1l.validate_core_actions_binding(
        transplanted, COMMIT, "123456", "1"
    ) == [
        "provenance is not bound to the exact Core Actions workflow invocation"
    ]
    assert (
        "provenance does not match deterministic source and package inputs"
        in provenance_d1l.validate_against_inputs(
            transplanted,
            tmp_path,
            package_dir,
            expected_manifest,
            source_identity(),
        )
    )


def test_core_validator_rejects_valid_default_profile_statement(tmp_path):
    write_source_inputs(tmp_path)
    package_dir, manifest = write_package_inputs(tmp_path)
    default_statement = provenance_d1l.build_statement(
        tmp_path, package_dir, manifest, source_identity()
    )

    assert provenance_d1l.validate_profile(default_statement, COMMIT) == []
    errors = provenance_d1l.validate_core_actions_binding(
        default_statement, COMMIT, "123456", "1"
    )
    assert (
        "provenance is not bound to the exact Core Actions workflow invocation"
        in errors
    )


@pytest.mark.parametrize(
    ("field", "value", "match"),
    (
        ("repository", "fork/SIGUI", "canonical source repository"),
        ("workflow", "other-workflow", "workflow name"),
        ("run_id", "0", "run ID"),
        ("run_attempt", "0", "run attempt"),
    ),
)
def test_core_generator_rejects_incomplete_or_noncanonical_actions_identity(
    tmp_path, field, value, match
):
    write_source_inputs(tmp_path)
    package_dir, manifest = write_package_inputs(tmp_path)
    manifest = as_core_manifest(manifest)
    manifest["workflow"][field] = value
    if field == "run_id":
        manifest["actions_run"] = value
        manifest["workflow"]["run_url"] = (
            f"https://github.com/n30nex/SIGUI/actions/runs/{value}"
        )
    elif field == "run_attempt":
        manifest["actions_run_attempt"] = value

    with pytest.raises(ValueError, match=match):
        provenance_d1l.build_statement(
            tmp_path, package_dir, manifest, source_identity()
        )
