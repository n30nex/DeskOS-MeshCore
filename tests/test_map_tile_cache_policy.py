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
