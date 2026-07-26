from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_production_families_share_the_executable_lifetime_boundary():
    lifetime = read("main/mesh/meshcore_lifetime.h")
    node_store = read("main/mesh/node_store.c")
    path_state = read("main/mesh/meshcore_path_state.c")
    packet_hash = read("main/mesh/meshcore_packet_hash.c")

    assert "D1L_MESHCORE_CONTACT_REACHABLE_MAX_AGE_MS" in lifetime
    assert "(uint32_t)(now_ms - observed_at_ms) <= max_age_ms" in lifetime
    assert "candidate_timestamp > retained_timestamp" in lifetime
    assert "current_index + 1U == capacity" in lifetime

    assert "s_live_last_heard_ms[D1L_NODE_STORE_CAPACITY]" in node_store
    assert "s_live_heard_valid[D1L_NODE_STORE_CAPACITY]" in node_store
    assert "d1l_meshcore_lifetime_age_current_u32(" in node_store
    assert "d1l_meshcore_lifetime_advert_is_strictly_newer(" in node_store
    assert "d1l_meshcore_lifetime_age_current_u32(" in path_state
    assert "d1l_meshcore_lifetime_packet_fifo_next(" in packet_hash


def test_identity_and_replay_truth_do_not_expire_with_ui_reachability():
    lifetime = read("main/mesh/meshcore_lifetime.h")
    contact_header = read("main/mesh/contact_store.h")
    node_store = read("main/mesh/node_store.c")

    assert "Canonical retained contacts do not expire" in lifetime
    assert "d1l_contact_store_delete" in contact_header
    assert "Retain the historical" in node_store
    assert "s_live_heard_valid[index] &&" in node_store
    assert "memset(s_live_heard_valid, 0" in node_store
    assert "d1l_meshcore_lifetime_advert_is_strictly_newer(" in node_store
    assert "advert_timestamp = 0" not in node_store


def test_node_reachability_never_reuses_persisted_uptime_after_reload():
    node_store = read("main/mesh/node_store.c")

    init = node_store.split("esp_err_t d1l_node_store_init", 1)[1].split(
        "esp_err_t d1l_node_store_clear", 1
    )[0]
    upsert = node_store.split("esp_err_t d1l_node_store_upsert_advert", 1)[1].split(
        "d1l_node_store_stats_t d1l_node_store_stats", 1
    )[0]
    view = node_store.split("static void build_node_view", 1)[1].split(
        "static bool node_view_matches_filter", 1
    )[0]

    assert init.index("clear_ram()") < init.index("nvs_get_blob")
    assert "s_live_last_heard_ms[index] = now_ms" in upsert
    assert "s_live_heard_valid[index] = true" in upsert
    assert upsert.index("s_live_last_heard_ms[index] = now_ms") < upsert.index(
        "note_persistence_dirty_locked(false, now_ms)"
    )
    assert "persist_node_snapshot(" not in upsert
    assert "nvs_" not in upsert
    assert "node->last_heard_ms" not in view.split("view->reachable", 1)[1]
    assert "s_live_last_heard_ms[index]" in view
