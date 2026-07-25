import json
import shutil
from pathlib import Path

import pytest

from scripts import autonomous_hardware_validate_d1l as runner


COMMIT = "1600d649223d8f5cbf35cf587d44bd94c0f21293"


def test_cli_rejects_hardware_before_any_orchestration(
    monkeypatch, capsys
):
    monkeypatch.setattr(
        runner,
        "run_validation",
        lambda _args: pytest.fail(
            "disabled hardware CLI must not enter orchestration"
        ),
    )

    result = runner.main(
        [
            "--github-run-id",
            "123",
            "--commit",
            COMMIT,
        ]
    )

    output = json.loads(capsys.readouterr().out)
    assert result == 2
    assert output["classification"] == "hardware_execution_disabled"
    assert output["ok"] is False
    assert output["public_rf_tx"] is False
    assert output["formats_sd"] is False
    assert "narrow Core" in output["error"]


def patch_sd_evidence_runners(monkeypatch, ok_step):
    monkeypatch.setattr(runner, "run_sd_raw_diag", lambda ctx, dry_run: ok_step("sd_raw_diag"))
    monkeypatch.setattr(
        runner,
        "run_sd_boot_prepare_scenario",
        lambda ctx, scenario, dry_run: ok_step(f"sd_boot_prepare_{scenario.replace('-', '_')}"),
    )
    monkeypatch.setattr(runner, "run_sd_map_tile_canary", lambda ctx, dry_run: ok_step("sd_map_tile_canary"))
    monkeypatch.setattr(runner, "run_sd_export_canary", lambda ctx, dry_run: ok_step("sd_export_canary"))
    monkeypatch.setattr(runner, "run_sd_diagnostic_export", lambda ctx, dry_run: ok_step("sd_diagnostic_export"))
    monkeypatch.setattr(runner, "run_sd_data_export", lambda ctx, dry_run: ok_step("sd_data_export"))
    monkeypatch.setattr(runner, "run_sd_retained_history", lambda ctx, dry_run: ok_step("sd_retained_history"))
    monkeypatch.setattr(runner, "run_sd_reboot_remount", lambda ctx, dry_run: ok_step("sd_reboot_remount"))


def patch_diag_isolation_runners(monkeypatch, ok_step):
    monkeypatch.setattr(
        runner,
        "flash_esp32",
        lambda ctx, dry_run, phase="initial": {
            **ok_step("esp32_flash"),
            "phase": phase,
        },
    )
    monkeypatch.setattr(
        runner,
        "restore_bridge",
        lambda ctx, volume, uf2_timeout, dry_run, phase="initial": {
            **ok_step("rp2040_bridge_restore"),
            "phase": phase,
        },
    )
    monkeypatch.setattr(
        runner,
        "run_preflight",
        lambda ctx, dry_run, verify_artifact, phase="initial": {
            **ok_step("rp2040_preflight"),
            "phase": phase,
            "path": str(runner.preflight_out(ctx, phase)),
        },
    )
    monkeypatch.setattr(
        runner,
        "run_clean_preflight_gate",
        lambda ctx, preflight, dry_run, phase="post_diag": {
            **ok_step(f"{phase}_clean_preflight_gate"),
            "phase": phase,
        },
    )


def clean_preflight_payload() -> dict:
    clean_store = {
        "sd_read_fail_count": 0,
        "sd_write_fail_count": 0,
        "sd_rename_fail_count": 0,
        "sd_last_error": "ESP_OK",
        "sd_degraded_latched": False,
        "nvs_mirror_fail_count": 0,
        "nvs_mirror_last_error": "ESP_OK",
    }
    return {
        "ok": True,
        "ready_for_sd_acceptance": True,
        "port": "COM12",
        "commit": COMMIT,
        "storage_status": {
            "ok": True,
            "manager": {"running": True, "state": "READY_SD"},
            "sd": {
                "state": "ready",
                "present": True,
                "presence_stale": False,
                "mounted": True,
                "data_root_ready": True,
                "file_ops": True,
                "status_stale": False,
                "refresh_failures": 0,
                "last_error": "ESP_OK",
            },
            "data_backend": "mixed",
            "message_store_backend": "sd",
            "dm_store_backend": "sd",
            "route_store_backend": "sd",
            "packet_log_backend": "sd",
            "retained_sd": {
                "degraded": False,
                "backup_degraded": False,
                "stores": {
                    name: dict(clean_store) for name in runner.RETAINED_STORE_NAMES
                },
            },
        },
    }


def write_flasher_args(build: Path) -> None:
    (build / "bootloader").mkdir(parents=True, exist_ok=True)
    (build / "partition_table").mkdir(parents=True, exist_ok=True)
    (build / "bootloader" / "bootloader.bin").write_bytes(b"boot")
    (build / "partition_table" / "partition-table.bin").write_bytes(b"part")
    (build / "meshcore_deskos_d1l.bin").write_bytes(b"app")
    (build / "flasher_args.json").write_text(
        json.dumps(
            {
                "write_flash_args": ["--flash_mode", "dio", "--flash_size", "8MB", "--flash_freq", "80m"],
                "flash_files": {
                    "0x10000": "meshcore_deskos_d1l.bin",
                    "0x0": "bootloader/bootloader.bin",
                    "0x8000": "partition_table/partition-table.bin",
                },
                "extra_esptool_args": {
                    "after": "hard_reset",
                    "before": "default_reset",
                    "chip": "esp32s3",
                },
            }
        ),
        encoding="ascii",
    )
    artifact_root = build.parent
    rows = []
    for path in sorted(
        item
        for item in artifact_root.rglob("*")
        if item.is_file() and item.name != "SHA256SUMS.txt"
    ):
        relative = path.relative_to(artifact_root).as_posix()
        rows.append(f"{runner.sha256_file(path)}  {relative}")
    (artifact_root / "SHA256SUMS.txt").write_text("\n".join(rows) + "\n", encoding="ascii")


def test_console_summary_is_bounded_and_points_to_full_report(tmp_path):
    out = tmp_path / "full-report.json"
    report = {
        "schema": 1,
        "kind": "d1l_autonomous_hardware_validation",
        "ok": False,
        "mode": "hardware",
        "commit": COMMIT,
        "github_actions_run": "29234278626",
        "error": "sd_reboot_remount_failed",
        "recovery": {"ok": True, "details": "x" * 100_000},
        "ready_for_public_release": False,
        "runs": [{"output": "large-step-output" * 10_000}],
    }

    summary = runner.console_summary(report, out)
    encoded = json.dumps(summary)

    assert len(encoded) < 1024
    assert "large-step-output" not in encoded
    assert summary == {
        "schema": 1,
        "kind": "d1l_autonomous_hardware_validation_summary",
        "ok": False,
        "mode": "hardware",
        "commit": COMMIT,
        "github_actions_run": "29234278626",
        "completed_steps": 1,
        "report": str(out),
        "error": "sd_reboot_remount_failed",
        "recovery_ok": True,
        "ready_for_public_release": False,
    }


def test_console_summary_bounds_untrusted_text_and_missing_runs():
    summary = runner.console_summary(
        {
            "ok": False,
            "mode": "m" * 5000,
            "commit": "c" * 5000,
            "github_actions_run": "r" * 5000,
            "error": "e" * 5000,
            "runs": None,
        },
        Path("p" * 5000),
    )

    assert len(json.dumps(summary)) < 1024
    assert summary["completed_steps"] == 0
    assert summary["mode"].endswith("...")
    assert summary["commit"].endswith("...")
    assert summary["github_actions_run"].endswith("...")
    assert summary["error"].endswith("...")
    assert summary["report"].endswith("...")


def test_validation_runs_ok_accepts_only_recovered_bridge_retry_chain():
    runs = [
        {"kind": "input_artifact_check", "ok": True},
        {
            "kind": "rp2040_bridge_restore",
            "phase": "post_diag",
            "attempt": 1,
            "ok": False,
        },
        {
            "kind": "rp2040_bridge_restore",
            "phase": "post_diag",
            "attempt": 2,
            "ok": True,
        },
        {"kind": "d1l_smoke", "ok": True},
        {"kind": "release_gate_audit", "ok": True},
    ]

    assert runner.validation_runs_ok(runs) is True


@pytest.mark.parametrize(
    "bridge_attempts",
    [
        [{"attempt": 1, "ok": False}],
        [{"attempt": 1, "ok": False}, {"attempt": 2, "ok": False}],
        [{"attempt": 2, "ok": True}],
        [{"attempt": 1, "ok": True}, {"attempt": 2, "ok": True}],
        [{"attempt": 1, "ok": False}, {"attempt": 3, "ok": True}],
    ],
)
def test_validation_runs_ok_rejects_exhausted_or_malformed_bridge_retries(
    bridge_attempts,
):
    runs = [
        {
            "kind": "rp2040_bridge_restore",
            "phase": "post_diag",
            **attempt,
        }
        for attempt in bridge_attempts
    ]

    assert runner.validation_runs_ok(runs) is False


def test_validation_runs_ok_rejects_noncontiguous_or_mismatched_retry():
    attempt_one = {
        "kind": "rp2040_bridge_restore",
        "phase": "post_diag",
        "attempt": 1,
        "ok": False,
    }
    attempt_two = {
        "kind": "rp2040_bridge_restore",
        "phase": "post_diag",
        "attempt": 2,
        "ok": True,
    }

    assert runner.validation_runs_ok(
        [attempt_one, {"kind": "d1l_smoke", "ok": True}, attempt_two]
    ) is False
    assert runner.validation_runs_ok(
        [attempt_one, {**attempt_two, "phase": "pre_diag"}]
    ) is False


def test_validation_runs_ok_does_not_hide_unrelated_failure():
    runs = [
        {
            "kind": "rp2040_bridge_restore",
            "phase": "post_diag",
            "attempt": 1,
            "ok": False,
        },
        {
            "kind": "rp2040_bridge_restore",
            "phase": "post_diag",
            "attempt": 2,
            "ok": True,
        },
        {"kind": "sd_reboot_remount", "ok": False},
    ]

    assert runner.validation_runs_ok(runs) is False


