#!/usr/bin/env python3
"""Generate and verify the deterministic D1L release provenance statement.

SIGUI D1L package build type v1
--------------------------------
The build type URI ending in ``#sigui-d1l-package-v1`` identifies release
packaging performed by ``scripts/package_release_d1l.py``. Its complete
external-parameter schema is exactly:

* ``sourceRepository``: the canonical SIGUI repository URL;
* ``sourceRevision``: the exact 40-character Git commit being packaged;
* ``releaseProfile``: the package release profile.

The default/full-feature package retains the legacy ``d1l`` parameter shape.
Core 1.0 additionally binds the canonical Actions repository, workflow name and
path, exact run ID, and exact run attempt. This prevents a provenance statement
from another Actions invocation at the same source commit from being
transplanted into a Core candidate package.

The operation copies the ESP32 and optional RP2040 build outputs into a D1L
release directory, generates checksummed metadata, and emits one subject per
stable package payload. The source commit, exact submodule gitlinks, workflow,
toolchain/configuration inputs, and packaging scripts are resolved dependencies.

The output is an unsigned in-toto Statement v1 carrying the SLSA provenance v1
predicate. It is checksum-verifiable release metadata, not authenticated
provenance and not a claim of SLSA Build L2. A separately signed envelope with
a trusted signer-builder binding is required for authenticity.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any
from urllib.parse import quote

if __package__:
    from .sbom_d1l import (
        PROJECT_REPOSITORY,
        REQUIRED_SOURCE_INPUTS,
        SHA256_RE,
        discover_source_identity,
        exact_sha,
        load_json,
        manifest_claims,
        normalize_source_identity,
        sha256_file,
        validate_against_inputs as validate_sbom_against_inputs,
        validate_manifest_inputs,
    )
else:
    from sbom_d1l import (  # type: ignore[no-redef]
        PROJECT_REPOSITORY,
        REQUIRED_SOURCE_INPUTS,
        SHA256_RE,
        discover_source_identity,
        exact_sha,
        load_json,
        manifest_claims,
        normalize_source_identity,
        sha256_file,
        validate_against_inputs as validate_sbom_against_inputs,
        validate_manifest_inputs,
    )


STATEMENT_TYPE = "https://in-toto.io/Statement/v1"
PREDICATE_TYPE = "https://slsa.dev/provenance/v1"
BUILD_TYPE = (
    f"{PROJECT_REPOSITORY}/blob/main/docs/BUILD_PROVENANCE_D1L.md"
    "#sigui-d1l-package-v1"
)
GITHUB_HOSTED_BUILDER = "https://github.com/actions/runner/github-hosted"
LOCAL_BUILDER = (
    f"{PROJECT_REPOSITORY}/blob/main/docs/BUILD_PROVENANCE_D1L.md"
    "#local-builder-v1"
)
WORKFLOW_PATH = ".github/workflows/d1l-ci.yml"
WORKFLOW_NAME = "d1l-ci"
CANONICAL_REPOSITORY = "n30nex/SIGUI"
DEFAULT_RELEASE_PROFILE = "d1l"
CORE_RELEASE_PROFILE = "core_1_0"
PROFILE = "SIGUI deterministic unsigned D1L provenance profile v1"
PACKAGE_EXCLUSIONS = {
    "README_RELEASE.md",
    "SHA256SUMS.txt",
    "manifest.json",
}
ATTESTATION_EXTENSION = {
    "authenticated": False,
    "format": "unsigned-json",
    "slsaBuildLevel": "not-claimed",
}

CORE_EXTERNAL_PARAMETER_KEYS = {
    "sourceRepository",
    "sourceRevision",
    "releaseProfile",
    "workflowRepository",
    "workflowName",
    "workflowPath",
    "workflowRunId",
    "workflowRunAttempt",
}


def canonical_json(value: object) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def safe_file(root: Path, path: Path, label: str) -> Path:
    resolved_root = root.resolve()
    resolved = path.resolve()
    try:
        resolved.relative_to(resolved_root)
    except ValueError as exc:
        raise ValueError(f"{label} escapes its allowed root") from exc
    if not resolved.is_file():
        raise FileNotFoundError(f"Missing {label}: {path}")
    return resolved


def exact_sha256(value: object, field: str) -> str:
    digest = str(value or "").lower()
    if SHA256_RE.fullmatch(digest) is None:
        raise ValueError(f"{field} must be an exact lowercase SHA256 digest")
    return digest


def validate_sbom_binding(
    root: Path,
    manifest: dict,
    package_dir: Path,
    source_identity: dict,
) -> dict[str, Any]:
    source_commit = source_identity["commit"]
    metadata = manifest.get("sbom")
    if not isinstance(metadata, dict):
        raise ValueError("Package manifest has no SBOM binding")
    expected_path = f"sbom_{source_commit}.spdx.json"
    if metadata.get("path") != expected_path:
        raise ValueError("Package SBOM path is not bound to the source commit")
    if metadata.get("source_commit") != source_commit or metadata.get("valid") is not True:
        raise ValueError("Package SBOM identity is missing or invalid")
    path = safe_file(package_dir, package_dir / expected_path, "package SBOM")
    digest = exact_sha256(metadata.get("sha256"), "package SBOM SHA256")
    if sha256_file(path) != digest:
        raise ValueError("Package SBOM checksum does not match its manifest binding")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("Package SBOM is not readable JSON") from exc
    if not isinstance(document, dict) or document.get("name") != f"n30nex-SIGUI-{source_commit}":
        raise ValueError("Package SBOM document is not bound to the source commit")
    errors = validate_sbom_against_inputs(
        document,
        root,
        source_identity,
        package_dir=package_dir,
        package_manifest=manifest,
    )
    if errors:
        raise ValueError("Package SBOM failed deterministic validation: " + "; ".join(errors))
    return metadata


def collect_subjects(
    root: Path, package_dir: Path, manifest: dict, source_identity: dict
) -> list[dict[str, Any]]:
    source_commit = source_identity["commit"]
    package_dir = package_dir.resolve()
    validate_manifest_inputs(manifest, package_dir, source_commit)
    sbom = validate_sbom_binding(root, manifest, package_dir, source_identity)
    current_provenance = f"provenance_{source_commit}.json"
    subjects = []
    for path in sorted(package_dir.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(package_dir).as_posix()
        if relative in PACKAGE_EXCLUSIONS or relative == current_provenance:
            continue
        if re.fullmatch(r"provenance_[0-9a-f]{40}\.json", relative):
            raise ValueError(f"Package contains stale provenance: {relative}")
        if re.fullmatch(r"sbom_[0-9a-f]{40}\.spdx\.json", relative) and relative != sbom["path"]:
            raise ValueError(f"Package contains stale SBOM: {relative}")
        safe_file(package_dir, path, f"package subject {relative}")
        subjects.append({"name": relative, "digest": {"sha256": sha256_file(path)}})
    if not subjects:
        raise ValueError("Release package has no provenance subjects")

    subject_names = {item["name"] for item in subjects}
    required_names = set(manifest_claims(manifest)) | {sbom["path"]}
    missing = sorted(required_names - subject_names)
    if missing:
        raise ValueError("Provenance subjects omit manifest-bound payloads: " + ", ".join(missing))
    return sorted(subjects, key=lambda item: item["name"])


def source_material(source_identity: dict, reference: str) -> dict[str, Any]:
    return {
        "uri": f"git+{PROJECT_REPOSITORY}.git@{reference}",
        "name": "SIGUI source",
        "digest": {"gitCommit": source_identity["commit"]},
    }


def submodule_material(module: dict) -> dict[str, Any]:
    return {
        "uri": f"git+{module['url']}@{module['commit']}",
        "name": module["path"],
        "digest": {"gitCommit": module["commit"]},
    }


def file_material(root: Path, relative: str, source_commit: str) -> dict[str, Any]:
    path = safe_file(root, root / relative, f"provenance material {relative}")
    encoded = quote(relative.replace("\\", "/"), safe="/")
    return {
        "uri": f"{PROJECT_REPOSITORY}/blob/{source_commit}/{encoded}",
        "name": relative.replace("\\", "/"),
        "digest": {"sha256": sha256_file(path)},
    }


def collect_materials(
    root: Path, source_identity: dict, source_reference: str
) -> list[dict[str, Any]]:
    materials = [source_material(source_identity, source_reference)]
    materials.extend(submodule_material(module) for module in source_identity["submodules"])
    materials.extend(
        file_material(root, relative, source_identity["commit"])
        for relative in sorted(REQUIRED_SOURCE_INPUTS)
    )
    return sorted(materials, key=lambda item: (item["uri"], item["name"]))


def positive_run_identity(value: object, field: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"[1-9][0-9]*", value) is None:
        raise ValueError(f"{field} must be an exact positive decimal string")
    return value


def core_external_parameters(manifest: dict, source_commit: str) -> dict[str, str]:
    if manifest.get("release_profile") != CORE_RELEASE_PROFILE:
        raise ValueError("Core provenance requires release_profile=core_1_0")
    if exact_sha(manifest.get("firmware_commit"), "Core firmware commit") != source_commit:
        raise ValueError("Core firmware commit does not match provenance source")

    workflow = manifest.get("workflow")
    if not isinstance(workflow, dict):
        raise ValueError("Core provenance requires an exact Actions workflow identity")
    if exact_sha(workflow.get("sha"), "Core workflow source commit") != source_commit:
        raise ValueError("Core workflow source commit does not match provenance source")
    if workflow.get("repository") != CANONICAL_REPOSITORY:
        raise ValueError(
            "Core workflow repository does not match the canonical source repository"
        )
    if workflow.get("workflow") != WORKFLOW_NAME:
        raise ValueError("Core workflow name must be d1l-ci")
    if workflow.get("path") not in {None, WORKFLOW_PATH}:
        raise ValueError("Core workflow path is not canonical")

    run_id = positive_run_identity(workflow.get("run_id"), "Core workflow run ID")
    run_attempt = positive_run_identity(
        workflow.get("run_attempt"), "Core workflow run attempt"
    )
    if positive_run_identity(manifest.get("actions_run"), "Core package Actions run") != run_id:
        raise ValueError("Core package Actions run does not match workflow run")
    if (
        positive_run_identity(
            manifest.get("actions_run_attempt"), "Core package Actions run attempt"
        )
        != run_attempt
    ):
        raise ValueError("Core package Actions run attempt does not match workflow run")
    expected_run_url = (
        f"https://github.com/{CANONICAL_REPOSITORY}/actions/runs/{run_id}"
    )
    if workflow.get("run_url") != expected_run_url:
        raise ValueError("Core workflow run URL is not canonical")

    return {
        "sourceRepository": PROJECT_REPOSITORY,
        "sourceRevision": source_commit,
        "releaseProfile": CORE_RELEASE_PROFILE,
        "workflowRepository": CANONICAL_REPOSITORY,
        "workflowName": WORKFLOW_NAME,
        "workflowPath": WORKFLOW_PATH,
        "workflowRunId": run_id,
        "workflowRunAttempt": run_attempt,
    }


def workflow_context(
    manifest: dict, source_commit: str
) -> tuple[str, str, dict[str, str]]:
    workflow = manifest.get("workflow")
    if manifest.get("release_profile") == CORE_RELEASE_PROFILE:
        parameters = core_external_parameters(manifest, source_commit)
        reference = workflow.get("ref") if isinstance(workflow, dict) else None
        if not isinstance(reference, str) or not reference.strip():
            raise ValueError("Core workflow ref is missing or malformed")
        return reference, GITHUB_HOSTED_BUILDER, parameters

    parameters = {
        "sourceRepository": PROJECT_REPOSITORY,
        "sourceRevision": source_commit,
        "releaseProfile": DEFAULT_RELEASE_PROFILE,
    }
    if not isinstance(workflow, dict):
        return source_commit, LOCAL_BUILDER, parameters

    sha = workflow.get("sha")
    if sha is not None and exact_sha(sha, "workflow source commit") != source_commit:
        raise ValueError("Workflow source commit does not match provenance source")
    repository = workflow.get("repository")
    if repository is not None and repository != CANONICAL_REPOSITORY:
        raise ValueError("Workflow repository does not match the canonical source repository")
    reference = workflow.get("ref")
    if reference is not None and (not isinstance(reference, str) or not reference.strip()):
        raise ValueError("Workflow ref is malformed")
    run_id = workflow.get("run_id")
    if run_id is None:
        return reference or source_commit, LOCAL_BUILDER, parameters
    if not str(run_id).isdigit() or not repository:
        raise ValueError("GitHub Actions provenance has incomplete run identity")
    return reference or source_commit, GITHUB_HOSTED_BUILDER, parameters


def build_statement(
    root: Path,
    package_dir: Path,
    package_manifest: dict,
    source_identity: dict,
) -> dict[str, Any]:
    identity = normalize_source_identity(source_identity)
    source_commit = identity["commit"]
    subjects = collect_subjects(root, package_dir, package_manifest, identity)
    reference, builder_id, external_parameters = workflow_context(
        package_manifest, source_commit
    )
    materials = collect_materials(root.resolve(), identity, reference)
    statement = {
        "_type": STATEMENT_TYPE,
        "subject": subjects,
        "predicateType": PREDICATE_TYPE,
        "predicate": {
            "buildDefinition": {
                "buildType": BUILD_TYPE,
                "externalParameters": external_parameters,
                "resolvedDependencies": materials,
            },
            "runDetails": {"builder": {"id": builder_id}},
            "sigui_attestation": dict(ATTESTATION_EXTENSION),
        },
    }
    errors = validate_profile(statement, source_commit)
    if errors:
        raise ValueError("Generated provenance profile is invalid: " + "; ".join(errors))
    return statement


def valid_resource_descriptor(value: object) -> bool:
    if not isinstance(value, dict):
        return False
    if not isinstance(value.get("uri"), str) or not value["uri"]:
        return False
    if not isinstance(value.get("name"), str) or not value["name"]:
        return False
    digest = value.get("digest")
    if not isinstance(digest, dict) or len(digest) != 1:
        return False
    if "sha256" in digest:
        return SHA256_RE.fullmatch(str(digest["sha256"])) is not None
    if "gitCommit" in digest:
        try:
            exact_sha(digest["gitCommit"], "resource git commit")
        except ValueError:
            return False
        return True
    return False


def validate_profile(statement: object, source_commit: str) -> list[str]:
    errors: list[str] = []
    commit = exact_sha(source_commit, "expected provenance source commit")
    if not isinstance(statement, dict):
        return ["provenance statement must be an object"]
    if statement.get("_type") != STATEMENT_TYPE:
        errors.append("in-toto statement type is missing or invalid")
    if statement.get("predicateType") != PREDICATE_TYPE:
        errors.append("SLSA predicate type is missing or invalid")

    subjects = statement.get("subject")
    if not isinstance(subjects, list) or not subjects:
        errors.append("provenance subjects are missing")
        subjects = []
    names: list[str] = []
    for item in subjects:
        if not isinstance(item, dict) or set(item) != {"name", "digest"}:
            errors.append("provenance subject has an invalid shape")
            continue
        name = item.get("name")
        digest = item.get("digest")
        if (
            not isinstance(name, str)
            or not name
            or name.startswith(("/", "../"))
            or "\\" in name
            or not isinstance(digest, dict)
            or set(digest) != {"sha256"}
            or SHA256_RE.fullmatch(str(digest.get("sha256") or "")) is None
        ):
            errors.append("provenance subject name or digest is invalid")
            continue
        names.append(name)
    if names != sorted(names) or len(names) != len(set(names)):
        errors.append("provenance subjects are not sorted and unique")

    predicate = statement.get("predicate")
    if not isinstance(predicate, dict):
        return errors + ["SLSA predicate is missing"]
    definition = predicate.get("buildDefinition")
    if not isinstance(definition, dict) or definition.get("buildType") != BUILD_TYPE:
        errors.append("build definition or build type is missing or invalid")
        definition = {}
    parameters = definition.get("externalParameters")
    if not isinstance(parameters, dict):
        errors.append("external parameters are incomplete or unexpected")
        parameters = {}
    release_profile = parameters.get("releaseProfile")
    if release_profile == CORE_RELEASE_PROFILE:
        if set(parameters) != CORE_EXTERNAL_PARAMETER_KEYS:
            errors.append("Core external parameters are incomplete or unexpected")
        if (
            parameters.get("sourceRepository") != PROJECT_REPOSITORY
            or parameters.get("sourceRevision") != commit
            or parameters.get("workflowRepository") != CANONICAL_REPOSITORY
            or parameters.get("workflowName") != WORKFLOW_NAME
            or parameters.get("workflowPath") != WORKFLOW_PATH
        ):
            errors.append("Core source or workflow identity is invalid")
        try:
            positive_run_identity(
                parameters.get("workflowRunId"), "Core provenance workflow run ID"
            )
            positive_run_identity(
                parameters.get("workflowRunAttempt"),
                "Core provenance workflow run attempt",
            )
        except ValueError:
            errors.append("Core workflow run identity is invalid")
    else:
        expected_parameters = {
            "sourceRepository": PROJECT_REPOSITORY,
            "sourceRevision": commit,
            "releaseProfile": DEFAULT_RELEASE_PROFILE,
        }
        if parameters != expected_parameters:
            errors.append("external parameters are incomplete or unexpected")
    materials = definition.get("resolvedDependencies")
    if not isinstance(materials, list) or not materials:
        errors.append("resolved dependencies are missing")
        materials = []
    elif any(not valid_resource_descriptor(item) for item in materials):
        errors.append("resolved dependency descriptor is invalid")
    material_keys = [
        (item.get("uri"), item.get("name"))
        for item in materials
        if isinstance(item, dict)
    ]
    if material_keys != sorted(material_keys) or len(material_keys) != len(set(material_keys)):
        errors.append("resolved dependencies are not sorted and unique")

    run_details = predicate.get("runDetails")
    builder = run_details.get("builder") if isinstance(run_details, dict) else None
    builder_id = builder.get("id") if isinstance(builder, dict) else None
    if builder_id not in {GITHUB_HOSTED_BUILDER, LOCAL_BUILDER}:
        errors.append("builder identity is missing or not allowlisted")
    if release_profile == CORE_RELEASE_PROFILE and builder_id != GITHUB_HOSTED_BUILDER:
        errors.append("Core provenance requires the GitHub-hosted builder")
    if isinstance(run_details, dict) and "metadata" in run_details:
        errors.append("deterministic profile must omit invocation metadata")
    if predicate.get("sigui_attestation") != ATTESTATION_EXTENSION:
        errors.append("unsigned-attestation limitation is missing or invalid")
    return errors


def validate_core_actions_binding(
    statement: object,
    source_commit: str,
    run_id: object,
    run_attempt: object,
) -> list[str]:
    """Validate exact Core profile and Actions invocation semantics."""

    commit = exact_sha(source_commit, "expected Core provenance source commit")
    expected_run_id = positive_run_identity(run_id, "expected Core workflow run ID")
    expected_run_attempt = positive_run_identity(
        run_attempt, "expected Core workflow run attempt"
    )
    errors = validate_profile(statement, commit)
    parameters: object = None
    if isinstance(statement, dict):
        predicate = statement.get("predicate")
        definition = (
            predicate.get("buildDefinition")
            if isinstance(predicate, dict)
            else None
        )
        parameters = (
            definition.get("externalParameters")
            if isinstance(definition, dict)
            else None
        )
    expected = {
        "sourceRepository": PROJECT_REPOSITORY,
        "sourceRevision": commit,
        "releaseProfile": CORE_RELEASE_PROFILE,
        "workflowRepository": CANONICAL_REPOSITORY,
        "workflowName": WORKFLOW_NAME,
        "workflowPath": WORKFLOW_PATH,
        "workflowRunId": expected_run_id,
        "workflowRunAttempt": expected_run_attempt,
    }
    if parameters != expected:
        errors.append(
            "provenance is not bound to the exact Core Actions workflow invocation"
        )
    return errors


def validate_against_inputs(
    statement: object,
    root: Path,
    package_dir: Path,
    package_manifest: dict,
    source_identity: dict,
) -> list[str]:
    identity = normalize_source_identity(source_identity)
    errors = validate_profile(statement, identity["commit"])
    try:
        expected = build_statement(root, package_dir, package_manifest, identity)
    except (FileNotFoundError, OSError, ValueError) as exc:
        errors.append(f"cannot rebuild deterministic provenance: {exc}")
        return errors
    if statement != expected:
        errors.append("provenance does not match deterministic source and package inputs")
    return errors


def write_package_provenance(
    root: Path,
    package_dir: Path,
    package_manifest: dict,
    *,
    source_identity: dict | None = None,
    expected_source_sha: str | None = None,
) -> dict[str, Any]:
    identity = (
        normalize_source_identity(source_identity)
        if source_identity is not None
        else discover_source_identity(root, expected_source_sha)
    )
    if expected_source_sha is not None and identity["commit"] != exact_sha(
        expected_source_sha, "expected provenance source commit"
    ):
        raise ValueError("supplied provenance identity does not match expected source commit")
    statement = build_statement(root, package_dir, package_manifest, identity)
    path = package_dir / f"provenance_{identity['commit']}.json"
    path.write_text(canonical_json(statement), encoding="ascii")
    loaded = load_json(path)
    errors = validate_against_inputs(loaded, root, package_dir, package_manifest, identity)
    if errors:
        raise ValueError("Written provenance failed validation: " + "; ".join(errors))
    materials = statement["predicate"]["buildDefinition"]["resolvedDependencies"]
    metadata = {
        "path": path.relative_to(package_dir).as_posix(),
        "sha256": sha256_file(path),
        "statement_type": STATEMENT_TYPE,
        "predicate_type": PREDICATE_TYPE,
        "build_type": BUILD_TYPE,
        "source_commit": identity["commit"],
        "subject_count": len(statement["subject"]),
        "material_count": len(materials),
        "authenticated": False,
        "slsa_build_level": "not-claimed",
        "valid": True,
    }
    parameters = statement["predicate"]["buildDefinition"]["externalParameters"]
    metadata["release_profile"] = parameters["releaseProfile"]
    if parameters["releaseProfile"] == CORE_RELEASE_PROFILE:
        metadata.update(
            {
                "workflow_repository": parameters["workflowRepository"],
                "workflow_name": parameters["workflowName"],
                "workflow_path": parameters["workflowPath"],
                "workflow_run_id": parameters["workflowRunId"],
                "workflow_run_attempt": parameters["workflowRunAttempt"],
            }
        )
    return metadata


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--package-dir", required=True)
    parser.add_argument("--package-manifest", required=True)
    parser.add_argument("--out")
    parser.add_argument("--validate")
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    package_dir = Path(args.package_dir).resolve()
    package_manifest = load_json(Path(args.package_manifest))
    identity = discover_source_identity(root, args.source_sha)

    if args.validate:
        path = Path(args.validate).resolve()
        statement = load_json(path)
        errors = validate_against_inputs(
            statement, root, package_dir, package_manifest, identity
        )
        print(json.dumps({"ok": not errors, "path": str(path), "errors": errors}))
        return 1 if errors else 0

    statement = build_statement(root, package_dir, package_manifest, identity)
    out = (
        Path(args.out).resolve()
        if args.out
        else package_dir / f"provenance_{identity['commit']}.json"
    )
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(canonical_json(statement), encoding="ascii")
    print(json.dumps({"ok": True, "path": str(out), "sha256": sha256_file(out)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
