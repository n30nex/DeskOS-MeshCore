from dataclasses import replace
from pathlib import Path

from tools.rp2040_sd_protocol import (
    FILE_CAPABILITY_FIELDS,
    FILE_CANARY_FINAL_PATH,
    FILE_CANARY_PAYLOAD,
    FILE_CANARY_TMP_PATH,
    FILE_ERROR_CODES,
    FILE_LINE_MAX,
    FILE_REPLY,
    FILE_REQUEST,
    DIAG_REPLY,
    DIAG_REQUEST,
    MOUNT_REPLY,
    MOUNT_REQUEST,
    PING_FIELDS,
    PING_REPLY,
    PING_REQUEST,
    MAX_FILE_CHUNK_BYTES,
    MAX_FILE_PATH_CHARS,
    SdFileSystem,
    STATUS_FIELDS,
    SCENARIOS,
    STATUS_REPLY,
    STATUS_REQUEST,
    b64url,
    crc32_hex,
    encode_path,
    DATA_EXPORT_DEFAULT_TOKEN,
    DATA_EXPORT_START_ID,
    EXPORT_CANARY_DEFAULT_TOKEN,
    EXPORT_CANARY_START_ID,
    DIAGNOSTIC_EXPORT_DEFAULT_TOKEN,
    DIAGNOSTIC_EXPORT_START_ID,
    DESKOS_MANIFEST_PATH,
    DESKOS_MANIFEST_TMP_PATH,
    DESKOS_MANIFEST_BAD_PATH,
    DESKOS_MANIFEST_PAYLOAD,
    DESKOS_MAP_MANIFEST_PATH,
    DESKOS_MAP_MANIFEST_TMP_PATH,
    DESKOS_MAP_MANIFEST_BAD_PATH,
    DESKOS_MAP_MANIFEST_PAYLOAD,
    DESKOS_REQUIRED_DIRS,
    MAP_TILE_CANARY_DEFAULT_TOKEN,
    MAP_TILE_CANARY_START_ID,
    data_export_paths,
    data_export_payload,
    data_export_transcript,
    diagnostic_export_paths,
    diagnostic_export_payload,
    diagnostic_export_transcript,
    export_canary_paths,
    export_canary_payload,
    export_canary_transcript,
    file_canary_transcript,
    map_tile_canary_paths,
    map_tile_canary_payload,
    map_tile_canary_transcript,
    map_tile_check_transcript,
    reply_for_request,
    validate_relative_path,
)


ROOT = Path(__file__).resolve().parents[1]


def parse_tokens(line: str) -> dict[str, str]:
    parts = line.split()
    return {"prefix": parts[0], **dict(token.split("=", 1) for token in parts[1:])}


def test_status_protocol_lines_cover_boot_states():
    no_card = parse_tokens(reply_for_request(STATUS_REQUEST, SCENARIOS["no-card"]))
    ready = parse_tokens(reply_for_request(STATUS_REQUEST, SCENARIOS["ready"]))
    setup = parse_tokens(reply_for_request(STATUS_REQUEST, SCENARIOS["needs-fat32"]))
    fat16 = parse_tokens(reply_for_request(STATUS_REQUEST, SCENARIOS["fat16-mounted"]))

    assert no_card["prefix"] == STATUS_REPLY
    assert no_card["state"] == "no_card"
    assert no_card["present"] == "0"
    assert ready["state"] == "ready"
    assert ready["present"] == "1"
    assert ready["mounted"] == "1"
    assert ready["deskos"] == "1"
    assert ready["needs_fat32"] == "0"
    assert setup["state"] == "not_fat32_or_unmountable"
    assert setup["needs_fat32"] == "1"
    assert fat16["state"] == "not_fat32_or_unmountable"
    assert fat16["mounted"] == "1"
    assert fat16["fs"] == "fat16"
    assert fat16["needs_fat32"] == "1"
    assert fat16["file_ops"] == "0"
    assert ready["file_ops"] == "1"
    assert ready["file_line_max"] == str(FILE_LINE_MAX)
    assert ready["file_chunk_max"] == str(MAX_FILE_CHUNK_BYTES)
    assert ready["path_max"] == str(MAX_FILE_PATH_CHARS)
    assert ready["atomic_rename"] == "1"
    assert no_card["file_ops"] == "0"


def test_ping_protocol_is_fast_and_never_touches_sd():
    ping = parse_tokens(reply_for_request(PING_REQUEST, SCENARIOS["ready"]))

    assert ping["prefix"] == PING_REPLY
    assert ping["v"] == "1"
    assert ping["file_line_max"] == str(FILE_LINE_MAX)
    assert ping["file_chunk_max"] == str(MAX_FILE_CHUNK_BYTES)
    assert ping["path_max"] == str(MAX_FILE_PATH_CHARS)
    assert ping["atomic_rename"] == "1"
    assert ping["sd_touch"] == "0"
    assert set(PING_FIELDS).issubset(ping.keys())


