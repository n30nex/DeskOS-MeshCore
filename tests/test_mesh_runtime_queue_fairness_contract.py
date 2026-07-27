import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def canonical_sha256(relative: str) -> str:
    payload = (ROOT / relative).read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(payload).hexdigest()


def test_production_scheduler_has_exact_terminal_and_fairness_order() -> None:
    guard = read("main/mesh/meshcore_runtime_guard.h")
    choose = body(
        guard,
        "static inline d1l_mesh_owner_work_t d1l_mesh_owner_scheduler_choose(",
        "typedef enum {\n    D1L_MESH_TX_OPERATION_NONE",
    )

    assert "D1L_MESH_OWNER_RADIO_EVENT_BURST_MAX 4U" in guard
    assert "D1L_MESH_OWNER_PRIORITY_COMMAND_BURST_MAX 2U" in guard
    terminal = choose.index("if (terminal_recovery_pending)")
    radio = choose.index("if (radio_event_available &&")
    forced_normal = choose.index(
        "if (normal_command_available && priority_command_available"
    )
    priority = choose.index("if (priority_command_available)")
    normal = choose.index("if (normal_command_available)")
    assert terminal < radio < forced_normal < priority < normal
    assert "*out_forced_normal = true" in choose
    assert "scheduler->priority_command_burst = 0U" in choose


def test_owner_uses_separate_bounded_command_lanes_and_tested_selector() -> None:
    service = read("main/mesh/meshcore_service.c")
    guard = read("main/mesh/meshcore_runtime_guard.h")
    native = read("tests/native/meshcore_runtime_guard_test.c")
    task = body(
        service,
        "static void meshcore_service_task(void *arg)",
        "static esp_err_t meshcore_service_start_task(void)",
    )
    start = body(
        service,
        "static esp_err_t meshcore_service_start_task(void)",
        "static bool meshcore_service_called_from_owner(void)",
    )
    compact_task = "".join(task.split())

    assert "D1L_MESHCORE_SERVICE_QUEUE_LEN 6U" in service
    assert "D1L_MESHCORE_PRIORITY_QUEUE_LEN 4U" in service
    assert "D1L_MESHCORE_RADIO_EVENT_QUEUE_LEN 8U" in service
    assert "xQueueCreate(D1L_MESHCORE_SERVICE_QUEUE_LEN" in start
    assert "D1L_MESHCORE_PRIORITY_QUEUE_LEN" in start
    assert "D1L_MESHCORE_RADIO_EVENT_QUEUE_LEN" in start
    assert "d1l_mesh_owner_scheduler_choose(" in task
    assert "uxQueueMessagesWaiting(s_radio_event_queue)" in task
    assert "uxQueueMessagesWaiting(s_priority_command_queue)" in task
    assert "uxQueueMessagesWaiting(s_service_queue)" in task
    urgent_ack = task.index("const bool urgent_ack_response")
    urgent_dispatch = task.index(
        "work = D1L_MESH_OWNER_WORK_PRIORITY_COMMAND;", urgent_ack
    )
    choose = task.index("d1l_mesh_owner_scheduler_choose")
    assert urgent_ack < urgent_dispatch < choose
    urgent_body = task[
        task.index("if (urgent_ack_response)", urgent_ack):
        task.index("} else {", urgent_ack)
    ]
    assert "s_pending_ack_tx.active && !s_tx_busy" in task
    assert "scheduler.radio_event_burst = 0U" in urgent_body
    assert "scheduler.priority_command_burst++" in urgent_body
    ack_queue = body(
        service,
        "static esp_err_t meshcore_service_send_ack_async",
        "void d1l_meshcore_service_init",
    )
    assert "xQueueSendToFront(s_priority_command_queue" in ack_queue
    assert "const bool rx_recovery_deferred = rx_recovery && s_tx_busy" in task
    assert "rx_recovery && !rx_recovery_deferred" in task
    assert "d1l_mesh_terminal_lane_publish_slot" in guard
    assert "d1l_mesh_terminal_lane_try_reserve_owner" in guard
    assert "d1l_mesh_terminal_lane_owner_still_reserved" in guard
    assert "d1l_mesh_terminal_lane_release_owner" in guard
    assert "test_terminal_lane_publication_blocks_queue_dispatch" in native
    assert "test_dequeued_send_raw_is_retained_across_terminal_race" in native
    assert "test_accepted_send_raw_waits_for_active_tx_terminal" in native
    assert "xQueueReceive(s_priority_command_queue,&cmd,0)" in compact_task
    assert "xQueueReceive(s_service_queue,&cmd,0)" in compact_task
    assert "xQueueReceive(s_radio_event_queue,&cmd,0)" in compact_task

    take_terminal = task.index("meshcore_service_take_latched_terminal")
    latch_precedence = task.index("d1l_mesh_terminal_lane_has_pending")
    latch_yield = task.index("taskYIELD()", latch_precedence)
    terminal_dispatch = task.index(
        "meshcore_service_handle_latched_radio_terminal"
    )
    take_rx_recovery = task.index("const bool rx_recovery")
    reserve = task.index("d1l_mesh_terminal_lane_try_reserve_owner")
    first_receive = task.index("xQueueReceive")
    validate = task.index("d1l_mesh_terminal_lane_owner_still_reserved", first_receive)
    admit = task.index("meshcore_request_admit(&cmd)")
    assert (
        take_terminal
        < latch_precedence
        < latch_yield
        < terminal_dispatch
        < take_rx_recovery
        < reserve
        < choose
        < first_receive
        < validate
        < admit
    )
    latch_body = task[latch_precedence:take_rx_recovery]
    assert "continue;" in latch_body
    terminal_body = body(
        task,
        "if (terminal_recovery)",
        "const bool rx_recovery =",
    )
    assert "meshcore_service_handle_latched_radio_terminal" in terminal_body
    assert "meshcore_service_run_owner_maintenance();" in terminal_body
    assert "continue;" in terminal_body

    retain = task[validate:task.index("/* Accepted TX producers", validate)]
    assert "held_cmd = cmd" in retain
    assert "held_work = work" in retain
    assert "held_forced_normal = forced_normal" in retain
    assert "d1l_mesh_terminal_lane_release_owner" in retain
    assert "continue;" in retain

    busy_hold = body(
        task,
        "if (s_tx_busy && meshcore_service_command_requires_idle_tx(&cmd))",
        "if (from_held)",
    )
    assert "held_cmd = cmd" in busy_hold
    assert "d1l_mesh_terminal_lane_release_owner" in busy_hold
    assert "continue;" in busy_hold
    assert task.count("meshcore_service_run_owner_maintenance();") >= 3
    admission_drop = body(
        task,
        "if (!meshcore_request_admit(&cmd))",
        "esp_err_t ret = ESP_ERR_INVALID_ARG;",
    )
    assert admission_drop.index("meshcore_service_command_wipe(&cmd)") < (
        admission_drop.index("meshcore_service_run_owner_maintenance()")
    ) < admission_drop.index("continue;")
    assert "d1l_mesh_terminal_lane_release_owner" in admission_drop
    fairness_count = task.index("&s_runtime_fairness_forced_commands")
    assert task.index("if (!meshcore_request_admit(&cmd))") < fairness_count


