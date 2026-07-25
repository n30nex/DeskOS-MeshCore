import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_ui_home_view_model_native_truth_table(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for UI home view tests")
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
            f"ui_home_view_test_{name}.exe"
            if os.name == "nt"
            else f"ui_home_view_test_{name}"
        )
        subprocess.run(
            [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "main"),
            f"-DD1L_RELEASE_PROFILE={profile}",
            f"-DD1L_SD_HISTORY_MODE={sd_mode}",
            f"-DEXPECT_CORE={expect_core}",
            str(ROOT / "main/app/release_profile.c"),
            str(ROOT / "main/ui/ui_home_view.c"),
            str(ROOT / "tests/native/ui_home_view_test.c"),
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
        assert completed.stdout.strip() == "native UI home view model: ok"
