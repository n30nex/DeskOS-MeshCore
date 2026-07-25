import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def test_core_mesh_profile_boundary_native_matrix(tmp_path: Path) -> None:
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for Core Mesh boundary tests")

    cases = (
        ("core", "D1L_RELEASE_PROFILE_CORE_1_0", "EXPECT_CORE=1"),
        ("development", "D1L_RELEASE_PROFILE_DEVELOPMENT", "EXPECT_CORE=0"),
        ("full", "D1L_RELEASE_PROFILE_FULL_FEATURE", "EXPECT_CORE=0"),
    )
    for name, profile, expectation in cases:
        binary = tmp_path / (
            f"core_mesh_profile_boundary_{name}.exe"
            if os.name == "nt"
            else f"core_mesh_profile_boundary_{name}"
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
            str(ROOT / "tests/native"),
            "-I",
            str(ROOT / "main"),
            f"-DD1L_RELEASE_PROFILE={profile}",
            "-DD1L_SD_HISTORY_MODE=D1L_SD_HISTORY_MODE_DISABLED",
            f"-D{expectation}",
            str(ROOT / "main/app/release_profile.c"),
            str(ROOT / "main/mesh/channel_store.c"),
            str(ROOT / "tests/native/esp_nvs_stubs.c"),
            str(ROOT / "tests/native/mbedtls_md_stub.c"),
            str(ROOT / "tests/native/core_mesh_profile_boundary_test.c"),
            "-o",
            str(binary),
        ]
        subprocess.run(
            command, cwd=ROOT, check=True, capture_output=True, text=True
        )
        completed = subprocess.run(
            [str(binary)], cwd=ROOT, check=True, capture_output=True, text=True
        )
        assert completed.stdout.strip() == "native Core Mesh profile boundary: ok"
