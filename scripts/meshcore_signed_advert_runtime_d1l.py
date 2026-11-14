#!/usr/bin/env python3
"""Build and run the bounded pinned-MeshCore signed-advert semantic gate."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import tarfile
import tempfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any, Sequence


ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = (
    ROOT / "tests" / "meshcore_signed_advert_runtime" / "manifest.json"
)
HARNESS_PATH = (
    ROOT
    / "tests"
    / "meshcore_signed_advert_runtime"
    / "meshcore_signed_advert_runtime.cpp"
)
RNG_STUB_PATH = (
    ROOT / "tests" / "meshcore_signed_advert_runtime" / "crypto_rng_stub.cpp"
)
MBEDTLS_SHA256_BRIDGE_PATH = (
    ROOT
    / "tests"
    / "meshcore_signed_advert_runtime"
    / "mbedtls_md_sha256_bridge.cpp"
)
D1L_PACKET_HASH_SOURCE = ROOT / "main" / "mesh" / "meshcore_packet_hash.c"

MESH_CPP_SOURCES = (
    ROOT / "third_party" / "MeshCore" / "src" / "Identity.cpp",
    ROOT / "third_party" / "MeshCore" / "src" / "Packet.cpp",
    ROOT / "third_party" / "MeshCore" / "src" / "Dispatcher.cpp",
    ROOT / "third_party" / "MeshCore" / "src" / "Mesh.cpp",
    ROOT / "third_party" / "MeshCore" / "src" / "Utils.cpp",
    ROOT
    / "third_party"
    / "MeshCore"
    / "src"
    / "helpers"
    / "AdvertDataHelpers.cpp",
    ROOT
    / "third_party"
    / "MeshCore"
    / "src"
    / "helpers"
    / "TxtDataHelpers.cpp",
    ROOT
    / "third_party"
    / "MeshCore"
    / "src"
    / "helpers"
    / "BaseChatMesh.cpp",
)
ED25519_ROOT = ROOT / "third_party" / "MeshCore" / "lib" / "ed25519"
ED25519_DEFINED_ROOT = ROOT / "overlays" / "meshcore_ed25519_defined"
ED25519_C_SOURCES = tuple(
    (
        *(ED25519_DEFINED_ROOT / name for name in ("fe.c", "ge.c", "sc.c")),
        *(ED25519_ROOT / name for name in (
            "sha512.c",
            "verify.c",
            "key_exchange.c",
            "keypair.c",
            "sign.c",
        )),
    )
)
SIGNED_ADVERT_SANITIZER_POLICY = {
    "requested": ["address", "undefined"],
    "full_ubsan_clean": True,
    "exceptions": [],
    "source_level_remediation": {
        "id": "BLK-WP04-ED25519-SHIFT-UB-20260714",
        "status": "resolved",
        "upstream_commit": "e8d3c53ba1ea863937081cd0caad759b832f3028",
        "overlay_path": "overlays/meshcore_ed25519_defined",
        "validator": "scripts/validate_ed25519_defined_overlay.py",
        "transformed_expressions": 215,
    },
}
SIGNED_ADVERT_EVIDENCE_PROFILE = (
    "SIGUI MeshCore signed-advert semantic runtime canonical evidence v1"
)
SIGNED_ADVERT_ARTIFACT_TYPE = "d1l_meshcore_signed_advert_semantic_runtime"
AES_SOURCE = (
    ROOT
    / "third_party"
    / "sensecap_indicator_esp32"
    / "components"
    / "LoRaWAN"
    / "soft-se"
    / "aes.c"
)
EXTERNAL_CPP_SOURCES = (
    "Crypto.cpp",
    "Hash.cpp",
    "SHA512.cpp",
    "BigNumberUtil.cpp",
    "Curve25519.cpp",
    "Ed25519.cpp",
)


class GateFailure(RuntimeError):
    """The semantic gate failed closed."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def sha256_lf_text_file(path: Path) -> str:
    """Hash tracked text using the repository's canonical LF representation."""
    return sha256_bytes(path.read_bytes().replace(b"\r\n", b"\n"))


