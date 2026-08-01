import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_tile_cache_policy_is_registered_and_wired_to_sd_store():
    cmake = read("main/CMakeLists.txt")
    store = read("main/storage/map_tile_store.c")
    provider = read("main/map/map_tile_provider.c")
    manifest = read("sdcard/offline-tile-provider.json")

    assert '"map/map_tile_cache_policy.c"' in cmake
    assert "d1l_map_tile_cache_state_has_room" in store
    assert "d1l_map_tile_cache_state_note_evict" in store
    assert "d1l_map_tile_cache_record_encode" in store
    assert "d1l_map_tile_cache_record_decode" in store
    assert "cache_budget_mb" in provider
    assert '"cache_budget_mb"' in manifest
    assert "d1l_rp2040_bridge_file_delete(tile_path" in store
    assert "d1l_rp2040_bridge_file_delete(metadata_path" in store


def test_interrupted_tile_commit_uses_checksum_and_atomic_recovery():
    store = read("main/storage/map_tile_store.c")

    assert "verify_tile_file" in store
    assert "d1l_map_tile_cache_crc32" in store
    assert "write_cache_metadata_tmp" in store
    assert "append_cache_intent" in store
    assert "recover_cache_tail" in store
    assert "d1l_map_tile_cache_recovery_plan" in store
    assert "rename_cache_metadata" in store
    assert store.index("append_cache_intent") < store.index(
        "rename_cache_metadata"
    )

    assert "repair_cache_journal" in store
    assert "rebuild_cache_journal_prefix" in store
    assert "d1l_map_tile_cache_journal_repair_plan" in store
    assert "reconcile_cache_intent" in store
    assert "s_cache_state_loaded = false;" in store


def test_store_serializes_transactions_and_evicts_only_validated_actual_size():
    store = read("main/storage/map_tile_store.c")

    assert "D1L_MAP_TILE_STORE_TRANSACTION_TIMEOUT_MS" in store
    assert "xSemaphoreCreateMutexStatic" in store
    assert store.count("cache_transaction_take()") >= 2
    assert "cache_transaction_take_cancelable(" in store
    assert store.count("cache_transaction_give()") >= 3

    fetch = store[
        store.index("static esp_err_t map_tile_store_fetch_network") :
        store.index("esp_err_t d1l_map_tile_store_fetch")
    ]
    assert "prepare_cache_room" not in fetch
    assert "cache_transaction_take" not in fetch
    assert "d1l_rp2040_bridge_file_write" not in fetch
    assert fetch.index("result.content_crc32 =") < fetch.index(
        "persist_validated_tile("
    )

    persist = store[
        store.index("static esp_err_t persist_validated_tile") :
        store.index("static esp_err_t map_tile_store_fetch_network")
    ]
    assert persist.index("cache_transaction_take") < persist.index(
        "d1l_rp2040_bridge_file_write"
    )

    commit = store[
        store.index("static esp_err_t commit_cache_tile") :
        store.index("static bool attribution_metadata_present")
    ]
    assert "prepare_cache_room(\n        provider, storage, result->bytes" in commit
    assert "D1L_MAP_TILE_DOWNLOAD_MAX_BYTES" not in commit


def test_cache_identity_uses_stable_sd_backend_generation():
    store = read("main/storage/map_tile_store.c")
    load = store[
        store.index("static esp_err_t load_cache_state_for_generation") :
        store.index("static esp_err_t prepare_cache_room")
    ]

    assert "s_cache_state_manager_attempt" not in store
    assert "storage->manager_attempt" not in load
    assert "cache_backend_generation_matches(backend_generation)" in load
    assert (
        "s_cache_state_backend_generation == backend_generation"
        in load
    )
    assert (
        "s_cache_state_backend_generation = backend_generation"
        in load
    )
    wrapper = load.split("static esp_err_t load_cache_state(", 1)[1]
    assert "cache_backend_generation(&backend_generation)" in wrapper
    assert "load_cache_state_for_generation(" in wrapper


