from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_packet_feed_state_has_one_owned_bounded_controller():
    cmake = read("main/CMakeLists.txt")
    phase1 = read("main/ui/ui_phase1.c")
    header = read("main/ui/ui_packets.h")
    source = read("main/ui/ui_packets.c")

    assert '"ui/ui_packets.c"' in cmake
    assert '#include "ui_packets.h"' in phase1
    assert "d1l_ui_packets_controller_t" in header
    assert "d1l_packet_log_entry_t rows[D1L_PACKET_LOG_CAPACITY]" in header
    assert "char search_text[D1L_PACKET_LOG_QUERY_TEXT_LEN]" in header
    assert "static d1l_ui_packets_controller_t s_packets_controller EXT_RAM_BSS_ATTR;" in phase1
    assert (
        "static d1l_ui_packet_query_result_t s_packet_query_result "
        "EXT_RAM_BSS_ATTR;"
    ) in phase1
    assert "d1l_ui_packets_init(&s_packets_controller);" in phase1

    for function in (
        "d1l_ui_packets_select_filter",
        "d1l_ui_packets_toggle_pause",
        "d1l_ui_packets_set_search",
        "d1l_ui_packets_clear_search",
        "d1l_ui_packets_load_older",
        "d1l_ui_packets_load_newer",
        "d1l_ui_packets_query_request",
        "d1l_ui_packets_accept_query",
        "d1l_ui_packets_can_load_older",
        "d1l_ui_packets_can_load_newer",
        "d1l_ui_packets_entry_color",
        "d1l_ui_packets_entry_status",
    ):
        assert function in header
        assert function in source
        assert function in phase1 or function.startswith("d1l_ui_packets_entry_")

    for old_owner in (
        "s_packet_filtered_packets",
        "s_packet_filtered_count",
        "s_packet_filter_mode",
        "s_packet_row_limit",
        "s_packet_skip_newest",
        "s_packet_total_matches",
        "s_packet_sd_history_page",
        "s_packet_search_text[",
        "s_packets_paused",
    ):
        assert old_owner not in phase1


def test_packet_controller_has_no_lvgl_or_ui_task_side_effects():
    source = read("main/ui/ui_packets.c")

    assert "lv_" not in source
    assert "freertos/" not in source
    assert "d1l_app_model_" not in source
    assert "d1l_packet_log_query_page(" not in source
    assert "storage/" not in source
    assert "rp2040" not in source.lower()
    assert "request_content_refresh" not in source
    assert "show_modal" not in source
    assert "static d1l_packet_log_entry_t" not in source
    assert "controller->rows" in source
    assert "memcpy(controller->rows, rows" in source
    assert "controller->search_text" in source
    assert "controller->skip_newest" in source
