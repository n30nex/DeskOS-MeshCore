from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_messages_root_has_owned_view_model_action_and_lifetime_boundary():
    cmake = read("main/CMakeLists.txt")
    phase1 = read("main/ui/ui_phase1.c")
    header = read("main/ui/ui_messages.h")
    source = read("main/ui/ui_messages.c")

    assert '"ui/ui_messages.c"' in cmake
    assert '#include "ui_messages.h"' in phase1
    assert "d1l_ui_messages_view_model_t" in header
    assert "d1l_ui_messages_action_event_t" in header
    assert "d1l_ui_messages_controller_t" in header
    assert "d1l_ui_messages_render" in header
    assert "d1l_ui_messages_deactivate" in header
    assert '#include "app/app_model.h"' not in header
    assert '#include "mesh/dm_store.h"' in header
    assert '#include "mesh/message_store.h"' in header

    assert "controller->rendered = *view_model;" in source
    assert "binding->row_index >= controller->rendered.public_row_count" in source
    assert "binding->row_index >= controller->rendered.dm_row_count" in source
    assert "&controller->rendered.public_rows[binding->row_index]" in source
    assert "&controller->rendered.dm_rows[binding->row_index]" in source
    assert "memset(&controller->rendered, 0, sizeof(controller->rendered));" in source
    assert "controller->action_handler = NULL;" in source
    assert "static lv_obj_t *s_" not in source
    assert "lv_timer_" not in source
    assert "d1l_app_model_" not in source
    assert "d1l_meshcore_service_" not in source
    assert "d1l_storage_" not in source
    assert "freertos/" not in source

    assert "messages_view_model_from_snapshot(snapshot, &s_messages_controller.rendered);" in phase1
    assert "snapshot->active_channel_id" in phase1
    assert "d1l_app_model_query_channel_messages_page(" in phase1
    assert "snapshot->recent_dm_count" in phase1
    assert "D1L_UI_MESSAGES_PUBLIC_PREVIEW_ROWS" in phase1
    assert "D1L_UI_MESSAGES_DM_PREVIEW_ROWS" in phase1
    assert "handle_messages_action" in phase1
    assert "show_message_detail_for(event->public_message);" in phase1
    assert "show_dm_thread_for(event->dm_message->contact_fingerprint" in phase1
    assert "d1l_ui_messages_deactivate(&s_messages_controller);" in phase1


