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
MESHCOREBOT_PEER_STATUS = Path(
    "/opt/canadaverse/com11-meshcorebot/data/logs/meshcorebot.status.json"
)
MESHCOREBOT_PEER_SOCKET = Path(
    "/run/canadaverse-control/com11/control.sock"
)
MESHCOREBOT_PEER_DEVICE = "/dev/krab-com11"
MESHCOREBOT_PEER_SERVICE = "meshcorebot"
MESHCOREBOT_PEER_PUBLIC_KEY = (
    "0bf0a701d5ae2db679c641ee999a70d4b55b61a2b77c47337ce35c16c9c19193"
)
RADIO_LISTENER_PEER_STATUS = Path(
    "/opt/canadaverse/com15-responder/data/radio_listener.status.json"
)
RADIO_LISTENER_PEER_SOCKET = Path(
    "/run/canadaverse-control/com15/control.sock"
)
RADIO_LISTENER_PEER_DEVICE = "/dev/krab-t-echo"
RADIO_LISTENER_PEER_SERVICE = "openclaw-radio-listener"
RADIO_LISTENER_PEER_PUBLIC_KEY = (
    "024999dedfd26763c5606169c3ebd34e05a9475cf78220a81078b5dd27caca44"
)
RADIO_LISTENER_STATUS_SCHEMA = "openclaw_radio_listener_v1"
MESHCOREBOT_STATUS_SCHEMA = "meshcorebot_v1"
PEER_STATUS_SCHEMAS = frozenset(
    {RADIO_LISTENER_STATUS_SCHEMA, MESHCOREBOT_STATUS_SCHEMA}
)
PEER_PROFILE_BINDINGS = {
    RADIO_LISTENER_STATUS_SCHEMA: {
        "status_path": str(RADIO_LISTENER_PEER_STATUS),
        "control_socket": str(RADIO_LISTENER_PEER_SOCKET),
        "device": RADIO_LISTENER_PEER_DEVICE,
        "service": RADIO_LISTENER_PEER_SERVICE,
        "public_key": RADIO_LISTENER_PEER_PUBLIC_KEY,
    },
    MESHCOREBOT_STATUS_SCHEMA: {
        "status_path": str(MESHCOREBOT_PEER_STATUS),
        "control_socket": str(MESHCOREBOT_PEER_SOCKET),
        "device": MESHCOREBOT_PEER_DEVICE,
        "service": MESHCOREBOT_PEER_SERVICE,
        "public_key": MESHCOREBOT_PEER_PUBLIC_KEY,
    },
}
DEFAULT_PEER_STATUS_SCHEMA = MESHCOREBOT_STATUS_SCHEMA
DEFAULT_PEER_STATUS = MESHCOREBOT_PEER_STATUS
DEFAULT_PEER_SOCKET = MESHCOREBOT_PEER_SOCKET
DEFAULT_PEER_DEVICE = MESHCOREBOT_PEER_DEVICE
DEFAULT_PEER_SERVICE = MESHCOREBOT_PEER_SERVICE
MESHCOREBOT_HARDWARE_ID = "303a:1001"
MESHCOREBOT_BAUD = 115200
DEFAULT_PEER_PUBLIC_KEY = MESHCOREBOT_PEER_PUBLIC_KEY
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


