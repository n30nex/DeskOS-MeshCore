from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def test_builtin_openstreetmap_source_is_fixed_identified_and_attributed():
    header = read("main/storage/map_tile_store.h")
    console = read("main/comms/usb_console.c")

    assert 'D1L_MAP_TILE_SOURCE_ID "openstreetmap-standard"' in header
    assert (
        'D1L_MAP_TILE_SOURCE_URL_TEMPLATE '
        '"https://tile.openstreetmap.org/{z}/{x}/{y}.png"'
    ) in header
    assert (
        'D1L_MAP_TILE_USER_AGENT '
        '"MeshCore-DeskOS-D1L/1.0 (+https://github.com/n30nex/DeskOS-MeshCore)"'
    ) in header
    assert 'D1L_MAP_TILE_LICENSE_URL "https://www.openstreetmap.org/copyright"' in header
    assert 'D1L_MAP_TILE_ATTRIBUTION "\\xC2\\xA9 OpenStreetMap contributors"' in header
    assert "D1L_MAP_TILE_MIN_CACHE_DAYS 7U" in header
    assert "D1L_MAP_TILE_DOWNLOAD_MAX_BYTES (196U * 1024U)" in header

    assert "provider_saved" not in console
    assert "map-provider" not in console
    assert "storage map-tile-download" not in console
    assert "<url-template>" not in console
    assert "cmd_map_tiles_status" in console
    assert 'strcmp(line, "map tiles status")' in console


def test_current_view_planner_accepts_bounded_zoom_and_stays_at_most_three_by_three():
    header = read("main/map/map_math.h")
    source = read("main/map/map_math.c")

    assert "D1L_MAP_VIEW_DEFAULT_ZOOM 10U" in header
    assert "D1L_MAP_VIEW_MIN_ZOOM 8U" in header
    assert "D1L_MAP_VIEW_MAX_ZOOM 18U" in header
    assert "D1L_MAP_VIEW_MAX_TILES 9U" in header
    assert (
        "zoom >= D1L_MAP_VIEW_MIN_ZOOM && zoom <= D1L_MAP_VIEW_MAX_ZOOM"
        in source
    )
    assert "(max_unwrapped_x - min_unwrapped_x + 1LL) > 3LL" in source
    assert "(max_y - min_y + 1LL) > 3LL" in source
    assert "planned_count >= D1L_MAP_VIEW_MAX_TILES" in source
    assert "wrap_tile_x(raw_x, tile_count)" in source
    assert "raw_y < 0 || raw_y >= (int64_t)tile_count" in source
    assert "already_planned(planned, planned_count, x, y)" in source
    assert "sort_center_first(planned, planned_count)" in source
    assert "distance_sq" in source


def test_touch_pan_center_uses_bounded_web_mercator_math():
    header = read("main/map/map_math.h")
    source = read("main/map/map_math.c")
    pan = body(source, "bool d1l_map_math_pan_center", "\n}")

    assert "d1l_map_math_pan_center" in header
    assert "!map_zoom_valid(zoom)" in pan
    assert "D1L_MAP_MERCATOR_MAX_LAT_E7" in pan
    assert "world_x + (double)delta_x_pixels" in pan
    assert "world_y += (double)delta_y_pixels" in pan
    assert "fmod(" in pan
    assert "atan(sinh(mercator))" in pan
    assert "clamp_i64(new_lon_e7, -1800000000LL, 1800000000LL)" in pan


