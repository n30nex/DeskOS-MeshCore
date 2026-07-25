from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_retained_blob_store_has_sd_history_stores_with_nvs_fallback():
    header = read("main/storage/retained_blob_store.h")
    source = read("main/storage/retained_blob_store.c")
    cmake = read("main/CMakeLists.txt")

    assert "D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES" in header
    assert "D1L_RETAINED_BLOB_STORE_DM_MESSAGES" in header
    assert "D1L_RETAINED_BLOB_STORE_ROUTES" in header
    assert "D1L_RETAINED_BLOB_STORE_PACKET_LOG" in header
    assert "d1l_retained_blob_store_backend_name" in header
    assert "d1l_retained_blob_store_init" in header
    assert "d1l_retained_blob_store_nvs_ready" in header
    assert "d1l_retained_blob_store_nvs_error" in header
    assert "d1l_retained_blob_store_nvs_marker_ready" in header
    assert "d1l_retained_blob_store_nvs_markers_complete" in header
    assert "d1l_retained_blob_store_nvs_anchor_ready" in header
    assert "d1l_retained_blob_store_nvs_sentinel_ready" in header
    assert "d1l_retained_blob_store_nvs_external_init_required" in header
    assert "d1l_retained_blob_store_nvs_initialized_this_boot" in header
    assert "d1l_retained_blob_store_nvs_migrated_keys" in header
    assert "d1l_retained_blob_store_nvs_migration_error" in header
    assert "d1l_retained_blob_store_nvs_store_telemetry_t" in header
    assert "d1l_retained_blob_store_nvs_telemetry_t" in header
    assert "d1l_retained_blob_store_nvs_telemetry" in header
    assert "write_attempt_count" in header
    assert "write_commit_count" in header
    assert "write_bytes_attempted" in header
    assert "write_bytes_committed" in header
    assert "erase_attempt_count" in header
    assert "capacity_valid" in header
    assert "available_entries" in header
    assert "d1l_retained_blob_store_is_available" in header
    assert "d1l_retained_blob_store_uses_sd" in header
    assert "d1l_retained_blob_store_backend_state_t" in header
    assert "d1l_retained_blob_store_backend_state" in header
    assert "D1L_RETAINED_BLOB_STORE_SD_DEGRADED_NOTE" in header
    assert "d1l_retained_blob_store_sd_stats_t" in header
    assert "sd_read_fail_count" in header
    assert "sd_write_fail_count" in header
    assert "sd_rename_fail_count" in header
    assert "sd_last_error" in header
    assert "sd_degraded_latched" in header
    assert "nvs_mirror_fail_count" in header
    assert "d1l_retained_blob_store_sd_stats" in header
    assert "d1l_retained_blob_store_any_sd_degraded" in header
    assert "d1l_retained_blob_store_note_sd_backend" in header
    assert "d1l_retained_blob_store_read_sd_primary" in header
    assert "d1l_retained_blob_store_read_nvs_fallback" in header
    assert "d1l_retained_blob_store_write_sd_primary" in header
    assert "d1l_retained_blob_store_write_sd_primary_guarded" in header
    assert "d1l_retained_blob_store_write_nvs_fallback" in header
    assert "d1l_retained_blob_store_erase_sd_primary" in header
    assert "d1l_retained_blob_store_erase_sd_primary_guarded" in header
    assert "d1l_retained_blob_store_erase_nvs_fallback" in header
    assert "d1l_retained_blob_store_read" in header
    assert "d1l_retained_blob_store_read_fallback" in header
    assert "d1l_retained_blob_store_write" in header
    assert "d1l_retained_blob_store_write_split" in header
    assert "d1l_retained_blob_store_erase" in header
    assert '"storage/retained_blob_store.c"' in cmake

    assert 'D1L_RETAINED_PUBLIC_MESSAGE_NAMESPACE "d1l_messages"' in source
    assert 'D1L_RETAINED_DM_MESSAGE_NAMESPACE "d1l_dms"' in source
    assert 'D1L_RETAINED_ROUTE_NAMESPACE "d1l_routes"' in source
    assert 'D1L_RETAINED_PACKET_LOG_NAMESPACE "d1l_packets"' in source
    assert 'D1L_RETAINED_PUBLIC_MESSAGE_SD_DIR "stores/messages/public"' in source
    assert 'D1L_RETAINED_DM_MESSAGE_SD_DIR "stores/messages/dm"' in source
    assert 'D1L_RETAINED_ROUTE_SD_DIR "stores/routes"' in source
    assert 'D1L_RETAINED_PACKET_LOG_SD_DIR "stores/packet_log"' in source
    assert '.name = "public_messages"' in source
    assert '.name = "dm_messages"' in source
    assert '.name = "routes"' in source
    assert '.name = "packet_log"' in source
    assert 'return s_retained_nvs_ready ? "nvs" : "unavailable";' in source
    assert "s_store_sd_enabled[D1L_RETAINED_BLOB_STORE_COUNT]" in source
    assert "s_store_backend_generation[D1L_RETAINED_BLOB_STORE_COUNT]" in source
    assert "s_store_state_mux" in source
    assert "s_store_sd_enabled[config->id]" in source
    assert "s_store_sd_stats[D1L_RETAINED_BLOB_STORE_COUNT]" in source
    assert "note_sd_failure(config, D1L_RETAINED_SD_OP_READ, sd_ret)" in source
    assert "note_sd_failure(config, D1L_RETAINED_SD_OP_WRITE, failure)" in source
    assert "note_sd_failure(config, D1L_RETAINED_SD_OP_RENAME, ret)" in source
    assert "sd_error_latches_degraded" in source
    assert "stats->sd_degraded_latched = true" in source
    assert "note_nvs_mirror_failure(config" in source
    assert "d1l_retained_blob_store_note_sd_backend" in source
    assert 'D1L_RETAINED_NVS_PARTITION "d1l_retained"' in source
    assert 'D1L_RETAINED_NVS_META_PARTITION "d1l_ret_meta"' in source
    assert 'D1L_RETAINED_NVS_ANCHOR_KEY "anchor"' in source
    assert "D1L_RETAINED_NVS_META_LEGACY_VERSION 1U" in source
    assert "D1L_RETAINED_NVS_META_VERSION 2U" in source
    assert "retained_nvs_partition_erased" in source
    assert "esp_partition_erase_range(nvs_partition" not in source
    assert "sentinel_present" in source
    assert "ensure_default_retained_nvs_sentinel" in source
    assert "ensure_retained_nvs_anchor" in source
    assert "blank_resume_allowed" in source
    assert "require_retained_nvs_anchor" in source
    assert "NVS initialization is not a read-only probe" in source
    assert "retained_nvs_marker_valid" in source
    assert "retained_nvs_legacy_marker_valid" in source
    assert "finalize_retained_nvs_markers" in source
    assert "nvs_flash_init_partition(" in source
    assert "nvs_open_from_partition(" in source
    assert "migrate_known_legacy_nvs_keys();" in source
    assert "note_nvs_write_attempt(config, len)" in source
    assert "note_nvs_write_result(config, len, ret)" in source
    assert "note_nvs_erase_attempt(config)" in source
    assert "note_nvs_erase_result(config, ret)" in source
    assert "nvs_get_stats(D1L_RETAINED_NVS_PARTITION, &stats)" in source
    assert "UINT64_MAX - *value < increment" in source
    assert "physical flash program/erase cycles" in header
    assert '\\"retained_nvs\\":{\\"partition\\":\\"d1l_retained\\"' in read(
        "main/comms/usb_console.c"
    )
    console = read("main/comms/usb_console.c")
    assert '\\"telemetry\\":' in console
    assert '\\"scope\\":\\"boot_runtime_api_calls\\"' in console
    assert '\\"physical_flash_cycles_measured\\":false' in console
    assert '\\"write_amplification\\"' in console
    assert '\\"capacity\\"' in console
    assert "bool d1l_retained_blob_store_backend_state(" in source
    assert "profile_allows_sd && s_store_sd_enabled[config->id]" in source
    assert "out_state->generation = s_store_backend_generation[config->id]" in source
    assert "s_store_sd_enabled[i] != can_use_retained_sd" in source
    assert "s_store_backend_generation[i]++" in source
    assert "portENTER_CRITICAL(&s_store_state_mux)" in source
    assert "portEXIT_CRITICAL(&s_store_state_mux)" in source
    assert "esp_err_t d1l_retained_blob_store_read_sd_primary(" in source
    assert "esp_err_t d1l_retained_blob_store_read_nvs_fallback(" in source
    assert "esp_err_t d1l_retained_blob_store_write_sd_primary(" in source
    assert "esp_err_t d1l_retained_blob_store_write_sd_primary_guarded(" in source
    assert "esp_err_t d1l_retained_blob_store_write_nvs_fallback(" in source
    assert "esp_err_t d1l_retained_blob_store_erase_sd_primary(" in source
    assert "esp_err_t d1l_retained_blob_store_erase_sd_primary_guarded(" in source
    assert "esp_err_t d1l_retained_blob_store_erase_nvs_fallback(" in source
    assert "data_ready &&" in source
    assert "file_ops_supported &&" in source
    assert "atomic_rename_supported &&" in source
    assert "file_line_max >= D1L_RP2040_FILE_LINE_MAX" in source
    assert "file_chunk_max >= D1L_RP2040_FILE_CHUNK_MAX" in source
    assert "path_max >= D1L_RP2040_FILE_PATH_MAX" in source
    assert "for (size_t i = 0; i < D1L_RETAINED_BLOB_STORE_COUNT; ++i)" in source
    assert "s_store_sd_enabled[i] = can_use_retained_sd" in source
    assert "build_sd_path(config, key, \".bin\"" in source
    assert "build_sd_path(config, key, \".tmp\"" in source
    assert "d1l_rp2040_bridge_file_stat" in source
    assert "d1l_rp2040_bridge_file_read" in source
    assert "d1l_rp2040_bridge_file_write" in source
    assert "d1l_rp2040_bridge_file_rename(temp_path, path, true" in source
    guarded_write = source.split("static esp_err_t sd_write_blob_for_generation", 1)[1].split(
        "static esp_err_t sd_write_blob(", 1
    )[0]
    read_blob = source.split("static esp_err_t sd_read_blob", 1)[1].split(
        "static esp_err_t sd_write_blob_for_generation", 1
    )[0]
    pre_rename = guarded_write.split("The replace-rename is the destructive commit point", 1)[1].split(
        "d1l_rp2040_bridge_file_rename", 1
    )[0]
    assert "store_backend_generation_matches(config, expected_generation)" in pre_rename
    assert "d1l_rp2040_bridge_file_delete" not in pre_rename
    assert "d1l_rp2040_bridge_file_delete" in source
    assert "D1L_RP2040_FILE_CHUNK_MAX" in source
    assert '#include "mesh/route_store_worker.h"' in source
    assert read_blob.count("d1l_route_store_persistence_should_yield()") == 2
    assert guarded_write.count("d1l_route_store_persistence_should_yield()") == 2
    assert read_blob.count("return ESP_ERR_NOT_FINISHED;") == 2
    assert guarded_write.count("return ESP_ERR_NOT_FINISHED;") == 2
    assert "ret != ESP_ERR_NOT_FINISHED" in source
    assert "nvs_read_blob" in source
    assert "nvs_write_blob" in source
    assert "nvs_erase_blob" in source
    assert "nvs_get_blob(handle, key, dst, len_inout)" in source
    assert "nvs_set_blob(handle, key, src, len)" in source
    assert "nvs_erase_key(handle, key)" in source
    assert "nvs_commit(handle)" in source
    assert "ESP_ERR_NVS_NOT_FOUND" in source
    assert "note_nvs_mirror_failure(config, nvs_write_blob(config, key, src, len))" in source
    assert "d1l_retained_blob_store_write_split" in source
    assert "const bool has_primary = primary_src && primary_len > 0" in source
    assert "const bool has_fallback = fallback_src && fallback_len > 0" in source
    assert "sd_write_blob(config, key, primary_src, primary_len)" in source
    assert "nvs_write_blob(config, key, nvs_src, nvs_len)" in source
    assert "note_nvs_mirror_failure(config, nvs_ret)" in source
    assert "return ESP_OK;" in source
    assert "return nvs_ret == ESP_OK ? ESP_OK : sd_ret;" in source
    assert "return sd_ret == ESP_ERR_NOT_FOUND ? nvs_ret : sd_ret;" in source