def test_tx_producers_are_held_until_the_exact_active_tx_terminal() -> None:
    service = read("main/mesh/meshcore_service.c")
    native = read("tests/native/meshcore_runtime_guard_test.c")
    requires_idle = body(
        service,
        "static bool meshcore_service_command_requires_idle_tx(",
        "static void meshcore_service_task(void *arg)",
    )
    task = body(
        service,
        "static void meshcore_service_task(void *arg)",
        "static esp_err_t meshcore_service_start_task(void)",
    )

    for command in (
        "D1L_MESHCORE_SERVICE_CMD_SEND_RAW",
        "D1L_MESHCORE_SERVICE_CMD_SEND_ADVERT",
        "D1L_MESHCORE_SERVICE_CMD_SEND_DM",
        "D1L_MESHCORE_SERVICE_CMD_SEND_TRACE_CONTACT",
        "D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGIN",
        "D1L_MESHCORE_SERVICE_CMD_ADMIN_REQUEST_STATUS",
    ):
        assert command in requires_idle
    assert "const bool from_held = held_cmd_valid" in task
    assert task.index("if (from_held)") < task.index("d1l_mesh_owner_scheduler_choose")
    assert task.index("s_tx_busy && meshcore_service_command_requires_idle_tx") < (
        task.index("meshcore_request_admit(&cmd)")
    )
    assert "test_accepted_send_raw_waits_for_active_tx_terminal" in native


