from pathlib import Path

from tools import ui_simulator


MORE_VIEWS = (
    "settings",
    "settings_tools_expanded",
    "settings_connections_expanded",
    "settings_storage_maps_expanded",
    "settings_device_expanded",
    "settings_support_expanded",
    "settings_advanced_expanded",
)

SCENARIOS = (
    "storage-degraded",
    "more-connectivity-ready",
    "more-connectivity-applying",
    "more-long-labels",
)


def view_map(report: dict[str, object]) -> dict[str, dict[str, object]]:
    return {str(view["name"]): view for view in report["views"]}


def test_more_scenarios_are_deterministic_bounded_and_side_effect_free(tmp_path: Path):
    reports: dict[str, dict[str, object]] = {}
    for scenario in SCENARIOS:
        first = ui_simulator.generate(
            tmp_path / scenario / "first", MORE_VIEWS, scenario=scenario
        )
        second = ui_simulator.generate(
            tmp_path / scenario / "second", MORE_VIEWS, scenario=scenario
        )
        reports[scenario] = first

        assert first["ok"] is True
        assert first["scenario"] == scenario
        assert first["overflow_count"] == 0
        assert first["touch_target_issue_count"] == 0
        assert first["sibling_text_overlap_count"] == 0
        assert first["required_labels_missing"] == []
        assert first["dock_invariant_issues"] == []
        assert first["flow_report"]["target_overlaps"] == []
        assert first["flow_report"]["format_actions"] == []
        assert first["flow_report"]["rf_actions"] == []
        assert first["flow_report"]["public_rf_actions"] == []

        first_views = view_map(first)
        second_views = view_map(second)
        assert set(first_views) == set(MORE_VIEWS)
        for view_name in MORE_VIEWS:
            first_view = first_views[view_name]
            second_view = second_views[view_name]
            assert first_view["labels"] == second_view["labels"]
            assert first_view["touch_targets"] == second_view["touch_targets"]
            assert first_view["metrics"] == second_view["metrics"]
            assert first_view["dock_target_count"] == 5
            assert first_view["dock_invariant_ok"] is True
            assert first_view["overflow"] == []
            assert first_view["touch_target_issues"] == []

    degraded = view_map(reports["storage-degraded"])
    storage_labels = degraded["settings_storage_maps_expanded"]["labels"]
    assert "SD Card" in storage_labels
    assert "Needs attention" in storage_labels

    ready = view_map(reports["more-connectivity-ready"])
    assert {"Connected", "On", "Not configured"} <= set(
        ready["settings_connections_expanded"]["labels"]
    )

    applying = view_map(reports["more-connectivity-applying"])
    assert {"Connecting", "Off", "Not configured"} <= set(
        applying["settings_connections_expanded"]["labels"]
    )

    long_labels = view_map(reports["more-long-labels"])
    about = long_labels["settings_support_expanded"]
    assert any(label.startswith("Version 1.0.0+exact-commit") for label in about["labels"])
    assert about["truncated_labels"]