def write_actions_provenance_fixture(
    run_dir: Path,
    *,
    commit: str = COMMIT,
    run_id: str = "28663994079",
    include_official: bool = False,
) -> Path:
    build = run_dir / "d1l-firmware-artifacts" / "build"
    write_flasher_args(build)
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
                "workflow_run_id": run_id,
                "workflow_run_attempt": "1",
            }
        ),
        encoding="utf-8",
    )

    package = run_dir / "d1l-release-package" / f"d1l-release-{commit}"
    flash_files = []
    flash_roles = (
        ("bootloader", "bootloader/bootloader.bin", "firmware/bootloader.bin", "0x0"),
        ("partition-table", "partition_table/partition-table.bin", "firmware/partition-table.bin", "0x8000"),
        ("app", "meshcore_deskos_d1l.bin", "firmware/meshcore_deskos_d1l.bin", "0x10000"),
    )
    for role, source_name, package_name, offset in flash_roles:
        source = build / source_name
        packaged = package / package_name
        packaged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, packaged)
        flash_files.append(
            {
                "role": role,
                "source": source_name,
                "path": package_name,
                "offset": offset,
                "size": source.stat().st_size,
                "sha256": runner.sha256_file(source),
            }
        )

    group_specs = [("rp2040-sd-bridge-firmware", "deskos_sd_bridge.ino.uf2")]
    if include_official:
        group_specs.append(
            (
                "rp2040-seeed-official-sd-smoke-firmware",
                "seeed_official_sd_smoke.ino.uf2",
            )
        )
    groups = []
    for group_name, uf2_name in group_specs:
        direct = run_dir / group_name
        direct.mkdir(parents=True, exist_ok=True)
        (direct / uf2_name).write_bytes(f"{group_name}-uf2".encode("ascii"))
        (direct / "SHA256SUMS.txt").write_text(
            f"{runner.sha256_file(direct / uf2_name)}  {uf2_name}\n",
            encoding="ascii",
        )
        package_prefix = f"rp2040/{group_name}"
        files = []
        for source in sorted(path for path in direct.iterdir() if path.is_file()):
            packaged = package / package_prefix / source.name
            packaged.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, packaged)
            files.append(
                {
                    "path": f"{package_prefix}/{source.name}",
                    "size": source.stat().st_size,
                    "sha256": runner.sha256_file(source),
                }
            )
        groups.append(
            {
                "name": group_name,
                "path": package_prefix,
                "files": files,
            }
        )

    package.mkdir(parents=True, exist_ok=True)
    manifest_path = package / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema": 1,
                "package": f"d1l-release-{commit}",
                "git": {"commit": commit, "dirty": False},
                "workflow": {
                    "run_id": run_id,
                    "run_attempt": "1",
                    "sha": commit,
                    "repository": "n30nex/SIGUI",
                },
                "flash_files": flash_files,
                "rp2040_artifacts": groups,
            }
        ),
        encoding="utf-8",
    )
    return manifest_path


def test_port_guard_blocks_forbidden_ports():
    with pytest.raises(ValueError):
        runner.enforce_port_guard("COM12", "COM29")
    with pytest.raises(ValueError):
        runner.enforce_port_guard("com11")
    with pytest.raises(ValueError):
        runner.enforce_port_guard("COM8")


def test_esptool_command_uses_actions_build_files(tmp_path):
    build = tmp_path / "build"
    write_flasher_args(build)

    command = runner.esptool_flash_command(build, "COM12", 460800)

    assert "--chip" in command
    assert "esp32s3" in command
    assert "-p" in command
    assert "COM12" in command
    assert "--flash-mode" in command
    assert "--flash_size" not in command
    assert command.index("0x0") < command.index("0x8000") < command.index("0x10000")
    assert str((build / "meshcore_deskos_d1l.bin").resolve()) in command


def test_flash_esp32_waits_after_successful_hard_reset(tmp_path, monkeypatch):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    write_flasher_args(run_dir / "d1l-firmware-artifacts" / "build")
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=run_dir,
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    sleeps = []

    monkeypatch.setattr(runner, "command_report", lambda *args, **kwargs: {"ok": True})
    monkeypatch.setattr(runner.time, "sleep", lambda seconds: sleeps.append(seconds))

    report = runner.flash_esp32(ctx, dry_run=False)

    assert report["ok"] is True
    assert report["artifact_verification"]["ok"] is True
    assert report["artifact_verification"]["manifest_complete"] is True
    assert len(report["artifact_verification"]["flash_files"]) == 3
    assert report["flashed_at"]
    assert report["post_flash_settle_sec"] == runner.POST_ESP32_FLASH_SETTLE_SEC
    assert sleeps == [runner.POST_ESP32_FLASH_SETTLE_SEC]


def test_flash_esp32_fails_closed_on_incomplete_or_tampered_actions_manifest(
    tmp_path, monkeypatch
):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    build = run_dir / "d1l-firmware-artifacts" / "build"
    write_flasher_args(build)
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=run_dir,
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    calls = []
    monkeypatch.setattr(runner, "command_report", lambda *args, **kwargs: calls.append(args))

    manifest = run_dir / "d1l-firmware-artifacts" / "SHA256SUMS.txt"
    manifest.write_text("", encoding="ascii")
    empty = runner.flash_esp32(ctx, dry_run=False)

    assert empty["ok"] is False
    assert empty["artifact_verification"]["ok"] is False
    assert calls == []

    write_flasher_args(build)
    (build / "meshcore_deskos_d1l.bin").write_bytes(b"tampered")
    tampered = runner.flash_esp32(ctx, dry_run=False)

    assert tampered["ok"] is False
    assert "checksum mismatch" in tampered["artifact_verification"]["error"]
    assert calls == []


def test_flash_esp32_rejects_checksummed_extra_flash_role(tmp_path):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    artifact_root = run_dir / "d1l-firmware-artifacts"
    build = artifact_root / "build"
    write_flasher_args(build)
    extra = build / "unexpected-nvs.bin"
    extra.write_bytes(b"must not be flashed")
    flasher_path = build / "flasher_args.json"
    flasher = json.loads(flasher_path.read_text(encoding="utf-8"))
    flasher["flash_files"]["0x9000"] = extra.name
    flasher_path.write_text(json.dumps(flasher), encoding="ascii")
    manifest = artifact_root / "SHA256SUMS.txt"
    rows = []
    for path in sorted(
        item
        for item in artifact_root.rglob("*")
        if item.is_file() and item != manifest
    ):
        rows.append(
            f"{runner.sha256_file(path)}  {path.relative_to(artifact_root).as_posix()}"
        )
    manifest.write_text("\n".join(rows) + "\n", encoding="ascii")
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=run_dir,
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )

    with pytest.raises(runner.FlashGuardError, match="must exactly match"):
        runner.verify_esp32_flash_inputs(ctx)


def test_release_gate_command_omits_unconfigured_peer_port(tmp_path):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )

    command = runner.release_gate_command(ctx, tmp_path / "gate.json")

    assert "--mesh-peer-port" not in command
    assert "--meshbot-port" not in command
    assert "COM8" not in command
    assert "COM11" not in command
    assert "COM29" not in command


def test_release_gate_uses_distinct_phase_receipts_and_canonical_final_path(tmp_path):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )

    before = runner.run_release_gate(
        ctx, dry_run=True, phase="after_sd_file_canary_failed"
    )
    after = runner.run_release_gate(
        ctx,
        dry_run=True,
        phase="after_sd_file_canary_failed_post_recovery",
    )
    final = runner.run_release_gate(ctx, dry_run=True)

    assert before["path"] != after["path"]
    assert before["phase"] == "after_sd_file_canary_failed"
    assert after["phase"] == "after_sd_file_canary_failed_post_recovery"
    assert final["path"].endswith("-autonomous-hw.json")
    assert "phase" not in final
    for report in (before, after, final):
        assert report["command"][report["command"].index("--out") + 1] == report["path"]


