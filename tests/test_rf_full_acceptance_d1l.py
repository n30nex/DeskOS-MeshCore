import base64
import hashlib
import json
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path
from types import SimpleNamespace

import pytest

from scripts import rf_full_acceptance_d1l as rf_accept
from scripts import d1l_serial_target
from scripts.smoke_d1l import expected_command_name


class FakeSerial:
    def __init__(self):
        self.reset_count = 0

    def reset_input_buffer(self):
        self.reset_count += 1

    def __enter__(self):
        return self

    def __exit__(self, _exc_type, _exc, _traceback):
        return False


def windows_target_row(
    *,
    vid=0x1A86,
    pid=0x7523,
    location="1-2",
):
    return {
        "device": "COM12",
        "vid": vid,
        "pid": pid,
        "serial_number": None,
        "hwid": "USB VID:PID=1A86:7523",
        "location": location,
    }


def windows_target() -> dict:
    return d1l_serial_target.resolve_target(
        "COM12",
        port_lister=lambda: [windows_target_row()],
        platform_name="nt",
        hostname=lambda: "rf-test-host",
    )


def windows_target_pair() -> dict:
    before = windows_target()
    return {
        "d1l_target": before,
        "d1l_target_after": json.loads(json.dumps(before)),
    }


def test_rf_full_acceptance_dry_run_is_dm_only():
    report = rf_accept.dry_run_report(
        port="COM12",
        peer_status_path=Path("peer-status.json"),
        peer_port="COM17",
        fingerprint="0BF0A701D5AE2DB6",
        public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="rf_unit",
        send_outbound=True,
    )

    assert report["ok"] is True
    assert report["schema"] == rf_accept.RF_FULL_ACCEPTANCE_SCHEMA == 2
    assert report["hardware_required"] is False
    assert report["physical_observed"] is False
    assert report["dry_run"] is True
    assert report["dm_rf_tx"] is False
    assert report["discord_command"] == f"+dm {rf_accept.DEFAULT_D1L_PUBLIC_KEY} rf_unit_in"
    assert "mesh send dm 0BF0A701D5AE2DB6 rf_unit_out" in report["commands"]
    assert "mesh send dm 0BF0A701D5AE2DB6 rf_unit_direct" in report["commands"]
    assert not any(command.startswith("mesh send public ") for command in report["commands"])


def test_rf_full_acceptance_report_requires_real_inbound_ack_and_direct_route():
    steps = [
        {
            "command": "version",
            "result": {
                "ok": True,
                "cmd": "version",
                "build_commit": "a" * 40,
                "release_profile": "core_1_0",
                "sd_history_mode": "disabled",
                "time": {
                    "protocol_tx_ready": True,
                    "protocol_tx_block": "none",
                },
            },
        },
        {
            "command": "identity status",
            "result": {"ok": True, "cmd": "identity status", "fingerprint": "BA14729E8588E30B"},
        },
        {
            "command": "mesh send dm 0BF0A701D5AE2DB6 rf_unit_out",
            "result": {"ok": True, "cmd": "mesh send dm"},
        },
        {
            "command": "packets search rf_unit_out",
            "result": {"ok": True, "cmd": "packets search", "entries": [{"note": "rf_unit_out"}]},
        },
        {
            "command": "messages dm 0BF0A701D5AE2DB6",
            "result": {
                "ok": True,
                "cmd": "messages dm",
                "fingerprint": "0BF0A701D5AE2DB6",
                "entries": [{"direction": "rx", "text": "rf_unit_in"}],
            },
        },
        {
            "command": "mesh send dm 0BF0A701D5AE2DB6 rf_unit_direct",
            "result": {"ok": True, "cmd": "mesh send dm"},
        },
        {
            "command": "packets search rf_unit_direct",
            "result": {"ok": True, "cmd": "packets search", "entries": [{"note": "rf_unit_direct"}]},
        },
        {
            "command": "messages dm 0BF0A701D5AE2DB6",
            "result": {
                "ok": True,
                "cmd": "messages dm",
                "fingerprint": "0BF0A701D5AE2DB6",
                "entries": [
                    {
                        "direction": "tx",
                        "text": "rf_unit_out",
                        "acked": True,
                        "ack_hash": 4815162342,
                    },
                    {"direction": "rx", "text": "rf_unit_in"},
                    {"direction": "tx", "text": "rf_unit_direct"},
                ],
            },
        },
        {
            "command": "packets",
            "result": {
                "ok": True,
                "cmd": "packets",
                "entries": [
                    {"kind": "dm_ack", "note": "ack matched"},
                    {"kind": "path_return", "note": "path from peer"},
                ],
            },
        },
        {
            "command": "routes trace 0BF0A701D5AE2DB6",
            "result": {
                "ok": True,
                "cmd": "routes trace",
                "fingerprint": "0BF0A701D5AE2DB6",
                "best_route": "direct",
                "entries": [
                    {
                        "target": "0BF0A701D5AE2DB6",
                        "kind": "dm_text",
                        "direction": "tx",
                        "route": "direct",
                    }
                ],
            },
        },
        {"command": "health", "result": {"ok": True, "cmd": "health", "board_ready": True, "ui_ready": True}},
    ]
    peer = {
        "serial": {"active_port": "COM17", "meshcore_connected": True},
        "discord": {"connected": True},
    }

    report = rf_accept.build_report(
        port="COM12",
        **windows_target_pair(),
        baud=115200,
        peer_status_path=Path("status.json"),
        peer_port="COM17",
        fingerprint="0BF0A701D5AE2DB6",
        public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="rf_unit",
        send_outbound=True,
        steps=steps,
        peer_before=peer,
        peer_after=peer,
        inbound_seen_at="2026-07-01T00:00:00+00:00",
        expected_commit="a" * 40,
    )

    assert report["ok"] is True
    assert report["schema"] == rf_accept.RF_FULL_ACCEPTANCE_SCHEMA == 2
    assert report["hardware_required"] is True
    assert report["physical_observed"] is True
    assert report["dry_run"] is False
    assert report["dm_rf_tx"] is True
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert report["controlled_peer"]["evidence_source"] == "explicit_peer_status"
    assert report["controlled_peer"]["port"] == "COM17"
    assert report["checks"]["identity_public_key_matches"] is True
    assert report["checks"]["controlled_peer_observed"] is True
    assert report["checks"]["inbound_dm"] is True
    assert report["checks"]["ack_path"] is True
    assert report["checks"]["direct_route"] is True
    assert report["checks"]["no_public_commands"] is True
    assert report["checks"]["protocol_tx_ready_before_rf"] is True
    assert report["checks"]["exact_candidate"] is True
    assert report["device_release_profile"] == "core_1_0"
    assert report["device_sd_history_mode"] == "disabled"

    d1l_observed = rf_accept.build_report(
        port="COM12",
        **windows_target_pair(),
        baud=115200,
        peer_status_path=None,
        peer_port=None,
        fingerprint="0BF0A701D5AE2DB6",
        public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="rf_unit",
        send_outbound=True,
        steps=steps,
        peer_before=None,
        peer_after=None,
        inbound_seen_at="2026-07-01T00:00:00+00:00",
        expected_commit="a" * 40,
    )

    assert d1l_observed["ok"] is True
    assert d1l_observed["controlled_peer"] == {
        "fingerprint": "0BF0A701D5AE2DB6",
        "evidence_source": "d1l_bidirectional_rf",
        "port": None,
        "status_path": None,
    }


def test_rf_full_acceptance_rejects_missing_inbound_token():
    peer = {
        "serial": {"active_port": "COM17", "meshcore_connected": True},
        "discord": {"connected": True},
    }
    report = rf_accept.build_report(
        port="COM12",
        **windows_target_pair(),
        baud=115200,
        peer_status_path=Path("status.json"),
        peer_port="COM17",
        fingerprint="0BF0A701D5AE2DB6",
        public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="rf_unit",
        send_outbound=False,
        steps=[
            {"command": "identity status", "result": {"ok": True, "fingerprint": "BA14729E8588E30B"}},
            {"command": "messages dm 0BF0A701D5AE2DB6", "result": {"ok": True, "entries": []}},
            {"command": "packets", "result": {"ok": True, "entries": [{"kind": "dm_ack"}]}},
            {
                "command": "routes trace 0BF0A701D5AE2DB6",
                "result": {
                    "ok": True,
                    "fingerprint": "0BF0A701D5AE2DB6",
                    "entries": [
                        {
                            "target": "0BF0A701D5AE2DB6",
                            "kind": "dm_text",
                            "direction": "tx",
                            "route": "direct",
                        }
                    ],
                },
            },
            {"command": "health", "result": {"ok": True, "board_ready": True, "ui_ready": True}},
        ],
        peer_before=peer,
        peer_after=peer,
        inbound_seen_at=None,
    )

    assert report["ok"] is False
    assert report["checks"]["inbound_dm"] is False
    assert report["checks"]["ack_path"] is False
    assert report["checks"]["direct_route"] is False


def test_rf_full_acceptance_rejects_stale_packet_ack_without_tx_ack():
    peer = {
        "serial": {"active_port": "COM17", "meshcore_connected": True},
        "discord": {"connected": True},
    }
    report = rf_accept.build_report(
        port="COM12",
        **windows_target_pair(),
        baud=115200,
        peer_status_path=Path("status.json"),
        peer_port="COM17",
        fingerprint="0BF0A701D5AE2DB6",
        public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="rf_unit",
        send_outbound=True,
        steps=[
            {"command": "identity status", "result": {"ok": True, "fingerprint": "BA14729E8588E30B"}},
            {"command": "mesh send dm 0BF0A701D5AE2DB6 rf_unit_out", "result": {"ok": True}},
            {"command": "packets search rf_unit_out", "result": {"ok": True, "entries": [{"note": "rf_unit_out"}]}},
            {"command": "mesh send dm 0BF0A701D5AE2DB6 rf_unit_direct", "result": {"ok": True}},
            {
                "command": "messages dm 0BF0A701D5AE2DB6",
                "result": {
                    "ok": True,
                    "fingerprint": "0BF0A701D5AE2DB6",
                    "entries": [
                        {"direction": "tx", "text": "rf_unit_out", "acked": False, "ack_hash": 0},
                        {"direction": "rx", "text": "rf_unit_in"},
                        {"direction": "tx", "text": "rf_unit_direct"},
                    ],
                },
            },
            {"command": "packets", "result": {"ok": True, "entries": [{"kind": "dm_ack"}]}},
            {
                "command": "routes trace 0BF0A701D5AE2DB6",
                "result": {
                    "ok": True,
                    "fingerprint": "0BF0A701D5AE2DB6",
                    "entries": [
                        {
                            "target": "0BF0A701D5AE2DB6",
                            "kind": "dm_text",
                            "direction": "tx",
                            "route": "direct",
                        }
                    ],
                },
            },
            {"command": "health", "result": {"ok": True, "board_ready": True, "ui_ready": True}},
        ],
        peer_before=peer,
        peer_after=peer,
        inbound_seen_at="2026-07-01T00:00:00+00:00",
    )

    assert report["ok"] is False
    assert report["checks"]["inbound_dm"] is True
    assert report["checks"]["ack_path"] is False
    assert report["checks"]["direct_route"] is True