def test_corrupt_cache_state_is_rebuilt_from_the_verified_journal():
    store = read("main/storage/map_tile_store.c")
    latest = store[
        store.index("static esp_err_t cache_record_is_latest_coordinate") :
        store.index("static esp_err_t recover_interrupted_record")
    ]
    recover = store[
        store.index("static esp_err_t recover_interrupted_record") :
        store.index("static esp_err_t rebuild_cache_journal_prefix")
    ]
    rebuild = store[
        store.index("static esp_err_t rebuild_cache_state_from_journal") :
        store.index("static esp_err_t load_cache_state_for_generation")
    ]
    repair_head = store[
        store.index("static esp_err_t repair_cache_head") :
        store.index("static esp_err_t recover_cache_tail")
    ]
    recover_tail = store[
        store.index("static esp_err_t recover_cache_tail") :
        store.index("static esp_err_t cache_path_absent")
    ]
    superseded = store[
        store.index(
            "static esp_err_t cache_record_superseded_by_later_journal",
            store.index("static esp_err_t cache_path_absent"),
        ) :
        store.index("static esp_err_t rebuild_cache_state_from_journal")
    ]
    journal = store[
        store.index("static esp_err_t validate_cache_journal_for_rebuild") :
        store.index("static esp_err_t load_cache_state_for_generation")
    ]
    load = store[
        store.index("static esp_err_t load_cache_state_for_generation") :
        store.index("static esp_err_t prepare_cache_room")
    ]

    assert "rebuild_cache_state_from_journal(" in store
    assert "bool rebuild_state = false;" in load
    assert "rebuild_state = true;" in load
    assert load.index("repair_cache_journal(") < load.index(
        "rebuild_cache_state_from_journal("
    )
    assert load.index("rebuild_cache_state_from_journal(") < load.index(
        "write_cache_state(paths, &loaded)"
    )
    normal_recovery = rebuild[
        rebuild.index("ret = recover_interrupted_record(provider, &record, false)") :
    ]
    assert normal_recovery.index("recover_interrupted_record(provider, &record, false)") < (
        normal_recovery.index("d1l_map_tile_cache_state_note_commit(state, &record)")
    )
    assert "cache_record_is_latest_coordinate(" in rebuild
    assert "cache_record_files_absent(" in rebuild
    assert "cache_record_superseded_by_later_journal(" in rebuild
    assert "const bool evicted_prefix =" in rebuild
    assert "const esp_err_t recovery_error = ret;" in rebuild
    assert "return recovery_error;" in rebuild
    assert "Interior holes stay charged until FIFO reaches them." in rebuild
    assert "if (!recovered && evicted_prefix &&" in rebuild
    assert rebuild.index("d1l_map_tile_cache_state_note_commit") < (
        rebuild.index("d1l_map_tile_cache_state_note_evict")
    )
    assert "allow_metadata_rebuild &&" in recover
    assert "cache_integrity_recoverable(final_metadata_ret)" in recover
    assert "cache_integrity_recoverable(temporary_metadata_ret)" in recover
    assert "final_tile_ret == ESP_OK ||" in recover
    assert "temporary_tile_ret == ESP_OK && temporary_metadata_matches" in recover
    assert "if (temporary_metadata_matches)" in recover
    assert "return reason == ESP_OK ? ESP_ERR_INVALID_CRC : reason;" in recover
    assert recover.index("allow_metadata_rebuild &&") < recover.index(
        "write_cache_metadata_tmp(&result, record)"
    )
    assert "cache_record_matches_tile(" in latest
    assert "content_crc32" not in latest
    assert "cache_record_is_latest_coordinate(" in repair_head
    assert repair_head.index("cache_record_files_absent(") < repair_head.index(
        "cache_record_is_latest_coordinate("
    )
    assert "recover_interrupted_record(\n                provider, &record, latest)" in repair_head
    assert "recover_interrupted_record(provider, &record, false)" in recover_tail
    assert "provider, &record, true" in recover_tail
    assert "cache_record_superseded_by_later_journal(" in recover_tail
    assert "if (superseded && evicted_prefix" in recover_tail
    assert "d1l_map_tile_cache_state_note_evict(state, &record)" in recover_tail
    assert "d1l_map_tile_cache_state_quarantine_head(state, 0U)" in recover_tail
    assert "cache_record_matches_tile(" in superseded
    assert "read_cache_metadata(\n        result.metadata_tmp_path" in superseded
    assert "verify_tile_file(result.path, &later)" in superseded
    assert "verify_tile_file(result.tmp_path, &later)" in superseded
    assert "cache_records_equal(\n                     &temporary_metadata, &later)" in superseded
    assert "discard_corrupt_cache_record" not in store
    assert "rebuild_cache_journal_prefix(" not in rebuild
    assert "rebuild_cache_journal_prefix(" not in journal
    assert "validate_cache_journal_for_rebuild(" in load
    assert "ret = rebuild_state ?" in load
    assert "delete_file_allow_missing(paths->state)" not in load


