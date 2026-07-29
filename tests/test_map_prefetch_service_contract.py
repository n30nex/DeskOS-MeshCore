import json

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_offline_provider_is_explicit_fail_closed_and_secret_safe():
    header = read("main/map/map_tile_provider.h")
    provider = read("main/map/map_tile_provider.c")
    store = read("main/storage/map_tile_store.c")

    assert 'D1L_MAP_PROVIDER_CONFIG_PATH "map/offline-provider.json"' in header
    assert "offline_storage_permitted" in header
    assert "background_prefetch_permitted" in header
    assert "minimum_request_interval_ms" in header
    assert "!offline_allowed" in provider
    assert "background_allowed && !provider.network_fetch_allowed" in provider
    assert 'strncmp(value, "https://", 8U) == 0' in provider
    assert "token_count(value, \"{z}\") == 1U" in provider
    assert "D1L_MAP_TILE_SOURCE_ID" in provider
    assert "out_provider->background_prefetch_permitted = false;" in provider
    assert "provider.url_template" in store
    assert "result.url" not in read("main/map/map_prefetch_service.c")


def test_authorized_default_provider_is_seeded_without_overwrite():
    provider = read("main/map/map_tile_provider.c")
    manifest = json.loads(read("sdcard/offline-tile-provider.json"))

    assert manifest == {
        "schema": 1,
        "source_id": "nrcan-cbmt",
        "attribution":
            "Natural Resources Canada; Open Government Licence - Canada",
        "license_url":
            "https://open.canada.ca/en/open-government-licence-canada",
        "offline_storage_permitted": True,
        "background_prefetch_permitted": True,
        "network_url_template":
            "https://maps.geogratis.gc.ca/wms/CBMT?mode=tile&tilemode=gmap&"
            "layers=National%20Sub_national%20Regional%20Sub_regional&"
            "tile={x}+{y}+{z}",
        "tile_template": "z{z}/x{x}/y{y}.png",
        "max_zoom": 15,
        "average_tile_bytes": 65536,
        "cache_budget_mb": 18432,
        "minimum_request_interval_ms": 1000,
    }
    assert (
        "if (read_ret == ESP_ERR_NOT_FOUND) {\n"
        "        (void)seed_default_provider_config();"
    ) in provider
    assert "D1L_MAP_PROVIDER_CONFIG_PATH, false," in provider


def test_provider_console_status_is_cached_atomic_and_fail_closed():
    header = read("main/map/map_tile_provider.h")
    provider = read("main/map/map_tile_provider.c")
    console = read("main/comms/usb_console.c")
    command = console.split(
        "static void cmd_map_provider_status(void)", 1
    )[1].split("#if D1L_ENABLE_QUALIFICATION_HOOKS", 1)[0]
    refresh = provider.split(
        "esp_err_t d1l_map_tile_provider_refresh(", 1
    )[1].split("bool d1l_map_tile_provider_path(", 1)[0]
    publish = provider.split(
        "static void publish_refresh_result(", 1
    )[1].split("static esp_err_t seed_default_provider_config", 1)[0]

    assert "d1l_map_tile_provider_status_t" in header
    assert "last_refresh_result" in header
    assert "refresh_generation" in header
    assert "d1l_map_tile_provider_status_snapshot(" in header
    assert "d1l_map_tile_provider_status_snapshot(&provider_status)" in command
    assert "d1l_map_tile_provider_refresh(" not in command
    assert "d1l_rp2040_bridge_" not in command
    assert (
        "provider_status.refresh_generation > 0U"
        in command
    )
    assert (
        "provider_status.last_refresh_result == ESP_OK"
        in command
    )
    assert "provider.configured" in command
    assert '\\"provider_refresh_generation\\":%lu' in command
    assert refresh.count("publish_refresh_result(") == 2
    assert (
        "publish_refresh_result(\n"
        "        ret, ret == ESP_OK ? &provider : NULL);"
        in refresh
    )
    assert publish.index("if (result == ESP_OK && provider)") < publish.index(
        "s_provider = *provider"
    ) < publish.index("s_provider_last_refresh_result = result")
    assert publish.index(
        "s_provider_last_refresh_result = result"
    ) < publish.index("++s_provider_refresh_generation")


