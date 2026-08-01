#!/usr/bin/env python3
"""Generate and validate the deterministic SPDX 2.3 release SBOM profile."""

from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SPDX_VERSION = "SPDX-2.3"
DATA_LICENSE = "CC0-1.0"
DOCUMENT_ID = "SPDXRef-DOCUMENT"
PROFILE = "SIGUI deterministic release SBOM profile v1"
CREATOR = "Tool: SIGUI-sbom-d1l-1"
PROJECT_NAME = "MeshCore DeskOS D1L"
PROJECT_LICENSE = "GPL-3.0-or-later"
PROJECT_REPOSITORY = "https://github.com/n30nex/SIGUI"
ROOT_PACKAGE_ID = "SPDXRef-Package-MeshCore-DeskOS-D1L"
SOURCE_INPUT_PACKAGE_ID = "SPDXRef-Package-D1L-Source-Inputs"
RELEASE_PACKAGE_ID = "SPDXRef-Package-D1L-Release-Bundle"

SHA1_RE = re.compile(r"[0-9a-f]{40}")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
SPDX_ID_RE = re.compile(r"SPDXRef-[A-Za-z0-9.+-]+")

SUBMODULE_LICENSES = {
    "third_party/MeshCore": "MIT",
    "third_party/sensecap_indicator_esp32": "Apache-2.0",
}
SUBMODULE_URLS = {
    "third_party/MeshCore": "https://github.com/meshcore-dev/MeshCore.git",
    "third_party/sensecap_indicator_esp32": (
        "https://github.com/Seeed-Solution/sensecap_indicator_esp32.git"
    ),
}

REQUIRED_NOTICE_SOURCES = (
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "docs/ATTRIBUTIONS.md",
    "docs/SOURCE_AUDIT_AND_ATTRIBUTION.md",
    "overlays/meshcore_ed25519_defined/license.txt",
)
PRODUCTION_REQUIRED_NOTICE_SOURCES = (
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "overlays/meshcore_ed25519_defined/license.txt",
)

REQUIRED_SOURCE_INPUTS = (
    ".gitmodules",
    ".github/d1l-build-inputs.json",
    ".github/workflows/d1l-ci.yml",
    "CMakeLists.txt",
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "dependencies.lock",
    "docs/ATTRIBUTIONS.md",
    "docs/BUILD_PROVENANCE_D1L.md",
    "docs/archive/pre-rc1-authority-reset/COMPLETION_LEDGER.yaml",
    "docs/SOURCE_AUDIT_AND_ATTRIBUTION.md",
    "main/CMakeLists.txt",
    "overlays/meshcore_ed25519_defined/README.md",
    "overlays/meshcore_ed25519_defined/fe.c",
    "overlays/meshcore_ed25519_defined/ge.c",
    "overlays/meshcore_ed25519_defined/license.txt",
    "overlays/meshcore_ed25519_defined/sc.c",
    "partitions_d1l.csv",
    "patches/sensecap_indicator_idf55_compat.patch",
    "patches/sensecap_indicator_touch_fix.patch",
    "patches/sensecap_indicator_tx_origin.patch",
    "scripts/compare_release_reproducibility_d1l.py",
    "scripts/meshcore_conformance_d1l.py",
    "scripts/meshcore_signed_advert_runtime_d1l.py",
    "scripts/package_release_d1l.py",
    "scripts/provenance_d1l.py",
    "scripts/sbom_d1l.py",
    "scripts/verify_arduino_build_inputs.py",
    "scripts/verify_ci_tool_inputs.py",
    "scripts/validate_ed25519_defined_overlay.py",
    "tests/ed25519_defined_overlay/driver.c",
    "tests/meshcore_oracle/manifest.json",
    "tests/meshcore_signed_advert_runtime/manifest.json",
    "requirements/ci-host-windows.txt",
    "sdkconfig.defaults",
)
PRODUCTION_SOURCE_INPUTS = (
    ".gitmodules",
    "CMakeLists.txt",
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "dependencies.lock",
    "docs/ATTRIBUTIONS.md",
    "main/CMakeLists.txt",
    "overlays/meshcore_ed25519_defined/README.md",
    "overlays/meshcore_ed25519_defined/fe.c",
    "overlays/meshcore_ed25519_defined/ge.c",
    "overlays/meshcore_ed25519_defined/license.txt",
    "overlays/meshcore_ed25519_defined/sc.c",
    "partitions_d1l.csv",
    "patches/sensecap_indicator_idf55_compat.patch",
    "patches/sensecap_indicator_touch_fix.patch",
    "patches/sensecap_indicator_tx_origin.patch",
    "scripts/package_release_d1l.py",
    "scripts/provenance_d1l.py",
    "scripts/sbom_d1l.py",
    "sdkconfig.defaults",
)