def test_worker_is_sequential_cancelable_and_never_fetches_without_persistent_cache():
    service = read("main/map/map_view_service.c")
    store = read("main/storage/map_tile_store.c")
    run = body(
        service,
        "static void run_generation",
        "void d1l_map_view_service_run_pending",
    )
    wait_for_sd = body(
        service, "static bool wait_for_sd_cache", "static void fill_placeholder"
    )
    wait_reason = body(
        service,
        "static void set_storage_wait_message_locked",
        "static bool generation_visible_locked",
    )

    assert run.count("for (uint8_t i = 0;") == 1
    assert run.index("d1l_map_tile_store_read(") < run.index("d1l_map_tile_store_fetch(")
    assert "wait_for_sd_cache(generation, &storage)" in run
    assert run.index("wait_for_sd_cache(generation, &storage)") < run.index(
        "publish_initial_frame(plan, generation)"
    ) < run.index("for (uint8_t i = 0;")
    assert "generation_continue(&generation)" in wait_for_sd
    assert "d1l_storage_status(&storage)" in wait_for_sd
    assert "d1l_map_tile_store_sd_ready(&storage)" in wait_for_sd
    assert "*out_storage = storage" in wait_for_sd
    assert "set_storage_wait_message_locked(&storage)" in wait_for_sd
    assert "set_storage_wait_message_locked(&storage)" in run
    for phase in (
        '"sd_attention"',
        '"sd_card_required"',
        '"sd_fat32_required"',
        '"sd_reconnecting"',
        '"sd_cache_required"',
    ):
        assert phase in wait_reason
    assert '"insert_card"' in wait_reason
    assert '"wait_for_storage_reconnect"' in wait_reason
    assert "D1L_MAP_SD_POLL_MS 500U" in service
    for forbidden in (
        "d1l_storage_status_refresh",
        "d1l_storage_status_mount",
        "d1l_storage_manager_reset_bridge",
        "d1l_map_tile_store_read",
        "d1l_map_tile_store_fetch",
        "network_requests",
    ):
        assert forbidden not in wait_for_sd
    assert "generation_continue(&generation)" in run
    assert "generation_continue, &generation" in run
    assert "wait_for_wifi(generation)" in run
    assert "++s_map.status.network_requests" in run
    assert run.index("++s_map.status.network_requests") < run.index(
        "d1l_map_tile_store_fetch("
    )
    assert "tile_result.status_code == 429 || tile_result.status_code == 503" in run
    assert "D1L_MAP_DEFAULT_RETRY_AFTER_SEC" in run
    assert "tile_result.retry_after_sec > 0U" in run
    assert "tile_result.status_code != 0 && tile_result.status_code != 200" in run
    systemic_http = run.split(
        "tile_result.status_code != 0 && tile_result.status_code != 200", 1
    )[1].split("continue;", 1)[0]
    assert "break;" in systemic_http
    assert "bool downloaded = false;" in run
    assert "downloaded = true;" in run
    cache_pacing = run.split(
        "if (!publish_tile_frame(plan, &plan->tiles[i], generation))", 1
    )[1].split("\n    }", 1)[0]
    assert "if (downloaded)" in cache_pacing
    assert "vTaskDelay(pdMS_TO_TICKS(D1L_MAP_TILE_GAP_MS));" in cache_pacing
    assert "taskYIELD();" in cache_pacing

    fetch = body(
        store,
        "static esp_err_t map_tile_store_fetch_network",
        "\nesp_err_t d1l_map_tile_store_fetch",
    )
    assert fetch.index("if (!wifi_connected)") < fetch.index("esp_http_client_init")
    assert fetch.index("if (!result.sd_ready)") < fetch.index("esp_http_client_init")
    assert "map_network_continue(&continuation)" in fetch
    assert ".user_agent = D1L_MAP_TILE_USER_AGENT" in fetch
    assert ".crt_bundle_attach = esp_crt_bundle_attach" in fetch
    assert "result.status_code == 429" in fetch
    assert "png_content_type(http_context.content_type)" in fetch
    assert "d1l_map_tile_png_valid(buffer, result.bytes)" in fetch


