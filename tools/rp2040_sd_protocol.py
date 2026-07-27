#!/usr/bin/env python3
"""Reference line-protocol simulator for the D1L RP2040 SD bridge."""

from __future__ import annotations

import argparse
import base64
import re
import zlib
from dataclasses import dataclass, field, replace


STATUS_REQUEST = "DESKOS_SD_STATUS"
STATUS_REPLY = "DESKOS_SD_STATUS"
MOUNT_REQUEST = "DESKOS_SD_MOUNT"
MOUNT_REPLY = "DESKOS_SD_MOUNT"
PING_REQUEST = "DESKOS_SD_PING"
PING_REPLY = "DESKOS_SD_PING"
BOOTLOADER_REQUEST = "DESKOS_SD_BOOTLOADER"
BOOTLOADER_REPLY = "DESKOS_SD_BOOTLOADER"
DIAG_REQUEST = "DESKOS_SD_DIAG"
DIAG_REPLY = "DESKOS_SD_DIAG"
FILE_REQUEST = "DESKOS_SD_FILE"
FILE_REPLY = "DESKOS_SD_FILE"
FILE_PROTOCOL_VERSION = 1
FILE_LINE_MAX = 512
MAX_FILE_PATH_CHARS = 96
MAX_FILE_CHUNK_BYTES = 192
FILE_CANARY_TMP_PATH = "canary/filecanary.tmp"
FILE_CANARY_FINAL_PATH = "canary/filecanary.bin"
FILE_CANARY_PAYLOAD = b"DeskOS SD file canary v1"
FILE_CANARY_START_ID = 1
EXPORT_CANARY_START_ID = 20
EXPORT_CANARY_DEFAULT_TOKEN = "export1"
DIAGNOSTIC_EXPORT_START_ID = 40
DIAGNOSTIC_EXPORT_DEFAULT_TOKEN = "diag1"
MAP_TILE_CANARY_START_ID = 80
MAP_TILE_CANARY_DEFAULT_TOKEN = "map1"
DATA_EXPORT_START_ID = 120
DATA_EXPORT_DEFAULT_TOKEN = "data1"
DESKOS_MANIFEST_PATH = "manifest.json"
DESKOS_MANIFEST_TMP_PATH = "manifest.json.tmp"
DESKOS_MANIFEST_BAD_PATH = "manifest.json.bad"
DESKOS_MAP_MANIFEST_PATH = "map/manifest.json"
DESKOS_MAP_MANIFEST_TMP_PATH = "map/manifest.json.tmp"
DESKOS_MAP_MANIFEST_BAD_PATH = "map/manifest.json.bad"
DESKOS_MANIFEST_PAYLOAD = (
    b'{"name":"MeshCore DeskOS D1L SD","schema":1,'
    b'"created_by":"MeshCore DeskOS D1L",'
    b'"device":"seeed-indicator-d1l",'
    b'"stores":["messages","dm","nodes","routes","packets","map_tiles"]}\n'
)
DESKOS_MAP_MANIFEST_PAYLOAD = (
    b'{"schema":1,"kind":"map_cache",'
    b'"tile_template":"map/tiles/z{z}/x{x}/y{y}.tile",'
    b'"download_supported":false}\n'
)
DESKOS_REQUIRED_DIRS = (
    "stores",
    "stores/messages",
    "stores/messages/public",
    "stores/messages/dm",
    "stores/nodes",
    "stores/contacts",
    "stores/routes",
    "stores/packet_log",
    "map",
    "map/tiles",
    "map/packs",
    "canary",
    "exports",
    "exports/diagnostics",
    "exports/data",
    "tmp",
    "logs",
)
STATUS_FIELDS = (
    "state",
    "present",
    "mounted",
    "deskos",
    "fs",
    "needs_fat32",
    "capacity_kb",
    "free_kb",
    "note",
    "probe_power",
    "probe_mode",
    "probe_present",
    "probe_err",
    "probe_data",
    "detect",
    "detect_driven",
    "det_pullup",
    "det_pulldown",
    "mount_err",
    "mount_data",
)
FILE_CAPABILITY_FIELDS = (
    "file_ops",
    "file_line_max",
    "file_chunk_max",
    "path_max",
    "atomic_rename",
)
PING_FIELDS = (
    "v",
    "file_line_max",
    "file_chunk_max",
    "path_max",
    "atomic_rename",
    "sd_touch",
)
FILE_REPLY_COMMON_FIELDS = ("v", "id", "ok", "op", "note")
FILE_ERROR_CODES = {
    "bad_request",
    "bad_value",
    "unsupported_op",
    "line_too_long",
    "not_ready",
    "no_card",
    "bad_path",
    "not_found",
    "is_dir",
    "exists",
    "range",
    "too_large",
    "decode_failed",
    "crc_mismatch",
    "open_failed",
    "read_failed",
    "write_failed",
    "flush_failed",
    "rename_failed",
    "delete_failed",
    "io_error",
    "timeout",
}
_VALID_PATH_RE = re.compile(r"^[A-Za-z0-9._/-]+$")


@dataclass(frozen=True)
class SdScenario:
    state: str
    present: bool
    mounted: bool
    deskos: bool
    fs: str
    needs_fat32: bool
    capacity_kb: int
    free_kb: int
    note: str


@dataclass
class SdFileSystem:
    files: dict[str, bytes] = field(default_factory=dict)
    dirs: set[str] = field(default_factory=lambda: {"."})


