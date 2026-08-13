import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_truthful_time_checkpoint_integration_native(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise AssertionError("A C compiler is required for truthful-time tests")

    executable = tmp_path / (
        "time_service_checkpoint_integration_test.exe"
        if os.name == "nt"
        else "time_service_checkpoint_integration_test"
    )
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DD1L_BUILD_EPOCH_SEC=1784068276ULL",
        "-include",
        str(ROOT / "tests/native/time_service_test_clock.h"),
        "-I",
        str(ROOT / "tests/native/stubs"),
        "-I",
        str(ROOT / "tests/native"),
        "-I",
        str(ROOT / "main"),
        str(ROOT / "main/app/settings_envelope.c"),
        str(ROOT / "main/app/settings_protocol_migration.c"),
        str(ROOT / "main/app/settings_time_checkpoint.c"),
        str(ROOT / "main/platform/time_service_core.c"),
        str(ROOT / "main/platform/time_display.c"),
        str(ROOT / "main/platform/time_service.c"),
        str(ROOT / "tests/native/esp_nvs_stubs.c"),
        str(ROOT / "tests/native/time_service_checkpoint_integration_test.c"),
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    for scenario in (
        "happy",
        "corrupt",
        "guard",
        "legacy-migration",
        "legacy-quarantine",
        "legacy-preinit-failure",
    ):
        completed = subprocess.run(
            [str(executable), scenario],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        assert (
            completed.stdout.strip()
            == "native truthful-time checkpoint integration: ok"
        )
