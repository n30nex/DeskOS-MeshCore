from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_connectivity_manager_keeps_companion_radios_default_off_and_persistent():
    header = read("main/comms/connectivity_manager.h")
    source = read("main/comms/connectivity_manager.c")
    policy_header = read("main/comms/wifi_retry_policy.h")
    policy_source = read("main/comms/wifi_retry_policy.c")
    cmake = read("main/CMakeLists.txt")
    defaults = read("sdkconfig.defaults")
    assert "d1l_connectivity_status_t" in header
    assert "d1l_connectivity_set_wifi_enabled" in header
    assert "d1l_connectivity_wifi_scan" in header
    assert "d1l_connectivity_wifi_connect" in header
    assert "d1l_connectivity_wifi_disconnect" in header
    assert "D1L_WIFI_SCAN_MAX" in header
    assert "d1l_connectivity_set_ble_enabled" in header
    assert "d1l_connectivity_save_wifi_profile" in header
    assert "d1l_connectivity_clear_wifi_profile" in header
    assert "d1l_connectivity_prepare_reboot" in header
    assert "wifi_profile_saved" in header
    assert "wifi_password_saved" in header
    assert "wifi_ssid" in header
    assert "wifi_retry_scheduled" in header
    assert "wifi_retry_attempt" in header
    assert "wifi_retry_delay_ms" in header
    assert "wifi_retry_task_stack_high_water_bytes" in header
    assert "wifi_last_disconnect_reason" in header
    assert "wifi_last_failure_class" in header
    assert "wifi_boot_guard_ready" in header
    assert "wifi_consecutive_crash_boots" in header
    assert "wifi_crash_loop_detected" in header
    assert "wifi_last_active_subsystem" in header
    assert '"offline_first_one_companion_radio"' in source
    assert "settings.wifi_enabled = true" in source
    assert "settings.ble_companion_enabled = enabled" in source
    assert "if (enabled && !build_wifi_enabled())" in source
    assert "if (enabled && !build_ble_enabled())" in source
    assert "settings.ble_companion_enabled = false" in source
    assert "settings.wifi_enabled = false" in source
    for state in [
        "off",
        "profile_required",
        "starting",
        "scanning",
        "connecting",
        "connected",
        "auth_failed",
        "ap_unavailable",
        "retry_backoff",
        "safe_mode_disabled",
    ]:
        assert f'"{state}"' in policy_source
    assert "D1L_WIFI_RETRY_BASE_DELAY_MS 1000U" in policy_header
    assert "D1L_WIFI_RETRY_MAX_DELAY_MS 30000U" in policy_header
    assert "D1L_WIFI_RETRY_MAX_WINDOW_MS 300000U" in policy_header
    assert "D1L_WIFI_RETRY_MAX_ATTEMPTS 8U" in policy_header
    assert "D1L_WIFI_CONTROL_LOCK_TIMEOUT_MS 10000U" in source
    assert "d1l_ble_companion_start()" in source
    assert "d1l_ble_companion_stop()" in source
    assert "d1l_ble_companion_status(&ble_status)" in source
    assert "d1l_settings_save_wifi_profile(ssid, password)" in source
    assert "d1l_settings_clear_wifi_profile()" in source
    assert "ESP_NETIF_DEFAULT_WIFI_STA()" in source
    assert "esp_netif_new(&netif_config)" in source
    assert "esp_netif_attach_wifi_station" in source
    assert "esp_wifi_set_default_wifi_sta_handlers" in source
    assert "esp_netif_create_default_wifi_sta" not in source
    assert "esp_wifi_set_mode(WIFI_MODE_STA)" in source
    assert "esp_wifi_scan_start(NULL, true)" in source
    assert "esp_wifi_scan_get_ap_records" in source
    assert "esp_wifi_set_config(WIFI_IF_STA, &config)" in source
    assert "copy_wifi_config_field(config.sta.ssid" in source
    assert "copy_wifi_config_field(config.sta.password" in source
    assert "snprintf((char *)config.sta.ssid" not in source
    assert "snprintf((char *)config.sta.password" not in source
    assert "esp_wifi_connect()" in source
    assert "WIFI_STORAGE_RAM" in source
    assert "ESP_ERR_NOT_SUPPORTED" in source
    assert "CONFIG_ESP_WIFI_ENABLED" in source
    assert "d1l_ble_companion_build_enabled()" in source
    assert "esp_wifi" in cmake
    assert "esp_netif" in cmake
    assert "esp_event" in cmake
    assert '"comms/connectivity_manager.c"' in cmake
    assert '"comms/connectivity_boot_guard.c"' in cmake
    assert '"comms/wifi_retry_policy.c"' in cmake
    assert "CONFIG_ESP_WIFI_ENABLED=y" in defaults
    assert "CONFIG_BT_ENABLED=y" in defaults
    assert "CONFIG_BT_NIMBLE_ENABLED=y" in defaults
    assert "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y" in defaults
    assert "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y" in defaults

    reset = source[
        source.index("esp_err_t d1l_connectivity_prepare_reboot"):
        source.index("esp_err_t d1l_connectivity_init")
    ]
    assert "if (s_wifi_initialized)" in reset
    assert "esp_wifi_stop()" in reset
    assert "return ble_ret == ESP_OK ? ret : ble_ret" in reset
    assert "s_wifi_started = false" in reset
    assert "s_wifi_connected = false" in reset
    assert "s_wifi_connecting = false" in reset
    assert "s_wifi_ip[0] = '\\0'" in reset
    assert "return ble_ret" in reset
    assert "d1l_settings" not in reset
    assert "esp_wifi_deinit" not in reset
    assert "d1l_ble_companion_prepare_reboot()" in reset

    connect = source[
        source.index("esp_err_t d1l_connectivity_wifi_connect"):
        source.index("esp_err_t d1l_connectivity_wifi_disconnect")
    ]
    assert connect.index("esp_wifi_disconnect()") < connect.index("esp_wifi_set_config(WIFI_IF_STA, &config)")