SCENARIOS: dict[str, SdScenario] = {
    "no-card": SdScenario(
        state="no_card",
        present=False,
        mounted=False,
        deskos=False,
        fs="none",
        needs_fat32=False,
        capacity_kb=0,
        free_kb=0,
        note="no_card",
    ),
    "ready": SdScenario(
        state="ready",
        present=True,
        mounted=True,
        deskos=True,
        fs="fat32",
        needs_fat32=False,
        capacity_kb=31166976,
        free_kb=31100000,
        note="ready",
    ),
    "needs-fat32": SdScenario(
        state="not_fat32_or_unmountable",
        present=True,
        mounted=False,
        deskos=False,
        fs="unknown",
        needs_fat32=True,
        capacity_kb=31166976,
        free_kb=0,
        note="needs_fat32_on_computer",
    ),
    "fat16-mounted": SdScenario(
        state="not_fat32_or_unmountable",
        present=True,
        mounted=True,
        deskos=False,
        fs="fat16",
        needs_fat32=True,
        capacity_kb=2097152,
        free_kb=1000000,
        note="needs_fat32_on_computer",
    ),
    "root-missing": SdScenario(
        state="creating_deskos_files",
        present=True,
        mounted=True,
        deskos=False,
        fs="fat32",
        needs_fat32=False,
        capacity_kb=31166976,
        free_kb=31090000,
        note="deskos_root_missing",
    ),
    "mount-required": SdScenario(
        state="mount_required",
        present=False,
        mounted=False,
        deskos=False,
        fs="none",
        needs_fat32=False,
        capacity_kb=0,
        free_kb=0,
        note="mount_not_checked",
    ),
    "mount-pending": SdScenario(
        state="mount_pending",
        present=False,
        mounted=False,
        deskos=False,
        fs="none",
        needs_fat32=False,
        capacity_kb=0,
        free_kb=0,
        note="mount_in_progress",
    ),
}


def bool_token(value: bool) -> str:
    return "1" if value else "0"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def b64url_decode(value: str) -> bytes:
    if not value:
        return b""
    padding = "=" * ((4 - len(value) % 4) % 4)
    return base64.urlsafe_b64decode(value + padding)


def crc32_hex(data: bytes) -> str:
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"


def encode_path(path: str) -> str:
    if not validate_relative_path(path):
        raise ValueError(f"invalid path: {path}")
    return b64url(path.encode("ascii"))


def decode_path(path64: str) -> str:
    try:
        path = b64url_decode(path64).decode("ascii")
    except (ValueError, UnicodeDecodeError) as exc:
        raise ValueError("decode_failed") from exc
    if not validate_relative_path(path):
        raise ValueError("bad_path")
    return path


def validate_relative_path(path: str) -> bool:
    if not path or len(path) > MAX_FILE_PATH_CHARS:
        return False
    if path.startswith("/") or path.endswith("/"):
        return False
    if ".." in path or "//" in path:
        return False
    if not _VALID_PATH_RE.fullmatch(path):
        return False
    return all(segment not in {"", ".", ".."} for segment in path.split("/"))


def status_line(scenario: SdScenario, prefix: str = STATUS_REPLY) -> str:
    probe_present = scenario.present
    return (
        f"{prefix} state={scenario.state}"
        f" present={bool_token(scenario.present)}"
        f" mounted={bool_token(scenario.mounted)}"
        f" deskos={bool_token(scenario.deskos)}"
        f" fs={scenario.fs}"
        f" needs_fat32={bool_token(scenario.needs_fat32)}"
        f" capacity_kb={scenario.capacity_kb}"
        f" free_kb={scenario.free_kb}"
        f" note={scenario.note}"
        f" probe_power=high"
        f" probe_mode={'mount' if scenario.mounted else 'dedicated'}"
        f" probe_present={bool_token(probe_present)}"
        f" probe_err={0 if probe_present else 254}"
        f" probe_data=0"
        f" detect=floating"
        f" detect_driven=0"
        f" det_pullup=1"
        f" det_pulldown=0"
        f" mount_err={0 if scenario.mounted else (1 if scenario.present else 254)}"
        f" mount_data=0"
        f" file_ops={bool_token(file_ready(scenario))}"
        f" file_line_max={FILE_LINE_MAX}"
        f" file_chunk_max={MAX_FILE_CHUNK_BYTES}"
        f" path_max={MAX_FILE_PATH_CHARS}"
        f" atomic_rename={bool_token(file_ready(scenario))}"
    )


def ping_line() -> str:
    return (
        f"{PING_REPLY} v=1"
        f" file_line_max={FILE_LINE_MAX}"
        f" file_chunk_max={MAX_FILE_CHUNK_BYTES}"
        f" path_max={MAX_FILE_PATH_CHARS}"
        " atomic_rename=1"
        " sd_touch=0"
    )


def bootloader_line() -> str:
    return (
        f"{BOOTLOADER_REPLY} ok=1"
        " sd_touch=0"
        " public_rf_tx=0"
        " formats_sd=0"
        " note=entering_uf2"
    )


def diag_probe_tokens(prefix: str, present: bool, err: int, capacity_kb: int) -> str:
    cmd0 = 1 if present else 255
    cmd8 = 1 if present else 255
    r7 = (0, 0, 1, 170) if present else (0, 0, 0, 0)
    return (
        f" {prefix}_p={bool_token(present)}"
        f" {prefix}_e={err}"
        f" {prefix}_d=0"
        f" {prefix}_c0={cmd0}"
        f" {prefix}_c8={cmd8}"
        f" {prefix}_r70={r7[0]}"
        f" {prefix}_r71={r7[1]}"
        f" {prefix}_r72={r7[2]}"
        f" {prefix}_r73={r7[3]}"
        f" {prefix}_kb={capacity_kb}"
    )


