from pathlib import Path

from scripts.smoke_d1l import SMOKE_COMMANDS


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


def test_full_feature_smoke_reads_every_service_surface_without_mutation():
    for command in (
        "channels",
        "admin status",
        "logs",
        "terminal status",
        "observer status",
        "update status",
    ):
        assert command in SMOKE_COMMANDS
    assert not any(
        command.startswith(
            (
                "admin login ",
                "admin clear-stats",
                "admin advertise-zero-hop",
                "logs clear",
                "observer configure",
                "observer on",
                "observer off",
                "observer clear",
                "update install",
                "update cancel",
                "update reboot",
            )
        )
        for command in SMOKE_COMMANDS
    )


def test_server_admin_login_is_available_on_device_and_scrubs_credentials():
    header = read("main/ui/ui_service_sheets.h")
    sheets = read("main/ui/ui_service_sheets.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "D1L_UI_SERVICE_ACTION_ADMIN_LOGIN" in header
    assert "admin_password_textarea" in header
    assert "admin_keyboard" in header
    assert "d1l_ui_service_sheets_take_admin_password" in header

    assert "lv_textarea_set_password_mode(" in sheets
    assert "D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES" in sheets
    assert "Password (empty allowed by peer)" in sheets
    assert "d1l_ui_keyboard_configure_input(" in sheets
    assert "d1l_ui_keyboard_clear_textarea(controller->admin_keyboard)" in sheets
    assert 'lv_textarea_set_text(controller->admin_password_textarea, "")' in \
        sheets
    assert "clear_admin_sensitive_input(controller);" in sheets
    assert (
        "Select a repeater or room in Nodes, open its details, then tap Admin."
    ) in sheets
    assert "Select a repeater or room in Network" not in sheets
    assert "Login is accepted only over local USB" not in sheets

    login = phase1.split(
        "case D1L_UI_SERVICE_ACTION_ADMIN_LOGIN:", 1
    )[1].split(
        "case D1L_UI_SERVICE_ACTION_ADMIN_REFRESH:", 1
    )[0]
    assert "d1l_ui_service_sheets_take_admin_password(" in login
    assert "d1l_meshcore_service_admin_login(" in login
    assert "d1l_meshcore_admin_secure_zero(password, sizeof(password));" in login
    assert "show_toast_text(password" not in login
    assert "ESP_LOG" not in login


def test_server_admin_close_preserves_session_and_target_change_logs_out():
    phase1 = read("main/ui/ui_phase1.c")
    sheets = read("main/ui/ui_service_sheets.c")
    close = phase1.split(
        "case D1L_UI_SERVICE_ACTION_CLOSE_ADMIN:", 1
    )[1].split(
        "case D1L_UI_SERVICE_ACTION_TERMINAL_LEVEL:", 1
    )[0]
    switch = phase1.split(
        "case D1L_UI_NODE_DETAIL_ACTION_OPEN_ADMIN:", 1
    )[1].split(
        "case D1L_UI_NODE_DETAIL_ACTION_NONE:", 1
    )[0]
    assert "d1l_meshcore_service_admin_logout();" not in close
    assert "hide_service_sheets();" in close
    assert "strcmp(admin_status.fingerprint," in switch
    assert "d1l_meshcore_service_admin_logout();" in switch

    begin_render = sheets.split(
        "static bool begin_render(", 1
    )[1].split(
        "static void finish_admin_render(", 1
    )[0]
    finish_render = sheets.split(
        "static void finish_admin_render(", 1
    )[1].split(
        "static bool render_header(", 1
    )[0]
    assert begin_render.index("lv_obj_get_scroll_y(sheet)") < \
        begin_render.index("lv_obj_clean(sheet)")
    assert "lv_obj_update_layout(sheet);" in finish_render
    assert "lv_obj_scroll_to_y(" in finish_render
    assert "finish_admin_render(controller, sheet);" in sheets


def test_authenticated_server_management_is_available_on_device_and_bounded():
    header = read("main/ui/ui_service_sheets.h")
    sheets = read("main/ui/ui_service_sheets.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "D1L_UI_SERVICE_ACTION_ADMIN_CLI_SEND" in header
    assert "D1L_UI_SERVICE_ACTION_ADMIN_CLI_SECURE_TOGGLE" in header
    assert "admin_cli_textarea" in header
    assert "d1l_ui_service_sheets_take_admin_cli" in header

    assert "D1L_MESHCORE_ADMIN_MAX_CLI_COMMAND_BYTES" in sheets
    assert "Authenticated server command" in sheets
    assert "Read-only commands send immediately." in sheets
    assert "Secure Input for passwords, secrets or private keys." in sheets
    assert "status->cli_reply" in sheets
    assert "D1L_MESHCORE_ADMIN_CLI_PENDING" in sheets

    cli = phase1.split(
        "case D1L_UI_SERVICE_ACTION_ADMIN_CLI_SEND:", 1
    )[1].split(
        "case D1L_UI_SERVICE_ACTION_ADMIN_LOGOUT:", 1
    )[0]
    assert "d1l_ui_service_sheets_take_admin_cli(" in cli
    assert "d1l_meshcore_admin_cli_command_sensitive(command)" in cli
    assert "d1l_meshcore_admin_cli_command_read_only(command)" in cli
    assert "d1l_meshcore_service_admin_request_cli(command, false)" in cli
    assert "d1l_meshcore_service_admin_request_cli(command, true)" in cli
    assert "Tap Confirm Command within 5 seconds" in cli
    assert cli.count(
        "d1l_meshcore_admin_secure_zero(command, sizeof(command));"
    ) >= 4
    assert "show_toast_text(command" not in cli


def test_acl_room_controls_and_usb_admin_surface_are_complete():
    header = read("main/ui/ui_service_sheets.h")
    sheets = read("main/ui/ui_service_sheets.c")
    phase1 = read("main/ui/ui_phase1.c")
    usb = read("main/comms/usb_console.c")

    for required in (
        "D1L_UI_SERVICE_ACTION_ADMIN_ACL_APPLY",
        "D1L_UI_SERVICE_ACTION_ADMIN_ROOM_READ_ONLY_ON",
        "D1L_UI_SERVICE_ACTION_ADMIN_ROOM_READ_ONLY_OFF",
        "admin_acl_textarea",
        "d1l_ui_service_sheets_take_admin_acl",
    ):
        assert required in header
    assert "Access-list editor" in sheets
    assert "0 remove, 1 read, 2 write, 3 admin" in sheets
    assert "Room guest access" in sheets
    assert "Controls allow.read.only." in sheets
    assert "unsupported, serial-only, OTA, reboot and power commands fail closed" \
        in sheets

    assert "D1L_UI_ADMIN_CLI_ORIGIN_ACL" in phase1
    assert "D1L_UI_ADMIN_CLI_ORIGIN_ROOM_READ_ONLY_ON" in phase1
    assert "D1L_UI_ADMIN_CLI_ORIGIN_ROOM_READ_ONLY_OFF" in phase1
    assert "case D1L_UI_SERVICE_ACTION_ADMIN_ACL_APPLY:" in phase1
    assert "d1l_meshcore_admin_format_acl_command(" in phase1
    assert '"set allow.read.only on"' in phase1
    assert '"set allow.read.only off"' in phase1
    assert "admin_cli_allowed_for_current_session(command)" in phase1

    assert "static void cmd_admin_cli(const char *line)" in usb
    assert "static void cmd_admin_room_post(const char *line)" in usb
    assert '"admin cli <documented-command> ' in usb
    assert '[CONFIRM-REMOTE-MUTATION]' in usb
    assert "admin room-post <text>" in usb
    admin_cli = usb.split(
        "static void cmd_admin_cli(const char *line)", 1
    )[1].split(
        "static void cmd_admin_room_post(const char *line)", 1
    )[0]
    assert "D1L_MESHCORE_ADMIN_CLI_SENSITIVE" in admin_cli
    assert "sensitive remote commands require on-device Secure Input" in \
        admin_cli
    assert "d1l_meshcore_service_admin_request_cli(" in admin_cli
    assert "print_json_string(remote_command)" not in admin_cli


def test_authenticated_room_console_receives_acks_and_posts_on_device():
    header = read("main/ui/ui_service_sheets.h")
    sheets = read("main/ui/ui_service_sheets.c")
    phase1 = read("main/ui/ui_phase1.c")
    service = read("main/mesh/meshcore_service.c")

    assert "D1L_UI_SERVICE_ACTION_ADMIN_ROOM_SEND" in header
    assert "admin_room_textarea" in header
    assert "d1l_ui_service_sheets_take_admin_room_post" in header
    assert "d1l_ui_service_sheets_admin_edit_has_text" in header
    assert "Live room console" in sheets
    assert "Guest permission: live room posts are read-only." in sheets
    assert "D1L_MESHCORE_ADMIN_PERMISSION_WRITE" in sheets
    assert "lv_keyboard_get_textarea(" in sheets

    room_send = phase1.split(
        "case D1L_UI_SERVICE_ACTION_ADMIN_ROOM_SEND:", 1
    )[1].split(
        "case D1L_UI_SERVICE_ACTION_ADMIN_CLI_SECURE_TOGGLE:", 1
    )[0]
    assert "d1l_ui_service_sheets_take_admin_room_post(" in room_send
    assert "d1l_meshcore_service_admin_send_room_post(text)" in room_send
    assert "d1l_meshcore_admin_secure_zero(text, sizeof(text));" in room_send
    assert "build_admin_room_transcript(&status);" in phase1
    assert "dm_stats.content_revision != s_admin_rendered_dm_revision" in phase1

    dm_rx = service.split(
        "static bool parse_rx_dm_packet", 1
    )[1].split(
        "typedef enum {\n    D1L_RX_ACK_UNMATCHED", 1
    )[0]
    for required in (
        "D1L_MESHCORE_TXT_TYPE_SIGNED_PLAIN",
        "d1l_meshcore_admin_runtime_capture_active(",
        "d1l_meshcore_admin_runtime_note_room_activity(",
        "wire_message_len",
        "settings->identity_public_key",
        "ack_wire_len",
        'room_post ? "room_post" : "dm_text"',
        "d1l_dm_store_append_rx_identity_deferred(",
    ):
        assert required in dm_rx
    assert "room_post ? 4U : D1L_MESHCORE_DM_ACK_WIRE_BYTES" in dm_rx

    public_send = service.split(
        "esp_err_t d1l_meshcore_service_admin_send_room_post", 1
    )[1].split(
        "esp_err_t d1l_meshcore_service_admin_logout", 1
    )[0]
    assert "D1L_MESHCORE_ADMIN_PERMISSION_WRITE" in public_send
    assert "d1l_meshcore_service_send_dm(fingerprint, text)" in public_send


def test_authenticated_server_data_is_available_and_paged_on_device():
    header = read("main/ui/ui_service_sheets.h")
    sheets = read("main/ui/ui_service_sheets.c")
    phase1 = read("main/ui/ui_phase1.c")
    service_h = read("main/mesh/meshcore_service.h")
    console = read("main/comms/usb_console.c")

    for action in (
        "D1L_UI_SERVICE_ACTION_ADMIN_TELEMETRY",
        "D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS",
        "D1L_UI_SERVICE_ACTION_ADMIN_NEIGHBOURS_NEXT",
        "D1L_UI_SERVICE_ACTION_ADMIN_ACCESS_LIST",
    ):
        assert action in header
        assert action in phase1
    for label in (
        "Authenticated server data",
        "Telemetry",
        "Neighbours",
        "Access List",
        "Next Neighbours",
    ):
        assert label in sheets
    assert "status->query_result.text" in sheets
    assert "status->query_result.offset" in sheets
    assert "status->query_result.total" in sheets
    assert "d1l_meshcore_service_admin_request_query" in service_h
    assert "d1l_meshcore_service_admin_request_query(query, offset)" in phase1
    assert '"admin telemetry"' in console
    assert "admin neighbours [offset]" in console
    assert '"admin access-list"' in console


def test_admin_edit_fields_are_not_destroyed_by_periodic_refresh():
    phase1 = read("main/ui/ui_phase1.c")
    timer = phase1.split(
        "static void refresh_timer_cb", 1
    )[1].split(
        "static void create_top_bar", 1
    )[0]
    conditional = phase1.split(
        "static void refresh_admin_service_sheet_if_changed", 1
    )[1].split(
        "static void service_sheets_action_handler", 1
    )[0]
    assert "refresh_admin_service_sheet_if_changed();" in timer
    assert "(void)render_admin_service_sheet();" not in timer
    assert "status.generation != s_admin_rendered_generation" in conditional
    assert "mutation_expired || cli_expired" in conditional
    assert "d1l_ui_service_sheets_admin_edit_has_text(" in conditional
    assert "room_content_changed && !edit_in_progress" in conditional


def test_logs_snapshot_uses_psram_instead_of_console_task_stack():
    console = read("main/comms/usb_console.c")
    function = console.split("static void cmd_logs(void)", 1)[1].split(
        "static void cmd_logs_clear", 1
    )[0]
    assert "d1l_event_log_entry_t entries[D1L_EVENT_LOG_CAPACITY]" not in function
    assert "heap_caps_calloc(" in function
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in function
    assert "heap_caps_free(entries);" in function
    assert 'err_result("logs", "ESP_ERR_NO_MEM"' in function
