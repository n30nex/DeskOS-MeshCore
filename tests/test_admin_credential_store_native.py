import os
import pathlib
import shutil
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_admin_credential_store_native(tmp_path: pathlib.Path) -> None:
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for native credential tests")
    executable = tmp_path / (
        "admin_credential_store_test.exe"
        if os.name == "nt"
        else "admin_credential_store_test"
    )
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "tests/native/stubs"),
            "-I",
            str(ROOT / "tests/native"),
            "-I",
            str(ROOT / "main"),
            str(ROOT / "tests/native/admin_credential_store_test.c"),
            str(ROOT / "tests/native/esp_nvs_stubs.c"),
            str(ROOT / "main/mesh/admin_credential_store.c"),
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
    assert completed.stdout.strip() == "admin_credential_store_test: ok"
