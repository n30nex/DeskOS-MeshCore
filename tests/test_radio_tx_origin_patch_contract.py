from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def added_patch_lines(patch: str) -> str:
    return "\n".join(
        line[1:]
        for line in patch.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )


def test_seeed_driver_latches_tx_origin_at_the_physical_irq_boundary():
    patch = read("patches/sensecap_indicator_tx_origin.patch")
    added = added_patch_lines(patch)

    assert "components/lora/radio.c" in patch
    assert "components/lora/radio.h" in patch
    assert "components/lora/sx126x_sensecap_board.c" in patch
    assert "void RadioSendWithOrigin" in added
    assert "__atomic_store_n( &RadioTxOrigin, origin, __ATOMIC_RELEASE );" in added
    assert "SX126xSendPayload( buffer, size, hardwareTimeout );" in added
    assert "TimerStart( &TxTimeoutTimer );" not in added

    # The board ISR captures A's generation into the queue element before B
    # can replace the driver's current origin. Delayed queue delivery therefore
    # still invokes the radio callback with A, never whichever TX is active now.
    assert "typedef struct {" in added
    assert "uint32_t tx_origin;" in added
    assert ".tx_origin = RadioGetTxOrigin()," in added
    assert "xQueueSendFromISR(gpio_evt_queue, &event, NULL);" in added
    assert "(g_dioIrq) ((void *)(uintptr_t)event.tx_origin);" in added
    assert "RadioIrqTxOrigin = origin;" in added
    assert "RadioEvents->TxDoneWithOrigin( txDoneOrigin );" in added
    assert "RadioEvents->TxTimeoutWithOrigin( txTimeoutOrigin );" in added

    # A delayed queue element for A must return before it can read/clear B's
    # expander-backed DIO1 latch. The same atomic claim serializes watchdog
    # cleanup and the vendor DIO task across ESP32 cores.
    dio = body(added, "void RadioOnDioIrq", "void RadioIrqProcess")
    claim = dio.index("RadioTryClaimIrqRecovery( )")
    active = dio.index("activeOrigin")
    mismatch = dio.index("origin != activeOrigin")
    clear_latch = dio.index("SX126xGetDio1PinState( )")
    process = dio.index("RadioIrqProcess();")
    assert claim < active < mismatch < clear_latch < process
    assert "if( SX126xGetDio1PinState( ) == 0 )" in dio
    assert "RadioReleaseIrqRecovery( );\n        return;" in dio
    assert "__atomic_compare_exchange_n( &RadioIrqRecoveryClaim" in added
    assert "__atomic_load_n( &RadioTxOrigin" in added
    assert "xQueueCreate(10, sizeof(radio_irq_event_t));" in added

    board = patch.split(
        "diff --git a/components/lora/sx126x_sensecap_board.c", 1
    )[1]
    board_added = added_patch_lines(board)
    assert "-            if( SX126xGetDio1PinState())" in board
    assert "SX126xGetDio1PinState" not in board_added
    assert "(g_dioIrq) ((void *)(uintptr_t)event.tx_origin);" in board_added


def test_exact_origin_watchdog_recovery_quiesces_irq_before_successor():
    patch = read("patches/sensecap_indicator_tx_origin.patch")
    added = added_patch_lines(patch)
    recover = body(
        added,
        "bool RadioRecoverTxWithOrigin",
        "void RadioSleep",
    )

    claim = recover.index("RadioTryClaimIrqRecovery( )")
    exact = recover.index("!= origin")
    standby = recover.index("RadioStandby( );")
    clear_irq = recover.index("SX126xClearIrqStatus( IRQ_RADIO_ALL );")
    clear_latch = recover.index("SX126xGetDio1PinState( );")
    clear_origin = recover.index(
        "__atomic_store_n( &RadioTxOrigin, 0, __ATOMIC_RELEASE );"
    )
    release = recover.rindex("RadioReleaseIrqRecovery( );")
    assert claim < exact < standby < clear_irq < clear_latch < clear_origin < release
    assert "bool ( *RecoverTxWithOrigin )( uint32_t origin );" in added