def diag_line(scenario: SdScenario) -> str:
    err = 0 if scenario.present else 254
    capacity = scenario.capacity_kb if scenario.present else 0
    return (
        f"{DIAG_REPLY} pins=det7-cs13-sck10-mosi11-miso12-pwr18 hz=1000000"
        f" selected_power=high selected_mode=dedicated"
        f" mount_selected={bool_token(scenario.mounted)}"
        f" detect=floating detect_driven=0 det_pullup=1 det_pulldown=0"
        f"{diag_probe_tokens('hd', scenario.present, err, capacity)}"
        f"{diag_probe_tokens('hs', False, 254, 0)}"
        f"{diag_probe_tokens('ld', False, 254, 0)}"
        f"{diag_probe_tokens('ls', False, 254, 0)}"
        f"{diag_probe_tokens('bb', scenario.present, err, capacity)}"
        f"{diag_probe_tokens('bi', scenario.present, err, capacity)}"
        f"{diag_probe_tokens('bs', scenario.present, err, capacity)}"
        f"{diag_probe_tokens('bcm', scenario.present, err, capacity)}"
        f"{diag_probe_tokens('bsc', scenario.present, err, capacity)}"
    )


def manifest_valid(fs: SdFileSystem, path: str = DESKOS_MANIFEST_PATH) -> bool:
    payload = fs.files.get(path)
    return (
        payload is not None
        and len(payload) <= 512
        and b'"schema":1' in payload
        and b'"name":"MeshCore DeskOS D1L SD"' in payload
        and b'"device":"seeed-indicator-d1l"' in payload
        and b'"map_tiles"' in payload
    )


def map_manifest_valid(fs: SdFileSystem, path: str = DESKOS_MAP_MANIFEST_PATH) -> bool:
    payload = fs.files.get(path)
    return (
        payload is not None
        and len(payload) <= 512
        and b'"schema":1' in payload
        and b'"kind":"map_cache"' in payload
        and b'"tile_template":"map/tiles/z{z}/x{x}/y{y}.tile"' in payload
    )


def write_atomic_text_file(
    fs: SdFileSystem,
    final_path: str,
    tmp_path: str,
    bad_path: str,
    payload: bytes,
    validator,
) -> bool:
    fs.files.pop(tmp_path, None)
    fs.files[tmp_path] = payload
    if not validator(fs, tmp_path):
        fs.files.pop(tmp_path, None)
        return False
    if final_path in fs.files:
        if validator(fs, final_path):
            fs.files.pop(tmp_path, None)
            return True
        fs.files.pop(bad_path, None)
        fs.files[bad_path] = fs.files.pop(final_path)
    fs.files[final_path] = fs.files.pop(tmp_path)
    return validator(fs, final_path)


def preserve_invalid_manifest(fs: SdFileSystem, final_path: str, tmp_path: str, bad_path: str) -> bool:
    fs.files.pop(tmp_path, None)
    if final_path not in fs.files:
        return False
    fs.files.pop(bad_path, None)
    fs.files[bad_path] = fs.files.pop(final_path)
    return True


def prepare_deskos_filesystem(fs: SdFileSystem) -> tuple[bool, bool, str]:
    created = not set(DESKOS_REQUIRED_DIRS).issubset(fs.dirs)
    fs.dirs.update(DESKOS_REQUIRED_DIRS)
    if DESKOS_MANIFEST_PATH in fs.files:
        if not manifest_valid(fs):
            if not preserve_invalid_manifest(
                fs,
                DESKOS_MANIFEST_PATH,
                DESKOS_MANIFEST_TMP_PATH,
                DESKOS_MANIFEST_BAD_PATH,
            ):
                return False, created, "deskos_manifest_unavailable"
            return False, created, "deskos_manifest_invalid"
    else:
        created = True
        if not write_atomic_text_file(
            fs,
            DESKOS_MANIFEST_PATH,
            DESKOS_MANIFEST_TMP_PATH,
            DESKOS_MANIFEST_BAD_PATH,
            DESKOS_MANIFEST_PAYLOAD,
            manifest_valid,
        ):
            return False, created, "deskos_manifest_unavailable"
    if DESKOS_MAP_MANIFEST_PATH in fs.files:
        if not map_manifest_valid(fs):
            if not preserve_invalid_manifest(
                fs,
                DESKOS_MAP_MANIFEST_PATH,
                DESKOS_MAP_MANIFEST_TMP_PATH,
                DESKOS_MAP_MANIFEST_BAD_PATH,
            ):
                return False, created, "deskos_map_manifest_unavailable"
            return False, created, "deskos_map_manifest_invalid"
    else:
        created = True
        if not write_atomic_text_file(
            fs,
            DESKOS_MAP_MANIFEST_PATH,
            DESKOS_MAP_MANIFEST_TMP_PATH,
            DESKOS_MAP_MANIFEST_BAD_PATH,
            DESKOS_MAP_MANIFEST_PAYLOAD,
            map_manifest_valid,
        ):
            return False, created, "deskos_map_manifest_unavailable"
    return True, created, "structure_created" if created else "ready"


def mounted_scenario(scenario: SdScenario, fs: SdFileSystem) -> SdScenario:
    if not scenario.present:
        return replace(scenario, state="no_card", note="no_card")
    if scenario.needs_fat32:
        return scenario
    if not scenario.mounted:
        return scenario
    if scenario.fs != "fat32":
        return replace(
            scenario,
            state="not_fat32_or_unmountable",
            deskos=False,
            needs_fat32=True,
            note="needs_fat32_on_computer",
        )
    prepared, created, prepare_note = prepare_deskos_filesystem(fs)
    if not prepared:
        state = (
            "deskos_manifest_invalid"
            if prepare_note in {"deskos_manifest_invalid", "deskos_map_manifest_invalid"}
            else "error"
        )
        return replace(scenario, state=state, deskos=False, note=prepare_note)
    note = prepare_note if created or not scenario.deskos else scenario.note
    return replace(
        scenario,
        state="ready",
        mounted=True,
        deskos=True,
        needs_fat32=False,
        note=note,
    )


def parse_tokens(request: str) -> tuple[str, dict[str, str]]:
    parts = request.strip().split()
    if not parts:
        raise ValueError("empty request")
    tokens: dict[str, str] = {}
    for token in parts[1:]:
        if "=" not in token:
            raise ValueError(f"malformed token: {token}")
        key, value = token.split("=", 1)
        tokens[key] = value
    return parts[0], tokens