def test_mount_protocol_is_explicit_sd_touch_request():
    status = parse_tokens(reply_for_request(STATUS_REQUEST, SCENARIOS["mount-required"]))
    pending = parse_tokens(reply_for_request(STATUS_REQUEST, SCENARIOS["mount-pending"]))
    mounted = parse_tokens(reply_for_request(MOUNT_REQUEST, SCENARIOS["ready"]))
    fs = SdFileSystem()
    root_prepared = parse_tokens(reply_for_request(MOUNT_REQUEST, SCENARIOS["root-missing"], fs))
    fat16_fs = SdFileSystem()
    fat16_rejected = parse_tokens(
        reply_for_request(MOUNT_REQUEST, SCENARIOS["fat16-mounted"], fat16_fs)
    )

    assert status["prefix"] == STATUS_REPLY
    assert status["state"] == "mount_required"
    assert status["present"] == "0"
    assert pending["prefix"] == STATUS_REPLY
    assert pending["state"] == "mount_pending"
    assert pending["file_ops"] == "0"
    assert mounted["prefix"] == MOUNT_REPLY
    assert mounted["state"] == "ready"
    assert mounted["present"] == "1"
    assert mounted["mounted"] == "1"
    assert mounted["file_ops"] == "1"
    assert root_prepared["prefix"] == MOUNT_REPLY
    assert root_prepared["state"] == "ready"
    assert root_prepared["deskos"] == "1"
    assert root_prepared["needs_fat32"] == "0"
    assert root_prepared["file_ops"] == "1"
    assert fat16_rejected["prefix"] == MOUNT_REPLY
    assert fat16_rejected["state"] == "not_fat32_or_unmountable"
    assert fat16_rejected["mounted"] == "1"
    assert fat16_rejected["deskos"] == "0"
    assert fat16_rejected["needs_fat32"] == "1"
    assert fat16_rejected["file_ops"] == "0"
    assert DESKOS_MANIFEST_PATH not in fat16_fs.files
    assert DESKOS_MANIFEST_PATH in fs.files
    assert DESKOS_MAP_MANIFEST_PATH in fs.files
    assert DESKOS_MANIFEST_TMP_PATH not in fs.files
    assert DESKOS_MAP_MANIFEST_TMP_PATH not in fs.files
    assert DESKOS_MANIFEST_BAD_PATH not in fs.files
    assert DESKOS_MAP_MANIFEST_BAD_PATH not in fs.files
    assert set(DESKOS_REQUIRED_DIRS).issubset(fs.dirs)
    assert b'"map_tiles"' in fs.files[DESKOS_MANIFEST_PATH]
    assert b'"kind":"map_cache"' in fs.files[DESKOS_MAP_MANIFEST_PATH]
    manifest_stat = parse_tokens(
        reply_for_request(
            file_request(20, "stat", path=encode_path(DESKOS_MANIFEST_PATH)),
            SCENARIOS["ready"],
            fs,
        )
    )
    assert manifest_stat["ok"] == "1"
    assert manifest_stat["exists"] == "1"
    assert manifest_stat["size"] == str(len(DESKOS_MANIFEST_PAYLOAD))
    map_manifest_stat = parse_tokens(
        reply_for_request(
            file_request(22, "stat", path=encode_path(DESKOS_MAP_MANIFEST_PATH)),
            SCENARIOS["ready"],
            fs,
        )
    )
    assert map_manifest_stat["ok"] == "1"
    assert map_manifest_stat["exists"] == "1"
    assert map_manifest_stat["size"] == str(len(DESKOS_MAP_MANIFEST_PAYLOAD))


def test_diag_protocol_reports_probe_matrix_without_formatting():
    diag = parse_tokens(reply_for_request(DIAG_REQUEST, SCENARIOS["no-card"]))

    assert diag["prefix"] == DIAG_REPLY
    assert diag["pins"] == "det7-cs13-sck10-mosi11-miso12-pwr18"
    assert diag["selected_power"] == "high"
    assert diag["selected_mode"] == "dedicated"
    assert diag["detect"] == "floating"
    assert diag["detect_driven"] == "0"
    assert diag["det_pullup"] == "1"
    assert diag["det_pulldown"] == "0"
    assert diag["hd_p"] == "0"
    assert diag["hs_p"] == "0"
    assert diag["ld_p"] == "0"
    assert diag["ls_p"] == "0"
    assert diag["bb_p"] == "0"
    assert diag["bi_p"] == "0"
    assert diag["bs_p"] == "0"
    assert diag["bcm_p"] == "0"
    assert diag["bsc_p"] == "0"
    for prefix in ("hd", "hs", "ld", "ls", "bb", "bi", "bs", "bcm", "bsc"):
        assert diag[f"{prefix}_c0"] == "255"
        assert diag[f"{prefix}_c8"] == "255"
        assert diag[f"{prefix}_r70"] == "0"
        assert diag[f"{prefix}_r71"] == "0"
        assert diag[f"{prefix}_r72"] == "0"
        assert diag[f"{prefix}_r73"] == "0"


def test_format_protocol_is_not_exposed():
    old_request = "DESKOS_SD_" + "FORMAT"
    old_confirmation = "FORMAT-" + "DESKOS-SD"

    try:
        reply_for_request(f"{old_request} {old_confirmation}", SCENARIOS["needs-fat32"])
    except ValueError as exc:
        assert "unsupported request" in str(exc)
    else:
        raise AssertionError("old SD format request unexpectedly succeeded")


def test_manifest_invalid_is_preserved_as_bad_and_blocks_sd_backend():
    partial_manifest = b'{"schema":'
    stale_tmp = b'{"name":"partial tmp"'
    fs = SdFileSystem(
        files={
            DESKOS_MANIFEST_PATH: partial_manifest,
            DESKOS_MANIFEST_TMP_PATH: stale_tmp,
        }
    )

    mounted = parse_tokens(reply_for_request(MOUNT_REQUEST, SCENARIOS["ready"], fs))
    invalid = replace(
        SCENARIOS["ready"],
        state="deskos_manifest_invalid",
        deskos=False,
        note="deskos_manifest_invalid",
    )
    stat = parse_tokens(
        reply_for_request(
            file_request(21, "stat", path=encode_path(DESKOS_MANIFEST_PATH)),
            invalid,
            fs,
        )
    )

    assert mounted["prefix"] == MOUNT_REPLY
    assert mounted["state"] == "deskos_manifest_invalid"
    assert mounted["deskos"] == "0"
    assert mounted["file_ops"] == "0"
    assert mounted["needs_fat32"] == "0"
    assert mounted["note"] == "deskos_manifest_invalid"
    assert DESKOS_MANIFEST_PATH not in fs.files
    assert fs.files[DESKOS_MANIFEST_BAD_PATH] == partial_manifest
    assert DESKOS_MANIFEST_TMP_PATH not in fs.files
    assert stale_tmp not in fs.files.values()
    assert stat["ok"] == "0"
    assert stat["err"] == "not_ready"