def test_corrupt_fifo_entry_is_atomically_quarantined_and_traced():
    policy = read("main/map/map_tile_cache_policy.c")
    store = read("main/storage/map_tile_store.c")
    state_quarantine = policy[
        policy.index("bool d1l_map_tile_cache_state_quarantine_head") :
        policy.index("bool d1l_map_tile_cache_recovery_plan")
    ]
    journal_rewrite = store[
        store.index("static esp_err_t rebuild_cache_journal(") :
        store.index("static uint32_t cache_next_sequence")
    ]
    repair_head = store[
        store.index("static esp_err_t repair_cache_head") :
        store.index("static esp_err_t recover_cache_tail")
    ]
    load = store[
        store.index("static esp_err_t load_cache_state_for_generation") :
        store.index("static esp_err_t prepare_cache_room")
    ]

    assert "state->head_offset += D1L_MAP_TILE_CACHE_RECORD_BYTES;" in state_quarantine
    assert "state->live_bytes > UINT64_MAX - additional_charge_bytes" in state_quarantine
    assert "state->live_bytes += additional_charge_bytes;" in state_quarantine
    assert "d1l_map_tile_cache_record_init_quarantine" in policy
    assert "D1L_MAP_TILE_DOWNLOAD_MAX_BYTES" in journal_rewrite
    assert "d1l_rp2040_bridge_file_rename(" in journal_rewrite
    assert "quarantine_cache_journal_record(" in journal_rewrite
    assert journal_rewrite.count("d1l_map_tile_cache_record_decode(") >= 2
    assert "record.sequence != expected_sequence" in journal_rewrite
    assert "read_cache_record(\n            paths->journal, record_offset" in journal_rewrite
    assert "if (ret == ESP_ERR_INVALID_CRC)" in repair_head
    assert repair_head.index("quarantine_cache_journal_record(") < (
        repair_head.index("d1l_map_tile_cache_state_quarantine_head")
    )
    assert "delete_file_allow_missing" not in repair_head[
        repair_head.index("if (ret == ESP_ERR_INVALID_CRC)") :
        repair_head.index("if (ret != ESP_OK)")
    ]
    for stage in ("cache_control_recover", "cache_journal_validate",
                  "cache_journal_repair",
                  "cache_state_rebuild", "cache_head_repair",
                  "cache_tail_recover"):
        assert f'"{stage}"' in load
    assert store.count("download_step(result, s_cache_recovery_stage") == 1
    assert store.count("&result, s_cache_recovery_stage") == 1


