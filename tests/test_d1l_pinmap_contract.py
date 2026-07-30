import json
from pathlib import Path

from scripts.smoke_d1l import SMOKE_COMMANDS


ROOT = Path(__file__).resolve().parents[1]


def load_pinmap() -> dict:
    return json.loads((ROOT / "boards" / "seeed_indicator_d1l" / "pinmap.json").read_text(encoding="utf-8"))


def test_rp2040_bridge_pin_contract():
    pinmap = load_pinmap()
    assert pinmap["rp2040_bridge"] == {
        "esp_rx_gpio": 20,
        "esp_tx_gpio": 19,
        "expander_reset": 8,
        "bootsel_control": None,
        "uart_port": 2,
        "baud": 115200,
    }


def test_phase1_smoke_covers_button_rp2040_and_packets():
    assert "button" in SMOKE_COMMANDS
    assert "rp2040 status" in SMOKE_COMMANDS
    assert "rp2040 ping" not in SMOKE_COMMANDS
    assert "rp2040 bootloader" not in SMOKE_COMMANDS
    assert "rp2040 double-reset" not in SMOKE_COMMANDS
    assert "rp2040 reset" not in SMOKE_COMMANDS
    assert "packets" in SMOKE_COMMANDS


def test_console_exposes_explicit_rp2040_reset_and_bootloader_commands():
    console = (ROOT / "main" / "comms" / "usb_console.c").read_text(encoding="utf-8")
    rp2040_header = (ROOT / "main" / "hal" / "rp2040_bridge.h").read_text(encoding="utf-8")
    rp2040_source = (ROOT / "main" / "hal" / "rp2040_bridge.c").read_text(encoding="utf-8")
    assert "d1l_rp2040_bridge_reset" in rp2040_header
    assert "d1l_rp2040_bridge_double_reset" in rp2040_header
    assert "d1l_rp2040_bridge_ping" in rp2040_header
    assert "d1l_rp2040_bridge_enter_bootloader" in rp2040_header
    assert "d1l_rp2040_bridge_stock_probe" in rp2040_header
    assert "D1L_RP2040_PING_QUERY" in rp2040_source
    assert "D1L_RP2040_BOOTLOADER_QUERY" in rp2040_source
    assert "seeed_model_title_query" in rp2040_source
    assert "{0x02U, 0xA5U, 0x00U}" in rp2040_source
    assert "tca9535_set_direction(pins->expander_reset, true)" in rp2040_source
    assert "tca9535_set_level(pins->expander_reset, false)" in rp2040_source
    assert "tca9535_set_level(pins->expander_reset, true)" in rp2040_source
    assert "settle_ms = 8000U" in console
    assert "settle_ms : 8000U" in rp2040_source
    assert 'ok_begin("rp2040 reset")' in console
    assert '"rp2040 ping"' in console
    assert 'cmd_rp2040_ping' in console
    assert 'strcmp(line, "rp2040 ping")' in console
    assert '"rp2040 bootloader"' in console
    assert 'cmd_rp2040_bootloader' in console
    assert 'strcmp(line, "rp2040 bootloader")' in console
    assert '"rp2040 stock-probe"' in console
    assert 'cmd_rp2040_stock_probe' in console
    assert 'strcmp(line, "rp2040 stock-probe")' in console
    stock_probe_body = console.split("static void cmd_rp2040_stock_probe", 1)[1].split("static void cmd_storage_status", 1)[0]
    assert '\\"sent_cobs_hex\\"' in stock_probe_body
    assert '\\"response_hex\\"' in stock_probe_body
    assert '\\"response_ascii\\"' in stock_probe_body
    assert '\\"sd_touched\\":false' in stock_probe_body
    assert '"rp2040 double-reset"' in console
    assert 'cmd_rp2040_double_reset' in console
    assert 'strncmp(line, "rp2040 double-reset"' in console
    double_reset_body = console.split("static void cmd_rp2040_double_reset", 1)[1].split("static void cmd_rp2040_ping", 1)[0]
    assert 'ok_begin(line)' in double_reset_body
    assert 'err_result(line, "INVALID_ARG"' in double_reset_body
    assert 'err_result(line, esp_err_to_name(ret)' in double_reset_body
    assert '"rp2040 reset"' in console
    assert 'strcmp(line, "rp2040 reset")' in console
    assert "public_rf_tx" in console
    assert "formats_sd" in console


