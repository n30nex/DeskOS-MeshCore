from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def volatile_branch(source: str, anchor: str) -> str:
    body = source.split(anchor, 1)[1]
    branch = body.split("if (!persist)", 1)[1].split("return", 1)[0]
    return branch


def test_volatile_canaries_have_preview_slots_outside_durable_rings():
    cases = [
        (
            "main/mesh/message_store.c",
            "static esp_err_t append_channel_internal",
            "d1l_message_entry_t",
            "s_entries[s_head] = entry;",
            [
                "d1l_message_store_copy_channel_recent",
                "d1l_message_store_query_channel_page",
            ],
        ),
        (
            "main/mesh/dm_store.c",
            "static esp_err_t append_internal",
            "d1l_dm_entry_t",
            "s_entries[s_head] = entry;",
            ["d1l_dm_store_copy_recent_page", "d1l_dm_store_query_thread_page"],
        ),
        (
            "main/mesh/packet_log.c",
            "static esp_err_t append_raw_internal",
            "d1l_packet_log_entry_t",
            "s_entries[s_head] = copy;",
            [
                "d1l_packet_log_copy_recent",
                "query_ram_locked",
                "d1l_packet_log_find_by_seq",
            ],
        ),
    ]

    for path, append_anchor, entry_type, durable_write, readers in cases:
        source = read(path)
        assert f"static {entry_type} s_volatile_entry;" in source
        assert "static bool s_volatile_valid;" in source
        branch = volatile_branch(source, append_anchor)
        assert "s_volatile_entry =" in branch
        assert "s_volatile_valid = true;" in branch
        assert "s_entries[s_head]" not in branch
        assert "s_next_seq++" not in branch
        assert "s_total_written++" not in branch
        assert "s_dropped_oldest++" not in branch
        assert "persist_store" not in branch
        assert source.index("s_volatile_valid = false;", source.index(append_anchor)) < source.index(
            durable_write, source.index(append_anchor)
        )
        for reader in readers:
            reader_body = source.split(reader, 1)[1]
            assert "s_volatile" in reader_body


def test_persistence_builders_do_not_use_content_sentinels_for_volatile_rows():
    for path in (
        "main/mesh/message_store.c",
        "main/mesh/dm_store.c",
        "main/mesh/packet_log.c",
        "main/mesh/route_store.c",
    ):
        source = read(path)
        assert "is_volatile_ui_canary" not in source


def test_public_and_dm_console_snapshots_bind_rows_stats_and_preview_under_one_lock():
    cases = [
        (
            "main/mesh/message_store.c",
            "size_t d1l_message_store_query_page_snapshot(",
            "size_t d1l_message_store_query(",
            "message_store_stats_locked(backend_state)",
        ),
        (
            "main/mesh/dm_store.c",
            "size_t d1l_dm_store_copy_recent_page_snapshot(",
            "size_t d1l_dm_store_query_thread_page(",
            "dm_store_stats_locked(backend_state)",
        ),
        (
            "main/mesh/dm_store.c",
            "size_t d1l_dm_store_copy_thread_page_snapshot(",
            "size_t d1l_dm_store_copy_thread(",
            "dm_store_stats_locked(backend_state)",
        ),
    ]

    for path, start, end, stats_call in cases:
        source = read(path)
        body = source.split(start, 1)[1].split(end, 1)[0]
        assert body.count("d1l_store_lock_take(&s_store_lock);") == 1
        assert body.count("d1l_store_lock_give(&s_store_lock);") == 1
        assert "s_volatile_entry.seq" in body
        assert stats_call in body
        assert body.index(stats_call) < body.index(
            "d1l_store_lock_give(&s_store_lock);"
        )


def test_release_only_append_if_space_apis_are_absent():
    for path in (
        "main/mesh/message_store.c",
        "main/mesh/message_store.h",
        "main/mesh/dm_store.c",
        "main/mesh/dm_store.h",
        "tests/native/retained_store_volatile_test.c",
    ):
        assert "append_if_space" not in read(path)
