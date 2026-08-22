from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_observer_region_is_editable_with_a_bounded_keyboard():
    header = read("main/ui/ui_service_sheets.h")
    sheets = read("main/ui/ui_service_sheets.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "D1L_UI_SERVICE_ACTION_OBSERVER_REGION_SAVE" in header
    assert "observer_region_textarea" in header
    assert "observer_keyboard" in header
    assert "d1l_ui_service_sheets_copy_observer_region" in header
    assert 'lv_textarea_set_max_length(controller->observer_region_textarea, 3U)' in sheets
    assert '"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"' in sheets
    assert "LV_KEYBOARD_MODE_TEXT_UPPER" in sheets
    assert "LV_EVENT_READY" in sheets
    assert "LV_EVENT_CANCEL" in sheets
    assert "d1l_ui_service_sheets_copy_observer_region(" in phase1
    assert "d1l_observer_set_region(region)" in phase1
    assert '"IATA region (3 letters)"' in sheets
    assert '"MQTT IATA saved"' in phase1