def test_background_service_is_sd_wifi_location_and_visible_map_gated():
    cmake = read("main/CMakeLists.txt")
    app = read("main/app_main.c")
    service = read("main/map/map_prefetch_service.c")

    assert '"map/map_prefetch_service.c"' in cmake
    assert app.index("d1l_connectivity_init()") < app.index(
        "d1l_map_prefetch_service_init()"
    )
    assert "d1l_settings_public_snapshot(&settings)" in service
    assert "!settings.map_location_set" in service
    assert "d1l_map_tile_store_sd_ready(&storage)" in service
    assert "!connectivity.wifi_connected" in service
    assert "visible_map_active()" in service
    assert "provider.background_prefetch_permitted" in service
    assert "d1l_node_store_copy_markers(" in service
    assert "D1L_NODE_SD_HISTORY_CAPACITY" in service
    assert "d1l_map_tile_store_cached(" in service
    assert "d1l_map_tile_store_fetch_background(" in service
    assert "plan->reserve_bytes + D1L_MAP_TILE_DOWNLOAD_MAX_BYTES" in service
    assert "provider.minimum_request_interval_ms" in read(
        "main/storage/map_tile_store.c"
    )
    assert "cache_budget_mb = provider.cache_budget_mb" in service
    assert "status->evicted_tiles += result.evicted_tiles" in service
    assert "status->cache_used_bytes = result.cache_used_bytes" in service
    cached = service.split(
        "if (ret == ESP_OK && cached)", 1
    )[1].split("if (ret != ESP_ERR_NOT_FOUND)", 1)[0]
    successful_fetch = service.split(
        "if (ret == ESP_OK) {", 1
    )[1].split(
        "if (result.cancelled ||", 1
    )[0]
    assert "taskYIELD()" in cached
    assert "task_pause_after_network_tile()" in successful_fetch
    request_tail = service.split(
        "ret = d1l_map_tile_store_fetch_background(", 1
    )[1].split("set_phase(status, \"ready\"", 1)[0]
    assert request_tail.count("task_pause_after_network_tile()") == 1
    assert request_tail.index(
        "if (result.status_code == 429"
    ) < request_tail.index(
        "if (ret == ESP_OK)"
    ) < request_tail.index(
        "task_pause_after_network_tile()"
    ) < request_tail.index(
        "if (result.cancelled ||"
    )
    assert "ulTaskNotifyTake(" in service.split(
        "static void task_pause_after_network_tile(void)", 1
    )[1].split("static bool visible_map_active", 1)[0]


def test_map_https_paths_share_one_measured_internal_worker_stack():
    prefetch = read("main/map/map_prefetch_service.c")
    prefetch_header = read("main/map/map_prefetch_service.h")
    view = read("main/map/map_view_service.c")
    view_header = read("main/map/map_view_service.h")
    console = read("main/comms/usb_console.c")
    dispatcher = prefetch.split(
        "static void prefetch_worker(void *context)", 1
    )[1].split("esp_err_t d1l_map_prefetch_service_init", 1)[0]

    assert "#define D1L_MAP_SHARED_WORKER_STACK_BYTES 20480U" in view_header
    assert (
        "#define D1L_MAP_PREFETCH_WORKER_PRIORITY (tskIDLE_PRIORITY + 1U)"
        in prefetch
    )
    assert "#define D1L_MAP_VISIBLE_WORKER_PRIORITY 2U" in prefetch
    assert (
        "#define D1L_MAP_PREFETCH_WORKER_STACK_BYTES \\\n"
        "    D1L_MAP_SHARED_WORKER_STACK_BYTES"
    ) in prefetch
    assert "uxTaskGetStackHighWaterMark(NULL)" in prefetch
    assert "uxTaskGetStackHighWaterMark(NULL)" in view
    assert prefetch.count("xTaskCreate(") == 1
    assert "xTaskCreate(" not in view
    assert "run_prefetch_pass()" in dispatcher
    assert "d1l_map_view_service_run_pending()" in prefetch
    assert "static __attribute__((noinline)) void run_prefetch_pass" in prefetch
    assert "static __attribute__((noinline)) void publish_visible_pause" in prefetch
    assert "publish_visible_pause()" in dispatcher
    assert "vTaskPrioritySet(" not in dispatcher
    after_visible_dispatch = dispatcher.split(
        "d1l_map_view_service_run_pending()", 1
    )[1]
    assert "D1L_MAP_PREFETCH_WORKER_PRIORITY" not in after_visible_dispatch
    assert "d1l_settings_t" not in dispatcher
    assert "d1l_map_prefetch_plan_t" not in dispatcher
    assert "d1l_map_prefetch_status_t" not in dispatcher
    assert "ulTaskNotifyTake(" in prefetch
    assert "xTaskNotifyGive(worker)" in prefetch
    assert view.count("d1l_map_prefetch_service_wake()") == 3
    assert "worker_stack_bytes" in prefetch_header
    assert "worker_stack_free_bytes" in prefetch_header
    assert "worker_stack_bytes" in view_header
    assert "worker_stack_free_bytes" in view_header
    assert console.count('\\"worker_stack_bytes\\":%lu') >= 2
    assert console.count('\\"worker_stack_free_bytes\\":%lu') >= 2


