import hashlib
from pathlib import Path

import pytest

from scripts import produce_rc1_bounded_physical_receipt_d1l as gate
from scripts import produce_rc1_protocol_acceptance_d1l as runner


COMMIT = "a" * 40
RUN = "123"
ATTEMPT = "1"
IDENTITY_KEY = "b" * 64
PEER_KEY = "c" * 64
ADMIN_KEY = "d" * 64
PEER_FP = PEER_KEY[:16].upper()
ADMIN_FP = ADMIN_KEY[:16].upper()
NONCE = "123456789abc"
PUBLIC_OUT = f"rc1-public-out-{COMMIT[:8]}-{NONCE}"
PUBLIC_IN = f"rc1-public-in-{COMMIT[:8]}-{NONCE}"
DM_OUT = f"rc1-dm-out-{COMMIT[:8]}-{NONCE}"
DM_IN = f"rc1-dm-in-{COMMIT[:8]}-{NONCE}"
CANDIDATE = {
    "firmware_commit": COMMIT,
    "actions_run": RUN,
    "actions_run_attempt": ATTEMPT,
}
OPERATIONS = (
    "version",
    "identity",
    "health_before",
    "mesh_status",
    "peer_advert",
    "contacts",
    "trace_request",
    "trace_result",
    "peer_before",
    "public_tx_authorization",
    "public_send",
    "public_tx_record",
    "peer_after_public",
    "peer_public_send",
    "public_receive",
    "peer_before_dm",
    "dm_send",
    "dm_ack",
    "peer_after_dm",
    "peer_dm_send",
    "dm_receive_ack",
    "path_request",
    "path_result",
    "admin_login_request",
    "admin_login_status",
    "admin_query_request",
    "admin_query_status",
    "admin_logout",
    "ping_request",
    "ping_result",
    "health_after",
    "crashlog",
)


def peer_capture(*, channel: int, dm: int, sender: str = "") -> dict:
    snapshot = {
        "service": "openclaw-radio-listener",
        "run_id": "controlled-peer-run",
        "status_written_at": "2026-07-25T20:00:00Z",
        "serial": {
            "mesh_connected": True,
            "port": "/dev/krab-t-echo",
            "public_key": PEER_KEY,
        },
        "counters": {
            "rx_channel_total": channel,
            "rx_dm_total": dm,
        },
        "mesh": {"last_rx_sender": sender},
    }
    return {
        "source": "local_peer_status_file",
        "path": (
            "/opt/canadaverse/com15-responder/data/"
            "radio_listener.status.json"
        ),
        "captured_at": "2026-07-25T20:00:01Z",
        "snapshot_sha256": hashlib.sha256(
            gate.canonical_json(snapshot)
        ).hexdigest(),
        "snapshot": snapshot,
    }


def control(operation: str, request_id: str, params: dict, result: dict) -> dict:
    return {
        "request": {
            "id": request_id,
            "op": operation,
            "params": params,
        },
        "response": {
            "id": request_id,
            "op": operation,
            "ok": True,
            "cached": False,
            "duration_ms": 5,
            "result": result,
            "error": None,
        },
    }


def matched_trace(command: str, fingerprint: str, tag: int, zero_hop: bool) -> dict:
    return {
        "schema": 1,
        "ok": True,
        "cmd": command,
        "fingerprint": fingerprint,
        "zero_hop": zero_hop,
        "matched": True,
        "pending": {"active": False},
        "last_attempt": {
            "valid": True,
            "tag": tag,
            "outcome": "matched",
        },
        "last_result": {"valid": True, "tag": tag},
    }


def admin_status(command: str, state: str) -> dict:
    return {
        "schema": 1,
        "ok": True,
        "cmd": command,
        "state": state,
        "role": "repeater",
        "fingerprint": ADMIN_FP,
        "status_valid": True,
        "credential_exposed": False,
        "session_secret_exposed": False,
        "login_tx_queued": 1,
        "query_accepted": 1,
        "query_result": {
            "valid": False,
            "kind": "none",
            "text": "",
        },
    }


