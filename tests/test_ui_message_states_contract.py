from __future__ import annotations

from pathlib import Path

import pytest

from tools import ui_simulator


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_messages_state_projection_uses_live_store_and_snapshot_signals() -> None:
    app_header = read("main/app/app_model.h")
    app_source = read("main/app/app_model.c")
    messages_header = read("main/ui/ui_messages.h")
    phase1 = read("main/ui/ui_phase1.c")

    for field in (
        "message_store_loaded",
        "dm_store_loaded",
        "message_store_persistence_degraded",
        "dm_store_persistence_degraded",
        "dm_retry_active",
        "dm_failure_latched",
        "dm_capable_contact_count",
    ):
        assert field in app_header
        assert f"snapshot->{field}" in app_source
        assert f"snapshot->{field}" in phase1

    assert "messages.loaded" in app_source
    assert "dms.loaded" in app_source
    assert "D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES" in app_source
    assert "D1L_RETAINED_BLOB_STORE_DM_MESSAGES" in app_source
    store_health = app_source.split(
        "static bool retained_store_persistence_degraded", 1
    )[1].split("static void hex_prefix", 1)[0]
    assert "storage.retained_backup_degraded" not in store_health
    assert "d1l_retained_blob_store_nvs_ready()" in store_health
    assert "d1l_retained_blob_store_nvs_error()" in store_health
    assert "d1l_retained_blob_store_nvs_migration_error()" in store_health
    assert "stats->nvs_mirror_last_error" in store_health
    assert "stats->sd_degraded_latched" in store_health
    assert "D1L_RETAINED_BLOB_STORE_PUBLIC_MESSAGES" in app_source
    assert "D1L_RETAINED_BLOB_STORE_DM_MESSAGES" in app_source
    assert "D1L_RETAINED_BLOB_STORE_PACKET_LOG" not in store_health
    assert "s_dm_capable_contact_source[D1L_CONTACT_STORE_CAPACITY]" in app_source
    assert "d1l_contact_store_copy_recent(" in app_source
    assert "d1l_contact_store_can_dm(" in app_source
    assert "D1L_UI_MESSAGES_STORE_LOADING" in messages_header
    assert "D1L_UI_MESSAGES_STORE_READY" in messages_header
    assert "D1L_UI_MESSAGES_STORE_DEGRADED" in messages_header
    assert "D1L_UI_MESSAGES_STORE_UNAVAILABLE" in messages_header
    assert "messages_store_state_from_snapshot(snapshot, false)" in phase1
    assert "messages_store_state_from_snapshot(snapshot, true)" in phase1
    assert 'strcmp(backend, "unavailable") != 0' in phase1


