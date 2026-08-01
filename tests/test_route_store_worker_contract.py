from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_route_persistence_has_a_dedicated_bounded_worker():
    worker = read("main/mesh/route_store_worker.c")
    header = read("main/mesh/route_store_worker.h")
    cmake = read("main/CMakeLists.txt")
    app = read("main/app_main.c")
    service = read("main/mesh/meshcore_service.c")
    console = read("main/comms/usb_console.c")

    assert "d1l_route_store_worker_start" in header
    assert "d1l_route_store_worker_force_flush" in header
    assert "D1L_ROUTE_STORE_WORKER_INTERVAL_MS 1000U" in worker
    assert "D1L_ROUTE_STORE_WORKER_STACK_BYTES 12288U" in worker
    assert "D1L_ROUTE_STORE_WORKER_PRIORITY (tskIDLE_PRIORITY + 1U)" in worker
    assert "d1l_health_monitor_register_retained_task(created_task)" in worker
    assert "flush_retained_stores" in worker
    assert "d1l_message_store_flush_if_due()" in worker
    assert "d1l_dm_store_flush_if_due()" in worker
    assert "d1l_packet_log_flush_if_due()" in worker
    assert "d1l_route_store_flush_if_due()" in worker
    assert "d1l_contact_store_flush_if_due()" in worker
    assert "d1l_read_state_flush_if_due()" in worker
    assert "d1l_node_store_flush_if_due()" in worker
    assert "d1l_message_store_flush()" in worker
    assert "d1l_dm_store_flush()" in worker
    assert "d1l_packet_log_flush()" in worker
    assert "d1l_route_store_flush()" in worker
    assert "d1l_contact_store_flush()" in worker
    assert "d1l_read_state_flush()" in worker
    assert "d1l_node_store_flush()" in worker
    contact_observer = worker.split(
        "static void observe_contacts", 1
    )[1].split("static esp_err_t flush_time_checkpoint", 1)[0]
    assert ".reconcile_pending = stats.sd_primary_reconcile_pending" in contact_observer
    read_state_observer = worker.split(
        "static void observe_read_state", 1
    )[1].split("static esp_err_t flush_time_checkpoint", 1)[0]
    assert ".revision = stats.persistence_revision" in read_state_observer
    assert ".commit_count = stats.persistence_commit_count" in read_state_observer
    assert ".failure_count = stats.persistence_fail_count" in read_state_observer
    assert ".reconcile_pending = stats.sd_primary_reconcile_pending" in (
        read_state_observer
    )
    assert "request_id" in worker
    assert "s_result_request_id" in worker
    assert "deadline_us" in worker
    assert "absolute_deadline_remaining_ticks(deadline_us)" in worker
    assert ".deadline_us = deadline_us" in worker
    assert "xQueueSend(s_request_queue, &request, 0)" in worker
    assert "s_worker_starting" in worker
    assert "already_starting" in worker
    assert '"mesh/route_store_worker.c"' in cmake
    assert "d1l_route_store_worker_start()" in app
    assert "xQueueReceive(s_service_queue, &cmd, 0)" in service
    assert "xQueueReceive(s_radio_event_queue, &cmd, 0)" in service
    assert "pdMS_TO_TICKS(D1L_MESHCORE_OWNER_POLL_MS)" in service
    assert "ulTaskNotifyTake(pdTRUE, portMAX_DELAY)" not in service
    assert "d1l_route_store_flush_if_due()" not in service
    assert "d1l_contact_store_flush_if_due()" not in service
    assert "d1l_contact_store_flush()" not in service
    assert "d1l_route_store_worker_force_flush(" in console
    assert "D1L_REBOOT_QUIESCE_TIMEOUT_MS = 15000U" in console
    assert "D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS = 50U" in console
    assert "D1L_REBOOT_RESTART_MARGIN_MS = 100U" in console
    assert "retained storage flush failed; reboot cancelled" in console
    assert r'\"retained_flush\":\"ESP_OK\"' in console
    assert '#include "esp_rom_sys.h"' in console


