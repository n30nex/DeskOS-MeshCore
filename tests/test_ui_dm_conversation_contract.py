from pathlib import Path

from tools import ui_simulator


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_dm_conversation_controller_owns_bounded_state_and_stale_guards():
    header = read("main/ui/ui_messages.h")
    source = read("main/ui/ui_messages.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "D1L_UI_MESSAGES_CONTROLLER_MAX_BYTES 16384U" in header
    assert "_Static_assert(sizeof(d1l_ui_messages_controller_t)" in source
    assert "lv_obj_t *thread_sheet" in header
    assert "thread_entries[D1L_UI_MESSAGES_THREAD_MAX_ROWS]" in header
    assert "thread_unread[D1L_UI_MESSAGES_THREAD_MAX_ROWS]" in header
    assert "thread_rows[D1L_UI_MESSAGES_THREAD_MAX_ROWS]" in header
    assert "thread_limit" in header
    assert "thread_row_count" in header
    assert "thread_total_matches" in header
    assert "dm_row_unread_count[D1L_UI_MESSAGES_DM_PREVIEW_ROWS]" in header
    assert "dm_row_muted[D1L_UI_MESSAGES_DM_PREVIEW_ROWS]" in header
    assert "controller->rendered.dm_row_unread_count[index]" in source
    assert "unread && !muted ? 0xFBBF24" in source
    assert '"%lu unread%s | %s"' in source
    assert "messages_deactivate_actions(controller);" in source
    assert source.index("messages_deactivate_actions(controller);") < source.index(
        "lv_obj_clean(sheet);"
    )
    assert "binding->generation != binding->controller->generation" in source
    assert "binding->row_index >= controller->thread_row_count" in source
    assert "controller->thread_row_count > D1L_UI_MESSAGES_THREAD_MAX_ROWS" in source
    assert "static lv_obj_t *s_dm_thread_sheet" not in phase1
    assert "static d1l_dm_entry_t s_dm_thread_entries" not in phase1
    assert "static bool s_dm_thread_unread" not in phase1
    assert "static size_t s_dm_thread_limit" not in phase1
    hide = source.split("void d1l_ui_messages_hide_thread", 1)[1].split(
        "void d1l_ui_messages_deactivate", 1
    )[0]
    assert "if (controller->thread_fingerprint[0] != '\\0')" in hide
    assert "messages_deactivate_actions(controller);" in hide
    toggle = source.split("bool d1l_ui_messages_toggle_thread_details", 1)[1].split(
        "bool d1l_ui_messages_thread_active", 1
    )[0]
    assert "session_id != 0U ?" in toggle
    assert "controller->expanded_delivery_session_id == session_id" in toggle
    assert "controller->expanded_row_seq == row_seq" in toggle


def test_dm_primary_delivery_labels_cover_every_persisted_outbound_state():
    source = read("main/ui/ui_messages.c")
    inbound = source.split("static const char *messages_inbound_state", 1)[1].split(
        "const char *d1l_ui_messages_delivery_label", 1
    )[0]
    label = source.split("const char *d1l_ui_messages_delivery_label", 1)[1].split(
        "static void messages_format_snr", 1
    )[0]
    expected = {
        "QUEUED": "Queued",
        "WAITING_RADIO": "Waiting for radio",
        "TX_ACTIVE": "Sending",
        "TX_DONE": "Sent over RF",
        "AWAITING_ACK": "Sent over RF / awaiting delivery",
        "ACKNOWLEDGED": "Delivered",
        "RETRY_WAIT": "Retry scheduled",
        "RETRY_TX": "Retrying",
        "FAILED_RADIO": "Failed",
        "FAILED_TIMEOUT": "Failed",
        "FAILED_QUEUE": "Failed",
        "INTERRUPTED_BY_REBOOT": "Interrupted by reboot",
        "CANCELLED": "Cancelled",
        "NOT_APPLICABLE": "Status unavailable",
    }
    for state, text in expected.items():
        assert f"D1L_DM_DELIVERY_{state}" in label
        assert f'return "{text}";' in label
    assert "entry->delivery_state" in label
    assert "entry->acked" not in label
    assert "entry->delivered" not in label
    assert "entry->ack_dispatch_count == 0U" in inbound
    assert "entry->ack_last_error == ESP_OK" in inbound
    assert 'return "Received / ACK needed";' in inbound


def test_dm_bubbles_wrap_align_and_disclose_exact_persisted_technical_state():
    source = read("main/ui/ui_messages.c")
    bubble = source.split("static bool messages_render_thread_bubble", 1)[1].split(
        "bool d1l_ui_messages_select_thread", 1
    )[0]

    assert "lv_obj_t *row = lv_obj_create(body);" in bubble
    assert "lv_obj_set_width(row, LV_PCT(100));" in bubble
    assert "outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START" in bubble
    assert 'const char *who = outgoing ? "You"' in bubble
    assert "LV_LABEL_LONG_WRAP" in source
    assert "entry->text[0] ? entry->text : \"-\"" in bubble
    assert "D1L_UI_MESSAGES_ACTION_TOGGLE_DM_DETAILS" in bubble
    assert "messages_thread_entry_is_expanded" in bubble
    for field in (
        "delivery_state",
        "delivery_reason",
        "delivery_last_error",
        "delivery_revision",
        "delivery_session_id",
        "delivery_retry_count",
        "attempt",
        "ack_state",
        "ack_dispatch_count",
        "ack_last_error",
        "ack_hash",
        "identity_digest_valid",
        "rssi_dbm",
        "snr_tenths",
        "path_hash_bytes",
        "path_hops",
        "seq",
    ):
        assert f"entry->{field}" in bubble or (
            field == "snr_tenths" and "messages_format_snr" in bubble
        )
    assert "Bounded delivery retry is active; no manual resend is needed." in bubble
    assert "Final delivery failure is retained." in bubble
    assert "no automatic retry is pending." in bubble
    detail_format = bubble.split("if (outgoing) {", 1)[1].split(
        "if (!messages_create_wrapped_label(bubble, technical", 1
    )[0]
    outgoing_details, incoming_details = detail_format.split("} else {", 1)
    assert "expected ACK %08lX" in outgoing_details
    assert "d1l_dm_ack_state_name" not in outgoing_details
    assert "ACK dispatch %s" in incoming_details
    assert "d1l_dm_ack_state_name(entry->ack_state)" in incoming_details


def test_dm_thread_marks_read_once_refreshes_on_transitions_and_stays_rf_silent():
    app_header = read("main/app/app_model.h")
    app_source = read("main/app/app_model.c")
    phase1 = read("main/ui/ui_phase1.c")
    messages = read("main/ui/ui_messages.c")

    show = phase1.split("static void show_dm_thread_for", 1)[1].split(
        "static void open_home_dm_preview_event_cb", 1
    )[0]
    refresh = phase1.split("static void process_pending_content_refresh", 1)[1].split(
        "static lv_obj_t *find_scrollable_descendant", 1
    )[0]
    actions = phase1.split("static void handle_messages_action", 2)[2].split(
        "static void render_messages", 1
    )[0]

    assert "uint32_t dm_content_revision" in app_header
    assert "snapshot->dm_content_revision = dms.content_revision" in app_source
    assert ".dm_content_revision = snapshot->dm_content_revision" in phase1
    assert "left->dm_content_revision == right->dm_content_revision" in phase1
    assert show.count("d1l_app_model_mark_dm_thread_read(fingerprint)") == 1
    assert show.index("d1l_app_model_mark_dm_thread_read(fingerprint)") < show.index(
        "render_dm_thread_sheet()"
    )
    assert "refresh_dm_thread" in refresh
    assert "render_dm_thread_sheet()" in refresh
    assert "D1L_UI_MESSAGES_ACTION_LOAD_OLDER_DM_THREAD" in actions
    assert "D1L_UI_MESSAGES_ACTION_REPLY_DM_THREAD" in actions
    assert "D1L_UI_MESSAGES_ACTION_TOGGLE_DM_DETAILS" in actions
    close_case = actions.split("D1L_UI_MESSAGES_ACTION_CLOSE_DM_THREAD", 1)[1].split(
        "D1L_UI_MESSAGES_ACTION_LOAD_OLDER_DM_THREAD", 1
    )[0]
    reply_case = actions.split("D1L_UI_MESSAGES_ACTION_REPLY_DM_THREAD", 1)[1].split(
        "D1L_UI_MESSAGES_ACTION_TOGGLE_DM_DETAILS", 1
    )[0]
    assert "request_content_refresh();" in close_case
    assert "request_content_refresh();" in reply_case
    assert "d1l_meshcore_service_send" not in actions
    assert "d1l_app_model_send_dm_text" not in actions
    assert (
        'sheet, "Message this contact                         >",\n'
        "            16, 360, 448, 52"
    ) in messages
    assert 'sheet, "Read"' not in messages.split(
        "bool d1l_ui_messages_render_thread(", 1
    )[1].split("bool d1l_ui_messages_expand_thread", 1)[0]


def test_simulator_projects_direction_and_every_delivery_state_truthfully():
    expected = {
        "not_applicable": "Status unavailable",
        "queued": "Queued",
        "waiting_radio": "Waiting for radio",
        "tx_active": "Sending",
        "tx_done": "Sent over RF",
        "awaiting_ack": "Sent over RF / awaiting delivery",
        "acknowledged": "Delivered",
        "retry_wait": "Retry scheduled",
        "retry_tx": "Retrying",
        "failed_radio": "Failed",
        "failed_timeout": "Failed",
        "failed_queue": "Failed",
        "interrupted_by_reboot": "Interrupted by reboot",
        "cancelled": "Cancelled",
    }
    list_expected = {
        "not_applicable": "Status unknown",
        "queued": "Queued",
        "waiting_radio": "Waiting radio",
        "tx_active": "Sending",
        "tx_done": "Sent RF",
        "awaiting_ack": "Awaiting ACK",
        "acknowledged": "Delivered",
        "retry_wait": "Retry waiting",
        "retry_tx": "Retrying",
        "failed_radio": "Failed",
        "failed_timeout": "Failed",
        "failed_queue": "Failed",
        "interrupted_by_reboot": "Interrupted",
        "cancelled": "Cancelled",
    }
    assert ui_simulator.DM_DELIVERY_LABELS == expected
    assert ui_simulator.DM_LIST_DELIVERY_LABELS == list_expected
    for state, label in expected.items():
        message = ui_simulator.Message(
            "You", "bounded message", "technical metadata",
            direction="tx", delivery_state=state,
        )
        assert ui_simulator.dm_primary_delivery_label(message) == label
        assert ui_simulator.dm_list_delivery_label(message) == list_expected[state]
    assert ui_simulator.dm_primary_delivery_label(
        ui_simulator.Message("Peer", "hello", "", unread=True)
    ) == "New"
    assert ui_simulator.dm_primary_delivery_label(
        ui_simulator.Message("Peer", "hello", "")
    ) == "Received"
    surface = ui_simulator.Surface("messages_dm")
    ui_simulator.render_messages_dm(surface, ui_simulator.sample_snapshot())
    assert "1 unread | Awaiting ACK" in surface.metrics["dm_rendered_states"]
    details = ui_simulator.Surface("dm_thread_details_sheet")
    ui_simulator.render_dm_thread_details_sheet(details, ui_simulator.sample_snapshot())
    assert details.metrics["dm_thread_details_expanded"] is True
    assert details.metrics["dm_thread_details_single_row"] is True
    assert details.metrics["dm_thread_details_rendered_lines"] > 0
    assert details.metrics["dm_thread_sticky_reply"] is True
    assert details.metrics["dm_thread_navigation_rf_silent"] is True