def protocol_transcript() -> dict:
    login_pending = admin_status("admin login", "login_pending")
    login_status = admin_status("admin status", "authenticated")
    login_status["status_valid"] = False
    query_pending = admin_status("admin telemetry", "query_pending")
    query_status = admin_status("admin status", "authenticated")
    query_status["query_result"] = {
        "valid": True,
        "kind": "telemetry",
        "text": "battery=4800mV uptime=10",
    }
    logout = admin_status("admin logout", "idle")
    logout["role"] = "none"
    logout["fingerprint"] = ""
    logout["status_valid"] = False

    responses = {
        "version": {
            "schema": 1,
            "ok": True,
            "cmd": "version",
            "build_commit": COMMIT,
            "release_profile": "core_1_0",
            "sd_history_mode": "conditional",
        },
        "identity": {
            "schema": 1,
            "ok": True,
            "cmd": "identity status",
            "node_name": "D1L",
            "role": "desk_companion",
            "public_key_ready": True,
            "public_key": IDENTITY_KEY,
            "fingerprint": IDENTITY_KEY[:16].upper(),
        },
        "health_before": {
            "schema": 1,
            "ok": True,
            "cmd": "health",
            "build_commit": COMMIT,
            "release_profile": "core_1_0",
            "sd_history_mode": "conditional",
            "boot_nonce": 77,
            "board_ready": True,
            "ui_ready": True,
        },
        "mesh_status": {
            "schema": 1,
            "ok": True,
            "cmd": "mesh status",
            "release_profile": "core_1_0",
            "sd_history_mode": "conditional",
            "identity_ready": True,
            "radio_ready": True,
            "advert_tx": {
                "queued": 1,
                "done": 1,
                "failed": 0,
                "boot_queued": 1,
                "boot_done": 1,
                "boot_failed": 0,
                "last_boot": True,
                "last_flood": True,
                "last_node_name": "D1L",
                "last_public_key_prefix": IDENTITY_KEY[:16],
                "boot_flood": True,
                "boot_node_name": "D1L",
                "boot_public_key_prefix": IDENTITY_KEY[:16],
            },
        },
        "peer_advert": control(
            "radio.advert",
            f"rc1-advert-{NONCE}",
            {"flood": False},
            {"sent": True, "flood": False},
        ),
        "contacts": {
            "schema": 1,
            "ok": True,
            "cmd": "contacts",
            "entries": [
                {
                    "fingerprint": PEER_FP,
                    "public_key": PEER_KEY,
                    "type": "chat",
                    "canonical": True,
                    "can_dm": True,
                    "can_admin": False,
                },
                {
                    "fingerprint": ADMIN_FP,
                    "public_key": ADMIN_KEY,
                    "type": "repeater",
                    "canonical": True,
                    "can_dm": False,
                    "can_admin": True,
                },
            ],
        },
        "trace_request": {
            "schema": 1,
            "ok": True,
            "cmd": "routes trace contact",
            "fingerprint": PEER_FP,
            "queued": True,
            "pending": True,
            "tag": 101,
            "targeted_trace_rf_tx": True,
            "public_rf_tx": False,
        },
        "trace_result": matched_trace(
            "routes trace status", PEER_FP, 101, False
        ),
        "peer_before": peer_capture(channel=10, dm=5),
        "public_tx_authorization": {
            "schema": 1,
            "ok": True,
            "authorized": True,
            "source": "cli_flag",
            "bounded_public_tx_count": 1,
        },
        "public_send": {
            "schema": 1,
            "ok": True,
            "cmd": "mesh send public",
            "queued": True,
            "text": PUBLIC_OUT,
        },
        "public_tx_record": {
            "schema": 1,
            "ok": True,
            "cmd": "packets search",
            "entries": [
                {
                    "direction": "tx",
                    "kind": "channel_text",
                    "note": PUBLIC_OUT,
                }
            ],
        },
        "peer_after_public": peer_capture(
            channel=11, dm=5, sender=IDENTITY_KEY[:12].upper()
        ),
        "peer_public_send": control(
            "radio.send_channel",
            f"rc1-public-{NONCE}",
            {"channel": 0, "text": PUBLIC_IN},
            {
                "channel": 0,
                "utf8_bytes": len(PUBLIC_IN.encode()),
                "delivery": {
                    "event": "SENT",
                    "payload": {},
                    "acknowledged": True,
                },
            },
        ),
        "public_receive": {
            "schema": 1,
            "ok": True,
            "cmd": "messages public",
            "entries": [{"text": PUBLIC_IN, "direction": "rx"}],
        },
        "peer_before_dm": peer_capture(channel=11, dm=5),
        "dm_send": {
            "schema": 1,
            "ok": True,
            "cmd": "mesh send dm",
            "queued": True,
            "fingerprint": PEER_FP,
        },
        "dm_ack": {
            "schema": 1,
            "ok": True,
            "cmd": "messages dm",
            "fingerprint": PEER_FP,
            "entries": [
                {
                    "text": DM_OUT,
                    "direction": "tx",
                    "acked": True,
                    "delivered": True,
                    "ack_hash": 42,
                }
            ],
        },
        "peer_after_dm": peer_capture(channel=11, dm=6),
        "peer_dm_send": control(
            "radio.send_dm",
            f"rc1-dm-{NONCE}",
            {"target": IDENTITY_KEY, "text": DM_IN},
            {
                "target": IDENTITY_KEY[:12],
                "utf8_bytes": len(DM_IN.encode()),
                "delivery": {
                    "event": "ACK",
                    "payload": {},
                    "acknowledged": True,
                },
            },
        ),
        "dm_receive_ack": {
            "schema": 1,
            "ok": True,
            "cmd": "messages dm",
            "fingerprint": PEER_FP,
            "entries": [
                {
                    "text": DM_IN,
                    "direction": "rx",
                    "ack_response": {
                        "identity_valid": True,
                        "state": "sent",
                        "dispatch_count": 1,
                        "last_kind": "direct_ack",
                        "last_error": "ESP_OK",
                    },
                }
            ],
        },
        "path_request": {
            "schema": 1,
            "ok": True,
            "cmd": "routes probe",
            "fingerprint": PEER_FP,
            "queued": True,
            "token": "path_0000002A",
            "dm_rf_tx": True,
            "public_rf_tx": False,
            "telemetry_requested": True,
        },
        "path_result": {
            "schema": 1,
            "ok": True,
            "cmd": "routes telemetry",
            "fingerprint": PEER_FP,
            "state": "received",
            "pending": False,
            "pending_tag": 0,
            "history_count": 1,
            "entries": [{"sequence": 1, "tag": 42}],
        },
        "admin_login_request": login_pending,
        "admin_login_status": login_status,
        "admin_query_request": query_pending,
        "admin_query_status": query_status,
        "admin_logout": logout,
        "ping_request": {
            "schema": 1,
            "ok": True,
            "cmd": "repeater ping",
            "fingerprint": ADMIN_FP,
            "queued": True,
            "pending": True,
            "tag": 202,
            "zero_hop": True,
            "targeted_trace_rf_tx": True,
            "public_rf_tx": False,
        },
        "ping_result": matched_trace(
            "repeater ping status", ADMIN_FP, 202, True
        ),
        "health_after": {
            "schema": 1,
            "ok": True,
            "cmd": "health",
            "build_commit": COMMIT,
            "release_profile": "core_1_0",
            "sd_history_mode": "conditional",
            "boot_nonce": 77,
            "board_ready": True,
            "ui_ready": True,
        },
        "crashlog": {
            "schema": 1,
            "ok": True,
            "cmd": "crashlog",
            "entries": [],
        },
    }
    commands = {
        "version": "version",
        "identity": "identity status",
        "health_before": "health",
        "mesh_status": "mesh status",
        "peer_advert": "controlled-peer radio.advert",
        "contacts": "contacts",
        "trace_request": f"routes trace contact {PEER_FP}",
        "trace_result": "routes trace status",
        "peer_before": "controlled-peer status capture",
        "public_tx_authorization": "operator flag --authorize-public-tx",
        "public_send": f"mesh send public {PUBLIC_OUT}",
        "public_tx_record": f"packets search {PUBLIC_OUT}",
        "peer_after_public": "controlled-peer status capture",
        "peer_public_send": "controlled-peer radio.send_channel",
        "public_receive": f"messages public search {PUBLIC_IN}",
        "peer_before_dm": "controlled-peer status capture",
        "dm_send": f"mesh send dm {PEER_FP} {DM_OUT}",
        "dm_ack": f"messages dm {PEER_FP}",
        "peer_after_dm": "controlled-peer status capture",
        "peer_dm_send": "controlled-peer radio.send_dm",
        "dm_receive_ack": f"messages dm {PEER_FP}",
        "path_request": f"routes probe {PEER_FP}",
        "path_result": f"routes telemetry {PEER_FP}",
        "admin_login_request": f"admin login {ADMIN_FP} <redacted>",
        "admin_login_status": "admin status",
        "admin_query_request": "admin telemetry",
        "admin_query_status": "admin status",
        "admin_logout": "admin logout",
        "ping_request": f"repeater ping {ADMIN_FP}",
        "ping_result": "repeater ping status",
        "health_after": "health",
        "crashlog": "crashlog",
    }
    return {
        "schema": 1,
        "kind": gate.PROTOCOL_KIND,
        "mode": "hardware",
        "physical_observed": True,
        "simulated": False,
        "dry_run": False,
        "manual_only": False,
        "port": gate.POSIX_D1L_TARGET,
        "d1l_target": {},
        "d1l_target_after": {},
        "runner_commit": COMMIT,
        "runner_source_clean": True,
        "expected_firmware_commit": COMMIT,
        "github_actions_run": RUN,
        "workflow_run_attempt": ATTEMPT,
        "steps": [
            {
                "sequence": sequence,
                "operation": operation,
                "command": commands[operation],
                "response": responses[operation],
            }
            for sequence, operation in enumerate(OPERATIONS, 1)
        ],
    }