def test_rf_full_acceptance_accepts_truncated_ack_kind_when_tx_is_acked():
    peer = {
        "serial": {"active_port": "COM17", "meshcore_connected": True},
        "discord": {"connected": True},
    }
    report = rf_accept.build_report(
        port="COM12",
        **windows_target_pair(),
        baud=115200,
        peer_status_path=Path("status.json"),
        peer_port="COM17",
        fingerprint="0BF0A701D5AE2DB6",
        public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="rf_unit",
        send_outbound=True,
        steps=[
            {"command": "identity status", "result": {"ok": True, "fingerprint": "BA14729E8588E30B"}},
            {"command": "mesh send dm 0BF0A701D5AE2DB6 rf_unit_out", "result": {"ok": True}},
            {"command": "packets search rf_unit_out", "result": {"ok": True, "entries": [{"note": "rf_unit_out"}]}},
            {"command": "mesh send dm 0BF0A701D5AE2DB6 rf_unit_direct", "result": {"ok": True}},
            {
                "command": "messages dm 0BF0A701D5AE2DB6",
                "result": {
                    "ok": True,
                    "fingerprint": "0BF0A701D5AE2DB6",
                    "entries": [
                        {
                            "direction": "tx",
                            "text": "rf_unit_out",
                            "acked": True,
                            "ack_hash": 4815162342,
                        },
                        {"direction": "rx", "text": "rf_unit_in"},
                        {"direction": "tx", "text": "rf_unit_direct"},
                    ],
                },
            },
            {"command": "packets", "result": {"ok": True, "entries": [{"kind": "dm_ack_unmatche"}]}},
            {
                "command": "routes trace 0BF0A701D5AE2DB6",
                "result": {
                    "ok": True,
                    "fingerprint": "0BF0A701D5AE2DB6",
                    "entries": [
                        {
                            "target": "0BF0A701D5AE2DB6",
                            "kind": "dm_text",
                            "direction": "tx",
                            "route": "direct",
                        }
                    ],
                },
            },
            {"command": "health", "result": {"ok": True, "board_ready": True, "ui_ready": True}},
        ],
        peer_before=peer,
        peer_after=peer,
        inbound_seen_at="2026-07-01T00:00:00+00:00",
    )

    assert report["ok"] is True
    assert report["checks"]["ack_path"] is True


def test_rf_full_acceptance_prefixes_match_console_response_names():
    assert expected_command_name("mesh send dm 0BF0A701D5AE2DB6 hello") == "mesh send dm"
    assert expected_command_name("messages dm 0BF0A701D5AE2DB6") == "messages dm"


def test_rf_full_acceptance_retries_read_only_timeout(monkeypatch):
    responses = iter([
        {"ok": False, "cmd": "identity status", "code": "TIMEOUT"},
        {"ok": True, "cmd": "identity status", "fingerprint": "BA14729E8588E30B"},
    ])
    calls = []

    def fake_send(_ser, command, _timeout):
        calls.append(command)
        return next(responses)

    monkeypatch.setattr(rf_accept, "send_console_command", fake_send)
    monkeypatch.setattr(rf_accept.time, "sleep", lambda _seconds: None)
    ser = FakeSerial()

    result = rf_accept.send_acceptance_command(ser, "identity status", 1.0)

    assert result["ok"] is True
    assert calls == ["identity status", "identity status"]
    assert ser.reset_count == 1


def test_rf_full_acceptance_does_not_retry_dm_send_timeout(monkeypatch):
    calls = []

    def fake_send(_ser, command, _timeout):
        calls.append(command)
        return {"ok": False, "cmd": "mesh send dm", "code": "TIMEOUT"}

    monkeypatch.setattr(rf_accept, "send_console_command", fake_send)
    ser = FakeSerial()

    result = rf_accept.send_acceptance_command(ser, "mesh send dm 0BF0A701D5AE2DB6 token", 1.0)

    assert result["ok"] is False
    assert calls == ["mesh send dm 0BF0A701D5AE2DB6 token"]
    assert ser.reset_count == 0
    assert expected_command_name("messages dm 0BF0A701D5AE2DB6") == "messages dm"
    assert expected_command_name("packets search rf_unit") == "packets search"


@pytest.mark.parametrize(
    "port",
    ["COM8", " com11 ", r"\\.\COM29", "//?/COM11"],
)
def test_rf_full_acceptance_rejects_forbidden_d1l_or_peer_ports(port):
    with pytest.raises(ValueError, match="forbidden"):
        rf_accept.enforce_port_policy(port)
    with pytest.raises(ValueError, match="forbidden"):
        rf_accept.enforce_port_policy("COM12", port)


def test_rf_full_acceptance_rejects_non_serial_peer_port():
    with pytest.raises(ValueError, match="invalid controlled-peer port"):
        rf_accept.enforce_port_policy("COM12", "COM_DISABLED")


@pytest.mark.parametrize("port", ["COM7", "COM13", "COM16", "COM30"])
def test_rf_full_acceptance_requires_com12_for_d1l(port):
    with pytest.raises(ValueError, match="requires COM12"):
        rf_accept.enforce_port_policy(port)


def test_rf_full_acceptance_accepts_only_the_exact_posix_by_id_target():
    assert rf_accept.enforce_port_policy(
        d1l_serial_target.POSIX_D1L_TARGET
    ) == (d1l_serial_target.POSIX_D1L_TARGET, None)
    with pytest.raises(ValueError, match="requires COM12"):
        rf_accept.enforce_port_policy("/dev/ttyUSB2")


def test_rf_full_acceptance_rejects_com16_as_rf_peer():
    with pytest.raises(ValueError, match="controlled-peer port COM16"):
        rf_accept.enforce_port_policy("COM12", "COM16")


def test_rf_full_acceptance_exact_device_commit_check():
    expected = "a" * 40
    assert rf_accept.firmware_identity_matches(
        {"ok": True, "build_commit": expected}, expected
    )
    assert not rf_accept.firmware_identity_matches(
        {"ok": True, "build_commit": "b" * 40}, expected
    )
    assert not rf_accept.firmware_identity_matches(
        {"ok": True, "build_commit": "abc123"}, expected
    )


@pytest.mark.parametrize(
    ("ready", "block"),
    [
        (False, "none"),
        (True, "legacy_protocol_lower_bound_unconfirmed"),
    ],
)
def test_rf_full_acceptance_rejects_protocol_tx_not_ready(
    ready, block
):
    version = {
        "ok": True,
        "build_commit": "a" * 40,
        "time": {
            "protocol_tx_ready": ready,
            "protocol_tx_block": block,
        },
    }

    assert not rf_accept.protocol_tx_ready_for_rf(version)


def test_rf_report_cannot_use_later_ready_version_to_override_first_block():
    blocked = {
        "ok": True,
        "cmd": "version",
        "build_commit": "a" * 40,
        "time": {
            "protocol_tx_ready": False,
            "protocol_tx_block": (
                "legacy_protocol_lower_bound_unconfirmed"
            ),
        },
    }
    later_ready = {
        **blocked,
        "time": {
            "protocol_tx_ready": True,
            "protocol_tx_block": "none",
        },
    }
    report = rf_accept.build_report(
        port="COM12",
        **windows_target_pair(),
        baud=115200,
        peer_status_path=None,
        peer_port=None,
        fingerprint="0BF0A701D5AE2DB6",
        public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="first_version",
        send_outbound=False,
        steps=[
            {"command": "version", "result": blocked},
            {"command": "version", "result": later_ready},
        ],
        peer_before=None,
        peer_after=None,
        inbound_seen_at=None,
        expected_commit="a" * 40,
    )

    assert report["checks"]["protocol_tx_ready_before_rf"] is False
    assert report["ok"] is False


