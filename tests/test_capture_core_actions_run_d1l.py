import hashlib
import io
import json
import zipfile
from pathlib import Path

import pytest

from scripts import capture_core_actions_run_d1l as capture


COMMIT = "a" * 40
RUN_ID = "123456789"
RUN_ATTEMPT = "2"


def zip_bytes(files: dict[str, bytes]) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, payload in files.items():
            archive.writestr(name, payload)
    return output.getvalue()


def api_fixture(*, unsafe: bool = False):
    scope = {
        "schema": 1,
        "kind": "d1l_candidate_scope",
        "source_commit": COMMIT,
        "workflow_run_id": RUN_ID,
        "workflow_run_attempt": RUN_ATTEMPT,
        "repository": "n30nex/SIGUI",
        "workflow": "d1l-ci",
        "event": "workflow_dispatch",
        "include_sd_bridge": False,
        "scope_reason": "esp32_only",
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
    }
    archives = {}
    rows = []
    for index, name in enumerate(capture.EXPECTED_ACTIONS_ARTIFACTS, 1):
        files = (
            {
                "build-inputs/SHA256SUMS.txt": b"fixture checksum manifest",
                "build-inputs/d1l-candidate-scope.json": (
                    json.dumps(scope, sort_keys=True).encode("ascii")
                ),
                "build-inputs/lowercase.json": b"lowercase fixture",
            }
            if name == "d1l-host-artifacts"
            else {f"{name}.txt": name.encode("ascii")}
        )
        if unsafe and index == 1:
            files = {"../escape.txt": b"escape"}
        raw = zip_bytes(files)
        archives[index] = raw
        rows.append(
            {
                "id": index,
                "name": name,
                "expired": False,
                "size_in_bytes": len(raw),
                "digest": "sha256:" + hashlib.sha256(raw).hexdigest(),
                "workflow_run": {
                    "id": int(RUN_ID),
                    "head_sha": COMMIT,
                    "head_branch": "release/24h-core",
                },
            }
        )
    run = {
        "id": int(RUN_ID),
        "status": "completed",
        "conclusion": "success",
        "head_sha": COMMIT,
        "head_branch": "release/24h-core",
        "event": "workflow_dispatch",
        "path": ".github/workflows/d1l-ci.yml",
        "name": "d1l-ci",
        "run_attempt": int(RUN_ATTEMPT),
        "repository": {"full_name": "n30nex/SIGUI"},
    }
    artifacts = {"total_count": len(rows), "artifacts": rows}
    return run, artifacts, archives


def install_fake_api(monkeypatch, *, unsafe: bool = False):
    run, artifacts, archives = api_fixture(unsafe=unsafe)

    def fake_api(_root: Path, endpoint: str) -> bytes:
        if endpoint.endswith(f"/actions/runs/{RUN_ID}"):
            return json.dumps(run).encode("utf-8")
        if endpoint.endswith(
            f"/actions/runs/{RUN_ID}/artifacts?per_page=100"
        ):
            return json.dumps(artifacts).encode("utf-8")
        artifact_id = int(endpoint.split("/")[-2])
        return archives[artifact_id]

    monkeypatch.setattr(capture, "_api", fake_api)
    monkeypatch.setattr(
        capture,
        "git_metadata",
        lambda _root: {
            "commit": COMMIT,
            "dirty": False,
            "dirty_entries": [],
        },
    )


def test_capture_downloads_api_bound_archives_and_revalidates(
    tmp_path, monkeypatch
):
    install_fake_api(monkeypatch)
    run_dir = tmp_path / "artifacts" / "github" / RUN_ID
    receipt = capture.capture(
        root=tmp_path,
        run_id=RUN_ID,
        commit=COMMIT,
        out_dir=run_dir / "core-actions-run-metadata",
        github_run_dir=run_dir,
    )

    verified = capture.validate_capture_receipt(
        receipt_path=receipt,
        root=tmp_path,
        github_run_dir=run_dir,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    assert verified["ok"] is True
    assert [row["name"] for row in verified["artifacts"]] == list(
        capture.EXPECTED_ACTIONS_ARTIFACTS
    )
    assert len(list((run_dir / "_archives").glob("*.zip"))) == 5

    marker = (
        run_dir
        / "d1l-firmware-artifacts"
        / "d1l-firmware-artifacts.txt"
    )
    marker.write_text("tampered", encoding="ascii")
    with pytest.raises(ValueError, match="extracted tree"):
        capture.validate_capture_receipt(
            receipt_path=receipt,
            root=tmp_path,
            github_run_dir=run_dir,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
        )


def test_zip_and_extracted_inventory_use_the_same_mixed_case_order(tmp_path):
    archive = tmp_path / "mixed-case.zip"
    archive.write_bytes(
        zip_bytes(
            {
                "build-inputs/lowercase.json": b"lower",
                "build-inputs/SHA256SUMS.txt": b"upper",
            }
        )
    )
    extracted = tmp_path / "extracted"
    capture.safe_extract(archive, extracted)

    assert capture.zip_inventory(archive) == capture.tree_inventory(
        extracted, tmp_path
    )["files"]


def test_capture_rejects_unsafe_zip_and_refuses_overwrite(
    tmp_path, monkeypatch
):
    install_fake_api(monkeypatch, unsafe=True)
    run_dir = tmp_path / "artifacts" / "github" / RUN_ID
    with pytest.raises(ValueError, match="Unsafe ZIP member"):
        capture.capture(
            root=tmp_path,
            run_id=RUN_ID,
            commit=COMMIT,
            out_dir=run_dir / "core-actions-run-metadata",
            github_run_dir=run_dir,
        )
    assert not run_dir.exists()
    assert not list(run_dir.parent.glob(f".core-actions-{RUN_ID}-*"))

    install_fake_api(monkeypatch)
    receipt = capture.capture(
        root=tmp_path,
        run_id=RUN_ID,
        commit=COMMIT,
        out_dir=run_dir / "core-actions-run-metadata",
        github_run_dir=run_dir,
    )
    assert receipt.is_file()
    with pytest.raises(ValueError, match="refusing to overwrite"):
        capture.capture(
            root=tmp_path,
            run_id=RUN_ID,
            commit=COMMIT,
            out_dir=run_dir / "core-actions-run-metadata",
            github_run_dir=run_dir,
        )
