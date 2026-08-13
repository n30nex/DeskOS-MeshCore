from pathlib import Path

from scripts.smoke_d1l import SMOKE_COMMANDS


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_crash_log_is_bounded_and_nvs_backed():
    header = read("main/diagnostics/crash_log.h")
    source = read("main/diagnostics/crash_log.c")
    app_main = read("main/app_main.c")
    cmake = read("main/CMakeLists.txt")
    assert "D1L_CRASH_LOG_CAPACITY 8U" in header
    assert "d1l_crash_log_entry_t" in header
    assert "d1l_crash_log_clear" in header
    assert 'D1L_CRASH_LOG_NAMESPACE "d1l_crash"' in source
    assert 'D1L_CRASH_LOG_KEY "ring"' in source
    assert "nvs_get_blob" in source
    assert "nvs_set_blob" in source
    assert "append_boot_reset" in source
    assert "ESP_RST_TASK_WDT" in source
    assert "esp_err_t crash_log_ret = d1l_crash_log_init()" in app_main
    assert '"diagnostics/crash_log.c"' in cmake


def test_health_monitor_reports_stack_lvgl_and_largest_blocks():
    header = read("main/diagnostics/health_monitor.h")
    source = read("main/diagnostics/health_monitor.c")
    ui = read("main/ui/ui_phase1.c")
    console = read("main/comms/usb_console.c")
    app_main = read("main/app_main.c")
    assert "boot_nonce" in header
    assert "nvs_ready" in header
    assert "nvs_error" in header
    assert "d1l_health_monitor_init" in header
    assert "generate_boot_nonce" in source
    assert "esp_random()" in source
    assert ".boot_nonce = d1l_health_monitor_boot_nonce()" in source
    assert ".nvs_ready = s_nvs_ready" in source
    assert "d1l_health_monitor_init(nvs_ret)" in app_main
    assert "nvs_flash_erase" not in app_main
    assert "ESP_ERROR_CHECK(nvs_ret)" not in app_main
    assert "NVS unavailable; preserving persisted data" in app_main
    assert "heap_largest_free" in header
    assert "internal_heap_free" in header
    assert "internal_heap_min_free" in header
    assert "internal_heap_largest_free" in header
    assert "dma_heap_free" in header
    assert "dma_heap_largest_free" in header
    assert "psram_min_free" in header
    assert "current_task_stack_free_words" in header
    assert "ui_task_stack_free_words" in header
    assert "retained_task_stack_free_bytes" in header
    assert "lvgl_used_pct" in header
    assert "heap_caps_get_largest_free_block" in source
    assert "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in source
    assert "MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL" in source
    assert "uxTaskGetStackHighWaterMark(NULL)" in source
    assert "uxTaskGetStackHighWaterMark(s_ui_task)" in source
    assert "uxTaskGetStackHighWaterMark(s_retained_task)" in source
    assert "lv_mem_monitor" in source
    assert "d1l_health_monitor_sample_lvgl" in header
    assert "d1l_health_monitor_register_ui_task" in ui
    assert "d1l_health_monitor_sample_lvgl();" in ui
    assert "d1l_health_monitor_set_lvgl_ready(true)" in ui
    assert '\\"ui_task_stack_free_words\\":%lu' in console
    assert '\\"retained_task_stack_free_bytes\\":%lu' in console
    assert '\\"lvgl_used_pct\\":%u' in console
    assert '\\"internal_heap_free\\":%lu' in console
    assert '\\"internal_heap_min_free\\":%lu' in console
    assert '\\"internal_heap_largest_free\\":%lu' in console
    assert '\\"dma_heap_free\\":%lu' in console
    assert '\\"dma_heap_largest_free\\":%lu' in console
    assert '\\"boot_nonce\\":%lu' in console
    assert '\\"nvs_ready\\":%s' in console
    assert '\\"nvs_error\\":\\"%s\\"' in console


def test_version_reports_exact_actions_or_checkout_build_commit():
    root_cmake = read("CMakeLists.txt")
    cmake = read("main/CMakeLists.txt")
    provenance = read("cmake/d1l_source_provenance.cmake")
    console = read("main/comms/usb_console.c")

    assert "d1l_resolve_source_provenance(" in root_cmake
    assert '$ENV{D1L_SOURCE_SHA}' in provenance
    assert '$ENV{GITHUB_SHA}' in provenance
    assert "COMMAND git rev-parse --verify HEAD" in provenance
    assert "COMMAND git show -s --format=%ct HEAD" in provenance
    assert 'D1L_BUILD_GIT_COMMIT="${D1L_BUILD_GIT_COMMIT}"' in cmake
    assert "D1L_BUILD_EPOCH_SEC=${D1L_BUILD_EPOCH_SEC}ULL" in cmake
    assert '\\"build_commit\\":\\"%s\\"' in console
    assert '\\"build_epoch_sec\\":%lu' in console
    assert "D1L_BUILD_GIT_COMMIT" in console


def test_console_exposes_crashlog_commands():
    console = read("main/comms/usb_console.c")
    assert 'ok_begin("crashlog")' in console
    assert 'ok_begin("crashlog clear")' in console
    assert "d1l_crash_log_copy_recent" in console
    assert "d1l_crash_log_clear()" in console
    assert 'strcmp(line, "crashlog")' in console
    assert 'strcmp(line, "crashlog clear")' in console
    assert "crashlog" in SMOKE_COMMANDS
