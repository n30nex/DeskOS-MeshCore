import copy
import json
import os
import re
import subprocess
import sys
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

ROOT = Path(__file__).resolve().parents[1]


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
    assert "core_release_ready" not in manifest
    assert "ready_for_public_release" not in manifest
    assert "source_build_dir" not in manifest
    assert manifest["rp2040_artifacts"] == []
    assert manifest["update_image"] is None
    assert not (package / "rp2040").exists()
    assert not (package / "update").exists()
    assert manifest["debug_files"] == []
    assert not (package / "debug").exists()
    package_release_d1l.validate_production_package_surface(package)
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
    with pytest.raises(
        ValueError,
        match="conditional-SD truth mismatch",
    ):
        core_flash_only_d1l.verify_core_package(
            github_run_dir=github_run_dir,
            package_dir=package,
            commit=commit,
            run_id="123456789",
            run_attempt="1",
            actions_verification={"flash_files": actions_flash_files},
        )

    ps1 = (package / "flash_project.ps1").read_text(encoding="ascii")
    sh = (package / "flash_project.sh").read_text(encoding="ascii")
    runner_text = (package / "flash_project.py").read_text(encoding="ascii")
    recovery = (package / "flash_full_8mb.ps1").read_text(encoding="ascii")
    guide = (
        package / "docs" / "CORE_INSTALL_RECOVERY.md"
    ).read_text(encoding="ascii")
    assert "Installing DeskOS D1L 1.0" in ps1
    assert "Parameter(Mandatory=$true)" in ps1
    assert "flash_project.py" in ps1
    assert stable_posix in sh
    assert os.access(package / "flash_project.sh", os.X_OK)
    assert "/dev/ttyUSB<number>" in guide
    assert stable_posix in guide
    assert "### Windows" in guide
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
    assert "operator-supplied `-Port`" in readme_text
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

    assert "build_inputs" not in manifest
    assert "capability_manifest" not in manifest
    assert not list(package.glob("build_inputs_*.json"))
    assert not list(package.glob("capability_manifest_*.json"))
    assert "release_evidence_index" not in manifest
    assert not list(package.glob("release_evidence_index_*.json"))

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


