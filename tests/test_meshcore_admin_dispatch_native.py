import os
import pathlib
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_meshcore_admin_dispatch_native(tmp_path: pathlib.Path) -> None:
    compiler = os.environ.get("CC", "gcc")
    executable = tmp_path / "meshcore_admin_dispatch_test"
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "main"),
            str(ROOT / "tests/native/meshcore_admin_dispatch_test.c"),
            str(ROOT / "main/mesh/meshcore_admin_dispatch.c"),
            str(ROOT / "main/mesh/user_text.c"),
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
    assert completed.stdout.strip() == "meshcore_admin_dispatch_test: ok"