def test_cache_control_replace_gap_recovers_only_committed_backup():
    store = read("main/storage/map_tile_store.c")
    recover = store[
        store.index("static esp_err_t promote_missing_cache_control_backup") :
        store.index("static esp_err_t read_cache_metadata")
    ]
    rewrite = store[
        store.index("static esp_err_t rebuild_cache_journal(") :
        store.index("static esp_err_t rebuild_cache_journal_prefix")
    ]

    assert '"cache-journal.v1.bak"' in store
    assert '"cache-state.v1.bak"' in store
    assert "if (!file_result_missing(ret, &canonical))" in recover
    assert recover.index("return ESP_OK;") < recover.index(
        "d1l_rp2040_bridge_file_stat(\n        backup_path"
    )
    assert "Repair temps can be structurally valid prefixes" in recover
    assert "journal_tmp" not in recover
    assert "state_tmp" not in recover
    assert "validate_cache_state_file(backup_path)" in recover
    assert "backup.size != D1L_MAP_TILE_CACHE_STATE_BYTES" in recover
    assert "if (ret == ESP_ERR_INVALID_CRC)" in recover
    assert "backup_path, canonical_path, false" in recover
    assert recover.count("cache_backend_generation_matches(") >= 2
    assert "!cache_backend_generation_matches(backend_generation) ?" in recover
    assert rewrite.index("paths->journal_backup") < rewrite.index(
        "paths->journal_tmp"
    ) < rewrite.index("paths->journal)")

    repair_head = store[
        store.index("static esp_err_t repair_cache_head") :
        store.index("static esp_err_t recover_cache_tail")
    ]
    assert "state->head_offset /" in repair_head
    assert "record.sequence != expected_sequence" in repair_head
    assert repair_head.index("record.sequence != expected_sequence") < (
        repair_head.index("quarantine_cache_journal_record(")
    )


def test_fresh_tile_miss_precedes_global_cache_recovery():
    store = read("main/storage/map_tile_store.c")
    cached = store[
        store.index("static esp_err_t map_tile_store_cached_locked") :
        store.index("esp_err_t d1l_map_tile_store_cached")
    ]
    cached_metadata = cached.index("read_cache_metadata(")
    assert cached.index("cache_backend_generation(") < cached_metadata
    assert cached_metadata < cached.index(
        "load_cache_state_for_generation("
    )
    assert cached.index("d1l_rp2040_bridge_file_stat(") < cached.index(
        "cache_backend_generation_matches("
    )
    assert (
        "metadata_ret == ESP_ERR_INVALID_CRC ||\n"
        "        metadata_ret == ESP_ERR_INVALID_SIZE"
        in cached
    )
    assert (
        "metadata_ret == ESP_OK &&\n"
        "         !cache_record_matches_tile(&metadata, z, x, y)"
        in cached
    )
    recoverable_miss = cached.index("return ESP_ERR_NOT_FOUND;")
    io_error = cached.index(
        "if (metadata_ret != ESP_OK) {\n"
        "        return metadata_ret;\n"
        "    }"
    )
    assert recoverable_miss < io_error < cached.index(
        "load_cache_state_for_generation("
    )

    cached_read = store[
        store.index("static esp_err_t map_tile_store_read_locked") :
        store.index("esp_err_t d1l_map_tile_store_read")
    ]
    assert cached_read.index("cache_backend_generation(") < cached_read.index(
        "read_cache_metadata("
    )
    assert cached_read.index("read_cache_metadata(") < cached_read.index(
        "load_cache_state_for_generation("
    )
    assert cached_read.index("result.checksum_verified =") < cached_read.index(
        "cache_backend_generation_matches("
    )
    assert "ret == ESP_OK ? ESP_ERR_INVALID_CRC : ret" in cached_read


def test_cache_policy_native_vectors(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for map cache vectors")

    executable = tmp_path / "map_tile_cache_policy_test"
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/map/map_tile_cache_policy.c"),
        str(ROOT / "tests/native/map_tile_cache_policy_test.c"),
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    subprocess.run(
        [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
    )
