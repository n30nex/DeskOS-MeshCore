import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_real_lvgl_keyboard_layouts_accents_and_cursor(tmp_path):
    lvgl = ROOT / "third_party/sensecap_indicator_esp32/components/lvgl"
    config = (lvgl / "lv_conf_template.h").read_text(encoding="utf-8")
    config = config.replace('#if 0 /*Set it to "1" to enable content*/', '#if 1 /*Native interaction test*/')
    config = re.sub(r'(#define LV_MEM_SIZE).*', r'\1 (4U * 1024U * 1024U)', config)
    for size in (14, 18, 24):
        config = re.sub(rf'(#define LV_FONT_MONTSERRAT_{size})\s+\d', r'\1 1', config)
    (tmp_path / "lv_conf.h").write_text(config, encoding="utf-8")
    sources = list(lvgl.glob("src/**/*.c")) + [ROOT / name for name in (
        "main/ui/ui_keyboard.c", "main/ui/ui_typography.c", "main/ui/ui_font_symbols_14.c",
        "main/ui/ui_font_latin_18.c", "main/ui/ui_device_sheets.c", "main/ui/ui_modal.c",
        "main/platform/time_display.c", "main/app/release_profile.c", "main/hal/display_preferences.c",
        "main/mesh/user_text.c", "tests/native/esp_nvs_stubs.c", "tests/native/ui_keyboard_test.c",
    )]
    args = ["-std=c11", "-O0", "-DLV_CONF_INCLUDE_SIMPLE",
            "-DD1L_RELEASE_PROFILE=D1L_RELEASE_PROFILE_FULL_FEATURE",
            "-DD1L_SD_HISTORY_MODE=D1L_SD_HISTORY_MODE_CONDITIONAL"]
    for include in (lvgl, lvgl.parent, tmp_path, ROOT / "main", ROOT / "tests/native/stubs", ROOT / "tests/native"):
        args += ["-I", include.as_posix()]
    args += [p.as_posix() for p in sources] + ["-lm"]
    response = tmp_path / "sources.rsp"
    response.write_text("\n".join('"' + a + '"' for a in args), encoding="utf-8")
    exe = tmp_path / ("keyboard.exe" if os.name == "nt" else "keyboard")
    built = subprocess.run([shutil.which("gcc") or "clang", "@" + str(response), "-o", str(exe)],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
    assert built.returncode == 0, built.stdout + built.stderr
    result = subprocess.run([str(exe), str(tmp_path)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "native LVGL keyboard layouts, accents, cursor, fonts and settings: ok" in result.stdout
