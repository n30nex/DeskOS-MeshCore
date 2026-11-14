import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_receive_animation_real_lvgl(tmp_path):
    lvgl = ROOT / "third_party/sensecap_indicator_esp32/components/lvgl"
    config = (lvgl / "lv_conf_template.h").read_text(encoding="utf-8")
    config = config.replace('#if 0 /*Set it to "1" to enable content*/', '#if 1 /*Native animation test*/')
    config = re.sub(r'(#define LV_MEM_SIZE).*', r'\1 (4U * 1024U * 1024U)', config)
    for size in (14, 18):
        config = re.sub(rf'(#define LV_FONT_MONTSERRAT_{size})\s+\d', r'\1 1', config)
    (tmp_path / "lv_conf.h").write_text(config, encoding="utf-8")
    sources = list(lvgl.glob("src/**/*.c")) + [ROOT / name for name in (
        "main/ui/ui_receive_animation.c", "main/ui/ui_typography.c",
        "main/ui/ui_font_symbols_14.c", "main/ui/ui_font_latin_18.c",
        "tests/native/ui_receive_animation_test.c",
    )]
    args = ["-std=c11", "-O0", "-DLV_CONF_INCLUDE_SIMPLE"]
    for include in (lvgl, lvgl.parent, tmp_path, ROOT / "main"):
        args += ["-I", include.as_posix()]
    args += [p.as_posix() for p in sources] + ["-lm"]
    response = tmp_path / "sources.rsp"
    response.write_text("\n".join('"' + arg + '"' for arg in args), encoding="utf-8")
    exe = tmp_path / ("receive-animation.exe" if os.name == "nt" else "receive-animation")
    build = subprocess.run([shutil.which("gcc") or "clang", "@" + str(response), "-o", str(exe)],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
    assert build.returncode == 0, build.stdout + build.stderr
    result = subprocess.run([str(exe), str(tmp_path)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "timing, bursts, suppression, touch and lifecycle passed" in result.stdout
