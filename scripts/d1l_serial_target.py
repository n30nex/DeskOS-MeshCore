#!/usr/bin/env python3
"""Fail-closed cross-platform identity contract for the Core D1L serial target.

This module only resolves and records a serial target. It deliberately never
opens a serial port. After validation, callers may open only ``requested_path``:
the stable by-id path on POSIX or canonical COM12 on Windows. ``resolved_tty``
is observational evidence and must never become the POSIX open target.
"""

from __future__ import annotations

import hashlib
import json
import os
import posixpath
import re
import socket
import unicodedata
from collections.abc import Callable, Iterable
from typing import Any


WINDOWS_D1L_TARGET = "COM12"
POSIX_D1L_TARGET = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
EXPECTED_VID = 0x1A86
EXPECTED_PID = 0x7523
FORBIDDEN_WINDOWS_TARGETS = frozenset(
    {"COM8", "COM11", "COM16", "COM29"}
)

SNAPSHOT_SCHEMA = 1
SNAPSHOT_KIND = "d1l_serial_target_snapshot"
WINDOWS_TARGET_KIND = "windows_com"
POSIX_TARGET_KIND = "posix_by_id"

_SNAPSHOT_KEYS = frozenset(
    {
        "schema",
        "kind",
        "target_kind",
        "requested_path",
        "resolved_tty",
        "vid",
        "pid",
        "serial_number",
        "hwid",
        "location",
        "hostname",
        "access",
        "stable_identity_sha256",
    }
)
_ACCESS_KEYS = frozenset({"read", "write"})
_POSIX_TTY_RE = re.compile(r"/dev/ttyUSB[0-9]+\Z")
_SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
_SAFE_SLUG_RE = re.compile(r"[^a-z0-9]+")


def _platform_kind(platform_name: str | None) -> str:
    value = os.name if platform_name is None else platform_name
    if not isinstance(value, str):
        raise ValueError("platform_name must be text")
    normalized = value.strip().lower()
    if normalized in {"nt", "win32", "windows"}:
        return WINDOWS_TARGET_KIND
    if normalized in {"posix", "linux", "darwin"}:
        return POSIX_TARGET_KIND
    raise ValueError(f"unsupported serial-target platform: {value!r}")


def _normalize_windows_target(value: object) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError("requested_target must be non-empty text")
    if any(ord(char) < 0x20 or ord(char) == 0x7F for char in value):
        raise ValueError("requested_target contains a control character")
    normalized = value.strip()
    if normalized.startswith("\\\\.\\"):
        normalized = normalized[4:]
    normalized = normalized.upper()
    if normalized in FORBIDDEN_WINDOWS_TARGETS:
        raise ValueError(f"forbidden Windows D1L target: {normalized}")
    if normalized != WINDOWS_D1L_TARGET:
        raise ValueError(
            f"Windows D1L target must resolve to {WINDOWS_D1L_TARGET}"
        )
    return WINDOWS_D1L_TARGET


def _row_value(row: object, field: str) -> Any:
    if isinstance(row, dict):
        return row.get(field)
    return getattr(row, field, None)


