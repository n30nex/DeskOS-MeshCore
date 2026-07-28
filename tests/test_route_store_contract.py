from pathlib import Path

from scripts.smoke_d1l import SMOKE_COMMANDS


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_route_store_is_bounded_and_retained_blob_store_backed():
    header = read("main/mesh/route_store.h")
    source = read("main/mesh/route_store.c")
    blob_store = read("main/storage/retained_blob_store.c")
    cmake = read("main/CMakeLists.txt")
    app_main = read("main/app_main.c")
    assert "D1L_ROUTE_STORE_CAPACITY 16U" in header
    assert "D1L_ROUTE_STORE_NVS_FALLBACK_CAPACITY 4U" in header
    assert "D1L_ROUTE_STORE_PERSIST_MIN_INTERVAL_MS 5000U" in header
    assert "D1L_ROUTE_STORE_PERSIST_MAX_INTERVAL_MS 30000U" in header
    assert "D1L_ROUTE_TARGET_LEN 17U" in header
    assert 'D1L_RETAINED_ROUTE_NAMESPACE "d1l_routes"' in blob_store
    assert 'D1L_RETAINED_ROUTE_SD_DIR "stores/routes"' in blob_store
    assert "D1L_ROUTE_STORE_ID D1L_RETAINED_BLOB_STORE_ROUTES" in source
    assert 'D1L_ROUTE_STORE_LEGACY_KEY "routes"' in source
    assert 'D1L_ROUTE_STORE_V2_KEY "routes_v2"' in source
    assert "D1L_ROUTE_STORE_LEGACY_SCHEMA 1U" in source
    assert "D1L_ROUTE_STORE_FULL_SCHEMA 2U" in source
    assert "D1L_ROUTE_STORE_FALLBACK_SCHEMA 3U" in source
    assert '#include "storage/retained_blob_store.h"' in source
    assert "d1l_retained_blob_store_read_sd_primary(" in source
    assert "d1l_retained_blob_store_read_nvs_fallback(" in source
    assert "d1l_retained_blob_store_write_sd_primary_guarded(" in source
    assert "d1l_retained_blob_store_write_nvs_fallback(" in source
    assert "d1l_retained_blob_store_erase_sd_primary_guarded(" in source
    assert "d1l_retained_blob_store_erase_nvs_fallback(" in source
    assert "d1l_retained_blob_store_backend_state(D1L_ROUTE_STORE_ID" in source
    assert "s_last_sd_backend_generation" in source
    assert "sd_backend_generation_matches" in source
    assert "nvs_get_blob" not in source
    assert "nvs_set_blob" not in source
    assert "static d1l_route_store_blob_t s_blob_scratch" in source
    assert "route_confidence" in source
    assert "now_ms < entry->first_seen_ms" in source
    assert "oldest_index" in source
    assert "d1l_route_store_find_by_seq" in header
    assert "d1l_route_store_flush" in header
    assert "d1l_route_store_flush_if_due" in header
    assert "persistence_commit_count" in header
    assert "persistence_coalesced_count" in header
    assert "persistence_fail_count" in header
    assert "persistence_dirty" in header
    assert "persistence_revision" in header
    assert "sd_primary_dirty" in header
    assert "sd_backend_generation" in header
    assert "sd_primary_reconcile_pending" in header
    assert "nvs_fallback_dirty" in header
    assert "clear_failure_latched" in header
    assert "s_persist_snapshot.revision = s_revision" in source
    assert "const bool same_revision = s_revision == s_persist_snapshot.revision" in source
    upsert = source.split("static esp_err_t upsert_observation_internal", 1)[1].split(
        "esp_err_t d1l_route_store_upsert_observation(", 1
    )[0]
    assert "d1l_route_store_flush" not in upsert
    assert "d1l_route_store_init" not in upsert
    assert "ensure_route_store_initialized" not in upsert
    assert "d1l_retained_blob_store_" not in upsert
    assert "reconcile_sd_primary" in source
    assert "adopt_primary_with_live_overlay_locked" in source
    assert "selected_nvs_with_readable_sd" in source
    assert "s_retry_pending && retry_ready" in source
    assert "now_ms - s_material_dirty_since_ms" in source
    assert "now_ms - s_dirty_since_ms" in source
    assert "static d1l_route_entry_t s_volatile_entry" in source
    assert "static bool s_volatile_valid" in source
    volatile_path = upsert.split("if (!persist)", 1)[1].split(
        "A UI-only canary borrows next_seq", 1
    )[0]
    assert "s_volatile_entry.seq = s_next_seq" in volatile_path
    assert "s_next_seq++" not in volatile_path
    assert "s_total_written++" not in volatile_path
    assert "s_dropped_oldest++" not in volatile_path
    assert "s_revision++" not in volatile_path
    assert "if (s_clear_failure_latched)" in source
    assert "if (force && s_clear_failure_latched)" not in source
    assert "independent durability target" in source
    assert "write_nvs_fallback_with_legacy_reclaim" in source
    assert "ret != ESP_ERR_NVS_NOT_ENOUGH_SPACE" in source
    assert "D1L_ROUTE_STORE_LEGACY_KEY" in source
    assert "s_nvs_legacy_reclaim_allowed" in source
    assert "persisted_route_entry_is_valid" in source
    assert "persisted_route_entries_are_valid" in source
    assert "memchr(text, '\\0', capacity)" in source
    assert "entries[i].seq == entries[j].seq" in source
    assert "entry->seq >= next_seq" in source
    assert "s_reconcile_overlay[position - 1U].seq > ordered.seq" in source
    assert "d1l_route_store_copy_for_target" in header
    assert "strncmp(s_entries[i].target, target" in source
    assert "ESP_ERR_NOT_FOUND" in source
    assert '\\"route_store_backend\\"' in read("main/comms/usb_console.c")
    assert '"mesh/route_store.c"' in cmake
    assert "d1l_route_store_init()" in app_main