@pytest.mark.parametrize(
    ("ready", "block"),
    [
        (False, "none"),
        (True, "legacy_protocol_lower_bound_unconfirmed"),
    ],
)
def test_rf_hardware_preflight_stops_before_any_rf_when_protocol_blocked(
    tmp_path, monkeypatch, ready, block
):
    scripts_dir = tmp_path / "scripts"
    scripts_dir.mkdir()
    monkeypatch.setattr(
        rf_accept,
        "__file__",
        str(scripts_dir / "rf_full_acceptance_d1l.py"),
    )
    monkeypatch.setitem(sys.modules, "serial", SimpleNamespace())
    monkeypatch.setattr(
        rf_accept,
        "git_metadata",
        lambda _root: {
            "commit": "a" * 40,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(
        rf_accept,
        "open_d1l_serial",
        lambda *_args, **_kwargs: FakeSerial(),
    )
    monkeypatch.setattr(rf_accept.time, "sleep", lambda _seconds: None)
    commands = []

    def fake_send(_ser, command, _timeout):
        commands.append(command)
        assert command == "version"
        return {
            "ok": True,
            "cmd": "version",
            "build_commit": "a" * 40,
            "idf": "v5.5.4",
            "release_profile": "core_1_0",
            "sd_history_mode": "disabled",
            "time": {
                "protocol_tx_ready": ready,
                "protocol_tx_block": block,
            },
        }

    monkeypatch.setattr(
        rf_accept, "send_acceptance_command", fake_send
    )
    report = rf_accept.run_hardware(
        port="COM12",
        baud=115200,
        timeout=1.0,
        wait_sec=1.0,
        poll_sec=0.1,
        peer_status_path=tmp_path / "peer-status.json",
        peer_port="COM17",
        fingerprint="0BF0A701D5AE2DB6",
        public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="protocol_blocked",
        send_outbound=True,
        expected_commit="a" * 40,
        github_run_id="1",
        workflow_run_attempt="1",
        peer_capture_dir=tmp_path / "rf-peer",
        port_lister=lambda: [windows_target_row()],
        platform_name="nt",
    )

    assert commands == ["version"]
    assert report["ok"] is False
    assert report["dm_rf_tx"] is False
    assert report["public_rf_tx"] is False
    assert report["checks"]["protocol_tx_ready_before_rf"] is False
    assert report["d1l_target"]["requested_path"] == "COM12"
    assert report["d1l_target_after"]["requested_path"] == "COM12"
    assert report["target_identity_continuity_ok"] is True
    assert (
        report["checks"]["d1l_target_identity_continuity"] is True
    )


@pytest.mark.parametrize(
    ("row_kwargs", "error"),
    [
        ({"vid": 0x10C4}, "VID must be 0x1A86"),
        ({"pid": 0x55D4}, "PID must be 0x7523"),
    ],
)
def test_rf_rejects_wrong_usb_identity_before_serial_or_peer(
    tmp_path,
    monkeypatch,
    row_kwargs,
    error,
):
    scripts_dir = tmp_path / "scripts"
    scripts_dir.mkdir()
    monkeypatch.setattr(
        rf_accept,
        "__file__",
        str(scripts_dir / "rf_full_acceptance_d1l.py"),
    )
    monkeypatch.setitem(sys.modules, "serial", SimpleNamespace())
    monkeypatch.setattr(
        rf_accept,
        "git_metadata",
        lambda _root: {
            "commit": "a" * 40,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    external_calls = []

    def unexpected_external(*_args, **_kwargs):
        external_calls.append(True)
        raise AssertionError(
            "wrong USB identity must fail before serial or peer I/O"
        )

    monkeypatch.setattr(
        rf_accept, "open_d1l_serial", unexpected_external
    )
    monkeypatch.setattr(
        rf_accept, "run_remote_peer_operation", unexpected_external
    )

    with pytest.raises(ValueError, match=error):
        rf_accept.run_hardware(
            port="COM12",
            baud=115200,
            timeout=1.0,
            wait_sec=1.0,
            poll_sec=0.1,
            peer_status_path=None,
            peer_port=None,
            fingerprint=rf_accept.REMOTE_PEER_FINGERPRINT,
            public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
            token="wrong_usb",
            send_outbound=True,
            expected_commit="a" * 40,
            github_run_id="1",
            workflow_run_attempt="1",
            remote_peer=remote_config(),
            port_lister=lambda: [windows_target_row(**row_kwargs)],
            platform_name="nt",
        )

    assert external_calls == []


def test_rf_rejects_target_drift_before_peer_capture(
    tmp_path,
    monkeypatch,
):
    scripts_dir = tmp_path / "scripts"
    scripts_dir.mkdir()
    monkeypatch.setattr(
        rf_accept,
        "__file__",
        str(scripts_dir / "rf_full_acceptance_d1l.py"),
    )
    monkeypatch.setitem(sys.modules, "serial", SimpleNamespace())
    monkeypatch.setattr(
        rf_accept,
        "git_metadata",
        lambda _root: {
            "commit": "a" * 40,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(
        rf_accept,
        "open_d1l_serial",
        lambda *_args, **_kwargs: FakeSerial(),
    )
    monkeypatch.setattr(rf_accept.time, "sleep", lambda _seconds: None)
    peer_calls = []

    def unexpected_peer(*_args, **_kwargs):
        peer_calls.append(True)
        raise AssertionError("target drift must stop before peer capture")

    monkeypatch.setattr(
        rf_accept, "capture_peer_status", unexpected_peer
    )
    monkeypatch.setattr(
        rf_accept,
        "send_acceptance_command",
        lambda _ser, _command, _timeout: {
            "ok": True,
            "cmd": "version",
            "build_commit": "a" * 40,
            "idf": "v5.5.4",
            "release_profile": "core_1_0",
            "sd_history_mode": "disabled",
            "time": {
                "protocol_tx_ready": False,
                "protocol_tx_block": (
                    "legacy_protocol_lower_bound_unconfirmed"
                ),
            },
        },
    )
    rows = iter(
        [
            [windows_target_row(location="1-2")],
            [windows_target_row(location="1-3")],
        ]
    )

    with pytest.raises(
        ValueError,
        match="identity changed during RF preflight",
    ):
        rf_accept.run_hardware(
            port="COM12",
            baud=115200,
            timeout=1.0,
            wait_sec=1.0,
            poll_sec=0.1,
            peer_status_path=tmp_path / "peer-status.json",
            peer_port="COM17",
            fingerprint="0BF0A701D5AE2DB6",
            public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
            token="target_drift",
            send_outbound=True,
            expected_commit="a" * 40,
            github_run_id="1",
            workflow_run_attempt="1",
            peer_capture_dir=tmp_path / "rf-peer",
            port_lister=lambda: next(rows),
            platform_name="nt",
        )

    assert peer_calls == []


def posix_target(resolved_tty: str) -> dict:
    requested = d1l_serial_target.POSIX_D1L_TARGET
    return d1l_serial_target.resolve_target(
        requested,
        port_lister=lambda: [
            {
                "device": resolved_tty,
                "vid": 0x1A86,
                "pid": 0x7523,
                "serial_number": None,
                "hwid": "USB VID:PID=1A86:7523",
                "location": "1-2",
            }
        ],
        platform_name="posix",
        exists=lambda path: path in {requested, resolved_tty},
        is_symlink=lambda path: path == requested,
        realpath=lambda path: (
            resolved_tty if path == requested else path
        ),
        access=lambda _path, _mode: True,
        hostname=lambda: "neopi5",
    )


def test_rf_opens_stable_posix_path_and_allows_tty_renumber(
    tmp_path,
    monkeypatch,
):
    scripts_dir = tmp_path / "scripts"
    scripts_dir.mkdir()
    monkeypatch.setattr(
        rf_accept,
        "__file__",
        str(scripts_dir / "rf_full_acceptance_d1l.py"),
    )
    monkeypatch.setitem(sys.modules, "serial", SimpleNamespace())
    monkeypatch.setattr(
        rf_accept,
        "git_metadata",
        lambda _root: {
            "commit": "a" * 40,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    snapshots = iter(
        [posix_target("/dev/ttyUSB2"), posix_target("/dev/ttyUSB7")]
    )
    monkeypatch.setattr(
        rf_accept,
        "resolve_core_target",
        lambda *_args, **_kwargs: next(snapshots),
    )
    opened = []
    monkeypatch.setattr(
        rf_accept,
        "open_d1l_serial",
        lambda *_args, **kwargs: (
            opened.append(kwargs["port"]) or FakeSerial()
        ),
    )
    monkeypatch.setattr(rf_accept.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(
        rf_accept,
        "send_acceptance_command",
        lambda _ser, _command, _timeout: {
            "ok": True,
            "cmd": "version",
            "build_commit": "a" * 40,
            "idf": "v5.5.4",
            "release_profile": "core_1_0",
            "sd_history_mode": "disabled",
            "time": {
                "protocol_tx_ready": False,
                "protocol_tx_block": (
                    "legacy_protocol_lower_bound_unconfirmed"
                ),
            },
        },
    )

    report = rf_accept.run_hardware(
        port=d1l_serial_target.POSIX_D1L_TARGET,
        baud=115200,
        timeout=1.0,
        wait_sec=1.0,
        poll_sec=0.1,
        peer_status_path=tmp_path / "peer-status.json",
        peer_port="COM17",
        fingerprint="0BF0A701D5AE2DB6",
        public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="tty_renumber",
        send_outbound=True,
        expected_commit="a" * 40,
        github_run_id="1",
        workflow_run_attempt="1",
        peer_capture_dir=tmp_path / "rf-peer",
        port_lister=lambda: [],
        platform_name="posix",
    )

    assert opened == [d1l_serial_target.POSIX_D1L_TARGET]
    assert report["port"] == d1l_serial_target.POSIX_D1L_TARGET
    assert report["d1l_target"]["resolved_tty"] == "/dev/ttyUSB2"
    assert report["d1l_target_after"]["resolved_tty"] == "/dev/ttyUSB7"
    assert report["target_identity_continuity_ok"] is True


def test_rf_full_acceptance_dry_run_cannot_close_identity():
    report = rf_accept.dry_run_report(
        port="COM12",
        peer_status_path=Path("peer-status.json"),
        peer_port="COM17",
        fingerprint="0BF0A701D5AE2DB6",
        public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="rf_unit",
        send_outbound=True,
        expected_commit="a" * 40,
    )

    assert report["ok"] is True
    assert report["execution_complete"] is False
    assert report["closure_eligible"] is False
    assert report["firmware_identity_required"] is True
    assert report["firmware_identity_ok"] is False


def test_openclaw_sender_is_exact_12_hex_d1l_key_prefix():
    public_key = rf_accept.DEFAULT_D1L_PUBLIC_KEY
    prefix = public_key[:12].upper()
    assert rf_accept.listener_sender_matches(
        {"mesh": {"last_rx_sender": prefix}}, public_key
    )
    assert not rf_accept.listener_sender_matches(
        {"mesh": {"last_rx_sender": public_key[:16].upper()}},
        public_key,
    )
    assert not rf_accept.listener_sender_matches(
        {"mesh": {"last_rx_sender": "0" * 12}}, public_key
    )


def test_listener_contact_import_requires_exact_key_and_canonical_chat():
    peer_key = "0123456789abcdef" * 4
    fingerprint = "0123456789ABCDEF"
    result = {
        "ok": True,
        "cmd": "contacts import",
        "persisted": True,
        "result": "created",
        "verification_source": "uri_import",
        "fingerprint": fingerprint,
        "public_key": peer_key,
        "alias": "CoreTestPeer",
        "type": "chat",
        "canonical": True,
        "can_dm": True,
        "can_admin": False,
    }
    assert rf_accept.contact_import_ok(
        result, peer_key, fingerprint
    )
    assert not rf_accept.contact_import_ok(
        {**result, "public_key": "f" * 64},
        peer_key,
        fingerprint,
    )


def test_listener_transaction_correlates_new_token_hash_packet_and_route():
    fingerprint = "0123456789ABCDEF"
    token = "rf_exact_out"
    baseline_messages = {
        "fingerprint": fingerprint,
        "entries": [
            {
                "seq": 1,
                "direction": "tx",
                "text": "older message",
            }
        ],
    }
    final_messages = {
        "fingerprint": fingerprint,
        "entries": [
            *baseline_messages["entries"],
            {
                "seq": 2,
                "fingerprint": fingerprint,
                "direction": "tx",
                "text": f"core acceptance test {token}",
                "acked": True,
                "delivered": True,
                "ack_hash": 1234567890,
                "ack_response": {
                    "identity_valid": False,
                    "state": "legacy_unverified",
                    "dispatch_count": 0,
                    "last_kind": "none",
                    "last_error": "ESP_OK",
                },
            },
            {
                "seq": 3,
                "fingerprint": fingerprint,
                "direction": "rx",
                "text": rf_accept.RADIO_LISTENER_REPLY,
                "ack_response": {
                    "identity_valid": True,
                    "state": "sent",
                    "dispatch_count": 1,
                    "last_kind": "direct_ack",
                    "last_error": "ESP_OK",
                },
            },
        ],
    }
    baseline_packets = {
        "entries": [
            {
                "seq": 10,
                "direction": "rx",
                "kind": "dm_ack",
                "note": "ack 1234567890 stale",
            }
        ]
    }
    final_packets = {
        "entries": [
            *baseline_packets["entries"],
            {
                "seq": 11,
                "direction": "rx",
                "kind": "dm_ack",
                "note": "ack 1234567890 CoreTestPeer",
                "rssi_dbm": -70,
                "snr_tenths": 80,
                "path_hash_bytes": 1,
                "path_hops": 0,
                "payload_len": 12,
            },
        ]
    }
    baseline_route = {
        "entries": [
            {
                "seq": 20,
                "target": fingerprint,
                "kind": "dm_ack",
                "direction": "rx",
                "route": "direct",
            }
        ]
    }
    final_route = {
        "entries": [
            {
                "seq": 21,
                "target": fingerprint,
                "kind": "dm_ack",
                "direction": "rx",
                "route": "direct",
                "last_rssi_dbm": -70,
                "last_snr_tenths": 80,
                "path_hash_bytes": 1,
                "path_hops": 0,
                "payload_len": 12,
            }
        ]
    }

    result = rf_accept.correlated_listener_transaction(
        baseline_messages=baseline_messages,
        final_messages=final_messages,
        baseline_packets=baseline_packets,
        final_packets=final_packets,
        baseline_route=baseline_route,
        final_route=final_route,
        outbound_token=token,
        fingerprint=fingerprint,
    )

    assert result == {
        "ok": True,
        "outbound_dm_seq": 2,
        "inbound_reply_seq": 3,
        "ack_hash": 1234567890,
        "reply_ack_state": "sent",
        "reply_ack_kind": "direct_ack",
        "packet_seq": 11,
        "route_seq": 21,
        "packet_route_metadata_match": True,
        "ack_path_ok": True,
        "direct_route_ok": True,
    }

    stale_only = rf_accept.correlated_listener_transaction(
        baseline_messages=baseline_messages,
        final_messages=final_messages,
        baseline_packets=baseline_packets,
        final_packets=baseline_packets,
        baseline_route=baseline_route,
        final_route=baseline_route,
        outbound_token=token,
        fingerprint=fingerprint,
    )
    assert stale_only["ok"] is False
    wrong_hash = json.loads(json.dumps(final_packets))
    wrong_hash["entries"][-1]["note"] = "ack 7 CoreTestPeer"
    assert rf_accept.correlated_listener_transaction(
        baseline_messages=baseline_messages,
        final_messages=final_messages,
        baseline_packets=baseline_packets,
        final_packets=wrong_hash,
        baseline_route=baseline_route,
        final_route=final_route,
        outbound_token=token,
        fingerprint=fingerprint,
    )["ok"] is False

    copied_packet = json.loads(json.dumps(final_packets))
    duplicate = dict(copied_packet["entries"][-1])
    duplicate["seq"] = 12
    copied_packet["entries"].append(duplicate)
    assert rf_accept.correlated_listener_transaction(
        baseline_messages=baseline_messages,
        final_messages=final_messages,
        baseline_packets=baseline_packets,
        final_packets=copied_packet,
        baseline_route=baseline_route,
        final_route=final_route,
        outbound_token=token,
        fingerprint=fingerprint,
    )["ok"] is False

    tampered_route = json.loads(json.dumps(final_route))
    tampered_route["entries"][-1]["last_rssi_dbm"] = -71
    assert rf_accept.correlated_listener_transaction(
        baseline_messages=baseline_messages,
        final_messages=final_messages,
        baseline_packets=baseline_packets,
        final_packets=final_packets,
        baseline_route=baseline_route,
        final_route=tampered_route,
        outbound_token=token,
        fingerprint=fingerprint,
    )["ok"] is False

    stale_sequence = json.loads(json.dumps(final_packets))
    stale_sequence["entries"][-1]["seq"] = 9
    assert rf_accept.correlated_listener_transaction(
        baseline_messages=baseline_messages,
        final_messages=final_messages,
        baseline_packets=baseline_packets,
        final_packets=stale_sequence,
        baseline_route=baseline_route,
        final_route=final_route,
        outbound_token=token,
        fingerprint=fingerprint,
    )["ok"] is False


def remote_status(
    observed: datetime,
    *,
    status_age_sec: int = 5,
    fetch_age_sec: int = 4,
    device: str = rf_accept.REMOTE_PEER_DEVICE,
    public_key: str = rf_accept.REMOTE_PEER_PUBLIC_KEY,
) -> dict:
    return {
        "service": "openclaw-radio-listener",
        "run_id": "pi5-peer-run",
        "status_written_at": (
            observed - timedelta(seconds=status_age_sec)
        ).isoformat(),
        "serial": {
            "port": device,
            "mesh_connected": True,
            "self_prefix": public_key[:12],
            "public_key": public_key,
        },
        "mesh": {
            "last_fetch_ok_at": (
                observed - timedelta(seconds=fetch_age_sec)
            ).isoformat(),
            "last_rx_at": "before-rx",
            "last_rx_kind": "dm",
            "last_rx_sender": rf_accept.DEFAULT_D1L_PUBLIC_KEY[:12],
            "last_tx_at": "before-tx",
            "last_tx_kind": "control_dm",
        },
        "startup_self_test": {
            "enabled": True,
            "ok": True,
        },
        "counters": {
            "rx_dm_total": 10,
            "tx_dm_total": 20,
            "local_fast_reply_total": 4,
            "tx_dm_ack_miss_total": 1,
        },
    }


def remote_config() -> dict:
    return rf_accept.remote_peer_config(
        ssh_host="neonx@192.168.0.24"
    )


def remote_control_response(
    target: str,
    token: str,
    *,
    acknowledged: bool = True,
    cached: bool = False,
) -> tuple[bytes, bytes]:
    request, request_raw = rf_accept.remote_control_request(
        target, token
    )
    response = {
        "id": request["id"],
        "op": "radio.send_dm",
        "ok": True,
        "cached": cached,
        "duration_ms": 123,
        "result": {
            "target": target[:12],
            "name": "D1L",
            "utf8_bytes": len(token.encode("utf-8")),
            "delivery": {
                "event": "CONTACT_MSG_RECV"
                if acknowledged
                else None,
                "payload": {"ack": True}
                if acknowledged
                else None,
                "acknowledged": acknowledged,
            },
        },
        "error": None,
    }
    response_raw = (
        json.dumps(
            response,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        + b"\n"
    )
    return request_raw, response_raw


def test_remote_peer_status_requires_fresh_exact_safe_identity():
    observed = datetime(2026, 7, 23, 15, 0, tzinfo=timezone.utc)
    status = remote_status(observed)

    validation = rf_accept.validate_remote_peer_status(
        status,
        remote_config(),
        observed_at=observed,
    )

    assert validation["ok"] is True
    assert all(validation["checks"].values())

    stale = remote_status(observed, status_age_sec=121)
    assert rf_accept.validate_remote_peer_status(
        stale,
        remote_config(),
        observed_at=observed,
    )["ok"] is False

    wrong_device = remote_status(
        observed, device=rf_accept.REMOTE_PEER_FORBIDDEN_DEVICE
    )
    wrong_validation = rf_accept.validate_remote_peer_status(
        wrong_device,
        remote_config(),
        observed_at=observed,
    )
    assert wrong_validation["ok"] is False
    assert wrong_validation["checks"]["device_exact"] is False
    assert wrong_validation["checks"]["device_non_forbidden"] is False

    wrong_key = remote_status(observed, public_key="f" * 64)
    assert rf_accept.validate_remote_peer_status(
        wrong_key,
        remote_config(),
        observed_at=observed,
    )["ok"] is False

    disconnected = remote_status(observed)
    disconnected["serial"]["mesh_connected"] = False
    assert rf_accept.validate_remote_peer_status(
        disconnected,
        remote_config(),
        observed_at=observed,
    )["ok"] is False

    self_test_failed = remote_status(observed)
    self_test_failed["startup_self_test"]["ok"] = False
    assert rf_accept.validate_remote_peer_status(
        self_test_failed,
        remote_config(),
        observed_at=observed,
    )["ok"] is False


@pytest.mark.parametrize(
    "device",
    [
        rf_accept.REMOTE_PEER_FORBIDDEN_DEVICE,
        "/dev/other-radio",
    ],
)
def test_remote_peer_config_rejects_forbidden_or_unpinned_device(device):
    config = remote_config()
    config["device"] = device
    with pytest.raises(ValueError):
        rf_accept.validate_remote_peer_config(config)


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("ssh_host", "neonx@192.168.0.25"),
        ("hostname", "other-pi"),
        ("status_path", "/tmp/radio_listener.status.json"),
        ("control_socket", "/tmp/control.sock"),
        ("max_status_age_sec", 120.001),
    ],
)
def test_remote_peer_config_rejects_forged_identity_or_freshness(
    field,
    value,
):
    config = remote_config()
    config[field] = value

    with pytest.raises(ValueError):
        rf_accept.validate_remote_peer_config(config)


@pytest.mark.parametrize(
    "token",
    [
        "",
        "-leading-option",
        "contains space",
        "line\nbreak",
        "line\rbreak",
        "semicolon;command",
        "ampersand&command",
        "unicode-\u2603",
        "x" * 129,
    ],
)
def test_rf_token_rejects_injection_before_control_serialization(token):
    with pytest.raises(ValueError):
        rf_accept.validate_safe_token(token)
    with pytest.raises(ValueError):
        rf_accept.remote_control_request(
            rf_accept.DEFAULT_D1L_PUBLIC_KEY,
            token,
        )


def test_bounded_base_token_allows_only_the_fixed_dm_suffix_headroom():
    base = "x" * 96

    assert rf_accept.validate_safe_token(base) == base
    request, _ = rf_accept.remote_control_request(
        rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        f"{base}_in",
    )
    assert request["params"]["text"] == f"{base}_in"
    with pytest.raises(ValueError):
        rf_accept.validate_safe_token(base + "x")


@pytest.mark.parametrize(
    "device",
    [
        "COM8",
        "COM11",
        "COM29",
        "/dev/krab-com11",
    ],
)
def test_remote_status_explicitly_rejects_every_forbidden_identity(
    device,
):
    observed = datetime(2026, 7, 23, 15, 0, tzinfo=timezone.utc)
    validation = rf_accept.validate_remote_peer_status(
        remote_status(observed, device=device),
        remote_config(),
        observed_at=observed,
    )
    assert validation["ok"] is False
    assert validation["checks"]["device_exact"] is False
    assert validation["checks"]["device_non_forbidden"] is False


def test_remote_control_exchange_binds_exact_target_token_and_ack():
    token = "rf_unit_in"
    request_raw, response_raw = remote_control_response(
        rf_accept.DEFAULT_D1L_PUBLIC_KEY, token
    )

    validation = rf_accept.validate_remote_control_exchange(
        request_raw,
        response_raw,
        d1l_public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token=token,
    )

    assert validation["ok"] is True
    assert all(validation["checks"].values())

    _, unacked = remote_control_response(
        rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token,
        acknowledged=False,
    )
    assert rf_accept.validate_remote_control_exchange(
        request_raw,
        unacked,
        d1l_public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token=token,
    )["ok"] is False

    _, cached = remote_control_response(
        rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token,
        cached=True,
    )
    assert rf_accept.validate_remote_control_exchange(
        request_raw,
        cached,
        d1l_public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token=token,
    )["ok"] is False

    assert rf_accept.validate_remote_control_exchange(
        request_raw,
        response_raw,
        d1l_public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="wrong-token",
    )["ok"] is False


def local_config() -> dict:
    return rf_accept.local_peer_config()


@pytest.mark.parametrize(
    "mtime_ns",
    [
        0,
        10**100,
        int(
            datetime(
                2026, 7, 23, 15, 0, 31, tzinfo=timezone.utc
            ).timestamp()
            * 1_000_000_000
        ),
        int(
            datetime(
                2026, 7, 23, 14, 59, 0, tzinfo=timezone.utc
            ).timestamp()
            * 1_000_000_000
        ),
    ],
)
def test_local_status_mtime_fails_closed_for_invalid_epoch_or_binding(
    mtime_ns,
):
    observed = datetime(2026, 7, 23, 15, 0, tzinfo=timezone.utc)
    validation = rf_accept.validate_local_peer_status(
        remote_status(observed),
        local_config(),
        observed_at=observed,
        source_mtime_ns=mtime_ns,
    )

    assert validation["ok"] is False
    assert validation["source_mtime"]["ok"] is False


def test_local_status_mtime_accepts_fresh_epoch_bound_to_payload():
    observed = datetime(2026, 7, 23, 15, 0, tzinfo=timezone.utc)
    status = remote_status(observed)
    written = rf_accept.parse_aware_timestamp(
        status["status_written_at"]
    )
    assert written is not None

    validation = rf_accept.validate_local_peer_status(
        status,
        local_config(),
        observed_at=observed,
        source_mtime_ns=int(
            written.timestamp() * 1_000_000_000
        ),
    )

    assert validation["ok"] is True
    assert all(validation["source_mtime"]["checks"].values())


def test_local_peer_mode_is_mutually_exclusive_before_io(
    monkeypatch,
    capsys,
):
    calls = []
    monkeypatch.setattr(
        rf_accept.subprocess,
        "run",
        lambda *_args, **_kwargs: calls.append("ssh"),
    )
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "rf_full_acceptance_d1l.py",
            "--dry-run",
            "--peer-local",
            "--peer-ssh-host",
            rf_accept.REMOTE_PEER_SSH_HOST,
        ],
    )

    with pytest.raises(SystemExit):
        rf_accept.main()

    assert "mutually exclusive" in capsys.readouterr().err
    assert calls == []


def test_local_peer_mode_rejects_wrong_hostname_before_io(monkeypatch):
    calls = []
    monkeypatch.setattr(rf_accept.socket, "gethostname", lambda: "other-pi")
    monkeypatch.setattr(
        rf_accept.os,
        "open",
        lambda *_args, **_kwargs: calls.append("open"),
    )
    monkeypatch.setattr(
        rf_accept.subprocess,
        "run",
        lambda *_args, **_kwargs: calls.append("ssh"),
    )

    with pytest.raises(rf_accept.RemotePeerError) as exc_info:
        rf_accept.run_local_peer_operation(local_config(), "capture_status")

    assert exc_info.value.code == "local_hostname_mismatch"
    assert calls == []


def test_local_peer_operations_never_ssh_or_open_peer_tty(monkeypatch):
    observed = datetime.now(timezone.utc)
    status_raw = json.dumps(remote_status(observed), separators=(",", ":")).encode(
        "utf-8"
    )
    request_raw, response_raw = remote_control_response(
        rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        "rf_local_in",
    )
    regular = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFREG | 0o600,
        st_size=len(status_raw),
        st_dev=1,
        st_ino=2,
        st_mtime_ns=3,
    )
    unix_socket = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFSOCK | 0o600,
        st_dev=4,
        st_ino=5,
    )
    directory = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFDIR | 0o755,
        st_dev=6,
        st_ino=7,
    )
    read_chunks = [status_raw, b""]
    opened = []
    socket_paths = []
    ssh_calls = []

    def fake_open(path, _flags):
        opened.append(path)
        assert path == rf_accept.REMOTE_PEER_STATUS_PATH
        assert path != rf_accept.REMOTE_PEER_DEVICE
        return 17

    class FakeUnixSocket:
        def __init__(self):
            self.sent = b""

        def settimeout(self, value):
            assert value == rf_accept.REMOTE_PEER_SSH_TIMEOUT_SEC

        def connect(self, path):
            socket_paths.append(path)
            assert path == rf_accept.REMOTE_PEER_CONTROL_SOCKET
            assert path != rf_accept.REMOTE_PEER_DEVICE

        def getsockopt(self, level, option, size):
            assert level == rf_accept.socket.SOL_SOCKET
            assert option == rf_accept.LOCAL_PEER_SO_PEERCRED
            assert size == rf_accept.struct.calcsize("3i")
            return rf_accept.struct.pack("3i", 7896, 0, 0)

        def sendall(self, value):
            self.sent = value

        def shutdown(self, how):
            assert how == rf_accept.socket.SHUT_WR

        def recv(self, _size):
            value = response_raw
            response_raw_holder[0] = b""
            return value

        def close(self):
            return None

    response_raw_holder = [response_raw]

    def fake_recv(self, _size):
        value = response_raw_holder[0]
        response_raw_holder[0] = b""
        return value

    FakeUnixSocket.recv = fake_recv
    client = FakeUnixSocket()
    monkeypatch.setattr(rf_accept.socket, "AF_UNIX", 1, raising=False)
    monkeypatch.setattr(
        rf_accept.socket, "gethostname", lambda: rf_accept.REMOTE_PEER_HOSTNAME
    )
    monkeypatch.setattr(rf_accept.os, "open", fake_open)
    monkeypatch.setattr(rf_accept.os, "fstat", lambda _fd: regular)
    monkeypatch.setattr(rf_accept.os, "read", lambda _fd, _size: read_chunks.pop(0))
    monkeypatch.setattr(rf_accept.os, "close", lambda _fd: None)
    def fake_lstat(path):
        value = str(path).replace("\\", "/")
        if value == rf_accept.REMOTE_PEER_STATUS_PATH:
            return regular
        if value == rf_accept.REMOTE_PEER_CONTROL_SOCKET:
            return unix_socket
        if (
            rf_accept.REMOTE_PEER_STATUS_PATH.startswith(
                value.rstrip("/") + "/"
            )
            or rf_accept.REMOTE_PEER_CONTROL_SOCKET.startswith(
                value.rstrip("/") + "/"
            )
        ):
            return directory
        raise AssertionError(path)

    monkeypatch.setattr(rf_accept.os, "lstat", fake_lstat)
    monkeypatch.setattr(
        rf_accept.socket,
        "socket",
        lambda family, kind: (
            client
            if (family, kind)
            == (rf_accept.socket.AF_UNIX, rf_accept.socket.SOCK_STREAM)
            else (_ for _ in ()).throw(AssertionError((family, kind)))
        ),
    )
    monkeypatch.setattr(
        rf_accept.subprocess,
        "run",
        lambda *_args, **_kwargs: ssh_calls.append(True),
    )

    captured = rf_accept.run_local_peer_operation(local_config(), "capture_status")
    exchange = rf_accept.run_local_peer_operation(
        local_config(),
        "send_control",
        control_request=request_raw,
    )

    assert base64.b64decode(captured["raw_b64"]) == status_raw
    assert base64.b64decode(exchange["response_b64"]) == response_raw
    assert client.sent == request_raw
    assert opened == [rf_accept.REMOTE_PEER_STATUS_PATH]
    assert socket_paths == [rf_accept.REMOTE_PEER_CONTROL_SOCKET]
    assert ssh_calls == []


def test_local_status_rejects_linked_parent_before_open(monkeypatch):
    directory = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFDIR | 0o755,
        st_dev=1,
        st_ino=2,
    )
    linked = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFLNK | 0o777,
        st_dev=1,
        st_ino=3,
    )
    regular = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFREG | 0o600,
        st_dev=1,
        st_ino=4,
    )
    calls = []

    def fake_lstat(path):
        value = str(path).replace("\\", "/")
        if value == "/opt/canadaverse":
            return linked
        if value == rf_accept.REMOTE_PEER_STATUS_PATH:
            return regular
        return directory

    monkeypatch.setattr(
        rf_accept.socket,
        "gethostname",
        lambda: rf_accept.REMOTE_PEER_HOSTNAME,
    )
    monkeypatch.setattr(rf_accept.os, "lstat", fake_lstat)
    monkeypatch.setattr(
        rf_accept.os,
        "open",
        lambda *_args, **_kwargs: calls.append("open"),
    )

    with pytest.raises(rf_accept.RemotePeerError) as exc_info:
        rf_accept.run_local_peer_operation(
            local_config(), "capture_status"
        )

    assert exc_info.value.code == "local_status_failed"
    assert calls == []


