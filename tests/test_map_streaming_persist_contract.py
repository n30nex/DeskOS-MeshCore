from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def function_body(source: str, marker: str) -> str:
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {marker}")


def test_stream_reply_parser_requires_and_binds_every_sequence_field():
    header = read("main/hal/rp2040_bridge.h")
    parser_header = read("main/hal/rp2040_file_reply.h")
    parser = read("main/hal/rp2040_file_reply.c")

    assert "uint32_t next_offset;" in header
    assert "bool removed;" in header
    for name in ("put_begin", "put_chunk", "put_end", "put_abort"):
        assert f"d1l_rp2040_file_reply_bind_{name}(" in parser_header
        assert f'd1l_rp2040_file_reply_bind_{name}(' in parser

    parse = function_body(parser, "esp_err_t d1l_rp2040_file_reply_parse(")
    begin = parse.index('strcmp(expected_op, "put_begin")')
    chunk = parse.index('strcmp(expected_op, "put_chunk")')
    end = parse.index('strcmp(expected_op, "put_end")')
    abort = parse.index('strcmp(expected_op, "put_abort")')
    assert begin < chunk < end < abort

    begin_section = parse[begin:chunk]
    assert 'parse_u32_token(line, "off", &result->offset)' in begin_section
    assert 'parse_u32_token(line, "size", &result->size)' in begin_section
    assert 'parse_hex_u32_token(line, "crc", &result->crc32)' in begin_section

    chunk_section = parse[chunk:end]
    assert 'parse_u32_token(line, "off", &result->offset)' in chunk_section
    assert 'parse_u32_token(line, "len", &result->length)' in chunk_section
    assert (
        'parse_u32_token(line, "next", &result->next_offset)'
        in chunk_section
    )

    end_section = parse[end:abort]
    assert 'parse_u32_token(line, "size", &result->size)' in end_section
    assert 'parse_hex_u32_token(line, "crc", &result->crc32)' in end_section

    abort_section = parse[abort:]
    assert 'parse_bool_token(line, "removed", &result->removed)' in abort_section


def test_bridge_verified_writer_is_one_bounded_fail_closed_session():
    header = read("main/hal/rp2040_bridge.h")
    bridge = read("main/hal/rp2040_bridge.c")
    parser = read("main/hal/rp2040_file_reply.c")

    assert "typedef bool (*d1l_rp2040_file_continue_cb_t)" in header
    assert "d1l_rp2040_bridge_file_write_verified(" in header
    writer = function_body(
        bridge, "esp_err_t d1l_rp2040_bridge_file_write_verified("
    )

    begin = writer.index('"put_begin"')
    chunk = writer.index('"put_chunk"')
    end = writer.index('"put_end"')
    assert begin < chunk < end
    assert "abort_verified_put_while_locked(" in writer
    abort = function_body(
        bridge, "static esp_err_t abort_verified_put_while_locked("
    )
    assert '"put_abort"' in abort
    assert "d1l_rp2040_file_reply_bind_put_abort(" in abort
    abort_bind = function_body(
        parser, "esp_err_t d1l_rp2040_file_reply_bind_put_abort("
    )
    assert "result->removed_known && result->removed" in abort_bind
    assert "D1L_RP2040_FILE_CHUNK_MAX" in writer
    assert "d1l_rp2040_file_reply_bind_put_begin(" in writer
    assert "d1l_rp2040_file_reply_bind_put_chunk(" in writer
    assert "d1l_rp2040_file_reply_bind_put_end(" in writer
    assert "should_continue" in writer
    assert "note_verified_put_cancelled(" in writer
    assert "take_bridge_lock(" in writer
    assert "give_bridge_lock();" in writer
    assert "d1l_rp2040_bridge_file_read(" not in writer


def test_terminal_cleanup_truth_suppresses_only_redundant_abort():
    header = read("main/hal/rp2040_bridge.h")
    parser = read("main/hal/rp2040_file_reply.c")
    bridge = read("main/hal/rp2040_bridge.c")

    assert "bool removed_known;" in header
    parse = function_body(parser, "esp_err_t d1l_rp2040_file_reply_parse(")
    remote_error = parse[
        parse.index("if (!result->ok)") :
        parse.index('if (strcmp(expected_op, "stat")')
    ]
    assert '"removed"' in remote_error
    assert "result->removed_known = true;" in remote_error

    confirmed = function_body(
        bridge, "static bool verified_put_cleanup_confirmed("
    )
    assert "result->removed_known && result->removed" in confirmed

    writer = function_body(
        bridge, "esp_err_t d1l_rp2040_bridge_file_write_verified("
    )
    assert writer.count(
        "verified_put_cleanup_confirmed(&step_result)"
    ) >= 3
    assert writer.count("abort_verified_put_while_locked(") == 1
    final_cleanup = writer[writer.index("verified_put_done:") :]
    assert "ret != ESP_OK && session_maybe_active" in final_cleanup


