import pytest

from scripts import core_smoke_d1l as core_smoke


COMMIT = "a" * 40


def windows_target():
    return core_smoke.resolve_core_target(
        "COM12",
        platform_name="nt",
        port_lister=lambda: [
            {
                "device": "COM12",
                "vid": 0x1A86,
                "pid": 0x7523,
                "serial_number": None,
                "hwid": "USB VID:PID=1A86:7523 LOCATION=1-2",
                "location": "1-2",
            }
        ],
    )


def test_core_smoke_requires_exact_com12():
    assert core_smoke.enforce_core_port(" com12 ") == "COM12"
    assert core_smoke.enforce_core_port(r"\\.\COM12") == "COM12"
    assert (
        core_smoke.enforce_core_port(core_smoke.D1L_CORE_POSIX_TARGET)
        == core_smoke.D1L_CORE_POSIX_TARGET
    )
    for port in (None, "COM8", "COM11", "COM16", "COM29", "COM30"):
        with pytest.raises(ValueError, match="requires COM12"):
            core_smoke.enforce_core_port(port)
    for port in ("/dev/ttyUSB2", "/dev/ttyACM0", "/dev/serial/by-id/other"):
        with pytest.raises(ValueError, match="exact"):
            core_smoke.enforce_core_port(port)


def test_core_smoke_plan_never_closes_or_transmits_public_rf():
    plan = core_smoke.command_plan("disabled")

    assert plan["ok"] is False
    assert plan["closure_eligible"] is False
    assert plan["physical_observed"] is False
    assert plan["port"] == "COM12"
    assert plan["release_profile"] == "core_1_0"
    assert plan["public_rf_tx"] is False
    assert plan["formats_sd"] is False
    assert not any(
        command.startswith("mesh send public ")
        for command in plan["supported_commands"]
    )
    assert not any(
        command.startswith(("wifi ", "ble ", "map "))
        for command in plan["supported_commands"]
    )


def test_core_smoke_mutation_plan_matches_profile_and_disabled_sd():
    conditional = core_smoke.mutation_probe_plan("conditional")
    disabled = core_smoke.mutation_probe_plan("disabled")

    assert {row["feature"] for row in conditional} == {
        "wifi_user_control",
        "ble",
        "map",
        "location",
        "multi_channel_management",
        "packets",
        "nodes",
        "user_trace",
        "admin",
        "observer_mqtt",
        "signed_update",
        "mutable_terminal",
        "advanced_qr_emoji",
    }
    assert not any(row["feature"] == "sd_history" for row in conditional)
    assert {
        "command": "packets clear",
        "feature": "packets",
    } in conditional
    assert [
        row for row in disabled if row["feature"] == "sd_history"
    ] == [
        {"command": "storage mount", "feature": "sd_history"},
        {"command": "rp2040 reset", "feature": "sd_history"},
        {
            "command": "ui scroll-probe storage_card",
            "feature": "sd_history",
        },
        {
            "command": "ui scroll-probe storage_data",
            "feature": "sd_history",
        },
    ]
    assert core_smoke.unavailable_status_probe_plan("conditional") == []
    assert core_smoke.unavailable_status_probe_plan("disabled") == [
        {"command": "storage diag", "feature": "sd_history"},
        {"command": "storage diag raw", "feature": "sd_history"},
        {"command": "storage setup", "feature": "sd_history"},
        {"command": "rp2040 status", "feature": "sd_history"},
        {"command": "rp2040 ping", "feature": "sd_history"},
        {"command": "rp2040 stock-probe", "feature": "sd_history"},
    ]


def test_core_smoke_exact_identity_and_unsupported_contract():
    identity = {
        "ok": True,
        "build_commit": COMMIT,
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
    }
    unsupported = {
        "ok": False,
        "cmd": "wifi on",
        "code": "ESP_ERR_NOT_SUPPORTED",
        "release_profile": "core_1_0",
        "feature": "wifi_user_control",
    }

    assert core_smoke.exact_identity(identity, COMMIT, "disabled")
    assert core_smoke.exact_unsupported_result(
        unsupported, "wifi on", "wifi_user_control"
    )
    assert not core_smoke.exact_identity(
        {**identity, "build_commit": "b" * 40}, COMMIT, "disabled"
    )
    assert not core_smoke.exact_unsupported_result(
        {**unsupported, "code": "ESP_OK"},
        "wifi on",
        "wifi_user_control",
    )
    unavailable = {
        **identity,
        "cmd": "rp2040 status",
        "available": False,
        "feature": "sd_history",
        "mutation_allowed": False,
        "reason": "unavailable_in_release_profile",
    }
    assert core_smoke.exact_unavailable_status_result(
        unavailable,
        "rp2040 status",
        "sd_history",
        COMMIT,
        "disabled",
    )
    assert not core_smoke.exact_unavailable_status_result(
        {**unavailable, "uart_ready": True},
        "rp2040 status",
        "sd_history",
        COMMIT,
        "disabled",
    )
    assert not core_smoke.exact_unsupported_result(
        {**unsupported, "cmd": "wifi off"},
        "wifi on",
        "wifi_user_control",
    )


def test_identity_failure_report_cannot_claim_closure():
    report = core_smoke.identity_failure_report(
        port="COM12",
        baud=115200,
        expected_commit=COMMIT,
        expected_sd_history_mode="disabled",
        version={"ok": True, "build_commit": "b" * 40},
        github_run_id="123456789",
        workflow_run_attempt="1",
        d1l_target=windows_target(),
    )

    assert report["schema"] == 2
    assert report["d1l_target"]["stable_identity_sha256"]
    assert report["ok"] is False
    assert report["closure_eligible"] is False
    assert report["identity_preflight_only"] is True
    assert report["supported_commands_executed"] == []
    assert report["unavailable_mutation_probes"] == []
    assert report["unavailable_status_probes"] == []
    assert report["public_rf_tx"] is False


def test_hardware_smoke_rejects_zero_run_before_serial_open():
    with pytest.raises(ValueError, match="positive integers"):
        core_smoke.run_core_smoke(
            port="COM12",
            baud=115200,
            timeout=1.0,
            expected_commit=COMMIT,
            expected_sd_history_mode="disabled",
            persistence_test=False,
            manual_touch=False,
            github_run_id="0",
            workflow_run_attempt="1",
        )


def test_invalid_hardware_target_fails_before_serial_open(monkeypatch):
    monkeypatch.setattr(
        core_smoke,
        "git_metadata",
        lambda _root: {
            "commit": COMMIT,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(
        core_smoke,
        "open_d1l_serial",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            AssertionError("invalid identity must fail before serial open")
        ),
    )

    with pytest.raises(ValueError, match="VID"):
        core_smoke.run_core_smoke(
            port="COM12",
            baud=115200,
            timeout=1.0,
            expected_commit=COMMIT,
            expected_sd_history_mode="disabled",
            persistence_test=False,
            manual_touch=False,
            github_run_id="123",
            workflow_run_attempt="1",
            platform_name="nt",
            port_lister=lambda: [
                {
                    "device": "COM12",
                    "vid": 0x10C4,
                    "pid": 0xEA60,
                    "serial_number": "wrong",
                    "hwid": "wrong",
                    "location": "1-9",
                }
            ],
        )


def test_pi_target_output_path_never_embeds_absolute_path():
    path = core_smoke.resolve_out_path(
        None,
        core_smoke.D1L_CORE_POSIX_TARGET,
    )
    assert "dev-serial-by-id-usb-1a86-usb-serial-if00-port0" in str(path)
    assert core_smoke.D1L_CORE_POSIX_TARGET not in str(path)