def test_map_manifest_invalid_is_preserved_as_bad_and_blocks_sd_backend():
    partial_manifest = b'{"kind":"partial"'
    fs = SdFileSystem(
        files={
            DESKOS_MANIFEST_PATH: DESKOS_MANIFEST_PAYLOAD,
            DESKOS_MAP_MANIFEST_PATH: partial_manifest,
            DESKOS_MAP_MANIFEST_TMP_PATH: b"stale",
        }
    )

    mounted = parse_tokens(reply_for_request(MOUNT_REQUEST, SCENARIOS["ready"], fs))
    invalid = replace(
        SCENARIOS["ready"],
        state="deskos_manifest_invalid",
        deskos=False,
        note="deskos_map_manifest_invalid",
    )
    stat = parse_tokens(
        reply_for_request(
            file_request(22, "stat", path=encode_path(DESKOS_MAP_MANIFEST_PATH)),
            invalid,
            fs,
        )
    )

    assert mounted["state"] == "deskos_manifest_invalid"
    assert mounted["deskos"] == "0"
    assert mounted["file_ops"] == "0"
    assert mounted["note"] == "deskos_map_manifest_invalid"
    assert DESKOS_MAP_MANIFEST_PATH not in fs.files
    assert fs.files[DESKOS_MAP_MANIFEST_BAD_PATH] == partial_manifest
    assert DESKOS_MAP_MANIFEST_TMP_PATH not in fs.files
    assert stat["ok"] == "0"
    assert stat["err"] == "not_ready"


def test_non_fat32_mounted_card_blocks_file_ops_before_deskos_prepare():
    fs = SdFileSystem()
    mounted = parse_tokens(reply_for_request(MOUNT_REQUEST, SCENARIOS["fat16-mounted"], fs))
    stat = parse_tokens(
        reply_for_request(
            file_request(23, "stat", path=encode_path(DESKOS_MANIFEST_PATH)),
            SCENARIOS["fat16-mounted"],
            fs,
        )
    )

    assert mounted["state"] == "not_fat32_or_unmountable"
    assert mounted["note"] == "needs_fat32_on_computer"
    assert mounted["fs"] == "fat16"
    assert mounted["deskos"] == "0"
    assert mounted["file_ops"] == "0"
    assert stat["ok"] == "0"
    assert stat["err"] == "not_ready"
    assert DESKOS_MANIFEST_PATH not in fs.files
    assert DESKOS_MAP_MANIFEST_PATH not in fs.files


def test_storage_edge_scenarios_and_constants_match_c_contract():
    root_missing = parse_tokens(reply_for_request(STATUS_REQUEST, SCENARIOS["root-missing"]))

    assert root_missing["state"] == "creating_deskos_files"
    assert root_missing["present"] == "1"
    assert root_missing["mounted"] == "1"
    assert root_missing["deskos"] == "0"
    assert root_missing["needs_fat32"] == "0"

    for scenario in SCENARIOS.values():
        assert " " not in scenario.note

    c_header = (ROOT / "main/hal/rp2040_bridge.h").read_text(encoding="utf-8")
    c_source = (ROOT / "main/hal/rp2040_bridge.c").read_text(encoding="utf-8")
    rp2040_sketch = (
        ROOT / "firmware/rp2040_sd_bridge/deskos_sd_bridge/deskos_sd_bridge.ino"
    ).read_text(encoding="utf-8")
    assert STATUS_REQUEST in c_source
    assert MOUNT_REQUEST in c_source
    assert PING_REQUEST in c_source
    assert STATUS_REQUEST in rp2040_sketch
    assert MOUNT_REQUEST in rp2040_sketch
    assert PING_REQUEST in rp2040_sketch
    assert "d1l_rp2040_bridge_file_create" in c_header
    assert "d1l_rp2040_bridge_file_create" in c_source
    assert "op=create" in c_source
    assert "O_WRONLY | O_CREAT | O_EXCL" in rp2040_sketch
    old_request = "DESKOS_SD_" + "FORMAT"
    old_confirmation = "FORMAT-" + "DESKOS-SD"
    assert old_request not in c_source
    assert old_confirmation not in c_header
    assert old_request not in rp2040_sketch
    assert old_confirmation not in rp2040_sketch
    assert "FatFormatter" not in rp2040_sketch
    for field in STATUS_FIELDS:
        assert f"{field}=" in rp2040_sketch
    for field in FILE_CAPABILITY_FIELDS:
        assert f"{field}=" in rp2040_sketch


def file_tokens(line: str) -> dict[str, str]:
    parsed = parse_tokens(line)
    assert parsed["prefix"] == FILE_REPLY
    return parsed


def file_request(request_id: int, op: str, **tokens: object) -> str:
    merged = {"v": 1, "id": request_id, "op": op, **tokens}
    return " ".join([FILE_REQUEST, *(f"{key}={value}" for key, value in merged.items())])


