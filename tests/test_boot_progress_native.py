from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def test_early_boot_progress_renders_both_buffers_before_lvgl(tmp_path):
    board = (ROOT / "main/hal/indicator_board.c").read_text()
    helpers = board.split("typedef struct {\n    char character;", 1)[1].split("static uint16_t clamp_touch_coord", 1)[0]
    renderer = "esp_err_t d1l_board_display_boot_progress" + board.split(
        "esp_err_t d1l_board_display_boot_progress", 1
    )[1].split("const d1l_board_status_t *d1l_board_status", 1)[0]
    constants = "\n".join(re.findall(r"^#define D1L_(?:LCD_|SPLASH_).*$", board, re.M))
    program = tmp_path / "boot_progress.c"
    program.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#define CONFIG_LCD_LVGL_DIRECT_MODE 1
static struct {bool ready;} s_status = {true};
static uint16_t first[480*480], second[480*480];
static void bsp_lcd_get_frame_buffer(void **a, void **b) {*a=first; *b=second;}
static void Cache_WriteBack_Addr(uint32_t address, uint32_t size) {(void)address; (void)size;}
''' + constants + "\ntypedef struct {\n    char character;" + helpers + renderer + r'''
int main(void) {
    assert(d1l_board_display_boot_progress(0)==ESP_OK);
    assert(first[290*480+180]==0x18e3);
    assert(first[258*480+178]==0x2eba); /* LOADING remains legible above the bar. */
    assert(d1l_board_display_boot_progress(50)==ESP_OK);
    assert(first[290*480+180]==0x2eba && first[290*480+300]==0x18e3);
    assert(memcmp(first,second,sizeof(first))==0);
    assert(d1l_board_display_boot_progress(255)==ESP_OK);
    assert(first[290*480+359]==0x2eba && first[290*480+360]==0x0861);
    s_status.ready=false;
    assert(d1l_board_display_boot_progress(50)==ESP_ERR_INVALID_STATE);
    return 0;
}
''')
    compiler = shutil.which("gcc") or shutil.which("clang")
    assert compiler, "A C compiler is required for boot rendering checks"
    binary = tmp_path / "boot_progress"
    subprocess.run([compiler, "-std=c11", "-O2", "-Wno-pointer-to-int-cast",
                    "-I", str(ROOT / "tests/native/stubs"), str(program), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    startup = (ROOT / "main/app_main.c").read_text()
    assert startup.rindex("d1l_board_display_boot_progress(") < startup.index("d1l_ui_phase1_start()")
