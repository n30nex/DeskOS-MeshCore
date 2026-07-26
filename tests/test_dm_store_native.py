import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _build_native_dm_harness(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for native DM store tests")

    executable = tmp_path / (
        "dm_store_behavior_test.exe" if os.name == "nt" else "dm_store_behavior_test"
    )
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "tests/native/stubs"),
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/mesh/dm_delivery_state.c"),
        str(ROOT / "main/mesh/dm_store.c"),
        str(ROOT / "main/mesh/user_text.c"),
        str(ROOT / "tests/native/dm_store_behavior_test.c"),
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    return executable


def test_dm_retained_backend_reconciliation_and_split_retry(tmp_path):
    executable = _build_native_dm_harness(tmp_path)
    completed = subprocess.run(
        [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
    )
    assert completed.stdout.strip() == "native DM retained durability: ok"


def test_dm_deferred_ack_persists_only_after_explicit_flush(tmp_path):
    executable = _build_native_dm_harness(tmp_path)
    completed = subprocess.run(
        [str(executable), "deferred-ack"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    assert completed.stdout.strip() == "native DM deferred ACK persistence: ok"


def test_dm_deferred_ack_unloaded_path_performs_no_storage_io(tmp_path):
    executable = _build_native_dm_harness(tmp_path)
    completed = subprocess.run(
        [str(executable), "deferred-unloaded"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    assert completed.stdout.strip() == "native DM deferred ACK unloaded path: ok"