def test_file_protocol_create_is_atomic_and_never_clobbers():
    fs = SdFileSystem()
    relative = "map/offline-provider.stage-rc1-001.json"
    path = encode_path(relative)
    original = b"first-stage"
    replacement = b"must-not-replace"

    created = file_tokens(
        reply_for_request(
            file_request(
                60,
                "create",
                path=path,
                len=len(original),
                data=b64url(original),
                crc=crc32_hex(original),
            ),
            SCENARIOS["ready"],
            fs,
        )
    )
    collision = file_tokens(
        reply_for_request(
            file_request(
                61,
                "create",
                path=path,
                len=len(replacement),
                data=b64url(replacement),
                crc=crc32_hex(replacement),
            ),
            SCENARIOS["ready"],
            fs,
        )
    )

    assert created["ok"] == "1"
    assert created["op"] == "create"
    assert created["off"] == "0"
    assert created["size"] == str(len(original))
    assert collision["ok"] == "0"
    assert collision["op"] == "create"
    assert collision["err"] == "exists"
    assert fs.files[relative] == original


def test_file_protocol_supports_bounded_write_read_rename_delete_cycle():
    fs = SdFileSystem()
    path = encode_path("logs/packet.bin")
    tmp = encode_path("logs/packet.tmp")
    final = encode_path("logs/packet.final")
    data = b"packet-log-canary"
    more = b"-append"

    write = file_tokens(
        reply_for_request(
            file_request(
                7,
                "write",
                path=tmp,
                off=0,
                len=len(data),
                trunc=1,
                data=b64url(data),
                crc=crc32_hex(data),
            ),
            SCENARIOS["ready"],
            fs,
        )
    )
    append = file_tokens(
        reply_for_request(
            file_request(
                8,
                "append",
                path=tmp,
                len=len(more),
                data=b64url(more),
                crc=crc32_hex(more),
            ),
            SCENARIOS["ready"],
            fs,
        )
    )
    rename = file_tokens(
        reply_for_request(
            file_request(9, "rename", path=tmp, to=final, replace=1),
            SCENARIOS["ready"],
            fs,
        )
    )
    stat = file_tokens(
        reply_for_request(file_request(10, "stat", path=final), SCENARIOS["ready"], fs)
    )
    read = file_tokens(
        reply_for_request(
            file_request(11, "read", path=final, off=0, len=MAX_FILE_CHUNK_BYTES),
            SCENARIOS["ready"],
            fs,
        )
    )
    delete = file_tokens(
        reply_for_request(file_request(12, "delete", path=final), SCENARIOS["ready"], fs)
    )
    deleted = file_tokens(
        reply_for_request(file_request(13, "stat", path=final), SCENARIOS["ready"], fs)
    )

    assert write["ok"] == "1"
    assert write["op"] == "write"
    assert write["size"] == str(len(data))
    assert append["ok"] == "1"
    assert append["off"] == str(len(data))
    assert rename["ok"] == "1"
    assert stat["exists"] == "1"
    assert stat["kind"] == "file"
    assert stat["size"] == str(len(data + more))
    assert read["data"] == b64url(data + more)
    assert read["crc"] == crc32_hex(data + more)
    assert delete["ok"] == "1"
    assert deleted["exists"] == "0"
    assert path != final


def test_file_protocol_supports_max_path_atomic_rename_delete_cycle():
    fs = SdFileSystem()
    final_relative = f"maxpath/{'f' * 84}.bin"
    tmp_relative = f"maxpath/{'t' * 84}.tmp"
    assert len(final_relative) == MAX_FILE_PATH_CHARS
    assert len(tmp_relative) == MAX_FILE_PATH_CHARS

    final = encode_path(final_relative)
    tmp = encode_path(tmp_relative)
    old_payload = b"old-final"
    payload = b"max-path-retained-commit"
    fs.files[final_relative] = old_payload

    write = file_tokens(
        reply_for_request(
            file_request(
                70,
                "write",
                path=tmp,
                off=0,
                len=len(payload),
                trunc=1,
                data=b64url(payload),
                crc=crc32_hex(payload),
            ),
            SCENARIOS["ready"],
            fs,
        )
    )
    rename = file_tokens(
        reply_for_request(
            file_request(71, "rename", path=tmp, to=final, replace=1),
            SCENARIOS["ready"],
            fs,
        )
    )
    read = file_tokens(
        reply_for_request(
            file_request(72, "read", path=final, off=0, len=MAX_FILE_CHUNK_BYTES),
            SCENARIOS["ready"],
            fs,
        )
    )
    delete = file_tokens(
        reply_for_request(file_request(73, "delete", path=final), SCENARIOS["ready"], fs)
    )
    deleted = file_tokens(
        reply_for_request(file_request(74, "stat", path=final), SCENARIOS["ready"], fs)
    )

    assert write["ok"] == "1"
    assert rename["ok"] == "1"
    assert read["ok"] == "1"
    assert read["data"] == b64url(payload)
    assert read["data"] != b64url(old_payload)
    assert delete["ok"] == "1"
    assert deleted["exists"] == "0"
    assert tmp_relative not in fs.files
    assert final_relative not in fs.files


def test_file_protocol_replace_preserves_final_and_zero_length_write_policy():
    fs = SdFileSystem(files={"logs/final.bin": b"old-final"})
    tmp = encode_path("logs/tmp.bin")
    final = encode_path("logs/final.bin")
    zero = b""
    new = b"new-final"

    zero_write = file_tokens(
        reply_for_request(
            file_request(
                30,
                "write",
                path=tmp,
                off=0,
                len=0,
                trunc=1,
                data=b64url(zero),
                crc=crc32_hex(zero),
            ),
            SCENARIOS["ready"],
            fs,
        )
    )
    rewrite = file_tokens(
        reply_for_request(
            file_request(
                31,
                "write",
                path=tmp,
                off=0,
                len=len(new),
                trunc=1,
                data=b64url(new),
                crc=crc32_hex(new),
            ),
            SCENARIOS["ready"],
            fs,
        )
    )
    replace = file_tokens(
        reply_for_request(
            file_request(32, "rename", path=tmp, to=final, replace=1),
            SCENARIOS["ready"],
            fs,
        )
    )
    read = file_tokens(
        reply_for_request(
            file_request(33, "read", path=final, off=0, len=MAX_FILE_CHUNK_BYTES),
            SCENARIOS["ready"],
            fs,
        )
    )
    empty_append = file_tokens(
        reply_for_request(
            file_request(
                34,
                "append",
                path=tmp,
                len=0,
                data=b64url(zero),
                crc=crc32_hex(zero),
            ),
            SCENARIOS["ready"],
            fs,
        )
    )

    assert zero_write["ok"] == "1"
    assert zero_write["size"] == "0"
    assert rewrite["ok"] == "1"
    assert replace["ok"] == "1"
    assert read["data"] == b64url(new)
    assert empty_append["ok"] == "0"
    assert empty_append["err"] == "bad_value"


