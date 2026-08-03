import json

import pytest

from scripts import core_smoke_d1l as core_smoke


COMMIT = "a" * 40
PUBLIC_KEY = "0123456789abcdef" * 4


def identity_status(public_key=PUBLIC_KEY):
    normalized = core_smoke.exact_public_key(public_key)
    assert normalized is not None
    return {
        "schema": 1,
        "ok": True,
        "cmd": "identity status",
        "public_key_ready": True,
        "public_key": normalized,
        "fingerprint": normalized[:16].upper(),
        "role": "desk_companion",
    }


def windows_port():
    return {
        "device": "COM12",
        "vid": 0x1A86,
        "pid": 0x7523,
        "serial_number": None,
        "hwid": "USB VID:PID=1A86:7523 LOCATION=1-2",
        "location": "1-2",
    }


def windows_target():
    return core_smoke.resolve_core_target(
        "COM12",
        platform_name="nt",
        port_lister=lambda: [windows_port()],
    )


class FakeSerial:
    def __init__(self):
        self.reset_count = 0
        self.writes = []

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def reset_input_buffer(self):
        self.reset_count += 1

    def write(self, value):
        self.writes.append(bytes(value))

    def flush(self):
        return None


def install_hardware_fakes(monkeypatch, command_runner):
    fake = FakeSerial()
    monkeypatch.setattr(
        core_smoke,
        "git_metadata",
        lambda _root: {
            "commit": COMMIT,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(core_smoke.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(
        core_smoke,
        "open_d1l_serial",
        lambda *_args, **_kwargs: fake,
    )
    monkeypatch.setattr(core_smoke, "send_console_command", command_runner)
    return fake


def install_persistence_fakes(monkeypatch, *, wrong_identity_number=None):
    commands = []
    current_name = {"value": "DeskOS"}
    identity_number = {"value": 0}
    health_number = {"value": 0}
    ready_rows = iter(
        (
            persistence_health(11),
            persistence_health(12),
        )
    )

    def command_runner(_ser, command, _timeout):
        commands.append(command)
        if command == "settings get":
            return {
                "schema": 1,
                "ok": True,
                "cmd": "settings get",
                "node_name": current_name["value"],
                "path_hash_bytes": 2,
            }
        if command.startswith("settings set name "):
            current_name["value"] = command.removeprefix("settings set name ")
            return {
                "schema": 1,
                "ok": True,
                "cmd": "settings set name",
            }
        if command == "health":
            health_number["value"] += 1
            return persistence_health(9 + health_number["value"])
        if command == "reboot":
            return {
                "schema": 1,
                "ok": True,
                "cmd": "reboot",
                "rebooting": True,
                "reset_scope": "system",
                "storage_manager_quiesced": True,
                "retained_worker_quiesced": True,
                "rp2040_bridge_quiesced": True,
                "connectivity_prepare": "ESP_OK",
                "retained_flush": "ESP_OK",
                "route_flush": "ESP_OK",
            }
        if command == "identity status":
            identity_number["value"] += 1
            if identity_number["value"] == wrong_identity_number:
                return identity_status("f" * 64)
            return identity_status()
        raise AssertionError(f"unexpected persistence command: {command}")

    monkeypatch.setattr(core_smoke, "send_console_command", command_runner)
    monkeypatch.setattr(core_smoke, "wait_after_reboot", lambda *_args: None)
    monkeypatch.setattr(
        core_smoke,
        "wait_for_console_ready",
        lambda *_args: next(ready_rows),
    )
    return commands, current_name


def persistence_health(nonce):
    return {
        "schema": 1,
        "ok": True,
        "cmd": "health",
        "build_commit": COMMIT,
        "release_profile": core_smoke.CORE_RELEASE_PROFILE,
        "sd_history_mode": "disabled",
        "board_ready": True,
        "ui_ready": True,
        "boot_nonce": nonce,
        "reset_reason": "SW",
    }


def test_core_persistence_binds_full_identity_after_each_reboot(monkeypatch):
    commands, current_name = install_persistence_fakes(monkeypatch)

    report = core_smoke.run_core_persistence_check(
        object(),
        1.0,
        PUBLIC_KEY,
    )

    assert report["ok"] is True
    assert report["post_reboot_identity_status"] == identity_status()
    assert report["post_reboot_identity_ok"] is True
    assert report["cleanup_post_reboot_identity_status"] == identity_status()
    assert report["cleanup_post_reboot_identity_ok"] is True
    assert report["d1l_public_key_continuity_ok"] is True
    assert [step["command"] for step in report["steps"]] == [
        "settings get",
        "settings set name D1L Core Persist",
        "settings get",
        "health",
        "reboot",
        "health",
        "identity status",
        "settings get",
        "settings set name DeskOS",
        "settings get",
        "health",
        "reboot",
        "health",
        "identity status",
        "settings get",
    ]
    assert commands.count("identity status") == 2
    assert current_name["value"] == "DeskOS"


@pytest.mark.parametrize("wrong_identity_number", [1, 2])
def test_core_persistence_key_drift_stops_before_next_settings_operation(
    monkeypatch,
    wrong_identity_number,
):
    commands, current_name = install_persistence_fakes(
        monkeypatch,
        wrong_identity_number=wrong_identity_number,
    )

    report = core_smoke.run_core_persistence_check(
        object(),
        1.0,
        PUBLIC_KEY,
    )

    assert report["ok"] is False
    assert report["d1l_public_key_continuity_ok"] is False
    assert commands[-1] == "identity status"
    if wrong_identity_number == 1:
        assert report["post_reboot_identity_ok"] is False
        assert report["reboot_count"] == 1
        assert current_name["value"] == "D1L Core Persist"
    else:
        assert report["post_reboot_identity_ok"] is True
        assert report["cleanup_post_reboot_identity_ok"] is False
        assert report["reboot_count"] == 2
        assert current_name["value"] == "DeskOS"


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
    assert plan["preflight_commands"] == [
        "version",
        "identity status",
        "health",
    ]
    assert "identity status" not in plan["supported_commands"]
    assert not any(
        command.startswith("mesh send public ")
        for command in plan["supported_commands"]
    )
    mutation_commands = {
        command for command, _feature in core_smoke.UNAVAILABLE_MUTATION_PROBES
    }
    assert mutation_commands.isdisjoint(plan["supported_commands"])


def test_production_smoke_uses_no_qualification_only_display_or_touch_hooks():
    assert "display test" not in core_smoke.CORE_SMOKE_COMMANDS
    assert "touch test" not in core_smoke.CORE_SMOKE_COMMANDS
    assert "touch raw" in core_smoke.CORE_SMOKE_COMMANDS


def test_production_smoke_covers_core_status_surfaces():
    assert {
        "map center",
        "companion status",
        "wifi status",
        "wifi scan",
        "ble status",
        "rp2040 status",
        "storage map-policy",
        "storage setup",
        "messages unread",
        "channels",
        "contacts export",
        "roomservers",
        "repeaters",
        "admin status",
        "terminal status",
        "observer status",
        "update status",
    }.issubset(core_smoke.CORE_SMOKE_COMMANDS)


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


def test_hardware_smoke_binds_full_identity_before_health_and_supported_commands(
    monkeypatch,
):
    commands = []

    def command_runner(_ser, command, _timeout):
        commands.append(command)
        if command == "version":
            return {
                "schema": 1,
                "ok": True,
                "cmd": "version",
                "build_commit": COMMIT,
                "idf": core_smoke.EXPECTED_IDF_VERSION,
                "release_profile": core_smoke.CORE_RELEASE_PROFILE,
                "sd_history_mode": "conditional",
            }
        if command == "identity status":
            return identity_status()
        if command == "health":
            return {
                "schema": 1,
                "ok": True,
                "cmd": "health",
                "build_commit": COMMIT,
                "release_profile": core_smoke.CORE_RELEASE_PROFILE,
                "sd_history_mode": "conditional",
                "board_ready": True,
                "ui_ready": True,
            }
        if command == "crashlog":
            return {
                "schema": 1,
                "ok": True,
                "cmd": "crashlog",
                "entries": [],
            }
        return {"schema": 1, "ok": True, "cmd": command}

    fake = install_hardware_fakes(monkeypatch, command_runner)
    mutation_features = {
        row["command"]: row["feature"]
        for row in core_smoke.mutation_probe_plan("conditional")
    }

    def exact_probe(_ser, command, _timeout):
        return {
            "schema": 1,
            "ok": False,
            "cmd": command,
            "code": "ESP_ERR_NOT_SUPPORTED",
            "release_profile": core_smoke.CORE_RELEASE_PROFILE,
            "feature": mutation_features[command],
        }

    monkeypatch.setattr(core_smoke, "send_exact_console_command", exact_probe)

    report = core_smoke.run_core_smoke(
        port="COM12",
        baud=115200,
        timeout=1.0,
        expected_commit=COMMIT,
        expected_sd_history_mode="conditional",
        expected_d1l_public_key=PUBLIC_KEY,
        persistence_test=False,
        manual_touch=False,
        github_run_id="123",
        workflow_run_attempt="1",
        platform_name="nt",
        port_lister=lambda: [windows_port()],
    )

    assert report["ok"] is True
    assert fake.reset_count == 1
    assert commands == [
        "version",
        "identity status",
        "health",
        *core_smoke.CORE_SMOKE_COMMANDS,
        "health",
        "identity status",
    ]
    assert report["expected_d1l_public_key"] == PUBLIC_KEY
    assert report["d1l_identity_status"] == identity_status()
    assert report["d1l_identity_ok"] is True
    assert report["d1l_identity_status_final"] == identity_status()
    assert report["d1l_identity_final_ok"] is True
    assert report["d1l_public_key_continuity_ok"] is True
    assert report["supported_commands_executed"] == list(
        core_smoke.CORE_SMOKE_COMMANDS
    )
    assert "identity status" not in report["supported_commands_executed"]
    assert [row["cmd"] for row in report["results"][:3]] == [
        "version",
        "identity status",
        "health",
    ]


@pytest.mark.parametrize(
    "case",
    [
        "wrong-key",
        "missing-key",
        "same-prefix-key",
        "wrong-role",
        "bool-schema",
        "wrong-fingerprint",
        "not-ready",
    ],
)
def test_live_key_failure_stops_before_health_display_touch_or_persistence(
    monkeypatch,
    case,
):
    observed_identity = identity_status()
    if case == "wrong-key":
        observed_identity = identity_status("f" * 64)
    elif case == "missing-key":
        observed_identity.pop("public_key")
    elif case == "same-prefix-key":
        observed_identity = identity_status(PUBLIC_KEY[:16] + "f" * 48)
    elif case == "wrong-role":
        observed_identity["role"] = "repeater"
    elif case == "bool-schema":
        observed_identity["schema"] = True
    elif case == "wrong-fingerprint":
        observed_identity["fingerprint"] = "0" * 16
    else:
        observed_identity["public_key_ready"] = False
    commands = []

    def command_runner(_ser, command, _timeout):
        commands.append(command)
        if command == "version":
            return {
                "schema": 1,
                "ok": True,
                "cmd": "version",
                "build_commit": COMMIT,
                "idf": core_smoke.EXPECTED_IDF_VERSION,
                "release_profile": core_smoke.CORE_RELEASE_PROFILE,
                "sd_history_mode": "disabled",
            }
        if command == "identity status":
            return observed_identity
        raise AssertionError(f"unexpected command after identity failure: {command}")

    install_hardware_fakes(monkeypatch, command_runner)
    monkeypatch.setattr(
        core_smoke,
        "send_exact_console_command",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            AssertionError("mutation/status probes must not run")
        ),
    )
    monkeypatch.setattr(
        core_smoke,
        "run_core_persistence_check",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            AssertionError("persistence must not run")
        ),
    )

    report = core_smoke.run_core_smoke(
        port="COM12",
        baud=115200,
        timeout=1.0,
        expected_commit=COMMIT,
        expected_sd_history_mode="disabled",
        expected_d1l_public_key=PUBLIC_KEY,
        persistence_test=True,
        manual_touch=True,
        github_run_id="123",
        workflow_run_attempt="1",
        platform_name="nt",
        port_lister=lambda: [windows_port()],
    )

    assert commands == ["version", "identity status"]
    assert report["ok"] is False
    assert report["closure_eligible"] is False
    assert report["expected_d1l_public_key"] == PUBLIC_KEY
    assert report["d1l_identity_status"] == observed_identity
    assert report["d1l_identity_ok"] is False
    assert report["supported_commands_executed"] == []
    assert [row["cmd"] for row in report["results"]] == [
        "version",
        "identity status",
    ]


@pytest.mark.parametrize("value", [None, "", "0" * 63, "g" * 64, True])
def test_hardware_smoke_requires_full_expected_key_before_serial(
    monkeypatch,
    value,
):
    monkeypatch.setattr(
        core_smoke,
        "git_metadata",
        lambda _root: (_ for _ in ()).throw(
            AssertionError("source and serial preflight must not start")
        ),
    )
    monkeypatch.setattr(
        core_smoke,
        "open_d1l_serial",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            AssertionError("serial must not open")
        ),
    )

    with pytest.raises(ValueError, match="exact 64-hex"):
        core_smoke.run_core_smoke(
            port="COM12",
            baud=115200,
            timeout=1.0,
            expected_commit=COMMIT,
            expected_sd_history_mode="disabled",
            expected_d1l_public_key=value,
            persistence_test=False,
            manual_touch=False,
            github_run_id="123",
            workflow_run_attempt="1",
        )