def test_ping_parses_and_exposes_stream_write_capability():
    header = read("main/hal/rp2040_bridge.h")
    bridge = read("main/hal/rp2040_bridge.c")
    console = read("main/comms/usb_console.c")

    assert "bool stream_write_supported;" in header
    ping_parser = function_body(
        bridge, "static esp_err_t parse_ping_line("
    )
    assert (
        'parse_bool_token(\n'
        '        line, "stream_write", &ping->stream_write_supported)'
        in ping_parser
    )
    ping_command = function_body(
        console, "static void cmd_rp2040_ping("
    )
    assert '\\"stream_write\\":%s' in ping_command
    assert "bool_json(ping.stream_write_supported)" in ping_command


def test_rp2040_stream_session_verifies_total_size_and_crc_before_success():
    sketch = read(
        "firmware/rp2040_sd_bridge/deskos_sd_bridge/deskos_sd_bridge.ino"
    )

    assert "FilePutSession s_file_put" in sketch
    assert "bool cleanup_required;" in sketch
    assert "stream_write=1" in sketch
    for handler in (
        "handle_file_put_begin",
        "handle_file_put_chunk",
        "handle_file_put_end",
        "handle_file_put_abort",
    ):
        assert handler in sketch
    for operation in ("put_begin", "put_chunk", "put_end", "put_abort"):
        assert f'"{operation}"' in sketch
    assert "no_session" in sketch
    assert "busy" in sketch

    end = function_body(sketch, "void handle_file_put_end(")
    assert "s_file_put.expected_size" in end
    assert "s_file_put.expected_crc" in end
    assert "actual_size != expected_size" in end
    assert "actual_crc != expected_crc" in end
    assert "crc32_update(" in end
    assert ".flush()" in end
    assert "close_file_put_session();" in end
    assert end.count("remove_file_put_target_and_release();") >= 4
    assert end.count("send_file_terminal_error(") >= 4

    chunk = function_body(sketch, "void handle_file_put_chunk(")
    assert chunk.count("remove_file_put_target_and_release();") >= 2
    assert chunk.count("send_file_terminal_error(") >= 2

    terminal_error = function_body(
        sketch, "void send_file_terminal_error("
    )
    assert '" ok=0 op="' in terminal_error
    assert '" removed="' in terminal_error

    abort = function_body(sketch, "void handle_file_put_abort(")
    assert "remove_file_put_target_and_release();" in abort
    assert "removed=" in abort

    cleanup = function_body(sketch, "bool remove_file_put_target_and_release(")
    assert "s_file_put.file.close();" in cleanup
    assert "SD.remove(s_file_put.full_path)" in cleanup
    assert "close_file_put_session();" in cleanup
    assert "s_file_put.cleanup_required = true;" in cleanup
    assert "s_file_put.active = true;" in cleanup

    route = function_body(sketch, "void handle_file_line(")
    assert "s_file_put.cleanup_required && !put_abort" in route
    assert '"abort_required"' in route


def test_map_candidate_stream_keeps_metadata_journal_and_atomic_commit_order():
    store = read("main/storage/map_tile_store.c")
    persist = function_body(store, "static esp_err_t persist_validated_tile(")
    commit = function_body(store, "static esp_err_t commit_cache_tile(")

    verified = persist.index("d1l_rp2040_bridge_file_write_verified(")
    checksum = persist.index("result->checksum_verified = true;", verified)
    attribution = persist.index("write_attribution_metadata(", checksum)
    commit_call = persist.index("commit_cache_tile(", attribution)
    assert verified < checksum < attribution < commit_call
    candidate = persist[verified:attribution]
    assert "d1l_rp2040_bridge_file_write(" not in candidate
    assert "verify_tile_file_continue(" not in candidate
    assert "cleanup_partial(result);" in persist
    assert "result->cache_intent_recorded" in persist

    metadata = commit.index("write_cache_metadata_tmp(")
    journal = commit.index("append_cache_intent(")
    tile_rename = commit.index("d1l_rp2040_bridge_file_rename(")
    metadata_rename = commit.index("rename_cache_metadata(")
    state = commit.index("write_cache_state(")
    assert metadata < journal < tile_rename < metadata_rename < state