def test_filecanary_transcript_matches_storage_filecanary_contract():
    transcript = file_canary_transcript(SCENARIOS["ready"])
    requests = [exchange["request"] for exchange in transcript]
    replies = [parse_tokens(exchange["reply"]) for exchange in transcript]
    tmp_path = encode_path(FILE_CANARY_TMP_PATH)
    final_path = encode_path(FILE_CANARY_FINAL_PATH)
    payload64 = b64url(FILE_CANARY_PAYLOAD)
    payload_crc = crc32_hex(FILE_CANARY_PAYLOAD)

    assert len(transcript) == 9
    assert requests[0] == file_request(1, "delete", path=tmp_path)
    assert replies[0]["ok"] == "0"
    assert replies[0]["err"] == "not_found"
    assert requests[1] == file_request(2, "delete", path=final_path)
    assert replies[1]["err"] == "not_found"
    assert requests[2] == file_request(
        3,
        "write",
        path=tmp_path,
        off=0,
        len=len(FILE_CANARY_PAYLOAD),
        trunc=1,
        data=payload64,
        crc=payload_crc,
    )
    assert replies[2]["ok"] == "1"
    assert replies[2]["size"] == str(len(FILE_CANARY_PAYLOAD))
    assert requests[3] == file_request(4, "read", path=tmp_path, off=0, len=24)
    assert replies[3]["data"] == payload64
    assert replies[3]["crc"] == payload_crc
    assert requests[4] == file_request(5, "rename", path=tmp_path, to=final_path, replace=1)
    assert replies[4]["ok"] == "1"
    assert requests[5] == file_request(6, "stat", path=final_path)
    assert replies[5]["exists"] == "1"
    assert replies[5]["kind"] == "file"
    assert requests[6] == file_request(7, "read", path=final_path, off=0, len=24)
    assert replies[6]["data"] == payload64
    assert requests[7] == file_request(8, "delete", path=final_path)
    assert replies[7]["ok"] == "1"
    assert requests[8] == file_request(9, "stat", path=final_path)
    assert replies[8]["exists"] == "0"


def test_filecanary_transcript_fails_safely_without_ready_card():
    for scenario_name, expected_error in {
        "no-card": "no_card",
        "needs-fat32": "not_ready",
        "root-missing": "not_ready",
    }.items():
        transcript = file_canary_transcript(SCENARIOS[scenario_name])
        replies = [parse_tokens(exchange["reply"]) for exchange in transcript]
        assert {reply["ok"] for reply in replies} == {"0"}
        assert {reply["err"] for reply in replies} == {expected_error}


def test_export_canary_transcript_commits_diagnostic_export_without_delete():
    token = EXPORT_CANARY_DEFAULT_TOKEN
    transcript = export_canary_transcript(SCENARIOS["ready"], token)
    requests = [exchange["request"] for exchange in transcript]
    replies = [parse_tokens(exchange["reply"]) for exchange in transcript]
    tmp_path, final_path = export_canary_paths(token)
    tmp = encode_path(tmp_path)
    final = encode_path(final_path)
    payload = export_canary_payload(token)
    payload64 = b64url(payload)
    payload_crc = crc32_hex(payload)

    assert len(transcript) == 6
    assert requests[0] == file_request(EXPORT_CANARY_START_ID, "delete", path=tmp)
    assert replies[0]["ok"] == "0"
    assert replies[0]["err"] == "not_found"
    assert requests[1] == file_request(
        EXPORT_CANARY_START_ID + 1,
        "write",
        path=tmp,
        off=0,
        len=len(payload),
        trunc=1,
        data=payload64,
        crc=payload_crc,
    )
    assert replies[1]["ok"] == "1"
    assert requests[2] == file_request(
        EXPORT_CANARY_START_ID + 2,
        "read",
        path=tmp,
        off=0,
        len=len(payload),
    )
    assert replies[2]["data"] == payload64
    assert requests[3] == file_request(
        EXPORT_CANARY_START_ID + 3,
        "rename",
        path=tmp,
        to=final,
        replace=1,
    )
    assert replies[3]["ok"] == "1"
    assert requests[4] == file_request(EXPORT_CANARY_START_ID + 4, "stat", path=final)
    assert replies[4]["exists"] == "1"
    assert replies[4]["kind"] == "file"
    assert requests[5] == file_request(
        EXPORT_CANARY_START_ID + 5,
        "read",
        path=final,
        off=0,
        len=len(payload),
    )
    assert replies[5]["data"] == payload64
    assert not any(" op=delete " in request and final in request for request in requests[1:])
    assert b'"public_rf_tx":false' in payload
    assert b'"formats_sd":false' in payload


def test_export_canary_transcript_fails_safely_without_ready_card():
    for scenario_name, expected_error in {
        "no-card": "no_card",
        "needs-fat32": "not_ready",
        "root-missing": "not_ready",
    }.items():
        transcript = export_canary_transcript(SCENARIOS[scenario_name], "ex1")
        replies = [parse_tokens(exchange["reply"]) for exchange in transcript]
        assert {reply["ok"] for reply in replies} == {"0"}
        assert {reply["err"] for reply in replies} == {expected_error}