def test_local_control_rejects_linked_parent_before_socket(monkeypatch):
    directory = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFDIR | 0o755,
        st_dev=1,
        st_ino=2,
    )
    linked = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFLNK | 0o777,
        st_dev=1,
        st_ino=3,
    )
    unix_socket = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFSOCK | 0o600,
        st_dev=1,
        st_ino=4,
    )
    calls = []

    def fake_lstat(path):
        value = str(path).replace("\\", "/")
        if value == "/run/canadaverse-control":
            return linked
        if value == rf_accept.REMOTE_PEER_CONTROL_SOCKET:
            return unix_socket
        return directory

    _, request_raw = rf_accept.remote_control_request(
        rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        "linked_parent_in",
    )
    monkeypatch.setattr(
        rf_accept.socket,
        "gethostname",
        lambda: rf_accept.REMOTE_PEER_HOSTNAME,
    )
    monkeypatch.setattr(rf_accept.os, "lstat", fake_lstat)
    monkeypatch.setattr(
        rf_accept.socket,
        "socket",
        lambda *_args, **_kwargs: calls.append("socket"),
    )

    with pytest.raises(rf_accept.RemotePeerError) as exc_info:
        rf_accept.run_local_peer_operation(
            local_config(),
            "send_control",
            control_request=request_raw,
        )

    assert exc_info.value.code == "local_control_failed"
    assert calls == []