def test_meshcore_public_and_advert_paths_feed_routes():
    source = read("main/mesh/meshcore_service.c")
    assert '#include "mesh/route_store.h"' in source
    assert "route_name(packet.route)" in source
    assert "channel_id == D1L_CHANNEL_PUBLIC_ID" in source
    assert '"public_text" : "channel_text"' in source
    assert 'snprintf(dest, 17U, "public")' in source
    assert "route_target, channel.name, packet_kind" in source
    assert "advert.name[0] ? advert.name : pub_prefix" in source
    assert '"advert"' in source
    assert "d1l_route_store_upsert_observation_deferred(" in source
    assert "s_pending_channel_route_history_admitted" in source
    assert 'route store advert tx failed' in source
    assert "D1L_MESHCORE_ROUTE_TRANSPORT_DIRECT" in source
    assert "d1l_meshcore_service_request_path_discovery_probe" in read("main/mesh/meshcore_service.h")
    assert "D1L_MESHCORE_PATH_PROBE_COOLDOWN_MS 30000U" in source
    assert '"path_%08lX"' in source
    assert '"path_probe"' in source
    assert "route store path probe tx failed" in source
    assert "xQueueReceive(s_service_queue, &cmd, 0)" in source
    assert "xQueueReceive(s_radio_event_queue, &cmd, 0)" in source
    assert "pdMS_TO_TICKS(D1L_MESHCORE_OWNER_POLL_MS)" in source
    assert "ulTaskNotifyTake(pdTRUE, portMAX_DELAY)" not in source
    assert "d1l_route_store_flush_if_due()" not in source


def test_ui_console_and_smoke_expose_routes():
    app_header = read("main/app/app_model.h")
    app_source = read("main/app/app_model.c")
    ui = read("main/ui/ui_phase1.c")
    console = read("main/comms/usb_console.c")
    roadmap = read("docs/ROADMAP.md")
    assert "recent_routes" in app_header
    assert "route_total_written" in app_header
    assert "d1l_route_store_copy_recent" in app_source
    assert "d1l_app_model_copy_route_trace" in app_header
    assert "d1l_route_store_copy_for_target(fingerprint, out_entries, max_entries)" in app_source
    assert "route_count" in ui
    assert 'ok_begin("routes")' in console
    assert '\\\"persistence\\\":{\\\"revision\\\"' in console
    assert '\\\"sd_primary\\\":{\\\"required\\\"' in console
    assert '\\\"nvs_fallback\\\":{\\\"dirty\\\"' in console
    assert "backend_generation" in console
    assert "reconcile_pending" in console
    assert "bool_json(!stats.persistence_dirty)" in console
    assert "d1l_route_store_worker_force_flush(" in console
    assert '"retained storage flush failed; reboot cancelled"' in console
    assert 'ok_begin("routes detail")' in console
    assert 'ok_begin("routes trace")' in console
    assert "d1l_route_store_find_by_seq" in console
    assert "d1l_app_model_copy_route_trace(fingerprint, entries, D1L_ROUTE_STORE_CAPACITY)" in console
    assert "d1l_app_model_request_path_discovery_probe(" in console
    assert 'strcmp(line, "routes")' in console
    assert 'strncmp(line, "routes detail ", 14)' in console
    assert 'strncmp(line, "routes trace ", 13)' in console
    assert 'strncmp(line, "routes probe ", 13)' in console
    assert 'strcmp(line, "routes clear")' in console
    assert "routes detail <seq>" in console
    assert "routes trace <fingerprint>" in console
    assert "routes probe <fingerprint>" in console
    assert "Routes are learned from MeshCore path metadata" in console
    assert "Encrypted PATH discovery and base-telemetry request queued" in console
    assert '\\"telemetry_requested\\":true' in console
    assert "routes" in SMOKE_COMMANDS
    assert "routes trace 0BF0A701D5AE2DB6" in SMOKE_COMMANDS
    assert "routes clear" in roadmap


def test_usb_console_keeps_large_preview_buffers_off_main_stack():
    console = read("main/comms/usb_console.c")
    assert "static d1l_packet_log_entry_t entries[8]" in console
    assert "static d1l_message_entry_t entries[D1L_MESSAGE_STORE_CAPACITY]" in console
    assert "static d1l_node_view_t entries[8]" in console
    assert "static d1l_contact_entry_t entries[8]" in console
    assert "static d1l_route_entry_t entries[8]" in console
    assert "static d1l_route_entry_t entries[D1L_ROUTE_STORE_CAPACITY]" in console
    assert "static char line[256]" in console