def test_wifi_memory_policy_and_failed_start_are_fail_closed():
    source = read("main/comms/connectivity_manager.c")
    app_main = read("main/app_main.c")
    defaults = read("sdkconfig.defaults")

    for setting in [
        "CONFIG_SPIRAM_USE_MALLOC=y",
        "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024",
        "CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y",
        "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536",
        "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y",
        "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6",
        "CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32",
        "CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y",
        "CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=6",
        "CONFIG_ESP_WIFI_RX_BA_WIN=6",
        "# CONFIG_ESP_WIFI_IRAM_OPT is not set",
        "# CONFIG_ESP_WIFI_RX_IRAM_OPT is not set",
    ]:
        assert setting in defaults

    assert "fail_closed_wifi_runtime" in source
    assert "d1l_settings_public_snapshot(&safe_settings)" in source
    assert "safe_settings.wifi_enabled = false" in source
    rollback = source[source.index("fail_closed_wifi_runtime"):source.index("wifi_boot_recovery_reason")]
    assert "d1l_settings_clear_wifi_profile" not in rollback
    assert "rollback_save_failed" in source
    assert rollback.index("d1l_settings_update_fields(") < rollback.index(
        "clear_boot_guard()"
    )
    assert "boot_recovery_panic" in source
    assert "boot_recovery_wdt" in source
    assert "boot_recovery_brownout" in source
    boot_recovery = source[
        source.index("esp_err_t d1l_connectivity_init"):
        source.index("void d1l_connectivity_status")
    ]
    assert "d1l_wifi_retry_policy_enter_safe_mode" in boot_recovery
    assert "load_and_advance_boot_guard" in boot_recovery
    assert "repeated_wifi_crash" in boot_recovery
    assert "decision.crash_loop_detected" in boot_recovery
    assert "mark_boot_guard_active" not in boot_recovery
    assert '"boot_recovery_wifi_crash_loop"' in boot_recovery
    assert '"boot_guard_unavailable"' in boot_recovery
    assert "safe_settings.wifi_enabled" not in boot_recovery
    assert "d1l_settings_save" not in boot_recovery.split("#ifdef CONFIG_ESP_WIFI_ENABLED", 1)[1]
    assert "esp_event_handler_instance_unregister" in source
    assert "esp_wifi_deinit()" in source
    assert "esp_netif_destroy_default_wifi" in source
    teardown = source[source.index("static void stop_wifi_runtime"):source.index("static esp_err_t fail_closed_wifi_runtime")]
    initialized_cleanup = teardown[teardown.index("if (s_wifi_initialized)"):]
    assert initialized_cleanup.index("esp_wifi_stop()") < initialized_cleanup.index("esp_wifi_deinit()")

    enable = source[
        source.index("esp_err_t d1l_connectivity_set_wifi_enabled"):
        source.index("esp_err_t d1l_connectivity_set_ble_enabled")
    ]
    assert enable.index("ensure_wifi_started()") < enable.index("settings.wifi_enabled = true")
    assert app_main.index("d1l_board_init()") < app_main.index("d1l_connectivity_init()")
    assert app_main.index("d1l_connectivity_init()") < app_main.index("d1l_ui_phase1_start()")