def exact_peer_binding(
    *,
    status_schema: str,
    status_path: Path,
    control_socket: Path,
    device: str,
    service: str,
    public_key: str,
) -> dict[str, str]:
    binding = {
        "status_path": str(status_path),
        "control_socket": str(control_socket),
        "device": device,
        "service": service,
        "public_key": public_key,
    }
    if PEER_PROFILE_BINDINGS.get(status_schema) != binding:
        raise ProtocolAcceptanceError(
            "controlled-peer profile does not match an exact authorized binding"
        )
    return {"status_schema": status_schema, **binding}


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
    expected_device: str = DEFAULT_PEER_DEVICE,
    expected_service: str = DEFAULT_PEER_SERVICE,
    status_schema: str = DEFAULT_PEER_STATUS_SCHEMA,
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
    status = data if isinstance(data, dict) else {}
    written = json_time(status.get("status_written_at"))
    age = (
        (datetime.now(timezone.utc) - written.astimezone(timezone.utc)).total_seconds()
        if written is not None
        else -1.0
    )
    common_valid = bool(
        isinstance(data, dict)
        and status.get("service") == expected_service
        and isinstance(status.get("counters"), dict)
        and isinstance(status.get("mesh"), dict)
        and 0.0 <= age <= max_age_seconds
    )
    if status_schema == RADIO_LISTENER_STATUS_SCHEMA:
        identity_valid = bool(
            str(path) == str(RADIO_LISTENER_PEER_STATUS)
            and expected_service == RADIO_LISTENER_PEER_SERVICE
            and expected_device == RADIO_LISTENER_PEER_DEVICE
            and expected_public_key == RADIO_LISTENER_PEER_PUBLIC_KEY
            and isinstance(status.get("run_id"), str)
            and bool(status["run_id"])
            and nested(status, "serial", "mesh_connected") is True
            and nested(status, "serial", "port") == expected_device
            and str(nested(status, "serial", "public_key") or "").strip().lower()
            == expected_public_key
        )
    elif status_schema == MESHCOREBOT_STATUS_SCHEMA:
        started = json_time(status.get("started_at"))
        poll_at = json_time(nested(status, "mesh", "last_poll_at"))
        poll_age = (
            (
                datetime.now(timezone.utc)
                - poll_at.astimezone(timezone.utc)
            ).total_seconds()
            if poll_at is not None
            else -1.0
        )
        pid = status.get("pid")
        identity_valid = bool(
            str(path) == str(MESHCOREBOT_PEER_STATUS)
            and expected_service == MESHCOREBOT_PEER_SERVICE
            and expected_device == MESHCOREBOT_PEER_DEVICE
            and expected_public_key == MESHCOREBOT_PEER_PUBLIC_KEY
            and isinstance(pid, int)
            and not isinstance(pid, bool)
            and pid > 0
            and started is not None
            and nested(status, "serial", "active_port") == expected_device
            and nested(status, "serial", "configured_port") == expected_device
            and nested(status, "serial", "hardware_id")
            == MESHCOREBOT_HARDWARE_ID
            and nested(status, "serial", "baud_rate") == MESHCOREBOT_BAUD
            and nested(status, "serial", "meshcore_connected") is True
            and nested(status, "discord", "connected") is True
            and str(nested(status, "mqtt", "device_public_key") or "")
            .strip()
            .lower()
            == expected_public_key
            and 0.0 <= poll_age <= max_age_seconds
        )
    else:
        raise ProtocolAcceptanceError(
            f"unsupported controlled-peer status schema: {status_schema}"
        )
    if not (common_valid and identity_valid):
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


def peer_control_timeout(status_schema: str) -> float:
    if status_schema == MESHCOREBOT_STATUS_SCHEMA:
        # Meshcorebot's bounded radio operation may spend up to 55 seconds
        # before returning its exact response.
        return 60.0
    if status_schema == RADIO_LISTENER_STATUS_SCHEMA:
        return 35.0
    raise ProtocolAcceptanceError(
        f"unsupported controlled-peer status schema: {status_schema}"
    )


def peer_counter(
    status: dict[str, Any],
    name: str,
    status_schema: str = RADIO_LISTENER_STATUS_SCHEMA,
) -> int | None:
    counter_name = name
    if status_schema == MESHCOREBOT_STATUS_SCHEMA and name == "rx_dm_total":
        counter_name = "rx_contact_total"
    elif status_schema not in PEER_STATUS_SCHEMAS:
        return None
    return integer(nested(status, "snapshot", "counters", counter_name))


def peer_session_identity(
    status: dict[str, Any], status_schema: str
) -> tuple[object, ...] | None:
    if status_schema == RADIO_LISTENER_STATUS_SCHEMA:
        value = nested(status, "snapshot", "run_id")
        return ("run_id", value) if isinstance(value, str) and value else None
    if status_schema == MESHCOREBOT_STATUS_SCHEMA:
        pid = nested(status, "snapshot", "pid")
        started_at = nested(status, "snapshot", "started_at")
        if (
            isinstance(pid, int)
            and not isinstance(pid, bool)
            and pid > 0
            and json_time(started_at) is not None
        ):
            return ("pid_started_at", pid, started_at)
    return None


