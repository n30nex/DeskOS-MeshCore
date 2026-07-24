#!/usr/bin/env python3
"""Full D1L-port-only RF acceptance runner for MeshCore DeskOS D1L."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import socket
import stat
import struct
import subprocess
import time
from collections.abc import Callable, Iterable
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any

try:
    from artifact_metadata import git_metadata, stamp_report
    from core_smoke_d1l import enforce_core_port, resolve_core_target
    from d1l_serial_target import (
        POSIX_D1L_TARGET,
        WINDOWS_D1L_TARGET,
        safe_slug,
        validate_snapshot,
    )
    from smoke_d1l import open_d1l_serial, send_console_command
    from verify_checksums import is_link_or_reparse, sha256_file
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import git_metadata, stamp_report
    from scripts.core_smoke_d1l import enforce_core_port, resolve_core_target
    from scripts.d1l_serial_target import (
        POSIX_D1L_TARGET,
        WINDOWS_D1L_TARGET,
        safe_slug,
        validate_snapshot,
    )
    from scripts.smoke_d1l import open_d1l_serial, send_console_command
    from scripts.verify_checksums import is_link_or_reparse, sha256_file


DEFAULT_TARGET_FINGERPRINT = "0BF0A701D5AE2DB6"
DEFAULT_D1L_PUBLIC_KEY = "ba14729e8588e30b44b36ff9c6c5511b9d88bf787196c6a46de102af6ebafa07"
RF_FULL_ACCEPTANCE_SCHEMA = 2
FORBIDDEN_PORTS = {"COM" + str(number) for number in (8, 11, 29)}
D1L_REQUIRED_PORT = WINDOWS_D1L_TARGET
D1L_REQUIRED_POSIX_TARGET = POSIX_D1L_TARGET
RF_PEER_FORBIDDEN_PORTS = FORBIDDEN_PORTS | {D1L_REQUIRED_PORT, "COM16"}
RADIO_LISTENER_PORT = "COM15"
RADIO_LISTENER_REPLY = "Test OK DM."
RADIO_LISTENER_PROFILE = "openclaw_radio_listener"
RADIO_LISTENER_CONTACT_NAME = "CoreTestPeer"
RADIO_LISTENER_STATUS_PATH = Path(
    r"F:\openclaw\runtime\workspace\radio_listener.status.json"
)
REMOTE_PEER_EVIDENCE_SOURCE = "remote_peer_status_ssh"
REMOTE_PEER_ADAPTER = "pi5_unix_control_socket"
LOCAL_PEER_EVIDENCE_SOURCE = "local_peer_status_file"
LOCAL_PEER_ADAPTER = "pi5_local_unix_control_socket"
LOCAL_PEER_STATUS_TRANSPORT = "local-file"
LOCAL_PEER_CONTROL_REQUEST_TRANSPORT = "local-unix-socket-request"
LOCAL_PEER_CONTROL_RESPONSE_TRANSPORT = "local-unix-socket-response"
LOCAL_PEER_MAX_MTIME_STATUS_SKEW_SEC = 30.0
LOCAL_PEER_CONTROL_UID = 0
LOCAL_PEER_CONTROL_GID = 0
LOCAL_PEER_SO_PEERCRED = getattr(socket, "SO_PEERCRED", 17)
REMOTE_PEER_SSH_HOST = "neonx@192.168.0.24"
REMOTE_PEER_HOSTNAME = "neopi5"
REMOTE_PEER_STATUS_PATH = (
    "/opt/canadaverse/com15-responder/data/radio_listener.status.json"
)
REMOTE_PEER_CONTROL_SOCKET = (
    "/run/canadaverse-control/com15/control.sock"
)
REMOTE_PEER_DEVICE = "/dev/krab-t-echo"
REMOTE_PEER_PUBLIC_KEY = (
    "024999dedfd26763c5606169c3ebd34e05a9475cf78220a81078b5dd27caca44"
)
REMOTE_PEER_FINGERPRINT = REMOTE_PEER_PUBLIC_KEY[:16].upper()
REMOTE_PEER_MAX_STATUS_AGE_SEC = 120.0
REMOTE_PEER_MAX_STATUS_BYTES = 1024 * 1024
REMOTE_PEER_MAX_CONTROL_BYTES = 64 * 1024
REMOTE_PEER_SSH_TIMEOUT_SEC = 45.0
REMOTE_PEER_SSH_IDENTITY_ENV = "MESH_PEER_SSH_IDENTITY"
REMOTE_PEER_HELPER_SCHEMA = 1
REMOTE_PEER_FORBIDDEN_DEVICE = "/dev/krab-" + "com" + str(11)


class RemotePeerError(RuntimeError):
    """A bounded, operator-facing remote controlled-peer failure."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


REMOTE_PEER_HELPER = r"""
import base64
import hashlib
import json
import os
import socket
import stat
import sys

SCHEMA = 1
MAX_INPUT = 32768
MAX_STATUS = 1048576
MAX_CONTROL = 65536

def emit(value):
    sys.stdout.write(json.dumps(value, separators=(",", ":"), sort_keys=True) + "\n")
    sys.stdout.flush()

def fail(code, message):
    emit({
        "schema": SCHEMA,
        "ok": False,
        "operation": None,
        "result": None,
        "error": {"code": code, "message": str(message)[:240]},
    })

def absolute_path(value, name):
    if (
        not isinstance(value, str)
        or not value.startswith("/")
        or len(value) > 512
        or any(ord(char) < 32 for char in value)
    ):
        raise ValueError(name + " must be a bounded absolute POSIX path")
    parts = value.split("/")
    if any(part in (".", "..") for part in parts):
        raise ValueError(name + " cannot contain dot segments")
    return value

def read_regular_file(path):
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode):
            raise ValueError("status path is not a regular file")
        if before.st_size < 2 or before.st_size > MAX_STATUS:
            raise ValueError("status file size is outside the bounded range")
        chunks = []
        remaining = MAX_STATUS + 1
        while remaining > 0:
            chunk = os.read(fd, min(65536, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks)
        after = os.fstat(fd)
        if len(raw) > MAX_STATUS:
            raise ValueError("status file exceeds the bounded range")
        if (
            before.st_dev != after.st_dev
            or before.st_ino != after.st_ino
            or before.st_size != after.st_size
            or before.st_mtime_ns != after.st_mtime_ns
            or len(raw) != before.st_size
        ):
            raise ValueError("status file changed during capture")
        return raw, after.st_mtime_ns
    finally:
        os.close(fd)

try:
    request_raw = sys.stdin.buffer.read(MAX_INPUT + 1)
    if len(request_raw) > MAX_INPUT:
        raise ValueError("request exceeds the bounded input size")
    request = json.loads(request_raw.decode("utf-8"))
    if not isinstance(request, dict) or set(request) != {
        "schema", "operation", "status_path", "control_socket", "request_b64"
    }:
        raise ValueError("request has an invalid envelope")
    if request.get("schema") != SCHEMA:
        raise ValueError("request schema is unsupported")
    operation = request.get("operation")
    status_path = absolute_path(request.get("status_path"), "status_path")
    control_socket = absolute_path(request.get("control_socket"), "control_socket")
    if operation == "capture_status":
        if request.get("request_b64") is not None:
            raise ValueError("capture_status cannot include a control request")
        raw, mtime_ns = read_regular_file(status_path)
        result = {
            "path": status_path,
            "size": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "mtime_ns": mtime_ns,
            "hostname": socket.gethostname(),
            "raw_b64": base64.b64encode(raw).decode("ascii"),
        }
    elif operation == "send_control":
        encoded = request.get("request_b64")
        if not isinstance(encoded, str):
            raise ValueError("send_control requires request_b64")
        raw_request = base64.b64decode(encoded.encode("ascii"), validate=True)
        if (
            not raw_request.endswith(b"\n")
            or len(raw_request) < 3
            or len(raw_request) > 16384
        ):
            raise ValueError("control request is not bounded newline JSON")
        socket_stat = os.lstat(control_socket)
        if not stat.S_ISSOCK(socket_stat.st_mode):
            raise ValueError("control_socket is not a Unix socket")
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(40.0)
        try:
            client.connect(control_socket)
            client.sendall(raw_request)
            client.shutdown(socket.SHUT_WR)
            chunks = []
            total = 0
            while True:
                chunk = client.recv(min(8192, MAX_CONTROL + 1 - total))
                if not chunk:
                    break
                chunks.append(chunk)
                total += len(chunk)
                if total > MAX_CONTROL:
                    raise ValueError("control response exceeds the bounded size")
                if b"\n" in chunk:
                    break
            raw_response = b"".join(chunks)
        finally:
            client.close()
        if (
            not raw_response.endswith(b"\n")
            or raw_response.count(b"\n") != 1
            or len(raw_response) > MAX_CONTROL
        ):
            raise ValueError("control response is not one bounded newline JSON object")
        result = {
            "socket_path": control_socket,
            "hostname": socket.gethostname(),
            "request_size": len(raw_request),
            "request_sha256": hashlib.sha256(raw_request).hexdigest(),
            "response_size": len(raw_response),
            "response_sha256": hashlib.sha256(raw_response).hexdigest(),
            "response_b64": base64.b64encode(raw_response).decode("ascii"),
        }
    else:
        raise ValueError("operation is unsupported")
    emit({
        "schema": SCHEMA,
        "ok": True,
        "operation": operation,
        "result": result,
        "error": None,
    })
except Exception as exc:
    fail(type(exc).__name__, exc)
"""
REMOTE_PEER_HELPER_COMMAND = (
    'python3 -c "import base64;'
    "exec(base64.b64decode('"
    + base64.b64encode(REMOTE_PEER_HELPER.encode("utf-8")).decode("ascii")
    + "'))\""
)


class EvidenceReservation:
    """One exclusively created evidence file kept open through capture."""

    def __init__(
        self,
        *,
        root: Path,
        path: Path,
        label: str,
    ) -> None:
        self.root = root.resolve(strict=True)
        candidate = path if path.is_absolute() else self.root / path
        self.path = Path(os.path.abspath(os.fspath(candidate)))
        self.label = label
        self.fd: int | None = None
        self.written = False
        self.external_io_started = False
        self._reserve()

    def _reserve(self) -> None:
        try:
            relative = self.path.relative_to(self.root)
        except ValueError as exc:
            raise ValueError(
                f"{self.label} evidence path must stay inside the repository"
            ) from exc
        if not relative.parts:
            raise ValueError(f"{self.label} evidence path cannot be the root")
        if any(
            ":" in part or any(ord(char) < 32 for char in part)
            for part in relative.parts
        ):
            raise ValueError(
                f"{self.label} evidence path contains an unsafe component"
            )
        self._assert_safe_parents(require_exists=False)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._assert_safe_parents(require_exists=True)
        flags = (
            os.O_RDWR
            | os.O_CREAT
            | os.O_EXCL
            | getattr(os, "O_BINARY", 0)
            | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_NOFOLLOW", 0)
        )
        try:
            self.fd = os.open(self.path, flags, 0o600)
        except FileExistsError as exc:
            raise ValueError(
                f"refusing to overwrite reserved evidence: {self.path}"
            ) from exc
        try:
            self._assert_path_identity()
            self._write_marker(
                state="reserved_before_external_io",
                external_io_started=False,
            )
        except Exception:
            self._close()
            try:
                self.path.unlink()
            except OSError:
                pass
            raise

    def _assert_safe_parents(self, *, require_exists: bool) -> None:
        relative = self.path.relative_to(self.root)
        cursor = self.root
        if is_link_or_reparse(cursor):
            raise ValueError(
                f"{self.label} repository root cannot be a link/reparse point"
            )
        for part in relative.parts[:-1]:
            cursor /= part
            lexically_exists = os.path.lexists(cursor)
            if lexically_exists and is_link_or_reparse(cursor):
                raise ValueError(
                    f"{self.label} evidence parent cannot be a link/reparse point"
                )
            if cursor.exists() and not cursor.is_dir():
                raise ValueError(
                    f"{self.label} evidence parent must be a directory"
                )
            if require_exists and not cursor.is_dir():
                raise ValueError(
                    f"{self.label} evidence parent was not created"
                )

    def _assert_path_identity(self) -> None:
        if self.fd is None:
            raise ValueError(f"{self.label} evidence reservation is closed")
        self._assert_safe_parents(require_exists=True)
        if is_link_or_reparse(self.path):
            raise ValueError(
                f"{self.label} evidence path cannot be a link/reparse point"
            )
        path_stat = os.lstat(self.path)
        if not os.path.samestat(os.fstat(self.fd), path_stat):
            raise ValueError(
                f"{self.label} evidence reservation identity changed"
            )
        resolved = self.path.resolve(strict=True)
        resolved.relative_to(self.root)

    def _write_raw(self, raw: bytes) -> None:
        if self.fd is None:
            raise ValueError(f"{self.label} evidence reservation is closed")
        self._assert_path_identity()
        os.lseek(self.fd, 0, os.SEEK_SET)
        os.ftruncate(self.fd, 0)
        offset = 0
        while offset < len(raw):
            written = os.write(self.fd, raw[offset:])
            if written <= 0:
                raise OSError("evidence write made no progress")
            offset += written
        os.fsync(self.fd)
        self._assert_path_identity()
        stat_result = os.fstat(self.fd)
        if stat_result.st_size != len(raw):
            raise OSError(
                f"{self.label} evidence size changed during write"
            )

    def _marker_bytes(
        self,
        *,
        state: str,
        external_io_started: bool,
        error_type: str | None = None,
    ) -> bytes:
        marker = {
            "schema": 1,
            "kind": "sigui_evidence_reservation",
            "state": state,
            "label": self.label,
            "path": self.path.relative_to(self.root).as_posix(),
            "external_io_started": external_io_started,
            "transmission_may_have_occurred": external_io_started,
            "error_type": error_type,
        }
        return (
            json.dumps(marker, sort_keys=True, separators=(",", ":"))
            + "\n"
        ).encode("ascii")

    def _write_marker(
        self,
        *,
        state: str,
        external_io_started: bool,
        error_type: str | None = None,
    ) -> None:
        self._write_raw(
            self._marker_bytes(
                state=state,
                external_io_started=external_io_started,
                error_type=error_type,
            )
        )

    def mark_external_io_started(self) -> None:
        if self.written:
            return
        self._write_marker(
            state="reserved_external_io_may_follow",
            external_io_started=True,
        )
        self.external_io_started = True

    def mark_incomplete(self, error_type: str) -> None:
        if self.written:
            return
        try:
            self._write_marker(
                state="incomplete_external_io_may_have_occurred",
                external_io_started=True,
                error_type=error_type[:96],
            )
        except Exception:
            # The pre-I/O marker remains the fail-closed evidence when a
            # post-I/O disk failure prevents a more specific marker.
            pass
        self.external_io_started = True

    def write_bytes(self, raw: bytes) -> None:
        if self.written:
            raise ValueError(
                f"{self.label} evidence reservation was already written"
            )
        self._write_raw(raw)
        self.written = True

    def receipt(
        self,
        *,
        raw: bytes,
        source_path: str,
        source_host: str | None = None,
        transport: str | None = None,
        extra: dict | None = None,
    ) -> dict:
        if not self.written:
            raise ValueError(
                f"{self.label} evidence reservation is not complete"
            )
        self._assert_path_identity()
        digest = hashlib.sha256(raw).hexdigest()
        if (
            self.path.stat().st_size != len(raw)
            or sha256_file(self.path) != digest
        ):
            raise ValueError(
                f"{self.label} evidence bytes changed after capture"
            )
        receipt = {
            "path": self.path.relative_to(self.root).as_posix(),
            "size": len(raw),
            "sha256": digest,
            "source_path": source_path,
            "captured_at": datetime.now(timezone.utc).isoformat(),
        }
        if source_host is not None:
            receipt["source_host"] = source_host
        if transport is not None:
            receipt["transport"] = transport
        if extra:
            receipt.update(extra)
        return receipt

    def cleanup_if_safe(self) -> None:
        if self.written or self.external_io_started:
            return
        try:
            self._assert_path_identity()
            self._close()
            self.path.unlink()
        except (OSError, RuntimeError, ValueError):
            self._close()

    def _close(self) -> None:
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def close(self) -> None:
        self._close()


