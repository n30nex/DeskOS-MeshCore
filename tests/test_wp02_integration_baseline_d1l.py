from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from scripts import wp02_integration_baseline_d1l as baseline


RUN_ID = "29300000001"
D1L_PORT = "COM12"
RP2040_PORT = "COM16"


def git(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=root,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result.stdout.strip()


def commit_file(root: Path, name: str, text: str) -> str:
    (root / name).write_text(text, encoding="utf-8")
    git(root, "add", name)
    git(root, "commit", "-m", f"add {name}")
    return git(root, "rev-parse", "HEAD")


def write_manifest(root: Path) -> Path:
    manifest = root / "SHA256SUMS.txt"
    rows = []
    for path in sorted(
        candidate
        for candidate in root.rglob("*")
        if candidate.is_file() and candidate != manifest
    ):
        rows.append(
            f"{baseline.sha256_file(path)}  {path.relative_to(root).as_posix()}"
        )
    manifest.write_text("\n".join(rows) + "\n", encoding="ascii")
    return manifest


def write_actions_fixture(root: Path, commit: str) -> tuple[Path, dict[str, str]]:
    run_dir = root / "artifacts" / "github" / f"{RUN_ID}-{commit[:7]}"
    firmware_root = run_dir / "d1l-firmware-artifacts"
    build = firmware_root / "build"
    sources = {
        "bootloader": ("bootloader/bootloader.bin", b"bootloader"),
        "partition-table": ("partition_table/partition-table.bin", b"partition"),
        "ota-data": ("ota_data_initial.bin", b"ota-data"),
        "app": ("meshcore_deskos_d1l.bin", b"application"),
    }
    for source, content in sources.values():
        path = build / source
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    write_manifest(firmware_root)

    direct_groups: dict[str, Path] = {}
    for index, name in enumerate(baseline.RP2040_GROUPS, start=1):
        group = run_dir / name
        group.mkdir(parents=True, exist_ok=True)
        (group / f"{name}.uf2").write_bytes(f"rp2040-{index}".encode("ascii"))
        write_manifest(group)
        direct_groups[name] = group

    package = run_dir / "d1l-release-package" / f"d1l-release-{commit}"
    flash_files = []
    package_paths = {
        "bootloader": "firmware/bootloader.bin",
        "partition-table": "firmware/partition-table.bin",
        "ota-data": "firmware/ota_data_initial.bin",
        "app": "firmware/meshcore_deskos_d1l.bin",
    }
    for role, (source, _) in sources.items():
        direct = build / source
        packaged = package / package_paths[role]
        packaged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(direct, packaged)
        flash_files.append(
            {
                "role": role,
                "source": source,
                "path": package_paths[role],
                "size": direct.stat().st_size,
                "sha256": baseline.sha256_file(direct),
            }
        )

    package_groups = []
    for name, direct_root in direct_groups.items():
        files = []
        for direct in sorted(path for path in direct_root.rglob("*") if path.is_file()):
            local = direct.relative_to(direct_root).as_posix()
            package_relative = f"rp2040/{name}/{local}"
            packaged = package / package_relative
            packaged.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(direct, packaged)
            files.append(
                {
                    "path": package_relative,
                    "size": direct.stat().st_size,
                    "sha256": baseline.sha256_file(direct),
                }
            )
        package_groups.append({"name": name, "path": f"rp2040/{name}", "files": files})

    package.mkdir(parents=True, exist_ok=True)
    package_manifest = package / "manifest.json"
    package_manifest.write_text(
        json.dumps(
            {
                "schema": 1,
                "package": f"d1l-release-{commit}",
                "git": {"commit": commit, "dirty": False},
                "workflow": {
                    "run_id": RUN_ID,
                    "run_attempt": "1",
                    "sha": commit,
                    "repository": baseline.REPOSITORY,
                },
                "flash_files": flash_files,
                "rp2040_artifacts": package_groups,
            },
            sort_keys=True,
        ),
        encoding="utf-8",
    )
    write_manifest(package)

    host = run_dir / "d1l-host-artifacts" / "host-checks"
    host.mkdir(parents=True, exist_ok=True)
    (host / f"d1l_host_checks_success_{commit}.json").write_text(
        json.dumps(
            {
                "schema": 1,
                "artifact_type": "d1l_host_checks_success",
                "status": "pass",
                "passed": True,
                "all_prior_steps_completed": True,
                "job": "host-checks",
                "repository_commit": commit,
                "workflow_run_id": RUN_ID,
                "workflow_run_attempt": "1",
            },
            sort_keys=True,
        ),
        encoding="utf-8",
    )
    identities = {
        "release_manifest_sha256": baseline.sha256_file(package_manifest),
        "esp32_app_sha256": baseline.sha256_file(build / sources["app"][0]),
        "rp2040_bridge_sha256": baseline.sha256_file(
            direct_groups["rp2040-sd-bridge-firmware"] / "rp2040-sd-bridge-firmware.uf2"
        ),
    }
    return run_dir, identities


def write_receipts(
    root: Path, commit: str, identities: dict[str, str]
) -> dict[str, Path]:
    receipt_root = root / "artifacts" / "hardware" / "wp02"
    receipt_root.mkdir(parents=True, exist_ok=True)
    paths: dict[str, Path] = {}
    for role, required_checks in baseline.ROLE_CHECKS.items():
        payload = {
            "schema": 1,
            "kind": f"wp02_{role}_qualification",
            "mode": "hardware",
            "physical_observed": True,
            "dry_run": False,
            "simulated": False,
            "simulation": False,
            "source_inspection": False,
            "ok": True,
            "commit": commit,
            "github_actions_run": RUN_ID,
            "device_build_commit": commit,
            "firmware_identity_ok": True,
            "ports": {"d1l": D1L_PORT, "rp2040": RP2040_PORT},
            "artifact_identity": {
                "release_manifest_sha256": identities["release_manifest_sha256"],
                "esp32_app_sha256": identities["esp32_app_sha256"],
            },
            "checks": {name: True for name in required_checks},
            "public_rf_tx": False,
            "dm_rf_tx": False,
            "formats_sd": False,
            "failures": [],
        }
        if role in {"sd", "reboot"}:
            payload["rp2040_device_build_commit"] = commit
            payload["artifact_identity"]["rp2040_bridge_sha256"] = identities[
                "rp2040_bridge_sha256"
            ]
        path = receipt_root / f"{role}.json"
        path.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        paths[role] = path
    return paths


@pytest.fixture()
def context(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> dict:
    root = tmp_path / "repo"
    root.mkdir()
    git(root, "init", "-b", "main")
    git(root, "config", "user.email", "qa@example.invalid")
    git(root, "config", "user.name", "WP02 QA")
    (root / ".gitignore").write_text("artifacts/\n", encoding="utf-8")
    git(root, "add", ".gitignore")
    git(root, "commit", "-m", "initial")
    pr62 = commit_file(root, "pr62.txt", "62\n")
    pr64 = commit_file(root, "pr64.txt", "64\n")
    pr80 = commit_file(root, "pr80.txt", "80\n")
    monkeypatch.setattr(
        baseline,
        "TRUSTED_PR_LAYERS",
        {62: pr62, 64: pr64, 80: pr80},
    )
    run_dir, identities = write_actions_fixture(root, pr80)
    receipts = write_receipts(root, pr80, identities)
    assert git(root, "status", "--porcelain", "--untracked-files=all") == ""
    return {
        "root": root,
        "commit": pr80,
        "layers": {62: pr62, 64: pr64, 80: pr80},
        "run_dir": run_dir,
        "identities": identities,
        "receipts": receipts,
    }


def build(context: dict, **overrides) -> dict:
    values = {
        "root": context["root"],
        "main_sha": context["commit"],
        "github_actions_run": RUN_ID,
        "github_run_dir": context["run_dir"],
        "d1l_port": D1L_PORT,
        "rp2040_port": RP2040_PORT,
        "layers": context["layers"],
        "receipts": context["receipts"],
    }
    values.update(overrides)
    return baseline.build_integration_baseline(**values)


def mutate_receipt(path: Path, mutation) -> None:
    payload = json.loads(path.read_text(encoding="utf-8"))
    mutation(payload)
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def test_exact_main_baseline_is_deterministic_and_source_bound(context: dict):
    first = build(context)
    second = build(context)

    assert first == second
    assert first["ok"] is True
    assert first["failures"] == []
    assert first["repository"]["main_ref"] == "refs/heads/main"
    assert [row["pr"] for row in first["repository"]["layers"]] == [62, 64, 80]
    assert all(row["ancestor_of_main"] for row in first["repository"]["layers"])
    assert first["actions"]["checksum_manifest_count"] == 8
    assert first["actions"]["checksum_entry_count"] > 0
    assert first["actions"]["tree_file_count"] > 0
    assert all(first["checks"].values())
    assert baseline.validate_integration_baseline(first, context["root"]) is True


def test_dirty_or_non_main_repository_fails_closed(context: dict):
    dirty = context["root"] / "dirty.txt"
    dirty.write_text("dirty\n", encoding="utf-8")
    report = build(context)
    assert report["ok"] is False
    assert "repository:dirty" in report["failures"]
    dirty.unlink()

    git(context["root"], "checkout", "-b", "future")
    future = commit_file(context["root"], "future.txt", "future\n")
    report = build(context, main_sha=future)
    assert report["ok"] is False
    assert "repository:main_ref_not_exact_main_sha" in report["failures"]


def test_missing_receipt_baseline_is_repo_relative_and_reproducible_across_roots(
    context: dict, tmp_path: Path
):
    clone_root = tmp_path / "relocated-repo"
    shutil.copytree(context["root"], clone_root)

    for path in context["receipts"].values():
        path.unlink()
    clone_receipts = {
        role: clone_root / path.relative_to(context["root"])
        for role, path in context["receipts"].items()
    }
    for path in clone_receipts.values():
        path.unlink()

    original = build(context)
    relocated = build(
        context,
        root=clone_root,
        github_run_dir=clone_root / context["run_dir"].relative_to(context["root"]),
        receipts=clone_receipts,
    )

    assert original == relocated
    assert original["ok"] is False
    assert str(context["root"]) not in json.dumps(original)
    assert str(clone_root) not in json.dumps(relocated)
    assert all(
        entry["path"].startswith("artifacts/hardware/wp02/")
        for entry in original["qualification"].values()
    )


def test_all_required_pr_layers_must_be_landed(context: dict):
    report = build(
        context, layers={62: context["layers"][62], 80: context["layers"][80]}
    )

    assert report["ok"] is False
    assert "repository:required_pr_layers_incomplete" in report["failures"]
    assert "repository:pr_64_commit_invalid" in report["failures"]


def test_required_pr_layers_reject_wrong_or_duplicate_valid_ancestors(context: dict):
    initial = git(context["root"], "rev-list", "--max-parents=0", "HEAD")
    report = build(context, layers={62: initial, 64: initial, 80: context["commit"]})

    assert report["ok"] is False
    assert "repository:pr_62_commit_untrusted" in report["failures"]
    assert "repository:pr_64_commit_untrusted" in report["failures"]
    assert report["repository"]["checks"]["required_layers_exact"] is False
    assert report["repository"]["checks"]["required_layers_unique"] is False
    assert report["repository"]["checks"]["required_layers_landed"] is False


def test_tampered_actions_payload_fails_checksum_and_payload_identity(context: dict):
    app = (
        context["run_dir"]
        / "d1l-firmware-artifacts"
        / "build"
        / "meshcore_deskos_d1l.bin"
    )
    app.write_bytes(b"tampered")

    report = build(context)

    assert report["ok"] is False
    assert "actions:checksum_verification_failed" in report["failures"]
    assert "actions:firmware_payload_identity_invalid" in report["failures"]
    assert report["checks"]["actions_checksums_verified"] is False


@pytest.mark.parametrize(
    ("role", "mutation"),
    [
        ("board", lambda row: row.__setitem__("commit", "0" * 40)),
        ("ui", lambda row: row.__setitem__("github_actions_run", "999")),
        ("sd", lambda row: row.__setitem__("public_rf_tx", True)),
        (
            "reboot",
            lambda row: row["artifact_identity"].__setitem__(
                "rp2040_bridge_sha256", "0" * 64
            ),
        ),
        ("map_open", lambda row: row["checks"].__setitem__("map_opened", False)),
    ],
)
def test_each_qualification_role_rejects_stale_or_unsafe_receipts(
    context: dict, role: str, mutation
):
    mutate_receipt(context["receipts"][role], mutation)

    report = build(context)

    assert report["ok"] is False
    assert f"receipt:{role}:invalid_or_stale" in report["failures"]
    assert report["checks"][f"{role}_receipt"] is False


def test_sd_and_reboot_require_exact_rp2040_device_commit(context: dict):
    mutate_receipt(
        context["receipts"]["sd"],
        lambda row: row.__setitem__("rp2040_device_build_commit", "0" * 40),
    )

    report = build(context)

    assert report["ok"] is False
    assert "receipt:sd:invalid_or_stale" in report["failures"]


def test_receipts_must_be_independent_and_hardware_mode(context: dict):
    receipts = dict(context["receipts"])
    receipts["map_open"] = receipts["ui"]
    report = build(context, receipts=receipts)
    assert report["ok"] is False
    assert "receipt:paths_not_unique" in report["failures"]

    mutate_receipt(
        context["receipts"]["board"], lambda row: row.__setitem__("mode", "dry-run")
    )
    report = build(context)
    assert report["ok"] is False
    assert "receipt:board:invalid_or_stale" in report["failures"]


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("dry_run", True),
        ("simulated", True),
        ("simulation", True),
        ("source_inspection", True),
        ("physical_observed", False),
        ("dry_run", "false"),
    ],
)
def test_hardware_receipts_reject_contradictory_or_noncanonical_physical_fields(
    context: dict, field: str, value: object
):
    mutate_receipt(
        context["receipts"]["board"], lambda row: row.__setitem__(field, value)
    )

    report = build(context)

    assert report["ok"] is False
    assert "receipt:board:invalid_or_stale" in report["failures"]
    assert report["checks"]["receipt_physical_fields"] is False