def test_connectivity_boot_guard_is_scoped_checksummed_and_not_written_from_events():
    source = read("main/comms/connectivity_manager.c")
    guard_header = read("main/comms/connectivity_boot_guard.h")
    guard_source = read("main/comms/connectivity_boot_guard.c")

    assert "D1L_CONNECTIVITY_BOOT_GUARD_SCHEMA 1U" in guard_header
    assert "D1L_CONNECTIVITY_BOOT_GUARD_FAILURE_LIMIT 2U" in guard_header
    assert "consecutive_crash_boots" in guard_header
    assert "last_active_subsystem" in guard_header
    assert "record_checksum" in guard_source
    assert "0xEDB88320U" in guard_source
    assert "UINT8_MAX" in guard_source
    assert "UINT32_MAX" in guard_source
    for subsystem in ["none", "wifi"]:
        assert f'"{subsystem}"' in guard_source

    assert 'D1L_CONNECTIVITY_GUARD_NAMESPACE "d1l_conn"' in source
    assert 'D1L_CONNECTIVITY_GUARD_KEY "boot_guard"' in source
    assert "nvs_get_blob" in source
    assert "nvs_set_blob" in source
    assert "nvs_commit" in source
    assert "ESP_ERR_NVS_INVALID_LENGTH" in source
    assert "reset_reason_is_crash_like" in source
    assert "decision.previous_active_subsystem" in source
    assert "conservative_first_recovery" in source
    assert "(void)clear_boot_guard()" not in source
    assert "D1L_CONNECTIVITY_GUARD_LOCK_TIMEOUT_MS 10000U" in source
    assert "D1L_CONNECTIVITY_GUARD_STABLE_WINDOW_MS 30000U" in source
    assert "s_boot_guard_lock = xSemaphoreCreateMutex()" in source
    assert "xSemaphoreTake(s_boot_guard_lock, timeout_ticks)" in source
    assert "portMAX_DELAY" not in source[
        source.index("static esp_err_t take_boot_guard_lock"):
        source.index("static void give_boot_guard_lock")
    ]
    assert "d1l_connectivity_boot_guard_acknowledge_stable" in guard_source
    assert "record->last_active_subsystem = D1L_CONNECTIVITY_SUBSYSTEM_NONE" in guard_source

    event_handler = source[
        source.index("static void wifi_event_handler"):
        source.index("static void stop_wifi_runtime")
    ]
    assert "nvs_" not in event_handler
    assert "persist_boot_guard_record" not in event_handler
    assert "s_boot_guard_stable_ack_due_ms" in event_handler
    assert "cancel_boot_guard_stable_ack" in event_handler
    stable_due = source[
        source.index("static bool take_boot_guard_stable_ack_if_due"):
        source.index("static uint64_t wifi_now_ms")
    ]
    assert "D1L_WIFI_RUNTIME_CONNECTED" in stable_due
    assert "D1L_WIFI_RUNTIME_SCANNING" in stable_due
    worker = source[
        source.index("static void wifi_retry_worker"):
        source.index("static esp_err_t ensure_wifi_retry_worker")
    ]
    assert "take_boot_guard_stable_ack_if_due" in worker
    assert "acknowledge_boot_guard_stable" in worker
    connect = source[
        source.index("esp_err_t d1l_connectivity_wifi_connect"):
        source.index("esp_err_t d1l_connectivity_wifi_disconnect")
    ]
    assert connect.index("mark_boot_guard_active") < connect.index(
        "ensure_wifi_started()"
    )
    assert "cancel_boot_guard_stable_ack" in connect
    cancelled_connect = connect[connect.index("if (!still_connecting)"):]
    assert "clear_boot_guard()" in cancelled_connect
    scan = source[
        source.index("esp_err_t d1l_connectivity_wifi_scan"):
        source.index("esp_err_t d1l_connectivity_wifi_connect")
    ]
    assert scan.index("d1l_wifi_retry_policy_scan_allowed") < scan.index(
        "mark_boot_guard_active"
    )
    assert scan.index("mark_boot_guard_active") < scan.index(
        "ensure_wifi_started"
    ) < scan.index("d1l_wifi_retry_policy_begin_scan")
    disable = source[
        source.index("esp_err_t d1l_connectivity_set_wifi_enabled"):
        source.index("esp_err_t d1l_connectivity_set_ble_enabled")
    ]
    assert "clear_boot_guard()" in disable
    disable_path = disable[disable.index("if (!enabled)"):disable.index(
        "settings.ble_companion_enabled = false"
    )]
    assert disable_path.index("d1l_settings_update_fields(") < disable_path.index(
        "stop_wifi_runtime()"
    ) < disable_path.index("clear_boot_guard()")
    ble_toggle = source[
        source.index("esp_err_t d1l_connectivity_set_ble_enabled"):
        source.index("esp_err_t d1l_connectivity_save_wifi_profile")
    ]
    assert ble_toggle.index("stop_wifi_runtime()") < ble_toggle.index(
        "clear_boot_guard()"
    )
    assert "mark_boot_guard_active" not in ble_toggle
    clear_profile = source[source.index("esp_err_t d1l_connectivity_clear_wifi_profile"):]
    assert "d1l_connectivity_wifi_disconnect()" in clear_profile
    assert "(void)d1l_connectivity_wifi_disconnect()" not in clear_profile
    assert "disconnect_ret != ESP_OK" in clear_profile