def validate(
    transcript: dict, monkeypatch: pytest.MonkeyPatch
) -> dict[str, bool | int]:
    monkeypatch.setattr(gate, "_target_pair", lambda _data: True)
    return gate.validate_protocol(transcript, CANDIDATE)


def test_machine_transcript_closes_the_protocol_gate(
    monkeypatch: pytest.MonkeyPatch,
):
    assert set(OPERATIONS) == gate.PROTOCOL_OPERATIONS
    assert validate(protocol_transcript(), monkeypatch) == {
        "boot_advert": True,
        "public_send_count": 1,
        "path": True,
        "trace": True,
        "ping": True,
        "repeater_login": True,
        "repeater_query": True,
    }


@pytest.mark.parametrize(
    ("operation", "mutation"),
    [
        (
            "mesh_status",
            lambda response: response["advert_tx"].update(
                {"boot_node_name": "Unknown"}
            ),
        ),
        (
            "health_after",
            lambda response: response.update({"boot_nonce": 78}),
        ),
        (
            "path_result",
            lambda response: response["entries"][0].update(
                {"tag": "path_0000002A"}
            ),
        ),
        (
            "public_tx_authorization",
            lambda response: response.update({"authorized": False}),
        ),
    ],
)
def test_machine_transcript_rejects_unproven_claims(
    operation: str,
    mutation,
    monkeypatch: pytest.MonkeyPatch,
):
    transcript = protocol_transcript()
    response = next(
        row["response"]
        for row in transcript["steps"]
        if row["operation"] == operation
    )
    mutation(response)
    with pytest.raises(gate.EvidenceError):
        validate(transcript, monkeypatch)