def test_autonomous_smoke_binds_device_version_to_selected_commit(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    captured = {}

    def fake_run(_ctx, kind, args, out, timeout, dry_run):
        captured["kind"] = kind
        captured["args"] = args
        return {"ok": True}

    monkeypatch.setattr(runner, "run_existing_script", fake_run)

    report = runner.run_smoke(ctx, dry_run=True)

    assert report["ok"] is True
    assert captured["kind"] == "smoke"
    index = captured["args"].index("--expected-firmware-commit")
    assert captured["args"][index + 1] == COMMIT


def test_autonomous_reboot_remount_binds_exact_device_commit(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    captured = {}

    def fake_run(_ctx, kind, args, out, timeout, dry_run):
        captured.update(
            {"kind": kind, "args": args, "timeout": timeout, "dry_run": dry_run}
        )
        return {"ok": True}

    monkeypatch.setattr(runner, "run_existing_script", fake_run)

    report = runner.run_sd_reboot_remount(ctx, dry_run=True)

    assert report["ok"] is True
    assert captured["kind"] == "sd_reboot_remount"
    index = captured["args"].index("--expected-firmware-commit")
    assert captured["args"][index + 1] == COMMIT
    assert captured["timeout"] == 300


def test_compose_keyboard_capture_command_uses_com12_targets_and_artifact_path(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    captured = {}

    def fake_run_existing_script(ctx_arg, kind, args, out, timeout, dry_run):
        captured.update({"kind": kind, "args": args, "out": out, "timeout": timeout, "dry_run": dry_run})
        return {"schema": 1, "kind": kind, "ok": True}

    monkeypatch.setattr(runner, "run_existing_script", fake_run_existing_script)

    report = runner.run_ui_compose_keyboard_capture(ctx, dry_run=False)

    assert report["ok"] is True
    assert captured["kind"] == "ui_compose_keyboard_capture"
    assert captured["args"][captured["args"].index("--port") + 1] == "COM12"
    assert captured["args"][captured["args"].index("--targets") + 1] == "all"
    assert "COM8" not in json.dumps(captured["args"])
    assert "COM11" not in json.dumps(captured["args"])
    assert "COM29" not in json.dumps(captured["args"])
    assert captured["out"].name == "ui_compose_keyboard_capture_1600d64_actions_28663994079_COM12.json"
    assert captured["timeout"] == 900


def test_pixel_capture_generates_home_reference_and_diff_gate(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    captured = {}

    def fake_command_report(name, command, cwd, timeout=None):
        assert name == "ui_simulator_reference"
        out_dir = Path(command[command.index("--out") + 1])
        out_dir.mkdir(parents=True)
        (out_dir / "home.png").write_bytes(b"png")
        (out_dir / "ui-sim-report.json").write_text("{}", encoding="ascii")
        return {"ok": True}

    def fake_run_existing_script(ctx_arg, kind, args, out, timeout, dry_run):
        captured.update({"kind": kind, "args": args, "out": out, "timeout": timeout, "dry_run": dry_run})
        return {"schema": 1, "kind": kind, "ok": True}

    monkeypatch.setattr(runner, "command_report", fake_command_report)
    monkeypatch.setattr(runner, "run_existing_script", fake_run_existing_script)

    report = runner.run_ui_pixel_capture(ctx, dry_run=False)

    assert report["ok"] is True
    assert report["reference"]["kind"] == "ui_simulator_reference"
    assert report["reference"]["view"] == "home"
    assert captured["kind"] == "ui_pixel_capture"
    assert captured["args"][captured["args"].index("--port") + 1] == "COM12"
    assert captured["args"][captured["args"].index("--prep-command") + 1] == "ui tab home"
    assert captured["args"][captured["args"].index("--reference-view") + 1] == "home"
    assert captured["args"][captured["args"].index("--reference-png") + 1].endswith("home.png")
    assert captured["args"][captured["args"].index("--diff-out") + 1].endswith("_simdiff.json")
    assert "COM8" not in json.dumps(captured["args"])
    assert "COM11" not in json.dumps(captured["args"])
    assert "COM29" not in json.dumps(captured["args"])


def test_smoke_retries_once_after_post_flash_console_timeout(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    attempts = []
    sleeps = []

    def fake_run_existing_script(ctx_arg, kind, args, out, timeout, dry_run):
        attempts.append(out)
        out.parent.mkdir(parents=True, exist_ok=True)
        ok = len(attempts) == 2
        out.write_text(json.dumps({"schema": 1, "kind": kind, "ok": ok}), encoding="ascii")
        return {"schema": 1, "kind": kind, "ok": ok, "path": str(out)}

    monkeypatch.setattr(runner, "run_existing_script", fake_run_existing_script)
    monkeypatch.setattr(runner.time, "sleep", lambda seconds: sleeps.append(seconds))

    report = runner.run_smoke(ctx, dry_run=False)

    assert report["ok"] is True
    assert report["retry_after_failure"] is True
    assert report["smoke_retry_settle_sec"] == runner.SMOKE_RETRY_SETTLE_SEC
    assert sleeps == [runner.SMOKE_RETRY_SETTLE_SEC]
    assert len(attempts) == 2
    assert Path(report["first_attempt_path"]).read_text(encoding="ascii")


def test_sd_file_canary_uses_post_restore_timing_window(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    captured = {}

    def fake_run_existing_script(ctx_arg, kind, args, out, timeout, dry_run):
        captured.update({"kind": kind, "args": args, "out": out, "timeout": timeout, "dry_run": dry_run})
        return {"schema": 1, "kind": kind, "ok": True}

    monkeypatch.setattr(runner, "run_existing_script", fake_run_existing_script)

    report = runner.run_sd_file_canary(ctx, dry_run=False)

    assert report["ok"] is True
    assert captured["kind"] == "sd_file_canary"
    assert captured["args"][captured["args"].index("--port") + 1] == "COM12"
    assert captured["args"][captured["args"].index("--timeout") + 1] == "45"
    assert captured["args"][captured["args"].index("--mount-wait-sec") + 1] == "60"
    assert captured["out"].name == "sd_file_canary_1600d64_actions_28663994079_COM12.json"
    assert captured["timeout"] == 120


def test_dry_run_plan_is_noninteractive_and_port_safe(tmp_path):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--dry-run",
            "--skip-rp2040-official-smoke",
        ]
    )

    ctx = runner.build_context(args)
    plan = runner.plan_report(ctx, args)

    assert plan["manual_user_required"] is False
    assert plan["ports"]["d1l"] == "COM12"
    assert plan["ports"]["rp2040"] == "COM16"
    assert "COM8" not in json.dumps(plan["steps"])
    assert "COM11" not in json.dumps(plan["steps"])
    assert "COM29" not in json.dumps(plan["steps"])
    assert plan["rp2040_uf2_flash"] is True
    assert plan["rp2040_flash_mode"] == "exact_bridge_pre_and_post_diag"
    assert "rp2040_autonomous_access_precheck" in plan["steps"]
    assert "flash_official_rp2040_sd_smoke" not in plan["steps"]
    assert "restore_exact_rp2040_bridge_pre_diag" in plan["steps"]
    assert "reflash_exact_esp32_pre_diag" not in plan["steps"]
    assert "restore_exact_rp2040_bridge_post_diag" in plan["steps"]
    assert "reflash_exact_esp32_post_diag" in plan["steps"]
    assert "pre_diag_clean_preflight_gate" in plan["steps"]
    assert "post_diag_clean_preflight_gate" in plan["steps"]
    assert "sd_raw_diag" in plan["steps"]
    assert "sd_boot_prepare_correct_structure" in plan["steps"]
    assert "sd_boot_prepare_missing_structure" in plan["steps"]
    assert "sd_boot_prepare_existing_data" in plan["steps"]
    assert "sd_export_canary" in plan["steps"]
    assert "sd_diagnostic_export" in plan["steps"]
    assert "sd_data_export" in plan["steps"]
    assert "d1l_500_cycle_tab_abuse" not in plan["steps"]
    assert "d1l_ui_corruption_probe" not in plan["steps"]
    assert "d1l_scroll_probe" not in plan["steps"]
    assert "d1l_ui_pixel_capture" not in plan["steps"]
    assert "d1l_ui_compose_keyboard_capture" not in plan["steps"]


def test_refresh_plan_reflashes_exact_esp32_after_bridge_restore(tmp_path):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--dry-run",
            "--refresh-rp2040-smoke",
        ]
    )

    plan = runner.plan_report(runner.build_context(args), args)
    restore_index = plan["steps"].index("restore_exact_rp2040_bridge_pre_diag")
    reflash_index = plan["steps"].index("reflash_exact_esp32_pre_diag")
    preflight_index = plan["steps"].index("rp2040_bridge_preflight_pre_diag")

    assert restore_index < reflash_index < preflight_index


def test_refresh_reflashes_esp32_before_pre_diag_preflight(tmp_path, monkeypatch):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--refresh-rp2040-smoke",
        ]
    )
    events = []

    def ok_step(kind: str) -> dict:
        return {"schema": 1, "kind": kind, "ok": True}

    monkeypatch.setattr(
        runner,
        "verify_inputs",
        lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"),
    )

    def flash(ctx, dry_run, phase="initial"):
        events.append(f"flash:{phase}")
        return {**ok_step("esp32_flash"), "phase": phase}

    def restore(ctx, volume, uf2_timeout, dry_run, phase="initial"):
        events.append(f"restore:{phase}")
        return {**ok_step("rp2040_bridge_restore"), "phase": phase}

    def preflight(ctx, dry_run, verify_artifact, phase="initial"):
        events.append(f"preflight:{phase}")
        return {**ok_step("rp2040_preflight"), "phase": phase}

    monkeypatch.setattr(runner, "flash_esp32", flash)
    monkeypatch.setattr(
        runner,
        "rp2040_access_precheck",
        lambda ctx, dry_run: ok_step("rp2040_autonomous_access_precheck"),
    )
    monkeypatch.setattr(
        runner,
        "run_official_sd_smoke",
        lambda *args, **kwargs: events.append("official_smoke")
        or ok_step("seeed_official_sd_smoke_capture"),
    )
    monkeypatch.setattr(
        runner,
        "run_sd_boot_prepare_scenario",
        lambda ctx, scenario, dry_run: events.append(f"scenario:{scenario}")
        or ok_step(f"sd_boot_prepare_{scenario.replace('-', '_')}"),
    )
    monkeypatch.setattr(runner, "restore_bridge", restore)
    monkeypatch.setattr(runner, "run_preflight", preflight)
    monkeypatch.setattr(
        runner,
        "run_clean_preflight_gate",
        lambda ctx, preflight, dry_run, phase="post_diag": {
            **ok_step(f"{phase}_clean_preflight_gate"),
            "ok": False,
            "phase": phase,
            "error": "test_stop_after_preflight",
        },
    )
    monkeypatch.setattr(
        runner,
        "run_release_gate",
        lambda ctx, dry_run, phase=None: {
            **ok_step("release_gate_audit"),
            "ready_for_public_release": False,
        },
    )

    report = runner.run_validation(args)

    assert report["ok"] is False
    assert events == [
        "flash:initial",
        "official_smoke",
        "scenario:rp2040-unavailable",
        "restore:pre_diag",
        "flash:pre_diag",
        "preflight:pre_diag",
    ]