def test_disconnect_handler_classifies_then_defers_bounded_retry_to_worker():
    source = read("main/comms/connectivity_manager.c")
    policy = read("main/comms/wifi_retry_policy.c")

    handler = source[
        source.index("static void wifi_event_handler"):
        source.index("static void stop_wifi_runtime")
    ]
    disconnect = handler[
        handler.index("WIFI_EVENT_STA_DISCONNECTED"):
        handler.index("WIFI_EVENT_STA_START")
    ]
    assert "classify_disconnect_reason" in disconnect
    assert "d1l_wifi_retry_policy_on_disconnect" in disconnect
    assert "notify_wifi_retry_worker" in disconnect
    assert "esp_wifi_connect()" not in disconnect
    assert "vTaskDelay" not in disconnect
    assert "ulTaskNotifyTake" not in disconnect

    worker = source[
        source.index("static void wifi_retry_worker"):
        source.index("static esp_err_t ensure_wifi_retry_worker")
    ]
    assert "ulTaskNotifyTake" in worker
    assert "d1l_wifi_retry_policy_begin_due_retry" in worker
    assert "esp_wifi_connect()" in worker
    assert "D1L_WIFI_FAILURE_TRANSIENT" in worker
    assert worker.index("take_wifi_control()") < worker.index("esp_wifi_connect()")
    control_lock = source[
        source.index("static bool take_wifi_control"):
        source.index("static void give_wifi_control")
    ]
    assert "D1L_WIFI_CONTROL_LOCK_TIMEOUT_MS" in control_lock
    assert "portMAX_DELAY" not in control_lock

    for reason in [
        "WIFI_REASON_AUTH_FAIL",
        "WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT",
        "WIFI_REASON_HANDSHAKE_TIMEOUT",
        "WIFI_REASON_NO_AP_FOUND",
        "WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD",
    ]:
        assert reason in source
    assert "D1L_WIFI_RUNTIME_AUTH_FAILED" in policy
    assert "D1L_WIFI_RUNTIME_AP_UNAVAILABLE" in policy
    assert "D1L_WIFI_RUNTIME_RETRY_BACKOFF" in policy


