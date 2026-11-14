from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_notifications_are_emitted_only_for_new_live_admissions():
    source = (ROOT / "main/mesh/meshcore_service.c").read_text(encoding="utf-8")
    group = source.split("static void parse_rx_channel_packet(", 1)[1].split("static bool parse_rx_dm_packet(", 1)[0]
    assert group.index("d1l_meshcore_packet_hash_cache_contains") < group.index("if (!message_result.admitted)")
    append = source.split("static d1l_channel_rx_store_result_t append_channel_message_store_rx(", 1)[1].split("static bool append_channel_message_store_tx(", 1)[0]
    assert append.index("d1l_message_store_append_channel_at(") < append.index("if (message_seq != 0U)") < append.index("note_received_message(false, channel_name, author, body)")
    assert append.count("note_received_message(") == 1
    dm = source.split("static bool parse_rx_dm_packet(", 1)[1].split("typedef enum {", 1)[0]
    assert dm.index("if (duplicate)") < dm.index("if (!store_outcome.inserted)") < dm.index("if (!contact->muted)") < dm.index("note_received_message(!room_post,")
    assert dm.count("note_received_message(") == 1
    assert source.count("note_received_message(") == 3  # definition + two RX paths


def test_ui_consumes_receive_events_without_using_unread_deltas():
    source = (ROOT / "main/ui/ui_phase1.c").read_text(encoding="utf-8")
    timer = source.split("static void display_runtime_timer_cb(", 1)[1].split("static void map_viewport_timer_cb(", 1)[0]
    assert "s_last_notification_unread" not in source
    for guard in ("D1L_NOTIFICATION_MODE_OFF", "D1L_NOTIFICATION_MODE_QUIET_HOURS", "notification_quiet_now()",
                  "!s_backlight_dimmed", "!s_lock_visible", "!s_onboarding_visible", "!d1l_ui_boot_scene_visible"):
        assert guard in timer
    assert "d1l_meshcore_service_receive_snapshot(&groups, &dms, &received)" in timer
    assert "!preferences.presentation_mode && preferences.timeout_seconds" in timer
    assert "d1l_ui_receive_animation_set_message" in timer
    assert "preferences.presentation_mode && !d1l_ui_modal_has_active()" in timer
    lock = source.split("static void lock_event_cb(lv_event_t *event)\n{", 1)[1].split("static uint8_t desired_backlight_percent", 1)[0]
    assert "d1l_ui_receive_animation_stop" in lock