def test_reboot_quiesces_storage_and_uart_under_one_deadline():
    console = read("main/comms/usb_console.c")
    storage = read("main/storage/storage_status.c")
    storage_header = read("main/storage/storage_status.h")
    bridge = read("main/hal/rp2040_bridge.c")
    bridge_header = read("main/hal/rp2040_bridge.h")

    assert "d1l_storage_manager_quiesce_begin" in storage_header
    assert "d1l_storage_manager_quiesce_end" in storage_header
    assert "d1l_rp2040_bridge_quiesce_begin" in bridge_header
    assert "d1l_rp2040_bridge_quiesce_end" in bridge_header

    manager_quiesce = storage.split(
        "esp_err_t d1l_storage_manager_quiesce_begin", 1
    )[1].split("void d1l_storage_manager_quiesce_end", 1)[0]
    manager_quiesce_end = storage.split(
        "void d1l_storage_manager_quiesce_end", 1
    )[1].split("void d1l_storage_manager_force_nvs", 1)[0]
    assert "return ESP_ERR_INVALID_ARG" in manager_quiesce
    assert "return ESP_ERR_INVALID_STATE" in manager_quiesce
    assert manager_quiesce.index("d1l_storage_manager_pause") < manager_quiesce.index(
        "xSemaphoreTake(s_manager_sequence_mutex, ticks)"
    )
    assert "d1l_storage_manager_resume();\n        return ESP_ERR_TIMEOUT;" in manager_quiesce
    assert manager_quiesce.index("xSemaphoreTake(s_manager_sequence_mutex, ticks)") < manager_quiesce.index(
        "s_manager_quiesce_owner = xTaskGetCurrentTaskHandle()"
    )
    assert "s_manager_quiesce_owner != current" in manager_quiesce_end
    assert manager_quiesce_end.index("s_manager_quiesce_owner = NULL") < manager_quiesce_end.index(
        "manager_sequence_give()"
    ) < manager_quiesce_end.index("d1l_storage_manager_resume()")

    bridge_quiesce = bridge.split(
        "esp_err_t d1l_rp2040_bridge_quiesce_begin", 1
    )[1].split("void d1l_rp2040_bridge_quiesce_end", 1)[0]
    bridge_quiesce_end = bridge.split(
        "void d1l_rp2040_bridge_quiesce_end", 1
    )[1].split("static esp_err_t pulse_rp2040_reset", 1)[0]
    assert "static TaskHandle_t s_bridge_quiesce_requester;" in bridge
    assert "return ESP_ERR_INVALID_ARG" in bridge_quiesce
    assert "return ESP_ERR_INVALID_STATE" in bridge_quiesce
    assert "const int64_t started_us = esp_timer_get_time();" in bridge_quiesce
    assert "s_bridge_quiesce_owner == NULL" in bridge_quiesce
    assert "s_bridge_quiesce_requester == NULL" in bridge_quiesce
    assert "s_bridge_quiesce_requester = current" in bridge_quiesce
    assert bridge_quiesce.count(
        "clear_bridge_quiesce_requester(current);"
    ) == 6
    assert "xSemaphoreTake(s_bridge_mutex, ticks)" in bridge_quiesce
    assert "uart_is_driver_installed(uart_port)" in bridge_quiesce
    assert "uart_wait_tx_done(uart_port, ticks)" in bridge_quiesce
    assert "D1L_RP2040_BRIDGE_LOCK_GRACE_MS" not in bridge_quiesce
    requester_registration = bridge_quiesce.index(
        "s_bridge_quiesce_requester = current"
    )
    lock_take = bridge_quiesce.index("xSemaphoreTake(s_bridge_mutex, ticks)")
    tx_idle = bridge_quiesce.index("uart_wait_tx_done(uart_port, ticks)")
    owner_promotion = bridge_quiesce.index("s_bridge_quiesce_owner = current")
    requester_release = bridge_quiesce.index(
        "s_bridge_quiesce_requester = NULL", owner_promotion
    )
    assert requester_registration < lock_take < tx_idle < owner_promotion < requester_release
    assert bridge_quiesce.index("xSemaphoreTake(s_bridge_mutex, ticks)") < bridge_quiesce.index(
        "uart_wait_tx_done(uart_port, ticks)"
    ) < owner_promotion
    assert bridge_quiesce.count(
        "bridge_quiesce_remaining_ticks(started_us, timeout_ms)"
    ) == 3
    no_mutex_failure = bridge_quiesce.split(
        "if (!s_bridge_mutex)", 1
    )[1].split("TickType_t ticks", 1)[0]
    assert no_mutex_failure.index(
        "clear_bridge_quiesce_requester(current)"
    ) < no_mutex_failure.index("return ESP_ERR_NO_MEM")
    lock_failure = bridge_quiesce.split(
        "if (ticks == 0U || xSemaphoreTake(s_bridge_mutex, ticks) != pdTRUE)", 1
    )[1].split("/* esp_restart_noos", 1)[0]
    assert lock_failure.index(
        "clear_bridge_quiesce_requester(current)"
    ) < lock_failure.index("return ESP_ERR_TIMEOUT")
    uart_budget_failure = bridge_quiesce.split(
        "if (uart_is_driver_installed(uart_port))", 1
    )[1].split("const esp_err_t tx_idle_ret", 1)[0]
    assert uart_budget_failure.index(
        "clear_bridge_quiesce_requester(current)"
    ) < uart_budget_failure.index("give_bridge_lock()") < uart_budget_failure.index(
        "return ESP_ERR_TIMEOUT"
    )
    tx_idle_failure = bridge_quiesce.split(
        "if (tx_idle_ret != ESP_OK)", 1
    )[1].split(
        "if (bridge_quiesce_remaining_ticks", 1
    )[0]
    assert tx_idle_failure.index(
        "clear_bridge_quiesce_requester(current)"
    ) < tx_idle_failure.index("give_bridge_lock()") < tx_idle_failure.index(
        "return tx_idle_ret"
    )
    final_deadline_failure = bridge_quiesce.split(
        "if (bridge_quiesce_remaining_ticks(started_us, timeout_ms) == 0U)", 1
    )[1].split("portENTER_CRITICAL", 1)[0]
    assert final_deadline_failure.index(
        "clear_bridge_quiesce_requester(current)"
    ) < final_deadline_failure.index("give_bridge_lock()") < final_deadline_failure.index(
        "return ESP_ERR_TIMEOUT"
    )
    promotion_failure = bridge_quiesce.split(
        "if (!promoted)", 1
    )[1].split("return ESP_OK", 1)[0]
    assert promotion_failure.index(
        "clear_bridge_quiesce_requester(current)"
    ) < promotion_failure.index("give_bridge_lock()") < promotion_failure.index(
        "return ESP_ERR_INVALID_STATE"
    )
    assert "s_bridge_quiesce_owner != current" in bridge_quiesce_end
    assert "s_bridge_quiesce_requester = NULL" in bridge_quiesce_end
    assert bridge_quiesce_end.index("s_bridge_quiesce_owner = NULL") < bridge_quiesce_end.index(
        "give_bridge_lock()"
    )

    reboot = console.split(
        '} else if (strcmp(line, "reboot") == 0) {', 1
    )[1].split('} else if (strcmp(line, "factory-reset-confirm") == 0) {', 1)[0]
    assert reboot.count("reboot_deadline_remaining_ms(reboot_started_us)") == 5
    assert reboot.index("d1l_storage_manager_quiesce_begin") < reboot.index(
        "d1l_route_store_worker_force_flush"
    )
    assert reboot.index("d1l_route_store_worker_force_flush") < reboot.index(
        "d1l_route_store_worker_quiesce_begin"
    )
    assert reboot.index("d1l_route_store_worker_quiesce_begin") < reboot.index(
        "d1l_rp2040_bridge_quiesce_begin"
    )
    assert reboot.index("d1l_rp2040_bridge_quiesce_begin") < reboot.index(
        "remaining_ms <= D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS +"
    ) < reboot.index(
        "d1l_connectivity_prepare_reboot()"
    ) < reboot.index(
        "ok_begin(\"reboot\")"
    ) < reboot.index(
        "fflush(stdout)"
    ) < reboot.index(
        "vTaskDelay(pdMS_TO_TICKS(D1L_REBOOT_CONSOLE_DRAIN_GRACE_MS))"
    ) < reboot.index(
        "esp_rom_software_reset_system()"
    )
    assert "esp_restart()" not in reboot
    assert reboot.count("d1l_storage_manager_quiesce_end()") == 5
    assert reboot.count("d1l_route_store_worker_quiesce_end()") == 3
    assert reboot.count("d1l_rp2040_bridge_quiesce_end()") == 2
    final_deadline = reboot.index("reboot deadline lacks console drain headroom")
    assert reboot.rfind("d1l_rp2040_bridge_quiesce_end()", 0, final_deadline) < reboot.rfind(
        "d1l_route_store_worker_quiesce_end()", 0, final_deadline
    ) < reboot.rfind(
        "d1l_storage_manager_quiesce_end()", 0, final_deadline
    )
    assert final_deadline < reboot.index("ok_begin(\"reboot\")")
    flush_failure = reboot.index("retained storage flush failed")
    assert reboot.rfind("d1l_storage_manager_quiesce_end()", 0, flush_failure) != -1
    bridge_failure = reboot.index("RP2040 bridge quiesce failed")
    assert reboot.rfind("d1l_storage_manager_quiesce_end()", flush_failure, bridge_failure) != -1
    assert "storage manager quiesce failed; reboot cancelled" in reboot
    assert "RP2040 bridge quiesce failed; reboot cancelled" in reboot
    assert r'\"storage_manager_quiesced\":true' in reboot
    assert r'\"retained_worker_quiesced\":true' in reboot
    assert r'\"rp2040_bridge_quiesced\":true' in reboot
    assert r'\"console_drain_grace_ms\":%lu' in reboot
    assert r'\"reset_scope\":\"system\"' in reboot
    assert r'\"connectivity_prepare\":\"ESP_OK\"' in reboot
    connectivity_failure = reboot.index("connectivity prepare failed")
    assert reboot.rfind(
        "d1l_rp2040_bridge_quiesce_end()", 0, connectivity_failure
    ) < reboot.rfind(
        "d1l_route_store_worker_quiesce_end()", 0, connectivity_failure
    ) < reboot.rfind(
        "d1l_storage_manager_quiesce_end()", 0, connectivity_failure
    )


