#!/usr/bin/env python3
"""Capture and safely materialize one exact Core GitHub Actions run."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath

try:
    from artifact_metadata import git_metadata
    from verify_checksums import is_link_or_reparse, sha256_file
except ImportError:  # pragma: no cover
    from scripts.artifact_metadata import git_metadata
    from scripts.verify_checksums import is_link_or_reparse, sha256_file


REPOSITORY = "n30nex/SIGUI"
WORKFLOW_NAME = "d1l-ci"
WORKFLOW_PATH = ".github/workflows/d1l-ci.yml"
RELEASE_BRANCH = "release/24h-core"
EXPECTED_ACTIONS_ARTIFACTS = (
    "d1l-host-artifacts",
    "d1l-meshcore-wire-conformance",
    "d1l-idf55-migration-state",
    "d1l-firmware-artifacts",
    "d1l-release-package",
)
MAX_ZIP_ENTRIES = 100_000
MAX_UNCOMPRESSED_BYTES = 8 * 1024 * 1024 * 1024


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _api(root: Path, endpoint: str) -> bytes:
    completed = subprocess.run(
        [
            "gh",
            "api",
            "-H",
            "Accept: application/vnd.github+json",
            "-H",
            "X-GitHub-Api-Version: 2022-11-28",
            endpoint,
        ],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"GitHub API capture failed for {endpoint}: "
            + completed.stderr.decode("utf-8", errors="replace")[-1000:]
        )
    return completed.stdout


def _json_object(raw: bytes, label: str) -> dict:
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"{label} returned invalid JSON") from exc
    if not isinstance(value, dict):
        raise RuntimeError(f"{label} returned a non-object")
    return value


def _exact_sha(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    value = value.strip().lower()
    return value if re.fullmatch(r"[0-9a-f]{40}", value) else None


def _inside(path: Path, root: Path, label: str) -> Path:
    resolved = path.resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as exc:
        raise ValueError(f"{label} must stay inside the repository") from exc
    return resolved


def file_row(path: Path, root: Path) -> dict:
    path = _inside(path, root, "Evidence path")
    if not path.is_file() or is_link_or_reparse(path):
        raise ValueError(f"Evidence file is missing or linked: {path}")
    return {
        "path": path.relative_to(root.resolve()).as_posix(),
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def tree_inventory(directory: Path, root: Path) -> dict:
    directory = _inside(directory, root, "Artifact directory")
    if not directory.is_dir() or is_link_or_reparse(directory):
        raise ValueError(f"Artifact directory is missing or linked: {directory}")
    rows: list[dict] = []
    for path in sorted(
        directory.rglob("*"),
        key=lambda candidate: candidate.relative_to(directory).as_posix(),
    ):
        if is_link_or_reparse(path):
            raise ValueError(f"Artifact tree contains a link/reparse point: {path}")
        if not path.is_file():
            continue
        rows.append(
            {
                "path": path.relative_to(directory).as_posix(),
                "size": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    if not rows:
        raise ValueError(f"Artifact tree is empty: {directory}")
    canonical = json.dumps(
        rows, sort_keys=True, separators=(",", ":")
    ).encode("ascii")
    return {
        "file_count": len(rows),
        "aggregate_sha256": hashlib.sha256(canonical).hexdigest(),
        "files": rows,
    }


def _zip_member_path(info: zipfile.ZipInfo) -> PurePosixPath:
    name = info.filename
    if (
        not name
        or "\x00" in name
        or "\\" in name
        or name.startswith("/")
        or re.match(r"^[A-Za-z]:", name)
    ):
        raise ValueError(f"Unsafe ZIP member path: {name!r}")
    member = PurePosixPath(name)
    if member.is_absolute() or any(part in {"", ".", ".."} for part in member.parts):
        raise ValueError(f"Unsafe ZIP member path: {name!r}")
    mode = info.external_attr >> 16
    file_type = stat.S_IFMT(mode)
    if stat.S_ISLNK(mode) or (
        not info.is_dir() and file_type not in {0, stat.S_IFREG}
    ):
        raise ValueError(f"ZIP member is not a regular file: {name!r}")
    if info.flag_bits & 0x1:
        raise ValueError(f"Encrypted ZIP member is unsupported: {name!r}")
    return member


def zip_inventory(archive: Path) -> list[dict]:
    """Return a safe member inventory without extracting the archive."""
    rows: list[dict] = []
    seen: set[str] = set()
    seen_casefold: set[str] = set()
    with zipfile.ZipFile(archive) as handle:
        infos = handle.infolist()
        if len(infos) > MAX_ZIP_ENTRIES:
            raise ValueError("Actions artifact ZIP contains too many entries")
        total_size = sum(info.file_size for info in infos)
        if total_size > MAX_UNCOMPRESSED_BYTES:
            raise ValueError("Actions artifact ZIP expands beyond the safety limit")
        for info in infos:
            member = _zip_member_path(info)
            name = member.as_posix()
            folded = name.casefold()
            if name in seen or folded in seen_casefold:
                raise ValueError(f"Duplicate ZIP member path: {name}")
            seen.add(name)
            seen_casefold.add(folded)
            if info.is_dir():
                continue
            digest = hashlib.sha256()
            with handle.open(info, "r") as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(chunk)
            rows.append(
                {
                    "path": name,
                    "size": info.file_size,
                    "sha256": digest.hexdigest(),
                }
            )
    if not rows:
        raise ValueError("Actions artifact ZIP contains no files")
    return sorted(rows, key=lambda row: row["path"])


def safe_extract(archive: Path, destination: Path) -> None:
    """Extract an Actions artifact after rejecting traversal, links, and duplicates."""
    if destination.exists():
        raise ValueError(f"Artifact destination already exists: {destination}")
    zip_inventory(archive)
    destination.mkdir(parents=True)
    with zipfile.ZipFile(archive) as handle:
        for info in handle.infolist():
            member = _zip_member_path(info)
            target = destination.joinpath(*member.parts)
            try:
                target.resolve().relative_to(destination.resolve())
            except ValueError as exc:  # defensive check beyond PurePosixPath validation
                raise ValueError(f"ZIP member escapes destination: {info.filename}") from exc
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with handle.open(info, "r") as source, target.open("xb") as sink:
                while True:
                    chunk = source.read(1024 * 1024)
                    if not chunk:
                        break
                    sink.write(chunk)


def validate_run(raw: dict, *, run_id: str, commit: str) -> None:
    repository = raw.get("repository")
    repository = repository if isinstance(repository, dict) else {}
    run_attempt = raw.get("run_attempt")
    if not (
        str(raw.get("id")) == run_id
        and raw.get("status") == "completed"
        and raw.get("conclusion") == "success"
        and _exact_sha(raw.get("head_sha")) == commit
        and raw.get("head_branch") == RELEASE_BRANCH
        and raw.get("event") == "workflow_dispatch"
        and raw.get("path") == WORKFLOW_PATH
        and raw.get("name") == WORKFLOW_NAME
        and isinstance(run_attempt, int)
        and not isinstance(run_attempt, bool)
        and run_attempt >= 1
        and repository.get("full_name") == REPOSITORY
    ):
        raise ValueError("GitHub Actions run is not the exact successful Core candidate")


def validate_artifacts(
    raw: dict, *, run_id: str, commit: str
) -> dict[str, dict]:
    artifacts = raw.get("artifacts")
    total = raw.get("total_count")
    if (
        not isinstance(artifacts, list)
        or total != len(EXPECTED_ACTIONS_ARTIFACTS)
        or len(artifacts) != len(EXPECTED_ACTIONS_ARTIFACTS)
    ):
        raise ValueError("Actions run does not expose exactly five Core artifacts")
    by_name: dict[str, dict] = {}
    ids: set[int] = set()
    for row in artifacts:
        workflow_run = row.get("workflow_run") if isinstance(row, dict) else None
        workflow_run = workflow_run if isinstance(workflow_run, dict) else {}
        artifact_id = row.get("id") if isinstance(row, dict) else None
        name = row.get("name") if isinstance(row, dict) else None
        size = row.get("size_in_bytes") if isinstance(row, dict) else None
        digest = row.get("digest") if isinstance(row, dict) else None
        if not (
            isinstance(artifact_id, int)
            and not isinstance(artifact_id, bool)
            and artifact_id > 0
            and artifact_id not in ids
            and isinstance(name, str)
            and name in EXPECTED_ACTIONS_ARTIFACTS
            and name not in by_name
            and row.get("expired") is False
            and isinstance(size, int)
            and not isinstance(size, bool)
            and size > 0
            and isinstance(digest, str)
            and re.fullmatch(r"sha256:[0-9a-f]{64}", digest)
            and str(workflow_run.get("id")) == run_id
            and _exact_sha(workflow_run.get("head_sha")) == commit
            and workflow_run.get("head_branch") == RELEASE_BRANCH
        ):
            raise ValueError("Actions artifact metadata is incomplete or mismatched")
        ids.add(artifact_id)
        by_name[name] = row
    if set(by_name) != set(EXPECTED_ACTIONS_ARTIFACTS):
        raise ValueError("Actions artifact names do not match the disabled-Core set")
    return by_name


def _read_json_file(path: Path, label: str) -> dict:
    if not path.is_file() or is_link_or_reparse(path):
        raise ValueError(f"{label} is missing or linked: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"{label} is invalid JSON: {path}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be a JSON object")
    return value


def _verified_row(row: object, root: Path, label: str) -> Path:
    if not isinstance(row, dict):
        raise ValueError(f"{label} inventory row is missing")
    raw_path = row.get("path")
    if not isinstance(raw_path, str) or not raw_path:
        raise ValueError(f"{label} inventory path is missing")
    path = Path(raw_path)
    if path.is_absolute():
        raise ValueError(f"{label} inventory path must be repository-relative")
    path = _inside(root / path, root, label)
    if (
        not path.is_file()
        or is_link_or_reparse(path)
        or isinstance(row.get("size"), bool)
        or row.get("size") != path.stat().st_size
        or not isinstance(row.get("sha256"), str)
        or row.get("sha256") != sha256_file(path)
    ):
        raise ValueError(f"{label} inventory hash/size mismatch")
    return path


def validate_capture_receipt(
    *,
    receipt_path: Path,
    root: Path,
    github_run_dir: Path,
    commit: str,
    run_id: str,
    run_attempt: str,
    max_age_sec: int = 24 * 60 * 60,
) -> dict:
    """Recompute the API/archive/extraction binding for one Core run."""
    root = root.resolve()
    commit = str(commit).strip().lower()
    run_id = str(run_id)
    run_attempt = str(run_attempt)
    if (
        _exact_sha(commit) is None
        or re.fullmatch(r"[1-9][0-9]*", run_id) is None
        or re.fullmatch(r"[1-9][0-9]*", run_attempt) is None
    ):
        raise ValueError("Candidate commit/run/run-attempt inputs are invalid")
    receipt_path = _inside(receipt_path, root, "Actions capture receipt")
    github_run_dir = _inside(
        github_run_dir, root, "GitHub Actions run directory"
    )
    receipt = _read_json_file(receipt_path, "Actions capture receipt")
    source = receipt.get("git")
    if not (
        receipt.get("schema") == 2
        and receipt.get("kind") == "core_actions_run_metadata"
        and receipt.get("mode") == "github-api-artifact-capture"
        and receipt.get("ok") is True
        and receipt.get("repository") == REPOSITORY
        and _exact_sha(receipt.get("expected_commit")) == commit
        and str(receipt.get("github_actions_run")) == run_id
        and str(receipt.get("workflow_run_attempt")) == run_attempt
        and isinstance(source, dict)
        and _exact_sha(source.get("commit")) == commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    ):
        raise ValueError("Actions capture receipt identity mismatch")
    try:
        captured_at = datetime.fromisoformat(
            str(receipt.get("captured_at")).replace("Z", "+00:00")
        ).astimezone(timezone.utc)
        age_sec = (datetime.now(timezone.utc) - captured_at).total_seconds()
    except (TypeError, ValueError) as exc:
        raise ValueError("Actions capture timestamp is invalid") from exc
    if not -300 <= age_sec <= max_age_sec:
        raise ValueError("Actions capture receipt is stale or future-dated")

    run_raw_path = _verified_row(
        receipt.get("raw_run_response"), root, "raw run response"
    )
    artifacts_raw_path = _verified_row(
        receipt.get("raw_artifacts_response"),
        root,
        "raw artifacts response",
    )
    run_raw = _read_json_file(run_raw_path, "raw run response")
    artifacts_raw = _read_json_file(
        artifacts_raw_path, "raw artifacts response"
    )
    validate_run(run_raw, run_id=run_id, commit=commit)
    if str(run_raw.get("run_attempt")) != run_attempt:
        raise ValueError("Actions raw run attempt mismatch")
    artifacts_by_name = validate_artifacts(
        artifacts_raw, run_id=run_id, commit=commit
    )
    receipt_rows = receipt.get("artifacts")
    if not isinstance(receipt_rows, list):
        raise ValueError("Actions capture artifact rows are missing")
    rows_by_name: dict[str, dict] = {}
    for row in receipt_rows:
        name = row.get("name") if isinstance(row, dict) else None
        if (
            not isinstance(name, str)
            or name in rows_by_name
            or name not in EXPECTED_ACTIONS_ARTIFACTS
        ):
            raise ValueError("Actions capture has invalid/duplicate artifact rows")
        rows_by_name[name] = row
    if set(rows_by_name) != set(EXPECTED_ACTIONS_ARTIFACTS):
        raise ValueError("Actions capture artifact row set is incomplete")

    verified_artifacts: list[dict] = []
    for name in EXPECTED_ACTIONS_ARTIFACTS:
        api_row = artifacts_by_name[name]
        row = rows_by_name[name]
        archive = _verified_row(
            row.get("archive"), root, f"{name} archive"
        )
        expected_digest = api_row["digest"].removeprefix("sha256:")
        if not (
            row.get("artifact_id") == api_row.get("id")
            and row.get("api_digest") == api_row.get("digest")
            and row.get("api_size_in_bytes") == api_row.get("size_in_bytes")
            and archive.stat().st_size == api_row.get("size_in_bytes")
            and sha256_file(archive) == expected_digest
        ):
            raise ValueError(f"{name} archive is not bound to its API row")
        zip_rows = zip_inventory(archive)
        extracted_text = row.get("extracted_root")
        if (
            not isinstance(extracted_text, str)
            or not extracted_text
            or Path(extracted_text).is_absolute()
        ):
            raise ValueError(f"{name} extracted root is invalid")
        extracted = _inside(
            root / extracted_text, root, f"{name} extracted root"
        )
        if extracted != (github_run_dir / name).resolve():
            raise ValueError(f"{name} extracted root is not canonical")
        extracted_inventory = tree_inventory(extracted, root)
        if not (
            row.get("zip_members") == zip_rows
            and row.get("extracted_inventory") == extracted_inventory
            and zip_rows == extracted_inventory.get("files")
        ):
            raise ValueError(f"{name} extracted tree does not match its archive")
        verified_artifacts.append(
            {
                "name": name,
                "artifact_id": api_row["id"],
                "archive_sha256": expected_digest,
                "archive_size": api_row["size_in_bytes"],
                "extracted_aggregate_sha256": extracted_inventory[
                    "aggregate_sha256"
                ],
            }
        )

    scope_path = (
        github_run_dir
        / "d1l-host-artifacts"
        / "build-inputs"
        / "d1l-candidate-scope.json"
    )
    scope = _read_json_file(scope_path, "Core candidate scope")
    if scope != {
        "schema": 1,
        "kind": "d1l_candidate_scope",
        "source_commit": commit,
        "workflow_run_id": run_id,
        "workflow_run_attempt": run_attempt,
        "repository": REPOSITORY,
        "workflow": WORKFLOW_NAME,
        "event": "workflow_dispatch",
        "include_sd_bridge": False,
        "scope_reason": "esp32_only",
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
    }:
        raise ValueError("Checksummed Core candidate scope is not exact")
    return {
        "ok": True,
        "receipt": file_row(receipt_path, root),
        "captured_age_sec": age_sec,
        "run_attempt": run_attempt,
        "artifacts": verified_artifacts,
        "scope_sha256": sha256_file(scope_path),
    }


def capture(
    *,
    root: Path,
    run_id: str,
    commit: str,
    out_dir: Path,
    github_run_dir: Path | None = None,
) -> Path:
    root = root.resolve()
    commit = commit.strip().lower()
    if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise ValueError("--commit must be an exact 40-character hexadecimal SHA")
    if re.fullmatch(r"[1-9][0-9]*", str(run_id)) is None:
        raise ValueError("--github-run-id must be numeric")
    run_id = str(run_id)
    source = git_metadata(root)
    if not (
        source.get("commit") == commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    ):
        raise ValueError(
            "Actions capture must run from the exact clean candidate"
        )

    run_raw = _api(root, f"repos/{REPOSITORY}/actions/runs/{run_id}")
    run_payload = _json_object(run_raw, "GitHub Actions run API")
    validate_run(run_payload, run_id=run_id, commit=commit)
    artifacts_raw = _api(
        root,
        f"repos/{REPOSITORY}/actions/runs/{run_id}/artifacts?per_page=100",
    )
    artifacts_payload = _json_object(
        artifacts_raw, "GitHub Actions artifacts API"
    )
    artifacts = validate_artifacts(
        artifacts_payload, run_id=run_id, commit=commit
    )

    out_dir = _inside(out_dir, root, "--out-dir")
    github_run_dir = _inside(
        github_run_dir or out_dir.parent, root, "--github-run-dir"
    )
    expected_out_dir = github_run_dir / "core-actions-run-metadata"
    if out_dir != expected_out_dir:
        raise ValueError(
            "--out-dir must be the canonical core-actions-run-metadata "
            "directory inside --github-run-dir"
        )
    if github_run_dir.exists():
        raise ValueError(
            "Actions evidence path already exists; refusing to overwrite: "
            + str(github_run_dir)
        )
    github_run_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(
            prefix=f".core-actions-{run_id}-",
            dir=github_run_dir.parent,
        )
    )

    def staged_row(staged_path: Path, final_path: Path) -> dict:
        return {
            "path": final_path.relative_to(root).as_posix(),
            "size": staged_path.stat().st_size,
            "sha256": sha256_file(staged_path),
        }

    try:
        staged_out = staging / "core-actions-run-metadata"
        staged_archives = staging / "_archives"
        staged_out.mkdir()
        staged_archives.mkdir()
        staged_run_raw = staged_out / f"actions_run_{run_id}_raw.json"
        staged_artifacts_raw = (
            staged_out / f"actions_run_{run_id}_artifacts_raw.json"
        )
        staged_receipt = staged_out / f"core_actions_run_{run_id}.json"
        final_run_raw = out_dir / staged_run_raw.name
        final_artifacts_raw = out_dir / staged_artifacts_raw.name
        final_receipt = out_dir / staged_receipt.name
        staged_run_raw.write_bytes(run_raw)
        staged_artifacts_raw.write_bytes(artifacts_raw)

        receipt_artifacts: list[dict] = []
        for name in EXPECTED_ACTIONS_ARTIFACTS:
            api_row = artifacts[name]
            artifact_id = api_row["id"]
            staged_archive = (
                staged_archives / f"{name}-{artifact_id}.zip"
            )
            final_archive = (
                github_run_dir
                / "_archives"
                / staged_archive.name
            )
            archive_bytes = _api(
                root,
                f"repos/{REPOSITORY}/actions/artifacts/{artifact_id}/zip",
            )
            expected_sha = api_row["digest"].removeprefix("sha256:")
            if (
                len(archive_bytes) != api_row["size_in_bytes"]
                or hashlib.sha256(archive_bytes).hexdigest()
                != expected_sha
            ):
                raise ValueError(
                    "Downloaded archive does not match GitHub digest/size: "
                    + name
                )
            staged_archive.write_bytes(archive_bytes)
            zip_members = zip_inventory(staged_archive)
            staged_extracted = staging / name
            final_extracted = github_run_dir / name
            safe_extract(staged_archive, staged_extracted)
            receipt_artifacts.append(
                {
                    "name": name,
                    "artifact_id": artifact_id,
                    "api_digest": api_row["digest"],
                    "api_size_in_bytes": api_row["size_in_bytes"],
                    "archive": staged_row(
                        staged_archive, final_archive
                    ),
                    "zip_members": zip_members,
                    "extracted_root": final_extracted.relative_to(
                        root
                    ).as_posix(),
                    "extracted_inventory": tree_inventory(
                        staged_extracted, root
                    ),
                }
            )

        receipt = {
            "schema": 2,
            "kind": "core_actions_run_metadata",
            "mode": "github-api-artifact-capture",
            "ok": True,
            "repository": REPOSITORY,
            "expected_commit": commit,
            "github_actions_run": run_id,
            "workflow_run_attempt": str(run_payload["run_attempt"]),
            "captured_at": utc_now(),
            "raw_run_response": staged_row(
                staged_run_raw, final_run_raw
            ),
            "raw_artifacts_response": staged_row(
                staged_artifacts_raw, final_artifacts_raw
            ),
            "artifacts": receipt_artifacts,
            "git": source,
        }
        staged_receipt.write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n",
            encoding="ascii",
        )
        staging.replace(github_run_dir)
        return final_receipt
    except Exception:
        if staging.exists():
            shutil.rmtree(staging)
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--out-dir")
    parser.add_argument("--github-run-dir")
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    github_run_dir = (
        Path(args.github_run_dir)
        if args.github_run_dir
        else root / "artifacts" / "github" / str(args.github_run_id)
    )
    if not github_run_dir.is_absolute():
        github_run_dir = root / github_run_dir
    out_dir = (
        Path(args.out_dir)
        if args.out_dir
        else github_run_dir / "core-actions-run-metadata"
    )
    if not out_dir.is_absolute():
        out_dir = root / out_dir
    try:
        out = capture(
            root=root,
            run_id=str(args.github_run_id),
            commit=args.commit,
            out_dir=out_dir,
            github_run_dir=github_run_dir,
        )
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(json.dumps({"ok": True, "out": str(out)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
