from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_timezone_setting_is_fixed_offset_and_display_only():
    header = read("main/platform/time_display.h")
    source = read("main/platform/time_display.c")
    settings = read("main/app/settings_model.h")
    service = read("main/platform/time_service.c")
    core = read("main/platform/time_service_core.c")

    assert "D1L_TIMEZONE_SETTING_SCHEMA_VERSION 1U" in header
    assert "D1L_TIMEZONE_OFFSET_MINUTES_MIN (-720)" in header
    assert "D1L_TIMEZONE_OFFSET_MINUTES_MAX 840" in header
    assert "d1l_time_display_format_clock" in source
    assert "timezone_schema_version" in settings
    assert "timezone_offset_minutes" in settings
    assert "populate_timezone_status" in service
    assert "populate_display_time" in service
    assert "timezone_offset" not in core
    assert "d1l_time_display" not in core


def test_usb_and_ui_disclose_fixed_offset_and_no_automatic_dst():
    console = read("main/comms/usb_console.c")
    display_sheet = read("main/ui/ui_device_sheets.c")
    more = read("main/ui/ui_more_view.c")

    assert "settings set timezone <UTC|UTC+HH:MM|UTC-HH:MM>" in console
    assert '\\"model\\":\\"fixed_utc_offset\\"' in console
    assert '\\"auto_dst\\":false' in console
    assert "d1l_app_model_set_timezone_offset_minutes" in console
    assert "Use Time -1h/+1h to set the local clock." in display_sheet
    assert "Daylight saving changes are" in display_sheet
    assert "manual; radio and security timestamps stay UTC." in display_sheet
    assert "timezone_settings_ready" in more
    assert '"Time setting unavailable"' in more
