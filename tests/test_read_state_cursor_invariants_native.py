from __future__ import annotations

import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_read_state_advances_only_the_selected_exact_cursor(tmp_path: Path) -> None:
    binary = tmp_path / "read_state_cursor_invariants_test"
    command = [
        "gcc",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DESP_ERR_INVALID_VERSION=0x10B",
        "-I",
        str(ROOT / "tests/native/stubs"),
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/mesh/read_state.c"),
        str(ROOT / "tests/native/read_state_lock_stubs.c"),
        str(ROOT / "tests/native/retained_blob_store_nvs_adapter.c"),
        str(ROOT / "tests/native/read_state_cursor_invariants_test.c"),
        "-o",
        str(binary),
    ]
    subprocess.run(command, check=True, cwd=ROOT)
    completed = subprocess.run(
        [str(binary)], check=True, cwd=ROOT, text=True, capture_output=True
    )
    assert completed.stdout.strip() == "native read-state cursor invariants: ok"