def test_refresh_stops_before_preflight_when_pre_diag_esp32_reflash_fails(
    tmp_path, monkeypatch
):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--refresh-rp2040-smoke",
        ]
    )
    flash_phases = []

    def ok_step(kind: str) -> dict:
        return {"schema": 1, "kind": kind, "ok": True}

    def flash(ctx, dry_run, phase="initial"):
        flash_phases.append(phase)
        return {
            **ok_step("esp32_flash"),
            "phase": phase,
            "ok": phase == "initial",
            "error": None if phase == "initial" else "checksum_bound_flash_failed",
        }

    monkeypatch.setattr(
        runner,
        "verify_inputs",
        lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"),
    )
    monkeypatch.setattr(runner, "flash_esp32", flash)
    monkeypatch.setattr(
        runner,
        "rp2040_access_precheck",
        lambda ctx, dry_run: ok_step("rp2040_autonomous_access_precheck"),
    )
    monkeypatch.setattr(
        runner,
        "run_official_sd_smoke",
        lambda *args, **kwargs: ok_step("seeed_official_sd_smoke_capture"),
    )
    monkeypatch.setattr(
        runner,
        "run_sd_boot_prepare_scenario",
        lambda ctx, scenario, dry_run: ok_step(
            f"sd_boot_prepare_{scenario.replace('-', '_')}"
        ),
    )
    monkeypatch.setattr(
        runner,
        "restore_bridge",
        lambda ctx, volume, uf2_timeout, dry_run, phase="initial": {
            **ok_step("rp2040_bridge_restore"),
            "phase": phase,
        },
    )
    monkeypatch.setattr(
        runner,
        "run_preflight",
        lambda *args, **kwargs: (_ for _ in ()).throw(
            AssertionError("preflight must not run after failed ESP32 reflash")
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_release_gate",
        lambda ctx, dry_run, phase=None: {
            **ok_step("release_gate_audit"),
            "ready_for_public_release": False,
        },
    )

    report = runner.run_validation(args)

    assert report["ok"] is False
    assert report["error"] == "pre_diag_esp32_reflash_failed"
    assert flash_phases == ["initial", "pre_diag"]
    assert [step["phase"] for step in report["runs"] if step["kind"] == "esp32_flash"] == [
        "initial",
        "pre_diag",
    ]


def test_dry_run_plan_includes_pixel_capture_with_ui_probes(tmp_path):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--dry-run",
            "--skip-rp2040-official-smoke",
            "--include-ui-probes",
        ]
    )

    ctx = runner.build_context(args)
    plan = runner.plan_report(ctx, args)

    assert "d1l_onboarding_complete_for_ui_probes" in plan["steps"]
    assert "d1l_ui_corruption_probe" in plan["steps"]
    assert "d1l_scroll_probe" in plan["steps"]
    assert "d1l_ui_pixel_capture" in plan["steps"]
    assert "d1l_ui_compose_keyboard_capture" in plan["steps"]
    assert "COM8" not in json.dumps(plan["steps"])
    assert "COM11" not in json.dumps(plan["steps"])
    assert "COM29" not in json.dumps(plan["steps"])


def test_dry_run_plan_can_focus_esp32_ui_without_sd_or_rp2040_flash(tmp_path):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--dry-run",
            "--skip-sd-suite",
            "--include-ui-probes",
        ]
    )

    ctx = runner.build_context(args)
    plan = runner.plan_report(ctx, args)

    assert plan["rp2040_uf2_flash"] is False
    assert plan["sd_suite_enabled"] is False
    assert "flash_esp32" in plan["steps"]
    assert "rp2040_autonomous_access_precheck" not in plan["steps"]
    assert "restore_rp2040_bridge" not in plan["steps"]
    assert "rp2040_bridge_preflight" not in plan["steps"]
    assert "sd_file_canary" not in plan["steps"]
    assert "d1l_onboarding_complete_for_ui_probes" in plan["steps"]
    assert "d1l_ui_corruption_probe" in plan["steps"]
    assert "d1l_ui_pixel_capture" in plan["steps"]


def test_skip_esp32_flash_is_rejected_for_sd_suite():
    with pytest.raises(SystemExit):
        runner.parse_args(
            [
                "--commit",
                COMMIT,
                "--github-run-id",
                "28663994079",
                "--skip-esp32-flash",
            ]
        )


@pytest.mark.parametrize("phase", ["pre_diag", "post_diag"])
def test_clean_preflight_gate_requires_zero_retained_failures(tmp_path, phase):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    preflight_path = runner.preflight_out(ctx, phase)
    preflight_path.parent.mkdir(parents=True, exist_ok=True)
    preflight = {
        "schema": 1,
        "kind": "rp2040_preflight",
        "phase": phase,
        "path": str(preflight_path),
        "ok": True,
    }

    preflight_path.write_text(json.dumps(clean_preflight_payload()), encoding="utf-8")
    clean = runner.run_clean_preflight_gate(
        ctx, preflight, dry_run=False, phase=phase
    )

    assert clean["ok"] is True
    assert clean["kind"] == f"{phase}_clean_preflight_gate"
    assert clean["phase"] == phase
    assert clean["validation"]["manager_state"] == "READY_SD"
    assert clean["validation"]["violations"] == []

    contaminated = clean_preflight_payload()
    contaminated["storage_status"]["retained_sd"]["degraded"] = True
    contaminated["storage_status"]["retained_sd"]["stores"]["packets"]["sd_read_fail_count"] = 1
    contaminated["storage_status"]["retained_sd"]["stores"]["packets"]["sd_degraded_latched"] = True
    preflight_path.write_text(json.dumps(contaminated), encoding="utf-8")
    failed = runner.run_clean_preflight_gate(
        ctx, preflight, dry_run=False, phase=phase
    )

    assert failed["ok"] is False
    assert failed["error"] == f"{phase}_preflight_not_clean"
    assert any("packets.sd_read_fail_count" in item for item in failed["validation"]["violations"])
    assert any("packets.sd_degraded_latched" in item for item in failed["validation"]["violations"])


def test_rp2040_port_discovery_reports_alternatives_but_never_selects_them(monkeypatch):
    monkeypatch.setattr(runner, "rp2040_port_snapshot", lambda port: {"port": port, "present": False, "matches": []})
    monkeypatch.setattr(
        runner,
        "serial_port_inventory",
        lambda: [
            {"device": "COM8", "description": "Raspberry Pi Pico", "vid": "2E8A", "pid": "000A"},
            {"device": "COM12", "description": "USB-SERIAL CH340", "vid": "1A86", "pid": "7523"},
            {"device": "COM29", "description": "Adafruit Feather RP2040", "vid": "239A", "pid": "8029"},
            {"device": "COM17", "description": "Raspberry Pi Pico", "vid": "2E8A", "pid": "000A"},
        ],
    )

    report = runner.rp2040_port_discovery("COM16", "COM12")

    assert report["present"] is False
    assert report["selected_port"] is None
    assert report["alternatives_read_only"] is True
    assert report["alternative_ports"] == ["COM17"]
    assert report["candidates"][0]["match_reasons"] == ["keyword:pico", "keyword:raspberry pi pico", "vid:2E8A"]
    assert {"device": "COM8", "reason": "forbidden_port"} in report["skipped"]
    assert {"device": "COM12", "reason": "d1l_console_port"} in report["skipped"]
    assert {"device": "COM29", "reason": "forbidden_port"} in report["skipped"]


