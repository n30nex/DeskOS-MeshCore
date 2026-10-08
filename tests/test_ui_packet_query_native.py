import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_packet_query_does_not_block_ui_or_publish_stale_pages(tmp_path):
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required"
    exe = tmp_path / ("packet_query.exe" if os.name == "nt" else "packet_query")
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/packet_query_stubs"),
        "-I", str(ROOT / "tests/native/stubs"), "-I", str(ROOT / "main"),
        str(ROOT / "tests/native/ui_packet_query_test.c"), "-o", str(exe),
    ], check=True, cwd=ROOT, capture_output=True, text=True)
    result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    assert result.stdout.strip() == "native asynchronous packet query: ok"