def _required_text(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or value != value.strip():
        raise ValueError(f"{label} must be non-empty canonical text")
    if any(ord(char) < 0x20 or ord(char) == 0x7F for char in value):
        raise ValueError(f"{label} contains a control character")
    return value


def _optional_text(value: object, label: str) -> str | None:
    if value is None or value == "":
        return None
    return _required_text(value, label)


def _exact_usb_id(value: object, expected: int, label: str) -> int:
    if type(value) is not int or value != expected:
        raise ValueError(
            f"D1L {label} must be 0x{expected:04X}; got {value!r}"
        )
    return value


def _canonical_json(value: object) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")


def _identity_projection(snapshot: dict[str, Any]) -> dict[str, Any]:
    """Return only fields that must survive a POSIX tty-number change."""

    return {
        "schema": SNAPSHOT_SCHEMA,
        "kind": SNAPSHOT_KIND,
        "target_kind": snapshot["target_kind"],
        "requested_path": snapshot["requested_path"],
        "vid": snapshot["vid"],
        "pid": snapshot["pid"],
        "serial_number": snapshot["serial_number"],
        "hwid": snapshot["hwid"],
        "location": snapshot["location"],
        "hostname": snapshot["hostname"],
    }


def _stable_identity(snapshot: dict[str, Any]) -> str:
    return hashlib.sha256(
        _canonical_json(_identity_projection(snapshot))
    ).hexdigest()


def _merge_matching_rows(
    rows: Iterable[object],
    *,
    target_kind: str,
    resolved_tty: str,
    realpath: Callable[[str], str],
) -> dict[str, Any]:
    matches: list[dict[str, Any]] = []
    seen_canonical_paths: set[str] = set()
    merged: dict[str, Any] = {
        "vid": None,
        "pid": None,
        "serial_number": None,
        "hwid": None,
        "location": None,
    }

    try:
        listed_rows = list(rows)
    except Exception as exc:
        raise ValueError("serial target enumeration failed") from exc

    for row in listed_rows:
        device = _row_value(row, "device")
        if not isinstance(device, str) or not device:
            continue
        if target_kind == WINDOWS_TARGET_KIND:
            canonical = device.upper()
            matches_target = canonical == WINDOWS_D1L_TARGET
        else:
            try:
                canonical = realpath(device)
            except Exception as exc:
                raise ValueError(
                    f"could not canonicalize listed serial path: {device!r}"
                ) from exc
            if not isinstance(canonical, str):
                raise ValueError("realpath hook returned a non-text path")
            canonical = posixpath.normpath(canonical)
            matches_target = canonical == resolved_tty
        if not matches_target:
            continue

        record = {
            "canonical_path": canonical,
            "vid": _row_value(row, "vid"),
            "pid": _row_value(row, "pid"),
            "serial_number": _optional_text(
                _row_value(row, "serial_number"),
                "serial_number",
            ),
            "hwid": _optional_text(_row_value(row, "hwid"), "hwid"),
            "location": _optional_text(
                _row_value(row, "location"),
                "location",
            ),
        }
        matches.append(record)
        seen_canonical_paths.add(canonical)

    if not matches:
        raise ValueError("the requested D1L target is not present")
    if len(seen_canonical_paths) != 1:
        raise ValueError("ambiguous D1L target paths were enumerated")

    for field in ("vid", "pid", "serial_number", "hwid", "location"):
        values = {
            record[field]
            for record in matches
            if record[field] not in (None, "")
        }
        if len(values) > 1:
            raise ValueError(
                f"ambiguous D1L {field} across alias/canonical rows"
            )
        if values:
            merged[field] = next(iter(values))

    # Alias and canonical pyserial entries resolve to one device and are
    # intentionally coalesced above. Conflicting metadata remains ambiguous.
    return merged


def resolve_target(
    requested_target: str,
    *,
    port_lister: Callable[[], Iterable[object]],
    platform_name: str | None = None,
    exists: Callable[[str], bool] = os.path.exists,
    is_symlink: Callable[[str], bool] = os.path.islink,
    realpath: Callable[[str], str] = os.path.realpath,
    access: Callable[[str, int], bool] = os.access,
    hostname: Callable[[], str] = socket.gethostname,
) -> dict[str, Any]:
    """Resolve one exact D1L endpoint into a validated identity snapshot.

    All system interactions are injectable for deterministic tests and remote
    execution. The resolver enumerates metadata but never opens the endpoint.
    A caller opens ``requested_path`` only; ``resolved_tty`` is evidence, not
    an authorized substitute for the stable POSIX by-id endpoint.
    """

    target_kind = _platform_kind(platform_name)
    requested = (
        _normalize_windows_target(requested_target)
        if target_kind == WINDOWS_TARGET_KIND
        else _required_text(requested_target, "requested_target")
    )

    if target_kind == WINDOWS_TARGET_KIND:
        resolved_tty = WINDOWS_D1L_TARGET
        access_snapshot: dict[str, bool | None] = {
            "read": None,
            "write": None,
        }
    else:
        if requested != POSIX_D1L_TARGET:
            raise ValueError(
                f"POSIX D1L target must be exactly {POSIX_D1L_TARGET}"
            )
        try:
            link_exists = exists(requested)
            link_is_symlink = is_symlink(requested)
            resolved = realpath(requested)
        except Exception as exc:
            raise ValueError("POSIX D1L target inspection failed") from exc
        if link_exists is not True:
            raise ValueError("POSIX D1L by-id target is missing or dangling")
        if link_is_symlink is not True:
            raise ValueError("POSIX D1L by-id target must be a symlink")
        if not isinstance(resolved, str):
            raise ValueError("realpath hook returned a non-text path")
        if resolved != posixpath.normpath(resolved):
            raise ValueError("resolved POSIX D1L tty is not canonical")
        if not _POSIX_TTY_RE.fullmatch(resolved):
            raise ValueError(
                "POSIX D1L by-id target must resolve to /dev/ttyUSB<number>"
            )
        try:
            resolved_exists = exists(resolved)
            read_access = access(resolved, os.R_OK)
            write_access = access(resolved, os.W_OK)
        except Exception as exc:
            raise ValueError(
                "POSIX D1L target access inspection failed"
            ) from exc
        if resolved_exists is not True:
            raise ValueError("resolved POSIX D1L tty is missing")
        if read_access is not True or write_access is not True:
            raise ValueError("resolved POSIX D1L tty is not read/write accessible")
        resolved_tty = resolved
        access_snapshot = {
            "read": True,
            "write": True,
        }

    try:
        rows = port_lister()
    except Exception as exc:
        raise ValueError("serial target enumeration failed") from exc
    metadata = _merge_matching_rows(
        rows,
        target_kind=target_kind,
        resolved_tty=resolved_tty,
        realpath=realpath,
    )
    vid = _exact_usb_id(metadata["vid"], EXPECTED_VID, "VID")
    pid = _exact_usb_id(metadata["pid"], EXPECTED_PID, "PID")
    serial_number = metadata["serial_number"]
    hwid = metadata["hwid"]
    location = metadata["location"]
    if all(value is None for value in (serial_number, hwid, location)):
        raise ValueError("D1L target lacks stable hardware identity metadata")

    try:
        host = hostname()
    except Exception as exc:
        raise ValueError("hostname lookup failed") from exc
    host = _required_text(host, "hostname")

    snapshot: dict[str, Any] = {
        "schema": SNAPSHOT_SCHEMA,
        "kind": SNAPSHOT_KIND,
        "target_kind": target_kind,
        "requested_path": requested,
        "resolved_tty": resolved_tty,
        "vid": vid,
        "pid": pid,
        "serial_number": serial_number,
        "hwid": hwid,
        "location": location,
        "hostname": host,
        "access": access_snapshot,
        "stable_identity_sha256": "",
    }
    snapshot["stable_identity_sha256"] = _stable_identity(snapshot)
    validate_snapshot(snapshot, requested)
    return snapshot


def validate_snapshot(snapshot: object, expected_target: str) -> bool:
    """Strictly validate an untrusted snapshot against an exact target."""

    if type(snapshot) is not dict:
        raise ValueError("D1L serial target snapshot must be an object")
    if set(snapshot) != _SNAPSHOT_KEYS:
        raise ValueError("D1L serial target snapshot keys are not exact")
    if (
        type(snapshot.get("schema")) is not int
        or snapshot.get("schema") != SNAPSHOT_SCHEMA
        or snapshot.get("kind") != SNAPSHOT_KIND
    ):
        raise ValueError("D1L serial target snapshot schema/kind is invalid")

    if expected_target == WINDOWS_D1L_TARGET:
        expected_kind = WINDOWS_TARGET_KIND
    elif expected_target == POSIX_D1L_TARGET:
        expected_kind = POSIX_TARGET_KIND
    else:
        raise ValueError("expected_target is not an authorized D1L target")

    if (
        snapshot.get("target_kind") != expected_kind
        or snapshot.get("requested_path") != expected_target
    ):
        raise ValueError("D1L serial target snapshot target binding is invalid")

    resolved_tty = snapshot.get("resolved_tty")
    if expected_kind == WINDOWS_TARGET_KIND:
        if resolved_tty != WINDOWS_D1L_TARGET:
            raise ValueError("Windows D1L resolved target is invalid")
    elif (
        not isinstance(resolved_tty, str)
        or not _POSIX_TTY_RE.fullmatch(resolved_tty)
    ):
        raise ValueError("POSIX D1L resolved tty is invalid")

    _exact_usb_id(snapshot.get("vid"), EXPECTED_VID, "VID")
    _exact_usb_id(snapshot.get("pid"), EXPECTED_PID, "PID")
    for field in ("serial_number", "hwid", "location"):
        _optional_text(snapshot.get(field), field)
    if all(
        snapshot.get(field) is None
        for field in ("serial_number", "hwid", "location")
    ):
        raise ValueError("D1L target lacks stable hardware identity metadata")
    _required_text(snapshot.get("hostname"), "hostname")

    access_snapshot = snapshot.get("access")
    if (
        type(access_snapshot) is not dict
        or set(access_snapshot) != _ACCESS_KEYS
    ):
        raise ValueError("D1L serial target access snapshot is invalid")
    if expected_kind == WINDOWS_TARGET_KIND:
        if access_snapshot != {"read": None, "write": None}:
            raise ValueError("Windows access snapshot must be non-filesystem")
    elif access_snapshot != {"read": True, "write": True}:
        raise ValueError("POSIX target must be read/write accessible")

    identity = snapshot.get("stable_identity_sha256")
    if (
        not isinstance(identity, str)
        or not _SHA256_RE.fullmatch(identity)
        or identity != _stable_identity(snapshot)
    ):
        raise ValueError("D1L stable identity digest is invalid")
    return True


def safe_slug(value: object) -> str:
    """Return a deterministic, traversal-safe artifact component."""

    if not isinstance(value, str) or not value:
        raise ValueError("slug source must be non-empty text")
    normalized = unicodedata.normalize("NFKD", value)
    ascii_value = normalized.encode("ascii", "ignore").decode("ascii").lower()
    slug = _SAFE_SLUG_RE.sub("-", ascii_value).strip("-")
    if not slug:
        slug = "target-" + hashlib.sha256(
            value.encode("utf-8")
        ).hexdigest()[:12]
    if len(slug) > 80:
        digest = hashlib.sha256(value.encode("utf-8")).hexdigest()[:12]
        slug = f"{slug[:67].rstrip('-')}-{digest}"
    if (
        not slug
        or slug in {".", ".."}
        or "/" in slug
        or "\\" in slug
        or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", slug)
    ):
        raise ValueError("could not derive a safe slug")
    return slug
