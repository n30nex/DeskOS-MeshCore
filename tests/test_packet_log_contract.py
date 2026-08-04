from pathlib import Path

from scripts.smoke_d1l import SMOKE_COMMANDS


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_packet_log_is_bounded_and_retained_blob_store_backed():
    header = read("main/mesh/packet_log.h")
    source = read("main/mesh/packet_log.c")
    app_main = read("main/app_main.c")
    blob_store = read("main/storage/retained_blob_store.c")
    cmake = read("main/CMakeLists.txt")
    assert "D1L_PACKET_LOG_RAM_CAPACITY 128U" in header
    assert "D1L_PACKET_LOG_NVS_FALLBACK_CAPACITY 8U" in header
    assert "D1L_PACKET_LOG_SD_RETENTION_HOURS 24U" in header
    assert "D1L_PACKET_LOG_SD_CAPACITY 4096U" in header
    assert "D1L_PACKET_LOG_SD_SEGMENT_COUNT 64U" in header
    assert "D1L_PACKET_LOG_SD_SEGMENT_CAPACITY" in header
    assert "D1L_PACKET_LOG_CAPACITY D1L_PACKET_LOG_RAM_CAPACITY" in header
    assert "D1L_PACKET_LOG_PERSIST_CAPACITY D1L_PACKET_LOG_NVS_FALLBACK_CAPACITY" in header
    assert "D1L_PACKET_LOG_SD_FLUSH_DIRTY_THRESHOLD 16U" in header
    assert "D1L_PACKET_LOG_SD_FLUSH_INTERVAL_MS 5000U" in header
    assert "sd_history_records" in header
    assert "sd_history_failed_writes" in header
    assert "sd_capacity" in header
    assert "sd_history_enabled" in header
    assert 'D1L_RETAINED_PACKET_LOG_NAMESPACE "d1l_packets"' in blob_store
    assert 'D1L_RETAINED_PACKET_LOG_SD_DIR "stores/packet_log"' in blob_store
    assert 'D1L_PACKET_LOG_KEY "ring"' in source
    assert "nvs_get_blob" in blob_store
    assert "nvs_set_blob" in blob_store
    assert "d1l_rp2040_bridge_file_write" in blob_store
    assert "d1l_rp2040_bridge_file_rename" in blob_store
    assert "nvs_get_blob" not in source
    assert "nvs_set_blob" not in source
    assert '#include "storage/retained_blob_store.h"' in source
    assert "d1l_retained_blob_store_read_sd_primary" in source
    assert "d1l_retained_blob_store_read_nvs_fallback" in source
    assert "d1l_retained_blob_store_write_sd_primary_guarded" in source
    assert "d1l_retained_blob_store_write_nvs_fallback" in source
    assert "d1l_retained_blob_store_write_split" not in source
    assert "d1l_retained_blob_store_erase_sd_primary_guarded" in source
    assert "d1l_retained_blob_store_erase_nvs_fallback" in source
    assert "d1l_retained_blob_store_uses_sd" in source
    assert "D1L_RETAINED_BLOB_STORE_PACKET_LOG" in source
    assert '"storage/retained_blob_store.c"' in cmake
    assert '"hal/rp2040_file_reply.c"' in cmake
    bridge_source = read("main/hal/rp2040_bridge.c")
    reply_source = read("main/hal/rp2040_file_reply.c")
    assert "d1l_rp2040_file_reply_parse(" in bridge_source
    assert "token_len >= key_len + 1U" in reply_source
    assert "data= is the canonical zero-byte EOF" in reply_source
    assert "static d1l_packet_log_primary_blob_t s_primary_blob_scratch" in source
    assert "static d1l_packet_log_fallback_blob_t s_fallback_blob_scratch" in source
    assert "D1L_PACKET_LOG_HISTORY_PATH_FMT \"stores/packet_log/segments/s%02lu.bin\"" in source
    assert "d1l_packet_log_history_record_t" in source
    assert "history_record_checksum" in source
    assert "history_record_is_valid" in source
    assert "read_sd_history_entry" in source
    assert "append_sd_history_for_generation" in source
    assert "clear_sd_history_for_generation" in source
    assert "d1l_rp2040_bridge_file_append" not in source
    assert "d1l_rp2040_bridge_file_read(path, offset" in source
    assert "const bool unwritten_eof = read_result.length == 0U" in source
    assert "read_result.eof" in source
    assert "read_result.offset == offset" in source
    assert "path, offset, (const uint8_t *)&record" in source
    assert "d1l_rp2040_bridge_file_delete" in source
    assert "D1L_PACKET_LOG_HISTORY_WRITE_TIMEOUT_MS 750U" in source
    assert "D1L_PACKET_LOG_HISTORY_MAX_CONSECUTIVE_MISSES 4U" in source
    assert "_Static_assert(sizeof(d1l_packet_log_history_record_t) <= D1L_RP2040_FILE_CHUNK_MAX" in source
    assert "D1L_PACKET_LOG_NVS_FALLBACK_CAPACITY" in source
    assert "D1L_PACKET_LOG_RAM_CAPACITY" in source
    assert "D1L_PACKET_LOG_SD_CAPACITY" in source
    assert "sanitize_ascii" in source
    assert "d1l_packet_log_find_by_seq" in header
    assert "d1l_packet_log_flush" in header
    assert "d1l_packet_log_flush_if_due" in header
    assert "D1L_PACKET_LOG_RAW_PREVIEW_BYTES 32U" in header
    assert "D1L_PACKET_LOG_RAW_HEX_LEN" in header
    assert "raw_hex" in header
    assert "raw_truncated" in header
    assert "d1l_packet_log_append_raw" in header
    assert "d1l_packet_log_query_page" in header
    assert "d1l_packet_log_query" in header
    assert "skip_newest" in source
    assert "out_total_matches" in source
    assert "out_sd_used" in source
    assert "query_sd_history" in source
    assert "query_ram_locked" in source
    assert "raw_to_hex_preview" in source
    assert "packet_matches" in source
    assert "D1L_PACKET_LOG_SCHEMA 3U" in source
    assert "D1L_PACKET_LOG_SCHEMA_V2 2U" in source
    assert "fallback_blob_v2_is_valid" in source
    assert "persist_store(bool flush_primary, bool force)" in source
    assert "reconcile_sd_primary" in source
    assert "packet_backend_generation_matches" in source
    assert "s_sd_reconcile_pending" in source
    assert "s_sd_primary_dirty" in source
    assert "s_nvs_fallback_dirty" in source
    assert "s_journal_dirty" in source
    assert "persistence_revision" in header
    assert "sd_primary_reconcile_pending" in header
    assert "nvs_fallback_last_error" in header
    assert "journal_last_error" in header
    assert "clear_in_progress" in header
    assert "bool loaded" in header
    assert "s_clear_in_progress" in source
    assert "s_append_clear_lock" in source
    assert "s_init_recovery_lock" in source
    assert "ensure_packet_log_initialized" in source
    assert "entries[i].seq != first_seq + (uint32_t)i" in source
    assert "!same_revision || still_dirty" in source
    assert "packet_backend_generation_matches(expected_generation)" in source
    assert "static d1l_packet_log_primary_blob_t s_sd_primary_blob_scratch EXT_RAM_BSS_ATTR" in source
    assert "s_merge_entries[D1L_PACKET_LOG_RAM_CAPACITY * 2U] EXT_RAM_BSS_ATTR" in source
    init_body = source[
        source.index("esp_err_t d1l_packet_log_init(void)") :
        source.index("esp_err_t d1l_packet_log_clear(void)")
    ]
    assert "d1l_retained_blob_store_erase" not in init_body
    clear_body = source[
        source.index("esp_err_t d1l_packet_log_clear(void)") :
        source.index("bool d1l_packet_log_append(")
    ]
    assert clear_body.index("d1l_store_lock_take(&s_persist_io_lock)") < clear_body.index(
        "packet_backend_state(&backend_state)"
    )
    persist_body = source[
        source.index("static esp_err_t persist_store(bool flush_primary, bool force)\n{") :
        source.index("esp_err_t d1l_packet_log_init(void)")
    ]
    assert persist_body.index("reconcile_sd_primary(") < persist_body.index(
        "flush_journal_for_generation("
    )
    journal_start = source.index("static esp_err_t flush_journal_for_generation")
    journal_flush = source[
        journal_start :
        source.index("static esp_err_t persist_store(bool flush_primary, bool force)", journal_start)
    ]
    assert "d1l_route_store_persistence_should_yield()" in journal_flush
    assert "return ESP_ERR_NOT_FINISHED;" in journal_flush
    assert persist_body.count("== ESP_ERR_NOT_FINISHED") >= 3
    assert persist_body.index("journal_ret == ESP_ERR_NOT_FINISHED") < persist_body.index(
        "s_journal_fail_count++"
    )
    assert "s_sd_dirty_count >= D1L_PACKET_LOG_SD_FLUSH_DIRTY_THRESHOLD" in source
    assert "s_sd_history_records < D1L_PACKET_LOG_SD_CAPACITY" in source
    assert "ESP_ERR_NOT_FOUND" in source
    assert '\\"packet_log_backend\\"' in read("main/comms/usb_console.c")
    assert "esp_err_t packet_log_ret = d1l_packet_log_init()" in app_main


