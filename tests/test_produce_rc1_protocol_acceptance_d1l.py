import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

import pytest

from scripts import produce_rc1_bounded_physical_receipt_d1l as gate
from scripts import produce_rc1_protocol_acceptance_d1l as runner


COMMIT = "a" * 40
RUN = "123"
ATTEMPT = "1"
IDENTITY_KEY = "b" * 64
PEER_KEY = gate.PEER_PROFILE_BINDINGS[
    gate.RADIO_LISTENER_STATUS_SCHEMA
]["public_key"]
ADMIN_KEY = "d" * 64
PEER_FP = PEER_KEY[:16].upper()
ADMIN_FP = ADMIN_KEY[:16].upper()
NONCE = "123456789abc"
PUBLIC_OUT = f"rc1-public-out-{COMMIT[:8]}-{NONCE}"
PUBLIC_IN = f"rc1-public-in-{COMMIT[:8]}-{NONCE}"
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
    "admin_login_request",
    "admin_login_status",
    "admin_query_request",
    "admin_query_status",
    "admin_logout",
    "path_request",
    "path_result",
    "ping_request",
    "ping_result",
    "peer_before",
    "public_tx_authorization",
    "public_send",
    "public_tx_record",
    "peer_after_public",
    "peer_public_send",
    "public_receive",
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
                    "verification_source": "signed_advert",
                },
                {
                    "fingerprint": ADMIN_FP,
                    "public_key": ADMIN_KEY,
                    "type": "repeater",
                    "canonical": True,
                    "can_dm": False,
                    "can_admin": True,
                    "verification_source": "signed_advert",
                },
            ],
        },
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
        "path_request": {
            "schema": 1,
            "ok": True,
            "cmd": "routes probe",
            "fingerprint": ADMIN_FP,
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
            "fingerprint": ADMIN_FP,
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
        "peer_before": "controlled-peer status capture",
        "public_tx_authorization": "operator flag --authorize-public-tx",
        "public_send": f"mesh send public {PUBLIC_OUT}",
        "public_tx_record": f"packets search {PUBLIC_OUT}",
        "peer_after_public": "controlled-peer status capture",
        "peer_public_send": "controlled-peer radio.send_channel",
        "public_receive": f"messages public search {PUBLIC_IN}",
        "path_request": f"routes probe {ADMIN_FP}",
        "path_result": f"routes telemetry {ADMIN_FP}",
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
        "controlled_peer": dict(
            gate.PEER_PROFILE_BINDINGS[
                gate.RADIO_LISTENER_STATUS_SCHEMA
            ]
        ),
        "protocol_targets": {
            "admin_fingerprint": ADMIN_FP,
        },
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
    transcript = protocol_transcript()
    assert validate(transcript, monkeypatch) == {
        "boot_advert": True,
        "public_send_count": 1,
        "path": True,
        "ping": True,
        "repeater_login": True,
        "repeater_query": True,
    }


def test_admin_path_and_ping_all_precede_public_send():
    transcript = protocol_transcript()
    steps = {
        row["operation"]: row
        for row in transcript["steps"]
    }

    assert (
        steps["contacts"]["sequence"]
        < steps["admin_login_request"]["sequence"]
        < steps["admin_login_status"]["sequence"]
        < steps["admin_query_request"]["sequence"]
        < steps["admin_query_status"]["sequence"]
        < steps["admin_logout"]["sequence"]
        < steps["path_request"]["sequence"]
        < steps["path_result"]["sequence"]
        < steps["ping_request"]["sequence"]
        < steps["ping_result"]["sequence"]
        < steps["peer_before"]["sequence"]
        < steps["public_tx_authorization"]["sequence"]
        < steps["public_send"]["sequence"]
    )
    assert steps["path_request"]["command"] == f"routes probe {ADMIN_FP}"
    assert steps["path_request"]["response"]["fingerprint"] == ADMIN_FP
    assert (
        steps["path_result"]["command"]
        == f"routes telemetry {ADMIN_FP}"
    )
    assert steps["path_result"]["response"]["fingerprint"] == ADMIN_FP
    assert steps["ping_request"]["command"] == f"repeater ping {ADMIN_FP}"
    assert steps["ping_request"]["response"]["fingerprint"] == ADMIN_FP


def test_protocol_rejects_legacy_public_before_admin_order(
    monkeypatch: pytest.MonkeyPatch,
):
    transcript = protocol_transcript()
    steps_by_operation = {
        row["operation"]: row for row in transcript["steps"]
    }
    public_operations = (
        "peer_before",
        "public_tx_authorization",
        "public_send",
        "public_tx_record",
        "peer_after_public",
        "peer_public_send",
        "public_receive",
    )
    legacy_order = [
        operation
        for operation in OPERATIONS
        if operation not in public_operations
    ]
    insertion = legacy_order.index("admin_login_request")
    legacy_order[insertion:insertion] = public_operations
    transcript["steps"] = [
        {
            **steps_by_operation[operation],
            "sequence": sequence,
        }
        for sequence, operation in enumerate(legacy_order, 1)
    ]

    with pytest.raises(gate.EvidenceError):
        validate(transcript, monkeypatch)


def test_protocol_transcript_excludes_the_rf_receipts_dm_exchange(
    monkeypatch: pytest.MonkeyPatch,
):
    transcript = protocol_transcript()
    operations = {row["operation"] for row in transcript["steps"]}
    commands = [row["command"] for row in transcript["steps"]]

    assert operations.isdisjoint(
        {
            "peer_before_dm",
            "dm_send",
            "dm_ack",
            "peer_after_dm",
            "peer_dm_send",
            "dm_receive_ack",
        }
    )
    assert not any(command.startswith("mesh send dm ") for command in commands)
    assert not any(command.startswith("messages dm ") for command in commands)
    assert "controlled-peer radio.send_dm" not in commands
    assert gate.COVERAGE["dm_ack"] == "rf"
    assert "dm_ack" not in validate(transcript, monkeypatch)

    transcript["steps"].append(
        {
            "sequence": len(transcript["steps"]) + 1,
            "operation": "dm_send",
            "command": f"mesh send dm {PEER_FP} redundant",
            "response": {"schema": 1, "ok": True, "cmd": "mesh send dm"},
        }
    )
    with pytest.raises(gate.EvidenceError):
        validate(transcript, monkeypatch)


def _meshcorebot_transcript() -> dict:
    transcript = protocol_transcript()
    binding = dict(
        gate.PEER_PROFILE_BINDINGS[gate.MESHCOREBOT_STATUS_SCHEMA]
    )
    meshcorebot_fingerprint = binding["public_key"][:16].upper()
    transcript = json.loads(
        json.dumps(transcript)
        .replace(PEER_KEY, binding["public_key"])
        .replace(PEER_FP, meshcorebot_fingerprint)
    )
    transcript["controlled_peer"] = binding
    peer_before_index = next(
        index
        for index, step in enumerate(transcript["steps"])
        if step["operation"] == "peer_before"
    )
    transcript["steps"][peer_before_index + 1 : peer_before_index + 1] = [
        {
            "sequence": 0,
            "operation": "d1l_advert",
            "command": "mesh advert flood",
            "response": {
                "schema": 1,
                "ok": True,
                "cmd": "mesh advert flood",
                "queued": True,
                "flood": True,
            },
        },
        {
            "sequence": 0,
            "operation": "peer_resolve_d1l",
            "command": "controlled-peer radio.resolve_contact D1L",
            "response": control(
                "radio.resolve_contact",
                f"rc1-resolve-{NONCE}-001",
                {"name": "D1L"},
                {
                    "name": "D1L",
                    "match_count": 1,
                    "unique": True,
                    "valid_signed_advert": True,
                    "public_key_prefix": IDENTITY_KEY[:12].lower(),
                    "last_advert": 123456,
                },
            ),
        },
    ]
    for sequence, step in enumerate(transcript["steps"], 1):
        step["sequence"] = sequence
    for step in transcript["steps"]:
        response = step["response"]
        if step["operation"] == "peer_advert":
            response["response"]["result"] = {
                "flood": False,
                "delivery": {
                    "event": "SENT",
                    "payload": {},
                    "acknowledged": True,
                },
            }
        if step["operation"] not in {
            "peer_before",
            "peer_after_public",
        }:
            continue
        old = response["snapshot"]
        mesh = dict(old["mesh"])
        if step["operation"] == "peer_after_public":
            mesh.update(
                {
                    "last_rx_sender_source": "unique_signed_advert_name",
                    "last_rx_sender_name": "D1L",
                    "last_rx_sender_advert_timestamp": 123456,
                }
            )
        snapshot = {
            "service": "meshcorebot",
            "pid": 17,
            "started_at": "2026-07-25T19:00:00Z",
            "status_written_at": old["status_written_at"],
            "serial": {
                "active_port": binding["device"],
                "configured_port": binding["device"],
                "hardware_id": gate.MESHCOREBOT_HARDWARE_ID,
                "baud_rate": gate.MESHCOREBOT_BAUD,
                "meshcore_connected": True,
            },
            "discord": {"connected": True},
            "mqtt": {"device_public_key": binding["public_key"].upper()},
            "counters": {
                "rx_channel_total": old["counters"]["rx_channel_total"],
                "rx_contact_total": old["counters"]["rx_dm_total"],
            },
            "mesh": {
                **mesh,
                "last_poll_at": old["status_written_at"],
            },
        }
        response["path"] = binding["status_path"]
        response["snapshot"] = snapshot
        response["snapshot_sha256"] = hashlib.sha256(
            gate.canonical_json(snapshot)
        ).hexdigest()
    return transcript


def test_meshcorebot_profile_closes_chat_gate_with_distinct_admin(
    monkeypatch: pytest.MonkeyPatch,
):
    transcript = _meshcorebot_transcript()

    assert validate(transcript, monkeypatch) == {
        "boot_advert": True,
        "public_send_count": 1,
        "path": True,
        "ping": True,
        "repeater_login": True,
        "repeater_query": True,
    }

    peer = transcript["controlled_peer"]
    login = next(
        step
        for step in transcript["steps"]
        if step["operation"] == "admin_login_request"
    )
    assert peer["public_key"][:16].upper() != ADMIN_FP
    assert ADMIN_FP in login["command"]


def _operation_response(transcript: dict, operation: str) -> dict:
    return next(
        step["response"]
        for step in transcript["steps"]
        if step["operation"] == operation
    )


def _rehash_peer_capture(capture: dict) -> None:
    capture["snapshot_sha256"] = hashlib.sha256(
        gate.canonical_json(capture["snapshot"])
    ).hexdigest()


@pytest.mark.parametrize(
    "mutation",
    [
        "mixed_socket",
        "changed_pid",
        "changed_started_at",
        "wrong_mqtt_key",
        "wrong_hardware",
        "wrong_baud",
        "stale_poll",
        "wrong_sender_source",
        "wrong_sender_name",
        "missing_sender_advert_timestamp",
        "listener_advert_shape",
        "resolve_wrong_prefix",
        "resolve_not_unique",
        "resolve_timestamp_mismatch",
        "resolve_after_public",
        "chat_peer_admin_capable",
        "admin_not_admin_capable",
        "chat_peer_not_signed",
        "admin_not_signed",
    ],
)
def test_meshcorebot_profile_rejects_identity_or_correlation_drift(
    mutation: str, monkeypatch: pytest.MonkeyPatch
):
    transcript = _meshcorebot_transcript()
    if mutation == "mixed_socket":
        transcript["controlled_peer"]["control_socket"] = (
            gate.PEER_PROFILE_BINDINGS[
                gate.RADIO_LISTENER_STATUS_SCHEMA
            ]["control_socket"]
        )
    elif mutation in {"changed_pid", "changed_started_at"}:
        capture = _operation_response(transcript, "peer_after_public")
        if mutation == "changed_pid":
            capture["snapshot"]["pid"] += 1
        else:
            capture["snapshot"]["started_at"] = "2026-07-25T19:00:01Z"
        _rehash_peer_capture(capture)
    elif mutation in {
        "wrong_mqtt_key",
        "wrong_hardware",
        "wrong_baud",
        "stale_poll",
    }:
        capture = _operation_response(transcript, "peer_before")
        if mutation == "wrong_mqtt_key":
            capture["snapshot"]["mqtt"]["device_public_key"] = "e" * 64
        elif mutation == "wrong_hardware":
            capture["snapshot"]["serial"]["hardware_id"] = "0000:0000"
        elif mutation == "wrong_baud":
            capture["snapshot"]["serial"]["baud_rate"] = 9600
        else:
            capture["snapshot"]["mesh"]["last_poll_at"] = (
                "2026-07-25T19:00:00Z"
            )
        _rehash_peer_capture(capture)
    elif mutation.startswith("wrong_sender") or mutation.startswith(
        "missing_sender"
    ):
        capture = _operation_response(transcript, "peer_after_public")
        mesh = capture["snapshot"]["mesh"]
        if mutation == "wrong_sender_source":
            mesh["last_rx_sender_source"] = "untrusted_name"
        elif mutation == "wrong_sender_name":
            mesh["last_rx_sender_name"] = "not-D1L"
        else:
            mesh.pop("last_rx_sender_advert_timestamp")
        _rehash_peer_capture(capture)
    elif mutation == "listener_advert_shape":
        advert = _operation_response(transcript, "peer_advert")
        advert["response"]["result"] = {"sent": True, "flood": False}
    elif mutation.startswith("resolve_"):
        resolution = _operation_response(
            transcript, "peer_resolve_d1l"
        )["response"]["result"]
        if mutation == "resolve_wrong_prefix":
            resolution["public_key_prefix"] = "0" * 12
        elif mutation == "resolve_not_unique":
            resolution["unique"] = False
        elif mutation == "resolve_timestamp_mismatch":
            resolution["last_advert"] += 1
        else:
            d1l_advert = next(
                step
                for step in transcript["steps"]
                if step["operation"] == "d1l_advert"
            )
            public_send = next(
                step
                for step in transcript["steps"]
                if step["operation"] == "public_send"
            )
            d1l_advert["sequence"], public_send["sequence"] = (
                public_send["sequence"],
                d1l_advert["sequence"],
            )
    else:
        contacts = _operation_response(transcript, "contacts")["entries"]
        if mutation == "chat_peer_admin_capable":
            contacts[0]["can_admin"] = True
        elif mutation == "admin_not_admin_capable":
            contacts[1]["can_admin"] = False
        elif mutation == "chat_peer_not_signed":
            contacts[0]["verification_source"] = "imported"
        else:
            contacts[1]["verification_source"] = "imported"

    with pytest.raises(gate.EvidenceError):
        validate(transcript, monkeypatch)


def test_meshcorebot_capture_requires_exact_identity_and_session(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    status_path = tmp_path / "meshcorebot.status.json"
    now = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
    status = {
        "service": runner.MESHCOREBOT_PEER_SERVICE,
        "pid": 19,
        "started_at": now,
        "status_written_at": now,
        "serial": {
            "active_port": runner.MESHCOREBOT_PEER_DEVICE,
            "configured_port": runner.MESHCOREBOT_PEER_DEVICE,
            "hardware_id": runner.MESHCOREBOT_HARDWARE_ID,
            "baud_rate": runner.MESHCOREBOT_BAUD,
            "meshcore_connected": True,
        },
        "discord": {"connected": True},
        "mqtt": {
            "device_public_key": runner.MESHCOREBOT_PEER_PUBLIC_KEY.upper()
        },
        "counters": {"rx_channel_total": 10, "rx_contact_total": 5},
        "mesh": {"last_poll_at": now},
    }
    status_path.write_text(json.dumps(status), encoding="utf-8")
    monkeypatch.setattr(runner, "MESHCOREBOT_PEER_STATUS", status_path)

    captured = runner.capture_peer_status(
        status_path,
        expected_public_key=runner.MESHCOREBOT_PEER_PUBLIC_KEY,
        expected_device=runner.MESHCOREBOT_PEER_DEVICE,
        expected_service=runner.MESHCOREBOT_PEER_SERVICE,
        status_schema=runner.MESHCOREBOT_STATUS_SCHEMA,
    )

    assert runner.peer_session_identity(
        captured, runner.MESHCOREBOT_STATUS_SCHEMA
    ) == ("pid_started_at", 19, now)
    assert (
        runner.peer_counter(
            captured, "rx_dm_total", runner.MESHCOREBOT_STATUS_SCHEMA
        )
        == 5
    )

    status["serial"]["active_port"] = "/dev/not-the-peer"
    status_path.write_text(json.dumps(status), encoding="utf-8")
    with pytest.raises(runner.ProtocolAcceptanceError):
        runner.capture_peer_status(
            status_path,
            expected_public_key=runner.MESHCOREBOT_PEER_PUBLIC_KEY,
            expected_device=runner.MESHCOREBOT_PEER_DEVICE,
            expected_service=runner.MESHCOREBOT_PEER_SERVICE,
            status_schema=runner.MESHCOREBOT_STATUS_SCHEMA,
        )


def test_peer_status_json_array_fails_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    status_path = tmp_path / "meshcorebot.status.json"
    status_path.write_text("[]", encoding="ascii")
    monkeypatch.setattr(runner, "MESHCOREBOT_PEER_STATUS", status_path)

    with pytest.raises(runner.ProtocolAcceptanceError):
        runner.capture_peer_status(
            status_path,
            expected_public_key=runner.MESHCOREBOT_PEER_PUBLIC_KEY,
            expected_device=runner.MESHCOREBOT_PEER_DEVICE,
            expected_service=runner.MESHCOREBOT_PEER_SERVICE,
            status_schema=runner.MESHCOREBOT_STATUS_SCHEMA,
        )


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


def test_admin_session_retries_once_after_exact_timeout(capsys):
    wire = f"admin login {ADMIN_FP} private-login"
    redacted = f"admin login {ADMIN_FP} <redacted>"
    calls = []
    poll_calls = 0

    def command(value, *, failure_label=None):
        calls.append((value, failure_label))
        if value.startswith("admin login "):
            return {
                "ok": True,
                "cmd": "admin login",
                "state": "login_pending",
                "attempt": sum(
                    call[0].startswith("admin login ") for call in calls
                ),
            }
        assert value == "admin status"
        return {
            "ok": True,
            "cmd": "admin status",
            "state": "timed_out",
            "fingerprint": ADMIN_FP.lower(),
            "last_error": "ESP_ERR_TIMEOUT",
            "credential_exposed": False,
            "session_secret_exposed": False,
        }

    def poll_function(*_args, **_kwargs):
        nonlocal poll_calls
        poll_calls += 1
        if poll_calls == 1:
            raise runner.ProtocolAcceptanceError(
                "timed out waiting for authenticated repeater session: {}"
            )
        return {
            "ok": True,
            "cmd": "admin status",
            "state": "authenticated",
            "fingerprint": ADMIN_FP.lower(),
            "credential_exposed": False,
            "session_secret_exposed": False,
        }

    login, status = runner.authenticate_admin_session(
        command,
        login_wire_command=wire,
        redacted_login_command=redacted,
        admin_fingerprint=ADMIN_FP,
        timeout=1.0,
        interval=0.0,
        poll_function=poll_function,
    )

    assert login["attempt"] == 2
    assert status["state"] == "authenticated"
    assert poll_calls == 2
    assert calls == [
        (wire, redacted),
        ("admin status", None),
        (wire, redacted),
    ]
    captured = capsys.readouterr()
    assert "bounded admin login retry" in captured.err
    assert "private-login" not in captured.err


def test_admin_session_accepts_late_authenticated_terminal_without_retry():
    wire = f"admin login {ADMIN_FP} private-login"
    calls = []

    def command(value, *, failure_label=None):
        calls.append((value, failure_label))
        if value.startswith("admin login "):
            return {"ok": True, "cmd": "admin login"}
        return {
            "ok": True,
            "cmd": "admin status",
            "state": "authenticated",
            "fingerprint": ADMIN_FP.lower(),
            "credential_exposed": False,
            "session_secret_exposed": False,
        }

    def poll_function(*_args, **_kwargs):
        raise runner.ProtocolAcceptanceError(
            "timed out waiting for authenticated repeater session: {}"
        )

    _login, status = runner.authenticate_admin_session(
        command,
        login_wire_command=wire,
        redacted_login_command=f"admin login {ADMIN_FP} <redacted>",
        admin_fingerprint=ADMIN_FP,
        timeout=1.0,
        interval=0.0,
        poll_function=poll_function,
    )

    assert status["state"] == "authenticated"
    assert [value for value, _label in calls] == [wire, "admin status"]


@pytest.mark.parametrize(
    ("state", "last_error", "credential_exposed"),
    [
        ("rejected", "ESP_ERR_INVALID_STATE", False),
        ("timed_out", "ESP_ERR_TIMEOUT", True),
        ("timed_out", "ESP_ERR_INVALID_STATE", False),
    ],
)
def test_admin_session_does_not_retry_unapproved_terminal_state(
    state: str,
    last_error: str,
    credential_exposed: bool,
):
    wire = f"admin login {ADMIN_FP} private-login"
    calls = []

    def command(value, *, failure_label=None):
        calls.append((value, failure_label))
        if value.startswith("admin login "):
            return {"ok": True, "cmd": "admin login"}
        return {
            "ok": True,
            "cmd": "admin status",
            "state": state,
            "fingerprint": ADMIN_FP.lower(),
            "last_error": last_error,
            "credential_exposed": credential_exposed,
            "session_secret_exposed": False,
        }

    def poll_function(*_args, **_kwargs):
        raise runner.ProtocolAcceptanceError(
            "timed out waiting for authenticated repeater session: {}"
        )

    with pytest.raises(runner.ProtocolAcceptanceError):
        runner.authenticate_admin_session(
            command,
            login_wire_command=wire,
            redacted_login_command=f"admin login {ADMIN_FP} <redacted>",
            admin_fingerprint=ADMIN_FP,
            timeout=1.0,
            interval=0.0,
            poll_function=poll_function,
        )

    assert [value for value, _label in calls] == [wire, "admin status"]


def test_admin_session_stops_after_one_retry():
    wire = f"admin login {ADMIN_FP} private-login"
    calls = []

    def command(value, *, failure_label=None):
        calls.append((value, failure_label))
        if value.startswith("admin login "):
            return {"ok": True, "cmd": "admin login"}
        return {
            "ok": True,
            "cmd": "admin status",
            "state": "timed_out",
            "fingerprint": ADMIN_FP.lower(),
            "last_error": "ESP_ERR_TIMEOUT",
            "credential_exposed": False,
            "session_secret_exposed": False,
        }

    def poll_function(*_args, **_kwargs):
        raise runner.ProtocolAcceptanceError(
            "timed out waiting for authenticated repeater session: {}"
        )

    with pytest.raises(runner.ProtocolAcceptanceError):
        runner.authenticate_admin_session(
            command,
            login_wire_command=wire,
            redacted_login_command=f"admin login {ADMIN_FP} <redacted>",
            admin_fingerprint=ADMIN_FP,
            timeout=1.0,
            interval=0.0,
            poll_function=poll_function,
        )

    assert [value for value, _label in calls] == [
        wire,
        "admin status",
        wire,
        "admin status",
    ]


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


def test_console_readiness_retries_a_command_lost_before_init(
    monkeypatch: pytest.MonkeyPatch,
):
    responses = [
        {"schema": 1, "ok": False, "cmd": "health", "code": "TIMEOUT"},
        {
            "schema": 1,
            "ok": True,
            "cmd": "health",
            "board_ready": True,
            "ui_ready": True,
        },
    ]
    calls = []

    def send(_ser, command, timeout):
        calls.append((command, timeout))
        return responses.pop(0)

    monkeypatch.setattr(runner, "send_console_command", send)

    result = runner.wait_for_console_ready(
        object(), timeout=1.0, command_timeout=0.1, poll_interval=0
    )

    assert result["ok"] is True
    assert [command for command, _timeout in calls] == ["health", "health"]


def test_runner_is_pi_only_stable_by_id_and_self_validating():
    source = Path(runner.__file__).read_text(encoding="utf-8")

    assert 'if os.name != "posix":' in source
    assert "socket.gethostname() != PI_HOST" in source
    assert "port=POSIX_D1L_TARGET" in source
    assert '"manual_only": False' in source
    assert "--authorize-public-tx" in source
    assert "--trace-fingerprint" not in source
    assert "validate_protocol(transcript, candidate)" in source
    assert "--dry-run" not in source
    assert 'parser.add_argument("--boot-timeout", type=float, default=75.0)' in source
    assert '"dm_send"' not in source
    assert "mesh send dm" not in source
    assert "messages dm" not in source
    assert "radio.send_dm" not in source
    assert source.index('"cold boot console readiness"') < source.index(
        'version = _step(steps, "version"'
    )
    assert (
        source.index('admin_logout = _step(')
        < source.index('\n        path_request = _step(')
        < source.index('label="PATH/base telemetry response"')
        < source.index('ping_request = _step(')
        < source.index('\n        before = _step(')
        < source.index('\n            "public_tx_authorization",')
    )