def git_output(*args: str) -> str:
    try:
        completed = subprocess.run(
            ["git", *args],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise GateFailure(f"git command failed: {' '.join(args)}") from exc
    return completed.stdout.strip()


def load_manifest(path: Path = MANIFEST_PATH) -> dict[str, Any]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateFailure(f"cannot read signed-advert runtime manifest: {exc}") from exc
    if manifest.get("schema_version") != 1:
        raise GateFailure("signed-advert runtime manifest schema drifted")
    if manifest.get("work_package") != "WP-04":
        raise GateFailure("signed-advert runtime work package drifted")
    if manifest.get("capability") != "identity_signed_advert_semantic_runtime":
        raise GateFailure("signed-advert runtime capability drifted")
    if manifest.get("wp04_closure_eligible") is not False:
        raise GateFailure("signed-advert runtime must not close WP-04")
    if manifest.get("closure_ready") is not False:
        raise GateFailure("signed-advert runtime must remain fail closed")
    if manifest.get("source_hash_mode") != "canonical_lf_text_sha256":
        raise GateFailure("signed-advert runtime source hash mode drifted")
    dependency = manifest.get("external_dependency", {})
    if (
        dependency.get("name") != "rweather/Crypto"
        or dependency.get("version") != "0.4.0"
        or dependency.get("registry_version_id") != 43204
        or dependency.get("release_commit")
        != "61e84b220fc8dfe83c2e04716d0fa88cfaddadf6"
        or dependency.get("archive_url")
        != "https://dl.registry.platformio.org/download/rweather/library/Crypto/0.4.0/Crypto-0.4.0.tar.gz"
        or dependency.get("archive_size") != 162696
        or dependency.get("archive_sha256")
        != "1867740aad0d61bdcbac25f6dbc8eefe6eed9e7b37f48d9d0b9d80500ad431e0"
    ):
        raise GateFailure("signed-advert external dependency pin drifted")
    if manifest.get("sanitizer_policy") != SIGNED_ADVERT_SANITIZER_POLICY:
        raise GateFailure("signed-advert sanitizer policy drifted")
    return manifest


def _source_groups(manifest: dict[str, Any]) -> Sequence[dict[str, str]]:
    return (
        manifest["upstream"]["sources"],
        manifest["vendored_crypto_sources"],
        manifest["host_sources"],
    )


def verify_repository_sources(
    manifest: dict[str, Any], expected_commit: str | None = None
) -> dict[str, Any]:
    files: dict[str, Any] = {}
    failures: list[str] = []
    seen: set[str] = set()
    root = ROOT.resolve()
    for group in _source_groups(manifest):
        if not isinstance(group, dict):
            raise GateFailure("signed-advert source registry is not an object")
        for relative, expected_hash in group.items():
            if (
                not isinstance(relative, str)
                or "\\" in relative
                or relative in seen
                or not re.fullmatch(r"[A-Za-z0-9_./-]+", relative)
            ):
                raise GateFailure("signed-advert source path registry is invalid")
            seen.add(relative)
            source = (ROOT / relative).resolve()
            try:
                source.relative_to(root)
            except ValueError as exc:
                raise GateFailure(f"source escapes repository root: {relative}") from exc
            actual_hash = sha256_lf_text_file(source) if source.is_file() else None
            matched = actual_hash == expected_hash
            files[relative] = {
                "expected_sha256": expected_hash,
                "actual_sha256": actual_hash,
                "matched": matched,
            }
            if not matched:
                failures.append(f"source hash mismatch: {relative}")

    upstream = manifest["upstream"]
    upstream_root = ROOT / upstream["path"]
    upstream_head = git_output("-C", str(upstream_root), "rev-parse", "HEAD")
    if upstream_head != upstream["commit"]:
        failures.append("checked-out MeshCore commit does not match runtime manifest")
    gitlink = git_output("ls-tree", "HEAD", "--", upstream["path"])
    match = re.fullmatch(r"160000 commit ([0-9a-f]{40})\t.+", gitlink)
    gitlink_commit = match.group(1) if match else None
    if gitlink_commit != upstream["commit"]:
        failures.append("MeshCore gitlink does not match runtime manifest")
    repository_commit = git_output("rev-parse", "HEAD")
    if expected_commit and repository_commit != expected_commit:
        failures.append(
            f"repository commit {repository_commit} does not match {expected_commit}"
        )
    if failures:
        raise GateFailure("; ".join(failures))
    return {
        "verified": True,
        "repository_commit": repository_commit,
        "expected_repository_commit": expected_commit,
        "upstream_commit": upstream_head,
        "gitlink_commit": gitlink_commit,
        "source_hash_mode": manifest["source_hash_mode"],
        "files": files,
    }


def obtain_external_archive(
    dependency: dict[str, Any], archive_path: Path | None = None
) -> tuple[bytes, dict[str, Any]]:
    # The registry no longer permits direct downloads. Keep its exact pinned
    # archive with the host fixture so CI never depends on that download API.
    archive_path = archive_path or MANIFEST_PATH.parent / "Crypto-0.4.0.tar.gz"
    try:
        archive = archive_path.read_bytes()
    except OSError as exc:
        raise GateFailure(f"cannot read external dependency archive: {exc}") from exc
    source = str(archive_path.resolve())
    actual_hash = sha256_bytes(archive)
    if len(archive) != dependency["archive_size"]:
        raise GateFailure("pinned Crypto archive size mismatch")
    if actual_hash != dependency["archive_sha256"]:
        raise GateFailure("pinned Crypto archive SHA256 mismatch")
    return archive, {
        "verified": True,
        "source": source,
        "url": dependency["archive_url"],
        "size": len(archive),
        "sha256": actual_hash,
        "version": dependency["version"],
        "registry_version_id": dependency["registry_version_id"],
        "release_commit": dependency["release_commit"],
    }


def extract_external_sources(
    archive: bytes, dependency: dict[str, Any], destination: Path
) -> dict[str, Any]:
    expected_sources = dependency["sources"]
    if not isinstance(expected_sources, dict) or not expected_sources:
        raise GateFailure("external source registry is empty")
    extracted: dict[str, Any] = {}
    destination.mkdir(parents=True, exist_ok=True)
    try:
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as bundle:
            members: dict[str, tarfile.TarInfo] = {}
            for member in bundle.getmembers():
                path = PurePosixPath(member.name)
                if path.is_absolute() or ".." in path.parts:
                    raise GateFailure("external archive contains an unsafe path")
                if member.name in members:
                    raise GateFailure("external archive contains duplicate paths")
                members[member.name] = member
            for relative, expected_hash in expected_sources.items():
                path = PurePosixPath(relative)
                if path.is_absolute() or ".." in path.parts or "\\" in relative:
                    raise GateFailure("external source path registry is invalid")
                member = members.get(relative)
                if member is None or not member.isfile():
                    raise GateFailure(f"external archive source is missing: {relative}")
                source = bundle.extractfile(member)
                if source is None:
                    raise GateFailure(f"cannot read external archive source: {relative}")
                data = source.read()
                actual_hash = sha256_bytes(data)
                if actual_hash != expected_hash:
                    raise GateFailure(f"external source hash mismatch: {relative}")
                output = destination.joinpath(*path.parts)
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(data)
                extracted[relative] = {
                    "sha256": actual_hash,
                    "size": len(data),
                }
    except (tarfile.TarError, OSError) as exc:
        raise GateFailure(f"cannot extract pinned Crypto sources: {exc}") from exc
    return {"verified": True, "files": extracted}


def command_plan(
    cc: str,
    cxx: str,
    external_root: str = "$EXTERNAL_ROOT",
    build_root: str = "$BUILD_ROOT",
    *,
    sanitize: bool,
) -> list[list[str]]:
    build = Path(build_root)
    external = Path(external_root)
    include_flags = [
        "-I",
        str(ROOT / "tests" / "meshcore_signed_advert_runtime" / "stubs"),
        "-I",
        str(ROOT / "tests" / "meshcore_oracle" / "stubs"),
        "-I",
        str(ROOT / "tests" / "native" / "stubs"),
        "-I",
        str(ROOT / "main"),
        "-I",
        str(external),
        "-I",
        str(ROOT / "third_party" / "MeshCore" / "src"),
        "-I",
        str(ED25519_ROOT),
    ]
    sanitizer_flags = ["-fsanitize=address,undefined"] if sanitize else []
    c_flags = [
        "-std=c11",
        "-O1",
        "-g",
        "-fno-omit-frame-pointer",
        "-ffunction-sections",
        "-fdata-sections",
        *sanitizer_flags,
        "-Wall",
        "-Wextra",
        "-Werror",
    ]
    imported_cxx_flags = [
        "-std=c++17",
        "-O1",
        "-g",
        "-fno-omit-frame-pointer",
        "-ffunction-sections",
        "-fdata-sections",
        *sanitizer_flags,
    ]
    harness_flags = [
        *imported_cxx_flags,
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unknown-warning-option",
        "-Wno-unused-parameter",
        "-Wno-reorder",
        "-Wno-class-memaccess",
    ]
    commands: list[list[str]] = []
    objects: list[str] = []
    c_sources = (*ED25519_C_SOURCES, AES_SOURCE, D1L_PACKET_HASH_SOURCE)
    for index, source in enumerate(c_sources):
        output = str(build / f"c_{index}.o")
        flags = list(c_flags)
        if source == AES_SOURCE:
            flags.append("-DAES_DEC_PREKEYED")
        commands.append(
            [cc, *flags, *include_flags, "-c", str(source), "-o", output]
        )
        objects.append(output)
    cpp_sources = [
        *MESH_CPP_SOURCES,
        *(external / name for name in EXTERNAL_CPP_SOURCES),
        RNG_STUB_PATH,
        MBEDTLS_SHA256_BRIDGE_PATH,
        HARNESS_PATH,
    ]
    for index, source in enumerate(cpp_sources):
        output = str(build / f"cpp_{index}.o")
        flags = (
            harness_flags
            if source in (RNG_STUB_PATH, MBEDTLS_SHA256_BRIDGE_PATH, HARNESS_PATH)
            else imported_cxx_flags
        )
        commands.append(
            [cxx, *flags, *include_flags, "-c", str(source), "-o", output]
        )
        objects.append(output)
    executable = str(build / "meshcore_signed_advert_runtime")
    if os.name == "nt":
        executable += ".exe"
    commands.append(
        [
            cxx,
            *objects,
            *sanitizer_flags,
            "-Wl,--gc-sections",
            "-o",
            executable,
        ]
    )
    commands.append([executable])
    return commands


def canonical_command_plan(commands: object) -> list[list[str]] | None:
    """Project an exact command plan onto relocatable root tokens.

    The Actions producer, firmware consumer, and later release auditor can use
    different absolute checkout and temporary paths. Only those three roots
    are relocatable; every compiler, source-relative path, flag, argument,
    command ordering decision, and output-relative path remains exact.
    """

    if not isinstance(commands, list) or not commands or not all(
        isinstance(command, list)
        and command
        and all(isinstance(argument, str) for argument in command)
        for command in commands
    ):
        return None

    c_source_count = len(ED25519_C_SOURCES) + 2  # pinned AES plus D1L packet hash
    try:
        first_c = commands[0]
        first_cpp = commands[c_source_count]
        source_index = first_c.index("-c") + 1
        output_index = first_c.index("-o") + 1
        first_relative = str(ED25519_C_SOURCES[0].relative_to(ROOT)).replace(
            "\\", "/"
        )
        first_source = first_c[source_index].replace("\\", "/")
        suffix = f"/{first_relative}"
        if not first_source.endswith(suffix):
            return None
        repository_root = first_source[: -len(suffix)]
        first_external = commands[c_source_count + len(MESH_CPP_SOURCES)]
        external_source_index = first_external.index("-c") + 1
        external_source = first_external[external_source_index].replace("\\", "/")
        external_suffix = f"/{EXTERNAL_CPP_SOURCES[0]}"
        if not external_source.endswith(external_suffix):
            return None
        external_root = external_source[: -len(external_suffix)].rstrip("/")
        first_output = first_c[output_index].replace("\\", "/")
        if "/" not in first_output:
            return None
        build_root = first_output.rsplit("/", 1)[0]
    except (IndexError, ValueError):
        return None

    def absolute_root(value: str) -> bool:
        return bool(
            value
            and (value.startswith("/") or re.fullmatch(r"[A-Za-z]:/.*", value))
        )

    roots = {
        "$REPOSITORY": repository_root.rstrip("/"),
        "$EXTERNAL": external_root,
        "$BUILD": build_root.rstrip("/"),
    }
    if (
        not all(absolute_root(value) for value in roots.values())
        or len(set(roots.values())) != len(roots)
        or first_c[0] != "clang-18"
        or first_cpp[0] != "clang++-18"
    ):
        return None

    try:
        link_output = commands[-2][commands[-2].index("-o") + 1].replace(
            "\\", "/"
        )
        executed_output = commands[-1][0].replace("\\", "/")
    except (IndexError, ValueError):
        return None
    if len(commands[-1]) != 1 or link_output != executed_output:
        return None

    ordered_roots = sorted(roots.items(), key=lambda item: len(item[1]), reverse=True)

    def project(argument: str) -> str:
        normalized = argument.replace("\\", "/")
        for token, root in ordered_roots:
            if normalized == root:
                normalized = token
                break
            if normalized.startswith(f"{root}/"):
                normalized = f"{token}{normalized[len(root):]}"
                break
        if normalized == "$BUILD/meshcore_signed_advert_runtime.exe":
            return "$BUILD/meshcore_signed_advert_runtime"
        return normalized

    return [[project(argument) for argument in command] for command in commands]


def run_command(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            list(command),
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:
        raise GateFailure(f"cannot execute command: {command[0]}") from exc
    except subprocess.CalledProcessError as exc:
        detail = (exc.stderr or exc.stdout or "").strip()[-4000:]
        raise GateFailure(f"command failed ({command[0]}): {detail}") from exc


def build_and_run(
    cc: str,
    cxx: str,
    external_root: Path,
    build_root: Path,
    expected_result: dict[str, Any],
    *,
    sanitize: bool,
) -> tuple[dict[str, Any], list[list[str]]]:
    commands = command_plan(
        cc,
        cxx,
        str(external_root),
        str(build_root),
        sanitize=sanitize,
    )
    runtime_output = ""
    for command in commands:
        completed = run_command(command)
        if len(command) == 1:
            runtime_output = completed.stdout.strip()
    try:
        result = json.loads(runtime_output)
    except json.JSONDecodeError as exc:
        raise GateFailure("signed-advert runtime did not emit one JSON result") from exc
    if result != expected_result:
        raise GateFailure("signed-advert runtime result drifted from the manifest")
    return result, commands


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def validate_completed_report(
    report: object,
    expected_commit: str,
    *,
    require_generated_at: bool = True,
    require_commands: bool = True,
) -> None:
    """Reject incomplete or weak signed-advert runtime receipts."""

    manifest = load_manifest()
    if not isinstance(report, dict):
        raise ValueError("signed-advert runtime report must be an object")
    repository = report.get("repository")
    repository = repository if isinstance(repository, dict) else {}
    files = repository.get("files")
    files = files if isinstance(files, dict) else {}
    expected_files = {
        path: digest for group in _source_groups(manifest) for path, digest in group.items()
    }
    repository_files_exact = set(files) == set(expected_files) and all(
        isinstance(files[path], dict)
        and files[path].get("expected_sha256") == digest
        and files[path].get("actual_sha256") == digest
        and files[path].get("matched") is True
        for path, digest in expected_files.items()
    )

    dependency = manifest["external_dependency"]
    archive = report.get("external_archive")
    archive = archive if isinstance(archive, dict) else {}
    external = report.get("external_sources")
    external = external if isinstance(external, dict) else {}
    external_files = external.get("files")
    external_files = external_files if isinstance(external_files, dict) else {}
    external_files_exact = set(external_files) == set(dependency["sources"]) and all(
        isinstance(external_files[path], dict)
        and external_files[path].get("sha256") == digest
        and isinstance(external_files[path].get("size"), int)
        and external_files[path]["size"] > 0
        for path, digest in dependency["sources"].items()
    )

    commands = report.get("commands")
    if require_commands:
        expected_commands = command_plan(
            "clang-18",
            "clang++-18",
            "/d1l-external-root",
            "/d1l-build-root",
            sanitize=True,
        )
        actual_plan = canonical_command_plan(commands)
        expected_plan = canonical_command_plan(expected_commands)
        commands_valid = (
            actual_plan is not None
            and expected_plan is not None
            and actual_plan == expected_plan
        )
    else:
        commands_valid = commands is None

    required = {
        "schema_version": report.get("schema_version") == 1,
        "artifact_type": report.get("artifact_type")
        == SIGNED_ADVERT_ARTIFACT_TYPE,
        "status": report.get("status") == "pass",
        "passed": report.get("passed") is True,
        "execution_complete": report.get("execution_complete") is True,
        "work_package": report.get("work_package") == "WP-04",
        "capability": report.get("capability") == manifest["capability"],
        "coverage_boundary": report.get("coverage_boundary")
        == manifest["coverage_boundary"],
        "wp04_closure_eligible_false": report.get("wp04_closure_eligible") is False,
        "closure_ready_false": report.get("closure_ready") is False,
        "repository_verified": repository.get("verified") is True,
        "repository_commit": repository.get("repository_commit") == expected_commit,
        "expected_repository_commit": repository.get("expected_repository_commit")
        == expected_commit,
        "upstream_commit": repository.get("upstream_commit")
        == manifest["upstream"]["commit"],
        "gitlink_commit": repository.get("gitlink_commit")
        == manifest["upstream"]["commit"],
        "source_hash_mode": repository.get("source_hash_mode")
        == manifest["source_hash_mode"],
        "repository_files": repository_files_exact,
        "external_archive": archive.get("verified") is True
        and archive.get("url") == dependency["archive_url"]
        and archive.get("size") == dependency["archive_size"]
        and archive.get("sha256") == dependency["archive_sha256"]
        and archive.get("version") == dependency["version"]
        and archive.get("registry_version_id") == dependency["registry_version_id"]
        and archive.get("release_commit") == dependency["release_commit"],
        "external_sources": external.get("verified") is True and external_files_exact,
        "sanitizers_enabled": report.get("sanitizers_enabled") is True,
        "sanitizer_policy": report.get("sanitizer_policy")
        == SIGNED_ADVERT_SANITIZER_POLICY,
        "full_ubsan_clean": report.get("full_ubsan_clean") is True,
        "commands": commands_valid,
        "result": report.get("result") == manifest["expected_result"],
        "assertions": report.get("assertions") == manifest["assertions"],
        "residual_gaps": report.get("residual_gaps") == manifest["residual_gaps"],
    }
    if require_generated_at:
        generated_at = report.get("generated_at")
        try:
            parsed = datetime.fromisoformat(str(generated_at).replace("Z", "+00:00"))
            required["generated_at"] = parsed.tzinfo is not None
        except (ValueError, OverflowError):
            required["generated_at"] = False
    elif "generated_at" in report:
        required["generated_at_absent"] = False
    failed = [name for name, passed in required.items() if not passed]
    if failed:
        raise ValueError(
            "signed-advert runtime validation failed: " + ", ".join(failed)
        )


def canonicalize_release_report(report: object, expected_commit: str) -> dict[str, Any]:
    """Return the deterministic semantic projection stored in release packages."""

    validate_completed_report(report, expected_commit)
    assert isinstance(report, dict)
    archive = dict(report["external_archive"])
    archive.pop("source", None)
    return {
        "schema_version": report["schema_version"],
        "artifact_type": report["artifact_type"],
        "evidence_profile": SIGNED_ADVERT_EVIDENCE_PROFILE,
        "status": report["status"],
        "passed": report["passed"],
        "execution_complete": report["execution_complete"],
        "work_package": report["work_package"],
        "capability": report["capability"],
        "coverage_boundary": report["coverage_boundary"],
        "wp04_closure_eligible": report["wp04_closure_eligible"],
        "closure_ready": report["closure_ready"],
        "repository": report["repository"],
        "external_archive": archive,
        "external_sources": report["external_sources"],
        "sanitizers_enabled": report["sanitizers_enabled"],
        "sanitizer_policy": report["sanitizer_policy"],
        "full_ubsan_clean": report["full_ubsan_clean"],
        "result": report["result"],
        "assertions": report["assertions"],
        "residual_gaps": report["residual_gaps"],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--commit")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        manifest = load_manifest()
        source_receipt = verify_repository_sources(manifest, args.commit)
        archive, archive_receipt = obtain_external_archive(
            manifest["external_dependency"], args.archive
        )
        with tempfile.TemporaryDirectory(prefix="sigui-signed-advert-") as temporary:
            temporary_root = Path(temporary)
            external_root = temporary_root / "crypto"
            build_root = temporary_root / "build"
            build_root.mkdir()
            external_receipt = extract_external_sources(
                archive, manifest["external_dependency"], external_root
            )
            result, commands = build_and_run(
                args.cc,
                args.cxx,
                external_root,
                build_root,
                manifest["expected_result"],
                sanitize=args.sanitize,
            )
        report = {
            "schema_version": 1,
            "artifact_type": SIGNED_ADVERT_ARTIFACT_TYPE,
            "status": "pass",
            "passed": True,
            "execution_complete": True,
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "work_package": "WP-04",
            "capability": manifest["capability"],
            "coverage_boundary": manifest["coverage_boundary"],
            "wp04_closure_eligible": False,
            "closure_ready": False,
            "repository": source_receipt,
            "external_archive": archive_receipt,
            "external_sources": external_receipt,
            "sanitizers_enabled": args.sanitize,
            "sanitizer_policy": manifest["sanitizer_policy"],
            "full_ubsan_clean": manifest["sanitizer_policy"]["full_ubsan_clean"],
            "commands": commands,
            "result": result,
            "assertions": manifest["assertions"],
            "residual_gaps": manifest["residual_gaps"],
        }
        write_report(args.out, report)
        print(json.dumps({"status": "pass", "out": str(args.out)}))
        return 0
    except GateFailure as exc:
        print(f"signed-advert runtime gate failed: {exc}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