def test_application_queues_exact_origin_tuple_and_rejects_late_a_for_b():
    service = read("main/mesh/meshcore_service.c")
    begin = body(
        service,
        "static bool meshcore_radio_tx_operation_begin",
        "static void meshcore_radio_tx_operation_clear",
    )
    capture = body(
        service,
        "static bool capture_callback_tx_operation",
        "static bool meshcore_radio_tx_operation_begin",
    )
    callbacks = service.split(
        "/* SX1262 callbacks are deliberately bounded", 1
    )[1].split("static esp_err_t configure_radio_profile", 1)[0]
    retry = body(
        service,
        "static esp_err_t retry_pending_dm_as_flood(uint64_t now_us)\n{",
        "static esp_err_t meshcore_service_handle_send_dm",
    )

    assert "publish_callback_tx_operation(&identity)" in begin
    assert "s_callback_tx_history[(origin - 1U) %" in capture
    assert "out_identity->operation_id == origin" in capture
    assert "capture_callback_tx_operation(origin, &identity)" in callbacks
    assert ".TxDoneWithOrigin = on_tx_done" in callbacks
    assert ".TxTimeoutWithOrigin = on_tx_timeout" in callbacks
    assert service.count("Radio.SendWithOrigin(") == 3
    assert retry.count("Radio.SendWithOrigin(") == 1
    assert retry.index("meshcore_radio_tx_operation_begin(") < retry.index(
        "Radio.SendWithOrigin("
    )
    assert "(uint32_t)s_active_radio_tx.operation_id" in retry
    assert "d1l_mesh_tx_operation_identity_equal(" in service
    assert "memset(s_callback_tx_history, 0, sizeof(s_callback_tx_history))" in service


def test_owner_watchdog_recovers_a_terminal_lost_by_full_vendor_irq_queue():
    service = read("main/mesh/meshcore_service.c")
    guard = read("main/mesh/meshcore_runtime_guard.h")
    native = read("tests/native/meshcore_runtime_guard_test.c")
    begin = body(
        service,
        "static bool meshcore_radio_tx_operation_begin",
        "static void meshcore_radio_tx_operation_clear",
    )
    clear = body(
        service,
        "static void meshcore_radio_tx_operation_clear",
        "static bool meshcore_radio_terminal_matches",
    )
    watchdog = body(
        service,
        "static void meshcore_service_handle_radio_tx_watchdog",
        "static void meshcore_service_run_owner_maintenance",
    )

    assert "d1l_mesh_tx_watchdog_t" in guard
    assert "d1l_mesh_tx_watchdog_arm(" in begin
    assert begin.index("d1l_mesh_tx_watchdog_arm(") < begin.index(
        "publish_callback_tx_operation(&identity)"
    )
    assert "d1l_mesh_tx_watchdog_reset(&s_radio_tx_watchdog)" in clear
    assert "d1l_mesh_tx_watchdog_take_due(" in watchdog
    recovery = watchdog.index("Radio.RecoverTxWithOrigin(")
    retry = watchdog.index("d1l_mesh_tx_watchdog_arm(", recovery)
    publish = watchdog.index("meshcore_service_handle_radio_tx_timeout(&event)")
    assert recovery < retry < publish
    assert ".tx_operation = operation" in watchdog
    assert "meshcore_service_handle_radio_tx_timeout(&event)" in watchdog
    assert "D1L_MESHCORE_TX_WATCHDOG_GRACE_MS 250U" in service
    assert "test_watchdog_recovers_when_full_irq_queue_drops_terminal" in native
    assert "test_vendor_terminal_claim_defers_competing_watchdog_recovery" in native
    assert "queued < 10U" in native
    assert "standby_before_late_a" in native
    assert "irq_clear_before_late_a" in native
    assert "watchdog_b.deadline_us == b_deadline" in native


def test_origin_patch_is_a_release_input_after_existing_bsp_patches():
    cmake = read("CMakeLists.txt")
    package = read("scripts/package_release_d1l.py")
    sbom = read("scripts/sbom_d1l.py")

    touch = cmake.index("SEEED_BSP_TOUCH_PATCH")
    compat = cmake.index("SEEED_BSP_IDF55_COMPAT_PATCH")
    origin = cmake.index("SEEED_BSP_TX_ORIGIN_PATCH")
    assert touch < compat < origin
    assert 'Path("patches/sensecap_indicator_tx_origin.patch")' in package
    assert '"patches/sensecap_indicator_tx_origin.patch"' in sbom
