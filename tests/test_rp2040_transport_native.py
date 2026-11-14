import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_real_uart_reader_handles_slow_sd_and_late_replies(tmp_path):
    source = (ROOT / "main/hal/rp2040_bridge.c").read_text(encoding="utf-8")
    functions = []
    for name in ("line_has_prefix", "token_value_span", "parse_u32_token", "exchange_prefixed_line_internal"):
        start = source.index("static ", source.rfind("\n", 0, source.index(name + "(")))
        brace = source.index("{", start)
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        functions.append(source[start:end])
    (tmp_path / "production_reader.inc").write_text("\n\n".join(functions), encoding="utf-8")
    exe = tmp_path / ("transport.exe" if os.name == "nt" else "transport")
    result = subprocess.run([
        shutil.which("gcc") or "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/stubs"), "-I", str(ROOT / "main"), "-I", str(tmp_path),
        str(ROOT / "tests/native/rp2040_transport_test.c"),
        str(ROOT / "main/hal/rp2040_file_reply.c"), "-o", str(exe),
    ], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "slow writes, late replies and bounded deadlines: ok" in result.stdout
