import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_display_timeout_choices_cover_short_idle_intervals(tmp_path):
    source = (ROOT / "main/hal/display_preferences.c").read_text(encoding="utf-8")
    helpers = source.split("bool d1l_display_timeout_valid", 1)[1].split(
        "static bool notification_mode_valid", 1)[0]
    harness = '#include <assert.h>\n#include <stddef.h>\n#include "hal/display_preferences.h"\n'
    harness += "bool d1l_display_timeout_valid" + helpers
    harness += r'''
int main(void) {
    const uint16_t choices[] = {0, 30, 60, 120, 300, 600};
    for (unsigned i = 0; i < 6; ++i) {
        assert(d1l_display_timeout_valid(choices[i]));
        assert(d1l_display_timeout_next(choices[i]) == choices[(i+1)%6]);
    }
    assert(!d1l_display_timeout_valid(1));
    assert(!d1l_display_timeout_valid(31));
    assert(!d1l_display_timeout_valid(65535));
    assert(d1l_display_timeout_next(65535) == 0);
    return 0;
}
'''
    path = tmp_path / "timeout.c"
    path.write_text(harness, encoding="utf-8")
    exe = tmp_path / "timeout"
    subprocess.run([shutil.which("gcc") or "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "tests/native/stubs"), "-I", str(ROOT / "main"),
                    str(path), "-o", str(exe)], check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True)