def test_machine_transcript_never_contains_the_admin_password(
    monkeypatch: pytest.MonkeyPatch,
):
    transcript = protocol_transcript()
    login = next(
        row
        for row in transcript["steps"]
        if row["operation"] == "admin_login_request"
    )
    login["command"] = f"admin login {ADMIN_FP} leaked-password"
    with pytest.raises(gate.EvidenceError):
        validate(transcript, monkeypatch)


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("runner_commit", "b" * 40),
        ("runner_source_clean", False),
    ],
)
def test_machine_transcript_rejects_wrong_runner_source(
    field: str,
    value,
    monkeypatch: pytest.MonkeyPatch,
):
    transcript = protocol_transcript()
    transcript[field] = value

    with pytest.raises(gate.EvidenceError):
        validate(transcript, monkeypatch)


def test_zero_byte_admin_password_file_is_valid(tmp_path: Path):
    password_file = tmp_path / "admin-password"
    password_file.write_bytes(b"")

    assert runner.load_admin_password(password_file) == ""


def test_public_tx_requires_explicit_operator_authorization():
    with pytest.raises(runner.ProtocolAcceptanceError):
        runner.require_public_tx_authorization(False)

    assert runner.require_public_tx_authorization(True) is None


def test_admin_login_failure_never_exposes_wire_password(
    monkeypatch: pytest.MonkeyPatch,
):
    password = "private-login"
    wire = f"admin login {ADMIN_FP} {password}"
    monkeypatch.setattr(
        runner,
        "send_console_command",
        lambda *_args: {
            "ok": False,
            "cmd": "admin login",
            "code": "TIMEOUT",
            "debug": wire,
        },
    )

    with pytest.raises(runner.ProtocolAcceptanceError) as caught:
        runner.checked_console_command(
            object(),
            wire,
            1.0,
            failure_label=f"admin login {ADMIN_FP} <redacted>",
        )

    assert password not in str(caught.value)
    assert "<redacted>" in str(caught.value)


def test_runner_is_pi_only_stable_by_id_and_self_validating():
    source = Path(runner.__file__).read_text(encoding="utf-8")

    assert 'if os.name != "posix":' in source
    assert "socket.gethostname() != PI_HOST" in source
    assert "port=POSIX_D1L_TARGET" in source
    assert '"manual_only": False' in source
    assert "--authorize-public-tx" in source
    assert "validate_protocol(transcript, candidate)" in source
    assert "--dry-run" not in source
    assert source.index(
        'label="inbound DM ACK dispatch"'
    ) < source.index('label="matched contact TRACE"')
