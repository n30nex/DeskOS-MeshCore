import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def compile_retained_store(
    compiler: str,
    executable: Path,
    profile: str,
    sd_mode: str,
    expected_sd_enabled: bool,
) -> None:
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
        f"-DEXPECT_PROFILE_SD_ENABLED={int(expected_sd_enabled)}",
        str(ROOT / "main/app/release_profile.c"),
        str(ROOT / "main/storage/retained_blob_store.c"),
        str(ROOT / "main/storage/factory_reset.c"),
        str(ROOT / "main/mesh/packet_log.c"),
        str(ROOT / "tests/native/retained_blob_store_backend_state_test.c"),
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)


def native_executable(tmp_path: Path, name: str) -> Path:
    return tmp_path / (f"{name}.exe" if os.name == "nt" else name)


def test_retained_backend_generation_and_partition_migration_contract(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for native retained-store tests")

    executable = native_executable(
        tmp_path, "retained_blob_store_backend_state_test"
    )
    compile_retained_store(
        compiler,
        executable,
        "D1L_RELEASE_PROFILE_DEVELOPMENT",
        "D1L_SD_HISTORY_MODE_CONDITIONAL",
        True,
    )
    completed = subprocess.run(
        [str(executable)], cwd=ROOT, check=True, capture_output=True, text=True
    )
    assert (
        completed.stdout.strip()
        == "native retained backend generation and NVS partition: ok"
    )


def test_retained_sd_profile_admission_matrix(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for native retained-store tests")

    cases = (
        (
            "core_supported_optional",
            "D1L_RELEASE_PROFILE_CORE_1_0",
            "D1L_SD_HISTORY_MODE_SUPPORTED_OPTIONAL",
            True,
        ),
        (
            "core_conditional",
            "D1L_RELEASE_PROFILE_CORE_1_0",
            "D1L_SD_HISTORY_MODE_CONDITIONAL",
            False,
        ),
        (
            "core_disabled",
            "D1L_RELEASE_PROFILE_CORE_1_0",
            "D1L_SD_HISTORY_MODE_DISABLED",
            False,
        ),
        (
            "development_conditional",
            "D1L_RELEASE_PROFILE_DEVELOPMENT",
            "D1L_SD_HISTORY_MODE_CONDITIONAL",
            True,
        ),
        (
            "development_disabled",
            "D1L_RELEASE_PROFILE_DEVELOPMENT",
            "D1L_SD_HISTORY_MODE_DISABLED",
            False,
        ),
        (
            "full_conditional",
            "D1L_RELEASE_PROFILE_FULL_FEATURE",
            "D1L_SD_HISTORY_MODE_CONDITIONAL",
            True,
        ),
        (
            "full_disabled",
            "D1L_RELEASE_PROFILE_FULL_FEATURE",
            "D1L_SD_HISTORY_MODE_DISABLED",
            False,
        ),
    )
    for name, profile, sd_mode, expected_sd_enabled in cases:
        executable = native_executable(tmp_path, f"retained_sd_{name}")
        compile_retained_store(
            compiler, executable, profile, sd_mode, expected_sd_enabled
        )
        completed = subprocess.run(
            [str(executable), "profile-gate"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        assert completed.stdout.strip() == (
            "native retained SD profile admission: ok"
        )
