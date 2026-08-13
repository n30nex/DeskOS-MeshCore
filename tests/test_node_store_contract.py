from pathlib import Path

from scripts.smoke_d1l import SMOKE_COMMANDS


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_heard_node_store_is_bounded_sd_primary_and_degrades_without_nvs_history():
    header = read("main/mesh/node_store.h")
    source = read("main/mesh/node_store.c")
    cmake = read("main/CMakeLists.txt")
    app_main = read("main/app_main.c")
    assert "D1L_NODE_RAM_ACTIVE_CAPACITY 512U" in header
    assert "D1L_NODE_NVS_FALLBACK_CAPACITY 16U" in header
    assert "D1L_NODE_SD_HISTORY_CAPACITY 512U" in header
    assert "D1L_NODE_STORE_CAPACITY D1L_NODE_SD_HISTORY_CAPACITY" in header
    assert "D1L_NODE_PUBLIC_KEY_HEX_LEN 65U" in header
    assert "D1L_HEARD_NODE_NAME_LEN 24U" in header
    assert "D1L_NODE_TYPE_LEN 9U" in header
    assert "D1L_NODE_STORE_SCHEMA_V4 4U" in source
    assert "D1L_NODE_STORE_SD_SCHEMA 5U" in source
    assert "D1L_NODE_STORE_SCHEMA_V3 3U" in source
    assert "D1L_NODE_STORE_SCHEMA_V2 2U" in source
    assert "D1L_NODE_STORE_LEGACY_CAPACITY D1L_NODE_NVS_FALLBACK_CAPACITY" in source
    assert "d1l_node_store_blob_v1_t" in source
    assert "d1l_node_store_blob_v2_t" in source
    assert "d1l_node_store_blob_v3_t" in source
    assert "d1l_node_store_blob_v4_t" in source
    assert "d1l_node_store_sd_blob_t" in source
    assert "migrate_v1_blob" in source
    assert "migrate_v2_blob" in source
    assert "migrate_v3_blob" in source
    assert "node schema v1 layout changed" in source
    assert "node schema v2/v3 layout changed" in source
    assert "entries[D1L_NODE_NVS_FALLBACK_CAPACITY]" in source
    assert "entries[D1L_NODE_SD_HISTORY_CAPACITY]" in source
    assert 'D1L_NODE_STORE_NAMESPACE "d1l_nodes"' in source
    assert 'D1L_NODE_STORE_KEY "heard"' in source
    assert 'D1L_NODE_STORE_EPOCH_KEY "marker_epoch"' in source
    assert 'D1L_NODE_STORE_SD_KEY "nodes_v1"' in source
    assert "nvs_get_blob" in source
    assert "nvs_set_blob" not in source
    assert "nvs_set_u32(handle, D1L_NODE_STORE_EPOCH_KEY, epoch)" in source
    assert (
        "static d1l_node_store_sd_blob_t s_sd_blob_scratch "
        "EXT_RAM_BSS_ATTR"
    ) in source
    assert "d1l_retained_blob_store_read_sd_primary(" in source
    assert "d1l_retained_blob_store_write_sd_primary_guarded(" in source
    assert "d1l_node_store_flush_if_due" in source
    assert "note_persistence_dirty_locked(false, now_ms)" in source
    upsert = source.split("esp_err_t d1l_node_store_upsert_advert", 1)[1].split(
        "d1l_node_store_stats_t d1l_node_store_stats", 1
    )[0]
    assert "nvs_" not in upsert
    assert "d1l_retained_blob_store_" not in upsert
    assert "find_by_fingerprint" in source
    assert "oldest_unlocated_index" in source
    assert "least_recent_unlocated_advert_index" in source
    assert "if (s_entries[i].location_valid)" in source
    assert "return ESP_ERR_NO_MEM;" in upsert
    assert '"mesh/node_store.c"' in cmake
    assert "d1l_node_store_init()" in app_main


