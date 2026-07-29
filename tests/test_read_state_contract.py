from pathlib import Path

from scripts.smoke_d1l import SMOKE_COMMANDS


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_read_state_tracks_bounded_per_thread_dm_cursors():
    header = read("main/mesh/read_state.h")
    source = read("main/mesh/read_state.c")
    cmake = read("main/CMakeLists.txt")
    app_main = read("main/app_main.c")
    app_header = read("main/app/app_model.h")
    app_source = read("main/app/app_model.c")

    assert "d1l_read_state_stats_t" in header
    assert "public_unread_count" in header
    assert "muted_dm_unread_count" in header
    assert "D1L_READ_STATE_DM_THREAD_CAPACITY 16U" in header
    assert "d1l_read_state_dm_thread_t" in header
    assert "d1l_read_state_mark_dm_thread_read" in header
    assert "d1l_read_state_dm_entry_is_unread" in header
    assert "d1l_read_state_copy_dm_threads" in header

    assert 'D1L_READ_STATE_KEY "state"' in source
    assert '#include "storage/retained_blob_store.h"' in source
    assert "D1L_RETAINED_BLOB_STORE_READ_STATE" in source
    assert "d1l_retained_blob_store_read(" in source
    assert "d1l_retained_blob_store_write(" in source
    assert "d1l_retained_blob_store_erase(" in source
    assert "nvs_get_blob" not in source
    assert "nvs_set_blob" not in source
    assert "static d1l_message_entry_t s_message_scratch" in source
    assert "static d1l_dm_entry_t s_dm_scratch" in source
    assert "D1L_READ_STATE_VISIBLE_DM_CAPACITY (D1L_DM_STORE_CAPACITY + 1U)" in source
    assert "s_dm_scratch[D1L_READ_STATE_VISIBLE_DM_CAPACITY]" in source
    assert "s_thread_scratch[D1L_READ_STATE_VISIBLE_DM_CAPACITY]" in source
    assert "d1l_contact_store_find_by_fingerprint" in source
    assert "contact.muted" in source
    assert "D1L_READ_STATE_SCHEMA 2U" in source
    assert "D1L_READ_STATE_SCHEMA_V1 1U" in source
    assert "dm_cursors[D1L_READ_STATE_DM_THREAD_CAPACITY]" in source
    assert "blob_v1_is_valid" in source
    assert "thread_seq > s_state.last_dm_read_seq" in source
    assert "return read_seq >= dm_stats.next_seq ? 0U : read_seq;" in source
    assert "stats.newest_public_rx_seq <= stats.last_public_read_seq" in source
    assert "newest_rx_seq <= dm_thread_read_seq(fingerprint)" in source
    assert "build_dm_thread_stats" in source
    assert source.count(
        "s_dm_scratch, D1L_READ_STATE_VISIBLE_DM_CAPACITY"
    ) == 2
    assert "upsert_dm_cursor(fingerprint, newest_rx_seq)" in source
    assert '"mesh/read_state.c"' in cmake
    assert "d1l_read_state_init()" in app_main

    assert "public_unread_count" in app_header
    assert "dm_unread_count" in app_header
    assert "muted_dm_unread_count" in app_header
    assert "last_public_read_seq" in app_header
    assert "last_dm_read_seq" in app_header
    assert "recent_dm_unread[D1L_APP_SNAPSHOT_DM_PREVIEW]" in app_header
    assert "recent_dm_unread_count[D1L_APP_SNAPSHOT_DM_PREVIEW]" in app_header
    assert "recent_dm_muted[D1L_APP_SNAPSHOT_DM_PREVIEW]" in app_header
    assert "d1l_app_model_mark_public_read" in app_header
    assert "d1l_app_model_mark_dm_thread_read" in app_header
    assert "d1l_read_state_stats()" in app_source
    assert "d1l_read_state_mark_public_read()" in app_source
    assert "d1l_read_state_mark_all_read()" not in app_source
    assert "d1l_read_state_dm_entry_is_unread(" in app_source
    assert "d1l_dm_conversation_list_project(" in app_source
    assert "snapshot->recent_dm_unread_count[i] > 0U" in app_source
    assert "d1l_read_state_mark_dm_thread_read(fingerprint)" in app_source


def test_console_controls_remain_and_ui_marks_dm_threads_read_on_open():
    console = read("main/comms/usb_console.c")
    app_source = read("main/app/app_model.c")
    ui = read("main/ui/ui_phase1.c")
    messages_ui = read("main/ui/ui_messages.c")
    show_thread = ui.split("static void show_dm_thread_for", 1)[1].split(
        "static void open_home_dm_preview_event_cb", 1
    )[0]

    assert 'ok_begin("messages unread")' in console
    assert 'strncmp(line, "messages read ", 14)' in console
    assert "d1l_app_model_mark_public_read()" in console
    assert "d1l_read_state_mark_public_read()" in app_source
    assert "d1l_read_state_mark_dm_read()" in console
    assert "d1l_read_state_mark_all_read()" in console
    assert '"messages unread"' in console
    assert "messages read <public|dm|dm <fingerprint>|all>" in console
    assert "d1l_read_state_mark_dm_thread_read(thread_fingerprint)" in console
    assert "d1l_read_state_copy_dm_threads(threads, 8)" in console
    assert "dm_threads" in console
    assert "dm_thread_count" in console
    assert "messages unread" in SMOKE_COMMANDS

    assert "mark_public_read_event_cb" in ui
    mark_channel = ui.split(
        "static void mark_public_read_event_cb", 1
    )[1].split("static void open_dm_compose_for_contact", 1)[0]
    assert "s_messages_controller.rendered.active_channel_id" in mark_channel
    assert "d1l_app_model_mark_channel_read(channel_id)" in mark_channel
    assert "d1l_app_model_mark_public_read()" not in mark_channel
    assert "d1l_app_model_mark_messages_read" not in ui
    public_view = messages_ui.split(
        "static void messages_render_public", 1
    )[1].split("static void messages_render_direct", 1)[0]
    assert 'parent, "Read", 306, 8, 64, 44' in public_view
    assert "D1L_UI_MESSAGES_ACTION_MARK_PUBLIC_READ" in public_view
    assert 'return unread ? "New" : "Received";' in messages_ui
    assert "snapshot->muted_dm_unread_count" in ui
    assert "read_dm_thread_event_cb" not in ui
    assert 'create_button(s_dm_thread_sheet, "Read"' not in ui
    assert 'sheet, "Read"' not in messages_ui.split(
        "bool d1l_ui_messages_render_thread(", 1
    )[1].split("bool d1l_ui_messages_expand_thread", 1)[0]
    assert "d1l_app_model_mark_dm_thread_read(fingerprint)" in show_thread
    assert show_thread.index("d1l_app_model_mark_dm_thread_read(fingerprint)") < show_thread.index(
        "render_dm_thread_sheet()"
    )
    assert "messages_render_dm_row(controller, body, row_y + (int)i * 80, i)" in messages_ui
    assert "d1l_ui_messages_delivery_label(entry, unread)" in messages_ui