def test_diagnostic_export_transcript_chunks_and_commits_without_delete():
    token = DIAGNOSTIC_EXPORT_DEFAULT_TOKEN
    transcript = diagnostic_export_transcript(SCENARIOS["ready"], token)
    requests = [exchange["request"] for exchange in transcript]
    replies = [parse_tokens(exchange["reply"]) for exchange in transcript]
    tmp_path, final_path = diagnostic_export_paths(token)
    tmp = encode_path(tmp_path)
    final = encode_path(final_path)
    payload = diagnostic_export_payload(token)
    chunk_count = (len(payload) + MAX_FILE_CHUNK_BYTES - 1) // MAX_FILE_CHUNK_BYTES

    assert len(payload) > MAX_FILE_CHUNK_BYTES
    assert len(transcript) == 3 + (chunk_count * 3)
    assert requests[0] == file_request(DIAGNOSTIC_EXPORT_START_ID, "delete", path=tmp)
    assert replies[0]["ok"] == "0"
    assert replies[0]["err"] == "not_found"

    write_requests = requests[1 : 1 + chunk_count]
    write_replies = replies[1 : 1 + chunk_count]
    for index, (request, reply) in enumerate(zip(write_requests, write_replies)):
        offset = index * MAX_FILE_CHUNK_BYTES
        chunk = payload[offset : offset + MAX_FILE_CHUNK_BYTES]
        assert request == file_request(
            DIAGNOSTIC_EXPORT_START_ID + 1 + index,
            "write",
            path=tmp,
            off=offset,
            len=len(chunk),
            trunc=1 if index == 0 else 0,
            data=b64url(chunk),
            crc=crc32_hex(chunk),
        )
        assert reply["ok"] == "1"

    tmp_read_start = 1 + chunk_count
    tmp_read_requests = requests[tmp_read_start : tmp_read_start + chunk_count]
    tmp_read_replies = replies[tmp_read_start : tmp_read_start + chunk_count]
    for index, (request, reply) in enumerate(zip(tmp_read_requests, tmp_read_replies)):
        offset = index * MAX_FILE_CHUNK_BYTES
        chunk = payload[offset : offset + MAX_FILE_CHUNK_BYTES]
        assert request == file_request(
            DIAGNOSTIC_EXPORT_START_ID + 1 + chunk_count + index,
            "read",
            path=tmp,
            off=offset,
            len=len(chunk),
        )
        assert reply["data"] == b64url(chunk)

    rename_index = 1 + (chunk_count * 2)
    assert requests[rename_index] == file_request(
        DIAGNOSTIC_EXPORT_START_ID + 1 + (chunk_count * 2),
        "rename",
        path=tmp,
        to=final,
        replace=1,
    )
    assert replies[rename_index]["ok"] == "1"
    assert requests[rename_index + 1] == file_request(
        DIAGNOSTIC_EXPORT_START_ID + 2 + (chunk_count * 2),
        "stat",
        path=final,
    )
    assert replies[rename_index + 1]["exists"] == "1"
    assert replies[rename_index + 1]["size"] == str(len(payload))

    final_read_requests = requests[rename_index + 2 :]
    final_read_replies = replies[rename_index + 2 :]
    for index, (request, reply) in enumerate(zip(final_read_requests, final_read_replies)):
        offset = index * MAX_FILE_CHUNK_BYTES
        chunk = payload[offset : offset + MAX_FILE_CHUNK_BYTES]
        assert request == file_request(
            DIAGNOSTIC_EXPORT_START_ID + 3 + (chunk_count * 2) + index,
            "read",
            path=final,
            off=offset,
            len=len(chunk),
        )
        assert reply["data"] == b64url(chunk)
    assert not any(" op=delete " in request and final in request for request in requests)
    assert b'"kind":"diagnostic_export"' in payload
    assert b'"limits"' in payload
    assert b'"map_tiles":{"exported":false,"cache_ready":true' in payload
    assert b'"backend":"sd_map_tiles_ready"' in payload
    assert b'"reason":"diagnostic_export_does_not_bundle_map_tiles"' in payload


def test_diagnostic_export_transcript_fails_safely_without_ready_card():
    for scenario_name, expected_error in {
        "no-card": "no_card",
        "needs-fat32": "not_ready",
        "root-missing": "not_ready",
    }.items():
        transcript = diagnostic_export_transcript(SCENARIOS[scenario_name], "diag1")
        replies = [parse_tokens(exchange["reply"]) for exchange in transcript]
        assert {reply["ok"] for reply in replies} == {"0"}
        assert {reply["err"] for reply in replies} == {expected_error}