def test_heard_node_query_supports_production_sort_filter_rows():
    header = read("main/mesh/node_store.h")
    source = read("main/mesh/node_store.c")

    assert "D1L_NODE_FILTER_COMPANION" in header
    assert "D1L_NODE_FILTER_REPEATER" in header
    assert "D1L_NODE_FILTER_ROOM" in header
    assert "D1L_NODE_FILTER_SENSOR" in header
    assert "D1L_NODE_FILTER_FAVORITE" in header
    assert "D1L_NODE_SORT_LAST_HEARD" in header
    assert "D1L_NODE_SORT_SIGNAL" in header
    assert "D1L_NODE_SORT_NAME" in header
    assert "D1L_NODE_SORT_ROLE" in header
    assert "D1L_NODE_SORT_FAVORITE" in header
    assert "d1l_node_query_t" in header
    assert "d1l_node_view_t" in header
    assert "display_name[D1L_HEARD_NODE_NAME_LEN]" in header
    assert "role[D1L_NODE_ROLE_LEN]" in header
    assert "bool favorite" in header
    assert "bool keyed" in header
    assert "bool reachable" in header
    assert "d1l_node_store_query" in header
    assert '#include "mesh/contact_store.h"' in source
    assert "static d1l_node_view_t s_query_scratch[D1L_NODE_STORE_CAPACITY]" in source
    assert "node_role_name" in source
    assert 'return "sensor";' in source
    assert "node_view_matches_filter" in source
    assert "node_view_better" in source
    assert "contains_casefold" in source
    assert "d1l_contact_store_find_by_fingerprint" in source


def test_verified_adverts_upsert_heard_nodes():
    source = read("main/mesh/meshcore_service.c")
    admission = read("main/mesh/meshcore_advert_admission.c")
    cmake = read("main/CMakeLists.txt")
    assert '#include "mesh/meshcore_advert_admission.h"' in source
    assert "verify_advert_signature" in source
    assert "D1L_NODE_PUBLIC_KEY_HEX_LEN" in source
    assert "hex_prefix(pub_key_hex" in source
    assert "d1l_meshcore_advert_admit_verified(" in source
    assert "d1l_node_store_upsert_advert(" in admission
    assert '"mesh/meshcore_advert_admission.c"' in cmake
    assert "read_le32(timestamp)" in source
    assert "node store upsert failed" in source
    assert "d1l_node_store_find_by_fingerprint" in read("main/mesh/node_store.h")


def test_ui_console_and_smoke_expose_heard_nodes():
    app_header = read("main/app/app_model.h")
    app_source = read("main/app/app_model.c")
    ui = read("main/ui/ui_phase1.c")
    nodes_ui = read("main/ui/ui_nodes.c")
    node_detail_ui = read("main/ui/ui_node_detail.c")
    console = read("main/comms/usb_console.c")
    assert "recent_nodes" in app_header
    assert "d1l_node_view_t recent_nodes" in app_header
    assert "d1l_app_model_query_nodes" in app_header
    assert "node_total_written" in app_header
    assert "d1l_app_model_query_nodes(&node_query" in app_source
    assert "d1l_node_store_query(query, out_entries, max_entries)" in app_source
    assert "nodes_render_node_row" in nodes_ui
    assert "render_role_badge" in node_detail_ui
    assert "D1L_UI_NODES_ACTION_OPEN_NODE" in nodes_ui
    assert '"Contact"' in node_detail_ui
    assert "role_display_label(view->role)" in node_detail_ui
    assert 'ok_begin("nodes")' in console
    assert 'strcmp(line, "nodes")' in console
    assert 'strcmp(line, "nodes clear")' in console
    assert "d1l_node_store_query(&query, entries, 8)" in console
    assert '\\"public_key\\"' in console
    assert '\\"display_name\\"' in console
    assert '\\"role\\"' in console
    assert '\\"favorite\\"' in console
    assert '\\"keyed\\"' in console
    assert '\\"reachable\\"' in console
    node_row = nodes_ui.split(
        "static void nodes_render_node_row", 1
    )[1].split("static void nodes_render_empty_state", 1)[0]
    assert 'nodes_create_button(row, "Chat"' in node_row
    message_branch = node_row.split("if (can_dm)", 1)[1].split("else if", 1)[0]
    assert "view->keyed" not in message_branch
    assert "const bool can_manage = view->keyed" in node_row
    assert 'nodes_create_button(row, "Login"' in node_row
    assert "entry->fingerprint" not in node_row
    assert 'view->keyed ? "saved" : "missing"' in node_detail_ui
    assert "Verified MeshCore adverts populate this bounded heard-node store" in console
    assert "nodes" in SMOKE_COMMANDS