def test_messages_states_keep_ram_history_readable_and_distinguish_empty_cases() -> None:
    messages = read("main/ui/ui_messages.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "Storage degraded; readable RAM history remains." in messages
    assert "Persistence unavailable; readable RAM history remains." in messages
    assert "Loading retained channel history..." in messages
    assert "Loading retained direct-message history..." in messages
    assert "No DM contacts available. Add a verified chat contact." in messages
    assert "No direct-message history yet." in messages
    assert "No readable direct-message history in RAM." in messages

    direct = messages.split("static void messages_render_direct", 1)[1].split(
        "void d1l_ui_messages_render", 1
    )[0]
    assert "messages_render_store_notice(" in direct
    assert "controller->rendered.dm_capable_contact_count == 0U" in direct
    assert "messages_render_dm_row(" in direct
    assert direct.index("messages_render_dm_row(") < direct.index(
        "controller->rendered.dm_row_count == 0U"
    )

    render_thread = messages.split("bool d1l_ui_messages_render_thread", 1)[1].split(
        "bool d1l_ui_messages_expand_thread", 1
    )[0]
    assert "bool reply_available" in render_thread
    assert "Contact unavailable; retained history remains readable." in render_thread
    assert (
        'sheet, "Message this contact                         >",\n'
        "            16, 360, 448, 52"
    ) in render_thread
    assert 'sheet, "Contact unavailable", 16, 360, 448, 52, NULL' in render_thread
    assert "lv_obj_add_state(reply, LV_STATE_DISABLED);" in render_thread
    assert 'sheet, "Search", 376, 6, 88, 44' in render_thread
    assert "16, 360, 448, 52" in render_thread

    phase_render = phase1.split("static bool render_dm_thread_sheet", 2)[2].split(
        "static void show_dm_thread_after_search", 1
    )[0]
    assert "d1l_app_model_find_contact(fingerprint, &contact)" in phase_render
    assert "dm_identity_for_contact(&contact, NULL).can_open_compose" in phase_render
    assert "d1l_app_model_send_dm_text" not in phase_render
    assert "d1l_app_model_mark_dm_thread_read" not in phase_render


def test_messages_retry_and_final_failure_are_distinct_live_delivery_states() -> None:
    header = read("main/ui/ui_messages.h")
    messages = read("main/ui/ui_messages.c")
    phase1 = read("main/ui/ui_phase1.c")

    assert "D1L_DM_DELIVERY_RETRY_WAIT" in header
    assert "D1L_DM_DELIVERY_RETRY_TX" in header
    for terminal in (
        "D1L_DM_DELIVERY_FAILED_RADIO",
        "D1L_DM_DELIVERY_FAILED_TIMEOUT",
        "D1L_DM_DELIVERY_FAILED_QUEUE",
        "D1L_DM_DELIVERY_INTERRUPTED_BY_REBOOT",
    ):
        assert terminal in header
    assert "A bounded delivery retry is in progress." in messages
    assert "A final delivery failure is retained; open it for details." in messages
    assert "Bounded delivery retry is active; no manual resend is needed." in messages
    assert "no automatic retry is pending" in messages
    app = read("main/app/app_model.c")
    projector = read("main/app/dm_conversation_list.c")
    assert "snapshot->dm_delivery_active &&" in app
    assert "delivery_state == D1L_DM_DELIVERY_RETRY_WAIT" in app
    assert "delivery_state == D1L_DM_DELIVERY_RETRY_TX" in app
    assert "d1l_dm_conversation_list_has_retained_failure(" in app
    assert "D1L_DM_CONVERSATION_SOURCE_CAPACITY" in app
    assert "delivery_failure_latched(rows[index].delivery_state)" in projector
    assert "view_model->dm_retry_active = snapshot->dm_retry_active" in phase1
    assert "view_model->dm_failure_latched = snapshot->dm_failure_latched" in phase1


@pytest.mark.parametrize("scenario", ("default", "large-mesh"))
def test_default_and_large_simulators_cover_all_message_states(
    tmp_path: Path, scenario: str
) -> None:
    views = (
        "messages_loading",
        "messages_public_storage_degraded",
        "messages_dm_storage_unavailable",
        "messages_dm_no_contact",
        "messages_dm_no_history",
        "messages_dm_retry",
        "messages_dm_failure",
        "dm_thread_no_contact",
    )
    report = ui_simulator.generate(
        tmp_path / scenario, views=views, scenario=scenario
    )
    assert report["ok"] is True
    rendered = {view["name"]: view for view in report["views"]}
    assert set(rendered) == set(views)

    loading = rendered["messages_loading"]["metrics"]
    assert loading["public_store_state"] == "loading"
    assert loading["dm_store_state"] == "loading"
    degraded = rendered["messages_public_storage_degraded"]["metrics"]
    assert degraded["public_store_state"] == "degraded"
    assert degraded["public_ram_rows_readable"] is True
    unavailable = rendered["messages_dm_storage_unavailable"]["metrics"]
    assert unavailable["dm_store_state"] == "unavailable"
    assert unavailable["dm_ram_rows_readable"] is True
    no_contact = rendered["messages_dm_no_contact"]["metrics"]
    assert no_contact["dm_no_contact"] is True
    assert no_contact["contact_source_count"] > 0
    assert no_contact["dm_capable_contact_count"] == 0
    no_history = rendered["messages_dm_no_history"]["metrics"]
    assert no_history["dm_no_history"] is True
    assert no_history["dm_capable_contact_count"] > 0
    assert rendered["messages_dm_retry"]["metrics"]["dm_retry_active"] is True
    assert rendered["messages_dm_failure"]["metrics"]["dm_failure_latched"] is True
    no_contact_thread = rendered["dm_thread_no_contact"]["metrics"]
    assert no_contact_thread["dm_thread_reply_enabled"] is False
    assert no_contact_thread["dm_thread_history_readable_without_contact"] is True
    for view in rendered.values():
        assert view["touch_target_issues"] == []
        for target in view["touch_targets"]:
            assert target["rf_tx"] is False
            assert target["public_rf_tx"] is False
            assert target["dm_tx"] is False


def test_simulator_uses_exact_store_health_and_full_delivery_source() -> None:
    snapshot = ui_simulator.sample_snapshot()
    packet_log_only_failure = ui_simulator.replace(
        snapshot,
        storage_retained_backup_degraded=True,
        message_store_persistence_degraded=False,
        dm_store_persistence_degraded=False,
    )
    assert ui_simulator.messages_store_state(
        packet_log_only_failure, direct=False
    ) == "ready"
    assert ui_simulator.messages_store_state(
        packet_log_only_failure, direct=True
    ) == "ready"

    for state in ("retry_wait", "failed_timeout"):
        state_snapshot = ui_simulator.messages_delivery_state_snapshot(
            snapshot,
            state,
            "retry_scheduled" if state == "retry_wait" else "ack_timeout",
        )
        preview = ui_simulator.dm_conversation_summaries(
            state_snapshot.dm_messages
        )[:5]
        assert all(message.delivery_state != state for message in preview)
        surface = ui_simulator.Surface("delivery-source-test")
        ui_simulator.render_messages_dm_list(surface, state_snapshot)
        metric = (
            "dm_retry_active" if state == "retry_wait" else
            "dm_failure_latched"
        )
        assert surface.metrics[metric] is True