def test_data_export_transcript_chunks_and_commits_without_delete():
    token = DATA_EXPORT_DEFAULT_TOKEN
    transcript = data_export_transcript(SCENARIOS["ready"], token)
    requests = [exchange["request"] for exchange in transcript]
    replies = [parse_tokens(exchange["reply"]) for exchange in transcript]
    tmp_path, final_path = data_export_paths(token)
    tmp = encode_path(tmp_path)
    final = encode_path(final_path)
    payload = data_export_payload(token)
    chunk_count = (len(payload) + MAX_FILE_CHUNK_BYTES - 1) // MAX_FILE_CHUNK_BYTES

    assert tmp_path == "exports/data/data-export-data1.tmp"
    assert final_path == "exports/data/data-export-data1.json"
    assert len(payload) > MAX_FILE_CHUNK_BYTES
    assert len(transcript) == 3 + (chunk_count * 3)
    assert requests[0] == file_request(DATA_EXPORT_START_ID, "delete", path=tmp)
    assert replies[0]["ok"] == "0"
    assert replies[0]["err"] == "not_found"

    write_requests = requests[1 : 1 + chunk_count]
    write_replies = replies[1 : 1 + chunk_count]
    for index, (request, reply) in enumerate(zip(write_requests, write_replies)):
        offset = index * MAX_FILE_CHUNK_BYTES
        chunk = payload[offset : offset + MAX_FILE_CHUNK_BYTES]
        assert request == file_request(
            DATA_EXPORT_START_ID + 1 + index,
            "write",
            path=tmp,
            off=offset,
            len=len(chunk),
            trunc=1 if index == 0 else 0,
            data=b64url(chunk),
            crc=crc32_hex(chunk),
        )
        assert reply["ok"] == "1"

    tmp_read_start = 1 + chunk_count
    tmp_read_requests = requests[tmp_read_start : tmp_read_start + chunk_count]
    tmp_read_replies = replies[tmp_read_start : tmp_read_start + chunk_count]
    for index, (request, reply) in enumerate(zip(tmp_read_requests, tmp_read_replies)):
        offset = index * MAX_FILE_CHUNK_BYTES
        chunk = payload[offset : offset + MAX_FILE_CHUNK_BYTES]
        assert request == file_request(
            DATA_EXPORT_START_ID + 1 + chunk_count + index,
            "read",
            path=tmp,
            off=offset,
            len=len(chunk),
        )
        assert reply["data"] == b64url(chunk)

    rename_index = 1 + (chunk_count * 2)
    assert requests[rename_index] == file_request(
        DATA_EXPORT_START_ID + 1 + (chunk_count * 2),
        "rename",
        path=tmp,
        to=final,
        replace=1,
    )
    assert replies[rename_index]["ok"] == "1"
    assert requests[rename_index + 1] == file_request(
        DATA_EXPORT_START_ID + 2 + (chunk_count * 2),
        "stat",
        path=final,
    )
    assert replies[rename_index + 1]["exists"] == "1"
    assert replies[rename_index + 1]["size"] == str(len(payload))

    final_read_requests = requests[rename_index + 2 :]
    final_read_replies = replies[rename_index + 2 :]
    for index, (request, reply) in enumerate(zip(final_read_requests, final_read_replies)):
        offset = index * MAX_FILE_CHUNK_BYTES
        chunk = payload[offset : offset + MAX_FILE_CHUNK_BYTES]
        assert request == file_request(
            DATA_EXPORT_START_ID + 3 + (chunk_count * 2) + index,
            "read",
            path=final,
            off=offset,
            len=len(chunk),
        )
        assert reply["data"] == b64url(chunk)
    assert not any(" op=delete " in request and final in request for request in requests)
    assert b'"kind":"data_export"' in payload
    assert b'"sampled":true' in payload
    assert b'"private_identity_exported":false' in payload
    assert b'"messages"' in payload
    assert b'"dm"' in payload
    assert b'"routes"' in payload
    assert b'"packets"' in payload
    assert b'"contacts"' in payload
    assert b'"nodes"' in payload
    assert b'"read_state"' in payload


def test_data_export_transcript_fails_safely_without_ready_card():
    for scenario_name, expected_error in {
        "no-card": "no_card",
        "needs-fat32": "not_ready",
        "root-missing": "not_ready",
    }.items():
        transcript = data_export_transcript(SCENARIOS[scenario_name], "data1")
        replies = [parse_tokens(exchange["reply"]) for exchange in transcript]
        assert {reply["ok"] for reply in replies} == {"0"}
        assert {reply["err"] for reply in replies} == {expected_error}


def test_map_tile_canary_transcript_commits_nested_cache_tile():
    token = MAP_TILE_CANARY_DEFAULT_TOKEN
    transcript = map_tile_canary_transcript(SCENARIOS["ready"], token)
    requests = [exchange["request"] for exchange in transcript]
    replies = [parse_tokens(exchange["reply"]) for exchange in transcript]
    tmp, final = map_tile_canary_paths(token)
    payload = map_tile_canary_payload(token)

    assert tmp == "map/tiles/z12/x1/y2-map1.tmp"
    assert final == "map/tiles/z12/x1/y2-map1.tile"
    assert validate_relative_path(tmp)
    assert validate_relative_path(final)
    assert payload.startswith(b'{"schema":1,"kind":"map_tile_cache_canary"')
    assert requests == [
        file_request(
            MAP_TILE_CANARY_START_ID,
            "delete",
            path=encode_path(tmp),
        ),
        file_request(
            MAP_TILE_CANARY_START_ID + 1,
            "write",
            path=encode_path(tmp),
            off=0,
            len=len(payload),
            trunc=1,
            data=b64url(payload),
            crc=crc32_hex(payload),
        ),
        file_request(
            MAP_TILE_CANARY_START_ID + 2,
            "read",
            path=encode_path(tmp),
            off=0,
            len=len(payload),
        ),
        file_request(
            MAP_TILE_CANARY_START_ID + 3,
            "rename",
            path=encode_path(tmp),
            to=encode_path(final),
            replace=1,
        ),
        file_request(
            MAP_TILE_CANARY_START_ID + 4,
            "stat",
            path=encode_path(final),
        ),
        file_request(
            MAP_TILE_CANARY_START_ID + 5,
            "read",
            path=encode_path(final),
            off=0,
            len=len(payload),
        ),
    ]
    assert replies[0]["ok"] == "0"
    assert replies[0]["err"] == "not_found"
    assert all(reply["ok"] == "1" for reply in replies[1:])
    assert replies[2]["data"] == b64url(payload)
    assert replies[4]["exists"] == "1"
    assert replies[4]["size"] == str(len(payload))
    assert replies[5]["data"] == b64url(payload)


