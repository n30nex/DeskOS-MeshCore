from dataclasses import replace

import pytest

from tools import ui_simulator


def test_ui_lifecycle_stress_executes_exactly_one_thousand_stateful_transitions():
    report = ui_simulator.run_lifecycle_stress(transitions=1000)

    assert report["ok"] is True
    assert report["artifact_kind"] == "host_simulator_ui_runtime_safety_partial"
    assert report["physical_acceptance_claimed"] is False
    assert report["rf_acceptance_claimed"] is False
    assert report["requested_transitions"] == 1000
    assert report["completed_transitions"] == 1000
    assert report["render_count"] == 1001
    assert report["render_fingerprint_checks"] == 1001
    assert report["stale_callback_rejections"] == 1000
    assert report["stale_callback_unexpected_accepts"] == 0
    assert report["modal_open_count"] - report["modal_close_count"] == report["final_modal_depth"]
    assert report["max_modal_depth"] == 2
    assert report["dock_transition_count"] > 0
    assert report["focus_entry_count"] > 0
    assert report["focus_binding_check_count"] > 0
    assert report["rf_actions_dispatched"] == 0
    assert report["destructive_actions_dispatched"] == 0
    assert report["format_actions_dispatched"] == 0
    assert report["incoming_event_probe_count"] == len(ui_simulator.EXPECTED_INCOMING_EVENT_FLOWS)
    assert report["incoming_event_report"]["ok"] is True
    assert report["incoming_event_report"]["skipped_flows"] == []
    assert report["final_state_validated"] is True
    assert report["failure_limit"] == 32
    assert report["failure_limit_hit"] is False
    assert report["failures"] == []
    assert set(report["view_activation_counts"]) >= {
        "home",
        "messages",
        "messages_public",
        "compose_sheet",
        "messages_dm",
        "dm_thread_sheet",
        "nodes",
        "contact_detail_sheet",
        "map",
        "map_options",
        "settings",
        "radio_settings_sheet",
        "wifi_setup_sheet",
        "ble_setup_sheet",
        "storage_setup_sheet",
        "diagnostics_sheet",
    }
    assert all(len(fingerprint) == 64 for fingerprint in report["render_fingerprints"].values())


def test_ui_lifecycle_dispatch_rejects_stale_and_unsafe_bindings_without_state_change():
    state, _surface, _summary, bindings, failures = ui_simulator.render_lifecycle_generation(
        ui_simulator.LifecycleState(),
        ui_simulator.sample_snapshot(),
    )
    assert failures == []
    open_messages = next(binding for binding in bindings if binding.action == "open_messages_root")
    accepted = ui_simulator.dispatch_lifecycle_binding(state, open_messages)
    assert accepted.accepted is True

    current, _surface, _summary, current_bindings, failures = ui_simulator.render_lifecycle_generation(
        accepted.state,
        ui_simulator.sample_snapshot(),
    )
    assert failures == []
    stale = ui_simulator.dispatch_lifecycle_binding(current, open_messages)
    assert stale.accepted is False
    assert stale.reason == "stale_callback"
    assert stale.state == current

    current_binding = next(binding for binding in current_bindings if binding.action == "open_messages_public")
    for unsafe_binding, expected_reason in (
        (replace(current_binding, rf_tx=True), "rf_action_forbidden"),
        (replace(current_binding, destructive=True), "destructive_action_forbidden"),
        (replace(current_binding, formats_sd=True), "format_action_forbidden"),
        (replace(current_binding, destination="missing_view"), "unknown_destination"),
    ):
        rejected = ui_simulator.dispatch_lifecycle_binding(current, unsafe_binding)
        assert rejected.accepted is False
        assert rejected.reason == expected_reason
        assert rejected.state == current


def test_ui_lifecycle_stress_returns_bounded_failure_receipt_for_missing_action():
    report = ui_simulator.run_lifecycle_stress(
        transitions=1,
        action_cycle=(("home", "missing_action"),),
    )

    assert report["ok"] is False
    assert report["completed_transitions"] == 0
    assert report["final_state_validated"] is False
    assert report["failure_limit_hit"] is False
    assert report["failures"] == [
        {
            "kind": "missing_action",
            "transition": 1,
            "view": "home",
            "action": "missing_action",
        }
    ]


def test_ui_lifecycle_stress_rejects_explicit_empty_action_cycle():
    with pytest.raises(ValueError, match="action_cycle must not be empty"):
        ui_simulator.run_lifecycle_stress(
            transitions=1,
            action_cycle=(),
        )