def test_console_exposes_packet_detail_and_clear():
    console = read("main/comms/usb_console.c")
    assert 'print_packet_entries_json("packets"' in console
    assert 'print_packet_entries_json("packets filter"' in console
    assert 'print_packet_entries_json("packets search"' in console
    assert 'ok_begin("packets detail")' in console
    assert 'ok_begin("packets raw")' in console
    assert 'ok_begin("packets clear")' in console
    assert "d1l_packet_log_find_by_seq" in console
    assert "d1l_packet_log_query" in console
    assert "d1l_packet_log_clear()" in console
    assert 'strncmp(line, "packets filter ", 15)' in console
    assert 'strncmp(line, "packets search ", 15)' in console
    assert 'strncmp(line, "packets detail ", 15)' in console
    assert 'strncmp(line, "packets raw ", 12)' in console
    assert 'strcmp(line, "packets clear")' in console
    assert "packets filter <any|rx|tx> <any|text|kind>" in console
    assert "packets search <text>" in console
    assert "packets detail <seq>" in console
    assert "packets raw <seq>" in console
    assert "packets clear" in console
    assert "stats->loaded && !stats->persistence_dirty" in console
    assert '\\"persistence\\":{\\"loaded\\":%s' in console
    assert '\\"raw_hex\\":\\"%s\\"' in console
    assert '\\"sd_history\\":{\\"enabled\\":%s' in console
    assert "stats->sd_history_records" in console
    assert "stats->sd_history_failed_writes" in console
    assert "packets" in SMOKE_COMMANDS
    assert "packets filter any any" in SMOKE_COMMANDS
    assert "packets search test" in SMOKE_COMMANDS
    assert "storage status" in SMOKE_COMMANDS


def test_filtered_packet_search_has_foreground_sd_priority():
    console = read("main/comms/usb_console.c")
    search = console.split("static void cmd_packets_search", 1)[1].split(
        "static void cmd_packets_detail", 1
    )[0]
    assert "stats.sd_history_enabled" in search
    assert "stats.sd_history_records > stats.count" in search
    assert "d1l_rp2040_bridge_quiesce_begin" in search
    assert "D1L_PACKET_QUERY_QUIESCE_TIMEOUT_MS" in search
    assert search.index("d1l_rp2040_bridge_quiesce_begin") < search.index(
        "d1l_packet_log_query"
    )
    assert search.index("d1l_packet_log_query") < search.index(
        "d1l_rp2040_bridge_quiesce_end"
    )