def peer_public_sender_matches(
    status: dict[str, Any],
    *,
    status_schema: str,
    d1l_public_key: str,
    d1l_node_name: str,
    resolved_advert_timestamp: int | None = None,
) -> bool:
    snapshot = nested(status, "snapshot")
    sender = str(nested(snapshot, "mesh", "last_rx_sender") or "").lower()
    if sender != d1l_public_key[:12].lower():
        return False
    if status_schema == RADIO_LISTENER_STATUS_SCHEMA:
        return True
    return bool(
        status_schema == MESHCOREBOT_STATUS_SCHEMA
        and nested(snapshot, "mesh", "last_rx_sender_source")
        == "unique_signed_advert_name"
        and nested(snapshot, "mesh", "last_rx_sender_name") == d1l_node_name
        and integer(
            nested(snapshot, "mesh", "last_rx_sender_advert_timestamp"),
            minimum=1,
        )
        == resolved_advert_timestamp
    )


def resolved_peer_contact(
    exchange: object,
    *,
    d1l_public_key: str,
    d1l_node_name: str,
) -> bool:
    response = exchange.get("response") if isinstance(exchange, dict) else None
    result = response.get("result") if isinstance(response, dict) else None
    return bool(
        isinstance(result, dict)
        and set(result)
        == {
            "name",
            "match_count",
            "unique",
            "valid_signed_advert",
            "public_key_prefix",
            "last_advert",
        }
        and result.get("name") == d1l_node_name
        and result.get("match_count") == 1
        and result.get("unique") is True
        and result.get("valid_signed_advert") is True
        and str(result.get("public_key_prefix") or "").lower()
        == d1l_public_key[:12].lower()
        and integer(result.get("last_advert"), minimum=1) is not None
    )


def exact_chat_contact(result: object, public_key: str) -> bool:
    contact = contact_entry(result, public_key=public_key)
    return bool(
        contact is not None
        and str(contact.get("fingerprint") or "").upper()
        == public_key[:16].upper()
        and contact.get("type") == "chat"
        and contact.get("canonical") is True
        and contact.get("can_dm") is True
        and contact.get("can_admin") is False
        and contact.get("verification_source") == "signed_advert"
    )


def exact_admin_contact(result: object, fingerprint: str) -> bool:
    contact = contact_entry(result, fingerprint=fingerprint)
    candidate = (
        str(contact.get("public_key") or "").strip().lower()
        if contact is not None
        else ""
    )
    public_key = candidate if PUBLIC_KEY_RE.fullmatch(candidate) else None
    return bool(
        contact is not None
        and public_key is not None
        and public_key[:16].upper() == fingerprint
        and contact.get("type") == "repeater"
        and contact.get("canonical") is True
        and contact.get("can_dm") is False
        and contact.get("can_admin") is True
        and contact.get("verification_source") == "signed_advert"
    )


def exact_trace_contact(result: object, fingerprint: str) -> bool:
    contact = contact_entry(result, fingerprint=fingerprint)
    candidate = (
        str(contact.get("public_key") or "").strip().lower()
        if contact is not None
        else ""
    )
    public_key = candidate if PUBLIC_KEY_RE.fullmatch(candidate) else None
    return bool(
        contact is not None
        and public_key is not None
        and public_key[:16].upper() == fingerprint
        and contact.get("type") in {"repeater", "room"}
        and contact.get("canonical") is True
        and contact.get("can_dm") is False
        and contact.get("can_admin") is True
        and contact.get("verification_source") == "signed_advert"
    )


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