def test_rp2040_access_precheck_fails_fast_without_volume_port_or_ping(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    commands = []

    monkeypatch.setattr(
        runner,
        "uf2_volume_snapshot",
        lambda: {"available": False, "candidates": []},
    )
    monkeypatch.setattr(runner, "rp2040_port_snapshot", lambda port: {"port": port, "present": False, "matches": []})
    monkeypatch.setattr(
        runner,
        "rp2040_port_discovery",
        lambda port, d1l_port: {"preferred_port": port, "d1l_port": d1l_port, "present": False, "selected_port": None, "candidates": [], "skipped": []},
    )
    monkeypatch.setattr(runner.time, "sleep", lambda _seconds: None)

    def fake_console(port, baud, command, timeout, *, settle_sec=1.0):
        commands.append(command)
        if command == "rp2040 status":
            return {"ok": True, "cmd": command, "uart_ready": True}
        if command.startswith("rp2040 double-reset") or command == "rp2040 reset":
            return {"ok": True, "cmd": command}
        return {"ok": False, "cmd": command, "code": "ESP_ERR_TIMEOUT", "protocol_supported": False}

    monkeypatch.setattr(runner, "send_d1l_console", fake_console)

    report = runner.rp2040_access_precheck(ctx, dry_run=False)
    payload = json.loads(runner.rp2040_access_precheck_out(ctx).read_text(encoding="ascii"))

    assert report["ok"] is False
    assert report["state"] == "no_autonomous_bootloader_path"
    assert report["error"] == "rp2040_bootloader_unavailable"
    assert report["manual_user_required"] is False
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert commands == [
        "rp2040 status",
        "rp2040 ping",
        "rp2040 baud-probe 700",
        "rp2040 double-reset 50 150 1500",
        "rp2040 double-reset 30 300 1500",
        "rp2040 double-reset 100 500 2000",
        "rp2040 reset",
        "rp2040 ping",
        "rp2040 baud-probe 700",
    ]
    assert payload["state"] == "no_autonomous_bootloader_path"


def test_rp2040_access_precheck_accepts_no_usb_bridge_protocol(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    absent = {
        "preferred_port": "COM16",
        "d1l_port": "COM12",
        "present": False,
        "selected_port": None,
        "candidates": [],
        "skipped": [],
    }
    commands = []
    monkeypatch.setattr(
        runner, "uf2_volume_snapshot", lambda: {"available": False, "candidates": []}
    )
    monkeypatch.setattr(
        runner, "rp2040_port_discovery", lambda port, d1l_port: dict(absent)
    )
    monkeypatch.setattr(
        runner,
        "rp2040_double_reset_sweep",
        lambda *args, **kwargs: (_ for _ in ()).throw(
            AssertionError("double-reset fallback should not run")
        ),
    )
    monkeypatch.setattr(
        runner,
        "enter_rp2040_bootloader_usb_touch",
        lambda port: (_ for _ in ()).throw(
            AssertionError("USB touch should not run")
        ),
    )

    def fake_console(port, baud, command, timeout, *, settle_sec=1.0):
        commands.append(command)
        if command == "rp2040 ping":
            return {"ok": True, "cmd": command, "protocol_supported": True}
        return {"ok": True, "cmd": command}

    monkeypatch.setattr(runner, "send_d1l_console", fake_console)

    report = runner.rp2040_access_precheck(ctx, dry_run=False)

    assert report["ok"] is True
    assert report["state"] == "bridge_protocol_ready"
    assert report["bootloader_entry"] == "rp2040 bootloader"
    assert report["rp2040_port_initial"]["present"] is False
    assert commands == ["rp2040 status", "rp2040 ping"]


def test_rp2040_access_precheck_switches_to_baud_probe_match(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    commands = []

    monkeypatch.setattr(runner, "uf2_volume_snapshot", lambda: {"available": False, "candidates": []})
    monkeypatch.setattr(
        runner,
        "rp2040_port_discovery",
        lambda port, d1l_port: {"preferred_port": port, "d1l_port": d1l_port, "present": False, "selected_port": None, "candidates": [], "skipped": []},
    )

    def fake_console(port, baud, command, timeout, *, settle_sec=1.0):
        commands.append(command)
        if command == "rp2040 status":
            return {"ok": True, "cmd": command, "uart_ready": True}
        if command == "rp2040 ping":
            return {"ok": False, "cmd": command, "code": "ESP_ERR_TIMEOUT", "protocol_supported": False}
        if command == "rp2040 baud-probe 700":
            return {"ok": True, "cmd": command, "found_deskos": True, "selected_baud": 115200}
        if command == "rp2040 set-baud 115200":
            return {"ok": True, "cmd": command, "baud": 115200}
        return {"ok": True, "cmd": command, "protocol_supported": True}

    def fake_console_with_verified_ping(port, baud, command, timeout, *, settle_sec=1.0):
        if command == "rp2040 ping" and commands and commands[-1] == "rp2040 set-baud 115200":
            commands.append(command)
            return {"ok": True, "cmd": command, "protocol_supported": True}
        return fake_console(port, baud, command, timeout, settle_sec=settle_sec)

    monkeypatch.setattr(runner, "send_d1l_console", fake_console_with_verified_ping)

    report = runner.rp2040_access_precheck(ctx, dry_run=False)

    assert report["ok"] is True
    assert report["state"] == "bridge_protocol_ready_after_baud_probe"
    assert report["selected_rp2040_uart_baud"] == 115200
    assert report["bootloader_entry"] == "rp2040 bootloader"
    assert commands == [
        "rp2040 status",
        "rp2040 ping",
        "rp2040 baud-probe 700",
        "rp2040 set-baud 115200",
        "rp2040 ping",
    ]


def test_validation_stops_at_access_precheck_before_official_smoke_or_restore(tmp_path, monkeypatch):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--refresh-rp2040-smoke",
        ]
    )

    monkeypatch.setattr(runner, "verify_inputs", lambda ctx, args, allow_download, dry_run: {"schema": 1, "kind": "input_artifact_check", "ok": True})
    monkeypatch.setattr(
        runner,
        "flash_esp32",
        lambda ctx, dry_run, phase="initial": {
            "schema": 1,
            "kind": "esp32_flash",
            "phase": phase,
            "ok": True,
        },
    )
    monkeypatch.setattr(
        runner,
        "rp2040_access_precheck",
        lambda ctx, dry_run: {
            "schema": 1,
            "kind": "rp2040_autonomous_access_precheck",
            "ok": False,
            "error": "rp2040_bootloader_unavailable",
        },
    )
    monkeypatch.setattr(runner, "run_official_sd_smoke", lambda *args, **kwargs: (_ for _ in ()).throw(AssertionError("official smoke should not run")))
    monkeypatch.setattr(runner, "restore_bridge", lambda *args, **kwargs: (_ for _ in ()).throw(AssertionError("restore should not run")))
    monkeypatch.setattr(runner, "run_release_gate", lambda ctx, dry_run, phase=None: {"schema": 1, "kind": "release_gate_audit", "ok": True, "ready_for_public_release": False, "failed_count": 1, "p0_failed_count": 1})

    report = runner.run_validation(args)

    assert report["ok"] is False
    assert report["error"] == "rp2040_bootloader_unavailable"
    assert [step["kind"] for step in report["runs"]] == [
        "input_artifact_check",
        "esp32_flash",
        "rp2040_autonomous_access_precheck",
        "release_gate_audit",
    ]
    assert report["ready_for_public_release"] is False
    assert report["release_ready"] is False


def test_bootloader_entry_prefers_bridge_command_when_ping_ready(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    commands = []

    snapshots = [
        {"available": False, "candidates": []},
        {"available": True, "candidates": [{"path": "G:\\"}]},
    ]
    monkeypatch.setattr(runner, "uf2_volume_snapshot", lambda: snapshots.pop(0))
    monkeypatch.setattr(runner, "rp2040_port_snapshot", lambda port: (_ for _ in ()).throw(AssertionError("COM16 fallback should not run")))
    monkeypatch.setattr(runner, "enter_rp2040_bootloader_usb_touch", lambda port: (_ for _ in ()).throw(AssertionError("USB touch should not run")))
    monkeypatch.setattr(runner.time, "sleep", lambda _seconds: None)

    def fake_console(port, baud, command, timeout, *, settle_sec=1.0):
        commands.append(command)
        if command == "rp2040 ping":
            return {"ok": True, "cmd": command, "protocol_supported": True}
        if command == "rp2040 bootloader":
            return {"ok": True, "cmd": command}
        return {"ok": False, "cmd": command}

    monkeypatch.setattr(runner, "send_d1l_console", fake_console)

    report = runner.enter_rp2040_bootloader(ctx, volume=None)

    assert report["ok"] is True
    assert report["method"] == "rp2040_bridge_command"
    assert report["selected_uf2_volume"] == "G:\\"
    assert commands == ["rp2040 ping", "rp2040 bootloader"]
    assert report["bridge_bootloader"]["cmd"] == "rp2040 bootloader"


def test_bootloader_entry_uses_only_configured_rp2040_usb_cdc_port(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    commands = []
    touched = []
    snapshots = [
        {"available": False, "candidates": []},
        {"available": True, "candidates": [{"drive": "G:"}]},
    ]

    monkeypatch.setattr(runner, "uf2_volume_snapshot", lambda: snapshots.pop(0))
    monkeypatch.setattr(
        runner,
        "rp2040_port_discovery",
        lambda port, d1l_port: {
            "preferred_port": port,
            "d1l_port": d1l_port,
            "present": True,
            "selected_port": "COM16",
            "selected_reason": "configured_port_present",
            "candidates": [],
            "skipped": [],
        },
    )
    monkeypatch.setattr(runner.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(runner, "enter_rp2040_bootloader_usb_touch", lambda port: touched.append(port) or {"ok": True, "port": port})

    def fake_console(port, baud, command, timeout, *, settle_sec=1.0):
        commands.append(command)
        return {"ok": False, "cmd": command, "code": "ESP_ERR_TIMEOUT", "protocol_supported": False}

    monkeypatch.setattr(runner, "send_d1l_console", fake_console)

    report = runner.enter_rp2040_bootloader(ctx, volume=None)

    assert report["ok"] is True
    assert report["method"] == "rp2040_1200_baud_touch"
    assert report["selected_rp2040_port"] == "COM16"
    assert touched == ["COM16"]
    assert commands == ["rp2040 ping"]


def test_bootloader_entry_never_touches_inventory_only_alternative_port(
    tmp_path, monkeypatch
):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )
    touched = []
    console_ports = []
    absent = {
        "preferred_port": "COM16",
        "d1l_port": "COM12",
        "present": False,
        "selected_port": None,
        "candidates": [{"device": "COM17", "match_reasons": ["vid:2E8A"]}],
        "alternative_ports": ["COM17"],
        "alternatives_read_only": True,
        "skipped": [],
    }
    monkeypatch.setattr(
        runner,
        "uf2_volume_snapshot",
        lambda: {"available": False, "candidates": []},
    )
    monkeypatch.setattr(
        runner, "rp2040_port_discovery", lambda port, d1l_port: dict(absent)
    )
    monkeypatch.setattr(
        runner,
        "rp2040_double_reset_sweep",
        lambda ctx, **kwargs: ([], None),
    )
    monkeypatch.setattr(runner.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(
        runner,
        "enter_rp2040_bootloader_usb_touch",
        lambda port: touched.append(port) or {"ok": True, "port": port},
    )

    def fake_console(port, baud, command, timeout, *, settle_sec=1.0):
        console_ports.append(port)
        return {"ok": False, "cmd": command, "protocol_supported": False}

    monkeypatch.setattr(runner, "send_d1l_console", fake_console)

    report = runner.enter_rp2040_bootloader(ctx, volume=None)

    assert report["ok"] is False
    assert touched == []
    assert set(console_ports) == {"COM12"}


def test_official_sd_smoke_exception_writes_gate_visible_artifact(tmp_path, monkeypatch):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--refresh-rp2040-smoke",
        ]
    )

    def ok_step(kind: str) -> dict:
        return {"schema": 1, "kind": kind, "ok": True, "public_rf_tx": False, "formats_sd": False}

    monkeypatch.setattr(runner, "verify_inputs", lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"))
    patch_diag_isolation_runners(monkeypatch, ok_step)
    monkeypatch.setattr(runner, "rp2040_access_precheck", lambda ctx, dry_run: ok_step("rp2040_autonomous_access_precheck"))
    monkeypatch.setattr(runner, "run_official_sd_smoke", lambda *args, **kwargs: (_ for _ in ()).throw(OSError(22, "Invalid argument")))
    monkeypatch.setattr(runner, "run_sd_file_canary", lambda ctx, dry_run: ok_step("sd_file_canary"))
    patch_sd_evidence_runners(monkeypatch, ok_step)
    monkeypatch.setattr(runner, "run_smoke", lambda ctx, dry_run: ok_step("d1l_smoke"))
    monkeypatch.setattr(
        runner,
        "run_ui_corruption_probe",
        lambda ctx, rounds, dry_run: ok_step("ui_corruption_probe"),
    )
    monkeypatch.setattr(runner, "run_scroll_probe", lambda ctx, dry_run: ok_step("scroll_probe"))
    monkeypatch.setattr(runner, "run_release_gate", lambda ctx, dry_run, phase=None: ok_step("release_gate_audit"))

    report = runner.run_validation(args)
    artifact = runner.official_sd_smoke_out(runner.build_context(args))
    payload = json.loads(artifact.read_text(encoding="ascii"))

    assert report["ok"] is False
    assert artifact.exists()
    assert payload["kind"] == "seeed_official_sd_smoke_capture"
    assert payload["ok"] is False
    assert payload["commit"] == COMMIT
    assert payload["github_actions_run"] == "28663994079"
    assert payload["port"] == "COM16"
    assert payload["public_rf_tx"] is False
    assert payload["formats_sd"] is False
    assert "Invalid argument" in payload["error"]


def test_bridge_restore_verifies_ping_before_reporting_ok(tmp_path, monkeypatch):
    ctx = runner.RunContext(
        root=tmp_path,
        commit=COMMIT,
        short_commit=COMMIT[:7],
        github_run_id="28663994079",
        github_run_dir=tmp_path / "artifacts" / "github" / "28663994079-current",
        d1l_port="COM12",
        rp2040_port="COM16",
        hardware_dir=tmp_path / "artifacts" / "hardware" / "com12",
        rp2040_hardware_dir=tmp_path / "artifacts" / "hardware" / "com16",
        baud=115200,
        esp32_flash_baud=460800,
    )

    monkeypatch.setattr(runner, "enter_rp2040_bootloader", lambda ctx, volume: {"ok": True, "port": ctx.rp2040_port})
    monkeypatch.setattr(runner, "copy_named_uf2", lambda **_kwargs: {"ok": True, "copied": True})
    monkeypatch.setattr(runner.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(
        runner,
        "send_d1l_console",
        lambda port, baud, command, timeout: {
            "ok": command == "rp2040 ping",
            "cmd": command,
            "protocol_supported": command == "rp2040 ping",
        },
    )

    report = runner.restore_bridge(ctx, volume=None, uf2_timeout=1.0, dry_run=False)

    assert report["ok"] is True
    assert report["reset"]["cmd"] == "rp2040 reset"
    assert report["ping"]["cmd"] == "rp2040 ping"
    assert json.loads(runner.bridge_restore_out(ctx).read_text(encoding="ascii"))["ok"] is True


def test_validation_stops_before_preflight_when_bridge_restore_not_verified(tmp_path, monkeypatch):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--refresh-rp2040-smoke",
        ]
    )

    restore_attempts = []

    def failed_restore(ctx, volume, uf2_timeout, dry_run, phase="initial"):
        restore_attempts.append(1)
        return {"schema": 1, "kind": "rp2040_bridge_restore", "phase": phase, "ok": False, "error": "bridge_restore_not_verified"}

    monkeypatch.setattr(runner, "verify_inputs", lambda ctx, args, allow_download, dry_run: {"schema": 1, "kind": "input_artifact_check", "ok": True})
    monkeypatch.setattr(
        runner,
        "flash_esp32",
        lambda ctx, dry_run, phase="initial": {
            "schema": 1,
            "kind": "esp32_flash",
            "phase": phase,
            "ok": True,
        },
    )
    monkeypatch.setattr(runner, "rp2040_access_precheck", lambda ctx, dry_run: {"schema": 1, "kind": "rp2040_autonomous_access_precheck", "ok": True})
    monkeypatch.setattr(runner, "restore_bridge", failed_restore)
    monkeypatch.setattr(runner, "run_preflight", lambda ctx, dry_run, verify_artifact, phase="initial": (_ for _ in ()).throw(AssertionError("preflight should not run")))
    monkeypatch.setattr(runner, "run_release_gate", lambda ctx, dry_run, phase=None: {"schema": 1, "kind": "release_gate_audit", "ok": True, "ready_for_public_release": False, "failed_count": 1, "p0_failed_count": 1})

    report = runner.run_validation(args)

    assert report["ok"] is False
    assert report["error"] == "bridge_restore_not_verified"
    assert len(restore_attempts) == 2
    assert [step["kind"] for step in report["runs"]] == [
        "input_artifact_check",
        "esp32_flash",
        "rp2040_autonomous_access_precheck",
        "seeed_official_sd_smoke_capture",
        "sd_boot_prepare_rp2040_unavailable",
        "rp2040_bridge_restore",
        "rp2040_bridge_restore",
        "release_gate_audit",
    ]
    assert report["release_gate"]["ready_for_public_release"] is False
    assert report["ready_for_public_release"] is False
    assert report["release_ready"] is False


def test_completed_validation_surfaces_release_not_ready(tmp_path, monkeypatch):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-rp2040-official-smoke",
        ]
    )

    def ok_step(kind: str) -> dict:
        return {"schema": 1, "kind": kind, "ok": True, "public_rf_tx": False, "formats_sd": False}

    monkeypatch.setattr(runner, "verify_inputs", lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"))
    patch_diag_isolation_runners(monkeypatch, ok_step)
    restore_calls = {}

    def restore_with_transient_post_diag_failure(
        ctx, volume, uf2_timeout, dry_run, phase="initial"
    ):
        restore_calls[phase] = restore_calls.get(phase, 0) + 1
        return {
            **ok_step("rp2040_bridge_restore"),
            "phase": phase,
            "ok": phase != "post_diag" or restore_calls[phase] == 2,
        }

    monkeypatch.setattr(
        runner, "restore_bridge", restore_with_transient_post_diag_failure
    )
    monkeypatch.setattr(runner, "rp2040_access_precheck", lambda ctx, dry_run: ok_step("rp2040_autonomous_access_precheck"))
    monkeypatch.setattr(runner, "run_sd_file_canary", lambda ctx, dry_run: ok_step("sd_file_canary"))
    patch_sd_evidence_runners(monkeypatch, ok_step)
    monkeypatch.setattr(runner, "run_smoke", lambda ctx, dry_run: ok_step("d1l_smoke"))
    monkeypatch.setattr(
        runner,
        "run_release_gate",
        lambda ctx, dry_run, phase=None: {
            "schema": 1,
            "kind": "release_gate_audit",
            "ok": True,
            "ready_for_public_release": False,
            "failed_count": 3,
            "p0_failed_count": 2,
        },
    )

    report = runner.run_validation(args)
    post_diag_attempts = [
        step
        for step in report["runs"]
        if step.get("kind") == "rp2040_bridge_restore"
        and step.get("phase") == "post_diag"
    ]

    assert report["ok"] is True
    assert [(step["attempt"], step["ok"]) for step in post_diag_attempts] == [
        (1, False),
        (2, True),
    ]
    assert report["ready_for_public_release"] is False
    assert report["release_ready"] is False
    assert report["release_gate"]["p0_failed_count"] == 2


def test_completed_validation_runs_compose_capture_when_ui_probes_enabled(tmp_path, monkeypatch):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-rp2040-official-smoke",
            "--include-ui-probes",
        ]
    )

    def ok_step(kind: str) -> dict:
        return {"schema": 1, "kind": kind, "ok": True, "public_rf_tx": False, "formats_sd": False}

    monkeypatch.setattr(runner, "verify_inputs", lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"))
    patch_diag_isolation_runners(monkeypatch, ok_step)
    monkeypatch.setattr(runner, "rp2040_access_precheck", lambda ctx, dry_run: ok_step("rp2040_autonomous_access_precheck"))
    monkeypatch.setattr(runner, "run_sd_file_canary", lambda ctx, dry_run: ok_step("sd_file_canary"))
    patch_sd_evidence_runners(monkeypatch, ok_step)
    monkeypatch.setattr(runner, "run_smoke", lambda ctx, dry_run: ok_step("d1l_smoke"))
    monkeypatch.setattr(runner, "run_onboarding_complete", lambda ctx, dry_run: ok_step("onboarding_complete"))
    monkeypatch.setattr(runner, "run_ui_corruption_probe", lambda ctx, rounds, dry_run: ok_step("ui_corruption_probe"))
    monkeypatch.setattr(runner, "run_scroll_probe", lambda ctx, dry_run: ok_step("scroll_probe"))
    monkeypatch.setattr(runner, "run_ui_pixel_capture", lambda ctx, dry_run: ok_step("ui_pixel_capture"))
    monkeypatch.setattr(
        runner,
        "run_ui_compose_keyboard_capture",
        lambda ctx, dry_run: ok_step("ui_compose_keyboard_capture"),
    )
    monkeypatch.setattr(
        runner,
        "run_release_gate",
        lambda ctx, dry_run, phase=None: {
            "schema": 1,
            "kind": "release_gate_audit",
            "ok": True,
            "ready_for_public_release": False,
            "failed_count": 3,
            "p0_failed_count": 2,
        },
    )

    report = runner.run_validation(args)

    assert [step["kind"] for step in report["runs"]] == [
        "input_artifact_check",
        "esp32_flash",
        "rp2040_autonomous_access_precheck",
        "rp2040_bridge_restore",
        "rp2040_preflight",
        "pre_diag_clean_preflight_gate",
        "sd_raw_diag",
        "rp2040_bridge_restore",
        "esp32_flash",
        "rp2040_preflight",
        "post_diag_clean_preflight_gate",
        "sd_file_canary",
        "sd_boot_prepare_correct_structure",
        "sd_boot_prepare_missing_structure",
        "sd_boot_prepare_existing_data",
        "sd_map_tile_canary",
        "sd_export_canary",
        "sd_diagnostic_export",
        "sd_data_export",
        "sd_retained_history",
        "sd_reboot_remount",
        "d1l_smoke",
        "onboarding_complete",
        "ui_corruption_probe",
        "scroll_probe",
        "ui_pixel_capture",
        "ui_compose_keyboard_capture",
        "release_gate_audit",
    ]
    assert report["ok"] is True


def test_raw_diag_failure_runs_exact_reentry_boundary_then_stops_before_canaries(
    tmp_path, monkeypatch
):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-rp2040-official-smoke",
        ]
    )

    def ok_step(kind: str) -> dict:
        return {
            "schema": 1,
            "kind": kind,
            "ok": True,
            "public_rf_tx": False,
            "formats_sd": False,
        }

    monkeypatch.setattr(
        runner,
        "verify_inputs",
        lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"),
    )
    patch_diag_isolation_runners(monkeypatch, ok_step)
    monkeypatch.setattr(
        runner,
        "rp2040_access_precheck",
        lambda ctx, dry_run: ok_step("rp2040_autonomous_access_precheck"),
    )
    monkeypatch.setattr(
        runner,
        "run_sd_raw_diag",
        lambda ctx, dry_run: {
            **ok_step("sd_raw_diag"),
            "ok": False,
            "error": "diagnostic deadline exhausted",
        },
    )
    monkeypatch.setattr(
        runner,
        "run_sd_file_canary",
        lambda ctx, dry_run: (_ for _ in ()).throw(
            AssertionError("canary must not run after failed raw diag")
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_release_gate",
        lambda ctx, dry_run, phase=None: {
            **ok_step("release_gate_audit"),
            "ready_for_public_release": False,
        },
    )

    report = runner.run_validation(args)

    assert report["ok"] is False
    assert report["error"] == "sd_raw_diag_failed"
    assert [step["kind"] for step in report["runs"]] == [
        "input_artifact_check",
        "esp32_flash",
        "rp2040_autonomous_access_precheck",
        "rp2040_bridge_restore",
        "rp2040_preflight",
        "pre_diag_clean_preflight_gate",
        "sd_raw_diag",
        "rp2040_bridge_restore",
        "esp32_flash",
        "rp2040_preflight",
        "post_diag_clean_preflight_gate",
        "release_gate_audit",
    ]


def test_raw_diag_failure_remains_primary_when_exact_recovery_also_fails(
    tmp_path, monkeypatch
):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-rp2040-official-smoke",
        ]
    )

    def ok_step(kind: str) -> dict:
        return {"schema": 1, "kind": kind, "ok": True}

    monkeypatch.setattr(
        runner,
        "verify_inputs",
        lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"),
    )
    patch_diag_isolation_runners(monkeypatch, ok_step)
    monkeypatch.setattr(
        runner,
        "rp2040_access_precheck",
        lambda ctx, dry_run: ok_step("rp2040_autonomous_access_precheck"),
    )

    def restore_with_post_diag_failure(
        ctx, volume, uf2_timeout, dry_run, phase="initial"
    ):
        return {
            **ok_step("rp2040_bridge_restore"),
            "phase": phase,
            "ok": phase != "post_diag",
            "error": "bridge_restore_not_verified" if phase == "post_diag" else None,
        }

    monkeypatch.setattr(runner, "restore_bridge", restore_with_post_diag_failure)
    monkeypatch.setattr(
        runner,
        "run_sd_raw_diag",
        lambda ctx, dry_run: {
            **ok_step("sd_raw_diag"),
            "ok": False,
            "error": "diagnostic deadline exhausted",
        },
    )
    monkeypatch.setattr(
        runner,
        "run_sd_file_canary",
        lambda ctx, dry_run: (_ for _ in ()).throw(
            AssertionError("canaries must not run")
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_release_gate",
        lambda ctx, dry_run, phase=None: {
            **ok_step("release_gate_audit"),
            "ready_for_public_release": False,
        },
    )

    report = runner.run_validation(args)

    assert report["error"] == "sd_raw_diag_failed"
    assert report["recovery"]["ok"] is False
    assert report["recovery"]["error"] == "post_diag_bridge_restore_failed"
    assert [
        (step["kind"], step.get("phase"))
        for step in report["runs"]
        if step["kind"] in {"esp32_flash", "rp2040_bridge_restore", "rp2040_preflight"}
    ] == [
        ("esp32_flash", "initial"),
        ("rp2040_bridge_restore", "pre_diag"),
        ("rp2040_preflight", "pre_diag"),
        ("rp2040_bridge_restore", "post_diag"),
        ("rp2040_bridge_restore", "post_diag"),
        ("esp32_flash", "post_diag"),
        ("rp2040_preflight", "post_diag"),
    ]


def test_unclean_post_diag_preflight_stops_before_canaries(tmp_path, monkeypatch):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-rp2040-official-smoke",
        ]
    )

    def ok_step(kind: str) -> dict:
        return {"schema": 1, "kind": kind, "ok": True}

    monkeypatch.setattr(
        runner,
        "verify_inputs",
        lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"),
    )
    patch_diag_isolation_runners(monkeypatch, ok_step)
    monkeypatch.setattr(
        runner,
        "rp2040_access_precheck",
        lambda ctx, dry_run: ok_step("rp2040_autonomous_access_precheck"),
    )
    monkeypatch.setattr(
        runner,
        "run_sd_raw_diag",
        lambda ctx, dry_run: ok_step("sd_raw_diag"),
    )
    monkeypatch.setattr(
        runner,
        "run_clean_preflight_gate",
        lambda ctx, preflight, dry_run, phase="post_diag": (
            {
                **ok_step("post_diag_clean_preflight_gate"),
                "phase": phase,
                "ok": False,
                "error": "post_diag_preflight_not_clean",
            }
            if phase == "post_diag"
            else {**ok_step("pre_diag_clean_preflight_gate"), "phase": phase}
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_sd_file_canary",
        lambda ctx, dry_run: (_ for _ in ()).throw(
            AssertionError("canary must not run with retained failures")
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_release_gate",
        lambda ctx, dry_run, phase=None: {
            **ok_step("release_gate_audit"),
            "ready_for_public_release": False,
        },
    )

    report = runner.run_validation(args)

    assert report["ok"] is False
    assert report["error"] == "post_diag_preflight_not_clean"
    assert "sd_file_canary" not in [step["kind"] for step in report["runs"]]


def test_actions_provenance_binds_exact_commit_run_and_artifact_hashes(tmp_path):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    write_actions_provenance_fixture(run_dir)
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-rp2040-official-smoke",
        ]
    )
    ctx = runner.build_context(args)

    provenance = runner.verify_actions_artifact_provenance(ctx, args)

    assert provenance["ok"] is True
    assert provenance["commit"] == COMMIT
    assert provenance["github_actions_run"] == "28663994079"
    assert provenance["workflow_run_attempt"] == "1"
    assert [group["name"] for group in provenance["rp2040_artifact_groups"]] == [
        "rp2040-sd-bridge-firmware"
    ]