def parse_uint_token(tokens: dict[str, str], key: str) -> int:
    try:
        value = int(tokens[key], 10)
    except (KeyError, ValueError) as exc:
        raise ValueError("bad_value") from exc
    if value < 0 or value > 0xFFFFFFFF:
        raise ValueError("bad_value")
    return value


def parse_bool_token(tokens: dict[str, str], key: str) -> bool:
    value = tokens.get(key)
    if value == "1":
        return True
    if value == "0":
        return False
    raise ValueError("bad_value")


def file_ready(scenario: SdScenario) -> bool:
    return (
        scenario.present
        and scenario.mounted
        and scenario.deskos
        and not scenario.needs_fat32
        and scenario.fs == "fat32"
    )


def file_line(**tokens: object) -> str:
    merged = {"v": FILE_PROTOCOL_VERSION, **tokens}
    return " ".join([FILE_REPLY, *(f"{key}={value}" for key, value in merged.items())])


def file_request_line(request_id: int, op: str, **tokens: object) -> str:
    merged = {"v": FILE_PROTOCOL_VERSION, "id": request_id, "op": op, **tokens}
    return " ".join([FILE_REQUEST, *(f"{key}={value}" for key, value in merged.items())])


def file_error(request_id: int, op: str, err: str) -> str:
    assert err in FILE_ERROR_CODES
    return file_line(id=request_id, ok=0, op=op or "unknown", err=err, note=err)


def require_file_ready(scenario: SdScenario) -> str | None:
    if file_ready(scenario):
        return None
    return "no_card" if not scenario.present else "not_ready"


def command_identity(tokens: dict[str, str]) -> tuple[int, str]:
    request_id = parse_uint_token(tokens, "id")
    if request_id == 0 or request_id > 65535:
        raise ValueError("bad_value")
    op = tokens.get("op", "unknown")
    return request_id, op


def decode_request_path(tokens: dict[str, str], key: str = "path") -> str:
    try:
        return decode_path(tokens[key])
    except KeyError as exc:
        raise ValueError("bad_path") from exc


def decode_request_data(tokens: dict[str, str], expected_len: int) -> bytes:
    if expected_len > MAX_FILE_CHUNK_BYTES:
        raise ValueError("too_large")
    try:
        data = b64url_decode(tokens.get("data", ""))
    except ValueError as exc:
        raise ValueError("decode_failed") from exc
    if len(data) != expected_len:
        raise ValueError("bad_value")
    if tokens.get("crc", "").upper() != crc32_hex(data):
        raise ValueError("crc_mismatch")
    return data


def ensure_parent_dirs(path: str, fs: SdFileSystem) -> None:
    parts = path.split("/")[:-1]
    current = ""
    for part in parts:
        current = part if not current else f"{current}/{part}"
        fs.dirs.add(current)


def stat_file(request_id: int, path: str, fs: SdFileSystem) -> str:
    data = fs.files.get(path)
    if data is not None:
        return file_line(id=request_id, ok=1, op="stat", exists=1, kind="file", size=len(data), note="ok")
    if path in fs.dirs:
        return file_line(id=request_id, ok=1, op="stat", exists=1, kind="dir", size=0, note="ok")
    return file_line(id=request_id, ok=1, op="stat", exists=0, kind="none", size=0, note="ok")


def read_file(request_id: int, tokens: dict[str, str], fs: SdFileSystem) -> str:
    path = decode_request_path(tokens)
    offset = parse_uint_token(tokens, "off")
    requested_len = parse_uint_token(tokens, "len")
    if requested_len > MAX_FILE_CHUNK_BYTES:
        raise ValueError("too_large")
    data = fs.files.get(path)
    if data is None:
        raise ValueError("not_found")
    if offset > len(data):
        raise ValueError("range")
    chunk = data[offset : offset + requested_len]
    return file_line(
        id=request_id,
        ok=1,
        op="read",
        off=offset,
        len=len(chunk),
        eof=bool_token(offset + len(chunk) >= len(data)),
        data=b64url(chunk),
        crc=crc32_hex(chunk),
        note="ok",
    )


def create_file(request_id: int, tokens: dict[str, str], fs: SdFileSystem) -> str:
    path = decode_request_path(tokens)
    expected_len = parse_uint_token(tokens, "len")
    if expected_len == 0:
        raise ValueError("bad_value")
    data = decode_request_data(tokens, expected_len)
    if path in fs.files or path in fs.dirs:
        raise ValueError("exists")
    ensure_parent_dirs(path, fs)
    fs.files[path] = data
    return file_line(
        id=request_id,
        ok=1,
        op="create",
        off=0,
        len=len(data),
        size=len(data),
        note="ok",
    )


def write_file(request_id: int, tokens: dict[str, str], fs: SdFileSystem) -> str:
    path = decode_request_path(tokens)
    offset = parse_uint_token(tokens, "off")
    expected_len = parse_uint_token(tokens, "len")
    truncate = parse_bool_token(tokens, "trunc")
    data = decode_request_data(tokens, expected_len)
    if truncate and offset != 0:
        raise ValueError("range")
    current = b"" if truncate else fs.files.get(path, b"")
    if offset != len(current):
        raise ValueError("range")
    ensure_parent_dirs(path, fs)
    fs.files[path] = current + data
    return file_line(
        id=request_id,
        ok=1,
        op="write",
        off=offset,
        len=len(data),
        size=len(fs.files[path]),
        note="ok",
    )