def test_visible_map_wake_preempts_background_worker_before_dispatch():
    prefetch = read("main/map/map_prefetch_service.c")
    wake = prefetch.split(
        "esp_err_t d1l_map_prefetch_service_wake(void)", 1
    )[1]
    visible = prefetch.split(
        "static bool visible_map_active(void)", 1
    )[1].split("static bool prefetch_continue", 1)[0]
    view = read("main/map/map_view_service.c")
    view_init = view.split(
        "esp_err_t d1l_map_view_service_init(void)", 1
    )[1].split("esp_err_t d1l_map_view_service_acquire_visible", 1)[0]
    blocking_visibility = view.split(
        "bool d1l_map_view_service_visible(void)", 1
    )[1].split("void d1l_map_view_service_status", 1)[0]
    assert (
        view_init.index("SemaphoreHandle_t lock = xSemaphoreCreateMutex()")
        < view_init.index("uint16_t *frame0 = heap_caps_malloc(")
        < view_init.index("memset(&s_map, 0, sizeof(s_map))")
        < view_init.index("s_map.status.initialized = true")
        < view_init.index("s_map.lock = lock")
    )
    before_publish = view_init.split(
        "memset(&s_map, 0, sizeof(s_map))", 1
    )[0]
    assert "vSemaphoreDelete(lock)" in before_publish
    assert "vSemaphoreDelete(s_map.lock)" not in view_init
    assert "d1l_map_view_service_visible()" in visible
    assert "d1l_map_view_service_status" not in visible
    assert "!s_map.status.initialized" not in blocking_visibility
    assert "xSemaphoreTake(s_map.lock, portMAX_DELAY)" in blocking_visibility
    assert "s_map.status.visible" in blocking_visibility
    assert "xSemaphoreTake(wake_lock, portMAX_DELAY)" in wake
    assert (
        wake.index("xSemaphoreTake(wake_lock, portMAX_DELAY)")
        < wake.index("const bool visible = visible_map_active()")
        < wake.index("vTaskPrioritySet(")
        < wake.index("xTaskNotifyGive(worker)")
        < wake.index("xSemaphoreGive(wake_lock)")
        < wake.index("d1l_map_tile_store_cancel_background_fetch()")
    )
    assert "D1L_MAP_VISIBLE_WORKER_PRIORITY" in wake
    assert "D1L_MAP_PREFETCH_WORKER_PRIORITY" in wake
    dispatcher = prefetch.split(
        "static void prefetch_worker(void *context)", 1
    )[1].split("esp_err_t d1l_map_prefetch_service_init", 1)[0]
    background = dispatcher.split("if (!visible_map_active()) {", 1)[1]
    assert "D1L_MAP_PREFETCH_WORKER_PRIORITY" not in background
    after_dispatch = dispatcher.split(
        "d1l_map_view_service_run_pending()", 1
    )[1]
    assert "D1L_MAP_PREFETCH_WORKER_PRIORITY" not in after_dispatch