def test_serial_remount_owner_safely_quiesces_retained_worker_without_reboot_reordering():
    worker = read("main/mesh/route_store_worker.c")
    header = read("main/mesh/route_store_worker.h")
    storage = read("main/storage/storage_status.c")
    console = read("main/comms/usb_console.c")

    assert "d1l_route_store_worker_quiesce_begin" in header
    assert "d1l_route_store_worker_quiesce_wait_begin" in header
    assert "d1l_route_store_worker_quiesce_end" in header
    assert "#include <stdbool.h>" in header
    assert "d1l_route_store_persistence_should_yield" in header
    assert "s_flush_mutex" in worker
    assert "s_quiesce_requester" in worker
    assert "worker_quiesce_requested" in worker
    assert "scheduler_cancel_requested" in worker
    assert "current != requester && current != owner" in worker.replace("\n", " ")
    assert "flush_retained_stores_locked(true, request.deadline_us)" in worker.replace(
        "\n", " "
    )
    assert "flush_retained_stores_locked(false, 0)" in worker

    flush = worker.split(
        "static esp_err_t flush_retained_stores_locked", 1
    )[1].split("static TickType_t quiesce_remaining_ticks", 1)[0]
    assert "d1l_retained_store_scheduler_run" in flush
    assert "absolute_deadline_remaining_ticks(deadline_us)" in flush
    run_index = flush.index("d1l_retained_store_scheduler_run")
    release_index = flush.index("xSemaphoreGive(s_flush_mutex)", run_index)
    publish_index = flush.index("publish_pass(&pass)", release_index)
    assert run_index < release_index < publish_index
    assert "pass.deadline_exhausted = true" in flush

    begin = worker.split(
        "static esp_err_t route_store_worker_quiesce_begin", 1
    )[1].split("void d1l_route_store_worker_quiesce_end", 1)[0]
    end = worker.split(
        "void d1l_route_store_worker_quiesce_end", 1
    )[1]
    assert "return ESP_ERR_INVALID_ARG" in begin
    assert "return ESP_ERR_INVALID_STATE" in begin
    assert "s_quiesce_requester == NULL" in begin
    assert "s_quiesce_requester = current" in begin
    assert begin.count("s_quiesce_requester = NULL") == 3
    assert "s_quiesce_preempt_requested = preempt" in begin
    assert begin.count("s_quiesce_preempt_requested = false") == 3
    assert begin.count("s_quiesce_preempt_requested = true") == 1
    assert "route_store_worker_quiesce_begin(timeout_ms, true)" in begin
    assert "route_store_worker_quiesce_begin(timeout_ms, false)" in begin
    assert "xSemaphoreTake(s_flush_mutex, ticks)" in begin
    flush_take = begin.index("xSemaphoreTake(s_flush_mutex, ticks)")
    final_deadline = begin.index(
        "esp_timer_get_time() - started_us >= (int64_t)timeout_ms * 1000LL",
        flush_take,
    )
    owner_promotion = begin.index("s_quiesce_owner = current")
    preempt_promotion = begin.index("s_quiesce_preempt_requested = true")
    assert begin.index("xSemaphoreTake(s_request_mutex, ticks)") < flush_take
    assert flush_take < final_deadline < owner_promotion < preempt_promotion < begin.index(
        "return ESP_OK", preempt_promotion
    )
    deadline_cleanup = begin[final_deadline:owner_promotion]
    assert "xSemaphoreGive(s_flush_mutex)" in deadline_cleanup
    assert "xSemaphoreGive(s_request_mutex)" in deadline_cleanup
    assert "return ESP_ERR_TIMEOUT" in deadline_cleanup
    assert begin.index("xSemaphoreTake(s_request_mutex, ticks)") < begin.index(
        "xSemaphoreTake(s_flush_mutex, ticks)"
    ) < begin.index(
        "s_quiesce_owner = current"
    )
    assert "s_quiesce_owner != current" in end
    assert "s_quiesce_requester = NULL" in end
    assert "s_quiesce_preempt_requested = false" in end
    assert end.index("s_quiesce_owner = NULL") < end.index(
        "xSemaphoreGive(s_flush_mutex)"
    ) < end.index("xSemaphoreGive(s_request_mutex)")

    remount = storage.split(
        "esp_err_t d1l_storage_status_remount_blocking", 1
    )[1].split(
        "void d1l_storage_status", 1
    )[0]
    assert remount.index("manager_sequence_take(remaining_ms)") < remount.index(
        "d1l_route_store_worker_quiesce_begin"
    ) < remount.index("storage_status_mount")
    sequence_failure = remount.split(
        "if (manager_sequence_ret != ESP_OK)", 1
    )[1].split("remaining_ms = storage_deadline_remaining_ms", 1)[0]
    assert "return manager_sequence_ret" in sequence_failure
    assert "d1l_route_store_worker_quiesce_begin" not in sequence_failure
    assert remount.rindex("d1l_route_store_worker_quiesce_end") < remount.rindex(
        "manager_sequence_give()"
    )
    acquisition_failure = remount.split(
        "if (retained_quiesce_ret != ESP_OK)", 1
    )[1].split(
        "if (out_retained_worker_quiesce_acquired)", 1
    )[0]
    assert "manager_sequence_give()" in acquisition_failure
    assert "storage_status_mount" not in acquisition_failure
    assert "return retained_quiesce_ret" in acquisition_failure
    assert remount.count("d1l_route_store_worker_quiesce_end()") == 1
    assert r'\"retained_worker_quiesce_acquired\":%s' in console

    manager = storage.split(
        "static void storage_manager_run_once(void)", 1
    )[1].split("static uint32_t storage_manager_pause_delay_ms", 1)[0]
    assert "D1L_STORAGE_MANAGER_PASSIVE_QUIESCE_TIMEOUT_MS 250U" in storage
    assert manager.index("manager_sequence_try_take()") < manager.index(
        "d1l_route_store_worker_quiesce_wait_begin("
    ) < manager.index("storage_manager_run_once_owned();")
    assert "d1l_route_store_worker_quiesce_begin(" not in manager
    assert manager.index("storage_manager_run_once_owned();") < manager.index(
        "d1l_route_store_worker_quiesce_end();"
    ) < manager.rindex("manager_sequence_give();")
    manager_acquisition_failure = manager.split(
        "if (retained_quiesce_ret != ESP_OK)", 1
    )[1].split("storage_manager_run_once_owned();", 1)[0]
    assert "manager_sequence_give();" in manager_acquisition_failure
    assert "d1l_rp2040_bridge_" not in manager_acquisition_failure
    assert 'set_manager_state(D1L_STORAGE_MANAGER_STATUS);' in manager_acquisition_failure

    reboot = console.split(
        '} else if (strcmp(line, "reboot") == 0) {', 1
    )[1].split('} else if (strcmp(line, "factory-reset-confirm") == 0) {', 1)[0]
    assert reboot.index("d1l_storage_manager_quiesce_begin") < reboot.index(
        "d1l_route_store_worker_force_flush"
    ) < reboot.index("d1l_route_store_worker_quiesce_begin") < reboot.index(
        "d1l_rp2040_bridge_quiesce_begin"
    )
    bridge_failure = reboot.split("if (bridge_quiesce_ret != ESP_OK)", 1)[1].split(
        "if (reboot_deadline_remaining_ms", 1
    )[0]
    assert bridge_failure.index("d1l_route_store_worker_quiesce_end") < bridge_failure.index(
        "d1l_storage_manager_quiesce_end"
    )