def authenticate_admin_session(
    command: Callable[..., dict[str, Any]],
    *,
    login_wire_command: str,
    redacted_login_command: str,
    admin_fingerprint: str,
    timeout: float,
    interval: float,
    poll_function: Callable[..., dict[str, Any]] = poll,
) -> tuple[dict[str, Any], dict[str, Any]]:
    def authenticated(result: dict[str, Any]) -> bool:
        return bool(
            result.get("state") == "authenticated"
            and str(result.get("fingerprint") or "").upper()
            == admin_fingerprint
            and result.get("credential_exposed") is False
            and result.get("session_secret_exposed") is False
        )

    for attempt in range(2):
        login_request = command(
            login_wire_command,
            failure_label=redacted_login_command,
        )
        try:
            login_status = poll_function(
                lambda: command("admin status"),
                authenticated,
                timeout=timeout,
                interval=interval,
                label="authenticated repeater session",
            )
            return login_request, login_status
        except ProtocolAcceptanceError as exc:
            if not str(exc).startswith(
                "timed out waiting for authenticated repeater session:"
            ):
                raise
            terminal_status = command("admin status")
            if response_booted(terminal_status):
                raise ProtocolAcceptanceError(
                    "device rebooted while confirming admin timeout"
                ) from exc
            if authenticated(terminal_status):
                return login_request, terminal_status
            retryable_timeout = bool(
                terminal_status.get("ok") is True
                and terminal_status.get("cmd") == "admin status"
                and terminal_status.get("state") == "timed_out"
                and str(terminal_status.get("fingerprint") or "").upper()
                == admin_fingerprint
                and terminal_status.get("last_error") == "ESP_ERR_TIMEOUT"
                and terminal_status.get("credential_exposed") is False
                and terminal_status.get("session_secret_exposed") is False
            )
            if attempt != 0 or not retryable_timeout:
                raise
            print(
                "bounded admin login retry after exact timed_out state",
                file=sys.stderr,
                flush=True,
            )
            time.sleep(interval)

    raise AssertionError("bounded admin login attempts exhausted")


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


