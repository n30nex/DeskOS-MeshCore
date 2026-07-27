import os
import pathlib
import shutil
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_meshcore_admin_runtime_replay_native(tmp_path: pathlib.Path) -> None:
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for native admin tests")
    executable = tmp_path / (
        "meshcore_admin_runtime_replay_test.exe"
        if os.name == "nt"
        else "meshcore_admin_runtime_replay_test"
    )
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffunction-sections",
            "-fdata-sections",
            "-DESP_ERR_INVALID_RESPONSE=0x108",
            "-DESP_ERR_NOT_ALLOWED=0x109",
            "-I",
            str(ROOT / "tests/native/stubs"),
            "-I",
            str(ROOT / "tests/native"),
            "-I",
            str(ROOT / "main"),
            str(ROOT / "tests/native/meshcore_admin_runtime_replay_test.c"),
            str(ROOT / "tests/native/esp_nvs_stubs.c"),
            str(ROOT / "main/mesh/meshcore_admin_runtime.c"),
            str(ROOT / "main/mesh/meshcore_admin_dispatch.c"),
            str(ROOT / "main/mesh/meshcore_wire.c"),
            "-Wl,--gc-sections",
            "-o",
            str(executable),
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    completed = subprocess.run(
        [str(executable)],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    assert completed.stdout.strip() == "meshcore_admin_runtime_replay_test: ok"