def test_same_generation_retry_resets_bounded_pass_progress_before_tile_io():
    header = read("main/map/map_view_service.h")
    service = read("main/map/map_view_service.c")
    console = read("main/comms/usb_console.c")
    run = body(
        service,
        "static void run_generation",
        "void d1l_map_view_service_run_pending",
    )
    before_io = run.split("d1l_storage_status(&storage);", 1)[0]

    assert "xSemaphoreTake(s_map.lock, pdMS_TO_TICKS(100))" in before_io
    assert "if (!generation_visible_locked(generation))" in before_io
    invalid_generation = before_io.split(
        "if (!generation_visible_locked(generation))", 1
    )[1].split("}", 1)[0]
    assert "xSemaphoreGive(s_map.lock)" in invalid_generation
    assert "return;" in invalid_generation

    for field in (
        "attempted_tiles",
        "cache_hits",
        "network_requests",
        "downloaded_tiles",
        "rendered_tiles",
        "failed_tiles",
        "decode_total_us",
        "decode_max_us",
        "decode_samples",
        "retry_after_sec",
    ):
        assert f"s_map.status.{field} = 0U;" in before_io
    assert "s_map.status.rate_limited = false;" in before_io
    assert "D1L_MAP_VIEW_MAX_GENERATION_PASSES 3U" in header
    backoff_guard = before_io.split(
        "if (now_us < s_map.backoff_until_us)", 1
    )[1].split("}", 1)[0]
    assert "s_map.status.worker_running = false;" in backoff_guard
    assert "s_map.status.rate_limited = true;" in backoff_guard
    assert "s_map.status.retry_after_sec =" in backoff_guard
    assert "xSemaphoreGive(s_map.lock);" in backoff_guard
    assert "return;" in backoff_guard
    assert before_io.index(
        "if (now_us < s_map.backoff_until_us)"
    ) < before_io.index(
        "if (s_map.status.pass_attempts >="
    ) < before_io.index(
        "++s_map.status.pass_attempts;"
    ) < before_io.index(
        "s_map.status.attempted_tiles = 0U;"
    )
    assert "++s_map.status.pass_attempts;" in before_io
    assert (
        "s_map.status.pass_attempts >=\n"
        "        D1L_MAP_VIEW_MAX_GENERATION_PASSES"
    ) in before_io
    for preserved in (
        "s_map.status.generation =",
        "s_map.status.planned_tiles =",
        "s_map.status.frame_revision =",
        "s_map.status.frame_ready =",
        "s_map.status.pass_attempts = 0U;",
    ):
        assert preserved not in before_io
    assert before_io.index("s_map.status.attempted_tiles = 0U;") < before_io.rindex(
        "xSemaphoreGive(s_map.lock)"
    )
    assert run.index("s_map.status.attempted_tiles = 0U;") < run.index(
        "d1l_map_tile_provider_refresh(&storage)"
    ) < run.index(
        "publish_initial_frame(plan, generation)"
    ) < run.index(
        "for (uint8_t i = 0;"
    )
    assert (
        "if (publish_placeholder &&\n"
        "        !publish_initial_frame(plan, generation))"
    ) in run
    assert "const bool publish_placeholder = !s_map.status.frame_ready;" in before_io
    worker = body(
        service,
        "void d1l_map_view_service_run_pending",
        "esp_err_t d1l_map_view_service_init",
    )
    bounded_guard = worker.split(
        "if (s_map.status.pass_attempts >=", 1
    )[1].split("plan = s_map.plan;", 1)[0]
    assert "D1L_MAP_VIEW_MAX_GENERATION_PASSES" in bounded_guard
    assert "s_map.status.worker_running = false;" in bounded_guard
    assert "break;" in bounded_guard
    assert worker.index("if (s_map.status.pass_attempts >=") < worker.index(
        "run_generation(&plan, generation)"
    )
    assert '\\"pass_attempts\\":%u' in console
    assert '\\"max_pass_attempts\\":%u' in console