def test_phase1_lvgl_uses_seeed_direct_anti_tear_path():
    defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
    assert "CONFIG_SENSECAP_INDICATOR_SCREEN_GX=y" in defaults
    assert "# CONFIG_SENSECAP_INDICATOR_SCREEN_DX is not set" in defaults
    assert "CONFIG_LCD_AVOID_TEAR=y" in defaults
    assert "CONFIG_LCD_LVGL_DIRECT_MODE=y" in defaults
    assert "CONFIG_LV_MEM_CUSTOM=y" in defaults
    assert "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y" in defaults
    assert "# CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 is not set" in defaults


def test_board_boot_defers_full_i2c_scan_to_explicit_console_diagnostic():
    board_source = (ROOT / "main" / "hal" / "indicator_board.c").read_text(
        encoding="utf-8"
    )
    console_source = (ROOT / "main" / "comms" / "usb_console.c").read_text(
        encoding="utf-8"
    )
    board_init = board_source.split("esp_err_t d1l_board_init(void)", 1)[1].split(
        "esp_err_t d1l_board_display_boot_splash(void)", 1
    )[0]
    i2c_command = console_source.split("static void cmd_i2c(void)", 1)[1].split(
        "static void cmd_button(void)", 1
    )[0]

    assert "d1l_board_i2c_scan(&s_status)" not in board_init
    assert "d1l_board_i2c_scan(&scan)" in i2c_command


def test_touch_path_uses_pressed_state_not_uninitialized_btn_val():
    ui_source = (ROOT / "main" / "ui" / "ui_phase1.c").read_text(encoding="utf-8")
    board_source = (ROOT / "main" / "hal" / "indicator_board.c").read_text(encoding="utf-8")
    seeed_patch = (ROOT / "patches" / "sensecap_indicator_touch_fix.patch").read_text(encoding="utf-8")
    console_source = (ROOT / "main" / "comms" / "usb_console.c").read_text(encoding="utf-8")

    assert "uint8_t btn_val = 0;" in seeed_patch
    assert "bool pressed = (tp_num > 0) || (btn_val != 0);" in seeed_patch
    assert "@@ -80,0 +84 @@" in seeed_patch
    assert "@@ -79,0 +82 @@" not in seeed_patch
    assert "d1l_board_touch_read(&sample)" in ui_source
    assert "read_cached_touch_state(&sample)" in ui_source
    assert "touch_poll_task" in ui_source
    assert "D1L_UI_TIMER_MIN_SLEEP_MS" in ui_source
    assert "uint32_t wait_ms = lv_timer_handler();" in ui_source
    assert "D1L_UI_TASK_CORE" in ui_source
    assert "xTaskCreatePinnedToCoreWithCaps(" in ui_source
    assert 'ui_task, "d1l_ui", D1L_UI_TASK_STACK_BYTES' in ui_source
    assert "d1l_ui_navigation_request(tab);" in ui_source
    assert "process_pending_tab_switch();" in ui_source
    assert '#include "ui_navigation.h"' in ui_source
    assert ui_source.index("typedef d1l_ui_screen_t d1l_ui_tab_t") < ui_source.index(
        "static void request_tab_switch"
    )
    assert "sample.pressed" in ui_source
    assert "data->state = LV_INDEV_STATE_REL;" in ui_source
    assert "touch_sample_has_valid_point(&sample)" in ui_source
    assert "CONFIG_LCD_EVB_SCREEN_WIDTH - 1 - (int32_t)sample.x" in ui_source
    assert "CONFIG_LCD_EVB_SCREEN_HEIGHT - 1 - (int32_t)sample.y" in ui_source
    assert "clamp_touch_coord" in ui_source
    assert "last_x" in ui_source
    assert "tp_dev_id = 0;" in seeed_patch
    assert "d1l_board_touch_read" in board_source
    assert "d1l_board_touch_raw_position_read(&raw)" in board_source
    assert "apply_raw_touch_state(out_state, &raw)" in board_source
    assert '\\"pressed\\":%s' in console_source
    assert '\\"coordinate_valid\\":%s' in console_source
    assert '"touch raw"' in console_source
    assert "d1l_board_touch_raw_read" in board_source
    assert "registers_00_1f" in console_source
    assert "id_a1_a9" in console_source

    dock_body = ui_source.split("static void dock_event_cb", 1)[1].split("static void process_pending_tab_switch", 1)[0]
    assert "render_active_tab();" not in dock_body
    assert "hide_sheet();" not in dock_body