PACKAGE_VERIFICATION_EXCLUSIONS = (
    "./README_RELEASE.md",
    "./SHA256SUMS.txt",
    "./manifest.json",
)

ORLP_ED25519_SOURCE_INPUTS = {
    "overlays/meshcore_ed25519_defined/fe.c",
    "overlays/meshcore_ed25519_defined/ge.c",
    "overlays/meshcore_ed25519_defined/license.txt",
    "overlays/meshcore_ed25519_defined/sc.c",
}
ORLP_ED25519_PACKAGE_NOTICE = "notices/ORLP_ED25519_ZLIB_LICENSE.txt"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha1_file(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_sha256(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def exact_sha(value: object, field: str) -> str:
    if not isinstance(value, str) or SHA1_RE.fullmatch(value.lower()) is None:
        raise ValueError(f"{field} must be an exact 40-character hexadecimal SHA")
    return value.lower()


def canonical_timestamp(value: object, field: str = "created") -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{field} is missing")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (ValueError, OverflowError) as exc:
        raise ValueError(f"{field} is not a valid timestamp") from exc
    if parsed.tzinfo is None:
        raise ValueError(f"{field} must include a timezone")
    return parsed.astimezone(timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z"
    )


def git_output(root: Path, *args: str) -> str:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=root,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except (FileNotFoundError, OSError, subprocess.CalledProcessError) as exc:
        raise ValueError(f"git {' '.join(args)} failed for SBOM identity") from exc
    value = result.stdout.strip()
    if not value:
        raise ValueError(f"git {' '.join(args)} returned no SBOM identity")
    return value


def read_gitmodules(root: Path) -> list[dict[str, str]]:
    path = root / ".gitmodules"
    if not path.is_file():
        raise FileNotFoundError("Missing required .gitmodules")
    parser = configparser.ConfigParser(interpolation=None)
    try:
        parser.read_string(path.read_text(encoding="utf-8"))
    except (configparser.Error, OSError, UnicodeError) as exc:
        raise ValueError(".gitmodules is unreadable") from exc
    modules: list[dict[str, str]] = []
    for section in parser.sections():
        module_path = parser.get(section, "path", fallback="").strip().replace("\\", "/")
        url = parser.get(section, "url", fallback="").strip()
        if not module_path or not url:
            raise ValueError(f"{section} is missing path or URL")
        modules.append({"path": module_path, "url": url})
    if not modules:
        raise ValueError(".gitmodules contains no submodules")
    return sorted(modules, key=lambda item: item["path"])


def normalize_source_identity(identity: object) -> dict[str, Any]:
    if not isinstance(identity, dict):
        raise ValueError("SBOM source identity must be an object")
    commit = exact_sha(identity.get("commit"), "source commit")
    created = canonical_timestamp(identity.get("created"), "source commit timestamp")
    repository = identity.get("repository")
    if not isinstance(repository, str) or repository.strip() != PROJECT_REPOSITORY:
        raise ValueError("SBOM source repository is missing or unexpected")
    raw_modules = identity.get("submodules")
    if not isinstance(raw_modules, list):
        raise ValueError("SBOM submodule identity is missing")
    modules = []
    seen: set[str] = set()
    for item in raw_modules:
        if not isinstance(item, dict):
            raise ValueError("SBOM submodule entry must be an object")
        path = str(item.get("path") or "").replace("\\", "/")
        url = item.get("url")
        if path in seen or path not in SUBMODULE_LICENSES:
            raise ValueError(f"Unexpected or duplicate SBOM submodule path: {path}")
        if not isinstance(url, str) or url.strip() != SUBMODULE_URLS[path]:
            raise ValueError(f"SBOM submodule URL is missing or unexpected for {path}")
        license_id = item.get("license")
        if license_id != SUBMODULE_LICENSES[path]:
            raise ValueError(f"SBOM submodule license is missing or wrong for {path}")
        modules.append(
            {
                "path": path,
                "url": url.strip(),
                "commit": exact_sha(item.get("commit"), f"submodule {path} commit"),
                "license": license_id,
            }
        )
        seen.add(path)
    if seen != set(SUBMODULE_LICENSES):
        missing = sorted(set(SUBMODULE_LICENSES) - seen)
        raise ValueError("SBOM source identity is missing submodules: " + ", ".join(missing))
    return {
        "commit": commit,
        "created": created,
        "repository": PROJECT_REPOSITORY,
        "submodules": sorted(modules, key=lambda item: item["path"]),
    }


def discover_source_identity(root: Path, expected_source_sha: str | None = None) -> dict[str, Any]:
    root = root.resolve()
    head = exact_sha(git_output(root, "rev-parse", "HEAD"), "repository HEAD")
    if expected_source_sha is not None and exact_sha(
        expected_source_sha, "expected source commit"
    ) != head:
        raise ValueError("expected source commit does not match repository HEAD")
    epoch_text = git_output(root, "show", "-s", "--format=%ct", head)
    try:
        epoch = int(epoch_text)
        created = datetime.fromtimestamp(epoch, tz=timezone.utc).isoformat().replace(
            "+00:00", "Z"
        )
    except (OverflowError, OSError, ValueError) as exc:
        raise ValueError("repository commit timestamp is invalid") from exc

    modules = []
    for module in read_gitmodules(root):
        path = module["path"]
        if path not in SUBMODULE_LICENSES:
            raise ValueError(f"No reviewed license mapping for submodule {path}")
        row = git_output(root, "ls-tree", head, "--", path)
        match = re.fullmatch(r"160000 commit ([0-9a-f]{40})\t(.+)", row)
        if match is None or match.group(2).replace("\\", "/") != path:
            raise ValueError(f"Missing exact gitlink identity for submodule {path}")
        modules.append(
            {
                "path": path,
                "url": module["url"],
                "commit": match.group(1),
                "license": SUBMODULE_LICENSES[path],
            }
        )
    return normalize_source_identity(
        {
            "commit": head,
            "created": created,
            "repository": PROJECT_REPOSITORY,
            "submodules": modules,
        }
    )


def file_type(path: str) -> str:
    suffix = Path(path).suffix.lower()
    if suffix in {".bin", ".elf", ".uf2"}:
        return "BINARY"
    if suffix in {".md", ".txt"} or Path(path).name in {"LICENSE", ".gitmodules"}:
        return "TEXT"
    if suffix in {".c", ".h", ".py", ".patch"}:
        return "SOURCE"
    return "OTHER"


def file_id(category: str, name: str) -> str:
    token = re.sub(r"[^A-Za-z0-9.+-]+", "-", name).strip("-")
    token = token[-48:] or "file"
    discriminator = hashlib.sha256(f"{category}:{name}".encode("utf-8")).hexdigest()[:12]
    return f"SPDXRef-File-{category}-{token}-{discriminator}"


def file_record(path: Path, name: str, category: str) -> dict[str, Any]:
    if not path.is_file():
        raise FileNotFoundError(f"Missing required SBOM input {path}")
    normalized_name = name.replace("\\", "/")
    is_orlp_ed25519 = normalized_name in ORLP_ED25519_SOURCE_INPUTS or (
        category == "Package" and normalized_name == ORLP_ED25519_PACKAGE_NOTICE
    )
    return {
        "SPDXID": file_id(category, normalized_name),
        "fileName": f"./{category.lower()}/{normalized_name}",
        "fileTypes": [file_type(normalized_name)],
        "checksums": [
            {"algorithm": "SHA1", "checksumValue": sha1_file(path)},
            {"algorithm": "SHA256", "checksumValue": sha256_file(path)},
        ],
        "licenseConcluded": "Zlib" if is_orlp_ed25519 else "NOASSERTION",
        "licenseInfoInFiles": ["Zlib"] if is_orlp_ed25519 else ["NOASSERTION"],
        "copyrightText": (
            "Copyright (c) 2015 Orson Peters <orsonpeters@gmail.com>"
            if is_orlp_ed25519
            else "NOASSERTION"
        ),
        "comment": f"Deterministically hashed {category.lower()} input: {normalized_name}",
    }


def collect_source_files(
    root: Path,
    *,
    production_only: bool = False,
) -> list[dict[str, Any]]:
    root = root.resolve()
    source_inputs = (
        PRODUCTION_SOURCE_INPUTS if production_only else REQUIRED_SOURCE_INPUTS
    )
    return [
        file_record(root / relative, relative, "Source")
        for relative in source_inputs
    ]


def manifest_claims(manifest: dict) -> dict[str, tuple[str, int | None]]:
    claims: dict[str, tuple[str, int | None]] = {}

    def add(value: object) -> None:
        if not isinstance(value, dict) or not isinstance(value.get("path"), str):
            return
        path = value["path"].replace("\\", "/")
        digest = value.get("sha256")
        size = value.get("size")
        if SHA256_RE.fullmatch(str(digest or "").lower()) is None:
            raise ValueError(f"Package manifest has no SHA256 for {path}")
        if size is not None and (isinstance(size, bool) or not isinstance(size, int) or size < 0):
            raise ValueError(f"Package manifest has an invalid size for {path}")
        claim = (str(digest).lower(), size)
        if path in claims and claims[path] != claim:
            raise ValueError(f"Package manifest has conflicting claims for {path}")
        claims[path] = claim

    for name in (
        "flash_files",
        "debug_files",
        "release_docs",
        "notice_files",
    ):
        values = manifest.get(name)
        if isinstance(values, list):
            for value in values:
                add(value)
    for name in (
        "update_image",
        "full_flash_image",
        "meshcore_conformance",
        "meshcore_signed_advert_runtime",
        "build_inputs",
        "capability_manifest",
        "release_evidence_index",
    ):
        add(manifest.get(name))
    groups = manifest.get("rp2040_artifacts")
    if isinstance(groups, list):
        for group in groups:
            if isinstance(group, dict) and isinstance(group.get("files"), list):
                for value in group["files"]:
                    add(value)
    return claims


def validate_manifest_inputs(manifest: dict, package_dir: Path, source_commit: str) -> None:
    if not isinstance(manifest, dict):
        raise ValueError("Package manifest must be an object")
    for container_name in ("git", "workflow"):
        container = manifest.get(container_name)
        claimed = container.get("commit" if container_name == "git" else "sha") if isinstance(container, dict) else None
        if claimed is not None and exact_sha(claimed, f"manifest {container_name} commit") != source_commit:
            raise ValueError(f"Package manifest {container_name} commit does not match SBOM source")
    roles = {
        item.get("role")
        for item in manifest.get("flash_files", [])
        if isinstance(item, dict)
    }
    if not {"bootloader", "partition-table", "app"}.issubset(roles):
        raise ValueError("Package manifest is missing the required ESP32 flash roles")
    notice_sources = {
        item.get("source")
        for item in manifest.get("notice_files", [])
        if isinstance(item, dict)
    }
    required_notice_sources = (
        PRODUCTION_REQUIRED_NOTICE_SOURCES
        if manifest.get("release_profile") == "core_1_0"
        else REQUIRED_NOTICE_SOURCES
    )
    if not set(required_notice_sources).issubset(notice_sources):
        raise ValueError("Package manifest is missing required licenses or notices")

    for relative, (digest, size) in manifest_claims(manifest).items():
        target = (package_dir / relative).resolve()
        try:
            target.relative_to(package_dir.resolve())
        except ValueError as exc:
            raise ValueError(f"Package manifest path escapes the package: {relative}") from exc
        if not target.is_file() or sha256_file(target) != digest:
            raise ValueError(f"Package manifest checksum does not match {relative}")
        if size is not None and target.stat().st_size != size:
            raise ValueError(f"Package manifest size does not match {relative}")


def collect_package_files(
    package_dir: Path, manifest: dict, source_commit: str
) -> list[dict[str, Any]]:
    package_dir = package_dir.resolve()
    validate_manifest_inputs(manifest, package_dir, source_commit)
    excluded_names = {name[2:] for name in PACKAGE_VERIFICATION_EXCLUSIONS}
    files = []
    for path in sorted(
        package_dir.rglob("*"),
        key=lambda item: item.relative_to(package_dir).as_posix(),
    ):
        if not path.is_file():
            continue
        relative = path.relative_to(package_dir).as_posix()
        if relative in excluded_names or relative in {
            f"sbom_{source_commit}.spdx.json",
            f"provenance_{source_commit}.json",
        }:
            continue
        files.append(file_record(path, relative, "Package"))
    if not files:
        raise ValueError("Release package has no SBOM-addressable files")
    return files


def checksum_value(record: dict, algorithm: str) -> str:
    rows = [
        row.get("checksumValue")
        for row in record.get("checksums", [])
        if isinstance(row, dict) and row.get("algorithm") == algorithm
    ]
    if len(rows) != 1 or not isinstance(rows[0], str):
        raise ValueError(f"File {record.get('fileName')} has no unique {algorithm} checksum")
    return rows[0]


def package_verification_code(files: list[dict]) -> str:
    joined = "".join(sorted(checksum_value(item, "SHA1") for item in files))
    return hashlib.sha1(joined.encode("ascii")).hexdigest()


def analyzed_package(
    spdx_id: str,
    name: str,
    version: str,
    purpose: str,
    files: list[dict],
    *,
    excluded_files: list[str] | None = None,
) -> dict[str, Any]:
    verification: dict[str, Any] = {
        "packageVerificationCodeValue": package_verification_code(files)
    }
    if excluded_files:
        verification["packageVerificationCodeExcludedFiles"] = sorted(excluded_files)
    return {
        "SPDXID": spdx_id,
        "name": name,
        "versionInfo": version,
        "downloadLocation": "NOASSERTION",
        "filesAnalyzed": True,
        "hasFiles": sorted(item["SPDXID"] for item in files),
        "packageVerificationCode": verification,
        "primaryPackagePurpose": purpose,
        "licenseConcluded": "NOASSERTION",
        "licenseDeclared": PROJECT_LICENSE,
        "copyrightText": "NOASSERTION",
    }


def build_spdx_document(
    root: Path,
    source_identity: dict,
    *,
    package_dir: Path | None = None,
    package_manifest: dict | None = None,
) -> dict[str, Any]:
    identity = normalize_source_identity(source_identity)
    source_files = collect_source_files(
        root,
        production_only=(
            isinstance(package_manifest, dict)
            and package_manifest.get("release_profile") == "core_1_0"
        ),
    )
    package_files: list[dict[str, Any]] = []
    if (package_dir is None) != (package_manifest is None):
        raise ValueError("package_dir and package_manifest must be supplied together")
    if package_dir is not None and package_manifest is not None:
        package_files = collect_package_files(
            package_dir, package_manifest, identity["commit"]
        )

    fingerprint_payload = {
        "profile": PROFILE,
        "identity": identity,
        "source_files": [
            {
                "name": item["fileName"],
                "sha256": checksum_value(item, "SHA256"),
            }
            for item in source_files
        ],
        "package": {
            "name": package_manifest.get("package"),
            "app_version": package_manifest.get("app_version"),
            "files": [
                {
                    "name": item["fileName"],
                    "sha256": checksum_value(item, "SHA256"),
                }
                for item in package_files
            ],
        }
        if package_manifest is not None
        else None,
    }
    fingerprint = canonical_sha256(fingerprint_payload)

    packages: list[dict[str, Any]] = [
        {
            "SPDXID": ROOT_PACKAGE_ID,
            "name": PROJECT_NAME,
            "versionInfo": identity["commit"],
            "downloadLocation": identity["repository"],
            "filesAnalyzed": False,
            "primaryPackagePurpose": "FIRMWARE",
            "licenseConcluded": PROJECT_LICENSE,
            "licenseDeclared": PROJECT_LICENSE,
            "copyrightText": "NOASSERTION",
            "homepage": identity["repository"],
            "sourceInfo": "Exact source identity is the Git commit and gitlink set in this SPDX document.",
        },
        analyzed_package(
            SOURCE_INPUT_PACKAGE_ID,
            f"{PROJECT_NAME} release source inputs",
            identity["commit"],
            "SOURCE",
            source_files,
        ),
    ]
    for module in identity["submodules"]:
        packages.append(
            {
                "SPDXID": "SPDXRef-Package-Submodule-"
                + re.sub(r"[^A-Za-z0-9.+-]+", "-", module["path"]),
                "name": module["path"],
                "versionInfo": module["commit"],
                "downloadLocation": module["url"],
                "filesAnalyzed": False,
                "primaryPackagePurpose": "LIBRARY",
                "licenseConcluded": module["license"],
                "licenseDeclared": module["license"],
                "copyrightText": "NOASSERTION",
            }
        )

    described = [ROOT_PACKAGE_ID]
    if package_manifest is not None:
        package_name = package_manifest.get("package")
        if not isinstance(package_name, str) or not package_name.strip():
            raise ValueError("Package manifest name is missing")
        release = analyzed_package(
            RELEASE_PACKAGE_ID,
            package_name,
            str(package_manifest.get("app_version") or identity["commit"]),
            "FIRMWARE",
            package_files,
            excluded_files=list(PACKAGE_VERIFICATION_EXCLUSIONS)
            + [
                f"./sbom_{identity['commit']}.spdx.json",
                f"./provenance_{identity['commit']}.json",
            ],
        )
        if package_manifest.get("release_profile") == "core_1_0":
            release["attributionTexts"] = [
                "See notices/LICENSE, notices/THIRD_PARTY_NOTICES.md, "
                "notices/ORLP_ED25519_ZLIB_LICENSE.txt, and "
                "docs/ATTRIBUTIONS.md."
            ]
        else:
            release["attributionTexts"] = [
                "See notices/LICENSE, notices/THIRD_PARTY_NOTICES.md, "
                "notices/ATTRIBUTIONS.md, "
                "notices/SOURCE_AUDIT_AND_ATTRIBUTION.md, and "
                "notices/ORLP_ED25519_ZLIB_LICENSE.txt."
            ]
        packages.append(release)
        described.append(RELEASE_PACKAGE_ID)

    all_files = sorted(source_files + package_files, key=lambda item: item["fileName"])
    relationships = [
        {
            "spdxElementId": DOCUMENT_ID,
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": item,
        }
        for item in described
    ]
    relationships.extend(
        {
            "spdxElementId": ROOT_PACKAGE_ID,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": package["SPDXID"],
        }
        for package in packages
        if package["SPDXID"].startswith("SPDXRef-Package-Submodule-")
    )
    relationships.extend(
        {
            "spdxElementId": SOURCE_INPUT_PACKAGE_ID,
            "relationshipType": "CONTAINS",
            "relatedSpdxElement": item["SPDXID"],
        }
        for item in source_files
    )
    if package_manifest is not None:
        relationships.extend(
            [
                {
                    "spdxElementId": RELEASE_PACKAGE_ID,
                    "relationshipType": "GENERATED_FROM",
                    "relatedSpdxElement": ROOT_PACKAGE_ID,
                },
                {
                    "spdxElementId": SOURCE_INPUT_PACKAGE_ID,
                    "relationshipType": "BUILD_DEPENDENCY_OF",
                    "relatedSpdxElement": RELEASE_PACKAGE_ID,
                },
            ]
        )
        relationships.extend(
            {
                "spdxElementId": RELEASE_PACKAGE_ID,
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": item["SPDXID"],
            }
            for item in package_files
        )
    relationships.sort(
        key=lambda item: (
            item["spdxElementId"],
            item["relationshipType"],
            item["relatedSpdxElement"],
        )
    )

    document = {
        "SPDXID": DOCUMENT_ID,
        "spdxVersion": SPDX_VERSION,
        "dataLicense": DATA_LICENSE,
        "name": f"n30nex-SIGUI-{identity['commit']}",
        "documentNamespace": (
            f"https://spdx.org/spdxdocs/n30nex-SIGUI/"
            f"{identity['commit']}/{fingerprint}"
        ),
        "documentDescribes": sorted(described),
        "creationInfo": {
            "created": identity["created"],
            "creators": [CREATOR],
            "comment": "Creation time is the immutable source commit timestamp.",
        },
        "comment": f"{PROFILE}; context-sha256:{fingerprint}",
        "packages": sorted(packages, key=lambda item: item["SPDXID"]),
        "files": all_files,
        "relationships": relationships,
    }
    errors = validate_spdx_profile(document, identity["commit"])
    if errors:
        raise ValueError("Generated SPDX profile is invalid: " + "; ".join(errors))
    return document


def validate_spdx_profile(document: object, expected_source_sha: str) -> list[str]:
    errors: list[str] = []
    if not isinstance(document, dict):
        return ["SPDX document must be an object"]
    source_sha = exact_sha(expected_source_sha, "expected source commit")
    required = {
        "SPDXID": DOCUMENT_ID,
        "spdxVersion": SPDX_VERSION,
        "dataLicense": DATA_LICENSE,
        "name": f"n30nex-SIGUI-{source_sha}",
    }
    for name, expected in required.items():
        if document.get(name) != expected:
            errors.append(f"{name} is missing or invalid")
    namespace = document.get("documentNamespace")
    if not isinstance(namespace, str) or source_sha not in namespace:
        errors.append("documentNamespace is not bound to the source commit")
    creation = document.get("creationInfo")
    if not isinstance(creation, dict) or creation.get("creators") != [CREATOR]:
        errors.append("creationInfo creator is missing or invalid")
    else:
        try:
            if canonical_timestamp(creation.get("created")) != creation.get("created"):
                errors.append("creationInfo.created is not canonical UTC")
        except ValueError:
            errors.append("creationInfo.created is invalid")

    packages = document.get("packages")
    files = document.get("files")
    relationships = document.get("relationships")
    if not isinstance(packages, list) or not packages:
        errors.append("packages are missing")
        packages = []
    if not isinstance(files, list) or not files:
        errors.append("files are missing")
        files = []
    if not isinstance(relationships, list) or not relationships:
        errors.append("relationships are missing")
        relationships = []

    ids: set[str] = {DOCUMENT_ID}
    file_ids: set[str] = set()
    for item in files:
        if not isinstance(item, dict):
            errors.append("file entry is not an object")
            continue
        spdx_id = item.get("SPDXID")
        if not isinstance(spdx_id, str) or SPDX_ID_RE.fullmatch(spdx_id) is None or spdx_id in ids:
            errors.append("file SPDXID is missing, malformed, or duplicated")
            continue
        ids.add(spdx_id)
        file_ids.add(spdx_id)
        checksums = {
            row.get("algorithm"): row.get("checksumValue")
            for row in item.get("checksums", [])
            if isinstance(row, dict)
        }
        if set(checksums) != {"SHA1", "SHA256"}:
            errors.append(f"{spdx_id} does not have exact SHA1 and SHA256 checksums")
        elif SHA1_RE.fullmatch(str(checksums["SHA1"])) is None or SHA256_RE.fullmatch(
            str(checksums["SHA256"])
        ) is None:
            errors.append(f"{spdx_id} has malformed checksums")
        if not isinstance(item.get("fileName"), str) or not item["fileName"].startswith("./"):
            errors.append(f"{spdx_id} has an invalid fileName")

    package_ids: set[str] = set()
    for item in packages:
        if not isinstance(item, dict):
            errors.append("package entry is not an object")
            continue
        spdx_id = item.get("SPDXID")
        if not isinstance(spdx_id, str) or SPDX_ID_RE.fullmatch(spdx_id) is None or spdx_id in ids:
            errors.append("package SPDXID is missing, malformed, or duplicated")
            continue
        ids.add(spdx_id)
        package_ids.add(spdx_id)
        if not isinstance(item.get("name"), str) or not isinstance(
            item.get("downloadLocation"), str
        ):
            errors.append(f"{spdx_id} is missing required package identity")
        analyzed = item.get("filesAnalyzed")
        has_files = item.get("hasFiles")
        verification = item.get("packageVerificationCode")
        if analyzed is True:
            if not isinstance(has_files, list) or not has_files:
                errors.append(f"{spdx_id} analyzes files but hasFiles is missing")
            if not isinstance(verification, dict) or SHA1_RE.fullmatch(
                str(verification.get("packageVerificationCodeValue") or "")
            ) is None:
                errors.append(f"{spdx_id} has no valid package verification code")
        elif analyzed is False and (has_files is not None or verification is not None):
            errors.append(f"{spdx_id} claims unanalyzed files but contains file metadata")
        elif analyzed not in (True, False):
            errors.append(f"{spdx_id} has no explicit filesAnalyzed value")

    for item in packages:
        if not isinstance(item, dict) or item.get("filesAnalyzed") is not True:
            continue
        for spdx_id in item.get("hasFiles", []):
            if spdx_id not in file_ids:
                errors.append(f"{item.get('SPDXID')} references an unknown file {spdx_id}")
    for item in relationships:
        if not isinstance(item, dict):
            errors.append("relationship entry is not an object")
            continue
        if item.get("spdxElementId") not in ids or item.get("relatedSpdxElement") not in ids:
            errors.append("relationship references an unknown SPDX element")
        if not isinstance(item.get("relationshipType"), str):
            errors.append("relationship type is missing")
    described = document.get("documentDescribes")
    if not isinstance(described, list) or not described or any(
        value not in package_ids and value not in file_ids for value in described
    ):
        errors.append("documentDescribes is missing or references unknown elements")
    return errors


def validate_against_inputs(
    document: object,
    root: Path,
    source_identity: dict,
    *,
    package_dir: Path | None = None,
    package_manifest: dict | None = None,
) -> list[str]:
    identity = normalize_source_identity(source_identity)
    errors = validate_spdx_profile(document, identity["commit"])
    try:
        expected = build_spdx_document(
            root,
            identity,
            package_dir=package_dir,
            package_manifest=package_manifest,
        )
    except (FileNotFoundError, OSError, ValueError) as exc:
        errors.append(f"cannot rebuild deterministic SPDX document: {exc}")
        return errors
    if document != expected:
        errors.append("SPDX document does not match the deterministic source and package inputs")
    return errors


def serialize_spdx(document: dict) -> str:
    return json.dumps(document, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def write_package_sbom(
    root: Path,
    package_dir: Path,
    package_manifest: dict,
    *,
    source_identity: dict | None = None,
    expected_source_sha: str | None = None,
) -> dict[str, Any]:
    identity = normalize_source_identity(source_identity) if source_identity is not None else discover_source_identity(
        root, expected_source_sha
    )
    if expected_source_sha is not None and identity["commit"] != exact_sha(
        expected_source_sha, "expected source commit"
    ):
        raise ValueError("supplied SBOM source identity does not match expected source commit")
    document = build_spdx_document(
        root,
        identity,
        package_dir=package_dir,
        package_manifest=package_manifest,
    )
    path = package_dir / f"sbom_{identity['commit']}.spdx.json"
    path.write_text(serialize_spdx(document), encoding="ascii")
    loaded = json.loads(path.read_text(encoding="ascii"))
    errors = validate_against_inputs(
        loaded,
        root,
        identity,
        package_dir=package_dir,
        package_manifest=package_manifest,
    )
    if errors:
        raise ValueError("Written SPDX SBOM failed validation: " + "; ".join(errors))
    return {
        "path": path.relative_to(package_dir).as_posix(),
        "sha256": sha256_file(path),
        "spdx_version": SPDX_VERSION,
        "profile": PROFILE,
        "source_commit": identity["commit"],
        "document_namespace": document["documentNamespace"],
        "package_count": len(document["packages"]),
        "file_count": len(document["files"]),
        "valid": True,
    }


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"Cannot read JSON object from {path}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"JSON in {path} is not an object")
    return value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--package-dir")
    parser.add_argument("--package-manifest")
    parser.add_argument("--out")
    parser.add_argument("--validate")
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    identity = discover_source_identity(root, args.source_sha)
    package_dir = Path(args.package_dir).resolve() if args.package_dir else None
    manifest = load_json(Path(args.package_manifest)) if args.package_manifest else None
    if (package_dir is None) != (manifest is None):
        raise SystemExit("--package-dir and --package-manifest must be supplied together")

    if args.validate:
        path = Path(args.validate).resolve()
        document = load_json(path)
        errors = validate_against_inputs(
            document,
            root,
            identity,
            package_dir=package_dir,
            package_manifest=manifest,
        )
        print(json.dumps({"ok": not errors, "path": str(path), "errors": errors}))
        return 1 if errors else 0

    document = build_spdx_document(
        root,
        identity,
        package_dir=package_dir,
        package_manifest=manifest,
    )
    out = (
        Path(args.out).resolve()
        if args.out
        else root / "artifacts" / "release" / f"sbom_{identity['commit']}.spdx.json"
    )
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(serialize_spdx(document), encoding="ascii")
    print(json.dumps({"ok": True, "path": str(out), "sha256": sha256_file(out)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