def test_product_map_status_retains_actionable_last_tile_failure_diagnostics():
    header = read("main/map/map_view_service.h")
    service = read("main/map/map_view_service.c")
    console = read("main/comms/usb_console.c")
    run = body(
        service,
        "static void run_generation",
        "void d1l_map_view_service_run_pending",
    )

    for field in (
        "last_failure_step",
        "last_failure_detail_step",
        "last_failure_error",
        "last_failure_http_status",
        "last_failure_retry_after_sec",
        "last_failure_bytes",
        "last_failure_content_type_valid",
        "last_failure_png_valid",
        "last_failure_checksum_verified",
        "last_failure_cache_intent_recorded",
        "last_failure_zoom",
        "last_failure_x",
        "last_failure_y",
        "last_failure_file_ok",
        "last_failure_file_response_truncated",
        "last_failure_file_cancelled",
        "last_failure_file_error",
        "last_failure_file_size",
        "last_failure_file_offset",
        "last_failure_file_length",
        "last_failure_file_op",
        "last_failure_file_err",
        "last_failure_file_note",
    ):
        assert field in header
        assert field in console
    assert "static void note_tile_failure" in service
    assert "result->step[0] ? result->step : \"unknown\"" in service
    assert "result->last_error" in service
    assert "result->status_code" in service
    assert "result->persistence_step" in service
    assert run.index("note_tile_failure(generation, &tile_result)") < run.index(
        "note_failure(generation, tile_result.step"
    )
    assert '\\"last_failure\\":{' in console
    assert '\\"detail_step\\":' in console
    assert '\\"file\\":{\\"ok\\":%s' in console
    assert "esp_err_to_name(status.last_failure_error)" in console
    assert "esp_err_to_name(status.last_failure_file_error)" in console
    assert "status.last_failure_file_error != ESP_OK ?" in console


def test_cache_persist_failure_preserves_specific_rp2040_file_diagnostics():
    header = read("main/map/map_view_service.h")
    store = read("main/storage/map_tile_store.c")
    persist = body(
        store,
        "static esp_err_t persist_validated_tile",
        "static esp_err_t map_tile_store_fetch_network",
    )
    fetch = body(
        store,
        "static esp_err_t map_tile_store_fetch_network",
        "\nesp_err_t d1l_map_tile_store_fetch",
    )

    assert persist.index(
        "d1l_rp2040_bridge_file_write_verified("
    ) < persist.index(
        "result->file = file;"
    ) < persist.index(
        'download_step(result, "cache_write", ret, &file);'
    )
    assert "persistence_step(result, s_cache_recovery_stage);" in persist
    assert "download_step(result, s_cache_recovery_stage, ret, NULL);" in persist
    assert 'download_step(\n                result, "cache_commit", ret' in persist
    for detail in (
        '"cache_lock"',
        '"cache_state"',
        '"cache_write"',
        '"cache_attribution"',
        '"cache_room"',
        '"cache_metadata"',
        '"cache_intent"',
        '"cache_tile_rename"',
        '"cache_metadata_rename"',
        '"cache_state_note"',
        '"cache_state_write"',
    ):
        assert detail in store
    assert "char persistence_step[32];" in read("main/storage/map_tile_store.h")
    assert "char last_failure_detail_step[32];" in header
    assert (
        "append_cache_intent(&paths, state, &record, result)"
        in store
    )
    assert "&result->file" in body(
        store,
        "static esp_err_t append_cache_intent",
        "static esp_err_t rename_cache_metadata",
    )
    failure = fetch.split("if (persist_tile) {", 1)[1].split(
        "background_fetch_finish(", 1
    )[0]
    assert "result.cancelled" in failure
    assert "result.step[0] == '\\0'" in failure
    assert '"cache_persist"' in failure
    assert "result.last_error = ret;" in failure


