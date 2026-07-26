import copy
import json
import os
import re
import types
from pathlib import Path

import pytest

from scripts import (
    core_flash_only_d1l,
    core_release_gate_audit_d1l,
    package_release_d1l,
)
from scripts.verify_checksums import verify_checksum_tree
from tests.test_package_release_d1l import (
    fake_source_identity,
    install_fake_source_identity,
    write_fake_build,
    write_fake_config,
    write_fake_notices,
    write_fake_rp2040_artifacts,
)


def load_generated_flash_runner(package: Path) -> types.ModuleType:
    path = package / "flash_project.py"
    module = types.ModuleType("generated_core_flash_runner")
    module.__file__ = str(path)
    exec(compile(path.read_text(encoding="ascii"), str(path), "exec"), module.__dict__)
    return module


def assert_actions_identity(
    text: str,
    *,
    run_id: str,
    run_attempt: str,
) -> None:
    assert re.findall(
        r"^GitHub Actions run: `([^`]+)`$",
        text,
        flags=re.MULTILINE,
    ) == [run_id]
    assert re.findall(
        r"^GitHub Actions run attempt: `([^`]+)`$",
        text,
        flags=re.MULTILINE,
    ) == [run_attempt]


def test_core_disabled_package_binds_truth_and_omits_rp2040(
    tmp_path, monkeypatch
):
    root = tmp_path
    build = root / "build"
    github_run_dir = root / "github-run"
    out = github_run_dir / "d1l-release-package"
    write_fake_build(build)
    write_fake_notices(root)
    write_fake_config(root)
    rp2040 = write_fake_rp2040_artifacts(root)
    commit = "a" * 40
    monkeypatch.setenv("GITHUB_SHA", commit)
    monkeypatch.setenv("GITHUB_RUN_ID", "123456789")
    monkeypatch.setenv("GITHUB_RUN_ATTEMPT", "1")
    monkeypatch.setenv("GITHUB_REPOSITORY", "n30nex/SIGUI")
    monkeypatch.setenv("GITHUB_WORKFLOW", "d1l-ci")
    monkeypatch.setenv("GITHUB_REF", "refs/heads/release/24h-core")
    install_fake_source_identity(monkeypatch, commit)

    manifest = package_release_d1l.create_release_package(
        root=root,
        build_dir=build,
        out_dir=out,
        package_name="core-disabled",
        full_size=0x20000,
        rp2040_artifact_root=rp2040,
        release_profile="core_1_0",
        sd_history_mode="disabled",
    )

    package = out / "core-disabled"
    truth = package_release_d1l.core_capability_truth("disabled")
    assert manifest["release_profile"] == "core_1_0"
    assert manifest["firmware_commit"] == commit
    assert manifest["actions_run"] == "123456789"
    assert manifest["actions_run_attempt"] == "1"
    assert manifest["supported_capabilities"] == truth["supported_capabilities"]
    assert (
        manifest["unavailable_capabilities"]
        == truth["unavailable_capabilities"]
    )
    assert "sd_history" in manifest["unavailable_capabilities"]
    assert manifest["sd_history_mode"] == "disabled"
    assert manifest["storage_authority"] == "nvs"
    assert manifest["full_feature_release_ready"] is False
    assert manifest["rp2040_artifacts"] == []
    assert manifest["update_image"] is None
    assert not (package / "rp2040").exists()
    assert not (package / "update").exists()
    assert manifest["release_docs"] == [
        {
            "path": "docs/CORE_INSTALL_RECOVERY.md",
            "source": "generated_core_profile",
            "size": (
                package / "docs" / "CORE_INSTALL_RECOVERY.md"
            ).stat().st_size,
            "sha256": package_release_d1l.sha256_file(
                package / "docs" / "CORE_INSTALL_RECOVERY.md"
            ),
        }
    ]
    assert (package / "docs" / "CORE_INSTALL_RECOVERY.md").is_file()
    for excluded in (
        "USER_GUIDE_D1L.md",
        "DEVELOPER_GUIDE_D1L.md",
        "FLASH_RECOVERY_D1L.md",
        "RP2040_SD_BRIDGE_FLASH_D1L.md",
    ):
        assert not (package / "docs" / excluded).exists()
    assert (package / "SUPPORTED_FEATURES.md").is_file()
    supported_text = (package / "SUPPORTED_FEATURES.md").read_text(
        encoding="ascii"
    )
    assert "SD history is disabled and deferred" in supported_text
    assert "NVS is authoritative" in supported_text
    assert "Never format an SD card on the device" in supported_text
    assert_actions_identity(
        supported_text,
        run_id="123456789",
        run_attempt="1",
    )
    assert (
        "GitHub Actions run, and run attempt\nshown above"
        in supported_text
    )
    assert manifest["supported_features"]["sha256"] == (
        package_release_d1l.sha256_file(package / "SUPPORTED_FEATURES.md")
    )
    assert manifest["schema"] == 2
    install = manifest["install_recovery_guide"]
    assert install["schema"] == 2
    assert install["normal_install_script"] == "flash_project.py"
    assert install["normal_install_scripts"] == {
        "windows": "flash_project.ps1",
        "posix": "flash_project.sh",
    }
    stable_posix = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
    assert install["normal_install_port"] == stable_posix
    assert install["normal_install_targets"] == {
        "windows": {
            "requested_path": None,
            "target_kind": "windows_com_operator_supplied",
            "vid": 0x1A86,
            "pid": 0x7523,
            "qualifying": False,
            "explicit_operator_port_required": True,
            "port_probe_forbidden": True,
        },
        "posix": {
            "requested_path": stable_posix,
            "target_kind": "posix_by_id",
            "vid": 0x1A86,
            "pid": 0x7523,
            "qualifying": True,
        },
    }
    assert install["target_policy"] == {
        "stable_requested_path_only": True,
        "resolved_tty_observational_only": True,
        "hardware_identity_required": True,
        "raw_posix_tty_forbidden": True,
        "qualification_platform": "posix",
        "qualification_target": stable_posix,
        "windows_manual_non_qualifying": True,
        "windows_explicit_operator_port_required": True,
        "windows_port_probe_forbidden": True,
    }
    assert install["recovery_platform"] == "windows_only"
    assert install["posix_recovery_script"] is None
    assert install["install_guide"] == "docs/CORE_INSTALL_RECOVERY.md"
    assert install["no_on_device_sd_format"] is True
    assert manifest["scripts"] == {
        "shared_project_flash": "flash_project.py",
        "serial_target_resolver": "d1l_serial_target.py",
        "windows_project_flash": "flash_project.ps1",
        "posix_project_flash": "flash_project.sh",
        "windows_full_flash": "flash_full_8mb.ps1",
        "posix_full_flash": None,
    }
    for relative, binding in install["generated_files"].items():
        generated = package / relative
        assert generated.is_file()
        assert binding == {
            "size": generated.stat().st_size,
            "sha256": package_release_d1l.sha256_file(generated),
        }
    actions_flash_files = [
        {
            "offset": int(row["offset"], 0),
            "path": f"build/{row['source']}",
            "size": row["size"],
            "sha256": row["sha256"],
        }
        for row in manifest["flash_files"]
    ]
    verified = core_flash_only_d1l.verify_core_package(
        github_run_dir=github_run_dir,
        package_dir=package,
        commit=commit,
        run_id="123456789",
        run_attempt="1",
        actions_verification={"flash_files": actions_flash_files},
    )
    assert verified["ok"] is True

    ps1 = (package / "flash_project.ps1").read_text(encoding="ascii")
    sh = (package / "flash_project.sh").read_text(encoding="ascii")
    runner_text = (package / "flash_project.py").read_text(encoding="ascii")
    recovery = (package / "flash_full_8mb.ps1").read_text(encoding="ascii")
    guide = (
        package / "docs" / "CORE_INSTALL_RECOVERY.md"
    ).read_text(encoding="ascii")
    assert "NON-QUALIFYING MANUAL WINDOWS INSTALL" in ps1
    assert "Parameter(Mandatory=$true)" in ps1
    assert "flash_project.py" in ps1
    assert stable_posix in sh
    assert os.access(package / "flash_project.sh", os.X_OK)
    assert "/dev/ttyUSB<number>" in guide
    assert stable_posix in guide
    assert "non-qualifying manual convenience" in guide
    assert "No POSIX recovery wrapper is shipped" in " ".join(guide.split())
    for generated_text in (ps1, runner_text, recovery, guide):
        assert "COM12" not in generated_text
    assert_actions_identity(
        guide,
        run_id="123456789",
        run_attempt="1",
    )
    assert "verify_complete_package(root)" in runner_text
    assert "authorized_port = snapshot[\"requested_path\"]" in runner_text
    assert "erase-flash" not in runner_text
    assert runner_text.index("verify_complete_package(root)") < runner_text.index(
        "runner(command, cwd=root)"
    )
    assert recovery.index("--validate-only") < recovery.index(
        "python -m esptool"
    )

    runner = load_generated_flash_runner(package)
    commands: list[list[str]] = []

    def record_command(command, **_kwargs):
        commands.append(command)
        return 0

    manual_windows_port = "COM37"
    windows = runner.run_flash(
        manual_windows_port,
        package_root=package,
        platform_name="nt",
        port_lister=lambda: [
            {
                "device": manual_windows_port,
                "vid": 0x1A86,
                "pid": 0x7523,
                "hwid": "USB VID:PID=1A86:7523",
            }
        ],
        command_runner=record_command,
    )
    windows_command = windows["command"]
    assert windows_command[windows_command.index("--port") + 1] == (
        manual_windows_port
    )
    assert windows["target"]["qualification_route"] is False
    assert windows["target"]["operator_supplied"] is True

    raw_tty = "/dev/ttyUSB2"
    posix_hooks = {
        "exists": lambda path: path in {stable_posix, raw_tty},
        "is_symlink": lambda path: path == stable_posix,
        "realpath": lambda path: raw_tty if path == stable_posix else path,
        "access": lambda path, _mode: path == raw_tty,
        "hostname": lambda: "neopi5",
    }
    posix = runner.run_flash(
        stable_posix,
        package_root=package,
        platform_name="posix",
        port_lister=lambda: [
            {
                "device": raw_tty,
                "vid": 0x1A86,
                "pid": 0x7523,
                "hwid": "USB VID:PID=1A86:7523",
            }
        ],
        resolver_hooks=posix_hooks,
        command_runner=record_command,
    )
    posix_command = posix["command"]
    assert posix_command[posix_command.index("--port") + 1] == stable_posix
    assert raw_tty not in posix_command

    def esptool_must_not_run(*_args, **_kwargs):
        pytest.fail("esptool command ran after a rejected package or target")

    with pytest.raises(ValueError, match="present exactly once"):
        runner.run_flash(
            "COM38",
            package_root=package,
            platform_name="nt",
            port_lister=lambda: [],
            command_runner=esptool_must_not_run,
        )
    for wrong_vid, wrong_pid, expected_error in (
        (0x10C4, 0x7523, "VID"),
        (0x1A86, 0xEA60, "PID"),
    ):
        with pytest.raises(ValueError, match=expected_error):
            runner.run_flash(
                manual_windows_port,
                package_root=package,
                platform_name="nt",
                port_lister=lambda: [
                    {
                        "device": manual_windows_port,
                        "vid": wrong_vid,
                        "pid": wrong_pid,
                        "hwid": "wrong adapter",
                    }
                ],
                command_runner=esptool_must_not_run,
            )
    with pytest.raises(ValueError, match="exactly"):
        runner.run_flash(
            raw_tty,
            package_root=package,
            platform_name="posix",
            port_lister=lambda: [],
            command_runner=esptool_must_not_run,
        )

    readme_path = package / "README_RELEASE.md"
    readme_text = readme_path.read_text(encoding="ascii")
    assert_actions_identity(
        readme_text,
        run_id="123456789",
        run_attempt="1",
    )
    assert (
        "Actions run\n`123456789`, run attempt\n`1`"
        in readme_text
    )
    assert stable_posix in readme_text
    assert "non-qualifying manual convenience" in readme_text
    assert "COM12" not in readme_text
    original_readme = readme_path.read_bytes()
    readme_path.write_bytes(original_readme + b"tampered\n")
    try:
        with pytest.raises(ValueError, match="SHA256 mismatch"):
            runner.run_flash(
                manual_windows_port,
                package_root=package,
                platform_name="nt",
                port_lister=lambda: [],
                command_runner=esptool_must_not_run,
            )
    finally:
        readme_path.write_bytes(original_readme)
    assert "./rp2040/" not in (package / "SHA256SUMS.txt").read_text(
        encoding="ascii"
    )
    provenance_path = package / manifest["provenance"]["path"]
    provenance = json.loads(provenance_path.read_text(encoding="ascii"))
    assert provenance["predicate"]["buildDefinition"]["externalParameters"] == {
        "sourceRepository": "https://github.com/n30nex/SIGUI",
        "sourceRevision": commit,
        "releaseProfile": "core_1_0",
        "workflowRepository": "n30nex/SIGUI",
        "workflowName": "d1l-ci",
        "workflowPath": ".github/workflows/d1l-ci.yml",
        "workflowRunId": "123456789",
        "workflowRunAttempt": "1",
    }
    assert manifest["provenance"]["release_profile"] == "core_1_0"
    assert manifest["provenance"]["workflow_repository"] == "n30nex/SIGUI"
    assert manifest["provenance"]["workflow_name"] == "d1l-ci"
    assert manifest["provenance"]["workflow_path"] == (
        ".github/workflows/d1l-ci.yml"
    )
    assert manifest["provenance"]["workflow_run_id"] == "123456789"
    assert manifest["provenance"]["workflow_run_attempt"] == "1"

    capability_payload = package_release_d1l.load_required_json_object(
        package / manifest["capability_manifest"]["path"],
        "Core capability manifest",
    )
    assert capability_payload["release_profile"] == "core_1_0"
    assert capability_payload["sd_history_mode"] == "disabled"
    assert capability_payload["supported_capabilities"] == truth[
        "supported_capabilities"
    ]
    assert capability_payload["capabilities"] == [
        {"id": capability, "core_state": "supported"}
        for capability in truth["supported_capabilities"]
    ] + [
        {"id": capability, "core_state": "unavailable"}
        for capability in truth["unavailable_capabilities"]
    ]
    assert capability_payload["full_feature_release_ready"] is False
    evidence_payload = package_release_d1l.load_required_json_object(
        package / manifest["release_evidence_index"]["path"],
        "Core release evidence index",
    )
    assert "scripts/rc1_release_gate_audit_d1l.py" in evidence_payload["note"]
    assert "scripts/core_release_gate_audit_d1l.py" not in evidence_payload["note"]

    monkeypatch.setattr(
        core_release_gate_audit_d1l,
        "find_release_package",
        lambda _run_dir: package,
    )
    monkeypatch.setattr(
        core_release_gate_audit_d1l,
        "git_metadata",
        lambda _root: {
            "commit": commit,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(
        core_release_gate_audit_d1l,
        "discover_source_identity",
        lambda _root, _expected: fake_source_identity(commit),
    )
    assert core_release_gate_audit_d1l.package_gate(
        tmp_path,
        root,
        commit,
        "123456789",
        "1",
        "disabled",
    ).ok

    original_provenance = copy.deepcopy(provenance)
    for field_name, transplanted_value in (
        ("workflowRunId", "987654321"),
        ("workflowRunAttempt", "2"),
        ("releaseProfile", "d1l"),
    ):
        transplanted = copy.deepcopy(original_provenance)
        transplanted["predicate"]["buildDefinition"]["externalParameters"][
            field_name
        ] = transplanted_value
        provenance_path.write_text(
            package_release_d1l.canonical_json(transplanted),
            encoding="ascii",
        )
        manifest["provenance"]["sha256"] = package_release_d1l.sha256_file(
            provenance_path
        )
        (package / "manifest.json").write_text(
            json.dumps(manifest, indent=2),
            encoding="ascii",
        )
        gate = core_release_gate_audit_d1l.package_gate(
            tmp_path,
            root,
            commit,
            "123456789",
            "1",
            "disabled",
        )
        assert gate.ok is False
        assert "provenance.semantic_binding" in gate.details["failures"]

    for tamper in ("subject_digest", "material_digest"):
        transplanted = copy.deepcopy(original_provenance)
        if tamper == "subject_digest":
            transplanted["subject"][0]["digest"]["sha256"] = "0" * 64
        else:
            material = next(
                row
                for row in transplanted["predicate"]["buildDefinition"][
                    "resolvedDependencies"
                ]
                if "sha256" in row["digest"]
            )
            material["digest"]["sha256"] = "0" * 64
        provenance_path.write_text(
            package_release_d1l.canonical_json(transplanted),
            encoding="ascii",
        )
        manifest["provenance"]["sha256"] = package_release_d1l.sha256_file(
            provenance_path
        )
        (package / "manifest.json").write_text(
            json.dumps(manifest, indent=2),
            encoding="ascii",
        )
        package_release_d1l.write_sha256sums(package)
        assert verify_checksum_tree(package) is True
        gate = core_release_gate_audit_d1l.package_gate(
            tmp_path,
            root,
            commit,
            "123456789",
            "1",
            "disabled",
        )
        assert gate.ok is False
        assert "provenance.semantic_binding" in gate.details["failures"]
        assert (
            "provenance does not match deterministic source and package inputs"
            in gate.details["provenance_validation_errors"]
        )


def test_core_supported_optional_package_requires_paired_rp2040(
    tmp_path, monkeypatch
):
    build = tmp_path / "build"
    write_fake_build(build)
    write_fake_notices(tmp_path)
    write_fake_config(tmp_path)
    commit = "b" * 40
    monkeypatch.setenv("GITHUB_SHA", commit)
    monkeypatch.setenv("GITHUB_RUN_ID", "234567890")
    monkeypatch.setenv("GITHUB_RUN_ATTEMPT", "1")
    monkeypatch.setenv("GITHUB_REPOSITORY", "n30nex/SIGUI")
    monkeypatch.setenv("GITHUB_REF", "refs/heads/release/24h-core")
    install_fake_source_identity(monkeypatch, commit)

    with pytest.raises(ValueError, match="paired RP2040"):
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=tmp_path / "release",
            package_name="core-sd",
            full_size=0x20000,
            release_profile="core_1_0",
            sd_history_mode="supported_optional",
        )


def test_rc1_conditional_capability_truth_matches_production_surface():
    truth = package_release_d1l.core_capability_truth("conditional")

    for capability in (
        "sd_history",
        "map",
        "wifi_user_control",
        "multi_channel_management",
        "admin",
        "observer_mqtt",
        "mutable_terminal",
        "location",
        "user_trace",
    ):
        assert capability in truth["supported_capabilities"]
        assert capability not in truth["unavailable_capabilities"]

    assert truth["unavailable_capabilities"] == [
        "usb_recovery",
        "ble",
        "signed_update",
        "advanced_qr_emoji",
    ]
    assert truth["sd_history_state"] == "runtime_conditional_sd_primary"
    assert truth["storage_authority"] == "sd_primary_live_only_without_sd"


def test_sd_preparation_bundle_is_packaged_without_format_authority(tmp_path):
    script = tmp_path / "scripts" / "prepare_deskos_sd.py"
    script.parent.mkdir(parents=True)
    script.write_text("#!/usr/bin/env python3\n", encoding="ascii")
    manifest = tmp_path / "sdcard" / "deskos" / "manifest.json"
    manifest.parent.mkdir(parents=True)
    manifest.write_text('{"schema":1}\n', encoding="ascii")
    package = tmp_path / "package"
    package.mkdir()

    receipt = package_release_d1l.copy_sd_preparation_bundle(tmp_path, package)

    assert receipt["script"] == "scripts/prepare_deskos_sd.py"
    assert receipt["bundle_root"] == "sdcard"
    assert receipt["minimum_card_bytes"] == 28_000_000_000
    assert receipt["filesystem"] == "FAT32"
    assert receipt["formats_sd"] is False
    assert (package / receipt["script"]).is_file()
    assert (package / "sdcard" / "deskos" / "manifest.json").is_file()
    assert {row["path"] for row in receipt["files"]} == {
        "scripts/prepare_deskos_sd.py",
        "sdcard/deskos/manifest.json",
    }


def test_core_package_requires_exact_actions_run(tmp_path, monkeypatch):
    build = tmp_path / "build"
    write_fake_build(build)
    write_fake_notices(tmp_path)
    write_fake_config(tmp_path)
    commit = "c" * 40
    monkeypatch.setenv("GITHUB_SHA", commit)
    monkeypatch.delenv("GITHUB_RUN_ID", raising=False)
    monkeypatch.setenv("GITHUB_RUN_ATTEMPT", "1")
    install_fake_source_identity(monkeypatch, commit)

    with pytest.raises(ValueError, match="numeric GitHub Actions run ID"):
        package_release_d1l.create_release_package(
            root=tmp_path,
            build_dir=build,
            out_dir=tmp_path / "release",
            package_name="core-no-run",
            full_size=0x20000,
            release_profile="core_1_0",
            sd_history_mode="disabled",
        )


def test_build_release_settings_read_exact_cmake_cache(tmp_path):
    (tmp_path / "CMakeCache.txt").write_text(
        "\n".join(
            (
                "D1L_RELEASE_PROFILE:STRING=core_1_0",
                "D1L_SD_HISTORY_MODE:STRING=disabled",
            )
        )
        + "\n",
        encoding="ascii",
    )

    assert package_release_d1l.build_release_settings(tmp_path) == (
        "core_1_0",
        "disabled",
    )
    with pytest.raises(ValueError, match="does not match configured firmware"):
        package_release_d1l.build_release_settings(
            tmp_path, release_profile="full_feature"
        )


def test_default_callable_package_profile_behavior_is_unchanged(tmp_path):
    assert package_release_d1l.build_release_settings(tmp_path) == (
        "full_feature",
        "conditional",
    )