def test_rx_restart_is_centrally_deferred_behind_exact_tx_terminal() -> None:
    service = read("main/mesh/meshcore_service.c")
    guard = read("main/mesh/meshcore_runtime_guard.h")
    native = read("tests/native/meshcore_runtime_guard_test.c")
    start_rx = body(
        service,
        "static void d1l_meshcore_start_rx(void)\n{",
        "static esp_err_t meshcore_service_handle_start_rx(void)",
    )

    gate = start_rx.index("d1l_mesh_rx_restart_begin(")
    configure = start_rx.index("configure_radio_profile(&profile)")
    radio_rx = min(
        start_rx.index("Radio.RxBoosted(0)"),
        start_rx.index("Radio.Rx(0)"),
    )
    assert gate < configure < radio_rx
    assert "&s_pending_rx_recovery, s_tx_busy" in start_rx
    assert "__atomic_store_n(pending_restart, 1U" in guard
    assert "if (tx_busy)" in guard
    assert "test_older_queued_rx_timeout_defers_after_send_raw" in native


def test_latched_terminal_and_pending_rx_request_restart_exactly_once() -> None:
    service = read("main/mesh/meshcore_service.c")
    guard = read("main/mesh/meshcore_runtime_guard.h")
    native = read("tests/native/meshcore_runtime_guard_test.c")
    task = body(
        service,
        "static void meshcore_service_task(void *arg)",
        "static esp_err_t meshcore_service_start_task(void)",
    )
    take = body(
        guard,
        "static inline bool d1l_mesh_rx_recovery_take(",
        "/* Exact terminals and ordinary owner work linearize",
    )

    assert "d1l_mesh_rx_recovery_take(" in task
    assert "&s_pending_rx_recovery, false" in task
    assert take.index("if (!pending_restart || terminal_recovery)") < (
        take.index("__atomic_exchange_n(")
    )
    assert "test_latched_terminal_consumes_pending_rx_recovery_once" in native
    native_case = body(
        native,
        "static void test_latched_terminal_consumes_pending_rx_recovery_once(void)",
        "static void test_owner_scheduler_bounds_radio_and_priority_starvation(void)",
    )
    assert "assert(pending_rx_restart == 1U)" in native_case
    assert "assert(pending_rx_restart == 0U)" in native_case
    assert "assert(rx_restarts == 1U)" in native_case


def test_every_exact_terminal_bypasses_a_full_or_backlogged_radio_fifo() -> None:
    service = read("main/mesh/meshcore_service.c")
    guard = read("main/mesh/meshcore_runtime_guard.h")
    native = read("tests/native/meshcore_runtime_guard_test.c")
    enqueue = body(
        service,
        "static void enqueue_radio_event(",
        "/* SX1262 callbacks are deliberately bounded",
    )
    task = body(
        service,
        "static void meshcore_service_task(void *arg)",
        "static esp_err_t meshcore_service_start_task(void)",
    )

    terminal_branch = enqueue.index(
        "type == D1L_MESHCORE_SERVICE_EVENT_TX_DONE ||"
    )
    terminal_latch = enqueue.index(
        "meshcore_service_latch_radio_recovery(type, tx_operation)"
    )
    terminal_return = enqueue.index("return;", terminal_latch)
    ordinary_send = enqueue.index(
        "xQueueSend(s_radio_event_queue, &event, 0)"
    )
    assert terminal_branch < terminal_latch < terminal_return < ordinary_send
    assert "d1l_mesh_terminal_lane_publish_slot(" in service
    assert "d1l_mesh_terminal_lane_take_pending(" in service
    assert "d1l_mesh_terminal_lane_has_pending(" in task
    assert "case D1L_MESHCORE_SERVICE_EVENT_TX_DONE:" not in task
    assert "case D1L_MESHCORE_SERVICE_EVENT_TX_TIMEOUT:" not in task
    assert "test_full_radio_backlog_exact_terminal_preempts_and_restarts_once" in native

    publish = body(
        guard,
        "static inline bool d1l_mesh_terminal_lane_publish_slot(",
        "static inline uint32_t d1l_mesh_terminal_lane_take_pending(",
    )
    assert publish.count("__atomic_fetch_or(") == 1
    assert "for (" not in publish
    assert "while (" not in publish
    take = body(
        service,
        "static bool meshcore_service_take_latched_terminal(",
        "static void status_lock",
    )
    assert "identity.operation_id <= best_identity.operation_id" in take
    assert "out_event->tx_operation = best_identity" in take
    assert "if (incomplete_snapshot)" in take
    incomplete = take.split("if (incomplete_snapshot)", 1)[1].split(
        "if (!found)", 1
    )[0]
    assert "d1l_mesh_terminal_lane_publish_slot(" in incomplete
    assert "&s_terminal_lane, slot" in incomplete
    native_case = body(
        native,
        "static void test_full_radio_backlog_exact_terminal_preempts_and_restarts_once(void)",
        "static void test_owner_scheduler_bounds_radio_and_priority_starvation(void)",
    )
    assert "const unsigned radio_backlog = 8U" in native_case
    assert "D1L_MESH_OWNER_WORK_TERMINAL_RECOVERY" in native_case
    assert "assert(rx_restarts == 1U)" in native_case