def test_messages_hierarchy_is_simple_bounded_and_navigation_is_rf_silent():
    header = read("main/ui/ui_messages.h")
    source = read("main/ui/ui_messages.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "D1L_UI_MESSAGES_MODE_ROOT = 0" in header
    assert "D1L_UI_MESSAGES_MODE_PUBLIC" in header
    assert "D1L_UI_MESSAGES_MODE_DIRECT" in header
    assert "D1L_UI_MESSAGES_ACTION_SHOW_ROOT" in header
    assert '"Choose a conversation type"' in source
    assert '"Default channel conversation"' in source
    assert '"Private contact conversations"' in source
    assert "messages_render_root_card(" in source
    assert "messages_create_scroll_body(parent, 18, 112, 424, 184)" in source
    assert "messages_create_scroll_body(parent, 18, 68, 424, 286)" in source
    assert '"Compose" : "Channel unavailable"' in source
    assert "18, 304, 424, 50" in source
    assert "D1L_UI_MESSAGES_ACTION_SEND_PUBLIC_TEST" not in header
    assert "d1l_app_model_send_public_test()" not in phase1
    assert "lv_obj_scroll_to_y(body, LV_COORD_MAX, LV_ANIM_OFF);" in source

    actions = phase1.split("static void handle_messages_action", 1)[1].split(
        "static void render_messages", 1
    )[0]
    for action in (
        "D1L_UI_MESSAGES_ACTION_SHOW_ROOT",
        "D1L_UI_MESSAGES_ACTION_SHOW_PUBLIC",
        "D1L_UI_MESSAGES_ACTION_SHOW_DIRECT",
    ):
        action_body = actions.split(f"case {action}:", 1)[1].split("break;", 1)[0]
        assert "d1l_app_model_send_public" not in action_body
        assert "d1l_app_model_send_dm" not in action_body
        assert "d1l_meshcore_service_" not in action_body


def test_direct_list_projects_unique_conversations_with_thread_cursor_truth():
    cmake = read("main/CMakeLists.txt")
    app_header = read("main/app/app_model.h")
    app_source = read("main/app/app_model.c")
    projector_header = read("main/app/dm_conversation_list.h")
    projector = read("main/app/dm_conversation_list.c")
    messages_header = read("main/ui/ui_messages.h")
    messages = read("main/ui/ui_messages.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert '"app/dm_conversation_list.c"' in cmake
    assert "D1L_DM_CONVERSATION_SOURCE_CAPACITY" in projector_header
    assert "d1l_dm_conversation_list_project(" in projector_header
    assert "for (size_t index = row_count; index > first; --index)" in projector
    assert "newest conversation first" in projector_header
    assert "conversation_unread_count(" in projector
    assert "row_unread[index]" in projector
    assert "d1l_dm_store_copy_recent(" in app_source
    assert "d1l_read_state_dm_entry_is_unread(" in app_source
    assert "d1l_dm_conversation_list_project(" in app_source
    assert "d1l_contact_store_find_by_fingerprint(" in app_source
    assert "contact.muted" in app_source
    assert "dm_conversation_count" in app_header
    assert "recent_dm_unread_count" in app_header
    assert "recent_dm_muted" in app_header
    assert "dm_row_unread_count" in messages_header
    assert "dm_row_muted" in messages_header
    assert "snapshot->dm_conversation_count" in phase1
    assert "snapshot->recent_dm_unread_count" in phase1
    assert "snapshot->recent_dm_muted" in phase1
    assert '"%lu unread%s | %s"' in messages
    assert '"%u conversations | %lu unread + %lu excluded"' in messages


def test_public_conversation_bubbles_align_and_show_truthful_time_and_state():
    source = read("main/ui/ui_messages.c")
    phase1 = read("main/ui/ui_phase1.c")
    bubble = source.split("static void messages_render_public_row", 1)[1].split(
        "static void messages_render_dm_row", 1
    )[0]

    assert "messages_public_time_label(" in bubble
    assert '"time unknown"' in source
    assert "no persisted boot epoch" in source
    assert 'return "Sent over RF";' in source
    assert 'return unread ? "New" : "Received";' in source
    assert "outgoing ? 76 : 8" in bubble
    assert "LV_LABEL_LONG_WRAP" in bubble
    assert "D1L_UI_MESSAGES_ACTION_OPEN_PUBLIC_MESSAGE" in bubble
    assert "details >" in bubble
    assert '"queued"' not in bubble


def test_channel_mark_read_targets_the_exact_active_channel():
    app_header = read("main/app/app_model.h")
    app_source = read("main/app/app_model.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "d1l_app_model_mark_public_read" in app_header
    app_action = app_source.split("esp_err_t d1l_app_model_mark_public_read", 1)[1].split(
        "esp_err_t d1l_app_model_mark_dm_thread_read", 1
    )[0]
    assert "d1l_read_state_mark_public_read()" in app_action
    assert "d1l_read_state_mark_all_read()" not in app_action
    ui_action = phase1.split("static void mark_public_read_event_cb", 1)[1].split(
        "static void open_dm_compose_for_contact", 1
    )[0]
    assert "s_messages_controller.rendered.active_channel_id" in ui_action
    assert "d1l_app_model_mark_channel_read(channel_id)" in ui_action
    assert "d1l_app_model_mark_messages_read" not in ui_action


def test_public_tx_truth_is_consistent_in_bubble_history_and_detail():
    phase1 = read("main/ui/ui_phase1.c")
    delivery = phase1.split("static const char *message_delivery_label", 1)[1].split(
        "static void reply_message_detail_event_cb", 1
    )[0]
    history = phase1.split("static const char *public_row_state", 1)[1].split(
        "static void close_public_history_event_cb", 1
    )[0]
    detail = phase1.split("static void render_message_detail_sheet", 1)[1].split(
        "static void show_message_detail_for", 1
    )[0]

    assert 'return "sent over RF";' in delivery
    assert '"sent over RF"' in history
    assert '"queued"' not in delivery
    assert '"queued"' not in history
    assert '"Signal  not measured for retained channel TxDone"' in detail
    assert '"Path hops  not measured for retained channel TxDone"' in detail
    assert "retained after %s" in detail
    assert '"delivered"' not in detail
