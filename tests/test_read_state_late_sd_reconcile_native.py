from __future__ import annotations

import os
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_read_state_reconciles_late_sd_generation_safely(tmp_path: Path) -> None:
    binary = tmp_path / "read_state_late_sd_reconcile_test"
    command = [
        "gcc",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DESP_ERR_INVALID_VERSION=0x10B",
        "-DD1L_READ_STATE_TEST_HOOKS",
        "-DD1L_TEST_REAL_MUTEX",
        "-I",
        str(ROOT / "tests/native/stubs"),
        "-I",
        str(ROOT / "tests/native"),
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/mesh/read_state.c"),
        str(ROOT / "tests/native/esp_nvs_stubs.c"),
        str(ROOT / "tests/native/retained_blob_store_nvs_adapter.c"),
        str(ROOT / "tests/native/read_state_late_sd_reconcile_test.c"),
    ]
    if os.name != "nt":
        command.append("-pthread")
    command.extend(["-o", str(binary)])
    subprocess.run(command, check=True, cwd=ROOT)
    completed = subprocess.run(
        [str(binary)], check=True, cwd=ROOT, text=True, capture_output=True
    )
    assert completed.stdout.strip() == "native read-state late SD reconciliation: ok"