def test_identity_failure_report_cannot_claim_closure():
    report = core_smoke.identity_failure_report(
        port="COM12",
        baud=115200,
        expected_commit=COMMIT,
        expected_sd_history_mode="disabled",
        expected_d1l_public_key=PUBLIC_KEY,
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
    assert report["expected_d1l_public_key"] == PUBLIC_KEY
    assert report["d1l_identity_status"] == {}
    assert report["d1l_identity_ok"] is False
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
            expected_d1l_public_key=PUBLIC_KEY,
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
            expected_d1l_public_key=PUBLIC_KEY,
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


def test_cli_dry_run_allows_omitted_key_and_never_enters_hardware(
    monkeypatch,
    capsys,
):
    monkeypatch.setattr(
        core_smoke,
        "run_core_smoke",
        lambda **_kwargs: (_ for _ in ()).throw(
            AssertionError("dry-run must not enter hardware")
        ),
    )

    rc = core_smoke.main(
        [
            "--dry-run",
            "--expected-firmware-commit",
            COMMIT,
            "--github-run-id",
            "123",
            "--github-run-attempt",
            "1",
            "--expected-sd-history-mode",
            "disabled",
        ]
    )

    report = json.loads(capsys.readouterr().out)
    assert rc == 0
    assert report["dry_run"] is True
    assert report["planning_only"] is True
    assert report["ok"] is False
    assert report["closure_eligible"] is False
    assert report["expected_d1l_public_key"] is None
    assert report["d1l_identity_status"] == {}
    assert report["d1l_identity_ok"] is None
    assert report["preflight_commands"] == [
        "version",
        "identity status",
        "health",
    ]


def test_cli_hardware_requires_expected_key_before_runner(monkeypatch):
    monkeypatch.setattr(
        core_smoke,
        "run_core_smoke",
        lambda **_kwargs: (_ for _ in ()).throw(
            AssertionError("hardware runner must not start")
        ),
    )

    with pytest.raises(SystemExit):
        core_smoke.main(
            [
                "--port",
                "COM12",
                "--expected-firmware-commit",
                COMMIT,
                "--github-run-id",
                "123",
                "--github-run-attempt",
                "1",
                "--expected-sd-history-mode",
                "disabled",
            ]
        )