def test_local_control_rejects_non_root_socket_peer_before_send(
    monkeypatch,
):
    directory = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFDIR | 0o755,
        st_dev=1,
        st_ino=2,
    )
    unix_socket = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFSOCK | 0o600,
        st_dev=1,
        st_ino=4,
    )
    sent = []

    def fake_lstat(path):
        if (
            str(path).replace("\\", "/")
            == rf_accept.REMOTE_PEER_CONTROL_SOCKET
        ):
            return unix_socket
        return directory

    class FakeSocket:
        def settimeout(self, _value):
            return None

        def connect(self, _path):
            return None

        def getsockopt(self, _level, _option, _size):
            return rf_accept.struct.pack("3i", 123, 1000, 1000)

        def sendall(self, value):
            sent.append(value)

        def close(self):
            return None

    _, request_raw = rf_accept.remote_control_request(
        rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        "wrong_credentials_in",
    )
    monkeypatch.setattr(
        rf_accept.socket, "AF_UNIX", 1, raising=False
    )
    monkeypatch.setattr(
        rf_accept.socket,
        "gethostname",
        lambda: rf_accept.REMOTE_PEER_HOSTNAME,
    )
    monkeypatch.setattr(rf_accept.os, "lstat", fake_lstat)
    monkeypatch.setattr(
        rf_accept.socket,
        "socket",
        lambda *_args, **_kwargs: FakeSocket(),
    )

    with pytest.raises(rf_accept.RemotePeerError) as exc_info:
        rf_accept.run_local_peer_operation(
            local_config(),
            "send_control",
            control_request=request_raw,
        )

    assert exc_info.value.code == "local_control_failed"
    assert sent == []


def test_local_status_rejects_path_identity_change_during_read(
    monkeypatch,
):
    raw = b"{}"
    file_stat = SimpleNamespace(
        st_mode=rf_accept.stat.S_IFREG | 0o600,
        st_size=len(raw),
        st_dev=1,
        st_ino=2,
        st_mtime_ns=3,
    )
    snapshots = [
        (
            ("/opt", 1, 10, rf_accept.stat.S_IFDIR),
            (
                rf_accept.REMOTE_PEER_STATUS_PATH,
                1,
                2,
                rf_accept.stat.S_IFREG,
            ),
        ),
        (
            ("/opt", 1, 11, rf_accept.stat.S_IFDIR),
            (
                rf_accept.REMOTE_PEER_STATUS_PATH,
                1,
                2,
                rf_accept.stat.S_IFREG,
            ),
        ),
    ]
    reads = [raw, b""]

    monkeypatch.setattr(
        rf_accept,
        "_local_posix_path_snapshot",
        lambda *_args, **_kwargs: snapshots.pop(0),
    )
    monkeypatch.setattr(rf_accept.os, "open", lambda *_args: 17)
    monkeypatch.setattr(
        rf_accept.os, "fstat", lambda _fd: file_stat
    )
    monkeypatch.setattr(
        rf_accept.os,
        "read",
        lambda _fd, _size: reads.pop(0),
    )
    monkeypatch.setattr(rf_accept.os, "close", lambda _fd: None)

    with pytest.raises(ValueError, match="path changed"):
        rf_accept._read_local_regular_file(
            rf_accept.REMOTE_PEER_STATUS_PATH
        )


