from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, signature: str, next_signature: str) -> str:
    return source.rsplit(signature, 1)[1].split(next_signature, 1)[0]


def test_packet_and_route_callbacks_use_immutable_generation_bound_payloads() -> None:
    source = read("main/ui/ui_phase1.c")

    assert "s_packet_row_payloads[D1L_PACKET_LOG_CAPACITY]" in source
    assert "s_route_row_payloads[D1L_APP_SNAPSHOT_ROUTE_PREVIEW]" in source
    assert "static bool row_token_index(" in source

    packet_row = body(
        source,
        "static void render_packet_row(",
        "static void render_route_row(",
    )
    route_row = body(
        source,
        "static void render_route_row(",
        "static bool snapshot_find_channel(",
    )
    assert "row_token(s_packet_row_generation, payload_index)" in packet_row
    assert "row_token(s_route_row_generation, payload_index)" in route_row
    assert "(void *)entry" not in packet_row
    assert "(void *)entry" not in route_row

    render = body(
        source,
        "static void render_packets(",
        "static void radio_settings_action_handler(",
    )
    assert render.index(
        "s_packet_row_generation = next_row_generation(s_packet_row_generation);"
    ) < render.index("s_packet_row_payloads[i] = s_packets_controller.rows[i];")
    assert render.index(
        "s_route_row_generation = next_row_generation(s_route_row_generation);"
    ) < render.index("s_route_row_payloads[i] = snapshot->recent_routes[i];")
    assert (
        "render_packet_row(content, y, &s_packet_row_payloads[i], i);" in render
    )
    assert "render_route_row(content, y, &s_route_row_payloads[i], i);" in render

    route_callback = body(
        source,
        "static void open_route_detail_event_cb(",
        "static void close_message_detail_event_cb(",
    )
    packet_callback = body(
        source,
        "static void open_packet_detail_event_cb(",
        "static void packet_filter_event_cb(",
    )
    assert "s_route_row_generation" in route_callback
    assert "s_route_row_payload_count" in route_callback
    assert "s_route_detail_route = s_route_row_payloads[payload_index];" in route_callback
    assert "s_packet_row_generation" in packet_callback
    assert "s_packet_row_payload_count" in packet_callback
    assert (
        "s_packet_detail_packet = s_packet_row_payloads[payload_index];"
        in packet_callback
    )
    assert "const d1l_route_entry_t *entry = (const d1l_route_entry_t *)" not in source
    assert (
        "const d1l_packet_log_entry_t *entry = "
        "(const d1l_packet_log_entry_t *)lv_event_get_user_data(event)"
        not in source
    )


def test_lvgl_runtime_initialization_uses_ui_owned_cooperative_handshake() -> None:
    source = read("main/ui/ui_phase1.c")

    runtime_init = body(
        source,
        "static esp_err_t initialize_ui_runtime(void)",
        "static void fail_ui_start_on_ui_task(esp_err_t result)",
    )
    for call in (
        "lv_init();",
        "lv_disp_draw_buf_init(",
        "lv_disp_drv_init(",
        "lv_disp_drv_register(",
        "lv_indev_drv_init(",
        "lv_indev_drv_register(",
        "d1l_ui_phase1_show_home()",
        "esp_timer_create(",
        "esp_timer_start_periodic(",
    ):
        assert call in runtime_init
    assert source.count("lv_init();") == 1

    ui_task = body(
        source,
        "static void ui_task(void *arg)",
        "static void touch_poll_task(void *arg)",
    )
    assert "s_ui_start_result = initialize_ui_runtime();" in ui_task
    assert ui_task.index("initialize_ui_runtime();") < ui_task.index(
        "uint32_t wait_ms = lv_timer_handler();"
    )
    assert 'touch_poll_task, "d1l_touch"' in ui_task
    assert ui_task.index("initialize_ui_runtime();") < ui_task.index(
        'touch_poll_task, "d1l_touch"'
    )
    assert ui_task.index('touch_poll_task, "d1l_touch"') < ui_task.index(
        "d1l_health_monitor_register_ui_task(xTaskGetCurrentTaskHandle());"
    )
    assert ui_task.index(
        "d1l_health_monitor_register_ui_task(xTaskGetCurrentTaskHandle());"
    ) < ui_task.index("xSemaphoreGive(s_ui_start_done_sem)")
    assert "xSemaphoreGive(s_ui_start_done_sem)" in ui_task
    assert "s_ui_start_result != ESP_OK" in ui_task
    assert ui_task.count("fail_ui_start_on_ui_task(") == 2

    touch_task = body(
        source,
        "static void touch_poll_task(void *arg)",
        "esp_err_t d1l_ui_phase1_show_home(void)",
    )
    assert "d1l_board_touch_read(&sample)" in touch_task
    assert "lv_" not in touch_task

    failure = body(
        source,
        "static void fail_ui_start_on_ui_task(esp_err_t result)",
        "esp_err_t d1l_ui_phase1_start(void)",
    )
    assert "esp_timer_stop(s_lv_tick_timer)" in failure
    assert "esp_timer_delete(s_lv_tick_timer)" in failure
    assert "d1l_health_monitor_register_ui_task(NULL);" in failure
    assert "d1l_health_monitor_set_lvgl_ready(false);" in failure
    assert "d1l_app_model_get()->ui_ready = false;" in failure
    assert "s_touch_task_handle = NULL;" in failure
    assert "s_ui_task_handle = NULL;" in failure
    assert "s_ui_start_result = result;" in failure
    assert failure.index("s_ui_start_result = result;") < failure.index(
        "xSemaphoreGive(s_ui_start_done_sem)"
    )
    assert "vTaskDelete(NULL);" in failure
    assert source.count("vTaskDelete(") == 1

    start = source.rsplit("esp_err_t d1l_ui_phase1_start(void)", 1)[1]
    assert "xSemaphoreCreateBinary()" in start
    assert "xSemaphoreTake(s_ui_start_done_sem, portMAX_DELAY)" in start
    assert "D1L_UI_STARTUP_TIMEOUT_MS" not in source
    assert "ESP_ERR_TIMEOUT" not in start
    assert 'touch_poll_task, "d1l_touch"' not in start
    assert 'ui_task, "d1l_ui"' in start
    assert start.count("xTaskCreatePinnedToCoreWithCaps(") == 1
    assert "const esp_err_t start_result = s_ui_start_result;" in start
    assert "return start_result;" in start
    assert "vTaskDelete(" not in start
    assert "lv_init();" not in start
    assert "d1l_ui_phase1_show_home()" not in start