def test_history_backends_are_reported_from_blob_store_and_can_switch_to_sd():
    storage_status = read("main/storage/storage_status.c")
    packet_log = read("main/mesh/packet_log.c")

    assert '#include "storage/retained_blob_store.h"' in storage_status
    assert "d1l_retained_blob_store_note_sd_backend(sd->data_ready" in storage_status
    assert "D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES" in storage_status
    assert "D1L_RETAINED_BLOB_STORE_DM_MESSAGES" in storage_status
    assert "D1L_RETAINED_BLOB_STORE_ROUTES" in storage_status
    assert "sd->file_ops_supported" in storage_status
    assert "sd->atomic_rename_supported" in storage_status
    assert "d1l_retained_blob_store_backend_name(D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES)" in storage_status
    assert "d1l_retained_blob_store_backend_name(D1L_RETAINED_BLOB_STORE_DM_MESSAGES)" in storage_status
    assert "d1l_retained_blob_store_backend_name(D1L_RETAINED_BLOB_STORE_ROUTES)" in storage_status
    assert "d1l_retained_blob_store_backend_name(D1L_RETAINED_BLOB_STORE_PACKET_LOG)" in storage_status
    assert 'status->data_backend = any_retained_sd ? "mixed" :' in storage_status
    assert '"retained_history_sd_enabled"' in storage_status
    assert 'status->message_store_backend = "nvs"' not in storage_status
    assert 'status->dm_store_backend = "nvs"' not in storage_status
    assert 'status->route_store_backend = "nvs"' not in storage_status
    assert 'status->packet_log_backend = "nvs"' not in storage_status
    assert "d1l_retained_blob_store_backend_state(" in packet_log
    assert "d1l_retained_blob_store_read_sd_primary(" in packet_log
    assert "d1l_retained_blob_store_read_nvs_fallback(" in packet_log
    assert "d1l_retained_blob_store_write_sd_primary_guarded(" in packet_log
    assert "d1l_retained_blob_store_write_nvs_fallback(" in packet_log
    assert "d1l_retained_blob_store_erase_sd_primary_guarded(" in packet_log
    assert "d1l_retained_blob_store_erase_nvs_fallback(" in packet_log
    assert "d1l_retained_blob_store_read(D1L_RETAINED_BLOB_STORE_PACKET_LOG" not in packet_log
    assert "d1l_retained_blob_store_read_fallback(D1L_RETAINED_BLOB_STORE_PACKET_LOG" not in packet_log
    assert "d1l_retained_blob_store_write_split(D1L_RETAINED_BLOB_STORE_PACKET_LOG" not in packet_log
    assert "d1l_retained_blob_store_erase(D1L_RETAINED_BLOB_STORE_PACKET_LOG" not in packet_log


def test_release_docs_require_external_provenance_for_ambiguous_retained_bytes():
    developer = read("docs/DEVELOPER_GUIDE_D1L.md")
    checklist = read("docs/RELEASE_CHECKLIST.md")
    roadmap = read("docs/ROADMAP.md")
    test_plan = read("docs/TEST_PLAN_D1L.md")
    limitations = read("docs/KNOWN_LIMITATIONS.md")
    combined = "\n".join([developer, checklist, roadmap, test_plan, limitations])

    assert "external_init_required=true" in combined
    assert "markers_complete=true" in combined
    assert "prepare_retained_nvs_upgrade_d1l.py" in developer
    assert "prepare_retained_nvs_upgrade_d1l.py" in checklist
    assert "prepare_retained_nvs_upgrade_d1l.py" in test_plan
    assert "firmware neither probes nor erases ambiguous nonblank bytes" in checklist
    assert "firmware must not delete it automatically" in test_plan
    assert "direct scoped first-upgrade erase" not in combined
    assert "triple absence directly" not in combined
    assert "storage status.retained_nvs.telemetry" in combined
    assert 'scope="boot_runtime_api_calls"' in combined
    assert "physical_flash_cycles_measured=false" in combined
    assert "API-level" in combined
