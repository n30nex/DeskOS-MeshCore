import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_drafts_and_composer_text_on_production_code(tmp_path):
    exe = tmp_path / "drafts"
    subprocess.run([
        shutil.which("gcc") or "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(ROOT / "tests/native/stubs"), "-I", str(ROOT / "main"),
        str(ROOT / "main/mesh/draft_store.c"), str(ROOT / "main/mesh/user_text.c"),
        str(ROOT / "main/app/settings_envelope.c"), str(ROOT / "main/ui/compose_text.c"),
        str(ROOT / "tests/native/draft_store_test.c"), "-o", str(exe),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