def test_priority_responses_cannot_reorder_or_starve_normal_fifo() -> None:
    service = read("main/mesh/meshcore_service.c")
    raw_response = body(
        service,
        "static esp_err_t meshcore_service_queue_raw_response(",
        "static esp_err_t meshcore_service_send_ack_async(",
    )
    ack = body(
        service,
        "static esp_err_t meshcore_service_send_ack_async(",
        "void d1l_meshcore_service_init(void)",
    )
    claimed = body(
        service,
        "static esp_err_t meshcore_service_send_claimed_command(",
        "static esp_err_t meshcore_service_send_command(",
    )

    assert "xQueueSend(s_priority_command_queue, &cmd, 0)" in raw_response
    assert "runtime_note_command_saturation(true)" in raw_response
    assert "xQueueSend(s_priority_command_queue, &cmd, 0)" in ack
    assert "runtime_note_command_saturation(true)" in ack
    assert "complete_pending_ack_tx(false, ESP_ERR_TIMEOUT)" in ack
    assert "xQueueSendToFront(s_service_queue" not in service
    assert ".queue = s_service_queue" in claimed
    assert "runtime_note_command_saturation(false)" in claimed
    assert "meshcore_request_wait(cmd, timeout_ticks)" in claimed


def test_synchronous_cancellation_wipe_and_recursion_guards_remain_fail_closed() -> None:
    service = read("main/mesh/meshcore_service.c")
    command_guard = read("main/mesh/meshcore_command_guard.h")
    wait = body(
        service,
        "static esp_err_t meshcore_request_wait(",
        "static void meshcore_request_abort_unqueued(",
    )
    owner_check = body(
        service,
        "static bool meshcore_service_called_from_owner(void)",
        "typedef struct {\n    QueueHandle_t queue;",
    )

    assert "d1l_mesh_command_request_cancel_and_release" in wait
    assert "d1l_mesh_command_request_enqueue(" in command_guard
    assert "d1l_mesh_command_request_cancel_and_release" in command_guard
    assert "d1l_mesh_command_request_wipe_admin_password" in command_guard
    assert "d1l_mesh_command_sync_wait_allowed" in owner_check
    assert "xTaskGetCurrentTaskHandle()" in owner_check


def test_saturation_and_fairness_telemetry_is_public_and_exactly_pinned() -> None:
    header = read("main/mesh/meshcore_service.h")
    service = read("main/mesh/meshcore_service.c")
    console = read("main/comms/usb_console.c")
    manifest = json.loads(read("tests/meshcore_oracle/manifest.json"))
    runner = read("scripts/meshcore_conformance_d1l.py")
    allowlist = runner.split(
        "EXPECTED_ORACLE_PRODUCTION_BINDING_SOURCE_PATHS = {", 1
    )[1].split("}", 1)[0]

    fields = (
        "runtime_priority_queue_depth",
        "runtime_priority_queue_high_water",
        "runtime_command_queue_saturation",
        "runtime_priority_queue_saturation",
        "runtime_fairness_forced_commands",
        "runtime_priority_burst_high_water",
        "runtime_priority_burst_bound",
        "runtime_owner_maintenance_runs",
        "runtime_terminal_recovery_dispatches",
    )
    for field in fields:
        assert field in header
        assert field in service
        assert field in console
    assert service.count("&s_runtime_terminal_recovery_dispatches") >= 2
    assert "d1l_mesh_runtime_counter_increment_saturating" in service
    assert "__atomic_add_fetch(&s_runtime" not in service
    assert "runtime_task_heartbeat++" not in service
    assert "test_forced_fairness_counts_only_admitted_generation" in read(
        "tests/native/meshcore_runtime_guard_test.c"
    )
    assert "test_runtime_telemetry_counters_saturate" in read(
        "tests/native/meshcore_runtime_guard_test.c"
    )

    for relative in (
        "main/comms/usb_console.c",
        "main/mesh/meshcore_runtime_guard.h",
        "main/mesh/meshcore_service.c",
        "main/mesh/meshcore_service.h",
    ):
        assert manifest["production_binding_sources"][relative] == (
            canonical_sha256(relative)
        )
        assert f'"{relative}"' in allowlist