class EvidenceBundle:
    """A group of evidence files reserved before any external operation."""

    def __init__(self, root: Path) -> None:
        self.root = root.resolve(strict=True)
        self.files: dict[str, EvidenceReservation] = {}
        self.external_io_started = False

    def reserve(
        self, name: str, path: Path, *, label: str | None = None
    ) -> EvidenceReservation:
        if self.external_io_started:
            raise ValueError(
                "cannot reserve additional evidence after external I/O started"
            )
        if name in self.files:
            raise ValueError(f"duplicate evidence reservation {name}")
        try:
            reservation = EvidenceReservation(
                root=self.root,
                path=path,
                label=label or name,
            )
        except Exception:
            self.cleanup_if_safe()
            raise
        self.files[name] = reservation
        return reservation

    def get(self, name: str) -> EvidenceReservation:
        if name not in self.files:
            raise ValueError(f"missing evidence reservation {name}")
        return self.files[name]

    def mark_external_io_started(self) -> None:
        if self.external_io_started:
            return
        for reservation in self.files.values():
            reservation.mark_external_io_started()
        self.external_io_started = True

    def mark_incomplete(self, exc: BaseException) -> None:
        if not self.external_io_started:
            return
        for reservation in self.files.values():
            reservation.mark_incomplete(type(exc).__name__)

    def cleanup_if_safe(self) -> None:
        if self.external_io_started:
            return
        for reservation in self.files.values():
            reservation.cleanup_if_safe()

    def close(self) -> None:
        for reservation in self.files.values():
            reservation.close()

    def __enter__(self) -> "EvidenceBundle":
        return self

    def __exit__(self, exc_type, exc, _traceback) -> bool:
        if exc is not None:
            if self.external_io_started:
                self.mark_incomplete(exc)
            else:
                self.cleanup_if_safe()
        self.close()
        return False


def utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def default_token(commit: str | None = None) -> str:
    prefix = f"rf_accept_{commit[:7]}" if commit else "rf_accept"
    return f"{prefix}_{utc_stamp()}"


def normalize_port(value: str | None) -> str | None:
    if not isinstance(value, str) or not value.strip():
        return None
    normalized = value.strip().upper().replace("/", "\\")
    for prefix in ("\\\\.\\", "\\\\?\\"):
        if normalized.startswith(prefix):
            normalized = normalized[len(prefix):]
            break
    return normalized


def default_d1l_target() -> str:
    return (
        D1L_REQUIRED_PORT
        if os.name == "nt"
        else D1L_REQUIRED_POSIX_TARGET
    )