def wait_for_console_ready(
    ser: Any,
    timeout: float,
    command_timeout: float,
    *,
    poll_interval: float = 0.1,
) -> dict[str, Any]:
    """Retry health because a command sent before console init is discarded."""
    failure_label = "cold boot console readiness"
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ProtocolAcceptanceError(
                f"serial command failed: {failure_label}"
            )
        result = send_console_command(
            ser,
            "health",
            min(command_timeout, remaining),
        )
        if (
            result.get("ok") is True
            and result.get("board_ready") is True
            and result.get("ui_ready") is True
        ):
            return result
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ProtocolAcceptanceError(
                f"serial command failed: {failure_label}"
            )
        time.sleep(min(poll_interval, remaining))


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
    peer_device: str,
    peer_service: str,
    peer_status_schema: str,
    admin_fingerprint: str,
    trace_fingerprint: str,
    admin_password_path: Path,
    authorize_public_tx: bool,
    baud: int = 115200,
    boot_timeout: float = 75.0,
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
    controlled_peer = exact_peer_binding(
        status_schema=peer_status_schema,
        status_path=peer_status_path,
        control_socket=peer_socket_path,
        device=peer_device,
        service=peer_service,
        public_key=peer_public_key,
    )
    admin_fingerprint = exact_fingerprint(admin_fingerprint, "admin fingerprint")
    trace_fingerprint = exact_fingerprint(trace_fingerprint, "trace fingerprint")
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
    steps: list[dict[str, Any]] = []

    def peer_status() -> dict[str, Any]:
        return status_capture(
            peer_status_path,
            expected_public_key=peer_public_key,
            expected_device=peer_device,
            expected_service=peer_service,
            status_schema=peer_status_schema,
        )

    with open_d1l_serial(
        serial_module,
        port=POSIX_D1L_TARGET,
        baudrate=baud,
        timeout=command_timeout,
    ) as ser:
        wait_for_console_ready(
            ser,
            boot_timeout,
            command_timeout,
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
        if version.get("cmd") != "version":
            raise ProtocolAcceptanceError("version response command changed")
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
            timeout=peer_control_timeout(peer_status_schema),
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
                exact_chat_contact(result, peer_public_key)
                and exact_admin_contact(result, admin_fingerprint)
                and exact_trace_contact(result, trace_fingerprint)
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="controlled peer, admin, and TRACE contacts",
        )
        _step(steps, "contacts", "contacts", contacts)

        # Prove the exact TRACE target has a current-boot PATH before any
        # Public transmission. A signed advert identifies the contact but does
        # not establish the immutable direct route required by real TRACE.
        trace_path_request = _step(
            steps,
            "trace_path_request",
            f"routes probe {trace_fingerprint}",
            command(f"routes probe {trace_fingerprint}"),
        )
        trace_path_token = trace_path_request.get("token")
        if (
            not isinstance(trace_path_token, str)
            or re.fullmatch(r"path_[0-9A-F]{8}", trace_path_token) is None
        ):
            raise ProtocolAcceptanceError(
                "TRACE PATH preflight did not return its exact correlation "
                "token"
            )
        trace_path_tag = int(trace_path_token[5:], 16)
        trace_path_result = poll(
            lambda: command(f"routes telemetry {trace_fingerprint}"),
            lambda result: (
                result.get("state") == "received"
                and result.get("pending") is False
                and result.get("pending_tag") == 0
                and integer(result.get("history_count"), minimum=1) is not None
                and isinstance(result.get("entries"), list)
                and any(
                    isinstance(row, dict)
                    and row.get("tag") == trace_path_tag
                    and integer(row.get("sequence"), minimum=1) is not None
                    for row in result["entries"]
                )
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="current-boot TRACE target PATH response",
        )
        _step(
            steps,
            "trace_path_result",
            f"routes telemetry {trace_fingerprint}",
            trace_path_result,
        )

        # Run and correlate the real TRACE before authorizing the sole Public
        # token. A route-width or current-boot-path failure therefore cannot
        # consume the candidate's one allowed Public transmission.
        trace_request = _step(
            steps,
            "trace_request",
            f"routes trace contact {trace_fingerprint}",
            command(f"routes trace contact {trace_fingerprint}"),
        )
        trace_tag = integer(trace_request.get("tag"), minimum=1)
        if trace_tag is None:
            raise ProtocolAcceptanceError(
                "TRACE request did not return its exact correlation tag"
            )
        trace_result = poll(
            lambda: command("routes trace status"),
            lambda result: (
                result.get("matched") is True
                and result.get("zero_hop") is False
                and str(result.get("fingerprint") or "").upper()
                == trace_fingerprint
                and nested(result, "last_result", "tag") == trace_tag
            ),
            timeout=rf_timeout,
            interval=poll_interval,
            label="matched repeater TRACE",
        )
        _step(steps, "trace_result", "routes trace status", trace_result)

        cooldown_ms = integer(
            nested(trace_result, "cooldown", "remaining_ms")
        )
        if cooldown_ms is not None and cooldown_ms > 0:
            time.sleep(min((cooldown_ms / 1000.0) + 0.25, 31.0))

        # Finish every fallible targeted/admin RF gate before capturing the
        # controlled-peer baseline and authorizing the sole Public transmit.
        password_argument = admin_password or "<empty>"
        login_wire_command = (
            f"admin login {admin_fingerprint} {password_argument}"
        )
        redacted_login_command = (
            f"admin login {admin_fingerprint} <redacted>"
        )
        login_request, admin_login_status = authenticate_admin_session(
            command,
            login_wire_command=login_wire_command,
            redacted_login_command=redacted_login_command,
            admin_fingerprint=admin_fingerprint,
            timeout=rf_timeout,
            interval=poll_interval,
        )
        _step(
            steps,
            "admin_login_request",
            redacted_login_command,
            login_request,
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

        path_request = _step(
            steps,
            "path_request",
            f"routes probe {admin_fingerprint}",
            command(f"routes probe {admin_fingerprint}"),
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
            lambda: command(f"routes telemetry {admin_fingerprint}"),
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
            f"routes telemetry {admin_fingerprint}",
            path_result,
        )

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

        before = _step(
            steps,
            "peer_before",
            "controlled-peer status capture",
            peer_status(),
        )
        resolved_advert_timestamp: int | None = None
        if peer_status_schema == MESHCOREBOT_STATUS_SCHEMA:
            d1l_advert = _step(
                steps,
                "d1l_advert",
                "mesh advert flood",
                command("mesh advert flood"),
            )
            if not (
                d1l_advert.get("cmd") == "mesh advert flood"
                and d1l_advert.get("queued") is True
                and d1l_advert.get("flood") is True
            ):
                raise ProtocolAcceptanceError(
                    "D1L signed flood advert was not queued exactly once"
                )

            resolve_attempt = 0

            def resolve_d1l_contact() -> dict[str, Any]:
                nonlocal resolve_attempt
                resolve_attempt += 1
                if resolve_attempt > 999:
                    raise ProtocolAcceptanceError(
                        "controlled-peer contact resolution exceeded its bound"
                    )
                try:
                    return control_sender(
                        peer_socket_path,
                        request_id=(
                            f"rc1-resolve-{nonce}-{resolve_attempt:03d}"
                        ),
                        operation="radio.resolve_contact",
                        params={"name": EXPECTED_NODE_NAME},
                        timeout=peer_control_timeout(peer_status_schema),
                    )
                except ProtocolAcceptanceError:
                    return {}

            peer_resolution = poll(
                resolve_d1l_contact,
                lambda result: resolved_peer_contact(
                    result,
                    d1l_public_key=str(identity.get("public_key") or ""),
                    d1l_node_name=EXPECTED_NODE_NAME,
                ),
                timeout=rf_timeout,
                interval=poll_interval,
                label="controlled-peer unique signed D1L advert",
            )
            _step(
                steps,
                "peer_resolve_d1l",
                "controlled-peer radio.resolve_contact D1L",
                peer_resolution,
            )
            resolved_advert_timestamp = integer(
                nested(peer_resolution, "response", "result", "last_advert"),
                minimum=1,
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
        before_public = peer_counter(
            before, "rx_channel_total", peer_status_schema
        )
        if before_public is None:
            raise ProtocolAcceptanceError("controlled peer lacks rx_channel_total")
        peer_after_public = poll(
            peer_status,
            lambda result: (
                peer_session_identity(result, peer_status_schema)
                == peer_session_identity(before, peer_status_schema)
                and peer_counter(
                    result, "rx_channel_total", peer_status_schema
                )
                == before_public + 1
                and peer_public_sender_matches(
                    result,
                    status_schema=peer_status_schema,
                    d1l_public_key=str(identity.get("public_key") or ""),
                    d1l_node_name=str(identity.get("node_name") or ""),
                    resolved_advert_timestamp=resolved_advert_timestamp,
                )
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
            timeout=peer_control_timeout(peer_status_schema),
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

        health_after = _step(
            steps, "health_after", "health", command("health")
        )
        _step(steps, "crashlog", "crashlog", command("crashlog"))

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
        "controlled_peer": controlled_peer,
        "protocol_targets": {
            "admin_fingerprint": admin_fingerprint,
            "trace_fingerprint": trace_fingerprint,
        },
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
    parser.add_argument("--peer-device", default=DEFAULT_PEER_DEVICE)
    parser.add_argument("--peer-service", default=DEFAULT_PEER_SERVICE)
    parser.add_argument(
        "--peer-status-schema",
        choices=sorted(PEER_STATUS_SCHEMAS),
        default=DEFAULT_PEER_STATUS_SCHEMA,
    )
    parser.add_argument("--admin-fingerprint", required=True)
    parser.add_argument("--trace-fingerprint", required=True)
    parser.add_argument("--admin-password-file", required=True)
    parser.add_argument(
        "--authorize-public-tx",
        action="store_true",
        help="explicitly authorize the one tokenized RC1 Public acceptance send",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--boot-timeout", type=float, default=75.0)
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
            peer_device=args.peer_device,
            peer_service=args.peer_service,
            peer_status_schema=args.peer_status_schema,
            admin_fingerprint=args.admin_fingerprint,
            trace_fingerprint=args.trace_fingerprint,
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
