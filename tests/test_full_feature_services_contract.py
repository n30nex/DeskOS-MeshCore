from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_full_feature_services_are_built_and_started():
    cmake = read("main/CMakeLists.txt")
    app_main = read("main/app_main.c")
    for source in (
        "comms/ble_companion_protocol.c",
        "comms/observer_manager.c",
        "diagnostics/event_log.c",
        "hal/display_preferences.c",
        "ui/ui_font_symbols_14.c",
        "ui/ui_service_sheets.c",
        "update/update_manager.c",
    ):
        assert f'"{source}"' in cmake
    for initialization in (
        "d1l_event_log_init();",
        "d1l_display_preferences_init();",
        "d1l_observer_manager_init();",
        "d1l_update_manager_init();",
    ):
        assert initialization in app_main


def test_observer_is_tls_qos1_bounded_and_privacy_scoped():
    observer = read("main/comms/observer_manager.c")
    ui = read("main/ui/ui_service_sheets.c")
    assert 'strncmp(uri, "mqtts://"' in observer
    assert ".verification.crt_bundle_attach = esp_crt_bundle_attach" in observer
    assert "esp_mqtt_client_enqueue(" in observer
    assert "s_client, topic, payload, 0, 1, 0, true" in observer
    assert "MQTT_EVENT_PUBLISHED" in observer
    assert "D1L_OBSERVER_QUEUE_CAPACITY" in observer
    assert "dropped_oldest" in observer
    assert "never message text, keys, contacts, or RF forwarding" in ui


def test_signed_update_is_dual_slot_verified_and_anti_rollback():
    update = read("main/update/update_manager.c")
    key = read("main/update/update_signing_key.h")
    partitions = read("partitions_d1l.csv")
    defaults = read("sdkconfig.defaults")
    assert 'D1L_UPDATE_SIGNER_KEY_ID "d1l-prod-8241789a002d0b50"' in key
    assert "D1L_UPDATE_SIGNING_PUBLIC_KEY[32]" in key
    assert "d1l_ed25519_signature_s_is_canonical(signature)" in update
    assert "ed25519_verify(signature, manifest_bytes, manifest_size" in update
    assert "manifest.security_sequence <= highest_sequence" in update
    assert "esp_ota_get_next_update_partition(NULL)" in update
    assert "esp_ota_set_boot_partition(target)" in update
    assert "esp_ota_mark_app_valid_cancel_rollback()" in update
    assert "esp_ota_mark_app_invalid_rollback_and_reboot()" in update
    assert "ota_0" in partitions and "ota_1" in partitions
    assert "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" in defaults


def test_device_service_sheets_and_notification_glyph_surfaces_are_live():
    phase1 = read("main/ui/ui_phase1.c")
    sheets = read("main/ui/ui_service_sheets.c")
    device = read("main/ui/ui_device_sheets.c")
    messages = read("main/ui/ui_messages.c")
    font = read("main/ui/ui_font_symbols_14.c")
    assert "d1l_ui_service_sheets_create(" in phase1
    for renderer in (
        "render_terminal",
        "render_observer",
        "render_update",
        "render_notifications",
        "render_admin",
    ):
        assert f"d1l_ui_service_sheets_{renderer}" in sheets
    assert "d1l_ui_device_sheets_render_display" in device
    assert "d1l_ui_device_sheets_render_diagnostics" in device
    assert "notification_quiet_now()" in phase1
    assert "s_last_notification_unread" in phase1
    assert "d1l_ui_font_symbols_14" in messages
    assert "const lv_font_t d1l_ui_font_symbols_14" in font
