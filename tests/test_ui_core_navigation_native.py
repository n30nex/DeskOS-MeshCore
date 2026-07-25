import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_ui_core_navigation_profile_matrix(tmp_path: Path) -> None:
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for Core UI tests")

    cases = (
        (
            "core",
            "D1L_RELEASE_PROFILE_CORE_1_0",
            "D1L_SD_HISTORY_MODE_CONDITIONAL",
            "1",
        ),
        (
            "development",
            "D1L_RELEASE_PROFILE_DEVELOPMENT",
            "D1L_SD_HISTORY_MODE_SUPPORTED_OPTIONAL",
            "0",
        ),
    )
    for name, profile, sd_mode, expect_core in cases:
        executable = tmp_path / (
            f"ui_core_navigation_{name}.exe"
            if os.name == "nt"
            else f"ui_core_navigation_{name}"
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
            f"-DD1L_RELEASE_PROFILE={profile}",
            f"-DD1L_SD_HISTORY_MODE={sd_mode}",
            f"-DEXPECT_CORE={expect_core}",
            str(ROOT / "main/app/release_profile.c"),
            str(ROOT / "main/ui/ui_navigation.c"),
            str(ROOT / "tests/native/ui_core_navigation_test.c"),
            "-o",
            str(executable),
        ]
        subprocess.run(
            command, cwd=ROOT, check=True, capture_output=True, text=True
        )
        completed = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        assert completed.stdout.strip() == "native UI Core navigation: ok"
