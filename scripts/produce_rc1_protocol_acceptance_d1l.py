#!/usr/bin/env python3
"""Produce the machine-observed RC1 MeshCore protocol acceptance transcript.

The runner is intentionally Pi-only.  It opens only the stable D1L by-id
endpoint, uses one explicitly identified local controlled peer, and reads the
admin password from a file without placing it in argv or the transcript.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import socket
import stat
import sys
import time
from collections.abc import Callable, Iterable
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from artifact_metadata import git_metadata
    from d1l_serial_target import (
        POSIX_D1L_TARGET,
        resolve_target,
        validate_snapshot,
    )
    from produce_rc1_bounded_physical_receipt_d1l import (
        PROTOCOL_KIND,
        validate_protocol,
    )
    from smoke_d1l import (
        exact_commit,
        open_d1l_serial,
        send_console_command,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import git_metadata
    from scripts.d1l_serial_target import (
        POSIX_D1L_TARGET,
        resolve_target,
        validate_snapshot,
    )
    from scripts.produce_rc1_bounded_physical_receipt_d1l import (
        PROTOCOL_KIND,
        validate_protocol,
    )
    from scripts.smoke_d1l import (
        exact_commit,
        open_d1l_serial,
        send_console_command,
    )


SCHEMA = 1
PI_HOST = "neopi5"
RELEASE_PROFILE = "core_1_0"
SD_HISTORY_MODE = "conditional"
EXPECTED_NODE_NAME = "D1L"
DEFAULT_PEER_STATUS = Path(
    "/opt/canadaverse/com15-responder/data/radio_listener.status.json"
)
DEFAULT_PEER_SOCKET = Path(
    "/run/canadaverse-control/com15/control.sock"
)
DEFAULT_PEER_DEVICE = "/dev/krab-t-echo"
DEFAULT_PEER_PUBLIC_KEY = (
    "024999dedfd26763c5606169c3ebd34e05a9475cf78220a81078b5dd27caca44"
)
MAX_PEER_STATUS_BYTES = 1024 * 1024
MAX_CONTROL_RESPONSE_BYTES = 64 * 1024
MAX_PASSWORD_BYTES = 15
MAX_STATUS_AGE_SECONDS = 120.0
COMMIT_RE = re.compile(r"[0-9a-f]{40}\Z")
PUBLIC_KEY_RE = re.compile(r"[0-9a-f]{64}\Z")
FINGERPRINT_RE = re.compile(r"[0-9A-F]{16}\Z")
POSITIVE_DECIMAL_RE = re.compile(r"[1-9][0-9]*\Z")


class ProtocolAcceptanceError(RuntimeError):
    """A fail-closed protocol acceptance error."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def canonical_json(value: object) -> bytes:
    return (
        json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        )
        + "\n"
    ).encode("ascii")


def positive_decimal(value: object, label: str) -> str:
    normalized = str(value)
    if not POSITIVE_DECIMAL_RE.fullmatch(normalized):
        raise ProtocolAcceptanceError(f"{label} must be a positive decimal")
    return normalized


def exact_public_key(value: object, label: str) -> str:
    normalized = str(value).strip().lower()
    if not PUBLIC_KEY_RE.fullmatch(normalized):
        raise ProtocolAcceptanceError(f"{label} must be exactly 64 hexadecimal characters")
    return normalized


def exact_fingerprint(value: object, label: str) -> str:
    normalized = str(value).strip().upper()
    if not FINGERPRINT_RE.fullmatch(normalized):
        raise ProtocolAcceptanceError(f"{label} must be exactly 16 hexadecimal characters")
    return normalized


def integer(value: object, *, minimum: int = 0) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        return None
    return value


def nested(value: object, *path: str) -> object:
    current = value
    for key in path:
        if not isinstance(current, dict):
            return None
        current = current.get(key)
    return current


def json_time(value: object) -> datetime | None:
    if not isinstance(value, str) or not value:
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed if parsed.tzinfo is not None else None


