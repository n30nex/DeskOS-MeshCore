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
    assert "d1l_map_tile_store_fetch(" in service
    assert "plan->reserve_bytes + D1L_MAP_TILE_DOWNLOAD_MAX_BYTES" in service
    assert "provider->minimum_request_interval_ms" in service
    assert "cache_budget_mb = provider.cache_budget_mb" in service
    assert "status->evicted_tiles += result.evicted_tiles" in service
    assert "status->cache_used_bytes = result.cache_used_bytes" in service


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
    assert (
        dispatcher.index(
            "vTaskPrioritySet(NULL, D1L_MAP_VISIBLE_WORKER_PRIORITY)"
        )
        < dispatcher.index("d1l_map_view_service_run_pending()")
        < dispatcher.index(
            "vTaskPrioritySet(NULL, D1L_MAP_PREFETCH_WORKER_PRIORITY)"
        )
    )
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
