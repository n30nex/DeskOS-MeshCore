import argparse
import inspect
import json
import re
import subprocess
from pathlib import Path

import pytest

from scripts import post_flash_feature_matrix_d1l as matrix


COMMIT = "a" * 40
OTHER_COMMIT = "b" * 40
RUN_ID = "123456789"
D1L_PORT = f"COM{matrix.D1L_PORT_NUMBER}"


def test_cli_rejects_hardware_before_any_child_or_identity_io(
    monkeypatch, capsys
):
    monkeypatch.setattr(
        matrix,
        "run_matrix",
        lambda _args: pytest.fail(
            "disabled hardware CLI must not enter the matrix"
        ),
    )

    result = matrix.main(
        [
            "--port",
            D1L_PORT,
            "--expected-firmware-commit",
            COMMIT,
            "--github-actions-run",
            RUN_ID,
        ]
    )

    output = json.loads(capsys.readouterr().out)
    assert result == 2
    assert output["classification"] == "hardware_execution_disabled"
    assert output["ok"] is False
    assert output["public_rf_tx"] is False
    assert output["dm_rf_tx"] is False
    assert output["formats_sd"] is False
    assert "narrow Core" in output["error"]


def args(**overrides):
    values = {
        "port": D1L_PORT,
        "expected_firmware_commit": COMMIT,
        "github_actions_run": RUN_ID,
        "github_run_dir": "artifacts/github/exact-run",
        "baud": matrix.D1L_BAUD,
        "timeout": 15.0,
        "session_timeout_sec": 900.0,
        "wifi_cycles": 1,
        "hardware_dir": "artifacts/hardware",
        "dry_run": True,
        "out": None,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


def clean_source(commit=COMMIT):
    return {
        "commit": commit,
        "short_commit": commit[:7],
        "branch": "main",
        "dirty": False,
        "dirty_entries": [],
    }


def common_receipt(**overrides):
    value = {
        "schema": 1,
        "mode": "hardware",
        "ok": True,
        "port": D1L_PORT,
        "baud": matrix.D1L_BAUD,
        "commit": COMMIT,
        "git": clean_source(),
    }
    value.update(overrides)
    return value


def smoke_receipt(**overrides):
    value = common_receipt(
        expected_firmware_commit=COMMIT,
        device_build_commit=COMMIT,
        firmware_identity_ok=True,
        persistence={
            "schema": 1,
            "test": "settings_persistence_reboot",
            "ok": True,
            "reboot_proven": True,
            "cleanup_reboot_proven": True,
            "cleanup_ok": True,
        },
    )
    value.update(overrides)
    return value


def wifi_receipt(**overrides):
    value = common_receipt(
        kind="wifi_saved_profile_resilience",
        expected_firmware_commit=COMMIT,
        acceptance_passed=True,
        truth={
            "exact_commit_verified": True,
            "physical_observed": True,
            "simulated": False,
            "dry_run": False,
        },
        physical_observed=True,
        simulated=False,
        dry_run=False,
        hardware_transport={
            "opened_by_runner": True,
            "closed_by_runner": True,
            "port": D1L_PORT,
            "baud": matrix.D1L_BAUD,
        },
        feature_evidence_eligible=True,
        profile_preserved=True,
        enable_intent_restored=True,
        credential_handling={
            "password_argument_supported": False,
            "credential_supplied_to_harness": False,
            "credential_logged": False,
            "credential_stored": False,
        },
        profile_mutation={
            "write_commands_allowed": False,
            "profile_written": False,
            "profile_cleared": False,
        },
        release_gate_eligible=False,
        full_wp13_matrix_covered=False,
        uncovered_release_scenarios=["full_wifi_matrix"],
        commands=[
            "version",
            "health",
            "wifi status",
            "wifi scan",
            "wifi connect",
        ],
        commands_redacted=True,
        checks={"exact_commit_verified": True, "profile_preserved": True},
        rp2040_accessed=False,
        public_rf_tx=False,
        dm_rf_tx=False,
        formats_sd=False,
    )
    value.update(overrides)
    return value


def ui_receipt(**overrides):
    value = common_receipt(
        screens=list(matrix.UI_SCREENS),
        failure_count=0,
        failures=[],
        map_network_evidence={"measured": True, "unchanged": True},
        network_tx=False,
        map_network_requests=False,
        read_only=True,
        manual_touch=False,
        clear_crashlog_before_start=False,
        save_actions=False,
        clear_actions=False,
        storage_mutation=False,
        wifi_mutation=False,
        rf_tx=False,
        public_rf_tx=False,
        dm_rf_tx=False,
        formats_sd=False,
    )
    value.update(overrides)
    return value


def sd_receipt(**overrides):
    value = common_receipt(
        commands=list(matrix.SD_FILE_CANARY_COMMANDS),
        sequence_completed=True,
        unexpected_console_restart=False,
        canary_passed=True,
        canary_unavailable_ok=False,
        storage_file_gate_ready_before=True,
        storage_file_gate_ready_after=True,
        retained_history_sd_ready_before=True,
        retained_history_sd_ready_after=True,
        allow_unavailable=False,
        public_rf_tx=False,
        formats_sd=False,
    )
    value.update(overrides)
    return value


def identity_receipt(**overrides):
    value = {
        "schema": 1,
        "kind": "post_flash_identity_guard",
        "mode": "hardware",
        "port": D1L_PORT,
        "baud": matrix.D1L_BAUD,
        "expected_firmware_commit": COMMIT,
        "device_build_commit": COMMIT,
        "boot_nonce": 123,
        "hardware_transport": {
            "opened_by_runner": True,
            "closed_by_runner": True,
            "port": D1L_PORT,
            "baud": matrix.D1L_BAUD,
        },
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
        "ok": True,
    }
    value.update(overrides)
    return value


def valid_actions_context(run_dir, expected_commit=COMMIT, run_id=RUN_ID):
    return {
        "ok": True,
        "github_run_dir": str(run_dir),
        "expected_firmware_commit": expected_commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": "1",
        "checks": {"exact_pair": True},
        "failures": [],
    }


def child_receipts():
    return {
        "smoke_d1l.py": smoke_receipt(),
        "wifi_resilience_d1l.py": wifi_receipt(),
        "scroll_probe_d1l.py": ui_receipt(),
        "sd_file_canary_d1l.py": sd_receipt(),
    }


def fake_process_runner(receipts, captured_environments=None):
    def run(command, **kwargs):
        if captured_environments is not None:
            captured_environments.append(dict(kwargs["env"]))
        receipt_path = Path(command[command.index("--out") + 1])
        receipt_path.write_text(
            json.dumps(receipts[Path(command[1]).name]),
            encoding="ascii",
        )
        return subprocess.CompletedProcess(command, 0, stdout="{}", stderr="")

    return run


def fake_identity(**_kwargs):
    return identity_receipt()


def fake_actions(run_dir, *, expected_commit, run_id):
    return valid_actions_context(run_dir, expected_commit, run_id)


def test_dry_run_is_truthful_bounded_and_not_release_evidence():
    report = matrix.dry_run_report(args())

    assert report["ok"] is True
    assert report["classification"] == "dry_run_plan_only"
    assert report["physical_observed"] is False
    assert report["feature_subset_complete"] is False
    assert report["full_feature_matrix_covered"] is False
    assert report["release_gate_eligible"] is False
    assert report["soak_started"] is False
    assert report["soak_deferred_until_feature_tests_complete"] is True
    assert report["actions_provenance_required_before_hardware"] is True
    assert [step["name"] for step in report["plan"]] == [
        "smoke_persistence",
        "wifi_saved_profile",
        "ui_scroll",
        "sd_file_canary",
    ]
    assert report["covered_feature_subsets"] == list(matrix.COVERED_FEATURE_SUBSETS)
    assert "sd_raw_diag_complete" in report["uncovered_full_feature_gates"]
    assert "ble_companion_transport_acceptance" in report[
        "uncovered_full_feature_gates"
    ]
    assert "full_duration_idle_soak" in report["uncovered_full_feature_gates"]


def test_plan_uses_current_child_clis_and_no_sensitive_or_destructive_arguments():
    report = matrix.dry_run_report(args())
    plans = {step["name"]: step["argv"] for step in report["plan"]}

    for argv in plans.values():
        assert argv[argv.index("--port") + 1] == D1L_PORT
        assert argv[argv.index("--baud") + 1] == str(matrix.D1L_BAUD)
        assert "--out" in argv
        lowered = " ".join(argv).lower()
        for forbidden in (
            "--password",
            "--psk",
            "mesh send",
            "storage format",
            "format-confirm",
            "diag raw",
            "soak_d1l.py",
            "flash_d1l.py",
            "esptool",
        ):
            assert forbidden not in lowered

    assert "--persistence-test" in plans["smoke_persistence"]
    assert "--expected-firmware-commit" in plans["smoke_persistence"]
    assert "--scan-timeout" in plans["wifi_saved_profile"]
    assert "--wait-sec" in plans["wifi_saved_profile"]
    assert "--screens" in plans["ui_scroll"]
    assert "--mount-wait-sec" in plans["sd_file_canary"]


@pytest.mark.parametrize("number", [8, 11, 29])
def test_forbidden_ports_fail_before_execution(number):
    with pytest.raises(ValueError, match="forbidden port"):
        matrix.dry_run_report(args(port=f"COM{number}"))


@pytest.mark.parametrize("port", ["COM7", "COM15", "ttyUSB0", "", "COM0"])
def test_every_non_d1l_port_fails_before_execution(port):
    with pytest.raises(ValueError):
        matrix.dry_run_report(args(port=port))


@pytest.mark.parametrize("commit", ["a" * 39, "g" * 40, "", "main"])
def test_exact_commit_is_required(commit):
    with pytest.raises(ValueError, match="exact 40-character SHA"):
        matrix.dry_run_report(args(expected_firmware_commit=commit))


@pytest.mark.parametrize("run_id", ["0", "-1", "12x", "", " 123 "])
def test_positive_exact_run_id_is_required(run_id):
    with pytest.raises(ValueError, match="positive numeric"):
        matrix.dry_run_report(args(github_actions_run=run_id))


def test_no_literal_protected_com_port_in_orchestrator_source():
    source = inspect.getsource(matrix)

    assert re.search(r"\bCOM(?:8|11|12|29)\b", source) is None
    assert 'parser.add_argument("--port", required=True)' in source


def test_smoke_receipt_matches_current_schema_and_requires_reboot_cleanup():
    result = matrix.validate_child_receipt(
        "smoke_persistence",
        smoke_receipt(),
        expected_commit=COMMIT,
    )
    assert result["valid"] is True

    broken = smoke_receipt()
    broken["persistence"]["cleanup_reboot_proven"] = False
    assert matrix.validate_child_receipt(
        "smoke_persistence", broken, expected_commit=COMMIT
    )["valid"] is False


def test_wifi_receipt_requires_exact_physical_safe_saved_profile_schema():
    assert matrix.validate_child_receipt(
        "wifi_saved_profile",
        wifi_receipt(),
        expected_commit=COMMIT,
    )["valid"] is True

    broken = wifi_receipt()
    broken["credential_handling"]["credential_logged"] = True
    assert matrix.validate_child_receipt(
        "wifi_saved_profile", broken, expected_commit=COMMIT
    )["valid"] is False

    broken = wifi_receipt(commands=["version", "wifi set password secret"])
    assert matrix.validate_child_receipt(
        "wifi_saved_profile", broken, expected_commit=COMMIT
    )["valid"] is False


def test_ui_receipt_requires_exact_screens_and_measured_no_network():
    assert matrix.validate_child_receipt(
        "ui_scroll",
        ui_receipt(),
        expected_commit=COMMIT,
    )["valid"] is True

    broken = ui_receipt()
    broken["map_network_evidence"]["unchanged"] = False
    assert matrix.validate_child_receipt(
        "ui_scroll", broken, expected_commit=COMMIT
    )["valid"] is False


def test_sd_receipt_requires_real_ready_file_canary_and_exact_command_list():
    assert matrix.validate_child_receipt(
        "sd_file_canary",
        sd_receipt(),
        expected_commit=COMMIT,
    )["valid"] is True

    broken = sd_receipt(canary_passed=False, canary_unavailable_ok=True)
    assert matrix.validate_child_receipt(
        "sd_file_canary", broken, expected_commit=COMMIT
    )["valid"] is False

    broken = sd_receipt(commands=[*matrix.SD_FILE_CANARY_COMMANDS, "storage diag raw"])
    assert matrix.validate_child_receipt(
        "sd_file_canary", broken, expected_commit=COMMIT
    )["valid"] is False


@pytest.mark.parametrize(
    "mutator",
    [
        lambda receipt: receipt.update(commit=OTHER_COMMIT),
        lambda receipt: receipt["git"].update(dirty=True),
        lambda receipt: receipt.update(port="COM7"),
        lambda receipt: receipt.update(baud=9600),
    ],
)
def test_all_child_receipts_require_clean_exact_source_and_transport(mutator):
    receipt = smoke_receipt()
    mutator(receipt)
    assert matrix.validate_child_receipt(
        "smoke_persistence", receipt, expected_commit=COMMIT
    )["valid"] is False


def test_receipt_safety_rejects_rf_format_raw_diag_flash_credentials_and_ports():
    receipt = common_receipt(
        results=[
            {"cmd": "mesh send dm target token", "ok": True},
            {"command": "storage format --format-confirm", "ok": True},
            {"command": "storage diag raw", "ok": True},
            {"command": "flash_d1l.py", "ok": True},
            {"password": "do-not-store"},
            {"port": "COM7"},
        ]
    )

    failures = matrix.safety_failures(receipt)
    assert "receipt_contains_mesh_send" in failures
    assert "receipt_contains_format_command" in failures
    assert "receipt_contains_raw_diag_command" in failures
    assert "receipt_contains_flash_command" in failures
    assert "receipt_contains_credential_material" in failures
    assert "receipt_claims_non_d1l_or_forbidden_port" in failures


def test_identity_receipt_fails_closed_on_wrong_port_or_commit():
    assert matrix.validate_identity_receipt(
        identity_receipt(), expected_commit=COMMIT
    )["valid"] is True

    assert matrix.validate_identity_receipt(
        identity_receipt(port="COM7"), expected_commit=COMMIT
    )["valid"] is False
    assert matrix.validate_identity_receipt(
        identity_receipt(device_build_commit=OTHER_COMMIT),
        expected_commit=COMMIT,
    )["valid"] is False


def test_child_environment_is_allowlisted_and_forwards_no_credentials():
    child = matrix.sanitized_child_environment(
        {
            "SYSTEMROOT": r"C:\Windows",
            "PATH": r"C:\Windows\System32",
            "D1L_WIFI_TARGET_SSID": "ignored-old-value",
            "WIFI_PASSWORD": "secret",
            "D1L_WIFI_PSK": "secret",
            "GH_TOKEN": "secret",
            "AWS_SECRET_ACCESS_KEY": "secret",
        },
        target_ssid="saved-profile",
    )

    assert child["D1L_WIFI_TARGET_SSID"] == "saved-profile"
    assert child["SYSTEMROOT"] == r"C:\Windows"
    assert child["PATH"] == r"C:\Windows\System32"
    assert "WIFI_PASSWORD" not in child
    assert "D1L_WIFI_PSK" not in child
    assert "GH_TOKEN" not in child
    assert "AWS_SECRET_ACCESS_KEY" not in child


def write_actions_fixture(root: Path, *, commit=COMMIT, run_id=RUN_ID):
    marker = (
        root
        / "d1l-host-artifacts"
        / "host-checks"
        / f"d1l_host_checks_success_{commit}.json"
    )
    marker.parent.mkdir(parents=True)
    marker.write_text(
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
        encoding="ascii",
    )
    manifest = (
        root
        / "d1l-release-package"
        / f"d1l-release-{commit}"
        / "manifest.json"
    )
    manifest.parent.mkdir(parents=True)
    manifest.write_text(
        json.dumps(
            {
                "schema": 1,
                "package": f"d1l-release-{commit}",
                "git": {
                    "commit": commit,
                    "dirty": False,
                    "dirty_entries": [],
                },
                "workflow": {
                    "repository": "n30nex/SIGUI",
                    "workflow": "d1l-ci",
                    "sha": commit,
                    "run_id": run_id,
                    "run_attempt": "1",
                },
            }
        ),
        encoding="ascii",
    )


def test_actions_context_binds_unique_host_marker_and_release_manifest(tmp_path):
    write_actions_fixture(tmp_path)

    result = matrix.validate_actions_context(
        tmp_path,
        expected_commit=COMMIT,
        run_id=RUN_ID,
    )

    assert result["ok"] is True
    assert len(result["host_marker_sha256"]) == 64
    assert len(result["release_manifest_sha256"]) == 64
    assert all(result["checks"].values())


def test_actions_context_rejects_predecessor_or_different_run(tmp_path):
    write_actions_fixture(tmp_path, run_id="987654321")

    result = matrix.validate_actions_context(
        tmp_path,
        expected_commit=COMMIT,
        run_id=RUN_ID,
    )

    assert result["ok"] is False
    assert "host_marker_run" in result["failures"]
    assert "release_workflow_run" in result["failures"]


def test_hardware_matrix_requires_explicit_actions_directory():
    with pytest.raises(ValueError, match="github-run-dir"):
        matrix.run_matrix(
            args(
                dry_run=False,
                github_run_dir=None,
            ),
            source_probe=lambda _root: clean_source(),
            environment={"D1L_WIFI_TARGET_SSID": "saved-profile"},
        )


def test_source_mismatch_fails_before_actions_or_serial(tmp_path):
    def must_not_run(*_args, **_kwargs):
        raise AssertionError("guard ordering opened a later stage")

    report = matrix.run_matrix(
        args(
            dry_run=False,
            hardware_dir=str(tmp_path / "hardware"),
            github_run_dir=str(tmp_path / "actions"),
        ),
        source_probe=lambda _root: clean_source(OTHER_COMMIT),
        actions_probe=must_not_run,
        identity_probe=must_not_run,
        environment={"D1L_WIFI_TARGET_SSID": "saved-profile"},
    )

    assert report["ok"] is False
    assert report["classification"] == "source_context_invalid"
    assert report["physical_observed"] is False


def test_actions_mismatch_fails_before_serial(tmp_path):
    def must_not_open_serial(**_kwargs):
        raise AssertionError("serial opened after Actions provenance failure")

    report = matrix.run_matrix(
        args(
            dry_run=False,
            hardware_dir=str(tmp_path / "hardware"),
            github_run_dir=str(tmp_path / "actions"),
        ),
        source_probe=lambda _root: clean_source(),
        actions_probe=lambda *_args, **_kwargs: {
            "ok": False,
            "failures": ["release_workflow_run"],
        },
        identity_probe=must_not_open_serial,
        environment={"D1L_WIFI_TARGET_SSID": "saved-profile"},
    )

    assert report["ok"] is False
    assert report["classification"] == "actions_context_invalid"
    assert report["physical_observed"] is False


def test_full_fake_matrix_binds_pair_preserves_receipts_and_never_forwards_secrets(
    tmp_path,
):
    captured_environments = []
    report = matrix.run_matrix(
        args(
            dry_run=False,
            hardware_dir=str(tmp_path / "hardware"),
            github_run_dir=str(tmp_path / "actions"),
        ),
        process_runner=fake_process_runner(
            child_receipts(),
            captured_environments,
        ),
        identity_probe=fake_identity,
        actions_probe=fake_actions,
        source_probe=lambda _root: clean_source(),
        environment={
            "SYSTEMROOT": r"C:\Windows",
            "PATH": r"C:\Windows\System32",
            "D1L_WIFI_TARGET_SSID": "saved-profile",
            "WIFI_PASSWORD": "never-forward",
            "GH_TOKEN": "never-forward",
        },
    )

    assert report["ok"] is True
    assert report["classification"] == "passed_feature_subset_only"
    assert report["feature_subset_complete"] is True
    assert report["release_gate_eligible"] is False
    assert report["full_feature_matrix_covered"] is False
    assert report["github_actions_run"] == RUN_ID
    assert report["expected_firmware_commit"] == COMMIT
    assert report["identity_continuity"] is True
    assert len(report["steps"]) == len(matrix.STEP_SPECS)
    assert len(report["preserved_child_receipts"]) == len(matrix.STEP_SPECS)
    assert all(step["receipt_exists"] for step in report["steps"])
    assert all(len(step["receipt_sha256"]) == 64 for step in report["steps"])
    assert all(
        step["evidence_context"]
        == {
            "expected_firmware_commit": COMMIT,
            "github_actions_run": RUN_ID,
            "port": D1L_PORT,
        }
        for step in report["steps"]
    )
    assert "never-forward" not in repr(report)
    assert captured_environments
    assert all("WIFI_PASSWORD" not in env for env in captured_environments)
    assert all("GH_TOKEN" not in env for env in captured_environments)
    assert all(
        env["D1L_WIFI_TARGET_SSID"] == "saved-profile"
        for env in captured_environments
    )


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


def test_absolute_deadline_stops_after_current_child_and_preserves_its_receipt(
    tmp_path,
):
    clock = FakeClock()

    def slow_runner(command, **_kwargs):
        receipt = Path(command[command.index("--out") + 1])
        receipt.write_text(json.dumps(smoke_receipt()), encoding="ascii")
        clock.now = 2.0
        return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

    report = matrix.run_matrix(
        args(
            dry_run=False,
            session_timeout_sec=1.0,
            hardware_dir=str(tmp_path / "hardware"),
            github_run_dir=str(tmp_path / "actions"),
        ),
        process_runner=slow_runner,
        identity_probe=fake_identity,
        actions_probe=fake_actions,
        source_probe=lambda _root: clean_source(),
        clock=clock,
        environment={"D1L_WIFI_TARGET_SSID": "saved-profile"},
    )

    assert report["ok"] is False
    assert report["deadline_exhausted"] is True
    assert len(report["steps"]) == 1
    assert report["steps"][0]["classification"] == "session_deadline_exhausted"
    assert report["steps"][0]["receipt_exists"] is True
    assert len(report["steps"][0]["receipt_sha256"]) == 64


def test_child_timeout_preserves_partial_receipt_and_fails_closed(tmp_path):
    def timeout_runner(command, **kwargs):
        receipt = Path(command[command.index("--out") + 1])
        receipt.write_text('{"partial": true}\n', encoding="ascii")
        raise subprocess.TimeoutExpired(command, kwargs["timeout"])

    report = matrix.run_matrix(
        args(
            dry_run=False,
            hardware_dir=str(tmp_path / "hardware"),
            github_run_dir=str(tmp_path / "actions"),
        ),
        process_runner=timeout_runner,
        identity_probe=fake_identity,
        actions_probe=fake_actions,
        source_probe=lambda _root: clean_source(),
        environment={"D1L_WIFI_TARGET_SSID": "saved-profile"},
    )

    assert report["ok"] is False
    assert report["child_timeout"] is True
    assert len(report["steps"]) == 1
    assert report["steps"][0]["classification"] == "child_timeout"
    assert report["steps"][0]["receipt_exists"] is True
    assert len(report["steps"][0]["receipt_sha256"]) == 64


def test_invalid_child_receipt_stops_following_children(tmp_path):
    receipts = child_receipts()
    receipts["smoke_d1l.py"]["device_build_commit"] = OTHER_COMMIT
    calls = []

    def runner(command, **kwargs):
        calls.append(Path(command[1]).name)
        return fake_process_runner(receipts)(command, **kwargs)

    report = matrix.run_matrix(
        args(
            dry_run=False,
            hardware_dir=str(tmp_path / "hardware"),
            github_run_dir=str(tmp_path / "actions"),
        ),
        process_runner=runner,
        identity_probe=fake_identity,
        actions_probe=fake_actions,
        source_probe=lambda _root: clean_source(),
        environment={"D1L_WIFI_TARGET_SSID": "saved-profile"},
    )

    assert report["ok"] is False
    assert calls == ["smoke_d1l.py"]
    assert report["steps"][0]["classification"] == "failed"
    assert "device_commit" in report["steps"][0]["validation"]["failures"]


def test_cli_has_no_credential_option_and_rejects_one_without_echoing_value(capsys):
    base = [
        "--port",
        D1L_PORT,
        "--expected-firmware-commit",
        COMMIT,
        "--github-actions-run",
        RUN_ID,
        "--dry-run",
    ]
    parsed = matrix.parse_args(base)
    assert not hasattr(parsed, "password")
    assert not hasattr(parsed, "psk")

    with pytest.raises(SystemExit):
        matrix.parse_args([*base, "--password", "secret-value"])
    assert "secret-value" not in capsys.readouterr().err