def append_file(request_id: int, tokens: dict[str, str], fs: SdFileSystem) -> str:
    path = decode_request_path(tokens)
    expected_len = parse_uint_token(tokens, "len")
    if expected_len == 0:
        raise ValueError("bad_value")
    data = decode_request_data(tokens, expected_len)
    current = fs.files.get(path, b"")
    ensure_parent_dirs(path, fs)
    fs.files[path] = current + data
    return file_line(
        id=request_id,
        ok=1,
        op="append",
        off=len(current),
        len=len(data),
        size=len(fs.files[path]),
        note="ok",
    )


def delete_file(request_id: int, tokens: dict[str, str], fs: SdFileSystem) -> str:
    path = decode_request_path(tokens)
    if path not in fs.files:
        raise ValueError("not_found")
    del fs.files[path]
    return file_line(id=request_id, ok=1, op="delete", note="ok")


def rename_file(request_id: int, tokens: dict[str, str], fs: SdFileSystem) -> str:
    source = decode_request_path(tokens)
    target = decode_request_path(tokens, "to")
    replace_target = parse_bool_token(tokens, "replace")
    if source not in fs.files:
        raise ValueError("not_found")
    if target in fs.files and not replace_target:
        raise ValueError("exists")
    ensure_parent_dirs(target, fs)
    fs.files[target] = fs.files.pop(source)
    return file_line(id=request_id, ok=1, op="rename", note="ok")


def file_reply_for_request(request: str, scenario: SdScenario, fs: SdFileSystem) -> str:
    if len(request) > FILE_LINE_MAX:
        return file_error(0, "unknown", "line_too_long")
    try:
        prefix, tokens = parse_tokens(request)
        if prefix != FILE_REQUEST:
            raise ValueError("bad_request")
        if parse_uint_token(tokens, "v") != FILE_PROTOCOL_VERSION:
            raise ValueError("bad_value")
        request_id, op = command_identity(tokens)
        if op not in {
            "stat",
            "read",
            "create",
            "write",
            "append",
            "delete",
            "rename",
        }:
            raise ValueError("unsupported_op")
        not_ready = require_file_ready(scenario)
        if not_ready:
            raise ValueError(not_ready)
        if op == "stat":
            return stat_file(request_id, decode_request_path(tokens), fs)
        if op == "read":
            return read_file(request_id, tokens, fs)
        if op == "create":
            return create_file(request_id, tokens, fs)
        if op == "write":
            return write_file(request_id, tokens, fs)
        if op == "append":
            return append_file(request_id, tokens, fs)
        if op == "delete":
            return delete_file(request_id, tokens, fs)
        if op == "rename":
            return rename_file(request_id, tokens, fs)
    except ValueError as exc:
        err = str(exc) if str(exc) in FILE_ERROR_CODES else "bad_request"
        request_id = 0
        op = "unknown"
        try:
            _, tokens = parse_tokens(request)
            request_id, op = command_identity(tokens)
        except ValueError:
            pass
        return file_error(request_id, op, err)
    raise AssertionError("unreachable")


def reply_for_request(
    request: str,
    scenario: SdScenario,
    fs: SdFileSystem | None = None,
) -> str:
    request = request.strip()
    fs = fs or SdFileSystem()
    if request == PING_REQUEST:
        return ping_line()
    if request == BOOTLOADER_REQUEST:
        return bootloader_line()
    if request == STATUS_REQUEST:
        return status_line(scenario)
    if request == MOUNT_REQUEST:
        return status_line(mounted_scenario(scenario, fs), MOUNT_REPLY)
    if request == DIAG_REQUEST:
        return diag_line(scenario)
    if request.startswith(FILE_REQUEST):
        return file_reply_for_request(request, scenario, fs)
    raise ValueError(f"unsupported request: {request}")


def file_canary_requests(request_id_start: int = FILE_CANARY_START_ID) -> list[str]:
    tmp = encode_path(FILE_CANARY_TMP_PATH)
    final = encode_path(FILE_CANARY_FINAL_PATH)
    payload = FILE_CANARY_PAYLOAD
    payload_len = len(payload)
    payload64 = b64url(payload)
    payload_crc = crc32_hex(payload)
    request_id = request_id_start
    requests = [
        file_request_line(request_id, "delete", path=tmp),
        file_request_line(request_id + 1, "delete", path=final),
        file_request_line(
            request_id + 2,
            "write",
            path=tmp,
            off=0,
            len=payload_len,
            trunc=1,
            data=payload64,
            crc=payload_crc,
        ),
        file_request_line(request_id + 3, "read", path=tmp, off=0, len=payload_len),
        file_request_line(request_id + 4, "rename", path=tmp, to=final, replace=1),
        file_request_line(request_id + 5, "stat", path=final),
        file_request_line(request_id + 6, "read", path=final, off=0, len=payload_len),
        file_request_line(request_id + 7, "delete", path=final),
        file_request_line(request_id + 8, "stat", path=final),
    ]
    return requests


def file_canary_transcript(
    scenario: SdScenario,
    request_id_start: int = FILE_CANARY_START_ID,
) -> list[dict[str, str]]:
    fs = SdFileSystem()
    transcript = []
    for request in file_canary_requests(request_id_start):
        transcript.append({"request": request, "reply": reply_for_request(request, scenario, fs)})
    return transcript


def export_canary_paths(token: str) -> tuple[str, str]:
    return (
        f"exports/diagnostics/export-canary-{token}.tmp",
        f"exports/diagnostics/export-canary-{token}.json",
    )


def export_canary_payload(token: str) -> bytes:
    return (
        f'{{"schema":1,"kind":"diagnostic_export_canary","token":"{token}",'
        '"public_rf_tx":false,"formats_sd":false}\n'
    ).encode("ascii")