def test_map_tile_canary_transcript_fails_safely_without_ready_card():
    for scenario_name, expected_error in {
        "no-card": "no_card",
        "needs-fat32": "not_ready",
        "root-missing": "not_ready",
    }.items():
        transcript = map_tile_canary_transcript(SCENARIOS[scenario_name], "map1")
        replies = [parse_tokens(exchange["reply"]) for exchange in transcript]
        assert {reply["ok"] for reply in replies} == {"0"}
        assert {reply["err"] for reply in replies} == {expected_error}


def test_map_tile_check_transcript_is_read_only_after_remount():
    token = MAP_TILE_CANARY_DEFAULT_TOKEN
    transcript = map_tile_check_transcript(SCENARIOS["ready"], token)
    requests = [exchange["request"] for exchange in transcript]
    replies = [parse_tokens(exchange["reply"]) for exchange in transcript]
    _tmp, final = map_tile_canary_paths(token)
    payload = map_tile_canary_payload(token)

    assert requests == [
        file_request(MAP_TILE_CANARY_START_ID + 20, "stat", path=encode_path(final)),
        file_request(
            MAP_TILE_CANARY_START_ID + 21,
            "read",
            path=encode_path(final),
            off=0,
            len=len(payload),
        ),
    ]
    assert replies[0]["ok"] == "1"
    assert replies[0]["exists"] == "1"
    assert replies[0]["kind"] == "file"
    assert replies[0]["size"] == str(len(payload))
    assert replies[1]["ok"] == "1"
    assert replies[1]["data"] == b64url(payload)
    assert all(" op=write " not in request for request in requests)
    assert all(" op=rename " not in request for request in requests)
    assert all(" op=delete " not in request for request in requests)


def test_retained_history_blob_paths_support_atomic_commit_cycle():
    retained_paths = (
        "stores/messages/public/public",
        "stores/messages/dm/threads",
        "stores/routes/routes",
        "stores/packet_log/ring",
    )
    payload = b"retained-history-blob-v1"

    for index, base_path in enumerate(retained_paths, start=40):
        fs = SdFileSystem()
        tmp = encode_path(f"{base_path}.tmp")
        final = encode_path(f"{base_path}.bin")
        write = file_tokens(
            reply_for_request(
                file_request(
                    index,
                    "write",
                    path=tmp,
                    off=0,
                    len=len(payload),
                    trunc=1,
                    data=b64url(payload),
                    crc=crc32_hex(payload),
                ),
                SCENARIOS["ready"],
                fs,
            )
        )
        rename = file_tokens(
            reply_for_request(
                file_request(index + 100, "rename", path=tmp, to=final, replace=1),
                SCENARIOS["ready"],
                fs,
            )
        )
        stat = file_tokens(
            reply_for_request(
                file_request(index + 200, "stat", path=final),
                SCENARIOS["ready"],
                fs,
            )
        )
        read = file_tokens(
            reply_for_request(
                file_request(index + 300, "read", path=final, off=0, len=len(payload)),
                SCENARIOS["ready"],
                fs,
            )
        )

        assert validate_relative_path(f"{base_path}.tmp")
        assert validate_relative_path(f"{base_path}.bin")
        assert write["ok"] == "1"
        assert rename["ok"] == "1"
        assert stat["exists"] == "1"
        assert stat["size"] == str(len(payload))
        assert read["data"] == b64url(payload)
        assert read["crc"] == crc32_hex(payload)


def test_file_protocol_rejects_bad_paths_crc_and_large_chunks():
    fs = SdFileSystem()
    safe_path = encode_path("logs/packet.bin")
    data = b"x"
    bad_crc = file_tokens(
        reply_for_request(
            file_request(
                21,
                "write",
                path=safe_path,
                off=0,
                len=len(data),
                trunc=1,
                data=b64url(data),
                crc="00000000",
            ),
            SCENARIOS["ready"],
            fs,
        )
    )
    too_large = file_tokens(
        reply_for_request(
            file_request(22, "read", path=safe_path, off=0, len=MAX_FILE_CHUNK_BYTES + 1),
            SCENARIOS["ready"],
            fs,
        )
    )
    not_ready = file_tokens(
        reply_for_request(file_request(23, "stat", path=safe_path), SCENARIOS["no-card"], fs)
    )

    assert bad_crc["ok"] == "0"
    assert bad_crc["err"] == "crc_mismatch"
    assert too_large["ok"] == "0"
    assert too_large["err"] == "too_large"
    assert not_ready["ok"] == "0"
    assert not_ready["err"] == "no_card"
    assert not validate_relative_path("../escape")
    assert not validate_relative_path("/absolute")
    assert not validate_relative_path("tiles//gap")
    assert not validate_relative_path("tiles/.")
    assert validate_relative_path("tiles/z12/x1/y2.bin")
    assert FILE_LINE_MAX == 512
    assert MAX_FILE_CHUNK_BYTES == 192
    assert "crc_mismatch" in FILE_ERROR_CODES


def test_reference_transcripts_stay_inside_line_cap():
    transcripts = [
        file_canary_transcript(SCENARIOS["ready"]),
        export_canary_transcript(SCENARIOS["ready"], "export1"),
        diagnostic_export_transcript(SCENARIOS["ready"], "diag1"),
        data_export_transcript(SCENARIOS["ready"], "data1"),
        map_tile_canary_transcript(SCENARIOS["ready"], "map1"),
    ]

    for transcript in transcripts:
        for exchange in transcript:
            assert len(exchange["request"]) <= FILE_LINE_MAX
            assert len(exchange["reply"]) <= FILE_LINE_MAX


def test_unknown_request_is_rejected():
    try:
        reply_for_request("DESKOS_SD_ERASE", SCENARIOS["ready"])
    except ValueError as exc:
        assert "unsupported request" in str(exc)
    else:
        raise AssertionError("unknown request should raise ValueError")