def _safe_regular_file(
    path: Path,
    *,
    maximum: int,
    label: str,
    minimum: int = 1,
) -> bytes:
    path = Path(path)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise ProtocolAcceptanceError(f"could not open {label}: {path}") from exc
    try:
        before = os.fstat(fd)
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_size < minimum
            or before.st_size > maximum
        ):
            raise ProtocolAcceptanceError(f"{label} is not one bounded regular file")
        chunks: list[bytes] = []
        remaining = maximum + 1
        while remaining > 0:
            chunk = os.read(fd, min(65536, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks)
        after = os.fstat(fd)
        if (
            len(raw) > maximum
            or before.st_dev != after.st_dev
            or before.st_ino != after.st_ino
            or before.st_size != after.st_size
            or before.st_mtime_ns != after.st_mtime_ns
            or len(raw) != before.st_size
        ):
            raise ProtocolAcceptanceError(f"{label} changed during capture")
        return raw
    finally:
        os.close(fd)


def load_admin_password(path: Path) -> str:
    raw = _safe_regular_file(
        path,
        maximum=MAX_PASSWORD_BYTES + 2,
        label="admin password file",
        minimum=0,
    )
    if raw.endswith(b"\r\n"):
        raw = raw[:-2]
    elif raw.endswith(b"\n"):
        raw = raw[:-1]
    try:
        value = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ProtocolAcceptanceError("admin password file is not UTF-8") from exc
    encoded = value.encode("utf-8")
    if (
        len(encoded) > MAX_PASSWORD_BYTES
        or any(char.isspace() for char in value)
        or any(ord(char) < 0x20 or ord(char) == 0x7F for char in value)
    ):
        raise ProtocolAcceptanceError(
            "admin password must be 0-15 UTF-8 bytes without whitespace or controls"
        )
    return value


def require_public_tx_authorization(authorized: bool) -> None:
    if authorized is not True:
        raise ProtocolAcceptanceError(
            "the bounded Public send requires explicit --authorize-public-tx"
        )


def capture_peer_status(
    path: Path,
    *,
    expected_public_key: str,
    max_age_seconds: float = MAX_STATUS_AGE_SECONDS,
) -> dict[str, Any]:
    raw = _safe_regular_file(
        path,
        maximum=MAX_PEER_STATUS_BYTES,
        label="controlled-peer status",
    )
    try:
        data = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolAcceptanceError("controlled-peer status is not JSON") from exc
    written = json_time(data.get("status_written_at") if isinstance(data, dict) else None)
    age = (
        (datetime.now(timezone.utc) - written.astimezone(timezone.utc)).total_seconds()
        if written is not None
        else -1.0
    )
    if not (
        isinstance(data, dict)
        and data.get("service") == "openclaw-radio-listener"
        and isinstance(data.get("run_id"), str)
        and bool(data["run_id"])
        and nested(data, "serial", "mesh_connected") is True
        and nested(data, "serial", "port") == DEFAULT_PEER_DEVICE
        and nested(data, "serial", "public_key") == expected_public_key
        and isinstance(data.get("counters"), dict)
        and 0.0 <= age <= max_age_seconds
    ):
        raise ProtocolAcceptanceError(
            "controlled-peer status identity, readiness, or freshness is invalid"
        )
    return {
        "source": "local_peer_status_file",
        "path": str(Path(path)),
        "captured_at": utc_now(),
        "snapshot_sha256": hashlib.sha256(canonical_json(data)).hexdigest(),
        "snapshot": data,
    }


def send_peer_control(
    socket_path: Path,
    *,
    request_id: str,
    operation: str,
    params: dict[str, Any],
    timeout: float = 35.0,
) -> dict[str, Any]:
    request = {
        "id": request_id,
        "op": operation,
        "params": params,
    }
    payload = canonical_json(request)
    if len(payload) > 16 * 1024:
        raise ProtocolAcceptanceError("controlled-peer request is too large")
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(timeout)
    try:
        client.connect(str(socket_path))
        client.sendall(payload)
        chunks: list[bytes] = []
        total = 0
        while total <= MAX_CONTROL_RESPONSE_BYTES:
            chunk = client.recv(min(4096, MAX_CONTROL_RESPONSE_BYTES + 1 - total))
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            if b"\n" in chunk:
                break
    except OSError as exc:
        raise ProtocolAcceptanceError(
            f"controlled-peer operation {operation} failed"
        ) from exc
    finally:
        client.close()
    raw = b"".join(chunks)
    if (
        len(raw) < 3
        or len(raw) > MAX_CONTROL_RESPONSE_BYTES
        or not raw.endswith(b"\n")
        or raw.count(b"\n") != 1
    ):
        raise ProtocolAcceptanceError("controlled-peer response framing is invalid")
    try:
        response = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolAcceptanceError("controlled-peer response is not JSON") from exc
    if not (
        isinstance(response, dict)
        and response.get("id") == request_id
        and response.get("op") == operation
        and response.get("ok") is True
        and response.get("cached") is False
        and isinstance(response.get("result"), dict)
        and response.get("error") is None
    ):
        raise ProtocolAcceptanceError(
            f"controlled-peer operation {operation} did not succeed exactly once"
        )
    return {"request": request, "response": response}


def peer_counter(status: dict[str, Any], name: str) -> int | None:
    return integer(nested(status, "snapshot", "counters", name))


def peer_run_id(status: dict[str, Any]) -> str | None:
    value = nested(status, "snapshot", "run_id")
    return value if isinstance(value, str) and value else None


def public_entry(result: object, *, text: str, direction: str) -> dict[str, Any] | None:
    entries = result.get("entries") if isinstance(result, dict) else None
    if not isinstance(entries, list):
        return None
    matches = [
        row
        for row in entries
        if isinstance(row, dict)
        and row.get("text") == text
        and row.get("direction") == direction
    ]
    return matches[0] if len(matches) == 1 else None


def dm_entry(result: object, *, text: str, direction: str) -> dict[str, Any] | None:
    return public_entry(result, text=text, direction=direction)


def contact_entry(
    result: object, *, public_key: str | None = None, fingerprint: str | None = None
) -> dict[str, Any] | None:
    entries = result.get("entries") if isinstance(result, dict) else None
    if not isinstance(entries, list):
        return None
    matches = []
    for row in entries:
        if not isinstance(row, dict):
            continue
        row_key = str(row.get("public_key") or row.get("public_key_hex") or "").lower()
        row_fingerprint = str(row.get("fingerprint") or "").upper()
        if public_key is not None and row_key != public_key:
            continue
        if fingerprint is not None and row_fingerprint != fingerprint:
            continue
        matches.append(row)
    return matches[0] if len(matches) == 1 else None


def response_booted(result: object) -> bool:
    return isinstance(result, dict) and result.get("ignored_boot_help_seen") is True


def poll(
    function: Callable[[], dict[str, Any]],
    predicate: Callable[[dict[str, Any]], bool],
    *,
    timeout: float,
    interval: float,
    label: str,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = function()
        if response_booted(last):
            raise ProtocolAcceptanceError(f"device rebooted while polling {label}")
        if predicate(last):
            return last
        time.sleep(interval)
    raise ProtocolAcceptanceError(f"timed out waiting for {label}: {last}")


def _safe_new_output(path: Path) -> Path:
    output = Path(path).resolve(strict=False)
    if output.exists() or os.path.lexists(output):
        raise ProtocolAcceptanceError(f"refusing to overwrite protocol receipt: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.parent.is_symlink():
        raise ProtocolAcceptanceError("protocol receipt parent cannot be a symlink")
    return output


def _step(
    steps: list[dict[str, Any]],
    operation: str,
    command: str,
    response: dict[str, Any],
) -> dict[str, Any]:
    if response_booted(response):
        raise ProtocolAcceptanceError(f"device rebooted during {operation}")
    if any(row["operation"] == operation for row in steps):
        raise ProtocolAcceptanceError(f"duplicate protocol operation: {operation}")
    steps.append(
        {
            "sequence": len(steps) + 1,
            "operation": operation,
            "command": command,
            "response": response,
        }
    )
    return response


def checked_console_command(
    ser: Any,
    value: str,
    timeout: float,
    *,
    failure_label: str | None = None,
) -> dict[str, Any]:
    result = send_console_command(ser, value, timeout)
    if result.get("ok") is not True:
        if failure_label is not None:
            raise ProtocolAcceptanceError(
                f"serial command failed: {failure_label}"
            )
        raise ProtocolAcceptanceError(
            f"serial command failed: {value}: {result}"
        )
    return result


def execute(
    *,
    root: Path,
    output: Path,
    expected_commit: str,
    github_run_id: str,
    workflow_run_attempt: str,
    peer_status_path: Path,
    peer_socket_path: Path,
    peer_public_key: str,
    admin_fingerprint: str,
    admin_password_path: Path,
    authorize_public_tx: bool,
    baud: int = 115200,
    boot_timeout: float = 45.0,
    command_timeout: float = 8.0,
    rf_timeout: float = 75.0,
    poll_interval: float = 0.5,
    port_lister: Callable[[], Iterable[object]] | None = None,
    serial_module: Any = None,
    status_capture: Callable[..., dict[str, Any]] = capture_peer_status,
    control_sender: Callable[..., dict[str, Any]] = send_peer_control,
) -> dict[str, Any]:
    require_public_tx_authorization(authorize_public_tx)
    root = Path(root).resolve()
    output = _safe_new_output(output)
    commit = exact_commit(expected_commit)
    if commit is None:
        raise ProtocolAcceptanceError("--commit must be an exact 40-hex SHA")
    run_id = positive_decimal(github_run_id, "GitHub run ID")
    run_attempt = positive_decimal(workflow_run_attempt, "workflow run attempt")
    peer_public_key = exact_public_key(peer_public_key, "peer public key")
    peer_fingerprint = peer_public_key[:16].upper()
    admin_fingerprint = exact_fingerprint(admin_fingerprint, "admin fingerprint")
    source = git_metadata(root)
    if not (
        source.get("commit") == commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    ):
        raise ProtocolAcceptanceError(
            "protocol acceptance must run from the exact clean candidate"
        )
    if os.name != "posix":
        raise ProtocolAcceptanceError("protocol acceptance is Pi/POSIX-only")
    if socket.gethostname() != PI_HOST:
        raise ProtocolAcceptanceError(f"protocol acceptance must run on {PI_HOST}")

    if port_lister is None:
        try:
            from serial.tools import list_ports
        except ImportError as exc:
            raise ProtocolAcceptanceError("pyserial is required") from exc

        def default_port_lister() -> Iterable[object]:
            return list_ports.comports(include_links=True)

        port_lister = default_port_lister
    if serial_module is None:
        try:
            import serial as serial_module
        except ImportError as exc:
            raise ProtocolAcceptanceError("pyserial is required") from exc

    d1l_target = resolve_target(
        POSIX_D1L_TARGET,
        port_lister=port_lister,
        platform_name="posix",
    )
    validate_snapshot(d1l_target, POSIX_D1L_TARGET)
    if d1l_target.get("hostname") != PI_HOST:
        raise ProtocolAcceptanceError("D1L target snapshot is not bound to neopi5")

    admin_password = load_admin_password(admin_password_path)
    nonce = secrets.token_hex(6)
    public_out_token = f"rc1-public-out-{commit[:8]}-{nonce}"
    public_in_token = f"rc1-public-in-{commit[:8]}-{nonce}"
    dm_out_token = f"rc1-dm-out-{commit[:8]}-{nonce}"
    dm_in_token = f"rc1-dm-in-{commit[:8]}-{nonce}"
    steps: list[dict[str, Any]] = []

    def peer_status() -> dict[str, Any]:
        return status_capture(
            peer_status_path,
            expected_public_key=peer_public_key,
        )

    with open_d1l_serial(
        serial_module,
        port=POSIX_D1L_TARGET,
        baudrate=baud,
        timeout=command_timeout,
    ) as ser:
        boot_health = checked_console_command(
            ser,
            "health",
            boot_timeout,
            failure_label="cold boot console readiness",
        )
        if not (
            boot_health.get("board_ready") is True
            and boot_health.get("ui_ready") is True
        ):
            raise ProtocolAcceptanceError(
                "cold boot console became responsive before board/UI readiness"
            )

        def command(
            value: str, *, failure_label: str | None = None
        ) -> dict[str, Any]:
            return checked_console_command(
                ser,
                value,
                command_timeout,
                failure_label=failure_label,
            )

        version = _step(steps, "version", "version", command("version"))
        identity = _step(
            steps, "identity", "identity status", command("identity status")
        )
        health_before = _step(
            steps, "health_before", "health", command("health")
        )
        mesh_status = poll(
            lambda: command("mesh status"),
            lambda result: (
                nested(result, "advert_tx", "boot_queued") is not None
                and integer(nested(result, "advert_tx", "boot_queued"), minimum=1)
                is not None
                and integer(nested(result, "advert_tx", "boot_done"), minimum=1)
                is not None
            ),
            timeout=min(15.0, rf_timeout),
            interval=poll_interval,
            label="boot advert completion",
        )
        _step(steps, "mesh_status", "mesh status", mesh_status)

        peer_advert = control_sender(
            peer_socket_path,
            request_id=f"rc1-advert-{nonce}",
            operation="radio.advert",
            params={"flood": False},
        )
        _step(
            steps,
            "peer_advert",
            "controlled-peer radio.advert",
            peer_advert,
        )
        contacts = poll(
            lambda: command("contacts"),
            lambda result: (
                contact_entry(result, public_key=peer_public_key) is not None
                and contact_entry(result, fingerprint=admin_fingerprint) is not None
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="controlled peer and admin contacts",
        )
        _step(steps, "contacts", "contacts", contacts)

        before = _step(
            steps,
            "peer_before",
            "controlled-peer status capture",
            peer_status(),
        )
        _step(
            steps,
            "public_tx_authorization",
            "operator flag --authorize-public-tx",
            {
                "schema": 1,
                "ok": True,
                "authorized": True,
                "source": "cli_flag",
                "bounded_public_tx_count": 1,
            },
        )
        public_send = _step(
            steps,
            "public_send",
            f"mesh send public {public_out_token}",
            command(f"mesh send public {public_out_token}"),
        )
        public_tx_record = poll(
            lambda: command(f"packets search {public_out_token}"),
            lambda result: (
                isinstance(result.get("entries"), list)
                and any(
                    isinstance(row, dict)
                    and row.get("direction") == "tx"
                    and row.get("kind") in {"public_text", "channel_text"}
                    and public_out_token in str(row.get("note") or "")
                    for row in result["entries"]
                )
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="retained Public TX",
        )
        _step(
            steps,
            "public_tx_record",
            f"packets search {public_out_token}",
            public_tx_record,
        )
        before_public = peer_counter(before, "rx_channel_total")
        if before_public is None:
            raise ProtocolAcceptanceError("controlled peer lacks rx_channel_total")
        peer_after_public = poll(
            peer_status,
            lambda result: (
                peer_run_id(result) == peer_run_id(before)
                and peer_counter(result, "rx_channel_total") == before_public + 1
                and str(nested(result, "snapshot", "mesh", "last_rx_sender") or "")
                .lower()
                .startswith(str(identity.get("public_key") or "")[:12].lower())
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="controlled-peer Public receive",
        )
        _step(
            steps,
            "peer_after_public",
            "controlled-peer status capture",
            peer_after_public,
        )
        if public_send.get("text") != public_out_token:
            raise ProtocolAcceptanceError("Public send response did not echo the exact token")

        peer_public_send = control_sender(
            peer_socket_path,
            request_id=f"rc1-public-{nonce}",
            operation="radio.send_channel",
            params={"channel": 0, "text": public_in_token},
        )
        _step(
            steps,
            "peer_public_send",
            "controlled-peer radio.send_channel",
            peer_public_send,
        )
        public_receive = poll(
            lambda: command(f"messages public search {public_in_token}"),
            lambda result: public_entry(
                result, text=public_in_token, direction="rx"
            )
            is not None,
            timeout=rf_timeout,
            interval=poll_interval,
            label="D1L Public receive",
        )
        _step(
            steps,
            "public_receive",
            f"messages public search {public_in_token}",
            public_receive,
        )

        peer_before_dm = _step(
            steps,
            "peer_before_dm",
            "controlled-peer status capture",
            peer_status(),
        )
        dm_send = _step(
            steps,
            "dm_send",
            f"mesh send dm {peer_fingerprint} {dm_out_token}",
            command(f"mesh send dm {peer_fingerprint} {dm_out_token}"),
        )
        dm_ack = poll(
            lambda: command(f"messages dm {peer_fingerprint}"),
            lambda result: (
                (entry := dm_entry(result, text=dm_out_token, direction="tx"))
                is not None
                and entry.get("acked") is True
                and entry.get("delivered") is True
                and integer(entry.get("ack_hash"), minimum=1) is not None
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="outbound DM ACK",
        )
        _step(
            steps,
            "dm_ack",
            f"messages dm {peer_fingerprint}",
            dm_ack,
        )
        before_dm = peer_counter(peer_before_dm, "rx_dm_total")
        if before_dm is None:
            raise ProtocolAcceptanceError("controlled peer lacks rx_dm_total")
        peer_after_dm = poll(
            peer_status,
            lambda result: (
                peer_run_id(result) == peer_run_id(peer_before_dm)
                and peer_counter(result, "rx_dm_total") == before_dm + 1
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="controlled-peer DM receive",
        )
        _step(
            steps,
            "peer_after_dm",
            "controlled-peer status capture",
            peer_after_dm,
        )
        if dm_send.get("fingerprint", "").upper() != peer_fingerprint:
            raise ProtocolAcceptanceError("DM send response target changed")

        peer_dm_send = control_sender(
            peer_socket_path,
            request_id=f"rc1-dm-{nonce}",
            operation="radio.send_dm",
            params={
                "target": exact_public_key(identity.get("public_key"), "D1L identity"),
                "text": dm_in_token,
            },
        )
        _step(
            steps,
            "peer_dm_send",
            "controlled-peer radio.send_dm",
            peer_dm_send,
        )
        dm_receive_ack = poll(
            lambda: command(f"messages dm {peer_fingerprint}"),
            lambda result: (
                (entry := dm_entry(result, text=dm_in_token, direction="rx"))
                is not None
                and nested(entry, "ack_response", "identity_valid") is True
                and nested(entry, "ack_response", "state") == "sent"
                and integer(
                    nested(entry, "ack_response", "dispatch_count"),
                    minimum=1,
                )
                is not None
                and nested(entry, "ack_response", "last_error") == "ESP_OK"
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="inbound DM ACK dispatch",
        )
        _step(
            steps,
            "dm_receive_ack",
            f"messages dm {peer_fingerprint}",
            dm_receive_ack,
        )

        # A contact-derived TRACE is intentionally after the controlled inbound
        # DM. That authenticated packet establishes the exact contact path in
        # this boot; an imported or retained contact alone is not proof of a
        # current routable loop.
        trace_request = _step(
            steps,
            "trace_request",
            f"routes trace contact {peer_fingerprint}",
            command(f"routes trace contact {peer_fingerprint}"),
        )
        trace_tag = integer(trace_request.get("tag"), minimum=1)
        trace_result = poll(
            lambda: command("routes trace status"),
            lambda result: (
                result.get("matched") is True
                and result.get("zero_hop") is False
                and str(result.get("fingerprint") or "").upper()
                == peer_fingerprint
                and nested(result, "last_result", "tag") == trace_tag
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="matched contact TRACE",
        )
        _step(steps, "trace_result", "routes trace status", trace_result)

        path_request = _step(
            steps,
            "path_request",
            f"routes probe {peer_fingerprint}",
            command(f"routes probe {peer_fingerprint}"),
        )
        path_token = path_request.get("token")
        if (
            not isinstance(path_token, str)
            or re.fullmatch(r"path_[0-9A-F]{8}", path_token) is None
        ):
            raise ProtocolAcceptanceError(
                "PATH request did not return its exact correlation token"
            )
        path_tag = int(path_token[5:], 16)
        path_result = poll(
            lambda: command(f"routes telemetry {peer_fingerprint}"),
            lambda result: (
                result.get("state") == "received"
                and result.get("pending") is False
                and result.get("pending_tag") == 0
                and integer(result.get("history_count"), minimum=1) is not None
                and isinstance(result.get("entries"), list)
                and any(
                    isinstance(row, dict)
                    and row.get("tag") == path_tag
                    and integer(row.get("sequence"), minimum=1) is not None
                    for row in result["entries"]
                )
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="PATH/base telemetry response",
        )
        _step(
            steps,
            "path_result",
            f"routes telemetry {peer_fingerprint}",
            path_result,
        )

        password_argument = admin_password or "<empty>"
        login_wire_command = (
            f"admin login {admin_fingerprint} {password_argument}"
        )
        redacted_login_command = (
            f"admin login {admin_fingerprint} <redacted>"
        )
        login_request = command(
            login_wire_command,
            failure_label=redacted_login_command,
        )
        _step(
            steps,
            "admin_login_request",
            redacted_login_command,
            login_request,
        )
        admin_login_status = poll(
            lambda: command("admin status"),
            lambda result: (
                result.get("state") == "authenticated"
                and str(result.get("fingerprint") or "").upper()
                == admin_fingerprint
                and result.get("credential_exposed") is False
                and result.get("session_secret_exposed") is False
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="authenticated repeater session",
        )
        _step(
            steps,
            "admin_login_status",
            "admin status",
            admin_login_status,
        )
        query_request = _step(
            steps,
            "admin_query_request",
            "admin telemetry",
            command("admin telemetry"),
        )
        admin_query_status = poll(
            lambda: command("admin status"),
            lambda result: (
                result.get("state") == "authenticated"
                and str(result.get("fingerprint") or "").upper()
                == admin_fingerprint
                and nested(result, "query_result", "valid") is True
                and nested(result, "query_result", "kind") == "telemetry"
                and result.get("credential_exposed") is False
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="authenticated repeater telemetry",
        )
        _step(
            steps,
            "admin_query_status",
            "admin status",
            admin_query_status,
        )
        if query_request.get("cmd") != "admin telemetry":
            raise ProtocolAcceptanceError("admin query response command changed")
        admin_logout = _step(
            steps,
            "admin_logout",
            "admin logout",
            command("admin logout"),
        )
        if admin_logout.get("state") != "idle":
            raise ProtocolAcceptanceError("admin authority did not clear")

        cooldown_ms = integer(
            nested(trace_result, "cooldown", "remaining_ms")
        )
        if cooldown_ms is not None and cooldown_ms > 0:
            time.sleep(min((cooldown_ms / 1000.0) + 0.25, 31.0))
        ping_request = _step(
            steps,
            "ping_request",
            f"repeater ping {admin_fingerprint}",
            command(f"repeater ping {admin_fingerprint}"),
        )
        ping_tag = integer(ping_request.get("tag"), minimum=1)
        ping_result = poll(
            lambda: command("repeater ping status"),
            lambda result: (
                result.get("matched") is True
                and result.get("zero_hop") is True
                and str(result.get("fingerprint") or "").upper()
                == admin_fingerprint
                and nested(result, "last_result", "tag") == ping_tag
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="matched zero-hop repeater Ping",
        )
        _step(
            steps,
            "ping_result",
            "repeater ping status",
            ping_result,
        )

        health_after = _step(
            steps, "health_after", "health", command("health")
        )
        crashlog = _step(
            steps, "crashlog", "crashlog", command("crashlog")
        )

    d1l_target_after = resolve_target(
        POSIX_D1L_TARGET,
        port_lister=port_lister,
        platform_name="posix",
    )
    validate_snapshot(d1l_target_after, POSIX_D1L_TARGET)
    if (
        d1l_target["stable_identity_sha256"]
        != d1l_target_after["stable_identity_sha256"]
    ):
        raise ProtocolAcceptanceError("D1L serial identity changed during acceptance")
    if health_before.get("boot_nonce") != health_after.get("boot_nonce"):
        raise ProtocolAcceptanceError("D1L rebooted during protocol acceptance")

    transcript = {
        "schema": SCHEMA,
        "kind": PROTOCOL_KIND,
        "mode": "hardware",
        "physical_observed": True,
        "simulated": False,
        "dry_run": False,
        "manual_only": False,
        "port": POSIX_D1L_TARGET,
        "d1l_target": d1l_target,
        "d1l_target_after": d1l_target_after,
        "runner_commit": source["commit"],
        "runner_source_clean": True,
        "expected_firmware_commit": commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "steps": steps,
    }
    candidate = {
        "firmware_commit": commit,
        "actions_run": run_id,
        "actions_run_attempt": run_attempt,
    }
    validate_protocol(transcript, candidate)
    output.write_bytes(canonical_json(transcript))
    return transcript


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--output", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-attempt", required=True)
    parser.add_argument("--peer-status", default=str(DEFAULT_PEER_STATUS))
    parser.add_argument(
        "--peer-control-socket", default=str(DEFAULT_PEER_SOCKET)
    )
    parser.add_argument(
        "--peer-public-key", default=DEFAULT_PEER_PUBLIC_KEY
    )
    parser.add_argument("--admin-fingerprint", required=True)
    parser.add_argument("--admin-password-file", required=True)
    parser.add_argument(
        "--authorize-public-tx",
        action="store_true",
        help="explicitly authorize the one tokenized RC1 Public acceptance send",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--boot-timeout", type=float, default=45.0)
    parser.add_argument("--command-timeout", type=float, default=8.0)
    parser.add_argument("--rf-timeout", type=float, default=75.0)
    parser.add_argument("--poll-interval", type=float, default=0.5)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        transcript = execute(
            root=Path(args.root),
            output=Path(args.output),
            expected_commit=args.commit,
            github_run_id=args.github_run_id,
            workflow_run_attempt=args.github_run_attempt,
            peer_status_path=Path(args.peer_status),
            peer_socket_path=Path(args.peer_control_socket),
            peer_public_key=args.peer_public_key,
            admin_fingerprint=args.admin_fingerprint,
            admin_password_path=Path(args.admin_password_file),
            authorize_public_tx=args.authorize_public_tx,
            baud=args.baud,
            boot_timeout=args.boot_timeout,
            command_timeout=args.command_timeout,
            rf_timeout=args.rf_timeout,
            poll_interval=args.poll_interval,
        )
    except (OSError, ProtocolAcceptanceError, TypeError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(
        json.dumps(
            {
                "ok": True,
                "out": str(Path(args.output).resolve()),
                "firmware_commit": transcript["expected_firmware_commit"],
                "github_actions_run": transcript["github_actions_run"],
                "workflow_run_attempt": transcript["workflow_run_attempt"],
                "port": transcript["port"],
                "manual_only": transcript["manual_only"],
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