def test_local_peer_capture_and_control_preserve_exact_sidecars(
    tmp_path: Path,
    monkeypatch,
):
    observed = datetime.now(timezone.utc)
    status = remote_status(observed)
    status_raw = json.dumps(status, sort_keys=True, separators=(",", ":")).encode(
        "utf-8"
    )
    token = "rf_local_in"
    request_raw, response_raw = remote_control_response(
        rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token,
    )

    def fake_operation(
        _config,
        operation,
        *,
        control_request=None,
        timeout_sec=rf_accept.REMOTE_PEER_SSH_TIMEOUT_SEC,
    ):
        assert timeout_sec == rf_accept.REMOTE_PEER_SSH_TIMEOUT_SEC
        if operation == "capture_status":
            assert control_request is None
            return {
                "path": rf_accept.REMOTE_PEER_STATUS_PATH,
                "size": len(status_raw),
                "sha256": hashlib.sha256(status_raw).hexdigest(),
                "mtime_ns": int(
                    observed.timestamp() * 1_000_000_000
                ),
                "hostname": rf_accept.REMOTE_PEER_HOSTNAME,
                "raw_b64": base64.b64encode(status_raw).decode("ascii"),
            }
        assert operation == "send_control"
        assert control_request == request_raw
        return {
            "socket_path": rf_accept.REMOTE_PEER_CONTROL_SOCKET,
            "hostname": rf_accept.REMOTE_PEER_HOSTNAME,
            "request_size": len(request_raw),
            "request_sha256": hashlib.sha256(request_raw).hexdigest(),
            "response_size": len(response_raw),
            "response_sha256": hashlib.sha256(response_raw).hexdigest(),
            "response_b64": base64.b64encode(response_raw).decode("ascii"),
            "peer_pid": 7896,
            "peer_uid": rf_accept.LOCAL_PEER_CONTROL_UID,
            "peer_gid": rf_accept.LOCAL_PEER_CONTROL_GID,
        }

    monkeypatch.setattr(rf_accept, "run_local_peer_operation", fake_operation)
    status_path = tmp_path / "evidence" / "status.json"
    request_path = tmp_path / "evidence" / "request.jsonl"
    response_path = tmp_path / "evidence" / "response.jsonl"

    captured_status, status_receipt, validation = rf_accept.capture_local_peer_status(
        local_config(), status_path, tmp_path
    )
    control = rf_accept.send_local_peer_dm(
        local_config(),
        d1l_public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token=token,
        request_capture_path=request_path,
        response_capture_path=response_path,
        root=tmp_path,
    )

    assert captured_status == status
    assert validation["ok"] is True
    assert status_path.read_bytes() == status_raw
    assert request_path.read_bytes() == request_raw
    assert response_path.read_bytes() == response_raw
    assert status_receipt["transport"] == rf_accept.LOCAL_PEER_STATUS_TRANSPORT
    assert status_receipt["source_path"] == rf_accept.REMOTE_PEER_STATUS_PATH
    assert status_receipt["source_hostname"] == rf_accept.REMOTE_PEER_HOSTNAME
    assert status_receipt["source_sha256"] == status_receipt["sha256"]
    assert (
        control["request_receipt"]["transport"]
        == rf_accept.LOCAL_PEER_CONTROL_REQUEST_TRANSPORT
    )
    assert (
        control["response_receipt"]["transport"]
        == rf_accept.LOCAL_PEER_CONTROL_RESPONSE_TRANSPORT
    )
    assert control["request_receipt"]["source_peer_pid"] == 7896
    assert (
        control["request_receipt"]["source_peer_uid"]
        == rf_accept.LOCAL_PEER_CONTROL_UID
    )


def test_local_sidecar_collision_prevents_serial_socket_and_ssh(
    tmp_path: Path,
    monkeypatch,
):
    (tmp_path / "scripts").mkdir()
    monkeypatch.setattr(
        rf_accept,
        "__file__",
        str(tmp_path / "scripts" / "rf_full_acceptance_d1l.py"),
    )
    monkeypatch.setitem(sys.modules, "serial", SimpleNamespace())
    monkeypatch.setattr(
        rf_accept,
        "git_metadata",
        lambda _root: {
            "commit": "a" * 40,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(
        rf_accept.socket, "gethostname", lambda: rf_accept.REMOTE_PEER_HOSTNAME
    )
    external_calls = []

    def unexpected_external(*_args, **_kwargs):
        external_calls.append(True)
        raise AssertionError("collision must fail before external I/O")

    monkeypatch.setattr(rf_accept, "open_d1l_serial", unexpected_external)
    monkeypatch.setattr(rf_accept, "run_local_peer_operation", unexpected_external)
    monkeypatch.setattr(rf_accept.subprocess, "run", unexpected_external)
    capture_dir = tmp_path / "artifacts" / "rf-peer"
    capture_dir.mkdir(parents=True)
    collision = capture_dir / "collision_peer_after.json"
    collision.write_bytes(b"sentinel")

    with pytest.raises(ValueError, match="refusing to overwrite"):
        rf_accept.run_hardware(
            port="COM12",
            baud=115200,
            timeout=1.0,
            wait_sec=1.0,
            poll_sec=0.1,
            peer_status_path=None,
            peer_port=None,
            fingerprint=rf_accept.REMOTE_PEER_FINGERPRINT,
            public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
            token="collision",
            send_outbound=True,
            expected_commit="a" * 40,
            github_run_id="1",
            workflow_run_attempt="1",
            peer_capture_dir=capture_dir,
            local_peer=local_config(),
            port_lister=lambda: [windows_target_row()],
            platform_name="nt",
        )

    assert external_calls == []
    assert collision.read_bytes() == b"sentinel"
    assert not (capture_dir / "collision_peer_before.json").exists()


def test_remote_peer_ssh_uses_fixed_argv_stdin_and_no_shell(monkeypatch):
    calls = []
    response = {
        "schema": rf_accept.REMOTE_PEER_HELPER_SCHEMA,
        "ok": True,
        "operation": "capture_status",
        "result": {},
        "error": None,
    }

    def fake_run(argv, **kwargs):
        calls.append((argv, kwargs))
        return SimpleNamespace(
            returncode=0,
            stdout=json.dumps(response).encode("utf-8"),
            stderr=b"",
        )

    monkeypatch.setattr(rf_accept.subprocess, "run", fake_run)

    assert rf_accept.run_remote_peer_operation(
        remote_config(), "capture_status"
    ) == {}
    argv, kwargs = calls[0]
    assert argv == [
        "ssh",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=8",
        "-o",
        "StrictHostKeyChecking=yes",
        "-o",
        "LogLevel=ERROR",
        rf_accept.REMOTE_PEER_SSH_HOST,
        rf_accept.REMOTE_PEER_HELPER_COMMAND,
    ]
    assert kwargs["shell"] is False
    assert kwargs["check"] is False
    assert kwargs["timeout"] == rf_accept.REMOTE_PEER_SSH_TIMEOUT_SEC
    assert kwargs["stdout"] is rf_accept.subprocess.PIPE
    assert kwargs["stderr"] is rf_accept.subprocess.PIPE
    assert rf_accept.REMOTE_PEER_STATUS_PATH not in " ".join(argv)
    request = json.loads(kwargs["input"].decode("utf-8"))
    assert request["status_path"] == rf_accept.REMOTE_PEER_STATUS_PATH
    assert request["control_socket"] == rf_accept.REMOTE_PEER_CONTROL_SOCKET


def test_remote_peer_ssh_uses_explicit_nonlinked_identity(
    tmp_path, monkeypatch
):
    identity = tmp_path / "neonx"
    identity.write_bytes(b"private-key-fixture")
    monkeypatch.setenv(
        rf_accept.REMOTE_PEER_SSH_IDENTITY_ENV, str(identity.resolve())
    )
    calls = []
    response = {
        "schema": rf_accept.REMOTE_PEER_HELPER_SCHEMA,
        "ok": True,
        "operation": "capture_status",
        "result": {},
        "error": None,
    }

    def fake_run(argv, **kwargs):
        calls.append((argv, kwargs))
        return SimpleNamespace(
            returncode=0,
            stdout=json.dumps(response).encode("utf-8"),
            stderr=b"",
        )

    monkeypatch.setattr(rf_accept.subprocess, "run", fake_run)

    assert rf_accept.run_remote_peer_operation(
        remote_config(), "capture_status"
    ) == {}
    argv, _ = calls[0]
    identity_index = argv.index("-i")
    assert argv[identity_index - 2 : identity_index] == [
        "-o",
        "IdentitiesOnly=yes",
    ]
    assert argv[identity_index + 1] == str(identity.resolve())
    assert argv[identity_index + 2] == rf_accept.REMOTE_PEER_SSH_HOST


@pytest.mark.parametrize("value", ["", "missing-private-key"])
def test_remote_peer_ssh_rejects_invalid_explicit_identity(
    tmp_path, monkeypatch, value
):
    configured = value
    if value:
        configured = str(tmp_path / value)
    monkeypatch.setenv(rf_accept.REMOTE_PEER_SSH_IDENTITY_ENV, configured)
    monkeypatch.setattr(
        rf_accept.subprocess,
        "run",
        lambda *_args, **_kwargs: pytest.fail(
            "invalid SSH identity must fail before subprocess"
        ),
    )

    with pytest.raises(
        rf_accept.RemotePeerError, match="SSH identity"
    ) as caught:
        rf_accept.run_remote_peer_operation(
            remote_config(), "capture_status"
        )
    assert caught.value.code == "ssh_identity_invalid"


def test_remote_peer_ssh_rejects_noncanonical_success_envelope(monkeypatch):
    response = {
        "schema": rf_accept.REMOTE_PEER_HELPER_SCHEMA,
        "ok": True,
        "operation": "capture_status",
        "result": {},
        "error": None,
        "unexpected": True,
    }

    monkeypatch.setattr(
        rf_accept.subprocess,
        "run",
        lambda *_args, **_kwargs: SimpleNamespace(
            returncode=0,
            stdout=json.dumps(response).encode("utf-8"),
            stderr=b"",
        ),
    )

    with pytest.raises(rf_accept.RemotePeerError) as exc_info:
        rf_accept.run_remote_peer_operation(
            remote_config(),
            "capture_status",
        )
    assert exc_info.value.code == "ssh_invalid_response"


def test_remote_peer_ssh_auth_failure_is_explicit(monkeypatch):
    def fake_run(_argv, **_kwargs):
        return SimpleNamespace(
            returncode=255,
            stdout=b"",
            stderr=b"Permission denied (publickey,password).",
        )

    monkeypatch.setattr(rf_accept.subprocess, "run", fake_run)

    with pytest.raises(rf_accept.RemotePeerError) as exc_info:
        rf_accept.run_remote_peer_operation(
            remote_config(), "capture_status"
        )
    assert exc_info.value.code == "ssh_auth_failed"
    assert "ephemeral" in str(exc_info.value)


def test_remote_status_capture_rejects_forged_hostname_and_keeps_marker(
    tmp_path,
    monkeypatch,
):
    raw = json.dumps(
        remote_status(datetime.now(timezone.utc)),
        separators=(",", ":"),
    ).encode("utf-8")

    monkeypatch.setattr(
        rf_accept,
        "run_remote_peer_operation",
        lambda *_args, **_kwargs: {
            "path": rf_accept.REMOTE_PEER_STATUS_PATH,
            "size": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "mtime_ns": 1,
            "hostname": "forged-pi",
            "raw_b64": base64.b64encode(raw).decode("ascii"),
        },
    )
    capture = tmp_path / "peer-before.json"

    with pytest.raises(rf_accept.RemotePeerError) as exc_info:
        rf_accept.capture_remote_peer_status(
            remote_config(),
            capture,
            tmp_path,
        )

    assert exc_info.value.code == "remote_status_invalid"
    marker = json.loads(capture.read_text(encoding="ascii"))
    assert marker["kind"] == "sigui_evidence_reservation"
    assert marker["transmission_may_have_occurred"] is True


def test_remote_peer_dry_run_never_invokes_ssh(tmp_path, monkeypatch):
    (tmp_path / "scripts").mkdir()
    monkeypatch.setattr(
        rf_accept,
        "__file__",
        str(tmp_path / "scripts" / "rf_full_acceptance_d1l.py"),
    )

    def fail_run(*_args, **_kwargs):
        raise AssertionError("dry run must not invoke subprocess")

    monkeypatch.setattr(rf_accept.subprocess, "run", fail_run)
    monkeypatch.setattr(
        rf_accept,
        "stamp_report",
        lambda report, _root: report,
    )
    report_path = tmp_path / "dry-run.json"
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "rf_full_acceptance_d1l.py",
            "--dry-run",
            "--port",
            "COM12",
            "--peer-ssh-host",
            "neonx@192.168.0.24",
            "--out",
            str(report_path),
        ],
    )

    assert rf_accept.main() == 0
    captured = json.loads(report_path.read_text(encoding="utf-8"))
    assert captured["controlled_peer"]["device"] == rf_accept.REMOTE_PEER_DEVICE
    assert captured["controlled_peer_control_plan"] == {
        "op": "radio.send_dm",
        "socket_path": rf_accept.REMOTE_PEER_CONTROL_SOCKET,
        "target": rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        "text": captured["inbound_token"],
        "transport": "ssh-stdin-json",
    }
    assert captured["closure_eligible"] is False


