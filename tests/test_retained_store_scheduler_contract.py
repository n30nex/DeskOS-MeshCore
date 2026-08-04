from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_common_scheduler_owns_descriptor_order_and_one_forced_deadline():
    worker = read("main/mesh/route_store_worker.c")
    header = read("main/mesh/route_store_worker.h")
    scheduler = read("main/storage/retained_store_scheduler.c")
    scheduler_header = read("main/storage/retained_store_scheduler.h")
    cmake = read("main/CMakeLists.txt")

    assert '"storage/retained_store_scheduler.c"' in cmake
    assert "d1l_retained_store_descriptor_t s_retained_stores[]" in worker
    descriptor_block = worker.split(
        "static const d1l_retained_store_descriptor_t s_retained_stores[]", 1
    )[1].split("static int64_t scheduler_clock", 1)[0]
    names = [
        '.name = "messages"',
        '.name = "direct_messages"',
        '.name = "packets"',
        '.name = "routes"',
        '.name = "contacts"',
        '.name = "read_state"',
    ]
    positions = [descriptor_block.index(name) for name in names]
    assert positions == sorted(positions)

    assert "int64_t deadline_us;" in worker
    assert ".deadline_us = deadline_us" in worker
    assert "flush_retained_stores_locked(true, request.deadline_us)" in worker.replace(
        "\n", " "
    )
    assert "options->force && options->deadline_us <= 0" in scheduler
    assert "deadline_exhausted(options, now_us)" in scheduler
    assert "cancel_remaining" in scheduler
    assert "No descriptor begins after that deadline" in scheduler_header
    assert "d1l_retained_store_finite_wait_ticks" in scheduler
    assert "max_finite_ticks" in scheduler

    assert "d1l_retained_store_worker_status_t" in header
    assert "d1l_retained_store_worker_status" in header
    assert "d1l_retained_store_pass_t last_pass" in header
    assert "a callback already in flight" in header
    assert "may finish after the caller receives ESP_ERR_TIMEOUT" in header

    force = worker.split(
        "esp_err_t d1l_route_store_worker_force_flush", 1
    )[1].split("static esp_err_t route_store_worker_quiesce_begin", 1)[0]
    assert force.index("const int64_t deadline_us") < force.index(
        "d1l_route_store_worker_start()"
    ) < force.index("xSemaphoreTake(s_request_mutex, remaining_ticks)")
    assert force.count("absolute_deadline_remaining_ticks(deadline_us)") == 2
    assert force.count("xSemaphoreGive(s_request_mutex)") == 5
    assert "force_flush_priority_begin(requester)" in force
    assert force.count("force_flush_priority_end(requester)") == 3


def test_scheduler_result_is_published_only_after_flush_lock_release():
    worker = read("main/mesh/route_store_worker.c")
    body = worker.split(
        "static esp_err_t flush_retained_stores_locked", 1
    )[1].split("static TickType_t quiesce_remaining_ticks", 1)[0]

    run = body.index("d1l_retained_store_scheduler_run")
    release = body.index("xSemaphoreGive(s_flush_mutex)", run)
    publish = body.index("publish_pass(&pass)", release)
    assert run < release < publish
    assert "wait_ticks = absolute_deadline_remaining_ticks(deadline_us)" in body
    lock_timeout = body.split(
        "if (xSemaphoreTake(s_flush_mutex, wait_ticks) != pdTRUE)", 1
    )[1].split("const d1l_retained_store_scheduler_options_t", 1)[0]
    assert "pass.result = ESP_ERR_TIMEOUT" in lock_timeout
    assert "publish_pass(&pass)" in lock_timeout
    assert "xSemaphoreGive(s_flush_mutex)" not in lock_timeout
    assert "set_worker_running(true, force, deadline_us)" in body
    assert ".progress = publish_store_progress" in body

    absolute_ticks = worker.split(
        "static TickType_t absolute_deadline_remaining_ticks", 1
    )[1].split("static void set_worker_running", 1)[0]
    quiesce_ticks = worker.split(
        "static TickType_t quiesce_remaining_ticks", 1
    )[1].split("static void publish_result", 1)[0]
    for conversion in (absolute_ticks, quiesce_ticks):
        assert "d1l_retained_store_finite_wait_ticks" in conversion
        assert "portMAX_DELAY - 1U" in conversion
        assert "pdMS_TO_TICKS" not in conversion


def test_storage_status_exposes_coherent_common_pass_results():
    console = read("main/comms/usb_console.c")
    body = console.split("static void print_retained_scheduler_json", 1)[1].split(
        "static bool parse_fingerprint_token", 1
    )[0]
    command = console.split("static void cmd_storage_status", 1)[1].split(
        "static void cmd_storage_mount", 1
    )[0]

    for field in (
        '"running"',
        '"active"',
        '"store_active"',
        '"store_started_us"',
        '"forced_passes"',
        '"deadline_exhaustions"',
        '"quiesce_cancellations"',
        '"deadline_exhausted"',
        '"coalesced"',
        '"skipped_clean"',
        '"reconcile_pending"',
        '"commits"',
        '"failures"',
    ):
        assert field.replace('"', '\\"') in body
    assert "d1l_retained_store_outcome_name" in body
    assert "d1l_retained_store_worker_status(&retained_scheduler)" in command
    assert '\\"retained_scheduler\\"' in command