def test_user_cancel_transient_retry_and_credentials_are_explicitly_bounded():
    source = read("main/comms/connectivity_manager.c")
    policy = read("main/comms/wifi_retry_policy.c")

    disconnect = source[
        source.index("esp_err_t d1l_connectivity_wifi_disconnect"):
        source.index("esp_err_t d1l_connectivity_set_wifi_enabled")
    ]
    assert "d1l_wifi_retry_policy_cancel" in disconnect
    assert disconnect.index("d1l_wifi_retry_policy_cancel") < disconnect.index(
        "esp_wifi_disconnect()"
    )
    assert disconnect.index("take_wifi_control()") < disconnect.index(
        "esp_wifi_disconnect()"
    )
    assert 'set_wifi_last_error("cancelled")' in disconnect

    runtime_failure = source[
        source.index("static esp_err_t fail_closed_wifi_runtime"):
        source.index("static const char *wifi_boot_recovery_reason")
    ]
    assert "safe_settings.wifi_enabled = false" in runtime_failure
    handler = source[
        source.index("static void wifi_event_handler"):
        source.index("static void stop_wifi_runtime")
    ]
    assert "safe_settings.wifi_enabled = false" not in handler
    assert "d1l_settings_save" not in handler

    connect = source[
        source.index("esp_err_t d1l_connectivity_wifi_connect"):
        source.index("esp_err_t d1l_connectivity_wifi_disconnect")
    ]
    assert "wipe_wifi_config(&config)" in connect
    wipe_config = source.split("static void wipe_wifi_config", 1)[1].split(
        "static void finish_wifi_scan_policy", 1
    )[0]
    assert "volatile uint8_t *bytes" in wipe_config
    assert "d1l_settings_public_snapshot(&settings)" in connect
    assert "d1l_settings_wifi_secret_snapshot(&wifi_secret)" in connect
    assert "d1l_settings_wifi_secret_wipe(&wifi_secret)" in connect
    assert "settings.wifi_password" not in connect
    secret_snapshot = connect.index("d1l_settings_wifi_secret_snapshot")
    password_copy = connect.index("wifi_secret.wifi_password")
    wipe_after_copy = connect.index(
        "d1l_settings_wifi_secret_wipe", password_copy
    )
    assert secret_snapshot < password_copy < wipe_after_copy < connect.index(
        "mark_boot_guard_active"
    )
    assert "d1l_wifi_retry_policy_begin_manual_connect" in connect
    assert "d1l_wifi_retry_policy_on_disconnect" in connect
    assert "wifi_password" not in policy
    assert "s_wifi_suppress_disconnect_retry" not in source
    assert "s_wifi_local_leave_token" in source
    assert "arm_local_leave_token()" in connect
    assert "generation" not in connect
    consume = source[
        source.index("static bool consume_expected_local_leave"):
        source.index("static bool take_wifi_control")
    ]
    assert "d1l_wifi_local_leave_token_consume" in consume
    assert "reason == WIFI_REASON_ASSOC_LEAVE" in consume
    disconnect_event = source[
        source.index("WIFI_EVENT_STA_DISCONNECTED"):
        source.index("WIFI_EVENT_STA_START")
    ]
    assert "consume_expected_local_leave(reason)" in disconnect_event
    assert disconnect_event.index("if (suppress_retry)") < disconnect_event.index(
        "d1l_wifi_retry_policy_on_disconnect"
    )
    after_connect = connect.split("ret = esp_wifi_connect();", 1)[1]
    before_failure = after_connect.split("if (ret != ESP_OK)", 1)[0]
    failure_branch = after_connect.split("if (ret != ESP_OK)", 1)[1]
    assert "clear_local_leave_token()" not in before_failure
    assert "clear_local_leave_token()" in failure_branch.split(
        "d1l_wifi_retry_policy_on_disconnect", 1
    )[0]
    got_ip = source[
        source.index("IP_EVENT_STA_GOT_IP"):
        source.index("static void stop_wifi_runtime")
    ]
    assert "clear_local_leave_token()" in got_ip
    assert "connection_accepted" in got_ip
    assert got_ip.index("d1l_wifi_retry_policy_connected") < got_ip.index(
        "s_wifi_connected = true"
    )
    assert "if (!connection_accepted)" in got_ip
    assert "s_wifi_ip[0] = '\\0'" in got_ip
    assert "Only an ASSOC_LEAVE event may consume it as intentional" in connect