def test_core_conditional_package_is_production_only(
    tmp_path, monkeypatch
):
    build = tmp_path / "build"
    write_fake_build(build)
    write_fake_notices(tmp_path)
    write_fake_config(tmp_path)
    rp2040 = write_fake_rp2040_artifacts(tmp_path)
    bridge_dir = rp2040 / "rp2040-sd-bridge-firmware"
    old_bridge = bridge_dir / "rp2040-sd-bridge-firmware.uf2"
    bridge = bridge_dir / "deskos_sd_bridge.ino.uf2"
    old_bridge.rename(bridge)
    for name, content in {
        "deskos_sd_bridge.ino.bin": b"BIN",
        "deskos_sd_bridge.ino.elf": b"ELF",
        "deskos_sd_bridge.ino.map": b"MAP",
        "build-inputs.json": b"{}\n",
        "sdfat-no-usb-patch.json": b"{}\n",
    }.items():
        (bridge_dir / name).write_bytes(content)
    package_release_d1l.write_sha256sums(bridge_dir)
    prepare = tmp_path / "scripts" / "prepare_deskos_sd.py"
    prepare.write_text(
        (ROOT / "scripts" / "prepare_deskos_sd.py").read_text(encoding="ascii"),
        encoding="ascii",
    )
    sd_manifest = tmp_path / "sdcard" / "deskos" / "manifest.json"
    sd_manifest.parent.mkdir(parents=True)
    sd_manifest.write_text('{"schema":1}\n', encoding="ascii")

    commit = "d" * 40
    monkeypatch.setenv("GITHUB_SHA", commit)
    monkeypatch.setenv("GITHUB_RUN_ID", "345678901")
    monkeypatch.setenv("GITHUB_RUN_ATTEMPT", "2")
    monkeypatch.setenv("GITHUB_REPOSITORY", "n30nex/SIGUI")
    monkeypatch.setenv("GITHUB_WORKFLOW", "d1l-ci")
    monkeypatch.setenv("GITHUB_REF", "refs/heads/main")
    install_fake_source_identity(monkeypatch, commit)

    manifest = package_release_d1l.create_release_package(
        root=tmp_path,
        build_dir=build,
        out_dir=tmp_path / "release",
        package_name="core-conditional",
        full_size=0x20000,
        rp2040_artifact_root=rp2040,
        release_profile="core_1_0",
        sd_history_mode="conditional",
    )

    package = tmp_path / "release" / "core-conditional"
    user_install = manifest["user_install"]
    assert "core_release_ready" not in manifest
    assert "ready_for_public_release" not in manifest
    assert "source_build_dir" not in manifest
    assert user_install["guide"] == "START_HERE.md"
    assert user_install["windows"] == {
        "prepare_sd": "prepare_sd_card.ps1",
        "flash_rp2040": "flash_rp2040.ps1",
        "flash_esp32": "flash_project.ps1",
    }
    assert user_install["linux"] == {
        "prepare_sd": "prepare_sd_card.sh",
        "flash_rp2040": "flash_rp2040.sh",
        "flash_esp32": "flash_project.sh",
    }
    assert set(user_install["files"]) == set(
        package_release_d1l.PRODUCTION_USER_INSTALL_FILES
    )
    for relative, binding in user_install["files"].items():
        path = package / relative
        assert path.is_file()
        assert path.stat().st_size == binding["size"]
        assert package_release_d1l.sha256_file(path) == binding["sha256"]
    for relative in (
        "prepare_sd_card.sh",
        "flash_rp2040.sh",
    ):
        assert os.access(package / relative, os.X_OK)

    guide = (package / "START_HERE.md").read_text(encoding="ascii")
    assert commit in guide
    assert "GitHub Actions run and attempt: see `manifest.json`" in guide
    assert "## 1. Prepare the microSD card" in guide
    assert "## 2. Flash the RP2040 SD-bridge side" in guide
    assert "## 4. Flash the ESP32 main GUI side" in guide
    assert "# DeskOS D1L 1.0 - Windows and Linux Install" in guide
    assert "Run the read-only RC1 test" not in guide
    assert verify_checksum_tree(package) is True

    verified = subprocess.run(
        [sys.executable, package / "scripts" / "verify_package.py", package],
        cwd=package,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert verified.returncode == 0, verified.stderr
    assert "FAIL" not in verified.stdout
    production_preparer = (
        package / "scripts" / "prepare_deskos_sd.py"
    ).read_text(encoding="utf-8")
    assert "--skip-filesystem-check" not in production_preparer
    production_rp2040_helper = (
        package / "scripts" / "flash_rp2040_sd_bridge_uf2.py"
    ).read_text(encoding="utf-8")
    assert "dry-run" not in production_rp2040_helper
    assert '"preview"' in production_rp2040_helper
    assert not (package / "test_rc1.ps1").exists()
    assert not (package / "test_rc1.sh").exists()
    assert not (package / "scripts" / "test_rc1.py").exists()
    assert not (package / "evidence").exists()
    assert "build_inputs" not in manifest
    assert "capability_manifest" not in manifest
    assert not list(package.glob("build_inputs_*.json"))
    assert not list(package.glob("capability_manifest_*.json"))
    assert not list(package.glob("release_evidence_index_*.json"))
    assert "meshcore_conformance" not in manifest
    assert "meshcore_signed_advert_runtime" not in manifest
    assert manifest["debug_files"] == []
    assert not (package / "debug").exists()
    assert [row["path"] for row in manifest["notice_files"]] == [
        "notices/LICENSE",
        "notices/THIRD_PARTY_NOTICES.md",
        "notices/ORLP_ED25519_ZLIB_LICENSE.txt",
    ]
    assert not (package / "notices" / "ATTRIBUTIONS.md").exists()
    assert not (
        package / "notices" / "SOURCE_AUDIT_AND_ATTRIBUTION.md"
    ).exists()
    customer_paths = [
        path.relative_to(package).as_posix().lower()
        for path in package.rglob("*")
        if path.is_file()
    ]
    assert not [
        path
        for path in customer_paths
        if "test" in path or "smoke" in path or path.startswith("evidence/")
    ]
    public_json = "\n".join(
        path.read_text(encoding="utf-8")
        for path in package.rglob("*.json")
    ).lower()
    for internal_marker in (
        '"tests/',
        "pytest",
        "smoke",
        "completion_ledger",
        "source_audit",
        "release_evidence",
    ):
        assert internal_marker not in public_json
    assert [row["name"] for row in manifest["rp2040_artifacts"]] == [
        "rp2040-sd-bridge-firmware"
    ]
    bridge_package = package / "rp2040" / "rp2040-sd-bridge-firmware"
    assert sorted(
        path.name for path in bridge_package.iterdir() if path.is_file()
    ) == ["SHA256SUMS.txt", "deskos_sd_bridge.ino.uf2"]
    assert verify_checksum_tree(bridge_package) is True
    assert not (package / "rp2040" / "rp2040-sd-smoke-firmware").exists()
    assert not (
        package / "rp2040" / "rp2040-seeed-official-sd-smoke-firmware"
    ).exists()
    package_release_d1l.validate_production_package_surface(package)


def test_production_surface_rejects_internal_artifact_names(tmp_path):
    internal = tmp_path / "evidence" / "report.json"
    internal.parent.mkdir()
    internal.write_text("{}\n", encoding="ascii")

    with pytest.raises(ValueError, match="internal-only path"):
        package_release_d1l.validate_production_package_surface(tmp_path)


def test_production_surface_rejects_debug_binary_metadata(tmp_path):
    debug_file = tmp_path / "firmware" / "deskos.elf"
    debug_file.parent.mkdir()
    debug_file.write_bytes(b"ELF")

    with pytest.raises(ValueError, match="debug-only file"):
        package_release_d1l.validate_production_package_surface(tmp_path)


def test_production_surface_rejects_customer_test_instructions(tmp_path):
    guide = tmp_path / "START_HERE.md"
    guide.write_text("Run the tests after installation.\n", encoding="ascii")

    with pytest.raises(ValueError, match="internal qualification language"):
        package_release_d1l.validate_production_package_surface(tmp_path)


def test_public_release_stages_only_production_zip_and_outer_checksums():
    release_doc = (
        ROOT / "docs" / "RC1_RELEASE_EXECUTION_D1L.md"
    ).read_text(encoding="utf-8")
    assets = re.search(
        r"RELEASE_ASSETS=\(\n(?P<body>.*?)\n\)",
        release_doc,
        flags=re.DOTALL,
    )
    assert assets is not None
    assert re.findall(r'"\$(\w+)"', assets.group("body")) == [
        "PACKAGE_ASSET",
        "ASSET_SUMS",
    ]
    release_command = release_doc.split(
        "gh release create v1.0.0", 1
    )[1].split("test \"$(gh release view", 1)[0]
    assert "--generate-notes" not in release_command
    assert "START_HERE.md" in release_command
    for internal_marker in (
        "START_HERE_RC1",
        "read-only RC1",
        "core-actions-run",
        "physical evidence",
        "AUDIT_ASSET",
        "PHYSICAL_ASSET",
    ):
        assert internal_marker not in release_command


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