def test_background_https_wait_is_bounded_and_wake_cancelable():
    prefetch = read("main/map/map_prefetch_service.c")
    store = read("main/storage/map_tile_store.c")
    store_header = read("main/storage/map_tile_store.h")
    fetch = store.split(
        "static esp_err_t map_tile_store_fetch_network", 1
    )[1].split("\nesp_err_t d1l_map_tile_store_fetch", 1)[0]
    foreground = store.split(
        "esp_err_t d1l_map_tile_store_fetch(uint8_t z", 1
    )[1].split(
        "esp_err_t d1l_map_tile_store_fetch_background", 1
    )[0]
    background = store.split(
        "esp_err_t d1l_map_tile_store_fetch_background", 1
    )[1]
    event = store.split(
        "static esp_err_t map_http_event", 1
    )[1].split("static bool png_content_type", 1)[0]
    publish_socket = store.split(
        "static void background_fetch_publish_socket", 1
    )[1].split("static void background_fetch_clear_socket", 1)[0]
    clear_socket = store.split(
        "static void background_fetch_clear_socket", 1
    )[1].split("static void background_fetch_detach_socket", 1)[0]
    cancel = store.split(
        "void d1l_map_tile_store_cancel_background_fetch", 1
    )[1].split("static esp_err_t request_gate_wait", 1)[0]
    gate = store.split(
        "static esp_err_t request_gate_wait", 1
    )[1].split("bool d1l_map_tile_store_coord_valid", 1)[0]
    network_done = fetch.split("network_done:", 1)[1]

    assert "d1l_map_tile_store_fetch_background(" in store_header
    assert "d1l_map_tile_store_fetch_background(" in prefetch
    assert "D1L_MAP_TILE_HTTP_TIMEOUT_MS 15000" in store
    assert "D1L_MAP_TILE_BACKGROUND_HTTP_TIMEOUT_MS 5000" in store
    assert "D1L_MAP_TILE_HTTP_IO_SLICE_MS 250" in store
    assert "D1L_MAP_TILE_HTTP_TIMEOUT_MS" in foreground
    assert "D1L_MAP_TILE_BACKGROUND_HTTP_TIMEOUT_MS" in background
    assert "false, buffer, buffer_size" in foreground
    assert "D1L_MAP_TILE_BACKGROUND_HTTP_TIMEOUT_MS, true" in background
    assert ".timeout_ms = D1L_MAP_TILE_HTTP_IO_SLICE_MS" in fetch
    assert ".is_async = true" in fetch
    assert "} while (ret == ESP_ERR_HTTP_EAGAIN);" in fetch
    open_wait = fetch.split(
        "ret = esp_http_client_open(client, 0)", 1
    )[1].split("if (ret != ESP_OK)", 1)[0]
    assert "esp_timer_get_time() >= open_deadline_us" in open_wait
    assert "while (content_length == -ESP_ERR_HTTP_EAGAIN)" in fetch
    header_wait = fetch.split(
        "while (content_length == -ESP_ERR_HTTP_EAGAIN)", 1
    )[1].split("result.status_code =", 1)[0]
    assert "map_network_continue(&continuation)" in header_wait
    assert "esp_timer_get_time() >= header_deadline_us" in header_wait
    read_wait = fetch.split(
        "if (read_len == -ESP_ERR_HTTP_EAGAIN)", 1
    )[1].split("if (read_len < 0)", 1)[0]
    assert "map_network_continue(&continuation)" in read_wait
    assert "esp_timer_get_time() >= read_deadline_us" in read_wait

    assert "esp_http_client_cancel_request" not in store
    assert "esp_http_client_close" not in cancel
    assert "esp_http_client_cleanup" not in cancel
    assert cancel.index("cancel_requested = true") < cancel.index(
        "shutdown("
    )
    assert "s_background_socket.token == token" in publish_socket
    assert publish_socket.index(
        "s_background_socket.cancel_requested"
    ) < publish_socket.index("shutdown(")
    assert "s_background_socket.token == token" in clear_socket
    assert "s_background_socket.socket_fd == socket_fd" in clear_socket
    assert "HTTP_EVENT_ON_CONNECTED" in event
    assert "esp_http_client_get_socket(event->client)" in event
    assert "background_fetch_publish_socket(" in event
    assert "HTTP_EVENT_DISCONNECTED" in event
    assert "background_fetch_clear_socket(" in event
    assert "HTTP_EVENT_HEADERS_SENT" in event
    assert "context->minimum_request_interval_ms" in event
    assert "request_gate_extend_minimum(" in event
    assert "D1L_MAP_TILE_REQUEST_GATE_SLICE_MS 25U" in store
    assert "request_gate_wait(" in fetch
    assert fetch.index("request_gate_wait(") < fetch.index(
        "esp_http_client_init(&config)"
    )
    assert "result.status_code == 429" in fetch
    assert "result.status_code == 503" in fetch
    assert "D1L_MAP_TILE_DEFAULT_RETRY_AFTER_SEC" in fetch
    assert "request_gate_extend_retry(" in fetch
    assert gate.index(
        "retry_remaining_us > 0"
    ) < gate.index(
        "*out_retry_status = retry_status"
    ) < gate.index(
        "return ESP_ERR_TIMEOUT"
    ) < gate.index(
        "minimum_until_us - now_us"
    ) < gate.index("vTaskDelay(")
    gated_fetch = fetch.split("ret = request_gate_wait(", 1)[1].split(
        "map_http_context_t http_context", 1
    )[0]
    assert "result.status_code = gate_retry_status" in gated_fetch
    assert 'download_step(\n                &result, "rate_limited"' in gated_fetch
    post_headers = fetch.split("if (content_length >= 0)", 1)[1]
    assert post_headers.index(
        "result.status_code ="
    ) < post_headers.index(
        "result.status_code == 429"
    ) < post_headers.index(
        "goto network_done;"
    ) < post_headers.index(
        "if (!map_network_continue(&continuation))"
    )
    assert network_done.index(
        "background_fetch_detach_socket("
    ) < network_done.index(
        "esp_http_client_close(client)"
    ) < network_done.index(
        "esp_http_client_cleanup(client)"
    ) < network_done.index(
        "persist_validated_tile("
    ) < network_done.index(
        "background_fetch_finish("
    )
    assert "d1l_map_tile_store_cancel_background_fetch(" in store_header
    request_tail = prefetch.split(
        "ret = d1l_map_tile_store_fetch_background(", 1
    )[1].split("set_phase(status, \"ready\"", 1)[0]
    assert request_tail.index(
        "result.status_code == 429"
    ) < request_tail.index(
        "if (result.cancelled ||"
    )
    assert "wait_minimum_request_interval" not in prefetch


