from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_storage_view_is_a_bounded_pure_component():
    header = read("main/ui/ui_storage_view.h")
    source = read("main/ui/ui_storage_view.c")
    cmake = read("main/CMakeLists.txt")

    assert '"ui/ui_storage_view.c"' in cmake
    assert "d1l_ui_storage_view_input_t" in header
    assert "d1l_ui_storage_view_model_t" in header
    assert "D1L_UI_STORAGE_LOCATION_COUNT 6U" in header
    assert "D1L_UI_STORAGE_VIEW_MODEL_MAX_BYTES" in header
    assert "_Static_assert(sizeof(d1l_ui_storage_view_model_t)" in source
    assert "d1l_ui_storage_view_model_is_valid" in source
    assert "bounded_text_is_terminated" in source
    assert "lvgl" not in source.lower()
    assert "d1l_app_model" not in source
    assert '#include "nvs' not in source
    assert "nvs_get_" not in source
    assert "nvs_set_" not in source
    assert "format" not in source.lower() or "format_kb" in source


def test_storage_sheet_consumes_only_the_owned_view_truth():
    phase1 = read("main/ui/ui_phase1.c")

    assert '#include "ui_storage_view.h"' in phase1
    assert "s_storage_view EXT_RAM_BSS_ATTR" in phase1
    assert "storage_view_input_from_snapshot" in phase1
    assert "d1l_ui_storage_view(&input, &s_storage_view)" in phase1
    assert "s_storage_view.hero" in phase1
    assert "s_storage_view.card_summary" in phase1
    assert "s_storage_view.data_summary" in phase1
    assert "s_storage_view.card" in phase1
    assert "s_storage_view.locations[index]" in phase1
    assert "d1l_ui_storage_location_available(location->location)" in phase1
    assert "visible_index" in phase1

    removed_helpers = (
        "storage_snapshot_needs_attention",
        "storage_card_state_friendly",
        "storage_filesystem_friendly",
        "storage_readiness_friendly",
        "storage_retained_backend_friendly",
        "storage_hero_copy_t",
    )
    for helper in removed_helpers:
        assert helper not in phase1


def test_storage_safety_boundary_remains_read_only_and_no_format():
    phase1 = read("main/ui/ui_phase1.c")
    view = read("main/ui/ui_storage_view.c")

    assert "FAT32 only - This device never formats cards." in phase1
    assert "Read-only card details" in phase1
    assert "format_kb" in view
    assert "esp_vfs_fat_sdcard_format" not in phase1
    assert "esp_vfs_fat_sdcard_format" not in view
    assert "f_mkfs" not in phase1
    assert "f_mkfs" not in view