def exact_commit(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip().lower()
    if re.fullmatch(r"[0-9a-f]{40}", normalized) is None:
        return None
    return normalized


def validate_safe_token(
    value: object,
    *,
    max_length: int = 96,
) -> str:
    if not isinstance(value, str):
        raise ValueError("RF token must be a string")
    if (
        not value
        or len(value) > max_length
        or len(value.encode("utf-8")) > max_length
        or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", value) is None
    ):
        raise ValueError(
            f"RF token must be 1-{max_length} ASCII letters, digits, dots, "
            "underscores, or hyphens and cannot contain whitespace or "
            "console metacharacters"
        )
    return value


def _reject_json_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number {value!r} is not allowed")


def _object_without_duplicate_keys(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON key {key!r}")
        value[key] = item
    return value


def strict_json_object(raw: bytes, label: str) -> dict:
    try:
        decoded = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError(f"{label} is not valid UTF-8") from exc
    try:
        value = json.loads(
            decoded,
            object_pairs_hook=_object_without_duplicate_keys,
            parse_constant=_reject_json_constant,
        )
    except (TypeError, ValueError, json.JSONDecodeError) as exc:
        raise ValueError(f"{label} is not strict JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be a JSON object")
    return value


def _bounded_posix_path(value: object, label: str) -> str:
    if (
        not isinstance(value, str)
        or not value.startswith("/")
        or len(value) > 512
        or any(ord(char) < 32 for char in value)
    ):
        raise ValueError(f"{label} must be a bounded absolute POSIX path")
    path = PurePosixPath(value)
    if (
        any(part in {".", ".."} for part in value.split("/"))
        or str(path) != value
    ):
        raise ValueError(f"{label} cannot contain dot segments")
    return value


def validate_ssh_host(value: object) -> str:
    if not isinstance(value, str):
        raise ValueError("controlled-peer SSH host is required")
    host = value.strip()
    if (
        not host
        or len(host) > 255
        or host.startswith("-")
        or any(ord(char) < 33 or ord(char) > 126 for char in host)
        or re.fullmatch(
            r"(?:[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}@)?"
            r"[A-Za-z0-9](?:[A-Za-z0-9.-]{0,252}[A-Za-z0-9])?",
            host,
        )
        is None
    ):
        raise ValueError(
            "controlled-peer SSH host must be a bounded user@host or host name"
        )
    if host != REMOTE_PEER_SSH_HOST:
        raise ValueError(
            "remote controlled-peer SSH target must be exactly "
            f"{REMOTE_PEER_SSH_HOST}"
        )
    return host


def validate_remote_peer_config(config: object) -> dict:
    if not isinstance(config, dict):
        raise ValueError("remote controlled-peer configuration is required")
    expected_keys = {
        "ssh_host",
        "hostname",
        "status_path",
        "control_socket",
        "device",
        "public_key",
        "max_status_age_sec",
    }
    if set(config) != expected_keys:
        raise ValueError("remote controlled-peer configuration has invalid fields")
    ssh_host = validate_ssh_host(config.get("ssh_host"))
    hostname = config.get("hostname")
    if hostname != REMOTE_PEER_HOSTNAME:
        raise ValueError(
            "remote controlled-peer hostname must be exactly "
            f"{REMOTE_PEER_HOSTNAME}"
        )
    status_path = _bounded_posix_path(
        config.get("status_path"), "controlled-peer status path"
    )
    control_socket = _bounded_posix_path(
        config.get("control_socket"), "controlled-peer control socket"
    )
    if status_path != REMOTE_PEER_STATUS_PATH:
        raise ValueError(
            "remote controlled-peer status path must be exactly "
            f"{REMOTE_PEER_STATUS_PATH}"
        )
    if control_socket != REMOTE_PEER_CONTROL_SOCKET:
        raise ValueError(
            "remote controlled-peer control socket must be exactly "
            f"{REMOTE_PEER_CONTROL_SOCKET}"
        )
    device = _bounded_posix_path(
        config.get("device"), "controlled-peer device"
    )
    forbidden = {
        item.casefold()
        for item in (*FORBIDDEN_PORTS, REMOTE_PEER_FORBIDDEN_DEVICE)
    }
    if device.casefold() in forbidden:
        raise ValueError(f"refusing forbidden controlled-peer device {device}")
    if device != REMOTE_PEER_DEVICE:
        raise ValueError(
            "remote controlled-peer device must be exactly "
            f"{REMOTE_PEER_DEVICE}"
        )
    public_key = exact_public_key(config.get("public_key"))
    if public_key is None:
        raise ValueError(
            "remote controlled-peer public key must be exactly 64 hex"
        )
    if public_key != REMOTE_PEER_PUBLIC_KEY:
        raise ValueError("remote controlled-peer public key does not match the pin")
    max_age = config.get("max_status_age_sec")
    if (
        isinstance(max_age, bool)
        or not isinstance(max_age, (int, float))
        or not 1.0 <= float(max_age) <= REMOTE_PEER_MAX_STATUS_AGE_SEC
    ):
        raise ValueError(
            "remote controlled-peer status age must be between 1 and "
            f"{REMOTE_PEER_MAX_STATUS_AGE_SEC:g} seconds"
        )
    return {
        "ssh_host": ssh_host,
        "hostname": hostname,
        "status_path": status_path,
        "control_socket": control_socket,
        "device": device,
        "public_key": public_key,
        "max_status_age_sec": float(max_age),
    }


def remote_peer_config(
    *,
    ssh_host: str = REMOTE_PEER_SSH_HOST,
    hostname: str = REMOTE_PEER_HOSTNAME,
    status_path: str = REMOTE_PEER_STATUS_PATH,
    control_socket: str = REMOTE_PEER_CONTROL_SOCKET,
    device: str = REMOTE_PEER_DEVICE,
    public_key: str = REMOTE_PEER_PUBLIC_KEY,
    max_status_age_sec: float = REMOTE_PEER_MAX_STATUS_AGE_SEC,
) -> dict:
    return validate_remote_peer_config(
        {
            "ssh_host": ssh_host,
            "hostname": hostname,
            "status_path": status_path,
            "control_socket": control_socket,
            "device": device,
            "public_key": public_key,
            "max_status_age_sec": max_status_age_sec,
        }
    )


def validate_local_peer_config(config: object) -> dict:
    if not isinstance(config, dict):
        raise ValueError("local controlled-peer configuration is required")
    expected_keys = {
        "hostname",
        "status_path",
        "control_socket",
        "device",
        "public_key",
        "max_status_age_sec",
    }
    if set(config) != expected_keys:
        raise ValueError("local controlled-peer configuration has invalid fields")
    validated = validate_remote_peer_config(
        {
            "ssh_host": REMOTE_PEER_SSH_HOST,
            **config,
        }
    )
    return {key: validated[key] for key in expected_keys}


def local_peer_config(
    *,
    hostname: str = REMOTE_PEER_HOSTNAME,
    status_path: str = REMOTE_PEER_STATUS_PATH,
    control_socket: str = REMOTE_PEER_CONTROL_SOCKET,
    device: str = REMOTE_PEER_DEVICE,
    public_key: str = REMOTE_PEER_PUBLIC_KEY,
    max_status_age_sec: float = REMOTE_PEER_MAX_STATUS_AGE_SEC,
) -> dict:
    return validate_local_peer_config(
        {
            "hostname": hostname,
            "status_path": status_path,
            "control_socket": control_socket,
            "device": device,
            "public_key": public_key,
            "max_status_age_sec": max_status_age_sec,
        }
    )


def require_local_peer_hostname() -> str:
    observed = socket.gethostname()
    if observed != REMOTE_PEER_HOSTNAME:
        raise RemotePeerError(
            "local_hostname_mismatch",
            "local controlled-peer mode requires hostname "
            f"{REMOTE_PEER_HOSTNAME}; observed {observed!r}",
        )
    return observed


def controlled_peer_report_row(
    config: dict,
    fingerprint: str,
    *,
    local_mode: bool,
) -> dict:
    validated = (
        validate_local_peer_config(config)
        if local_mode
        else validate_remote_peer_config(config)
    )
    row = {
        "fingerprint": fingerprint,
        "evidence_source": (
            LOCAL_PEER_EVIDENCE_SOURCE if local_mode else REMOTE_PEER_EVIDENCE_SOURCE
        ),
        "port": None,
        "status_path": validated["status_path"],
        "hostname": validated["hostname"],
        "control_socket": validated["control_socket"],
        "device": validated["device"],
        "public_key": validated["public_key"],
        "max_status_age_sec": validated["max_status_age_sec"],
    }
    if local_mode:
        row["access_mode"] = "local"
    else:
        row["ssh_host"] = validated["ssh_host"]
    return row


def _remote_peer_request(config: dict, operation: str, raw: bytes | None) -> bytes:
    request = {
        "schema": REMOTE_PEER_HELPER_SCHEMA,
        "operation": operation,
        "status_path": config["status_path"],
        "control_socket": config["control_socket"],
        "request_b64": (
            base64.b64encode(raw).decode("ascii")
            if raw is not None
            else None
        ),
    }
    return json.dumps(
        request,
        ensure_ascii=True,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _remote_peer_ssh_identity_args() -> list[str]:
    raw = os.environ.get(REMOTE_PEER_SSH_IDENTITY_ENV)
    if raw is None:
        return []
    value = raw.strip()
    if not value or "\x00" in value:
        raise RemotePeerError(
            "ssh_identity_invalid",
            "controlled-peer SSH identity setting "
            f"{REMOTE_PEER_SSH_IDENTITY_ENV} must name one private-key file",
        )
    try:
        supplied = Path(value).expanduser()
        identity = supplied.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise RemotePeerError(
            "ssh_identity_invalid",
            "configured controlled-peer SSH identity is unavailable",
        ) from exc
    if (
        not supplied.is_absolute()
        or supplied.is_symlink()
        or is_link_or_reparse(supplied)
        or not identity.is_file()
    ):
        raise RemotePeerError(
            "ssh_identity_invalid",
            "configured controlled-peer SSH identity must be an absolute, "
            "regular, non-linked file",
        )
    return ["-o", "IdentitiesOnly=yes", "-i", str(identity)]


def run_remote_peer_operation(
    config: dict,
    operation: str,
    *,
    control_request: bytes | None = None,
    timeout_sec: float = REMOTE_PEER_SSH_TIMEOUT_SEC,
) -> dict:
    config = validate_remote_peer_config(config)
    if operation not in {"capture_status", "send_control"}:
        raise ValueError("unsupported remote controlled-peer operation")
    if (operation == "capture_status") != (control_request is None):
        raise ValueError("remote controlled-peer operation payload mismatch")
    argv = [
        "ssh",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=8",
        "-o",
        "StrictHostKeyChecking=yes",
        "-o",
        "LogLevel=ERROR",
        *_remote_peer_ssh_identity_args(),
        config["ssh_host"],
        REMOTE_PEER_HELPER_COMMAND,
    ]
    try:
        completed = subprocess.run(
            argv,
            input=_remote_peer_request(
                config, operation, control_request
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_sec,
            check=False,
            shell=False,
        )
    except FileNotFoundError as exc:
        raise RemotePeerError(
            "ssh_unavailable", "OpenSSH client executable was not found"
        ) from exc
    except subprocess.TimeoutExpired as exc:
        raise RemotePeerError(
            "ssh_timeout",
            "remote controlled-peer operation exceeded its bounded timeout",
        ) from exc
    stderr = completed.stderr.decode("utf-8", errors="replace")
    if completed.returncode != 0:
        if "permission denied" in stderr.casefold():
            raise RemotePeerError(
                "ssh_auth_failed",
                "noninteractive SSH authentication failed; supply ephemeral "
                "authorized access before RF acceptance",
            )
        raise RemotePeerError(
            "ssh_failed",
            "remote controlled-peer SSH command failed with exit code "
            f"{completed.returncode}",
        )
    if len(completed.stdout) > (REMOTE_PEER_MAX_STATUS_BYTES * 2):
        raise RemotePeerError(
            "ssh_response_too_large",
            "remote controlled-peer response exceeded the bounded size",
        )
    try:
        envelope = strict_json_object(
            completed.stdout, "remote controlled-peer response"
        )
    except ValueError as exc:
        raise RemotePeerError("ssh_invalid_response", str(exc)) from exc
    error = envelope.get("error")
    if not (
        set(envelope)
        == {"schema", "ok", "operation", "result", "error"}
        and envelope.get("schema") == REMOTE_PEER_HELPER_SCHEMA
        and isinstance(envelope.get("ok"), bool)
        and envelope.get("operation") in {operation, None}
        and isinstance(error, (dict, type(None)))
    ):
        raise RemotePeerError(
            "ssh_invalid_response",
            "remote controlled-peer response envelope is invalid",
        )
    if envelope.get("ok") is not True:
        if not (
            envelope.get("operation") is None
            and envelope.get("result") is None
            and isinstance(error, dict)
            and set(error) == {"code", "message"}
            and isinstance(error.get("code"), str)
            and bool(error["code"])
            and isinstance(error.get("message"), str)
        ):
            raise RemotePeerError(
                "ssh_invalid_response",
                "remote controlled-peer failure response is invalid",
            )
        code = (
            str(error.get("code") or "remote_operation_failed")
            if isinstance(error, dict)
            else "remote_operation_failed"
        )
        raise RemotePeerError(
            code,
            "remote controlled-peer operation failed closed",
        )
    if not (
        envelope.get("operation") == operation
        and isinstance(envelope.get("result"), dict)
        and error is None
    ):
        raise RemotePeerError(
            "ssh_invalid_response",
            "remote controlled-peer success response is incomplete",
        )
    return envelope["result"]


def _local_posix_path_snapshot(
    path: str,
    *,
    final_kind: str,
) -> tuple[tuple[str, int, int, int], ...]:
    """Capture a non-linked POSIX path chain and its inode identities."""
    parts = PurePosixPath(path).parts
    if (
        not parts
        or parts[0] != "/"
        or final_kind not in {"regular", "socket"}
    ):
        raise ValueError("local controlled-peer path is invalid")
    rows: list[tuple[str, int, int, int]] = []
    for index in range(1, len(parts)):
        current = "/" + "/".join(parts[1 : index + 1])
        current_stat = os.lstat(current)
        mode = current_stat.st_mode
        if stat.S_ISLNK(mode):
            raise ValueError(
                "local controlled-peer path cannot contain symlinks"
            )
        final = index == len(parts) - 1
        if not final and not stat.S_ISDIR(mode):
            raise ValueError(
                "local controlled-peer parent is not a directory"
            )
        if final and (
            (final_kind == "regular" and not stat.S_ISREG(mode))
            or (final_kind == "socket" and not stat.S_ISSOCK(mode))
        ):
            raise ValueError(
                "local controlled-peer endpoint has the wrong type"
            )
        rows.append(
            (
                current,
                current_stat.st_dev,
                current_stat.st_ino,
                stat.S_IFMT(mode),
            )
        )
    if not rows:
        raise ValueError("local controlled-peer endpoint is missing")
    return tuple(rows)


def _read_local_regular_file(path: str) -> tuple[bytes, os.stat_result]:
    path_before = _local_posix_path_snapshot(
        path, final_kind="regular"
    )
    flags = (
        os.O_RDONLY
        | getattr(os, "O_BINARY", 0)
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )
    fd = os.open(path, flags)
    try:
        before = os.fstat(fd)
        if (
            before.st_dev != path_before[-1][1]
            or before.st_ino != path_before[-1][2]
        ):
            raise ValueError(
                "local controlled-peer status identity changed before open"
            )
        if not stat.S_ISREG(before.st_mode):
            raise ValueError("local controlled-peer status is not a regular file")
        if not 1 <= before.st_size <= REMOTE_PEER_MAX_STATUS_BYTES:
            raise ValueError("local controlled-peer status size is out of bounds")
        chunks: list[bytes] = []
        remaining = REMOTE_PEER_MAX_STATUS_BYTES + 1
        while remaining > 0:
            chunk = os.read(fd, min(65536, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks)
        after = os.fstat(fd)
        if (
            len(raw) > REMOTE_PEER_MAX_STATUS_BYTES
            or before.st_dev != after.st_dev
            or before.st_ino != after.st_ino
            or before.st_size != after.st_size
            or before.st_mtime_ns != after.st_mtime_ns
            or len(raw) != before.st_size
        ):
            raise ValueError("local controlled-peer status changed during capture")
        path_after = _local_posix_path_snapshot(
            path, final_kind="regular"
        )
        if path_after != path_before:
            raise ValueError(
                "local controlled-peer status path changed during capture"
            )
        return raw, after
    finally:
        os.close(fd)


def run_local_peer_operation(
    config: dict,
    operation: str,
    *,
    control_request: bytes | None = None,
    timeout_sec: float = REMOTE_PEER_SSH_TIMEOUT_SEC,
) -> dict:
    config = validate_local_peer_config(config)
    hostname = require_local_peer_hostname()
    if (
        isinstance(timeout_sec, bool)
        or not isinstance(timeout_sec, (int, float))
        or not 0.1 <= float(timeout_sec) <= REMOTE_PEER_SSH_TIMEOUT_SEC
    ):
        raise ValueError(
            "local controlled-peer timeout must be bounded to 0.1-45 seconds"
        )
    if operation not in {"capture_status", "send_control"}:
        raise ValueError("unsupported local controlled-peer operation")
    if (operation == "capture_status") != (control_request is None):
        raise ValueError("local controlled-peer operation payload mismatch")
    if operation == "capture_status":
        try:
            raw, status_stat = _read_local_regular_file(config["status_path"])
        except (OSError, ValueError) as exc:
            raise RemotePeerError(
                "local_status_failed",
                "local controlled-peer status capture failed closed",
            ) from exc
        return {
            "path": config["status_path"],
            "size": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "mtime_ns": status_stat.st_mtime_ns,
            "hostname": hostname,
            "raw_b64": base64.b64encode(raw).decode("ascii"),
        }

    request_raw = control_request or b""
    if (
        not request_raw.endswith(b"\n")
        or request_raw.count(b"\n") != 1
        or not 3 <= len(request_raw) <= 16384
    ):
        raise ValueError(
            "local control request must be one bounded newline JSON object"
        )
    try:
        strict_json_object(request_raw[:-1], "local controlled-peer request")
        socket_before = _local_posix_path_snapshot(
            config["control_socket"], final_kind="socket"
        )
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(timeout_sec)
        try:
            client.connect(config["control_socket"])
            peer_credentials = client.getsockopt(
                socket.SOL_SOCKET,
                LOCAL_PEER_SO_PEERCRED,
                struct.calcsize("3i"),
            )
            if (
                not isinstance(peer_credentials, bytes)
                or len(peer_credentials) != struct.calcsize("3i")
            ):
                raise ValueError(
                    "local controlled-peer credentials are unavailable"
                )
            peer_pid, peer_uid, peer_gid = struct.unpack(
                "3i", peer_credentials
            )
            if (
                peer_pid <= 0
                or peer_uid != LOCAL_PEER_CONTROL_UID
                or peer_gid != LOCAL_PEER_CONTROL_GID
            ):
                raise ValueError(
                    "local controlled-peer credentials do not match "
                    "the root-owned responder"
                )
            client.sendall(request_raw)
            client.shutdown(socket.SHUT_WR)
            chunks = []
            total = 0
            while True:
                remaining = REMOTE_PEER_MAX_CONTROL_BYTES + 1 - total
                chunk = client.recv(min(8192, remaining))
                if not chunk:
                    break
                chunks.append(chunk)
                total += len(chunk)
                if total > REMOTE_PEER_MAX_CONTROL_BYTES:
                    raise ValueError("local control response exceeds the bounded size")
                if b"\n" in chunk:
                    break
        finally:
            client.close()
        socket_after = _local_posix_path_snapshot(
            config["control_socket"], final_kind="socket"
        )
        if socket_after != socket_before:
            raise ValueError(
                "local controlled-peer control socket changed during exchange"
            )
        response_raw = b"".join(chunks)
        if (
            not response_raw.endswith(b"\n")
            or response_raw.count(b"\n") != 1
            or not 3 <= len(response_raw) <= REMOTE_PEER_MAX_CONTROL_BYTES
        ):
            raise ValueError(
                "local control response is not one bounded newline JSON object"
            )
        strict_json_object(response_raw[:-1], "local controlled-peer response")
    except (OSError, ValueError) as exc:
        raise RemotePeerError(
            "local_control_failed",
            "local controlled-peer Unix-socket exchange failed closed",
        ) from exc
    return {
        "socket_path": config["control_socket"],
        "hostname": hostname,
        "request_size": len(request_raw),
        "request_sha256": hashlib.sha256(request_raw).hexdigest(),
        "response_size": len(response_raw),
        "response_sha256": hashlib.sha256(response_raw).hexdigest(),
        "response_b64": base64.b64encode(response_raw).decode("ascii"),
        "peer_pid": peer_pid,
        "peer_uid": peer_uid,
        "peer_gid": peer_gid,
    }


def enforce_port_policy(
    port: str, peer_port: str | None = None
) -> tuple[str, str | None]:
    legacy_port = normalize_port(port)
    if legacy_port in FORBIDDEN_PORTS:
        raise ValueError(f"refusing forbidden D1L port {legacy_port}")
    normalized_port = enforce_core_port(port)
    normalized_peer = normalize_port(peer_port)
    if normalized_peer in FORBIDDEN_PORTS:
        raise ValueError(f"refusing forbidden controlled-peer port {normalized_peer}")
    if (
        normalized_peer is not None
        and re.fullmatch(r"COM[1-9][0-9]*", normalized_peer) is None
    ):
        raise ValueError(f"invalid controlled-peer port {normalized_peer}")
    if normalized_peer in RF_PEER_FORBIDDEN_PORTS:
        raise ValueError(
            f"refusing controlled-peer port {normalized_peer} for RF acceptance"
        )
    if normalized_peer is not None and normalized_peer == normalized_port:
        raise ValueError("D1L and controlled-peer ports must be distinct")
    return normalized_port, normalized_peer


def d1l_target_continuity_ok(
    *,
    port: object,
    before: object,
    after: object,
) -> bool:
    try:
        requested = enforce_core_port(port)
        validate_snapshot(before, requested)
        validate_snapshot(after, requested)
    except (TypeError, ValueError):
        return False
    return bool(
        isinstance(before, dict)
        and isinstance(after, dict)
        and before.get("requested_path") == requested
        and after.get("requested_path") == requested
        and before.get("stable_identity_sha256")
        == after.get("stable_identity_sha256")
    )


def firmware_identity_matches(
    version_result: object, expected_commit: str
) -> bool:
    return (
        isinstance(version_result, dict)
        and version_result.get("ok") is True
        and exact_commit(version_result.get("build_commit")) == expected_commit
    )


def protocol_tx_ready_for_rf(version_result: object) -> bool:
    if not isinstance(version_result, dict):
        return False
    time_state = version_result.get("time")
    return (
        isinstance(time_state, dict)
        and time_state.get("protocol_tx_ready") is True
        and time_state.get("protocol_tx_block") == "none"
    )


def read_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def get_path(obj: dict | None, *parts: str, default=None):
    current = obj
    for part in parts:
        if not isinstance(current, dict) or part not in current:
            return default
        current = current[part]
    return current


def status_snapshot(status: dict | None) -> dict:
    if not status:
        return {}
    counters = status.get("counters") if isinstance(status.get("counters"), dict) else {}
    return {
        "serial": status.get("serial") if isinstance(status.get("serial"), dict) else {},
        "discord": status.get("discord") if isinstance(status.get("discord"), dict) else {},
        "mesh": status.get("mesh") if isinstance(status.get("mesh"), dict) else {},
        "counters": {
            "rx_contact_total": counters.get("rx_contact_total"),
            "rx_log_total": counters.get("rx_log_total"),
            "relay_success_total": counters.get("relay_success_total"),
            "discord_send_success_total": counters.get("discord_send_success_total"),
            "rx_dm_total": counters.get("rx_dm_total"),
            "tx_dm_total": counters.get("tx_dm_total"),
            "tx_dm_ack_miss_total": counters.get("tx_dm_ack_miss_total"),
            "local_fast_reply_total": counters.get("local_fast_reply_total"),
        },
        "run_id": status.get("run_id"),
        "service": status.get("service"),
        "status_written_at": status.get("status_written_at"),
    }


def radio_listener_connected(
    status: dict | None, peer_port: str | None, fingerprint: str
) -> bool:
    serial_status = get_path(status, "serial", default={})
    public_key = get_path(status, "serial", "public_key")
    return (
        isinstance(serial_status, dict)
        and normalize_port(serial_status.get("port")) == peer_port
        and serial_status.get("mesh_connected") is True
        and exact_public_key(public_key) is not None
        and public_key_fingerprint(str(public_key or "")) == fingerprint
        and status.get("service") == "openclaw-radio-listener"
        and isinstance(status.get("run_id"), str)
        and bool(status.get("run_id"))
    )


def counter_value(status: dict | None, name: str) -> int | None:
    value = get_path(status, "counters", name)
    return (
        value
        if isinstance(value, int) and not isinstance(value, bool) and value >= 0
        else None
    )


def counter_delta(
    before: dict | None, after: dict | None, name: str
) -> int | None:
    left = counter_value(before, name)
    right = counter_value(after, name)
    return right - left if left is not None and right is not None else None


def new_exact_listener_reply(
    before: dict | None,
    after: dict | None,
    fingerprint: str,
    expected_text: str = RADIO_LISTENER_REPLY,
) -> bool:
    before_rows = {
        json.dumps(entry, sort_keys=True, separators=(",", ":"))
        for entry in entries(before)
        if entry.get("direction") == "rx"
        and entry.get("text") == expected_text
        and entry_matches_fingerprint(before, entry, fingerprint)
    }
    return any(
        entry.get("direction") == "rx"
        and entry.get("text") == expected_text
        and entry_matches_fingerprint(after, entry, fingerprint)
        and json.dumps(entry, sort_keys=True, separators=(",", ":"))
        not in before_rows
        for entry in entries(after)
    )


def capture_peer_status(
    source_path: Path,
    capture_path: Path,
    root: Path,
    *,
    reservation: EvidenceReservation | None = None,
) -> tuple[dict, dict]:
    raw = source_path.read_bytes()
    value = strict_json_object(raw, "controlled-peer status")
    receipt = capture_peer_bytes(
        raw,
        capture_path,
        root,
        source_path=str(source_path.resolve()),
        reservation=reservation,
    )
    return value, receipt


def capture_peer_bytes(
    raw: bytes,
    capture_path: Path,
    root: Path,
    *,
    source_path: str,
    source_host: str | None = None,
    transport: str | None = None,
    extra: dict | None = None,
    reservation: EvidenceReservation | None = None,
) -> dict:
    owned_bundle: EvidenceBundle | None = None
    if reservation is None:
        owned_bundle = EvidenceBundle(root)
        reservation = owned_bundle.reserve(
            "capture",
            capture_path,
            label="controlled-peer capture",
        )
    elif reservation.path != capture_path.resolve(strict=False):
        raise ValueError(
            "controlled-peer capture path does not match its reservation"
        )
    try:
        reservation.write_bytes(raw)
        return reservation.receipt(
            raw=raw,
            source_path=source_path,
            source_host=source_host,
            transport=transport,
            extra=extra,
        )
    except Exception as exc:
        if owned_bundle is not None:
            owned_bundle.mark_incomplete(exc)
        raise
    finally:
        if owned_bundle is not None:
            owned_bundle.close()


def parse_aware_timestamp(value: object) -> datetime | None:
    if not isinstance(value, str) or not value.strip():
        return None
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        return None
    return parsed.astimezone(timezone.utc)


def _fresh_timestamp(
    value: object,
    observed_at: datetime,
    max_age_sec: float,
) -> tuple[bool, float | None]:
    parsed = parse_aware_timestamp(value)
    if parsed is None:
        return False, None
    age = (observed_at - parsed).total_seconds()
    return -30.0 <= age <= max_age_sec, age


def validate_local_status_mtime(
    value: object,
    *,
    status_written_at: object,
    observed_at: datetime,
    max_age_sec: float,
) -> dict:
    """Bind a local status file's stat mtime to its payload timestamp."""
    if observed_at.tzinfo is None or observed_at.utcoffset() is None:
        raise ValueError("local status observation time must be timezone-aware")
    observed = observed_at.astimezone(timezone.utc)
    mtime: datetime | None = None
    if (
        isinstance(value, int)
        and not isinstance(value, bool)
        and value > 0
    ):
        seconds, nanoseconds = divmod(value, 1_000_000_000)
        try:
            mtime = datetime.fromtimestamp(
                seconds, timezone.utc
            ).replace(microsecond=nanoseconds // 1000)
        except (OverflowError, OSError, ValueError):
            mtime = None
    status_written = parse_aware_timestamp(status_written_at)
    mtime_age = (
        (observed - mtime).total_seconds()
        if mtime is not None
        else None
    )
    status_delta = (
        (mtime - status_written).total_seconds()
        if mtime is not None and status_written is not None
        else None
    )
    checks = {
        "source_mtime_positive_epoch_ns": mtime is not None,
        "source_mtime_fresh": (
            mtime_age is not None
            and -LOCAL_PEER_MAX_MTIME_STATUS_SKEW_SEC
            <= mtime_age
            <= max_age_sec
        ),
        "source_mtime_matches_status_timestamp": (
            status_delta is not None
            and abs(status_delta)
            <= LOCAL_PEER_MAX_MTIME_STATUS_SKEW_SEC
        ),
    }
    return {
        "ok": all(checks.values()),
        "source_mtime_ns": value,
        "source_mtime_at": (
            mtime.isoformat() if mtime is not None else None
        ),
        "source_mtime_age_sec": mtime_age,
        "source_mtime_status_delta_sec": status_delta,
        "checks": checks,
    }


def validate_remote_peer_status(
    status: object,
    config: dict,
    *,
    observed_at: datetime | None = None,
) -> dict:
    config = validate_remote_peer_config(config)
    observed = observed_at or datetime.now(timezone.utc)
    if observed.tzinfo is None or observed.utcoffset() is None:
        raise ValueError("remote status observation time must be timezone-aware")
    value = status if isinstance(status, dict) else {}
    status_fresh, status_age = _fresh_timestamp(
        value.get("status_written_at"),
        observed,
        config["max_status_age_sec"],
    )
    fetch_fresh, fetch_age = _fresh_timestamp(
        get_path(value, "mesh", "last_fetch_ok_at"),
        observed,
        config["max_status_age_sec"],
    )
    public_key = exact_public_key(
        get_path(value, "serial", "public_key")
    )
    serial_device = get_path(value, "serial", "port")
    forbidden = {
        item.casefold()
        for item in (*FORBIDDEN_PORTS, REMOTE_PEER_FORBIDDEN_DEVICE)
    }
    device_non_forbidden = (
        isinstance(serial_device, str)
        and serial_device.casefold() not in forbidden
    )
    checks = {
        "service_identity": value.get("service")
        == "openclaw-radio-listener",
        "run_identity": isinstance(value.get("run_id"), str)
        and bool(value["run_id"]),
        "device_exact": serial_device == config["device"],
        "device_non_forbidden": device_non_forbidden,
        "mesh_connected": get_path(
            value, "serial", "mesh_connected"
        )
        is True,
        "public_key_exact": public_key == config["public_key"],
        "self_prefix_exact": str(
            get_path(value, "serial", "self_prefix") or ""
        ).casefold()
        == config["public_key"][:12].casefold(),
        "fingerprint_exact": public_key_fingerprint(public_key or "")
        == REMOTE_PEER_FINGERPRINT,
        "startup_self_test_enabled": get_path(
            value, "startup_self_test", "enabled"
        )
        is True,
        "startup_self_test_ok": get_path(
            value, "startup_self_test", "ok"
        )
        is True,
        "status_timestamp_fresh": status_fresh,
        "mesh_fetch_timestamp_fresh": fetch_fresh,
    }
    return {
        "ok": all(checks.values()),
        "observed_at": observed.astimezone(timezone.utc).isoformat(),
        "max_status_age_sec": config["max_status_age_sec"],
        "status_age_sec": status_age,
        "mesh_fetch_age_sec": fetch_age,
        "checks": checks,
    }


def validate_local_peer_status(
    status: object,
    config: dict,
    *,
    observed_at: datetime | None = None,
    source_mtime_ns: object,
) -> dict:
    local_config = validate_local_peer_config(config)
    observed = observed_at or datetime.now(timezone.utc)
    validation = validate_remote_peer_status(
        status,
        {
            "ssh_host": REMOTE_PEER_SSH_HOST,
            **local_config,
        },
        observed_at=observed,
    )
    value = status if isinstance(status, dict) else {}
    mtime_validation = validate_local_status_mtime(
        source_mtime_ns,
        status_written_at=value.get("status_written_at"),
        observed_at=observed,
        max_age_sec=local_config["max_status_age_sec"],
    )
    checks = {
        **validation["checks"],
        **mtime_validation["checks"],
    }
    return {
        **validation,
        "ok": all(checks.values()),
        "checks": checks,
        "source_mtime": mtime_validation,
    }


def _capture_remote_peer_status_reserved(
    config: dict,
    capture_path: Path,
    root: Path,
    *,
    reservation: EvidenceReservation,
    evidence_bundle: EvidenceBundle,
) -> tuple[dict, dict, dict]:
    config = validate_remote_peer_config(config)
    if not evidence_bundle.external_io_started:
        raise ValueError(
            "remote status reservation must belong to an external-I/O bundle"
        )
    result = run_remote_peer_operation(config, "capture_status")
    try:
        raw = base64.b64decode(
            str(result.get("raw_b64") or "").encode("ascii"),
            validate=True,
        )
    except (ValueError, UnicodeEncodeError) as exc:
        raise RemotePeerError(
            "remote_status_invalid",
            "remote status capture has invalid base64",
        ) from exc
    digest = hashlib.sha256(raw).hexdigest()
    if not (
        result.get("path") == config["status_path"]
        and result.get("size") == len(raw)
        and result.get("sha256") == digest
        and isinstance(result.get("mtime_ns"), int)
        and not isinstance(result.get("mtime_ns"), bool)
        and result.get("hostname") == config["hostname"]
        and 1 <= len(raw) <= REMOTE_PEER_MAX_STATUS_BYTES
    ):
        raise RemotePeerError(
            "remote_status_invalid",
            "remote status capture metadata does not match its raw bytes",
        )
    try:
        status = strict_json_object(raw, "remote controlled-peer status")
    except ValueError as exc:
        raise RemotePeerError("remote_status_invalid", str(exc)) from exc
    receipt = capture_peer_bytes(
        raw,
        capture_path,
        root,
        source_path=config["status_path"],
        source_host=config["ssh_host"],
        transport="ssh",
        extra={
            "source_hostname": result["hostname"],
            "remote_mtime_ns": result["mtime_ns"],
            "remote_sha256": digest,
        },
        reservation=reservation,
    )
    observed_at = parse_aware_timestamp(receipt["captured_at"])
    validation = validate_remote_peer_status(
        status,
        config,
        observed_at=observed_at,
    )
    if validation["ok"] is not True:
        raise RemotePeerError(
            "remote_status_not_ready",
            "remote controlled-peer status failed identity/readiness checks",
        )
    return status, receipt, validation


def capture_remote_peer_status(
    config: dict,
    capture_path: Path,
    root: Path,
    *,
    reservation: EvidenceReservation | None = None,
    evidence_bundle: EvidenceBundle | None = None,
) -> tuple[dict, dict, dict]:
    if reservation is not None:
        if evidence_bundle is None:
            raise ValueError(
                "remote status reservation requires its evidence bundle"
            )
        return _capture_remote_peer_status_reserved(
            config,
            capture_path,
            root,
            reservation=reservation,
            evidence_bundle=evidence_bundle,
        )
    if evidence_bundle is not None:
        raise ValueError(
            "remote status evidence bundle requires a reservation"
        )
    with EvidenceBundle(root) as owned_bundle:
        owned_reservation = owned_bundle.reserve(
            "remote_status",
            capture_path,
            label="remote controlled-peer status",
        )
        owned_bundle.mark_external_io_started()
        return _capture_remote_peer_status_reserved(
            config,
            capture_path,
            root,
            reservation=owned_reservation,
            evidence_bundle=owned_bundle,
        )


def _capture_local_peer_status_reserved(
    config: dict,
    capture_path: Path,
    root: Path,
    *,
    reservation: EvidenceReservation,
    evidence_bundle: EvidenceBundle,
) -> tuple[dict, dict, dict]:
    config = validate_local_peer_config(config)
    if not evidence_bundle.external_io_started:
        raise ValueError(
            "local status reservation must belong to an external-I/O bundle"
        )
    result = run_local_peer_operation(config, "capture_status")
    try:
        raw = base64.b64decode(
            str(result.get("raw_b64") or "").encode("ascii"),
            validate=True,
        )
    except (ValueError, UnicodeEncodeError) as exc:
        raise RemotePeerError(
            "local_status_invalid",
            "local status capture has invalid base64",
        ) from exc
    digest = hashlib.sha256(raw).hexdigest()
    if not (
        result.get("path") == config["status_path"]
        and result.get("size") == len(raw)
        and result.get("sha256") == digest
        and isinstance(result.get("mtime_ns"), int)
        and not isinstance(result.get("mtime_ns"), bool)
        and result.get("hostname") == config["hostname"]
        and 1 <= len(raw) <= REMOTE_PEER_MAX_STATUS_BYTES
    ):
        raise RemotePeerError(
            "local_status_invalid",
            "local status capture metadata does not match its raw bytes",
        )
    try:
        status = strict_json_object(raw, "local controlled-peer status")
    except ValueError as exc:
        raise RemotePeerError("local_status_invalid", str(exc)) from exc
    receipt = capture_peer_bytes(
        raw,
        capture_path,
        root,
        source_path=config["status_path"],
        source_host=config["hostname"],
        transport=LOCAL_PEER_STATUS_TRANSPORT,
        extra={
            "source_hostname": result["hostname"],
            "source_mtime_ns": result["mtime_ns"],
            "source_sha256": digest,
        },
        reservation=reservation,
    )
    observed_at = parse_aware_timestamp(receipt["captured_at"])
    validation = validate_local_peer_status(
        status,
        config,
        observed_at=observed_at,
        source_mtime_ns=result["mtime_ns"],
    )
    if validation["ok"] is not True:
        raise RemotePeerError(
            "local_status_not_ready",
            "local controlled-peer status failed identity/readiness checks",
        )
    return status, receipt, validation


def capture_local_peer_status(
    config: dict,
    capture_path: Path,
    root: Path,
    *,
    reservation: EvidenceReservation | None = None,
    evidence_bundle: EvidenceBundle | None = None,
) -> tuple[dict, dict, dict]:
    if reservation is not None:
        if evidence_bundle is None:
            raise ValueError("local status reservation requires its evidence bundle")
        return _capture_local_peer_status_reserved(
            config,
            capture_path,
            root,
            reservation=reservation,
            evidence_bundle=evidence_bundle,
        )
    if evidence_bundle is not None:
        raise ValueError("local status evidence bundle requires a reservation")
    with EvidenceBundle(root) as owned_bundle:
        owned_reservation = owned_bundle.reserve(
            "local_status",
            capture_path,
            label="local controlled-peer status",
        )
        owned_bundle.mark_external_io_started()
        return _capture_local_peer_status_reserved(
            config,
            capture_path,
            root,
            reservation=owned_reservation,
            evidence_bundle=owned_bundle,
        )


def remote_control_request(
    d1l_public_key: object,
    token: object,
) -> tuple[dict, bytes]:
    target = exact_public_key(d1l_public_key)
    if target is None:
        raise ValueError("remote DM target must be an exact 64-hex public key")
    token = validate_safe_token(token, max_length=128)
    request_id = "sigui-rf-" + hashlib.sha256(
        (target + "\0" + token).encode("utf-8")
    ).hexdigest()[:24]
    request = {
        "id": request_id,
        "op": "radio.send_dm",
        "params": {"target": target, "text": token},
    }
    raw = (
        json.dumps(
            request,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        + b"\n"
    )
    return request, raw


def validate_remote_control_exchange(
    request_raw: bytes,
    response_raw: bytes,
    *,
    d1l_public_key: object,
    token: object,
) -> dict:
    expected_request, expected_raw = remote_control_request(
        d1l_public_key, token
    )
    try:
        request = strict_json_object(
            request_raw.rstrip(b"\n"), "remote control request"
        )
        response = strict_json_object(
            response_raw.rstrip(b"\n"), "remote control response"
        )
    except ValueError:
        request = {}
        response = {}
    target = exact_public_key(d1l_public_key)
    delivery = (
        get_path(response, "result", "delivery", default={})
        if isinstance(response, dict)
        else {}
    )
    delivery = delivery if isinstance(delivery, dict) else {}
    checks = {
        "request_raw_exact": request_raw == expected_raw,
        "request_object_exact": request == expected_request,
        "response_newline_exact": response_raw.endswith(b"\n")
        and response_raw.count(b"\n") == 1,
        "response_id_exact": response.get("id")
        == expected_request["id"],
        "response_op_exact": response.get("op") == "radio.send_dm",
        "response_ok": response.get("ok") is True,
        "response_not_cached": response.get("cached") is False,
        "response_error_clear": response.get("error") is None,
        "target_exact": str(
            get_path(response, "result", "target") or ""
        ).casefold()
        == str(target or "")[:12].casefold(),
        "utf8_bytes_exact": get_path(
            response, "result", "utf8_bytes"
        )
        == len(str(token or "").encode("utf-8")),
        "delivery_acknowledged": delivery.get("acknowledged") is True,
        "delivery_event_present": delivery.get("event") is not None
        and str(delivery.get("event")).upper() != "ERROR",
        "delivery_payload_present": "payload" in delivery,
    }
    return {
        "ok": all(checks.values()),
        "request": request,
        "response": response,
        "request_sha256": hashlib.sha256(request_raw).hexdigest(),
        "response_sha256": hashlib.sha256(response_raw).hexdigest(),
        "checks": checks,
    }


def send_remote_peer_dm(
    config: dict,
    *,
    d1l_public_key: str,
    token: str,
    request_capture_path: Path,
    response_capture_path: Path,
    root: Path,
    request_reservation: EvidenceReservation | None = None,
    response_reservation: EvidenceReservation | None = None,
    evidence_bundle: EvidenceBundle | None = None,
) -> dict:
    config = validate_remote_peer_config(config)
    request, request_raw = remote_control_request(
        d1l_public_key, token
    )
    owned_bundle: EvidenceBundle | None = None
    if request_reservation is None or response_reservation is None:
        if request_reservation is not None or response_reservation is not None:
            raise ValueError(
                "remote control request/response reservations must be paired"
            )
        owned_bundle = EvidenceBundle(root)
        request_reservation = owned_bundle.reserve(
            "request",
            request_capture_path,
            label="remote control request",
        )
        response_reservation = owned_bundle.reserve(
            "response",
            response_capture_path,
            label="remote control response",
        )
        owned_bundle.mark_external_io_started()
    elif evidence_bundle is None or not evidence_bundle.external_io_started:
        raise ValueError(
            "remote control reservations must belong to an external-I/O bundle"
        )
    try:
        result = run_remote_peer_operation(
            config,
            "send_control",
            control_request=request_raw,
        )
        try:
            response_raw = base64.b64decode(
                str(result.get("response_b64") or "").encode("ascii"),
                validate=True,
            )
        except (ValueError, UnicodeEncodeError) as exc:
            raise RemotePeerError(
                "remote_control_invalid",
                "remote control response has invalid base64",
            ) from exc
        request_digest = hashlib.sha256(request_raw).hexdigest()
        response_digest = hashlib.sha256(response_raw).hexdigest()
        if not (
            result.get("socket_path") == config["control_socket"]
            and result.get("hostname") == config["hostname"]
            and result.get("request_size") == len(request_raw)
            and result.get("request_sha256") == request_digest
            and result.get("response_size") == len(response_raw)
            and result.get("response_sha256") == response_digest
            and 1 <= len(response_raw) <= REMOTE_PEER_MAX_CONTROL_BYTES
        ):
            raise RemotePeerError(
                "remote_control_invalid",
                "remote control exchange metadata does not match its raw bytes",
            )
        request_receipt = capture_peer_bytes(
            request_raw,
            request_capture_path,
            root,
            source_path=config["control_socket"],
            source_host=config["ssh_host"],
            transport="ssh-unix-socket-request",
            extra={"source_hostname": result["hostname"]},
            reservation=request_reservation,
        )
        response_receipt = capture_peer_bytes(
            response_raw,
            response_capture_path,
            root,
            source_path=config["control_socket"],
            source_host=config["ssh_host"],
            transport="ssh-unix-socket-response",
            extra={"source_hostname": result["hostname"]},
            reservation=response_reservation,
        )
        validation = validate_remote_control_exchange(
            request_raw,
            response_raw,
            d1l_public_key=d1l_public_key,
            token=token,
        )
        if validation["ok"] is not True:
            raise RemotePeerError(
                "remote_control_delivery_failed",
                "remote radio.send_dm did not return exact acknowledged delivery",
            )
        return {
            "op": "radio.send_dm",
            "socket_path": config["control_socket"],
            "request_id": request["id"],
            "request": request,
            "response": validation["response"],
            "request_receipt": request_receipt,
            "response_receipt": response_receipt,
            "request_sha256": validation["request_sha256"],
            "response_sha256": validation["response_sha256"],
            "validation": validation,
        }
    except Exception as exc:
        if owned_bundle is not None:
            owned_bundle.mark_incomplete(exc)
        raise
    finally:
        if owned_bundle is not None:
            owned_bundle.close()


def send_local_peer_dm(
    config: dict,
    *,
    d1l_public_key: str,
    token: str,
    request_capture_path: Path,
    response_capture_path: Path,
    root: Path,
    request_reservation: EvidenceReservation | None = None,
    response_reservation: EvidenceReservation | None = None,
    evidence_bundle: EvidenceBundle | None = None,
) -> dict:
    config = validate_local_peer_config(config)
    request, request_raw = remote_control_request(d1l_public_key, token)
    owned_bundle: EvidenceBundle | None = None
    if request_reservation is None or response_reservation is None:
        if request_reservation is not None or response_reservation is not None:
            raise ValueError(
                "local control request/response reservations must be paired"
            )
        owned_bundle = EvidenceBundle(root)
        request_reservation = owned_bundle.reserve(
            "request",
            request_capture_path,
            label="local control request",
        )
        response_reservation = owned_bundle.reserve(
            "response",
            response_capture_path,
            label="local control response",
        )
        owned_bundle.mark_external_io_started()
    elif evidence_bundle is None or not evidence_bundle.external_io_started:
        raise ValueError(
            "local control reservations must belong to an external-I/O bundle"
        )
    try:
        result = run_local_peer_operation(
            config,
            "send_control",
            control_request=request_raw,
        )
        try:
            response_raw = base64.b64decode(
                str(result.get("response_b64") or "").encode("ascii"),
                validate=True,
            )
        except (ValueError, UnicodeEncodeError) as exc:
            raise RemotePeerError(
                "local_control_invalid",
                "local control response has invalid base64",
            ) from exc
        request_digest = hashlib.sha256(request_raw).hexdigest()
        response_digest = hashlib.sha256(response_raw).hexdigest()
        if not (
            result.get("socket_path") == config["control_socket"]
            and result.get("hostname") == config["hostname"]
            and result.get("request_size") == len(request_raw)
            and result.get("request_sha256") == request_digest
            and result.get("response_size") == len(response_raw)
            and result.get("response_sha256") == response_digest
            and isinstance(result.get("peer_pid"), int)
            and not isinstance(result.get("peer_pid"), bool)
            and result.get("peer_pid") > 0
            and result.get("peer_uid") == LOCAL_PEER_CONTROL_UID
            and result.get("peer_gid") == LOCAL_PEER_CONTROL_GID
            and 1 <= len(response_raw) <= REMOTE_PEER_MAX_CONTROL_BYTES
        ):
            raise RemotePeerError(
                "local_control_invalid",
                "local control exchange metadata does not match its raw bytes",
            )
        receipt_extra = {
            "source_hostname": result["hostname"],
            "source_peer_pid": result["peer_pid"],
            "source_peer_uid": result["peer_uid"],
            "source_peer_gid": result["peer_gid"],
        }
        request_receipt = capture_peer_bytes(
            request_raw,
            request_capture_path,
            root,
            source_path=config["control_socket"],
            source_host=config["hostname"],
            transport=LOCAL_PEER_CONTROL_REQUEST_TRANSPORT,
            extra=receipt_extra,
            reservation=request_reservation,
        )
        response_receipt = capture_peer_bytes(
            response_raw,
            response_capture_path,
            root,
            source_path=config["control_socket"],
            source_host=config["hostname"],
            transport=LOCAL_PEER_CONTROL_RESPONSE_TRANSPORT,
            extra=receipt_extra,
            reservation=response_reservation,
        )
        validation = validate_remote_control_exchange(
            request_raw,
            response_raw,
            d1l_public_key=d1l_public_key,
            token=token,
        )
        if validation["ok"] is not True:
            raise RemotePeerError(
                "local_control_delivery_failed",
                "local radio.send_dm did not return exact acknowledged delivery",
            )
        return {
            "op": "radio.send_dm",
            "socket_path": config["control_socket"],
            "request_id": request["id"],
            "request": request,
            "response": validation["response"],
            "request_receipt": request_receipt,
            "response_receipt": response_receipt,
            "request_sha256": validation["request_sha256"],
            "response_sha256": validation["response_sha256"],
            "validation": validation,
        }
    except Exception as exc:
        if owned_bundle is not None:
            owned_bundle.mark_incomplete(exc)
        raise
    finally:
        if owned_bundle is not None:
            owned_bundle.close()


def contains_token(value, token: str) -> bool:
    return token in json.dumps(value, sort_keys=True)


def public_key_fingerprint(public_key: str) -> str | None:
    key = str(public_key or "").strip().upper()
    if len(key) < 16:
        return None
    prefix = key[:16]
    if not all(char in "0123456789ABCDEF" for char in prefix):
        return None
    return prefix


def exact_public_key(public_key: object) -> str | None:
    if not isinstance(public_key, str):
        return None
    normalized = public_key.strip().lower()
    return (
        normalized
        if re.fullmatch(r"[0-9a-f]{64}", normalized)
        else None
    )


def listener_sender_matches(
    status: dict | None, d1l_public_key: object
) -> bool:
    public_key = exact_public_key(d1l_public_key)
    sender = get_path(status, "mesh", "last_rx_sender")
    return (
        public_key is not None
        and isinstance(sender, str)
        and re.fullmatch(r"[0-9A-Fa-f]{12}", sender) is not None
        and sender.upper() == public_key[:12].upper()
    )


def remote_peer_flow_validation(
    *,
    before: dict,
    after: dict,
    before_validation: dict,
    after_validation: dict,
    d1l_public_key: str,
    control: dict,
) -> dict:
    before_written = parse_aware_timestamp(before.get("status_written_at"))
    after_written = parse_aware_timestamp(after.get("status_written_at"))
    deltas = {
        name: counter_delta(before, after, name)
        for name in (
            "rx_dm_total",
            "tx_dm_total",
            "local_fast_reply_total",
            "tx_dm_ack_miss_total",
        )
    }
    tx_delta = deltas["tx_dm_total"]
    fast_reply_delta = deltas["local_fast_reply_total"]
    control_validation = (
        control.get("validation")
        if isinstance(control, dict)
        and isinstance(control.get("validation"), dict)
        else {}
    )
    checks = {
        "before_status_ready": before_validation.get("ok") is True,
        "after_status_ready": after_validation.get("ok") is True,
        "same_run_identity": isinstance(before.get("run_id"), str)
        and bool(before["run_id"])
        and before.get("run_id") == after.get("run_id"),
        "same_peer_public_key": exact_public_key(
            get_path(before, "serial", "public_key")
        )
        == REMOTE_PEER_PUBLIC_KEY
        and exact_public_key(get_path(after, "serial", "public_key"))
        == REMOTE_PEER_PUBLIC_KEY,
        "status_time_advanced": before_written is not None
        and after_written is not None
        and after_written >= before_written,
        "one_d1l_dm_received": deltas["rx_dm_total"] == 1,
        "peer_dm_send_observed": isinstance(tx_delta, int)
        and not isinstance(tx_delta, bool)
        and 1 <= tx_delta <= 2,
        "fast_reply_bounded": isinstance(fast_reply_delta, int)
        and not isinstance(fast_reply_delta, bool)
        and 0 <= fast_reply_delta <= 1,
        "peer_tx_exactly_control_plus_fast_reply": isinstance(tx_delta, int)
        and not isinstance(tx_delta, bool)
        and isinstance(fast_reply_delta, int)
        and not isinstance(fast_reply_delta, bool)
        and tx_delta == 1 + fast_reply_delta,
        "no_ack_miss_delta": deltas["tx_dm_ack_miss_total"] == 0,
        "d1l_sender_exact": listener_sender_matches(
            after, d1l_public_key
        ),
        "last_rx_is_dm": get_path(after, "mesh", "last_rx_kind")
        == "dm",
        "last_tx_is_control_dm": get_path(
            after, "mesh", "last_tx_kind"
        )
        == "control_dm",
        "rx_timestamp_advanced": get_path(
            before, "mesh", "last_rx_at"
        )
        != get_path(after, "mesh", "last_rx_at"),
        "tx_timestamp_advanced": get_path(
            before, "mesh", "last_tx_at"
        )
        != get_path(after, "mesh", "last_tx_at"),
        "control_delivery_acknowledged": control_validation.get("ok")
        is True,
    }
    return {"ok": all(checks.values()), "checks": checks, "deltas": deltas}


def remote_control_semantic_ok(
    control: object,
    *,
    d1l_public_key: object,
    token: object,
    control_socket: object = REMOTE_PEER_CONTROL_SOCKET,
) -> bool:
    if not isinstance(control, dict):
        return False
    try:
        expected_request, _ = remote_control_request(
            d1l_public_key, token
        )
    except ValueError:
        return False
    response = control.get("response")
    response = response if isinstance(response, dict) else {}
    delivery = get_path(response, "result", "delivery", default={})
    delivery = delivery if isinstance(delivery, dict) else {}
    target = exact_public_key(d1l_public_key)
    validation = control.get("validation")
    validation = validation if isinstance(validation, dict) else {}
    validation_checks = validation.get("checks")
    return (
        control.get("op") == "radio.send_dm"
        and control.get("socket_path") == control_socket
        and control.get("request_id") == expected_request["id"]
        and control.get("request") == expected_request
        and response.get("id") == expected_request["id"]
        and response.get("op") == "radio.send_dm"
        and response.get("ok") is True
        and response.get("cached") is False
        and response.get("error") is None
        and str(get_path(response, "result", "target") or "").casefold()
        == str(target or "")[:12].casefold()
        and get_path(response, "result", "utf8_bytes")
        == len(str(token or "").encode("utf-8"))
        and delivery.get("acknowledged") is True
        and delivery.get("event") is not None
        and str(delivery.get("event")).upper() != "ERROR"
        and "payload" in delivery
        and isinstance(control.get("request_receipt"), dict)
        and isinstance(control.get("response_receipt"), dict)
        and isinstance(control.get("request_sha256"), str)
        and re.fullmatch(
            r"[0-9a-f]{64}", control["request_sha256"]
        )
        is not None
        and isinstance(control.get("response_sha256"), str)
        and re.fullmatch(
            r"[0-9a-f]{64}", control["response_sha256"]
        )
        is not None
        and validation.get("ok") is True
        and isinstance(validation_checks, dict)
        and bool(validation_checks)
        and all(value is True for value in validation_checks.values())
    )


def remote_peer_report_shape_ok(data: object) -> bool:
    if not isinstance(data, dict):
        return False
    peer = data.get("controlled_peer")
    remote = data.get("controlled_peer_remote")
    if not isinstance(peer, dict) or not isinstance(remote, dict):
        return False
    evidence_source = peer.get("evidence_source")
    local_mode = evidence_source == LOCAL_PEER_EVIDENCE_SOURCE
    try:
        if local_mode:
            config = validate_local_peer_config(
                {
                    "hostname": peer.get("hostname"),
                    "status_path": peer.get("status_path"),
                    "control_socket": peer.get("control_socket"),
                    "device": peer.get("device"),
                    "public_key": peer.get("public_key"),
                    "max_status_age_sec": peer.get("max_status_age_sec"),
                }
            )
            binding_shape_ok = (
                peer.get("access_mode") == "local"
                and "ssh_host" not in peer
                and data.get("controlled_peer_adapter") == LOCAL_PEER_ADAPTER
            )
        elif evidence_source == REMOTE_PEER_EVIDENCE_SOURCE:
            config = validate_remote_peer_config(
                {
                    "ssh_host": peer.get("ssh_host"),
                    "hostname": peer.get("hostname"),
                    "status_path": peer.get("status_path"),
                    "control_socket": peer.get("control_socket"),
                    "device": peer.get("device"),
                    "public_key": peer.get("public_key"),
                    "max_status_age_sec": peer.get("max_status_age_sec"),
                }
            )
            binding_shape_ok = (
                "access_mode" not in peer
                and data.get("controlled_peer_adapter") == REMOTE_PEER_ADAPTER
            )
        else:
            return False
    except ValueError:
        return False
    before_validation = remote.get("before_validation")
    after_validation = remote.get("after_validation")
    flow = remote.get("flow")
    before_checks = (
        before_validation.get("checks") if isinstance(before_validation, dict) else None
    )
    after_checks = (
        after_validation.get("checks") if isinstance(after_validation, dict) else None
    )
    flow_checks = flow.get("checks") if isinstance(flow, dict) else None
    return (
        binding_shape_ok
        and peer.get("port") is None
        and peer.get("fingerprint") == REMOTE_PEER_FINGERPRINT
        and config["public_key"] == REMOTE_PEER_PUBLIC_KEY
        and isinstance(before_validation, dict)
        and before_validation.get("ok") is True
        and isinstance(before_checks, dict)
        and bool(before_checks)
        and all(value is True for value in before_checks.values())
        and isinstance(after_validation, dict)
        and after_validation.get("ok") is True
        and isinstance(after_checks, dict)
        and bool(after_checks)
        and all(value is True for value in after_checks.values())
        and isinstance(flow, dict)
        and flow.get("ok") is True
        and isinstance(flow_checks, dict)
        and bool(flow_checks)
        and all(value is True for value in flow_checks.values())
        and remote_control_semantic_ok(
            data.get("controlled_peer_control"),
            d1l_public_key=data.get("d1l_public_key"),
            token=data.get("inbound_token"),
            control_socket=config["control_socket"],
        )
    )


def contact_import_command(public_key: str) -> str:
    normalized = exact_public_key(public_key)
    if normalized is None:
        raise ValueError("controlled-peer public key must be exactly 64 hex")
    return (
        "contacts import meshcore://contact/add?"
        f"name={RADIO_LISTENER_CONTACT_NAME}"
        f"&public_key={normalized}&type=1"
    )


def contact_import_ok(
    result: object, public_key: str, fingerprint: str
) -> bool:
    normalized = exact_public_key(public_key)
    return (
        isinstance(result, dict)
        and result.get("ok") is True
        and result.get("cmd") == "contacts import"
        and result.get("persisted") is True
        and result.get("result") in {"created", "updated", "promoted"}
        and result.get("verification_source") == "uri_import"
        and str(result.get("fingerprint") or "").upper() == fingerprint
        and exact_public_key(result.get("public_key")) == normalized
        and result.get("alias") == RADIO_LISTENER_CONTACT_NAME
        and result.get("type") == "chat"
        and result.get("canonical") is True
        and result.get("can_dm") is True
        and result.get("can_admin") is False
    )


def contacts_has_exact_peer(
    result: object, public_key: str, fingerprint: str
) -> bool:
    normalized = exact_public_key(public_key)
    if not isinstance(result, dict) or result.get("ok") is not True:
        return False
    matches = [
        entry
        for entry in entries(result)
        if str(entry.get("fingerprint") or "").upper() == fingerprint
    ]
    return (
        normalized is not None
        and len(matches) == 1
        and exact_public_key(matches[0].get("public_key")) == normalized
        and matches[0].get("alias") == RADIO_LISTENER_CONTACT_NAME
        and matches[0].get("type") == "chat"
        and matches[0].get("verification_source") == "uri_import"
        and matches[0].get("canonical") is True
        and matches[0].get("can_dm") is True
        and matches[0].get("can_admin") is False
    )


def entries(value: dict | None) -> list[dict]:
    rows = value.get("entries") if isinstance(value, dict) else None
    return rows if isinstance(rows, list) else []


def entry_matches_fingerprint(value: dict | None, entry: dict, fingerprint: str) -> bool:
    top_level = value.get("fingerprint") if isinstance(value, dict) else None
    entry_fingerprint = entry.get("fingerprint")
    return (not top_level or top_level == fingerprint) and (not entry_fingerprint or entry_fingerprint == fingerprint)


def messages_have_inbound_token(value: dict | None, token: str, fingerprint: str) -> bool:
    for entry in entries(value):
        text = entry.get("text")
        if (
            entry.get("direction") == "rx"
            and isinstance(text, str)
            and token in text
            and entry_matches_fingerprint(value, entry, fingerprint)
        ):
            return True
    return False


def messages_have_tx_token(value: dict | None, token: str, fingerprint: str) -> bool:
    for entry in entries(value):
        text = entry.get("text")
        if (
            entry.get("direction") == "tx"
            and isinstance(text, str)
            and token in text
            and entry_matches_fingerprint(value, entry, fingerprint)
        ):
            return True
    return False


def messages_have_acked_tx(value: dict | None, token: str, fingerprint: str) -> bool:
    for entry in entries(value):
        ack_hash = entry.get("ack_hash")
        one_entry = {
            "fingerprint": value.get("fingerprint") if isinstance(value, dict) else None,
            "entries": [entry],
        }
        if (
            messages_have_tx_token(one_entry, token, fingerprint)
            and entry.get("acked") is True
            and ack_hash not in (None, "", 0, "0", "0x0", "0X0")
        ):
            return True
    return False


def packets_have_ack_or_path(value: dict | None) -> bool:
    for entry in entries(value):
        kind = str(entry.get("kind", "")).lower()
        if kind.startswith("dm_ack") or kind.startswith("path_return"):
            return True
    return False


def route_has_direct_path(value: dict | None, fingerprint: str) -> bool:
    if not isinstance(value, dict) or value.get("ok") is not True:
        return False
    if value.get("fingerprint") != fingerprint:
        return False
    for entry in entries(value):
        if (
            entry.get("target") == fingerprint
            and entry.get("kind") == "dm_text"
            and entry.get("direction") == "tx"
            and entry.get("route") == "direct"
        ):
            return True
    return False


def _positive_seq(value: object) -> int | None:
    return (
        value
        if isinstance(value, int)
        and not isinstance(value, bool)
        and value >= 1
        else None
    )


def new_entries_by_seq(
    before: dict | None, after: dict | None
) -> list[dict]:
    before_rows = entries(before)
    after_rows = entries(after)
    before_seq_list = [
        seq
        for row in before_rows
        if (seq := _positive_seq(row.get("seq"))) is not None
    ]
    after_seqs = [
        seq
        for row in after_rows
        if (seq := _positive_seq(row.get("seq"))) is not None
    ]
    if (
        len(before_seq_list) != len(before_rows)
        or len(before_seq_list) != len(set(before_seq_list))
        or len(after_seqs) != len(after_rows)
        or len(after_seqs) != len(set(after_seqs))
    ):
        return []
    before_seqs = set(before_seq_list)
    baseline_max = max(before_seq_list, default=0)
    fresh = [
        row
        for row in after_rows
        if _positive_seq(row.get("seq")) not in before_seqs
    ]
    if any(
        (_positive_seq(row.get("seq")) or 0) <= baseline_max
        for row in fresh
    ):
        return []
    return fresh


def correlated_listener_transaction(
    *,
    baseline_messages: dict | None,
    final_messages: dict | None,
    baseline_packets: dict | None,
    final_packets: dict | None,
    baseline_route: dict | None,
    final_route: dict | None,
    outbound_token: str,
    fingerprint: str,
    inbound_text: str = RADIO_LISTENER_REPLY,
) -> dict:
    new_messages = new_entries_by_seq(
        baseline_messages, final_messages
    )
    new_packets = new_entries_by_seq(baseline_packets, final_packets)
    new_routes = new_entries_by_seq(baseline_route, final_route)
    tx_rows = [
        row
        for row in new_messages
        if row.get("direction") == "tx"
        and row.get("text")
        == f"core acceptance test {outbound_token}"
        and entry_matches_fingerprint(
            final_messages, row, fingerprint
        )
    ]
    reply_rows = [
        row
        for row in new_messages
        if row.get("direction") == "rx"
        and row.get("text") == inbound_text
        and entry_matches_fingerprint(
            final_messages, row, fingerprint
        )
    ]
    tx = tx_rows[0] if len(tx_rows) == 1 else {}
    reply = reply_rows[0] if len(reply_rows) == 1 else {}
    ack_hash = tx.get("ack_hash")
    ack_hash = (
        ack_hash
        if isinstance(ack_hash, int)
        and not isinstance(ack_hash, bool)
        and 0 < ack_hash <= 0xFFFFFFFF
        else None
    )
    tx_ack_response = tx.get("ack_response")
    tx_ack_response = (
        tx_ack_response if isinstance(tx_ack_response, dict) else {}
    )
    reply_ack_response = reply.get("ack_response")
    reply_ack_response = (
        reply_ack_response
        if isinstance(reply_ack_response, dict)
        else {}
    )
    tx_row_ok = (
        bool(tx)
        and not contains_token(baseline_messages, outbound_token)
        and tx.get("acked") is True
        and tx.get("delivered") is True
        and ack_hash is not None
        and tx_ack_response
        == {
            "identity_valid": False,
            "state": "legacy_unverified",
            "dispatch_count": 0,
            "last_kind": "none",
            "last_error": "ESP_OK",
        }
    )
    reply_ack_ok = (
        bool(reply)
        and reply_ack_response.get("identity_valid") is True
        and reply_ack_response.get("state") == "sent"
        and isinstance(
            reply_ack_response.get("dispatch_count"), int
        )
        and not isinstance(
            reply_ack_response.get("dispatch_count"), bool
        )
        and reply_ack_response["dispatch_count"] >= 1
        and reply_ack_response.get("last_kind")
        in {
            "direct_ack",
            "flood_ack",
            "flood_ack_path",
            "path_ack",
        }
        and reply_ack_response.get("last_error") == "ESP_OK"
    )
    ack_note = (
        f"ack {ack_hash} {RADIO_LISTENER_CONTACT_NAME}"
        if ack_hash is not None
        else ""
    )
    packet_rows = [
        row
        for row in new_packets
        if row.get("direction") == "rx"
        and row.get("kind") == "dm_ack"
        and row.get("note") == ack_note
    ]
    route_rows = [
        row
        for row in new_routes
        if row.get("target") == fingerprint
        and row.get("kind") == "dm_ack"
        and row.get("direction") == "rx"
        and row.get("route") == "direct"
    ]
    packet = packet_rows[0] if len(packet_rows) == 1 else {}
    route = route_rows[0] if len(route_rows) == 1 else {}
    metadata_pairs = (
        ("rssi_dbm", "last_rssi_dbm"),
        ("snr_tenths", "last_snr_tenths"),
        ("path_hash_bytes", "path_hash_bytes"),
        ("path_hops", "path_hops"),
        ("payload_len", "payload_len"),
    )
    packet_metadata_ok = (
        isinstance(packet.get("rssi_dbm"), int)
        and not isinstance(packet.get("rssi_dbm"), bool)
        and isinstance(packet.get("snr_tenths"), int)
        and not isinstance(packet.get("snr_tenths"), bool)
        and isinstance(packet.get("path_hash_bytes"), int)
        and not isinstance(packet.get("path_hash_bytes"), bool)
        and 0 <= packet["path_hash_bytes"] <= 64
        and isinstance(packet.get("path_hops"), int)
        and not isinstance(packet.get("path_hops"), bool)
        and 0 <= packet["path_hops"] <= 64
        and isinstance(packet.get("payload_len"), int)
        and not isinstance(packet.get("payload_len"), bool)
        and 0 < packet["payload_len"] <= 65535
    )
    packet_route_metadata_match = (
        bool(packet and route)
        and packet_metadata_ok
        and all(
            packet.get(packet_field) == route.get(route_field)
            for packet_field, route_field in metadata_pairs
        )
    )
    ack_path_ok = (
        tx_row_ok
        and reply_ack_ok
        and len(packet_rows) == 1
        and packet_route_metadata_match
    )
    direct_route_ok = (
        ack_path_ok
        and len(route_rows) == 1
        and route.get("route") == "direct"
    )
    return {
        "ok": ack_path_ok and direct_route_ok,
        "outbound_dm_seq": _positive_seq(tx.get("seq")),
        "inbound_reply_seq": _positive_seq(reply.get("seq")),
        "ack_hash": ack_hash,
        "reply_ack_state": reply_ack_response.get("state"),
        "reply_ack_kind": reply_ack_response.get("last_kind"),
        "packet_seq": _positive_seq(packet.get("seq")),
        "route_seq": _positive_seq(route.get("seq")),
        "packet_route_metadata_match": packet_route_metadata_match,
        "ack_path_ok": ack_path_ok,
        "direct_route_ok": direct_route_ok,
    }


def command_has_public_tx(command: str) -> bool:
    return command.strip().lower().startswith("mesh send public ")


def command_step(steps: list[dict], command: str) -> dict | None:
    for step in steps:
        if step.get("command") == command:
            return step
    return None


def first_command_step(steps: list[dict], command: str) -> dict | None:
    return command_step(steps, command)


def latest_command_step(steps: list[dict], command: str) -> dict | None:
    for step in reversed(steps):
        if step.get("command") == command:
            return step
    return None


def latest_step_with_prefix(steps: list[dict], prefix: str) -> dict | None:
    for step in reversed(steps):
        if str(step.get("command", "")).startswith(prefix):
            return step
    return None


def discord_command(public_key: str, inbound_token: str) -> str:
    return f"+dm {public_key} {inbound_token}"


def dry_run_report(
    *,
    port: str,
    peer_status_path: Path | None,
    peer_port: str | None,
    fingerprint: str,
    public_key: str,
    token: str,
    send_outbound: bool,
    expected_commit: str | None = None,
    github_run_id: str | None = None,
    workflow_run_attempt: str | None = None,
    remote_peer: dict | None = None,
    local_peer: dict | None = None,
) -> dict:
    token = validate_safe_token(token)
    if remote_peer is not None and local_peer is not None:
        raise ValueError(
            "SSH and Pi-local controlled-peer modes are mutually exclusive"
        )
    remote_config = (
        validate_remote_peer_config(remote_peer) if remote_peer is not None else None
    )
    local_config = (
        validate_local_peer_config(local_peer) if local_peer is not None else None
    )
    if local_config is not None:
        require_local_peer_hostname()
    pinned_config = local_config or remote_config
    local_mode = local_config is not None
    outbound_token = f"{token}_out"
    inbound_token = f"{token}_in"
    direct_token = f"{token}_direct"
    commands = [
        "identity status",
        "contacts",
    ]
    if send_outbound:
        commands.extend(
            [
                f"mesh send dm {fingerprint} {outbound_token}",
                f"packets search {outbound_token}",
            ]
        )
    commands.append(f"messages dm {fingerprint}")
    if send_outbound:
        commands.extend(
            [
                "packets",
                f"routes trace {fingerprint}",
                f"mesh send dm {fingerprint} {direct_token}",
                f"packets search {direct_token}",
                f"messages dm {fingerprint}",
            ]
        )
    commands.extend(["packets", f"routes trace {fingerprint}", "health"])
    if pinned_config is not None:
        controlled_peer = controlled_peer_report_row(
            pinned_config,
            fingerprint,
            local_mode=local_mode,
        )
    else:
        controlled_peer = {
            "fingerprint": fingerprint,
            "evidence_source": (
                "explicit_peer_status"
                if peer_status_path is not None and peer_port is not None
                else "d1l_bidirectional_rf"
            ),
            "port": peer_port,
            "status_path": (
                str(peer_status_path) if peer_status_path is not None else None
            ),
        }
    return {
        "schema": RF_FULL_ACCEPTANCE_SCHEMA,
        "mode": "dry-run-rf-full-acceptance",
        "hardware_required": False,
        "physical_observed": False,
        "dry_run": True,
        "simulated": False,
        "simulation": False,
        "source_inspection": False,
        "execution_complete": False,
        "closure_eligible": False,
        "dm_rf_tx": False,
        "public_rf_tx": False,
        "formats_sd": False,
        "port": port,
        "controlled_peer": controlled_peer,
        "controlled_peer_adapter": (
            LOCAL_PEER_ADAPTER
            if local_mode
            else REMOTE_PEER_ADAPTER
            if remote_config is not None
            else None
        ),
        "controlled_peer_control_plan": (
            {
                "op": "radio.send_dm",
                "socket_path": pinned_config["control_socket"],
                "target": public_key,
                "text": inbound_token,
                "transport": ("local-unix-socket" if local_mode else "ssh-stdin-json"),
            }
            if pinned_config is not None
            else None
        ),
        "target_fingerprint": fingerprint,
        "d1l_public_key": public_key,
        "token": token,
        "expected_firmware_commit": exact_commit(expected_commit),
        "github_actions_run": (
            str(github_run_id) if github_run_id is not None else None
        ),
        "workflow_run_attempt": (
            str(workflow_run_attempt) if workflow_run_attempt is not None else None
        ),
        "device_release_profile": None,
        "device_sd_history_mode": None,
        "firmware_identity_required": True,
        "firmware_identity_ok": False,
        "outbound_token": outbound_token,
        "inbound_token": inbound_token,
        "direct_token": direct_token,
        "expected_identity_fingerprint": public_key_fingerprint(public_key),
        "discord_command": (
            None
            if pinned_config is not None
            else discord_command(public_key, inbound_token)
        ),
        "public_rf_transmit": False,
        "commands": commands,
        "ok": True,
    }


def build_report(
    *,
    port: str,
    d1l_target: dict[str, Any],
    d1l_target_after: dict[str, Any],
    baud: int,
    peer_status_path: Path | None,
    peer_port: str | None,
    fingerprint: str,
    public_key: str,
    token: str,
    send_outbound: bool,
    steps: list[dict],
    peer_before: dict | None,
    peer_after: dict | None,
    inbound_seen_at: str | None,
    expected_commit: str | None = None,
    peer_before_receipt: dict | None = None,
    peer_after_receipt: dict | None = None,
    github_run_id: str | None = None,
    workflow_run_attempt: str | None = None,
    remote_peer: dict | None = None,
    local_peer: dict | None = None,
    remote_before_validation: dict | None = None,
    remote_after_validation: dict | None = None,
    remote_control: dict | None = None,
) -> dict:
    token = validate_safe_token(token)
    target_identity_continuity_ok = d1l_target_continuity_ok(
        port=port,
        before=d1l_target,
        after=d1l_target_after,
    )
    outbound_token = f"{token}_out"
    inbound_token = f"{token}_in"
    direct_token = f"{token}_direct"
    if remote_peer is not None and local_peer is not None:
        raise ValueError(
            "SSH and Pi-local controlled-peer modes are mutually exclusive"
        )
    ssh_config = (
        validate_remote_peer_config(remote_peer)
        if remote_peer is not None
        else None
    )
    local_config = (
        validate_local_peer_config(local_peer)
        if local_peer is not None
        else None
    )
    remote_config = local_config or ssh_config
    local_mode = local_config is not None
    remote_mode = remote_config is not None
    listener_mode = remote_mode or (
        peer_port == RADIO_LISTENER_PORT
        and (
            get_path(peer_before, "service") == "openclaw-radio-listener"
            or get_path(peer_after, "service") == "openclaw-radio-listener"
        )
    )
    inbound_text = inbound_token if remote_mode else RADIO_LISTENER_REPLY
    outbound_text = (
        f"core acceptance test {outbound_token}"
        if listener_mode
        else outbound_token
    )
    outbound_command = f"mesh send dm {fingerprint} {outbound_text}"
    direct_command = f"mesh send dm {fingerprint} {direct_token}"
    peer_public_key = exact_public_key(
        get_path(peer_before, "serial", "public_key")
    )
    peer_after_public_key = exact_public_key(
        get_path(peer_after, "serial", "public_key")
    )
    import_command = (
        contact_import_command(peer_public_key)
        if listener_mode and peer_public_key is not None
        else None
    )
    identity_step = latest_command_step(steps, "identity status")
    first_contacts = first_command_step(steps, "contacts")
    latest_contacts = latest_command_step(steps, "contacts")
    import_step = (
        command_step(steps, import_command)
        if import_command is not None
        else None
    )
    outbound_step = command_step(steps, outbound_command)
    outbound_packets = command_step(steps, f"packets search {outbound_token}")
    direct_step = command_step(steps, direct_command)
    latest_messages = latest_step_with_prefix(steps, f"messages dm {fingerprint}")
    first_messages = first_command_step(
        steps, f"messages dm {fingerprint}"
    )
    latest_packets = latest_command_step(steps, "packets")
    latest_route = latest_command_step(steps, f"routes trace {fingerprint}")
    latest_health = latest_command_step(steps, "health")
    version_step = first_command_step(steps, "version")
    identity_result = identity_step.get("result", {}) if identity_step else {}
    route_result = latest_route.get("result", {}) if latest_route else {}
    packets_result = latest_packets.get("result", {}) if latest_packets else {}
    messages_result = latest_messages.get("result", {}) if latest_messages else {}
    baseline_messages_result = (
        first_messages.get("result", {}) if first_messages else {}
    )
    first_packets = first_command_step(steps, "packets")
    baseline_packets_result = (
        first_packets.get("result", {}) if first_packets else {}
    )
    first_route = first_command_step(
        steps, f"routes trace {fingerprint}"
    )
    baseline_route_result = (
        first_route.get("result", {}) if first_route else {}
    )
    health_result = latest_health.get("result", {}) if latest_health else {}
    version_result = version_step.get("result", {}) if version_step else {}
    contact_before_result = (
        first_contacts.get("result", {}) if first_contacts else {}
    )
    contact_after_result = (
        latest_contacts.get("result", {}) if latest_contacts else {}
    )
    contact_import_result = (
        import_step.get("result", {}) if import_step else {}
    )
    commands = [str(step.get("command", "")) for step in steps]
    identity_fingerprint = str(identity_result.get("fingerprint") or "").upper()
    expected_identity = public_key_fingerprint(public_key)
    peer_status_requested = (
        remote_mode
        or peer_status_path is not None
        or peer_port is not None
    )
    if remote_mode:
        remote_flow = remote_peer_flow_validation(
            before=peer_before or {},
            after=peer_after or {},
            before_validation=remote_before_validation or {},
            after_validation=remote_after_validation or {},
            d1l_public_key=public_key,
            control=remote_control or {},
        )
        peer_status_ok = bool(
            remote_before_validation
            and remote_before_validation.get("ok") is True
            and remote_after_validation
            and remote_after_validation.get("ok") is True
            and peer_public_key == remote_config["public_key"]
            and peer_after_public_key == remote_config["public_key"]
        )
        rx_dm_delta = remote_flow["deltas"]["rx_dm_total"]
        tx_dm_delta = remote_flow["deltas"]["tx_dm_total"]
        fast_reply_delta = remote_flow["deltas"][
            "local_fast_reply_total"
        ]
        ack_miss_delta = remote_flow["deltas"][
            "tx_dm_ack_miss_total"
        ]
        peer_counter_ok = remote_flow["ok"] is True
    elif listener_mode:
        remote_flow = None
        peer_status_ok = (
            radio_listener_connected(peer_before, peer_port, fingerprint)
            and radio_listener_connected(peer_after, peer_port, fingerprint)
            and peer_before.get("run_id") == peer_after.get("run_id")
            and peer_public_key is not None
            and peer_after_public_key == peer_public_key
        )
        rx_dm_delta = counter_delta(peer_before, peer_after, "rx_dm_total")
        tx_dm_delta = counter_delta(peer_before, peer_after, "tx_dm_total")
        fast_reply_delta = counter_delta(
            peer_before, peer_after, "local_fast_reply_total"
        )
        ack_miss_delta = counter_delta(
            peer_before, peer_after, "tx_dm_ack_miss_total"
        )
        peer_counter_ok = (
            rx_dm_delta == 1
            and tx_dm_delta == 1
            and fast_reply_delta == 1
            and ack_miss_delta == 0
            and listener_sender_matches(peer_after, public_key)
            and get_path(peer_after, "mesh", "last_rx_kind") == "dm"
            and get_path(peer_after, "mesh", "last_tx_kind") == "dm"
            and get_path(peer_before, "mesh", "last_rx_at")
            != get_path(peer_after, "mesh", "last_rx_at")
            and get_path(peer_before, "mesh", "last_tx_at")
            != get_path(peer_after, "mesh", "last_tx_at")
        )
    else:
        remote_flow = None
        peer_status_ok = (
            normalize_port(
                get_path(peer_before, "serial", "active_port")
            )
            == peer_port
            and get_path(
                peer_before, "serial", "meshcore_connected"
            )
            is True
            and get_path(peer_before, "discord", "connected") is True
        ) or (
            normalize_port(
                get_path(peer_after, "serial", "active_port")
            )
            == peer_port
            and get_path(peer_after, "serial", "meshcore_connected")
            is True
            and get_path(peer_after, "discord", "connected") is True
        )
        rx_dm_delta = None
        tx_dm_delta = None
        fast_reply_delta = None
        ack_miss_delta = None
        peer_counter_ok = peer_status_ok
    outbound_ok = bool(
        send_outbound
        and outbound_step
        and outbound_step.get("result", {}).get("ok") is True
        and outbound_packets
        and contains_token(outbound_packets.get("result"), outbound_token)
    )
    inbound_ok = bool(
        latest_messages
        and (
            new_exact_listener_reply(
                baseline_messages_result,
                messages_result,
                fingerprint,
                inbound_text,
            )
            if listener_mode
            else messages_have_inbound_token(
                messages_result, inbound_token, fingerprint
            )
        )
    )
    transaction_correlation = (
        correlated_listener_transaction(
            baseline_messages=baseline_messages_result,
            final_messages=messages_result,
            baseline_packets=baseline_packets_result,
            final_packets=packets_result,
            baseline_route=baseline_route_result,
            final_route=route_result,
            outbound_token=outbound_token,
            fingerprint=fingerprint,
            inbound_text=inbound_text,
        )
        if listener_mode
        else None
    )
    if listener_mode:
        ack_path_ok = bool(
            send_outbound
            and transaction_correlation
            and transaction_correlation.get("ack_path_ok") is True
        )
        direct_route_ok = bool(
            send_outbound
            and transaction_correlation
            and transaction_correlation.get("direct_route_ok") is True
        )
    else:
        ack_path_ok = bool(
            messages_have_acked_tx(
                messages_result, outbound_token, fingerprint
            )
            and packets_have_ack_or_path(packets_result)
        )
        direct_send_step = direct_step
        direct_route_ok = bool(
            send_outbound
            and direct_send_step
            and direct_send_step.get("result", {}).get("ok") is True
            and messages_have_tx_token(
                messages_result, direct_token, fingerprint
            )
            and route_has_direct_path(route_result, fingerprint)
        )
    controlled_peer_observed = (
        peer_status_ok
        if peer_status_requested
        else inbound_ok and ack_path_ok and direct_route_ok
    )
    checks = {
        "d1l_target_identity_continuity": (
            target_identity_continuity_ok
        ),
        "identity_public_key_matches": bool(
            identity_result.get("ok") is True
            and expected_identity
            and identity_fingerprint == expected_identity
        ),
        "controlled_peer_observed": controlled_peer_observed,
        "controlled_peer_status_connected": (
            peer_status_ok and peer_counter_ok
            if peer_status_requested
            else True
        ),
        "controlled_peer_contact_ready": (
            bool(
                listener_mode
                and peer_public_key is not None
                and import_command is not None
                and contact_import_ok(
                    contact_import_result,
                    peer_public_key,
                    fingerprint,
                )
                and contacts_has_exact_peer(
                    contact_after_result,
                    peer_public_key,
                    fingerprint,
                )
            )
            if listener_mode
            else True
        ),
        "outbound_dm": outbound_ok,
        "inbound_dm": inbound_ok,
        "ack_path": ack_path_ok,
        "direct_route": direct_route_ok,
        "health_ready": bool(
            health_result.get("ok") is True
            and health_result.get("board_ready") is True
            and health_result.get("ui_ready") is True
        ),
        "no_public_commands": not any(command_has_public_tx(command) for command in commands),
    }
    if expected_commit is not None:
        checks["protocol_tx_ready_before_rf"] = (
            protocol_tx_ready_for_rf(version_result)
        )
        checks["exact_candidate"] = firmware_identity_matches(
            version_result, expected_commit
        )
    if remote_mode:
        controlled_peer = controlled_peer_report_row(
            remote_config,
            fingerprint,
            local_mode=local_mode,
        )
    else:
        controlled_peer = {
            "fingerprint": fingerprint,
            "evidence_source": (
                "explicit_peer_status"
                if peer_status_requested
                else "d1l_bidirectional_rf"
            ),
            "port": peer_port,
            "status_path": (
                str(peer_status_path)
                if peer_status_path is not None
                else None
            ),
        }
    if listener_mode and not remote_mode:
        controlled_peer["public_key"] = peer_public_key
    report_ok = all(checks.values())
    return {
        "schema": RF_FULL_ACCEPTANCE_SCHEMA,
        "mode": "rf-full-acceptance",
        "hardware_required": True,
        "physical_observed": True,
        "dry_run": False,
        "simulated": False,
        "simulation": False,
        "source_inspection": False,
        "execution_complete": True,
        "closure_eligible": report_ok,
        "dm_rf_tx": bool(
            send_outbound
            and outbound_step
            and (listener_mode or direct_step)
        ),
        "public_rf_tx": False,
        "formats_sd": False,
        "port": port,
        "d1l_target": d1l_target,
        "d1l_target_after": d1l_target_after,
        "target_identity_continuity_ok": (
            target_identity_continuity_ok
        ),
        "baud": baud,
        "controlled_peer": controlled_peer,
        "controlled_peer_adapter": (
            LOCAL_PEER_ADAPTER
            if local_mode
            else REMOTE_PEER_ADAPTER
            if remote_mode
            else RADIO_LISTENER_PROFILE
            if listener_mode
            else "meshcorebot"
        ),
        "target_fingerprint": fingerprint,
        "d1l_public_key": public_key,
        "expected_identity_fingerprint": expected_identity,
        "identity_fingerprint": identity_fingerprint,
        "token": token,
        "expected_firmware_commit": expected_commit,
        "github_actions_run": (
            str(github_run_id) if github_run_id is not None else None
        ),
        "workflow_run_attempt": (
            str(workflow_run_attempt)
            if workflow_run_attempt is not None
            else None
        ),
        "device_build_commit": version_result.get("build_commit"),
        "device_idf_version": version_result.get("idf"),
        "device_release_profile": version_result.get("release_profile"),
        "device_sd_history_mode": version_result.get("sd_history_mode"),
        "firmware_identity_required": expected_commit is not None,
        "firmware_identity_ok": checks.get("exact_candidate"),
        "outbound_token": outbound_token,
        "inbound_token": (
            inbound_text if listener_mode else inbound_token
        ),
        "direct_token": direct_token,
        "discord_command": (
            None
            if remote_mode
            else discord_command(public_key, inbound_token)
        ),
        "public_rf_transmit": False,
        "inbound_seen_at": inbound_seen_at,
        "controlled_peer_before": status_snapshot(peer_before),
        "controlled_peer_after": status_snapshot(peer_after),
        "controlled_peer_before_receipt": peer_before_receipt,
        "controlled_peer_after_receipt": peer_after_receipt,
        "controlled_peer_counter_deltas": {
            "rx_dm_total": rx_dm_delta,
            "tx_dm_total": tx_dm_delta,
            "local_fast_reply_total": fast_reply_delta,
            "tx_dm_ack_miss_total": ack_miss_delta,
        },
        "controlled_peer_remote": (
            {
                "before_validation": remote_before_validation,
                "after_validation": remote_after_validation,
                "flow": remote_flow,
            }
            if remote_mode
            else None
        ),
        "controlled_peer_control": (
            remote_control if remote_mode else None
        ),
        "transaction_correlation": transaction_correlation,
        "controlled_peer_contact_setup": {
            "name": RADIO_LISTENER_CONTACT_NAME,
            "public_key": peer_public_key,
            "fingerprint": fingerprint,
            "import_command": import_command,
            "before": contact_before_result,
            "import_result": contact_import_result,
            "after": contact_after_result,
        },
        "checks": checks,
        "steps": steps,
        "ok": report_ok,
    }


def command_can_retry(command: str) -> bool:
    return not command.strip().lower().startswith("mesh send ")


def send_acceptance_command(ser, command: str, timeout: float, retries: int = 1) -> dict:
    result = send_console_command(ser, command, timeout)
    for _ in range(max(0, retries)):
        if result.get("code") != "TIMEOUT" or not command_can_retry(command):
            break
        time.sleep(0.2)
        ser.reset_input_buffer()
        result = send_console_command(ser, command, timeout)
    return result


def _run_hardware_reserved(
    *,
    port: str,
    baud: int,
    timeout: float,
    wait_sec: float,
    poll_sec: float,
    peer_status_path: Path | None,
    peer_port: str | None,
    fingerprint: str,
    public_key: str,
    token: str,
    send_outbound: bool,
    expected_commit: str,
    github_run_id: str,
    workflow_run_attempt: str,
    peer_capture_dir: Path | None = None,
    remote_peer: dict | None = None,
    local_peer: dict | None = None,
    port_lister: Callable[[], Iterable[object]] | None = None,
    platform_name: str | None = None,
    evidence_bundle: EvidenceBundle,
) -> dict:
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("pyserial is required: python -m pip install pyserial") from exc

    token = validate_safe_token(token)
    root = Path(__file__).resolve().parents[1]
    source_git = git_metadata(root)
    outbound_token = f"{token}_out"
    inbound_token = f"{token}_in"
    direct_token = f"{token}_direct"
    if remote_peer is not None and local_peer is not None:
        raise ValueError(
            "SSH and Pi-local controlled-peer modes are mutually exclusive"
        )
    ssh_config = (
        validate_remote_peer_config(remote_peer)
        if remote_peer is not None
        else None
    )
    local_config = (
        validate_local_peer_config(local_peer)
        if local_peer is not None
        else None
    )
    if local_config is not None:
        require_local_peer_hostname()
    remote_config = local_config or ssh_config
    local_mode = local_config is not None
    remote_mode = remote_config is not None
    normalized_commit = exact_commit(expected_commit)
    if normalized_commit is None:
        raise ValueError("expected_commit must be an exact 40-character hexadecimal SHA")
    if (
        exact_public_key(public_key) is None
        or re.fullmatch(r"[0-9A-F]{16}", str(fingerprint).upper())
        is None
    ):
        raise ValueError(
            "RF acceptance requires exact D1L public key and peer fingerprint"
        )
    if remote_mode and str(fingerprint).upper() != REMOTE_PEER_FINGERPRINT:
        raise ValueError(
            "remote controlled-peer fingerprint must match its exact "
            "pinned public key"
        )
    if (
        not str(github_run_id).isdigit()
        or int(github_run_id) < 1
        or not str(workflow_run_attempt).isdigit()
        or int(workflow_run_attempt) < 1
    ):
        raise ValueError(
            "GitHub run id and run attempt must be positive integers"
        )
    if not (
        exact_commit(source_git.get("commit")) == normalized_commit
        and source_git.get("dirty") is False
        and source_git.get("dirty_entries") == []
    ):
        raise ValueError(
            "RF acceptance must run from the exact clean candidate source"
        )
    port, peer_port = enforce_port_policy(port, peer_port)
    if port_lister is None:
        try:
            from serial.tools import list_ports
        except ImportError as exc:
            raise SystemExit(
                "pyserial list_ports is required to identify the Core D1L target"
            ) from exc

        def default_port_lister() -> Iterable[object]:
            return list_ports.comports(include_links=True)

        port_lister = default_port_lister
    d1l_target = resolve_core_target(
        port,
        port_lister=port_lister,
        platform_name=platform_name,
    )
    port = d1l_target["requested_path"]
    if remote_mode and (
        peer_status_path is not None or peer_port is not None
    ):
        raise ValueError(
            "pinned Pi and serial controlled-peer modes are mutually exclusive"
        )
    if (
        not remote_mode
        and (peer_status_path is None or peer_port is None)
    ):
        raise ValueError(
            "hardware RF acceptance requires an explicitly assigned "
            "controlled-peer status path and port"
        )
    if (
        not remote_mode
        and
        peer_port == RADIO_LISTENER_PORT
        and peer_status_path.resolve()
        != RADIO_LISTENER_STATUS_PATH.resolve()
    ):
        raise ValueError(
            "COM15 acceptance requires the exact OpenClaw radio listener "
            f"status path {RADIO_LISTENER_STATUS_PATH}"
        )
    steps: list[dict] = []
    peer_before = None
    peer_before_receipt = None
    peer_after_receipt = None
    remote_before_validation = None
    remote_after_validation = None
    remote_control = None
    inbound_seen_at = None
    capture_dir = (
        peer_capture_dir
        or root
        / "artifacts"
        / "hardware"
        / safe_slug(port)
        / "rf-peer"
    )
    safe_token = re.sub(r"[^A-Za-z0-9_.-]", "_", token)
    before_capture = capture_dir / f"{safe_token}_peer_before.json"
    after_capture = capture_dir / f"{safe_token}_peer_after.json"
    request_capture = capture_dir / f"{safe_token}_peer_request.jsonl"
    response_capture = capture_dir / f"{safe_token}_peer_response.jsonl"
    before_reservation = evidence_bundle.reserve(
        "peer_before",
        before_capture,
        label="controlled-peer before status",
    )
    after_reservation = evidence_bundle.reserve(
        "peer_after",
        after_capture,
        label="controlled-peer after status",
    )
    if remote_mode:
        request_reservation = evidence_bundle.reserve(
            "peer_request",
            request_capture,
            label="controlled-peer UDS request",
        )
        response_reservation = evidence_bundle.reserve(
            "peer_response",
            response_capture,
            label="controlled-peer UDS response",
        )
    else:
        request_reservation = None
        response_reservation = None
    evidence_bundle.mark_external_io_started()

    def run_command(ser, command: str, command_timeout: float | None = None) -> dict:
        result = send_acceptance_command(ser, command, command_timeout or timeout)
        steps.append({"command": command, "result": result})
        return result

    with open_d1l_serial(serial, port=port, baudrate=baud, timeout=timeout) as ser:
        time.sleep(1.0)
        ser.reset_input_buffer()
        version = run_command(ser, "version")
        if not (
            firmware_identity_matches(version, normalized_commit)
            and version.get("idf") == "v5.5.4"
            and version.get("release_profile") == "core_1_0"
            and version.get("sd_history_mode") == "disabled"
            and protocol_tx_ready_for_rf(version)
        ):
            d1l_target_after = resolve_core_target(
                port,
                port_lister=port_lister,
                platform_name=platform_name,
            )
            target_identity_continuity_ok = d1l_target_continuity_ok(
                port=port,
                before=d1l_target,
                after=d1l_target_after,
            )
            if not target_identity_continuity_ok:
                raise ValueError(
                    "D1L serial target identity changed during RF preflight"
                )
            return {
                "schema": RF_FULL_ACCEPTANCE_SCHEMA,
                "mode": "rf-full-acceptance",
                "hardware_required": True,
                "physical_observed": True,
                "dry_run": False,
                "simulated": False,
                "simulation": False,
                "source_inspection": False,
                "execution_complete": False,
                "closure_eligible": False,
                "dm_rf_tx": False,
                "public_rf_tx": False,
                "formats_sd": False,
                "port": port,
                "d1l_target": d1l_target,
                "d1l_target_after": d1l_target_after,
                "target_identity_continuity_ok": (
                    target_identity_continuity_ok
                ),
                "baud": baud,
                "expected_firmware_commit": normalized_commit,
                "github_actions_run": str(github_run_id),
                "workflow_run_attempt": str(workflow_run_attempt),
                "device_build_commit": version.get("build_commit"),
                "device_release_profile": version.get("release_profile"),
                "device_sd_history_mode": version.get("sd_history_mode"),
                "device_protocol_tx_ready": get_path(
                    version, "time", "protocol_tx_ready"
                ),
                "device_protocol_tx_block": get_path(
                    version, "time", "protocol_tx_block"
                ),
                "firmware_identity_required": True,
                "firmware_identity_ok": False,
                "controlled_peer": (
                    controlled_peer_report_row(
                        remote_config,
                        fingerprint,
                        local_mode=local_mode,
                    )
                    if remote_mode
                    else {
                        "fingerprint": fingerprint,
                        "evidence_source": "explicit_peer_status",
                        "port": peer_port,
                        "status_path": (
                            str(peer_status_path)
                            if peer_status_path is not None
                            else None
                        ),
                    }
                ),
                "checks": {
                    "d1l_target_identity_continuity": (
                        target_identity_continuity_ok
                    ),
                    "exact_candidate": False,
                    "protocol_tx_ready_before_rf": (
                        protocol_tx_ready_for_rf(version)
                    ),
                    "no_public_commands": True,
                },
                "steps": steps,
                "ok": False,
            }
        if local_mode:
            (
                peer_before,
                peer_before_receipt,
                remote_before_validation,
            ) = capture_local_peer_status(
                remote_config,
                before_capture,
                root,
                reservation=before_reservation,
                evidence_bundle=evidence_bundle,
            )
        elif remote_mode:
            (
                peer_before,
                peer_before_receipt,
                remote_before_validation,
            ) = capture_remote_peer_status(
                remote_config,
                before_capture,
                root,
                reservation=before_reservation,
                evidence_bundle=evidence_bundle,
            )
        else:
            peer_before, peer_before_receipt = capture_peer_status(
                peer_status_path,
                before_capture,
                root,
                reservation=before_reservation,
            )
        listener_mode = remote_mode or peer_port == RADIO_LISTENER_PORT
        if (
            not remote_mode
            and listener_mode
            and not radio_listener_connected(
                peer_before, peer_port, fingerprint
            )
        ):
            raise ValueError(
                "COM15 OpenClaw listener status/public-key identity is not ready"
            )
        run_command(ser, "identity status")
        run_command(ser, "contacts")
        if listener_mode:
            peer_public_key = exact_public_key(
                get_path(peer_before, "serial", "public_key")
            )
            if peer_public_key is None:
                raise ValueError(
                    "COM15 listener status has no exact 64-hex public key"
                )
            import_result = run_command(
                ser, contact_import_command(peer_public_key)
            )
            contacts_after = run_command(ser, "contacts")
            if not (
                contact_import_ok(
                    import_result, peer_public_key, fingerprint
                )
                and contacts_has_exact_peer(
                    contacts_after, peer_public_key, fingerprint
                )
            ):
                raise ValueError(
                    "Controlled COM15 peer contact import/verification failed"
                )
        baseline_messages = run_command(
            ser, f"messages dm {fingerprint}"
        )
        run_command(ser, "packets")
        run_command(ser, f"routes trace {fingerprint}")
        if send_outbound:
            outbound_text = (
                f"core acceptance test {outbound_token}"
                if listener_mode
                else outbound_token
            )
            run_command(
                ser,
                f"mesh send dm {fingerprint} {outbound_text}",
                max(timeout, 8.0),
            )
            time.sleep(2.0)
            run_command(ser, f"packets search {outbound_token}")
        if remote_mode:
            send_peer_dm = (
                send_local_peer_dm
                if local_mode
                else send_remote_peer_dm
            )
            remote_control = send_peer_dm(
                remote_config,
                d1l_public_key=public_key,
                token=inbound_token,
                request_capture_path=request_capture,
                response_capture_path=response_capture,
                root=root,
                request_reservation=request_reservation,
                response_reservation=response_reservation,
                evidence_bundle=evidence_bundle,
            )
        deadline = time.time() + wait_sec
        while time.time() < deadline:
            messages = run_command(ser, f"messages dm {fingerprint}")
            inbound_found = (
                new_exact_listener_reply(
                    baseline_messages,
                    messages,
                    fingerprint,
                    inbound_token
                    if remote_mode
                    else RADIO_LISTENER_REPLY,
                )
                if listener_mode
                else messages_have_inbound_token(
                    messages, inbound_token, fingerprint
                )
            )
            if inbound_found:
                inbound_seen_at = datetime.now(timezone.utc).isoformat()
                break
            time.sleep(max(0.25, poll_sec))
        if send_outbound:
            run_command(ser, "packets")
            run_command(ser, f"routes trace {fingerprint}")
            if not listener_mode:
                run_command(
                    ser,
                    f"mesh send dm {fingerprint} {direct_token}",
                    max(timeout, 8.0),
                )
                time.sleep(2.0)
                run_command(ser, f"packets search {direct_token}")
            run_command(ser, f"messages dm {fingerprint}")
        run_command(ser, "packets")
        run_command(ser, f"routes trace {fingerprint}")
        run_command(ser, "health")

    d1l_target_after = resolve_core_target(
        port,
        port_lister=port_lister,
        platform_name=platform_name,
    )
    if not d1l_target_continuity_ok(
        port=port,
        before=d1l_target,
        after=d1l_target_after,
    ):
        raise ValueError(
            "D1L serial target identity changed during RF acceptance"
        )

    if not remote_mode and peer_port == RADIO_LISTENER_PORT:
        status_deadline = time.time() + max(10.0, wait_sec)
        while time.time() < status_deadline:
            candidate = read_json(peer_status_path)
            if (
                counter_delta(peer_before, candidate, "rx_dm_total") == 1
                and counter_delta(peer_before, candidate, "tx_dm_total") == 1
                and counter_delta(
                    peer_before, candidate, "local_fast_reply_total"
                )
                == 1
            ):
                break
            time.sleep(max(0.25, poll_sec))
    if local_mode:
        (
            peer_after,
            peer_after_receipt,
            remote_after_validation,
        ) = capture_local_peer_status(
            remote_config,
            after_capture,
            root,
            reservation=after_reservation,
            evidence_bundle=evidence_bundle,
        )
    elif remote_mode:
        (
            peer_after,
            peer_after_receipt,
            remote_after_validation,
        ) = capture_remote_peer_status(
            remote_config,
            after_capture,
            root,
            reservation=after_reservation,
            evidence_bundle=evidence_bundle,
        )
    else:
        peer_after, peer_after_receipt = capture_peer_status(
            peer_status_path,
            after_capture,
            root,
            reservation=after_reservation,
        )
    return build_report(
        port=port,
        d1l_target=d1l_target,
        d1l_target_after=d1l_target_after,
        baud=baud,
        peer_status_path=peer_status_path,
        peer_port=peer_port,
        fingerprint=fingerprint,
        public_key=public_key,
        token=token,
        send_outbound=send_outbound,
        steps=steps,
        peer_before=peer_before,
        peer_after=peer_after,
        inbound_seen_at=inbound_seen_at,
        expected_commit=normalized_commit,
        peer_before_receipt=peer_before_receipt,
        peer_after_receipt=peer_after_receipt,
        github_run_id=str(github_run_id),
        workflow_run_attempt=str(workflow_run_attempt),
        remote_peer=ssh_config,
        local_peer=local_config,
        remote_before_validation=remote_before_validation,
        remote_after_validation=remote_after_validation,
        remote_control=remote_control,
    )


def run_hardware(
    *,
    evidence_bundle: EvidenceBundle | None = None,
    **kwargs,
) -> dict:
    root = Path(__file__).resolve().parents[1]
    if evidence_bundle is not None:
        if evidence_bundle.root != root.resolve(strict=True):
            raise ValueError(
                "RF evidence bundle is bound to a different repository"
            )
        return _run_hardware_reserved(
            evidence_bundle=evidence_bundle,
            **kwargs,
        )
    with EvidenceBundle(root) as owned_bundle:
        return _run_hardware_reserved(
            evidence_bundle=owned_bundle,
            **kwargs,
        )


def default_out_path(report: dict) -> Path:
    if report.get("mode") == "dry-run-rf-full-acceptance":
        return Path("artifacts") / "smoke" / f"d1l-rf-full-acceptance-dry-run-{utc_stamp()}.json"
    port = safe_slug(str(report.get("port") or "unknown"))
    token = str(report.get("token") or utc_stamp()).replace(":", "_")
    return Path("artifacts") / "hardware" / port / f"rf_full_acceptance_{token}.json"


def planned_report_path(
    *,
    root: Path,
    out_path: Path | None,
    dry_run: bool,
    port: str,
    token: str,
) -> Path:
    if out_path is not None:
        candidate = out_path if out_path.is_absolute() else root / out_path
    elif dry_run:
        candidate = (
            root
            / "artifacts"
            / "smoke"
            / f"d1l-rf-full-acceptance-dry-run-{utc_stamp()}.json"
        )
    else:
        safe_token = validate_safe_token(token)
        candidate = (
            root
            / "artifacts"
            / "hardware"
            / safe_slug(port)
            / f"rf_full_acceptance_{safe_token}.json"
        )
    root_resolved = root.resolve(strict=True)
    lexical = Path(os.path.abspath(os.fspath(candidate)))
    try:
        lexical.relative_to(root_resolved)
    except ValueError as exc:
        raise ValueError(
            "RF acceptance report must stay inside the repository"
        ) from exc
    return lexical


def write_report(
    report: dict,
    out_path: Path | None,
    *,
    reservation: EvidenceReservation | None = None,
) -> Path:
    root = Path(__file__).resolve().parents[1]
    path = out_path or default_out_path(report)
    if not path.is_absolute():
        path = root / path
    stamp_report(report, root)
    raw = (json.dumps(report, indent=2) + "\n").encode("utf-8")
    owned_bundle: EvidenceBundle | None = None
    if reservation is None:
        owned_bundle = EvidenceBundle(root)
        reservation = owned_bundle.reserve(
            "report",
            path,
            label="RF acceptance report",
        )
    elif reservation.path != path.resolve(strict=False):
        raise ValueError(
            "RF acceptance report path does not match its reservation"
        )
    try:
        reservation.write_bytes(raw)
        return reservation.path
    except Exception as exc:
        if owned_bundle is not None:
            owned_bundle.mark_incomplete(exc)
        raise
    finally:
        if owned_bundle is not None:
            owned_bundle.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--wait-sec", type=float, default=90.0)
    parser.add_argument("--poll-sec", type=float, default=3.0)
    parser.add_argument(
        "--fingerprint", default=os.environ.get("D1L_DM_TARGET")
    )
    parser.add_argument("--d1l-public-key", default=os.environ.get("D1L_PUBLIC_KEY", DEFAULT_D1L_PUBLIC_KEY))
    parser.add_argument(
        "--peer-status",
        "--bot-status",
        dest="peer_status",
        default=os.environ.get("MESH_PEER_STATUS_PATH"),
    )
    parser.add_argument(
        "--peer-port",
        "--bot-port",
        dest="peer_port",
        default=os.environ.get("MESH_PEER_PORT"),
    )
    parser.add_argument(
        "--peer-ssh-host",
        default=os.environ.get("MESH_PEER_SSH_HOST"),
    )
    parser.add_argument(
        "--peer-local",
        action="store_true",
        help="use the pinned neopi5 status file and Unix socket locally",
    )
    parser.add_argument(
        "--peer-remote-status",
        default=os.environ.get("MESH_PEER_REMOTE_STATUS_PATH"),
    )
    parser.add_argument(
        "--peer-control-socket",
        default=os.environ.get("MESH_PEER_CONTROL_SOCKET"),
    )
    parser.add_argument(
        "--peer-device",
        default=os.environ.get("MESH_PEER_DEVICE"),
    )
    parser.add_argument(
        "--peer-public-key",
        default=os.environ.get("MESH_PEER_PUBLIC_KEY"),
    )
    parser.add_argument(
        "--peer-max-status-age-sec",
        type=float,
        default=None,
    )
    parser.add_argument("--token", default=None)
    parser.add_argument("--commit", default=None)
    parser.add_argument("--github-run-id")
    parser.add_argument("--github-run-attempt")
    parser.add_argument("--skip-outbound", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--out")
    args = parser.parse_args()

    try:
        token = validate_safe_token(
            args.token or default_token(args.commit)
        )
    except ValueError as exc:
        parser.error(str(exc))
    remote_options = (
        args.peer_remote_status,
        args.peer_control_socket,
        args.peer_device,
        args.peer_public_key,
        args.peer_max_status_age_sec,
    )
    if args.peer_local and args.peer_ssh_host:
        parser.error(
            "--peer-local and --peer-ssh-host are mutually exclusive"
        )
    pinned_peer_requested = bool(args.peer_local or args.peer_ssh_host)
    if pinned_peer_requested:
        if args.peer_status or args.peer_port:
            parser.error(
                "Pinned Pi peer mode cannot be combined with "
                "--peer-status/--peer-port"
            )
        common_config = {
            "status_path": (
                args.peer_remote_status or REMOTE_PEER_STATUS_PATH
            ),
            "control_socket": (
                args.peer_control_socket or REMOTE_PEER_CONTROL_SOCKET
            ),
            "device": args.peer_device or REMOTE_PEER_DEVICE,
            "public_key": (
                args.peer_public_key or REMOTE_PEER_PUBLIC_KEY
            ),
            "max_status_age_sec": (
                args.peer_max_status_age_sec
                if args.peer_max_status_age_sec is not None
                else REMOTE_PEER_MAX_STATUS_AGE_SEC
            ),
        }
        try:
            if args.peer_local:
                require_local_peer_hostname()
                local_config = local_peer_config(**common_config)
                remote_config = None
            else:
                remote_config = remote_peer_config(
                    ssh_host=args.peer_ssh_host,
                    **common_config,
                )
                local_config = None
        except (RemotePeerError, ValueError) as exc:
            parser.error(str(exc))
    else:
        remote_config = None
        local_config = None
        if any(value is not None for value in remote_options):
            parser.error(
                "Pinned peer path/device options require "
                "--peer-local or --peer-ssh-host"
            )
    pinned_config = local_config or remote_config
    fingerprint = str(
        args.fingerprint
        or (
            REMOTE_PEER_FINGERPRINT
            if pinned_config is not None
            else DEFAULT_TARGET_FINGERPRINT
        )
    ).upper()
    if (
        pinned_config is not None
        and fingerprint != REMOTE_PEER_FINGERPRINT
    ):
        parser.error(
            "Pinned peer fingerprint must match the pinned public key"
        )
    if bool(args.peer_status) != bool(args.peer_port):
        parser.error("--peer-status and --peer-port must be supplied together")
    peer_status_path = Path(args.peer_status) if args.peer_status else None
    if args.port:
        try:
            port, peer_port = enforce_port_policy(
                args.port,
                None if pinned_config is not None else args.peer_port,
            )
        except ValueError as exc:
            parser.error(str(exc))
    elif args.dry_run:
        port = default_d1l_target()
        if args.peer_port and pinned_config is None:
            try:
                _, peer_port = enforce_port_policy(port, args.peer_port)
            except ValueError as exc:
                parser.error(str(exc))
        else:
            peer_port = None
    else:
        parser.error("No D1L port supplied. Set D1L_PORT or pass --port.")
    normalized_commit = exact_commit(args.commit)
    if not args.dry_run and normalized_commit is None:
        parser.error(
            "--commit is required for hardware RF acceptance and must be an "
            "exact 40-character hexadecimal SHA"
        )
    if not args.dry_run and (
        not str(args.github_run_id or "").isdigit()
        or not str(args.github_run_attempt or "").isdigit()
        or int(args.github_run_attempt) < 1
    ):
        parser.error(
            "hardware RF acceptance requires positive --github-run-id "
            "and --github-run-attempt"
        )
    if (
        not args.dry_run
        and pinned_config is None
        and (peer_status_path is None or peer_port is None)
    ):
        parser.error(
            "Hardware RF acceptance requires --peer-status and --peer-port for "
            "the explicitly assigned controlled peer"
        )
    root = Path(__file__).resolve().parents[1]
    planned_out = planned_report_path(
        root=root,
        out_path=Path(args.out) if args.out else None,
        dry_run=args.dry_run,
        port=port,
        token=token,
    )
    with EvidenceBundle(root) as evidence_bundle:
        report_reservation = evidence_bundle.reserve(
            "report",
            planned_out,
            label="RF acceptance report",
        )
        if args.dry_run:
            report = dry_run_report(
                port=port,
                peer_status_path=peer_status_path,
                peer_port=peer_port,
                fingerprint=fingerprint,
                public_key=args.d1l_public_key,
                token=token,
                send_outbound=not args.skip_outbound,
                expected_commit=args.commit,
                github_run_id=args.github_run_id,
                workflow_run_attempt=args.github_run_attempt,
                remote_peer=remote_config,
                local_peer=local_config,
            )
        else:
            if peer_status_path is not None and not peer_status_path.exists():
                parser.error(
                    f"Controlled-peer status file not found: {peer_status_path}"
                )
            report = run_hardware(
                port=port,
                baud=args.baud,
                timeout=args.timeout,
                wait_sec=args.wait_sec,
                poll_sec=args.poll_sec,
                peer_status_path=peer_status_path,
                peer_port=peer_port,
                fingerprint=fingerprint,
                public_key=args.d1l_public_key,
                token=token,
                send_outbound=not args.skip_outbound,
                expected_commit=normalized_commit,
                github_run_id=str(args.github_run_id),
                workflow_run_attempt=str(args.github_run_attempt),
                remote_peer=remote_config,
                local_peer=local_config,
                evidence_bundle=evidence_bundle,
            )

        written = write_report(
            report,
            planned_out,
            reservation=report_reservation,
        )
    print(json.dumps({"ok": report["ok"], "out": str(written), "mode": report["mode"]}, indent=2))
    if report.get("mode") == "dry-run-rf-full-acceptance":
        if report.get("discord_command"):
            print(
                "Controlled-peer inbound command: "
                f"{report['discord_command']}"
            )
        elif report.get("controlled_peer_control_plan"):
            local_plan = (
                get_path(report, "controlled_peer", "access_mode")
                == "local"
            )
            print(
                "Controlled-peer inbound plan: "
                + (
                    "use the pinned Pi-local Unix socket for one exact "
                    if local_plan
                    else "SSH to the pinned Pi peer and send one exact "
                )
                + "radio.send_dm request"
            )
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
