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


def test_authorized_default_provider_is_seeded_create_new_and_recovery_aware():
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
        "minimum_request_interval_ms": 1000,
    }
    missing_block = provider.split(
        "if (read_ret == ESP_ERR_NOT_FOUND) {", 1
    )[1].split("d1l_map_tile_provider_t provider", 1)[0]
    assert "find_preserved_invalid_backup(" in missing_block
    assert missing_block.index("find_preserved_invalid_backup(") < (
        missing_block.index("seed_default_provider_config()")
    )
    assert "recovery_ret == ESP_OK" in missing_block
    assert "read_ret = ESP_ERR_INVALID_STATE;" in missing_block
    assert "d1l_rp2040_bridge_file_create(" in provider
    stage_writer = provider.split(
        "static esp_err_t write_default_provider_stage", 1
    )[1].split("static bool default_provider_hash_matches", 1)[0]
    assert "d1l_rp2040_bridge_file_create(" in stage_writer
    assert "offset == 0U" in stage_writer
    assert "offset == 0U, &file" not in stage_writer
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