def test_invalid_rf_token_fails_before_serial_or_ssh(monkeypatch):
    calls = []

    def unexpected_external(*_args, **_kwargs):
        calls.append("external")
        raise AssertionError("invalid token must fail before external I/O")

    monkeypatch.setattr(
        rf_accept,
        "open_d1l_serial",
        unexpected_external,
    )
    monkeypatch.setattr(
        rf_accept,
        "run_remote_peer_operation",
        unexpected_external,
    )
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "rf_full_acceptance_d1l.py",
            "--port",
            "COM12",
            "--peer-ssh-host",
            rf_accept.REMOTE_PEER_SSH_HOST,
            "--token",
            "bad;mesh-send",
            "--commit",
            "a" * 40,
            "--github-run-id",
            "1",
            "--github-run-attempt",
            "1",
        ],
    )

    with pytest.raises(SystemExit) as exc_info:
        rf_accept.main()

    assert exc_info.value.code == 2
    assert calls == []


def test_remote_dm_capture_preserves_exact_request_and_response(
    tmp_path: Path,
    monkeypatch,
):
    token = "rf_capture_in"
    request_raw, response_raw = remote_control_response(
        rf_accept.DEFAULT_D1L_PUBLIC_KEY, token
    )

    def fake_operation(
        _config,
        operation,
        *,
        control_request=None,
        timeout_sec=rf_accept.REMOTE_PEER_SSH_TIMEOUT_SEC,
    ):
        assert operation == "send_control"
        assert control_request == request_raw
        assert timeout_sec == rf_accept.REMOTE_PEER_SSH_TIMEOUT_SEC
        return {
            "socket_path": rf_accept.REMOTE_PEER_CONTROL_SOCKET,
            "hostname": rf_accept.REMOTE_PEER_HOSTNAME,
            "request_size": len(request_raw),
            "request_sha256": __import__("hashlib").sha256(
                request_raw
            ).hexdigest(),
            "response_size": len(response_raw),
            "response_sha256": __import__("hashlib").sha256(
                response_raw
            ).hexdigest(),
            "response_b64": __import__("base64").b64encode(
                response_raw
            ).decode("ascii"),
        }

    monkeypatch.setattr(
        rf_accept, "run_remote_peer_operation", fake_operation
    )
    request_path = tmp_path / "evidence" / "request.jsonl"
    response_path = tmp_path / "evidence" / "response.jsonl"

    result = rf_accept.send_remote_peer_dm(
        remote_config(),
        d1l_public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token=token,
        request_capture_path=request_path,
        response_capture_path=response_path,
        root=tmp_path,
    )

    assert result["validation"]["ok"] is True
    assert request_path.read_bytes() == request_raw
    assert response_path.read_bytes() == response_raw
    assert (
        result["request_receipt"]["transport"]
        == "ssh-unix-socket-request"
    )
    assert (
        result["response_receipt"]["transport"]
        == "ssh-unix-socket-response"
    )
    assert (
        result["request_receipt"]["source_hostname"]
        == rf_accept.REMOTE_PEER_HOSTNAME
    )
    assert (
        result["response_receipt"]["source_hostname"]
        == rf_accept.REMOTE_PEER_HOSTNAME
    )

    def forged_hostname_operation(*args, **kwargs):
        forged = fake_operation(*args, **kwargs)
        forged["hostname"] = "forged-pi"
        return forged

    monkeypatch.setattr(
        rf_accept,
        "run_remote_peer_operation",
        forged_hostname_operation,
    )
    with pytest.raises(rf_accept.RemotePeerError) as exc_info:
        rf_accept.send_remote_peer_dm(
            remote_config(),
            d1l_public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
            token=token,
            request_capture_path=tmp_path / "forged-request.jsonl",
            response_capture_path=tmp_path / "forged-response.jsonl",
            root=tmp_path,
        )
    assert exc_info.value.code == "remote_control_invalid"


def test_evidence_reservations_reject_collision_and_reparse_parent(
    tmp_path,
    monkeypatch,
):
    collision = tmp_path / "existing.json"
    collision.write_bytes(b"sentinel")
    with rf_accept.EvidenceBundle(tmp_path) as bundle:
        with pytest.raises(ValueError, match="refusing to overwrite"):
            bundle.reserve("collision", collision)
    assert collision.read_bytes() == b"sentinel"

    reparse_parent = tmp_path / "reparse"
    reparse_parent.mkdir()
    monkeypatch.setattr(
        rf_accept,
        "is_link_or_reparse",
        lambda path: Path(path) == reparse_parent,
    )
    rejected = reparse_parent / "evidence.json"
    with rf_accept.EvidenceBundle(tmp_path) as bundle:
        with pytest.raises(ValueError, match="link/reparse"):
            bundle.reserve("reparse", rejected)
    assert not rejected.exists()


def test_rf_sidecar_collision_prevents_serial_and_ssh(
    tmp_path,
    monkeypatch,
):
    (tmp_path / "scripts").mkdir()
    monkeypatch.setattr(
        rf_accept,
        "__file__",
        str(tmp_path / "scripts" / "rf_full_acceptance_d1l.py"),
    )
    monkeypatch.setitem(sys.modules, "serial", SimpleNamespace())
    monkeypatch.setattr(
        rf_accept,
        "git_metadata",
        lambda _root: {
            "commit": "a" * 40,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    external_calls = []

    def unexpected_external(*_args, **_kwargs):
        external_calls.append(True)
        raise AssertionError("collision must fail before external I/O")

    monkeypatch.setattr(
        rf_accept,
        "open_d1l_serial",
        unexpected_external,
    )
    monkeypatch.setattr(
        rf_accept,
        "run_remote_peer_operation",
        unexpected_external,
    )
    capture_dir = tmp_path / "artifacts" / "rf-peer"
    capture_dir.mkdir(parents=True)
    collision = capture_dir / "collision_peer_after.json"
    collision.write_bytes(b"sentinel")

    with pytest.raises(ValueError, match="refusing to overwrite"):
        rf_accept.run_hardware(
            port="COM12",
            baud=115200,
            timeout=1.0,
            wait_sec=1.0,
            poll_sec=0.1,
            peer_status_path=None,
            peer_port=None,
            fingerprint=rf_accept.REMOTE_PEER_FINGERPRINT,
            public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
            token="collision",
            send_outbound=True,
            expected_commit="a" * 40,
            github_run_id="1",
            workflow_run_attempt="1",
            peer_capture_dir=capture_dir,
            remote_peer=remote_config(),
            port_lister=lambda: [windows_target_row()],
            platform_name="nt",
        )

    assert external_calls == []
    assert collision.read_bytes() == b"sentinel"
    assert not (capture_dir / "collision_peer_before.json").exists()


def test_rf_report_collision_prevents_hardware_entry(
    tmp_path,
    monkeypatch,
):
    (tmp_path / "scripts").mkdir()
    monkeypatch.setattr(
        rf_accept,
        "__file__",
        str(tmp_path / "scripts" / "rf_full_acceptance_d1l.py"),
    )
    report_path = tmp_path / "rf-report.json"
    report_path.write_bytes(b"sentinel")
    calls = []

    def unexpected_hardware(**_kwargs):
        calls.append(True)
        raise AssertionError("report collision must fail before hardware")

    monkeypatch.setattr(rf_accept, "run_hardware", unexpected_hardware)
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "rf_full_acceptance_d1l.py",
            "--port",
            "COM12",
            "--peer-ssh-host",
            rf_accept.REMOTE_PEER_SSH_HOST,
            "--token",
            "report_collision",
            "--commit",
            "a" * 40,
            "--github-run-id",
            "1",
            "--github-run-attempt",
            "1",
            "--out",
            str(report_path),
        ],
    )

    with pytest.raises(ValueError, match="refusing to overwrite"):
        rf_accept.main()

    assert calls == []
    assert report_path.read_bytes() == b"sentinel"


def test_rf_report_reparse_parent_prevents_hardware_entry(
    tmp_path,
    monkeypatch,
):
    (tmp_path / "scripts").mkdir()
    monkeypatch.setattr(
        rf_accept,
        "__file__",
        str(tmp_path / "scripts" / "rf_full_acceptance_d1l.py"),
    )
    alias_parent = tmp_path / "report-alias"
    alias_parent.mkdir()
    target_parent = tmp_path / "report-target"
    target_parent.mkdir()
    report_path = alias_parent / "rf-report.json"
    target_path = target_parent / report_path.name
    original_resolve = Path.resolve

    def junction_resolve(path, strict=False):
        if Path(path) == report_path:
            return target_path
        return original_resolve(path, strict=strict)

    monkeypatch.setattr(Path, "resolve", junction_resolve)
    monkeypatch.setattr(
        rf_accept,
        "is_link_or_reparse",
        lambda path: Path(path) == alias_parent,
    )
    calls = []

    def unexpected_hardware(**_kwargs):
        calls.append(True)
        raise AssertionError("report reparse must fail before hardware")

    monkeypatch.setattr(rf_accept, "run_hardware", unexpected_hardware)
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "rf_full_acceptance_d1l.py",
            "--port",
            "COM12",
            "--peer-ssh-host",
            rf_accept.REMOTE_PEER_SSH_HOST,
            "--token",
            "report_reparse",
            "--commit",
            "a" * 40,
            "--github-run-id",
            "1",
            "--github-run-attempt",
            "1",
            "--out",
            str(report_path),
        ],
    )

    with pytest.raises(ValueError, match="link/reparse"):
        rf_accept.main()

    assert calls == []
    assert not report_path.exists()
    assert not target_path.exists()


