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
    create_new_writer = provider.split(
        "static esp_err_t write_provider_create_new", 1
    )[1].split("static esp_err_t write_default_provider_stage", 1)[0]
    assert "d1l_rp2040_bridge_file_create(" in create_new_writer
    assert "d1l_rp2040_bridge_file_write(" in create_new_writer
    assert "offset == 0U" in create_new_writer
    assert "offset == 0U, &file" not in create_new_writer
    stage_writer = provider.split(
        "static esp_err_t write_default_provider_stage", 1
    )[1].split("static bool provider_sha256", 1)[0]
    assert "write_provider_create_new(" in stage_writer
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


def test_provider_recovery_inspection_is_exact_and_read_only():
    header = read("main/map/map_tile_provider.h")
    provider = read("main/map/map_tile_provider.c")
    console = read("main/comms/usb_console.c")

    assert (
        'D1L_MAP_PROVIDER_RECOVERY_STAGE_001_PATH \\\n'
        '    "map/offline-provider.stage-rc1-001.json"'
    ) in header
    assert (
        'D1L_MAP_PROVIDER_RECOVERY_BACKUP_001_PATH \\\n'
        '    "map/offline-provider.invalid-rc1-001.json"'
    ) in header

    path_inspector = provider.split(
        "static esp_err_t inspect_provider_path", 1
    )[1].split("static esp_err_t seed_default_provider_config", 1)[0]
    recovery_inspector = provider.split(
        "esp_err_t d1l_map_tile_provider_inspect_recovery", 1
    )[1].split(
        "esp_err_t d1l_map_tile_provider_repair_invalid_default", 1
    )[0]
    assert "d1l_rp2040_bridge_file_stat(" in path_inspector
    assert "read_provider_path(" in path_inspector
    assert "provider_sha256_hex(" in path_inspector
    assert "parse_provider_config(" in path_inspector
    assert "D1L_MAP_PROVIDER_CONFIG_PATH" in recovery_inspector
    assert "D1L_MAP_PROVIDER_RECOVERY_STAGE_001_PATH" in recovery_inspector
    assert "D1L_MAP_PROVIDER_RECOVERY_BACKUP_001_PATH" in recovery_inspector
    for mutator in (
        "seed_default_provider_config",
        "d1l_map_tile_provider_refresh",
        "d1l_rp2040_bridge_file_create",
        "d1l_rp2040_bridge_file_write",
        "d1l_rp2040_bridge_file_rename",
        "d1l_rp2040_bridge_file_delete",
    ):
        assert mutator not in path_inspector
        assert mutator not in recovery_inspector

    rule = console.split(
        '"map provider recovery-inspect", D1L_RELEASE_COMMAND_READ_ONLY', 1
    )
    assert len(rule) == 2
    command_body = console.split(
        "static void cmd_map_provider_recovery_inspect", 1
    )[1].split("static void print_map_provider_repair_fields", 1)[0]
    output_body = console.split(
        "static void print_map_provider_recovery_inspection_fields", 1
    )[1].split("static void cmd_map_provider_recovery_inspect", 1)[0]
    assert "d1l_map_tile_provider_inspect_recovery(" in command_body
    assert '"read_only\\":true' in output_body
    assert '"mutation_performed\\":false' in output_body
    assert "d1l_map_tile_provider_refresh(" not in command_body


def test_invalid_provider_repair_copies_then_commits_forward_only():
    provider = read("main/map/map_tile_provider.c")
    console = read("main/comms/usb_console.c")
    repair = provider.split(
        "esp_err_t d1l_map_tile_provider_repair_invalid_default", 1
    )[1].split("bool d1l_map_tile_provider_path", 1)[0]
    quiesce_helper = provider.split(
        "static esp_err_t provider_repair_quiesce_storage", 1
    )[1].split("static esp_err_t verify_invalid_provider_copy", 1)[0]

    assert "d1l_storage_manager_quiesce_begin(" in quiesce_helper
    assert "d1l_route_store_worker_quiesce_begin(" in quiesce_helper
    assert quiesce_helper.index(
        "d1l_storage_manager_quiesce_begin("
    ) < quiesce_helper.index("d1l_route_store_worker_quiesce_begin(")
    assert "d1l_storage_manager_pause(" not in quiesce_helper
    assert "d1l_route_store_worker_quiesce_end();" in repair
    assert "d1l_storage_manager_quiesce_end();" in repair
    assert repair.index("d1l_route_store_worker_quiesce_end();") < (
        repair.index("d1l_storage_manager_quiesce_end();")
    )
    assert "d1l_storage_manager_resume();" not in repair
    first_recovery_quiesce = repair.index(
        "provider_repair_quiesce_storage("
    )
    first_path_preflight = repair.index(
        "ret = provider_path_exists("
    )
    first_canonical_read = repair.index(
        "ret = read_provider_path("
    )
    assert first_recovery_quiesce < first_path_preflight
    assert first_path_preflight < first_canonical_read
    assert "D1L_MAP_PROVIDER_RECOVERY_STAGE_001_PATH" in repair
    assert "out_result->stage_reused = true;" in repair
    assert "write_provider_create_new(" in repair
    assert "verify_invalid_provider_copy(" in repair
    assert "d1l_rp2040_bridge_file_delete(" in repair
    assert "d1l_rp2040_bridge_file_rename(" in repair
    assert (
        "D1L_MAP_PROVIDER_CONFIG_PATH, out_result->backup_path"
        not in repair
    )

    backup_copy = repair.index("write_provider_create_new(")
    backup_verify = repair.index("verify_invalid_provider_copy(")
    canonical_delete = repair.index("d1l_rp2040_bridge_file_delete(")
    final_rename = repair.index("d1l_rp2040_bridge_file_rename(")
    final_validate = repair.index(
        "validate_default_provider_path(", final_rename
    )
    assert backup_copy < backup_verify < canonical_delete < final_rename
    assert final_rename < final_validate
    invalid_flow = repair.split(
        'provider_repair_set_stage(out_result, "stage_verified");', 1
    )[1].split("} else {", 1)[0]
    assert invalid_flow.index("provider_repair_quiesce_storage(") < (
        invalid_flow.index("ret = read_provider_path(")
    )
    assert invalid_flow.index("ret = read_provider_path(") < (
        invalid_flow.index("write_provider_create_new(")
    )
    assert "canonical_reverify_io_result = reverify_ret;" in invalid_flow
    assert "canonical_reverify_bytes_match" in invalid_flow
    assert (
        "out_result->stage_path, D1L_MAP_PROVIDER_CONFIG_PATH, false"
        in repair
    )
    assert "canonical_missing_recoverable" in repair
    assert '"delete_performed\\":%s' in console
    assert '"backup_copy_create_new_only\\":true' in console
    assert '"retained_worker_quiesce_attempted\\":%s' in console
    assert '"retained_worker_quiesced\\":%s' in console
    assert '"canonical_reverify_io_code\\":' in console
    assert '"rollback_attempted\\":false' in console
