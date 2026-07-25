import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_channel_message_coordinator_retries_revision_races(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for coordinator tests")

    cases = (
        ("core", "D1L_RELEASE_PROFILE_CORE_1_0", "EXPECT_CORE=1"),
        ("development", "D1L_RELEASE_PROFILE_DEVELOPMENT", "EXPECT_CORE=0"),
        ("full", "D1L_RELEASE_PROFILE_FULL_FEATURE", "EXPECT_CORE=0"),
    )
    for name, profile, expectation in cases:
        executable = tmp_path / (
            f"channel_message_coordinator_{name}.exe"
            if os.name == "nt"
            else f"channel_message_coordinator_{name}"
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
            "-DD1L_SD_HISTORY_MODE=D1L_SD_HISTORY_MODE_DISABLED",
            f"-D{expectation}",
            str(ROOT / "main/app/release_profile.c"),
            str(ROOT / "main/mesh/channel_message_coordinator.c"),
            str(ROOT / "tests/native/channel_message_coordinator_test.c"),
            "-o",
            str(executable),
        ]
        subprocess.run(
            command, cwd=ROOT, check=True, capture_output=True, text=True
        )
        completed = subprocess.run(
            [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
        )
        assert completed.stdout.strip() == "native channel-message coordinator: ok"
