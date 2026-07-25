import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_ui_node_detail_controller_native(tmp_path: Path) -> None:
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for Node Detail tests")
    executable = tmp_path / (
        "ui_node_detail_test.exe" if os.name == "nt" else "ui_node_detail_test"
    )
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "tests/native/ui_node_detail_stubs"),
        "-I",
        str(ROOT / "tests/native/stubs"),
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/app/release_profile.c"),
        str(ROOT / "main/ui/ui_node_detail.c"),
        str(ROOT / "tests/native/ui_node_detail_stubs/lvgl_stub.c"),
        str(ROOT / "tests/native/ui_node_detail_test.c"),
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    completed = subprocess.run(
        [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
    )
    assert completed.stdout.strip() == "native UI Node Detail controller: ok"