def export_canary_requests(token: str, request_id_start: int = EXPORT_CANARY_START_ID) -> list[str]:
    tmp_path, final_path = export_canary_paths(token)
    tmp = encode_path(tmp_path)
    final = encode_path(final_path)
    payload = export_canary_payload(token)
    payload64 = b64url(payload)
    payload_crc = crc32_hex(payload)
    request_id = request_id_start
    return [
        file_request_line(request_id, "delete", path=tmp),
        file_request_line(
            request_id + 1,
            "write",
            path=tmp,
            off=0,
            len=len(payload),
            trunc=1,
            data=payload64,
            crc=payload_crc,
        ),
        file_request_line(request_id + 2, "read", path=tmp, off=0, len=len(payload)),
        file_request_line(request_id + 3, "rename", path=tmp, to=final, replace=1),
        file_request_line(request_id + 4, "stat", path=final),
        file_request_line(request_id + 5, "read", path=final, off=0, len=len(payload)),
    ]


def export_canary_transcript(
    scenario: SdScenario,
    token: str = EXPORT_CANARY_DEFAULT_TOKEN,
    request_id_start: int = EXPORT_CANARY_START_ID,
) -> list[dict[str, str]]:
    fs = SdFileSystem()
    transcript = []
    for request in export_canary_requests(token, request_id_start):
        transcript.append({"request": request, "reply": reply_for_request(request, scenario, fs)})
    return transcript


def diagnostic_export_paths(token: str) -> tuple[str, str]:
    return (
        f"exports/diagnostics/diagnostic-export-{token}.tmp",
        f"exports/diagnostics/diagnostic-export-{token}.json",
    )


def diagnostic_export_payload(token: str) -> bytes:
    return (
        f'{{"schema":1,"kind":"diagnostic_export","token":"{token}",'
        '"public_rf_tx":false,"formats_sd":false,'
        '"storage":{"sd_state":"ready","data_backend":"mixed","export_backend":"sd_diagnostic_exports_ready"},'
        '"health":{"uptime_ms":123456,"heap_free":100000,"heap_min_free":99000,'
        '"heap_largest_free":50000,"psram_free":200000,"psram_min_free":199000,'
        '"psram_largest_free":150000,"current_task_stack_free_words":1200,'
        '"ui_task_stack_free_words":1300,"lvgl_used_pct":42,"reset_reason":"SW"},'
        '"limits":{"payload_max":4096,"file_chunk_max":192,"crashlog_capacity":8,"sampled":true},'
        '"map_tiles":{"exported":false,"cache_ready":true,"backend":"sd_map_tiles_ready",'
        '"reason":"diagnostic_export_does_not_bundle_map_tiles"},'
        '"crashlog":{"count":1,"capacity":8,"total_written":3,"dropped_oldest":0,'
        '"entries":[{"seq":3,"uptime_ms":50,"reset_reason":"SW","reset_reason_code":3,'
        '"crash_like":false,"heap_free":100000,"heap_min_free":99000,"psram_free":200000}]}}\n'
    ).encode("ascii")


def chunk_ranges(total_len: int, chunk_len: int = MAX_FILE_CHUNK_BYTES) -> list[tuple[int, int]]:
    return [
        (offset, min(chunk_len, total_len - offset))
        for offset in range(0, total_len, chunk_len)
    ]


def diagnostic_export_requests(
    token: str,
    request_id_start: int = DIAGNOSTIC_EXPORT_START_ID,
) -> list[str]:
    tmp_path, final_path = diagnostic_export_paths(token)
    tmp = encode_path(tmp_path)
    final = encode_path(final_path)
    payload = diagnostic_export_payload(token)
    request_id = request_id_start
    requests = [file_request_line(request_id, "delete", path=tmp)]
    request_id += 1
    for offset, chunk_len in chunk_ranges(len(payload)):
        chunk = payload[offset : offset + chunk_len]
        requests.append(
            file_request_line(
                request_id,
                "write",
                path=tmp,
                off=offset,
                len=chunk_len,
                trunc=1 if offset == 0 else 0,
                data=b64url(chunk),
                crc=crc32_hex(chunk),
            )
        )
        request_id += 1
    for offset, chunk_len in chunk_ranges(len(payload)):
        requests.append(file_request_line(request_id, "read", path=tmp, off=offset, len=chunk_len))
        request_id += 1
    requests.append(file_request_line(request_id, "rename", path=tmp, to=final, replace=1))
    request_id += 1
    requests.append(file_request_line(request_id, "stat", path=final))
    request_id += 1
    for offset, chunk_len in chunk_ranges(len(payload)):
        requests.append(file_request_line(request_id, "read", path=final, off=offset, len=chunk_len))
        request_id += 1
    return requests


def diagnostic_export_transcript(
    scenario: SdScenario,
    token: str = DIAGNOSTIC_EXPORT_DEFAULT_TOKEN,
    request_id_start: int = DIAGNOSTIC_EXPORT_START_ID,
) -> list[dict[str, str]]:
    fs = SdFileSystem()
    transcript = []
    for request in diagnostic_export_requests(token, request_id_start):
        transcript.append({"request": request, "reply": reply_for_request(request, scenario, fs)})
    return transcript


def data_export_paths(token: str) -> tuple[str, str]:
    return (
        f"exports/data/data-export-{token}.tmp",
        f"exports/data/data-export-{token}.json",
    )