def test_token_and_source_gate_models_preserve_cancellation_and_backoff():
    socket = {
        "token": 1,
        "fd": -1,
        "active": True,
        "cancelled": False,
    }

    def cancel(state):
        state["cancelled"] = state["active"]
        return state["fd"] if state["active"] and state["fd"] >= 0 else None

    def publish(state, token, fd):
        if not state["active"] or state["token"] != token:
            return None
        state["fd"] = fd
        return fd if state["cancelled"] else None

    assert cancel(socket) is None
    assert publish(socket, 1, 7) == 7
    socket["active"] = False
    socket = {
        "token": 2,
        "fd": -1,
        "active": True,
        "cancelled": False,
    }
    assert publish(socket, 1, 7) is None
    assert publish(socket, 2, 7) is None

    minimum_gate = {}
    retry_gate = {}

    def extend_minimum(source, deadline):
        minimum_gate[source] = max(
            minimum_gate.get(source, 0), deadline
        )

    def extend_retry(source, status, deadline):
        retry_gate[source] = (
            status,
            max(retry_gate.get(source, (0, 0))[1], deadline),
        )

    extend_minimum("provider-a", 1000)
    extend_minimum("provider-a", 500)
    extend_retry("provider-a", 429, 300000)
    extend_minimum("provider-b", 1200)
    assert minimum_gate == {
        "provider-a": 1000,
        "provider-b": 1200,
    }
    assert retry_gate == {
        "provider-a": (429, 300000),
    }
    assert retry_gate["provider-a"][1] > minimum_gate["provider-a"]


def test_map_ui_exposes_provider_and_background_state():
    ui = read("main/ui/ui_map.c")
    view_header = read("main/map/map_view_service.h")
    view_source = read("main/map/map_view_service.c")

    assert '#include "map/map_prefetch_service.h"' in ui
    assert "d1l_map_prefetch_service_status(&prefetch)" in ui
    assert '"Ready through z%u"' in ui
    assert '"Paused for Map"' in ui
    assert "interactive cache only" in ui
    assert "provider_max_zoom" in view_header
    assert "s_map.status.provider_max_zoom = provider->max_zoom" in view_source
    assert "provider.max_zoom" in ui