def test_console_reports_wifi_ble_status_scan_and_connect_without_password_echo():
    console = read("main/comms/usb_console.c")
    assert "cmd_wifi_status" in console
    assert "cmd_wifi_scan" in console
    assert "cmd_wifi_connect" in console
    assert "cmd_wifi_save" in console
    assert "cmd_wifi_clear" in console
    assert "cmd_ble_status" in console
    assert "WIFI_BUILD_DISABLED" in console
    assert "BLE_BUILD_DISABLED" in console
    assert "BLE_RUNTIME_PENDING" in console
    assert '\\"profile_saved\\":%s' in console
    assert '\\"password_saved\\":%s' in console
    assert "wifi save <ssid> [password]" in console
    assert "wifi connect" in console
    assert "password is not printed" in console
    wifi_save = console.split("static void cmd_wifi_save", 1)[1].split(
        "static void cmd_wifi_clear", 1
    )[0]
    save_call = wifi_save.index("d1l_connectivity_save_wifi_profile")
    password_wipe = wifi_save.index(
        "wipe_console_bytes(password, sizeof(password))"
    )
    error_branch = wifi_save.index("if (ret != ESP_OK)")
    assert save_call < password_wipe < error_branch
    assert "const char *password_to_save = password" in wifi_save
    wipe_helper = console.split("static void wipe_console_bytes", 1)[1].split(
        "static uint32_t deadline_remaining_ms", 1
    )[0]
    assert "volatile uint8_t *bytes" in wipe_helper
    console_loop = console.split("void d1l_usb_console_run(void)", 1)[1]
    assert console_loop.count("wipe_console_bytes(line, sizeof(line))") >= 4
    assert 'err_result("unknown", "UNKNOWN_COMMAND"' in console
    assert "err_result(line, \"UNKNOWN_COMMAND\"" not in console
    assert "could not fully save or apply Wi-Fi profile; inspect wifi status" in console
    assert "could not persist disabled state or clear connectivity crash guard" in console
    assert "could not fully clear Wi-Fi profile, disconnect runtime, or connectivity crash guard" in console
    assert "could not persist BLE disabled setting" in console
    assert "d1l_connectivity_wifi_scan(&scan)" in console
    assert "d1l_connectivity_wifi_connect()" in console
    assert '\\"networks\\":[' in console
    assert '\\"password_printed\\":false' in console
    assert '\\"connected\\":%s' in console
    assert '\\"ip\\":' in console
    for telemetry in [
        "retry_scheduled",
        "retry_attempt",
        "retry_delay_ms",
        "user_cancelled",
        "safe_mode",
        "last_disconnect_reason",
        "last_failure_class",
        "retry_task_stack_high_water_bytes",
        "boot_guard_ready",
        "boot_guard_recovered",
        "consecutive_crash_boots",
        "crash_loop_detected",
        "last_active_subsystem",
        "boot_guard_error",
    ]:
        assert f'\\"{telemetry}\\"' in console
    assert '\\"coexistence_policy\\"' in console