def data_export_payload(token: str) -> bytes:
    return (
        f'{{"schema":1,"kind":"data_export","token":"{token}",'
        '"sampled":true,"public_rf_tx":false,"formats_sd":false,'
        '"private_identity_exported":false,'
        '"storage":{"sd_state":"ready","data_backend":"mixed",'
        '"export_backend":"sd_diagnostic_exports_ready",'
        '"stores":{"messages":"sd","dm":"sd","routes":"sd","packets":"sd",'
        '"contacts":"onboard","nodes":"onboard","read_state":"onboard",'
        '"map_tiles":"sd_map_tiles_ready"},"file_ops":true,"atomic_rename":true},'
        '"limits":{"payload_max":12288,"sample_max":4},'
        '"settings":{"role":"client","onboarding_complete":true,'
        '"region":"Canada/USA","radio":{"frequency_hz":910525000,'
        '"bandwidth_khz":62.5,"sf":7,"cr":5,"tx_power_dbm":20,'
        '"rx_boost":true,"tcxo":"auto"},"node_name":"DeskOS Export"},'
        '"messages":{"count":1,"capacity":16,"total_written":7,"dropped_oldest":0,'
        '"entries":[{"seq":7,"uptime_ms":1000,"direction":"rx","author":"Meshbot",'
        '"text":"hello export","rssi_dbm":-70,"snr_tenths":80,'
        '"path_hash_bytes":2,"path_hops":1,"delivered":true}]},'
        '"dm":{"count":1,"capacity":16,"total_written":2,"dropped_oldest":0,'
        '"entries":[{"seq":2,"uptime_ms":1200,"contact_fingerprint":"0011223344556677",'
        '"contact_alias":"Meshbot","direction":"rx","text":"dm export",'
        '"rssi_dbm":-71,"snr_tenths":75,"path_hash_bytes":2,"path_hops":1,'
        '"attempt":0,"delivered":true,"acked":true,"ack_hash":1234}]},'
        '"routes":{"count":1,"capacity":16,"total_written":3,"dropped_oldest":0,'
        '"entries":[{"seq":3,"first_seen_ms":900,"last_seen_ms":1300,'
        '"seen_count":2,"target":"0011223344556677","label":"Meshbot",'
        '"kind":"advert","route":"local","direction":"rx","last_rssi_dbm":-71,'
        '"last_snr_tenths":75,"path_hash_bytes":2,"path_hops":1,'
        '"confidence":2,"payload_len":32}]},'
        '"packets":{"count":1,"capacity":32,"total_written":5,"dropped_oldest":0,'
        '"entries":[{"seq":5,"uptime_ms":1300,"direction":"rx","kind":"text",'
        '"rssi_dbm":-71,"snr_tenths":75,"path_hash_bytes":2,"path_hops":1,'
        '"payload_len":12,"raw_len":12,"raw_truncated":false,'
        '"note":"export sample","raw_hex":"68656C6C6F"}]},'
        '"contacts":{"count":1,"capacity":16,"total_written":1,"dropped_oldest":0,'
        '"entries":[{"seq":1,"created_ms":800,"updated_ms":1300,'
        '"out_path_updated_ms":0,"fingerprint":"0011223344556677",'
        '"public_key_hex":"00112233445566778899AABBCCDDEEFF",'
        '"alias":"Meshbot","heard_name":"Meshbot","type":"client",'
        '"last_rssi_dbm":-71,"last_snr_tenths":75,"path_hash_bytes":2,'
        '"path_hops":1,"out_path_valid":false,"out_path_len":0,'
        '"favorite":true,"muted":false}]},'
        '"nodes":{"count":1,"capacity":16,"total_written":1,"dropped_oldest":0,'
        '"entries":[{"seq":1,"first_heard_ms":800,"last_heard_ms":1300,'
        '"advert_timestamp":42,"heard_count":2,"fingerprint":"0011223344556677",'
        '"public_key_hex":"00112233445566778899AABBCCDDEEFF",'
        '"name":"Meshbot","type":"client","rssi_dbm":-71,"snr_tenths":75,'
        '"path_hash_bytes":2,"path_hops":1}]},'
        '"read_state":{"last_public_read_seq":7,"last_dm_read_seq":2,'
        '"newest_public_rx_seq":7,"newest_dm_rx_seq":2,"public_unread_count":0,'
        '"dm_unread_count":0,"muted_dm_unread_count":0,"dm_thread_count":1,'
        '"mark_read_count":2,"threads":[{"last_read_seq":2,"newest_rx_seq":2,'
        '"unread_count":0,"muted":false,"fingerprint":"0011223344556677"}]}}\n'
    ).encode("ascii")


def data_export_requests(
    token: str,
    request_id_start: int = DATA_EXPORT_START_ID,
) -> list[str]:
    tmp_path, final_path = data_export_paths(token)
    tmp = encode_path(tmp_path)
    final = encode_path(final_path)
    payload = data_export_payload(token)
    request_id = request_id_start
    requests = [file_request_line(request_id, "delete", path=tmp)]
    request_id += 1
    for offset, chunk_len in chunk_ranges(len(payload)):
        chunk = payload[offset : offset + chunk_len]
        requests.append(
            file_request_line(
                request_id,
                "write",
                path=tmp,
                off=offset,
                len=chunk_len,
                trunc=1 if offset == 0 else 0,
                data=b64url(chunk),
                crc=crc32_hex(chunk),
            )
        )
        request_id += 1
    for offset, chunk_len in chunk_ranges(len(payload)):
        requests.append(file_request_line(request_id, "read", path=tmp, off=offset, len=chunk_len))
        request_id += 1
    requests.append(file_request_line(request_id, "rename", path=tmp, to=final, replace=1))
    request_id += 1
    requests.append(file_request_line(request_id, "stat", path=final))
    request_id += 1
    for offset, chunk_len in chunk_ranges(len(payload)):
        requests.append(file_request_line(request_id, "read", path=final, off=offset, len=chunk_len))
        request_id += 1
    return requests


def data_export_transcript(
    scenario: SdScenario,
    token: str = DATA_EXPORT_DEFAULT_TOKEN,
    request_id_start: int = DATA_EXPORT_START_ID,
) -> list[dict[str, str]]:
    fs = SdFileSystem()
    transcript = []
    for request in data_export_requests(token, request_id_start):
        transcript.append({"request": request, "reply": reply_for_request(request, scenario, fs)})
    return transcript


def map_tile_canary_paths(token: str) -> tuple[str, str]:
    return (
        f"map/tiles/z12/x1/y2-{token}.tmp",
        f"map/tiles/z12/x1/y2-{token}.tile",
    )


