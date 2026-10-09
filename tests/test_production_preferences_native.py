import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_production_preferences_preserve_legacy_settings_and_fail_closed(tmp_path):
    exe = tmp_path / ("preferences.exe" if os.name == "nt" else "preferences")
    subprocess.run([
        shutil.which("gcc") or "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/stubs"), "-I", str(ROOT / "tests/native"),
        "-I", str(ROOT / "main"),
        str(ROOT / "main/mesh/contact_policy.c"), str(ROOT / "main/hal/display_preferences.c"),
        str(ROOT / "tests/native/esp_nvs_stubs.c"),
        str(ROOT / "tests/native/production_preferences_test.c"), "-o", str(exe),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    subprocess.run([str(exe), "saved"], check=True, capture_output=True, text=True)
    subprocess.run([str(exe), "invalid"], check=True, capture_output=True, text=True)