def test_input_check_fails_closed_on_swapped_manifest_or_tampered_artifact(tmp_path):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    manifest_path = write_actions_provenance_fixture(run_dir)
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-rp2040-official-smoke",
        ]
    )
    ctx = runner.build_context(args)

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["workflow"]["sha"] = "0" * 40
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    swapped = runner.verify_inputs(
        ctx, args, allow_download=False, dry_run=False
    )
    assert swapped["ok"] is False
    assert "workflow.sha" in swapped["provenance"]["error"]

    manifest["workflow"]["sha"] = COMMIT
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    (run_dir / "rp2040-sd-bridge-firmware" / runner.BRIDGE_UF2).write_bytes(
        b"tampered"
    )
    tampered = runner.verify_inputs(
        ctx, args, allow_download=False, dry_run=False
    )
    assert tampered["ok"] is False
    assert "sha256" in tampered["provenance"]["error"]


def test_context_requires_explicit_numeric_run_full_sha_and_fixed_ports(tmp_path):
    with pytest.raises(SystemExit):
        runner.parse_args(["--root", str(tmp_path), "--commit", COMMIT])

    short_sha_args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT[:7],
            "--github-run-id",
            "28663994079",
        ]
    )
    with pytest.raises(ValueError, match="canonical 40-hex"):
        runner.build_context(short_sha_args)

    nonnumeric_run_args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "latest",
        ]
    )
    with pytest.raises(ValueError, match="explicit numeric"):
        runner.build_context(nonnumeric_run_args)

    wrong_port_args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--rp2040-port",
            "COM17",
        ]
    )
    with pytest.raises(
        ValueError,
        match=(
            f"restricted to {runner.DEFAULT_D1L_PORT} and "
            f"{runner.DEFAULT_RP2040_PORT}"
        ),
    ):
        runner.build_context(wrong_port_args)


