from pathlib import Path

from tools import ui_simulator


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def function_body(source: str, signature: str, next_signature: str) -> str:
    return source.rsplit(signature, 1)[1].split(next_signature, 1)[0]


def test_cursor_advances_are_exact_idempotent_and_visible_to_refresh_generation() -> None:
    read_state = read("main/mesh/read_state.c")
    app_header = read("main/app/app_model.h")
    app_model = read("main/app/app_model.c")
    phase1 = read("main/ui/ui_phase1.c")

    public_mark = function_body(
        read_state,
        "esp_err_t d1l_read_state_mark_public_read(void)",
        "esp_err_t d1l_read_state_mark_dm_read(void)",
    )
    assert "stats.newest_public_rx_seq <= stats.last_public_read_seq" in public_mark
    assert public_mark.index("stats.newest_public_rx_seq <=") < public_mark.index(
        "s_state.last_public_read_seq = stats.newest_public_rx_seq"
    )

    dm_mark = function_body(
        read_state,
        "esp_err_t d1l_read_state_mark_dm_thread_read(const char *fingerprint)",
        "esp_err_t d1l_read_state_mark_all_read(void)",
    )
    assert "same_fingerprint(entry->contact_fingerprint, fingerprint)" in dm_mark
    assert "newest_rx_seq <= dm_thread_read_seq(fingerprint)" in dm_mark
    assert "upsert_dm_cursor(fingerprint, newest_rx_seq)" in dm_mark
    assert "d1l_read_state_mark_dm_read" not in dm_mark
    assert "d1l_read_state_mark_all_read" not in dm_mark
    assert "return read_seq >= dm_stats.next_seq ? 0U : read_seq;" in read_state
    assert "persist_mutation_or_rollback" in read_state
    assert "s_state = *previous;" in read_state
    assert "if (s_state.mark_read_count < UINT32_MAX)" in read_state
    assert "note_cursor_advance();" in public_mark
    assert "persist_mutation_or_rollback(&previous)" in public_mark
    assert "note_cursor_advance();" in dm_mark
    assert "persist_mutation_or_rollback(&previous)" in dm_mark

    assert "uint32_t read_state_mark_count;" in app_header
    assert "snapshot->read_state_mark_count = read_state.mark_read_count;" in app_model
    assert ".read_state_mark_count = snapshot->read_state_mark_count" in phase1
    assert "left->read_state_mark_count == right->read_state_mark_count" in phase1


def test_incoming_refresh_preserves_messages_mode_overlays_focus_and_rf_silence() -> None:
    phase1 = read("main/ui/ui_phase1.c")
    refresh = function_body(
        phase1,
        "static void process_pending_content_refresh(void)",
        "static lv_obj_t *find_scrollable_descendant",
    )

    assert "const d1l_ui_messages_mode_t messages_mode = s_messages_mode;" in refresh
    assert "s_messages_mode = messages_mode;" in refresh
    assert "d1l_ui_modal_visible(s_public_history_sheet)" in refresh
    assert "d1l_ui_modal_visible(s_public_search_sheet)" in refresh
    assert "d1l_ui_messages_thread_active(&s_messages_controller)" in refresh
    assert "d1l_ui_modal_visible(s_dm_search_sheet)" in refresh
    assert "render_public_history_sheet();" in refresh
    assert "render_dm_thread_sheet()" in refresh
    assert "const bool refresh_dm_thread =\n        d1l_ui_messages_thread_active" in refresh
    for forbidden in (
        "hide_compose_sheet",
        "hide_public_search_sheet",
        "hide_dm_search_sheet",
        "lv_keyboard_set_textarea",
        "d1l_app_model_mark_public_read",
        "d1l_app_model_mark_dm_thread_read",
        "d1l_app_model_send_public_text",
        "d1l_app_model_send_dm_text",
        "d1l_meshcore_service_send",
    ):
        assert forbidden not in refresh
    assert refresh.count("hide_dm_thread_sheet();") == 1
    assert refresh.index("hide_dm_thread_sheet();") > refresh.index(
        "if (render_dm_thread_sheet())"
    )

    active_render = function_body(
        phase1,
        "static void render_active_tab(void)",
        "esp_err_t d1l_ui_phase1_request_tab",
    )
    assert "d1l_ui_navigation_active() != D1L_UI_TAB_MESSAGES &&" in active_render
    assert "!d1l_ui_messages_thread_active(&s_messages_controller)" in active_render


def test_muted_unread_and_simulated_incoming_event_flows_remain_separate(tmp_path: Path) -> None:
    home_header = read("main/ui/ui_home_view.h")
    home_view = read("main/ui/ui_home_view.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "size_t muted_dm_unread_count;" in home_header
    assert '"%llu unread + %llu excluded"' in home_view
    assert '"%llu excluded"' in home_view
    assert ".muted_dm_unread_count = snapshot->muted_dm_unread_count" in phase1

    assert len(ui_simulator.EXPECTED_INCOMING_EVENT_FLOWS) == 4
    default_report = ui_simulator.generate(
        tmp_path / "default",
        views=tuple(flow["view"] for flow in ui_simulator.EXPECTED_INCOMING_EVENT_FLOWS),
    )
    large_report = ui_simulator.generate(
        tmp_path / "large",
        views=tuple(flow["view"] for flow in ui_simulator.EXPECTED_INCOMING_EVENT_FLOWS),
        scenario="large-mesh",
    )
    assert default_report["incoming_event_report"]["ok"] is True
    assert large_report["incoming_event_report"]["ok"] is True