def test_cmake_applies_parent_owned_seeed_patch_set():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    touch_patch = (ROOT / "patches" / "sensecap_indicator_touch_fix.patch").read_text(encoding="utf-8")
    compat_patch = (ROOT / "patches" / "sensecap_indicator_idf55_compat.patch").read_text(encoding="utf-8")

    assert 'COMMAND git rev-parse --short HEAD' in cmake
    assert 'set(PROJECT_VER "${D1L_GIT_SHORT_SHA}")' in cmake
    assert "SEEED_BSP_TOUCH_PATCH" in cmake
    assert "SEEED_BSP_IDF55_COMPAT_PATCH" in cmake
    assert "foreach(SEEED_BSP_PATCH IN LISTS SEEED_BSP_PATCHES)" in cmake
    assert "git apply --unidiff-zero --check" in cmake
    assert "git apply --unidiff-zero --reverse --check" in cmake
    assert "--ignore-space-change" in cmake
    assert "components/bsp/src/indev/indev.c" in touch_patch
    assert "components/i2c_devices/touch_panel/ft5x06.c" in touch_patch
    assert "data->btn_val = pressed ? 1 : 0;" in touch_patch
    assert "ft5x06_write_byte(FT5x06_DEVICE_MODE, 0x00)" in touch_patch
    assert "ft5x06_write_byte(FT5x06_ID_G_PMODE, 0x00)" in touch_patch
    assert "tt21100_read((uint8_t *)&reg_val" in compat_patch
    assert "gtp_get_points(&g_ts_data, points" in compat_patch
    assert "read_input_pins16" in compat_patch
    assert "g_dioIrq = dioIrq;" in compat_patch
    assert "static void (*lvgl_direct_mode_buf_copy)(void)" in compat_patch
    assert "const esp_lcd_rgb_panel_event_data_t *edata" in compat_patch
    assert "es8388_config_dac_output(es_dac_output_t output)" in compat_patch
    assert "static esp_err_t lcd_draw_bitmap" in compat_patch
    assert "const void *data" in compat_patch
    assert "-fpermissive" not in compat_patch
    assert "CONFIG_COMPILER_DISABLE_GCC14_WARNINGS" not in compat_patch


def test_build_script_is_host_only_and_does_not_rewrite_lcd_config():
    script = (ROOT / "scripts" / "build_d1l.ps1").read_text(encoding="utf-8")
    assert "Set-D1lSafeSdkconfig" not in script
    assert "CONFIG_LCD_AVOID_TEAR" not in script
    assert "CONFIG_LCD_LVGL_DIRECT_MODE" not in script
    assert "disabled_local_github_actions_only" in script
    assert "Use GitHub Actions d1l-ci firmware-build" in script
    assert 'framework = "ESP-IDF v5.5.4"' in script
    assert "ESP-IDF v5.1.x" not in script
    assert "& idf.py build" not in script