def test_wifi_shutdown_drains_owner_closed_http_client_before_driver_teardown():
    store = read("main/storage/map_tile_store.c")
    connectivity = read("main/comms/connectivity_manager.c")
    header = read("main/comms/connectivity_manager.h")
    fetch = body(
        store,
        "static esp_err_t map_tile_store_fetch_network",
        "\nesp_err_t d1l_map_tile_store_fetch",
    )
    network_done = fetch.split("network_done:", 1)[1]
    after_init = fetch.split("client = esp_http_client_init(&config);", 1)[1].split(
        "network_done:", 1
    )[0]

    assert "d1l_connectivity_network_lease_begin" in header
    assert "d1l_connectivity_network_lease_end" in header
    assert "d1l_connectivity_network_cancel_requested" in header
    assert fetch.index(
        "d1l_connectivity_network_lease_begin("
    ) < fetch.index(
        "d1l_time_service_wait_for_certificate_time("
    ) < fetch.index(
        "esp_http_client_init(&config)"
    )
    assert "return" not in after_init
    assert fetch.count("map_network_continue(&continuation)") >= 5
    assert fetch.index("esp_http_client_open(client, 0)") < fetch.index(
        "map_network_continue(&continuation)",
        fetch.index("esp_http_client_open(client, 0)"),
    ) < fetch.index("esp_http_client_fetch_headers(client)")
    assert network_done.index("esp_http_client_close(client)") < network_done.index(
        "esp_http_client_cleanup(client)"
    ) < network_done.index(
        "d1l_connectivity_network_lease_end()"
    ) < network_done.index(
        "persist_validated_tile("
    )
    assert (
        "!d1l_connectivity_network_cancel_requested()"
        in store
    )
    assert "D1L_WIFI_NETWORK_QUIESCE_TIMEOUT_MS 20000U" in connectivity


def test_osm_standard_tiles_are_dark_styled_locally_after_decode():
    decoder = read("main/map/map_png_decoder.c")
    service = read("main/map/map_view_service.c")
    store = read("main/storage/map_tile_store.h")

    assert "static uint16_t dark_style_rgb565" in decoder
    assert '#define D1L_MAP_RENDER_STYLE_ID "local-dark-v1"' in read(
        "main/map/map_png_decoder.h"
    )
    assert "77U * red" in decoder
    assert "150U * green" in decoder
    assert "29U * blue" in decoder
    assert "+ 128U) >> 8U" in decoder
    assert "(255U - luminance) * 207U + 128U" in decoder
    assert "dark_style_chroma_adjust" in decoder
    assert "delta * 3" in decoder
    assert "out_pixels[i] = dark_style_rgb565(red, green, blue)" in decoder
    assert "d1l_map_png_decode_rgb565" in service
    assert "decode_started_us = esp_timer_get_time()" in service
    assert "decode_total_us" in service
    assert "decode_max_us" in service
    assert "decode_samples" in service
    console = read("main/comms/usb_console.c")
    assert "D1L_MAP_RENDER_STYLE_ID" in console
    assert '\\"decode_total_us\\":%lu' in console
    assert '\\"decode_max_us\\":%lu' in console
    assert 'D1L_MAP_TILE_SOURCE_ID "openstreetmap-standard"' in store
    assert "tile.openstreetmap.org" in store


