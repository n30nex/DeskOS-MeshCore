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