def test_wait_for_uf2_volume_requires_correlated_or_explicit_target(monkeypatch):
    monkeypatch.setattr(
        runner,
        "choose_volume",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            AssertionError("automatic volume selection must not run")
        ),
    )

    with pytest.raises(runner.FlashGuardError, match="correlated or explicit"):
        runner.wait_for_uf2_volume(0, None)


def test_uf2_selection_ignores_preexisting_volume_and_selects_only_new_volume():
    initial = {runner.uf2_volume_key("F:\\")}
    snapshot = {
        "available": True,
        "candidates": [{"path": "f:"}, {"path": "G:\\"}],
    }

    selected, appeared = runner.select_correlated_uf2_volume(
        initial, snapshot, explicit_volume=None
    )

    assert selected == "G:\\"
    assert appeared == ["G:\\"]


def test_uf2_selection_rejects_multiple_new_volumes():
    with pytest.raises(runner.FlashGuardError, match="multiple UF2 volumes"):
        runner.select_correlated_uf2_volume(
            set(),
            {
                "available": True,
                "candidates": [{"path": "F:\\"}, {"path": "G:\\"}],
            },
            explicit_volume=None,
        )


def test_unclean_pre_diag_gate_stops_before_raw_diag(tmp_path, monkeypatch):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-rp2040-official-smoke",
        ]
    )

    def ok_step(kind: str) -> dict:
        return {"schema": 1, "kind": kind, "ok": True}

    monkeypatch.setattr(
        runner,
        "verify_inputs",
        lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"),
    )
    patch_diag_isolation_runners(monkeypatch, ok_step)
    monkeypatch.setattr(
        runner,
        "rp2040_access_precheck",
        lambda ctx, dry_run: ok_step("rp2040_autonomous_access_precheck"),
    )
    monkeypatch.setattr(
        runner,
        "run_clean_preflight_gate",
        lambda ctx, preflight, dry_run, phase="post_diag": (
            {
                **ok_step("pre_diag_clean_preflight_gate"),
                "ok": False,
                "phase": phase,
                "error": "pre_diag_preflight_not_clean",
            }
            if phase == "pre_diag"
            else {**ok_step("post_diag_clean_preflight_gate"), "phase": phase}
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_sd_raw_diag",
        lambda ctx, dry_run: (_ for _ in ()).throw(
            AssertionError("raw diagnostics must not run from a dirty baseline")
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_release_gate",
        lambda ctx, dry_run, phase=None: {
            **ok_step("release_gate_audit"),
            "ready_for_public_release": False,
        },
    )

    report = runner.run_validation(args)

    assert report["error"] == "pre_diag_preflight_not_clean"
    assert [step["kind"] for step in report["runs"]][-2:] == [
        "pre_diag_clean_preflight_gate",
        "release_gate_audit",
    ]


def test_later_sd_stage_failure_audits_then_recovers_before_return(
    tmp_path, monkeypatch
):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-rp2040-official-smoke",
        ]
    )

    def ok_step(kind: str) -> dict:
        return {"schema": 1, "kind": kind, "ok": True}

    monkeypatch.setattr(
        runner,
        "verify_inputs",
        lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"),
    )
    patch_diag_isolation_runners(monkeypatch, ok_step)
    patch_sd_evidence_runners(monkeypatch, ok_step)
    monkeypatch.setattr(
        runner,
        "rp2040_access_precheck",
        lambda ctx, dry_run: ok_step("rp2040_autonomous_access_precheck"),
    )
    monkeypatch.setattr(
        runner,
        "run_sd_file_canary",
        lambda ctx, dry_run: ok_step("sd_file_canary"),
    )
    monkeypatch.setattr(
        runner,
        "run_sd_map_tile_canary",
        lambda ctx, dry_run: {
            **ok_step("sd_map_tile_canary"),
            "ok": False,
            "path": "failed-map-receipt.json",
        },
    )
    monkeypatch.setattr(
        runner,
        "run_sd_export_canary",
        lambda ctx, dry_run: (_ for _ in ()).throw(
            AssertionError("later stage must not run")
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_smoke",
        lambda ctx, dry_run: (_ for _ in ()).throw(
            AssertionError("smoke must not run")
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_release_gate",
        lambda ctx, dry_run, phase=None: {
            **ok_step("release_gate_audit"),
            "ready_for_public_release": False,
        },
    )

    report = runner.run_validation(args)
    kinds = [step["kind"] for step in report["runs"]]

    assert report["error"] == "sd_map_tile_canary_failed"
    assert report["recovery"]["ok"] is True
    assert "sd_export_canary" not in kinds
    failed_index = kinds.index("sd_map_tile_canary")
    assert kinds[failed_index:failed_index + 6] == [
        "sd_map_tile_canary",
        "release_gate_audit",
        "rp2040_bridge_restore",
        "esp32_flash",
        "rp2040_preflight",
        "recovery_after_sd_map_tile_canary_failed_clean_preflight_gate",
    ]
    assert kinds[failed_index + 6] == "release_gate_audit"


def test_post_sd_smoke_failure_audits_recovers_reaudits_and_stops_ui(
    tmp_path, monkeypatch
):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-rp2040-official-smoke",
            "--include-ui-probes",
        ]
    )

    def ok_step(kind: str) -> dict:
        return {"schema": 1, "kind": kind, "ok": True}

    monkeypatch.setattr(
        runner,
        "verify_inputs",
        lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"),
    )
    patch_diag_isolation_runners(monkeypatch, ok_step)
    patch_sd_evidence_runners(monkeypatch, ok_step)
    monkeypatch.setattr(
        runner,
        "rp2040_access_precheck",
        lambda ctx, dry_run: ok_step("rp2040_autonomous_access_precheck"),
    )
    monkeypatch.setattr(
        runner,
        "run_sd_file_canary",
        lambda ctx, dry_run: ok_step("sd_file_canary"),
    )
    monkeypatch.setattr(
        runner,
        "run_smoke",
        lambda ctx, dry_run: {
            **ok_step("d1l_smoke"),
            "ok": False,
            "path": "failed-smoke.json",
        },
    )
    monkeypatch.setattr(
        runner,
        "run_onboarding_complete",
        lambda ctx, dry_run: (_ for _ in ()).throw(
            AssertionError("UI probes must not run after smoke failure")
        ),
    )
    audit_calls = []

    def release_gate(ctx, dry_run, phase=None):
        audit_calls.append(len(audit_calls) + 1)
        return {
            **ok_step("release_gate_audit"),
            "path": f"gate-{audit_calls[-1]}.json",
            "ready_for_public_release": False,
        }

    monkeypatch.setattr(runner, "run_release_gate", release_gate)

    report = runner.run_validation(args)
    kinds = [step["kind"] for step in report["runs"]]
    failed_index = kinds.index("d1l_smoke")

    assert report["error"] == "d1l_smoke_failed"
    assert report["recovery"]["ok"] is True
    assert audit_calls == [1, 2]
    assert report["release_gate"]["path"] == "gate-2.json"
    assert kinds[failed_index:] == [
        "d1l_smoke",
        "release_gate_audit",
        "rp2040_bridge_restore",
        "esp32_flash",
        "rp2040_preflight",
        "recovery_after_d1l_smoke_failed_clean_preflight_gate",
        "release_gate_audit",
    ]