def test_post_transmit_write_failure_leaves_explicit_incomplete_marker(
    tmp_path,
    monkeypatch,
):
    token = "post_tx_in"
    request_raw, response_raw = remote_control_response(
        rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token,
    )
    monkeypatch.setattr(
        rf_accept,
        "run_remote_peer_operation",
        lambda *_args, **_kwargs: {
            "socket_path": rf_accept.REMOTE_PEER_CONTROL_SOCKET,
            "hostname": rf_accept.REMOTE_PEER_HOSTNAME,
            "request_size": len(request_raw),
            "request_sha256": hashlib.sha256(request_raw).hexdigest(),
            "response_size": len(response_raw),
            "response_sha256": hashlib.sha256(response_raw).hexdigest(),
            "response_b64": base64.b64encode(response_raw).decode("ascii"),
        },
    )
    request_path = tmp_path / "request.jsonl"
    response_path = tmp_path / "response.jsonl"

    with pytest.raises(OSError, match="simulated evidence write failure"):
        with rf_accept.EvidenceBundle(tmp_path) as bundle:
            request_reservation = bundle.reserve(
                "request",
                request_path,
            )
            response_reservation = bundle.reserve(
                "response",
                response_path,
            )
            bundle.mark_external_io_started()

            def fail_write(_raw):
                raise OSError("simulated evidence write failure")

            monkeypatch.setattr(
                response_reservation,
                "write_bytes",
                fail_write,
            )
            rf_accept.send_remote_peer_dm(
                remote_config(),
                d1l_public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
                token=token,
                request_capture_path=request_path,
                response_capture_path=response_path,
                root=tmp_path,
                request_reservation=request_reservation,
                response_reservation=response_reservation,
                evidence_bundle=bundle,
            )

    assert request_path.read_bytes() == request_raw
    marker = json.loads(response_path.read_text(encoding="ascii"))
    assert marker["state"] == "incomplete_external_io_may_have_occurred"
    assert marker["external_io_started"] is True
    assert marker["transmission_may_have_occurred"] is True
    assert marker["error_type"] == "OSError"


def test_remote_build_report_requires_status_control_and_d1l_correlation():
    observed = datetime(2026, 7, 23, 15, 0, tzinfo=timezone.utc)
    before = remote_status(observed)
    after = json.loads(json.dumps(before))
    after["status_written_at"] = (
        observed - timedelta(seconds=1)
    ).isoformat()
    after["mesh"]["last_fetch_ok_at"] = (
        observed - timedelta(seconds=1)
    ).isoformat()
    after["mesh"]["last_rx_at"] = "after-rx"
    after["mesh"]["last_tx_at"] = "after-tx"
    after["counters"]["rx_dm_total"] += 1
    after["counters"]["tx_dm_total"] += 1

    before_validation = rf_accept.validate_remote_peer_status(
        before, remote_config(), observed_at=observed
    )
    after_validation = rf_accept.validate_remote_peer_status(
        after, remote_config(), observed_at=observed
    )
    request_raw, response_raw = remote_control_response(
        rf_accept.DEFAULT_D1L_PUBLIC_KEY, "rf_remote_in"
    )
    control_validation = rf_accept.validate_remote_control_exchange(
        request_raw,
        response_raw,
        d1l_public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="rf_remote_in",
    )
    request = control_validation["request"]
    control = {
        "op": "radio.send_dm",
        "socket_path": rf_accept.REMOTE_PEER_CONTROL_SOCKET,
        "request_id": request["id"],
        "request": request,
        "response": control_validation["response"],
        "request_receipt": {"path": "request.jsonl"},
        "response_receipt": {"path": "response.jsonl"},
        "request_sha256": control_validation["request_sha256"],
        "response_sha256": control_validation["response_sha256"],
        "validation": control_validation,
    }
    fingerprint = rf_accept.REMOTE_PEER_FINGERPRINT
    peer_key = rf_accept.REMOTE_PEER_PUBLIC_KEY
    import_command = rf_accept.contact_import_command(peer_key)
    contact = {
        "fingerprint": fingerprint,
        "public_key": peer_key,
        "alias": rf_accept.RADIO_LISTENER_CONTACT_NAME,
        "type": "chat",
        "verification_source": "uri_import",
        "canonical": True,
        "can_dm": True,
        "can_admin": False,
    }
    import_result = {
        "ok": True,
        "cmd": "contacts import",
        "persisted": True,
        "result": "created",
        **contact,
    }
    ack_hash = 1234567890
    baseline_messages = {
        "ok": True,
        "fingerprint": fingerprint,
        "entries": [
            {"seq": 1, "direction": "tx", "text": "older"}
        ],
    }
    final_messages = {
        "ok": True,
        "fingerprint": fingerprint,
        "entries": [
            *baseline_messages["entries"],
            {
                "seq": 2,
                "fingerprint": fingerprint,
                "direction": "tx",
                "text": "core acceptance test rf_remote_out",
                "acked": True,
                "delivered": True,
                "ack_hash": ack_hash,
                "ack_response": {
                    "identity_valid": False,
                    "state": "legacy_unverified",
                    "dispatch_count": 0,
                    "last_kind": "none",
                    "last_error": "ESP_OK",
                },
            },
            {
                "seq": 3,
                "fingerprint": fingerprint,
                "direction": "rx",
                "text": "rf_remote_in",
                "ack_response": {
                    "identity_valid": True,
                    "state": "sent",
                    "dispatch_count": 1,
                    "last_kind": "direct_ack",
                    "last_error": "ESP_OK",
                },
            },
        ],
    }
    baseline_packets = {
        "ok": True,
        "entries": [
            {"seq": 10, "kind": "other", "direction": "rx"}
        ],
    }
    final_packets = {
        "ok": True,
        "entries": [
            *baseline_packets["entries"],
            {
                "seq": 11,
                "direction": "rx",
                "kind": "dm_ack",
                "note": (
                    f"ack {ack_hash} "
                    f"{rf_accept.RADIO_LISTENER_CONTACT_NAME}"
                ),
                "rssi_dbm": -70,
                "snr_tenths": 80,
                "path_hash_bytes": 1,
                "path_hops": 0,
                "payload_len": 12,
            },
        ],
    }
    baseline_route = {
        "ok": True,
        "fingerprint": fingerprint,
        "entries": [
            {
                "seq": 20,
                "target": fingerprint,
                "kind": "other",
                "direction": "rx",
                "route": "direct",
            }
        ],
    }
    final_route = {
        "ok": True,
        "fingerprint": fingerprint,
        "entries": [
            {
                "seq": 21,
                "target": fingerprint,
                "kind": "dm_ack",
                "direction": "rx",
                "route": "direct",
                "last_rssi_dbm": -70,
                "last_snr_tenths": 80,
                "path_hash_bytes": 1,
                "path_hops": 0,
                "payload_len": 12,
            }
        ],
    }
    version = {
        "ok": True,
        "cmd": "version",
        "build_commit": "a" * 40,
        "idf": "v5.5.4",
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "time": {
            "protocol_tx_ready": True,
            "protocol_tx_block": "none",
        },
    }
    identity = {
        "ok": True,
        "cmd": "identity status",
        "public_key": rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        "fingerprint": rf_accept.DEFAULT_D1L_PUBLIC_KEY[
            :16
        ].upper(),
    }
    steps = [
        {"command": "version", "result": version},
        {"command": "identity status", "result": identity},
        {"command": "contacts", "result": {"ok": True, "entries": []}},
        {"command": import_command, "result": import_result},
        {
            "command": "contacts",
            "result": {"ok": True, "entries": [contact]},
        },
        {
            "command": f"messages dm {fingerprint}",
            "result": baseline_messages,
        },
        {"command": "packets", "result": baseline_packets},
        {
            "command": f"routes trace {fingerprint}",
            "result": baseline_route,
        },
        {
            "command": (
                f"mesh send dm {fingerprint} "
                "core acceptance test rf_remote_out"
            ),
            "result": {"ok": True},
        },
        {
            "command": "packets search rf_remote_out",
            "result": {"ok": True, "entries": [{"note": "rf_remote_out"}]},
        },
        {
            "command": f"messages dm {fingerprint}",
            "result": final_messages,
        },
        {"command": "packets", "result": final_packets},
        {
            "command": f"routes trace {fingerprint}",
            "result": final_route,
        },
        {
            "command": f"messages dm {fingerprint}",
            "result": final_messages,
        },
        {"command": "packets", "result": final_packets},
        {
            "command": f"routes trace {fingerprint}",
            "result": final_route,
        },
        {
            "command": "health",
            "result": {
                "ok": True,
                "cmd": "health",
                "build_commit": "a" * 40,
                "release_profile": "core_1_0",
                "sd_history_mode": "disabled",
                "board_ready": True,
                "ui_ready": True,
            },
        },
    ]

    report = rf_accept.build_report(
        port="COM12",
        **windows_target_pair(),
        baud=115200,
        peer_status_path=None,
        peer_port=None,
        fingerprint=fingerprint,
        public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        token="rf_remote",
        send_outbound=True,
        steps=steps,
        peer_before=before,
        peer_after=after,
        inbound_seen_at=observed.isoformat(),
        expected_commit="a" * 40,
        peer_before_receipt={"path": "before.json"},
        peer_after_receipt={"path": "after.json"},
        github_run_id="123",
        workflow_run_attempt="1",
        remote_peer=remote_config(),
        remote_before_validation=before_validation,
        remote_after_validation=after_validation,
        remote_control=control,
    )

    assert report["ok"] is True
    assert report["controlled_peer"]["port"] is None
    assert (
        report["controlled_peer"]["evidence_source"]
        == rf_accept.REMOTE_PEER_EVIDENCE_SOURCE
    )
    assert (
        report["controlled_peer"]["device"]
        == rf_accept.REMOTE_PEER_DEVICE
    )
    assert report["controlled_peer_remote"]["flow"]["ok"] is True
    assert report["checks"]["controlled_peer_status_connected"] is True
    assert rf_accept.remote_peer_report_shape_ok(report)

    mismatched_after = json.loads(json.dumps(after))
    mismatched_after["counters"]["tx_dm_total"] += 1
    mismatched_flow = rf_accept.remote_peer_flow_validation(
        before=before,
        after=mismatched_after,
        before_validation=before_validation,
        after_validation=after_validation,
        d1l_public_key=rf_accept.DEFAULT_D1L_PUBLIC_KEY,
        control=control,
    )
    assert mismatched_flow["ok"] is False
    assert (
        mismatched_flow["checks"][
            "peer_tx_exactly_control_plus_fast_reply"
        ]
        is False
    )

    report["controlled_peer_control"]["response"]["cached"] = True
    assert not rf_accept.remote_peer_report_shape_ok(report)