def test_local_dark_style_golden_palette_and_grayscale_contrast():
    def styled_rgb565(red: int, green: int, blue: int) -> int:
        luminance = (77 * red + 150 * green + 29 * blue + 128) >> 8
        dark_luminance = 14 + (((255 - luminance) * 207 + 128) >> 8)

        def adjust(channel: int) -> int:
            scaled = (channel - luminance) * 3
            if scaled > 0:
                return (scaled + 4) // 8
            if scaled < 0:
                return -((-scaled + 4) // 8)
            return 0

        channels = [
            max(0, min(255, dark_luminance + adjust(channel)))
            for channel in (red, green, blue)
        ]
        return ((channels[0] >> 3) << 11) | ((channels[1] >> 2) << 5) | (
            channels[2] >> 3
        )

    assert styled_rgb565(255, 255, 255) == 0x0861
    assert styled_rgb565(242, 239, 233) == 0x18C3
    assert styled_rgb565(68, 68, 68) == 0xA534
    assert styled_rgb565(170, 211, 223) == 0x29E8
    assert styled_rgb565(0, 0, 0) == 0xDEFB

    grayscale = [styled_rgb565(value, value, value) for value in range(256)]
    decoded_luminance = [
        (((pixel >> 11) & 0x1F) << 3) + (((pixel >> 5) & 0x3F) << 2) +
        ((pixel & 0x1F) << 3)
        for pixel in grayscale
    ]
    assert all(left >= right for left, right in zip(decoded_luminance, decoded_luminance[1:]))


def test_http_header_length_contract_handles_errors_chunking_and_hard_bounds():
    store = read("main/storage/map_tile_store.c")
    fetch = body(
        store,
        "static esp_err_t map_tile_store_fetch_network",
        "\nesp_err_t d1l_map_tile_store_fetch",
    )

    header_fetch = fetch.split(
        "esp_http_client_fetch_headers(client)", 1
    )[1].split("const bool chunked", 1)[0]
    assert "if (content_length < 0)" in header_fetch
    assert 'download_step(&result, "fetch_headers", ESP_FAIL' in header_fetch
    assert "esp_http_client_is_chunked_response(client)" in fetch
    assert "content_length_known = !chunked && content_length > 0" in fetch
    assert "buffer_size < D1L_MAP_TILE_DOWNLOAD_MAX_BYTES" in fetch

    assert "content_length_known && content_length > (int64_t)download_limit" in fetch
    assert "const size_t remaining = download_limit - result.bytes" in fetch
    assert "(size_t)read_len > want" in fetch
    assert "content_length_known && result.bytes != (size_t)content_length" in fetch
    assert "content_length >= 0 && result.bytes" not in fetch


def test_https_download_waits_for_valid_sntp_time_and_remains_cancelable():
    store = read("main/storage/map_tile_store.c")
    time_service = read("main/platform/time_service.c")
    service = read("main/map/map_view_service.c")
    ui = read("main/ui/ui_map.c")
    defaults = read("sdkconfig.defaults")
    fetch = body(
        store,
        "static esp_err_t map_tile_store_fetch_network",
        "\nesp_err_t d1l_map_tile_store_fetch",
    )
    clock = body(
        time_service,
        "static esp_err_t wait_for_time",
        "esp_err_t d1l_time_service_wait_for_certificate_time",
    )
    run = body(
        service,
        "static void run_generation",
        "void d1l_map_view_service_run_pending",
    )

    assert 'ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org")' in time_service
    assert "config.wait_for_sync = true" in time_service
    assert "esp_netif_sntp_init(&config)" in time_service
    assert "esp_netif_sntp_sync_wait(wait_ticks)" in clock
    assert "continue_allowed(should_continue, continue_context)" in clock
    assert "D1L_TIME_TLS_WAIT_SLICE_MS" in fetch
    assert fetch.index("d1l_time_service_wait_for_certificate_time(") < fetch.index(
        "esp_http_client_init(&config)"
    )
    assert "esp_netif_sntp" not in store
    assert "time(NULL)" not in store
    assert 'download_step(&result, "time_sync"' in fetch
    assert "D1L_MAP_TILE_HTTP_IO_SLICE_MS 250" in store
    assert ".timeout_ms = D1L_MAP_TILE_HTTP_IO_SLICE_MS" in fetch
    assert ".is_async = true" in fetch
    assert "} while (ret == ESP_ERR_HTTP_EAGAIN);" in fetch
    assert "while (content_length == -ESP_ERR_HTTP_EAGAIN)" in fetch
    assert "if (read_len == -ESP_ERR_HTTP_EAGAIN)" in fetch
    assert fetch.count("map_network_continue(&continuation)") >= 7
    assert "esp_timer_get_time() >= header_deadline_us" in fetch
    assert "esp_timer_get_time() >= read_deadline_us" in fetch
    assert 'strcmp(tile_result.step, "time_sync") == 0' in run
    assert 'strcmp(status->phase, "time_sync") == 0' in ui
    assert 'title = "Secure time needed"' in ui
    assert "CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y" in defaults
    assert "CONFIG_MBEDTLS_HAVE_TIME_DATE=y" in defaults


def test_cache_commit_requires_valid_png_and_attribution_metadata_atomically():
    source = read("main/storage/map_tile_store.c")
    read_cache = body(
        source,
        "static esp_err_t map_tile_store_read_locked",
        "\nesp_err_t d1l_map_tile_store_read",
    )
    fetch = body(
        source,
        "static esp_err_t map_tile_store_fetch_network",
        "\nesp_err_t d1l_map_tile_store_fetch",
    )
    persist = body(
        source,
        "static esp_err_t persist_validated_tile",
        "static esp_err_t map_tile_store_fetch_network",
    )

    assert "attribution_metadata_present(&result)" in read_cache
    assert "d1l_map_tile_png_valid(buffer, result.bytes)" in read_cache
    assert "result.cache_hit = true" in read_cache
    assert "const uint32_t expected_size = file.size" in read_cache
    assert "while (result.bytes < expected_size)" in read_cache
    assert "d1l_rp2040_file_result_t read_result = {0}" in read_cache
    assert "read_result.eof !=" in read_cache
    assert "result.bytes != (size_t)expected_size || !saw_eof" in read_cache
    assert "result.content_crc32 == metadata.content_crc32" in read_cache
    assert "d1l_map_tile_png_valid(buffer, result.bytes)" in fetch
    assert persist.index(
        "verify_tile_file_continue("
    ) < persist.index(
        "write_attribution_metadata("
    ) < persist.index("commit_cache_tile(")
    commit = body(
        source,
        "static esp_err_t commit_cache_tile",
        "static bool attribution_metadata_present",
    )
    assert commit.index("write_cache_metadata_tmp(result, &record)") < commit.index(
        "append_cache_intent(&paths, state, &record, result)"
    ) < commit.index(
        "d1l_rp2040_bridge_file_rename("
    ) < commit.index("rename_cache_metadata(result)")
    assert "write_cache_state(&paths, state)" in commit
    assert "!result->cache_intent_recorded" in persist


def test_background_persistence_is_cancelable_until_journal_intent():
    source = read("main/storage/map_tile_store.c")
    persist = body(
        source,
        "static esp_err_t persist_validated_tile",
        "static esp_err_t map_tile_store_fetch_network",
    )
    commit = body(
        source,
        "static esp_err_t commit_cache_tile",
        "static bool attribution_metadata_present",
    )
    prepare = body(
        source,
        "static esp_err_t prepare_cache_room",
        "static esp_err_t reconcile_cache_intent",
    )
    verify = body(
        source,
        "static esp_err_t verify_tile_file_continue",
        "static esp_err_t verify_tile_file(",
    )
    cancellable_take = body(
        source,
        "static esp_err_t cache_transaction_take_cancelable",
        "static bool cache_control_paths",
    )

    assert "cache_transaction_take_cancelable(" in persist
    assert "D1L_MAP_TILE_REQUEST_GATE_SLICE_MS" in cancellable_take
    assert "persistence_continue(" in cancellable_take
    stream_write = persist.split(
        "d1l_rp2040_bridge_file_write_verified(", 1
    )[1].split("write_attribution_metadata(", 1)[0]
    assert "should_continue" in stream_write
    assert "continue_context" in stream_write
    assert "if (file.cancelled)" in stream_write
    before_stream_write = persist.split(
        "d1l_rp2040_bridge_file_write_verified(", 1
    )[0]
    assert "persistence_continue(" in before_stream_write
    verify_loop = verify.split("while (offset < record->size)", 1)[1]
    assert verify_loop.index("persistence_continue(") < verify_loop.index(
        "d1l_rp2040_bridge_file_read("
    )
    eviction = prepare.split(
        "while (!d1l_map_tile_cache_state_has_room", 1
    )[1]
    assert eviction.index("persistence_continue(") < eviction.index(
        "d1l_rp2040_bridge_file_delete("
    )
    assert commit.index("write_cache_metadata_tmp(") < commit.index(
        "persistence_continue("
        , commit.index("write_cache_metadata_tmp(")
    ) < commit.index("result->cache_intent_recorded = true")
    irreversible = commit.split(
        "result->cache_intent_recorded = true", 1
    )[1]
    assert "persistence_continue(" not in irreversible
    assert (
        irreversible.index("append_cache_intent(")
        < irreversible.index("d1l_rp2040_bridge_file_rename(")
        < irreversible.index("rename_cache_metadata(")
        < irreversible.index("d1l_map_tile_cache_state_note_commit(")
        < irreversible.index("write_cache_state(")
    )
    assert "!result->cancelled" in persist


def test_worker_publishes_immutable_psram_frames_without_lvgl_calls_or_replay():
    service = read("main/map/map_view_service.c")
    worker = body(
        service,
        "void d1l_map_view_service_run_pending",
        "esp_err_t d1l_map_view_service_init",
    )
    release = service.split("void d1l_map_view_service_release_frame", 1)[1]

    assert "uint16_t *frames[2]" in service
    assert "uint8_t frame_readers[2]" in service
    assert service.count("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT") >= 4
    assert "wait_for_frame_slot(work, generation)" in service
    assert "++s_map.frame_readers[slot]" in service
    assert "--s_map.frame_readers[frame->slot]" in service
    assert "xSemaphoreTake(s_map.lock, portMAX_DELAY)" in release
    assert release.index("--s_map.frame_readers[frame->slot]") < release.index(
        "memset(frame, 0, sizeof(*frame))"
    )
    assert "lv_" not in service
    assert "#include \"lvgl" not in service
    assert "ulTaskNotifyTake(pdTRUE, 0U)" in worker
    assert "s_map.status.generation != generation" in worker
    assert "s_map.status.frame_ready = true" in service
    assert service.index("s_map.status.frame_ready = true") < service.index(
        "++s_map.status.frame_revision"
    )


def test_same_visible_or_complete_hidden_plan_reuses_frame_without_worker_replay():
    service = read("main/map/map_view_service.c")
    acquire = body(
        service,
        "esp_err_t d1l_map_view_service_acquire_visible",
        "void d1l_map_view_service_release_visible",
    )
    identical = acquire.split("if (!force_reload && same_plan &&", 1)[1].split(
        "uint32_t generation = s_map.status.generation + 1U", 1
    )[0]
    completed = body(
        service, "static bool completed_frame_locked", "static bool generation_continue"
    )
    worker = body(
        service,
        "void d1l_map_view_service_run_pending",
        "esp_err_t d1l_map_view_service_init",
    )

    assert "s_map.status.generation != 0U" in acquire
    assert "s_map.status.visible = true" in identical
    assert "if (completed_frame_locked())" in identical
    assert 'set_message_locked("ready", "Map ready")' in identical
    assert "const uint32_t generation = s_map.status.generation" in identical
    assert "*out_generation = generation" in identical
    assert "d1l_map_prefetch_service_wake()" in identical
    assert "s_map.status.failed_tiles > 0U" in identical
    assert "D1L_MAP_VIEW_MAX_GENERATION_PASSES" in identical
    assert "s_map.status.pass_attempts = 0U;" in identical
    assert identical.index("xSemaphoreGive(s_map.lock)") < identical.index(
        "d1l_map_prefetch_service_wake()"
    )
    assert acquire.count("d1l_map_prefetch_service_wake()") == 2
    assert acquire.index("uint32_t generation") < acquire.rindex(
        "d1l_map_prefetch_service_wake()"
    )

    for required in (
        "s_map.status.frame_ready",
        "s_map.status.frame_revision > 0U",
        "s_map.status.planned_tiles > 0U",
        "s_map.status.attempted_tiles == s_map.status.planned_tiles",
        "s_map.status.rendered_tiles == s_map.status.planned_tiles",
        "s_map.status.failed_tiles == 0U",
    ):
        assert required in completed
    assert "if (completed_frame_locked())" in worker
    assert worker.index("if (completed_frame_locked())") < worker.index(
        "run_generation(&plan, generation)"
    )
    assert "ulTaskNotifyTake(pdTRUE, 0U)" in worker


def test_visible_lease_revocation_cannot_time_out_before_worker_notification():
    service = read("main/map/map_view_service.c")
    release = body(
        service,
        "void d1l_map_view_service_release_visible",
        "void d1l_map_view_service_status",
    )

    assert "xSemaphoreTake(s_map.lock, portMAX_DELAY)" in release
    assert "pdMS_TO_TICKS" not in release
    assert (
        release.index("s_map.status.visible = false")
        < release.index("xSemaphoreGive(s_map.lock)")
        < release.index("d1l_map_prefetch_service_wake()")
    )