def test_ui_only_smoke_exception_audits_without_rp2040_recovery(
    tmp_path, monkeypatch
):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-sd-suite",
            "--include-ui-probes",
        ]
    )

    def ok_step(kind: str) -> dict:
        return {"schema": 1, "kind": kind, "ok": True}

    monkeypatch.setattr(
        runner,
        "verify_inputs",
        lambda ctx, args, allow_download, dry_run: ok_step("input_artifact_check"),
    )
    monkeypatch.setattr(
        runner,
        "flash_esp32",
        lambda ctx, dry_run, phase="initial": {
            **ok_step("esp32_flash"),
            "phase": phase,
        },
    )
    monkeypatch.setattr(
        runner,
        "run_smoke",
        lambda ctx, dry_run: (_ for _ in ()).throw(RuntimeError("smoke exploded")),
    )
    monkeypatch.setattr(
        runner,
        "restore_bridge",
        lambda *args, **kwargs: (_ for _ in ()).throw(
            AssertionError("UI-only smoke failure must not recover RP2040")
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_onboarding_complete",
        lambda ctx, dry_run: (_ for _ in ()).throw(
            AssertionError("UI probes must not run")
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_release_gate",
        lambda ctx, dry_run, phase=None: {
            **ok_step("release_gate_audit"),
            "ready_for_public_release": False,
        },
    )

    report = runner.run_validation(args)
    kinds = [step["kind"] for step in report["runs"]]

    assert report["error"] == "d1l_smoke_failed"
    assert kinds == [
        "input_artifact_check",
        "esp32_flash",
        "d1l_smoke",
        "release_gate_audit",
    ]
    assert "recovery" not in report


def test_recovery_exceptions_write_phase_specific_receipts(tmp_path, monkeypatch):
    run_dir = tmp_path / "artifacts" / "github" / "28663994079-current"
    args = runner.parse_args(
        [
            "--root",
            str(tmp_path),
            "--commit",
            COMMIT,
            "--github-run-id",
            "28663994079",
            "--github-run-dir",
            str(run_dir),
            "--skip-rp2040-official-smoke",
        ]
    )
    ctx = runner.build_context(args)
    runs = []
    monkeypatch.setattr(
        runner,
        "restore_bridge",
        lambda ctx, volume, uf2_timeout, dry_run, phase="initial": {
            "schema": 1,
            "kind": "rp2040_bridge_restore",
            "phase": phase,
            "ok": True,
        },
    )
    monkeypatch.setattr(
        runner,
        "flash_esp32",
        lambda *args, **kwargs: (_ for _ in ()).throw(RuntimeError("flash exploded")),
    )
    monkeypatch.setattr(
        runner,
        "run_preflight",
        lambda *args, **kwargs: (_ for _ in ()).throw(
            RuntimeError("preflight exploded")
        ),
    )
    monkeypatch.setattr(
        runner,
        "run_clean_preflight_gate",
        lambda *args, **kwargs: (_ for _ in ()).throw(
            RuntimeError("clean gate exploded")
        ),
    )

    recovery = runner.run_exact_recovery_boundary(
        ctx, args, runs, phase="receipt_test"
    )

    assert recovery["ok"] is False
    expected = [
        runner.esp32_flash_out(ctx, "receipt_test"),
        runner.preflight_out(ctx, "receipt_test"),
        runner.clean_preflight_gate_out(ctx, "receipt_test"),
    ]
    for path in expected:
        payload = json.loads(path.read_text(encoding="utf-8"))
        assert payload["ok"] is False
        assert payload["phase"] == "receipt_test"
        assert payload["path"] == str(path)