def map_tile_canary_payload(token: str) -> bytes:
    return (
        f'{{"schema":1,"kind":"map_tile_cache_canary","token":"{token}",'
        '"z":12,"x":1,"y":2,"public_rf_tx":false,"formats_sd":false}\n'
    ).encode("ascii")


def map_tile_canary_requests(
    token: str,
    request_id_start: int = MAP_TILE_CANARY_START_ID,
) -> list[str]:
    tmp_path, final_path = map_tile_canary_paths(token)
    tmp = encode_path(tmp_path)
    final = encode_path(final_path)
    payload = map_tile_canary_payload(token)
    payload64 = b64url(payload)
    payload_crc = crc32_hex(payload)
    return [
        file_request_line(request_id_start, "delete", path=tmp),
        file_request_line(
            request_id_start + 1,
            "write",
            path=tmp,
            off=0,
            len=len(payload),
            trunc=1,
            data=payload64,
            crc=payload_crc,
        ),
        file_request_line(request_id_start + 2, "read", path=tmp, off=0, len=len(payload)),
        file_request_line(request_id_start + 3, "rename", path=tmp, to=final, replace=1),
        file_request_line(request_id_start + 4, "stat", path=final),
        file_request_line(request_id_start + 5, "read", path=final, off=0, len=len(payload)),
    ]


def map_tile_canary_transcript(
    scenario: SdScenario,
    token: str = MAP_TILE_CANARY_DEFAULT_TOKEN,
    request_id_start: int = MAP_TILE_CANARY_START_ID,
) -> list[dict[str, str]]:
    fs = SdFileSystem()
    transcript = []
    for request in map_tile_canary_requests(token, request_id_start):
        transcript.append({"request": request, "reply": reply_for_request(request, scenario, fs)})
    return transcript


def map_tile_check_requests(
    token: str,
    request_id_start: int = MAP_TILE_CANARY_START_ID + 20,
) -> list[str]:
    _tmp_path, final_path = map_tile_canary_paths(token)
    final = encode_path(final_path)
    payload = map_tile_canary_payload(token)
    return [
        file_request_line(request_id_start, "stat", path=final),
        file_request_line(request_id_start + 1, "read", path=final, off=0, len=len(payload)),
    ]


def map_tile_check_transcript(
    scenario: SdScenario,
    token: str = MAP_TILE_CANARY_DEFAULT_TOKEN,
    request_id_start: int = MAP_TILE_CANARY_START_ID + 20,
) -> list[dict[str, str]]:
    fs = SdFileSystem()
    _tmp_path, final_path = map_tile_canary_paths(token)
    if file_ready(scenario):
        ensure_parent_dirs(final_path, fs)
        fs.files[final_path] = map_tile_canary_payload(token)
    transcript = []
    for request in map_tile_check_requests(token, request_id_start):
        transcript.append({"request": request, "reply": reply_for_request(request, scenario, fs)})
    return transcript


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", choices=sorted(SCENARIOS), default="ready")
    parser.add_argument("--request", default=STATUS_REQUEST)
    parser.add_argument("--file-canary-transcript", action="store_true")
    parser.add_argument("--export-canary-transcript", action="store_true")
    parser.add_argument("--diagnostic-export-transcript", action="store_true")
    parser.add_argument("--data-export-transcript", action="store_true")
    parser.add_argument("--map-tile-canary-transcript", action="store_true")
    parser.add_argument("--request-id-start", type=int)
    parser.add_argument("--token")
    parser.add_argument("--list-scenarios", action="store_true")
    args = parser.parse_args()

    if args.list_scenarios:
        for name in sorted(SCENARIOS):
            print(name)
        return 0

    if args.file_canary_transcript:
        request_id_start = (
            FILE_CANARY_START_ID if args.request_id_start is None else args.request_id_start
        )
        for exchange in file_canary_transcript(SCENARIOS[args.scenario], request_id_start):
            print(f"> {exchange['request']}")
            print(f"< {exchange['reply']}")
        return 0

    if args.export_canary_transcript:
        request_id_start = (
            EXPORT_CANARY_START_ID if args.request_id_start is None else args.request_id_start
        )
        token = args.token or EXPORT_CANARY_DEFAULT_TOKEN
        for exchange in export_canary_transcript(SCENARIOS[args.scenario], token, request_id_start):
            print(f"> {exchange['request']}")
            print(f"< {exchange['reply']}")
        return 0

    if args.diagnostic_export_transcript:
        request_id_start = (
            DIAGNOSTIC_EXPORT_START_ID if args.request_id_start is None else args.request_id_start
        )
        token = args.token or DIAGNOSTIC_EXPORT_DEFAULT_TOKEN
        for exchange in diagnostic_export_transcript(SCENARIOS[args.scenario], token, request_id_start):
            print(f"> {exchange['request']}")
            print(f"< {exchange['reply']}")
        return 0

    if args.data_export_transcript:
        request_id_start = (
            DATA_EXPORT_START_ID if args.request_id_start is None else args.request_id_start
        )
        token = args.token or DATA_EXPORT_DEFAULT_TOKEN
        for exchange in data_export_transcript(SCENARIOS[args.scenario], token, request_id_start):
            print(f"> {exchange['request']}")
            print(f"< {exchange['reply']}")
        return 0

    if args.map_tile_canary_transcript:
        request_id_start = (
            MAP_TILE_CANARY_START_ID if args.request_id_start is None else args.request_id_start
        )
        token = args.token or MAP_TILE_CANARY_DEFAULT_TOKEN
        for exchange in map_tile_canary_transcript(SCENARIOS[args.scenario], token, request_id_start):
            print(f"> {exchange['request']}")
            print(f"< {exchange['reply']}")
        return 0

    print(reply_for_request(args.request, SCENARIOS[args.scenario], SdFileSystem()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