@pytest.mark.parametrize(
    ("d1l_port", "rp2040_port"),
    [("COM8", "COM16"), ("COM12", "COM11"), ("COM29", "COM16"), ("COM12", "COM12")],
)
def test_forbidden_or_ambiguous_ports_fail_closed(
    context: dict, d1l_port: str, rp2040_port: str
):
    report = build(context, d1l_port=d1l_port, rp2040_port=rp2040_port)

    assert report["ok"] is False
    assert "context:ports_invalid_or_forbidden" in report["failures"]


def test_validator_rebuilds_sources_and_rejects_artifact_tampering(context: dict):
    report = build(context)
    report["qualification"]["map_open"]["required_checks"]["map_rendered"] = False

    assert baseline.validate_integration_baseline(report, context["root"]) is False


def test_generate_and_validate_cli_use_full_main_sha_filename(context: dict, capsys):
    argv = [
        "generate",
        "--root",
        str(context["root"]),
        "--main-sha",
        context["commit"],
        "--github-actions-run",
        RUN_ID,
        "--github-run-dir",
        str(context["run_dir"]),
        "--d1l-port",
        D1L_PORT,
        "--rp2040-port",
        RP2040_PORT,
    ]
    for pr, commit in context["layers"].items():
        argv.extend(["--layer", f"{pr}={commit}"])
    for role, path in context["receipts"].items():
        argv.extend([f"--{role.replace('_', '-')}-receipt", str(path)])

    assert baseline.main(argv) == 0
    generated = (
        context["root"]
        / "artifacts"
        / "release"
        / f"integration_baseline_{context['commit']}.json"
    )
    assert generated.is_file()
    generated_bytes = generated.read_bytes()
    assert generated_bytes.endswith(b"}\n")
    assert b"\r\n" not in generated_bytes
    summary = json.loads(capsys.readouterr().out)
    assert summary["ok"] is True
    assert summary["artifact"].endswith(
        f"integration_baseline_{context['commit']}.json"
    )

    assert (
        baseline.main(
            [
                "validate",
                "--root",
                str(context["root"]),
                "--artifact",
                str(generated),
            ]
        )
        == 0
    )
    assert json.loads(capsys.readouterr().out)["ok"] is True
