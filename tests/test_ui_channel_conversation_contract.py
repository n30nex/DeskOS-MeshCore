from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def test_active_channel_view_uses_exact_runtime_identity_and_rows() -> None:
    header = read("main/ui/ui_messages.h")
    phase1 = read("main/ui/ui_phase1.c")

    assert "uint64_t active_channel_id;" in header
    assert "char active_channel_name[D1L_CHANNEL_NAME_LEN];" in header
    assert "d1l_channel_info_t channels[D1L_CHANNEL_STORE_CAPACITY];" in header
    assert "uint32_t last_channel_read_seq;" in header

    projection = between(
        phase1,
        "static void messages_view_model_from_snapshot",
        "static void handle_messages_action",
    )
    assert "snapshot->active_channel_id" in projection
    assert "d1l_ui_messages_projected_channel_id(" in projection
    assert "snapshot, view_model->active_channel_id, &active_channel" in projection
    assert "active_channel.unread_count" in projection
    assert "snapshot_channel_read_seq(" in projection
    assert "d1l_app_model_query_channel_messages_page(" in projection
    assert "active_channel.channel_id" in projection
    assert "snapshot->recent_messages" not in projection
    assert "d1l_app_model_query_public_messages" not in projection


def test_selector_is_bounded_redacted_and_selection_is_fail_closed() -> None:
    header = read("main/ui/ui_messages.h")
    messages = read("main/ui/ui_messages.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "channel_rows[D1L_CHANNEL_STORE_CAPACITY]" in header
    selector = between(
        messages,
        "bool d1l_ui_messages_render_channel_selector",
        "bool d1l_ui_messages_channel_selector_active",
    )
    assert "D1L_CHANNEL_STORE_CAPACITY" in selector
    assert "8 + (int)i * 60, 416, 52" in selector
    assert "channel->name" in selector
    assert "channel->unread_count" in selector
    assert "channel->enabled" in selector
    assert "active | " in selector
    assert "channel_hash" not in selector
    assert "history_key" not in selector
    assert "secret" not in selector
    assert "if (!name)" not in selector
    assert "complete = false;" in selector
    assert "messages_deactivate_actions(controller);" in selector
    assert "lv_obj_clean(sheet);" in selector

    notice = between(
        messages,
        "static bool messages_render_channel_selector_notice",
        "bool d1l_ui_messages_render_channel_selector",
    )
    assert "if (!panel)" in notice
    assert "if (!label)" in notice
    assert "lv_obj_del(panel);" in notice

    actions = between(
        phase1,
        "static void handle_messages_action",
        "static void render_messages",
    )
    select = actions.split(
        "case D1L_UI_MESSAGES_ACTION_SELECT_CHANNEL:", 1
    )[1].split("case D1L_UI_MESSAGES_ACTION_NONE:", 1)[0]
    assert "!event->channel->enabled" in select
    assert "d1l_app_model_select_channel(" in select
    success = select.split("if (ret == ESP_OK)", 1)[1]
    assert "d1l_ui_messages_hide_channel_selector(" in success
    assert "request_content_refresh();" in success
    before_success = select.split("if (ret == ESP_OK)", 1)[0]
    assert "d1l_ui_messages_hide_channel_selector(" not in before_success
    assert "d1l_app_model_send_" not in select

    open_selector = actions.split(
        "case D1L_UI_MESSAGES_ACTION_OPEN_CHANNEL_SELECTOR:", 1
    )[1].split(
        "case D1L_UI_MESSAGES_ACTION_CLOSE_CHANNEL_SELECTOR:", 1
    )[0]
    assert "if (!show_channel_selector_sheet())" in open_selector
    assert "show_toast(\"Channels\", ESP_ERR_NO_MEM);" in open_selector
    assert "request_content_refresh();" in open_selector


def test_selector_exposes_bounded_management_entry_points_without_secrets() -> None:
    header = read("main/ui/ui_messages.h")
    messages = read("main/ui/ui_messages.c")

    for action in (
        "D1L_UI_MESSAGES_ACTION_CREATE_CHANNEL",
        "D1L_UI_MESSAGES_ACTION_IMPORT_CHANNEL",
        "D1L_UI_MESSAGES_ACTION_MANAGE_CHANNEL",
    ):
        assert action in header
        assert action in messages
    assert "channel_manage_rows[D1L_CHANNEL_STORE_CAPACITY]" in header

    selector = between(
        messages,
        "bool d1l_ui_messages_render_channel_selector",
        "bool d1l_ui_messages_channel_selector_active",
    )
    assert 'sheet, "New"' in selector
    assert 'sheet, "Import"' in selector
    assert 'row, "Manage"' in selector
    assert 'row, "Manage", 310, 0, 96, 44' in selector
    assert "channel_manage_rows[i]" in selector
    assert "channel->channel_id" in selector
    for forbidden in (
        "secret",
        "export_share_uri",
        "copy_protocol_key",
        "channel_hash",
        "history_key",
    ):
        assert forbidden not in selector

    dispatch = between(
        messages,
        "static void messages_dispatch_event_cb",
        "static lv_obj_t *messages_create_button",
    )
    assert "D1L_UI_MESSAGES_ACTION_MANAGE_CHANNEL" in dispatch
    assert "binding->row_index >= controller->rendered.channel_count" in dispatch
    assert "action_event.channel" in dispatch


def test_channel_management_ui_uses_confirmed_redacted_app_boundary() -> None:
    phase1 = read("main/ui/ui_phase1.c")
    actions = between(
        phase1,
        "static void channel_management_action_handler(",
        "static void handle_messages_action(",
    )

    for app_call in (
        "d1l_app_model_create_channel(",
        "d1l_app_model_import_channel_uri(",
        "d1l_app_model_update_channel(",
        "d1l_app_model_export_channel_share_uri(",
        "d1l_app_model_remove_channel(",
    ):
        assert app_call in actions
    confirm_remove = actions.split(
        "case D1L_UI_CHANNEL_ACTION_CONFIRM_REMOVE:", 1
    )[1].split("case D1L_UI_CHANNEL_ACTION_NONE:", 1)[0]
    assert "channel->channel_id, true" in confirm_remove
    assert "return_to_channel_selector();" in confirm_remove

    export = between(
        phase1,
        "static bool show_channel_export_sheet(",
        "static void return_to_channel_selector(",
    )
    assert "char uri[D1L_CHANNEL_SHARE_URI_LEN] = {0};" in export
    assert "d1l_ui_channel_sheets_set_export_uri(" in export
    assert "d1l_app_model_clear_channel_share_uri(uri, sizeof(uri));" in export
    assert "printf(" not in export
    assert "ESP_LOG" not in export

    assert "bounded_channel_uri_length(event->text)" in actions
    assert "d1l_ui_channel_sheets_hide_import(" in actions
    assert "D1L_CHANNEL_PUBLIC_ID" in actions
    for forbidden in (
        "d1l_app_model_send_public_text",
        "d1l_app_model_send_channel_text",
        "d1l_app_model_send_active_channel_text",
        "usb_console",
    ):
        assert forbidden not in actions

    messages_actions = between(
        phase1,
        "static void handle_messages_action(",
        "static void render_messages(",
    )
    assert "case D1L_UI_MESSAGES_ACTION_CREATE_CHANNEL:" in messages_actions
    assert "case D1L_UI_MESSAGES_ACTION_IMPORT_CHANNEL:" in messages_actions
    assert "case D1L_UI_MESSAGES_ACTION_MANAGE_CHANNEL:" in messages_actions
    assert "set_managed_channel(event->channel)" in messages_actions


def test_history_read_compose_and_reply_keep_the_captured_channel_id() -> None:
    phase1 = read("main/ui/ui_phase1.c")

    mark = between(
        phase1,
        "static void mark_public_read_event_cb",
        "static void open_dm_compose_for_contact",
    )
    assert "rendered.active_channel_id" in mark
    assert "d1l_app_model_mark_channel_read(channel_id)" in mark

    history = between(
        phase1,
        "static void render_public_history_sheet(void)\n{",
        "static void show_public_history_sheet(void)\n{",
    )
    assert "s_public_history_channel_id" in history
    assert "d1l_app_model_query_channel_messages_page(" in history
    assert "d1l_app_model_query_public_messages" not in history

    compose = between(
        phase1,
        "static bool show_channel_compose_sheet",
        "static void open_compose_event_cb",
    )
    assert "snapshot_find_channel(&s_snapshot, channel_id" in compose
    assert "!channel.enabled" in compose
    assert "s_compose_channel_id = channel.channel_id" in compose

    send = between(
        phase1,
        "static void send_compose_text(void)",
        "static void send_compose_event_cb",
    )
    assert "d1l_app_model_send_channel_text(" in send
    assert "s_compose_channel_id" in send
    assert "d1l_app_model_send_active_channel_text" not in send
    assert "d1l_app_model_send_public_text" not in send

    reply = between(
        phase1,
        "static void reply_message_detail_event_cb",
        "static d1l_ui_dm_identity_eligibility_t public_sender_dm_eligibility",
    )
    assert "entry.channel_id" in reply
    assert "show_channel_compose_sheet(" in reply


def test_channel_generation_refresh_preserves_modals_and_public_probe_aliases() -> None:
    phase1 = read("main/ui/ui_phase1.c")

    generation = between(
        phase1,
        "typedef struct {\n    uint32_t rx_packets;",
        "} d1l_ui_content_generation_t;",
    )
    assert "uint32_t channel_store_revision;" in generation
    assert "uint64_t active_channel_id;" in generation
    assert ".channel_store_revision = snapshot->channel_store_revision" in phase1
    assert ".active_channel_id = snapshot->active_channel_id" in phase1

    refresh = between(
        phase1,
        "static void process_pending_content_refresh(void)",
        "static lv_obj_t *find_scrollable_descendant",
    )
    assert "refresh_channel_selector" in refresh
    assert "d1l_ui_messages_render_channel_selector(" in refresh
    assert "refresh_public_history" in refresh
    assert "render_public_history_sheet();" in refresh
    refresh_failure = refresh.split(
        "if (refresh_channel_selector)", 1
    )[1].split("force_ui_layout_repaint();", 1)[0]
    assert "request_content_refresh();" in refresh_failure

    scroll_probe = between(
        phase1,
        "static lv_obj_t *scroll_probe_open_surface",
        "static void run_scroll_probe_on_ui_task",
    )
    assert 'strcmp(surface, "public_messages") == 0' in scroll_probe
    assert "s_public_history_channel_id = D1L_CHANNEL_PUBLIC_ID;" in scroll_probe

    keyboard_probe = between(
        phase1,
        "static void open_keyboard_probe_on_ui_task",
        "static void run_keyboard_probe_on_ui_task",
    )
    public_search = keyboard_probe.split(
        'if (strcmp(target, "public_search") == 0)', 1
    )[1].split('} else if (strcmp(target, "dm_search") == 0)', 1)[0]
    assert "s_public_history_channel_id = D1L_CHANNEL_PUBLIC_ID;" in public_search
    assert "open_public_search_event_cb(NULL);" in public_search
    assert "D1L_CHANNEL_PUBLIC_ID, \"Compose Public\", \"Public message\"" in phase1
    assert "d1l_channel_store_" not in between(
        phase1,
        "static void messages_view_model_from_snapshot",
        "static void render_messages",
    )
