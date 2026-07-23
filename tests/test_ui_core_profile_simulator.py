from pathlib import Path

import pytest

from tools import ui_simulator


def views_by_name(report: dict[str, object]) -> dict[str, dict[str, object]]:
    return {view["name"]: view for view in report["views"]}


def test_core_profile_renders_exact_root_surface_and_non_closure_truth(
    tmp_path: Path,
):
    report = ui_simulator.generate(
        tmp_path,
        release_profile=ui_simulator.CORE_RELEASE_PROFILE,
        lifecycle_transitions=len(
            ui_simulator.CORE_LIFECYCLE_TRANSITION_CYCLE
        )
        * 2,
    )

    assert report["ok"] is True
    assert report["artifact_kind"] == (
        "host_simulator_core_ui_regression_non_closure"
    )
    assert report["release_profile"] == "core_1_0"
    assert report["mode"] == "simulation"
    assert report["hardware_required"] is True
    assert report["closure_eligible"] is False
    assert report["physical_observed"] is False
    assert report["physical_acceptance_claimed"] is False
    assert report["rf_acceptance_claimed"] is False
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert [view["name"] for view in report["views"]] == list(
        ui_simulator.CORE_ROOT_VIEWS
    )
    assert sorted(path.name for path in tmp_path.glob("*.png")) == [
        f"{view}.png" for view in sorted(ui_simulator.CORE_ROOT_VIEWS)
    ]

    core = report["core_surface_report"]
    assert core["ok"] is True
    assert core["tab_count"] == 5
    assert [tab["destination"] for tab in core["tabs"]] == [
        "home",
        "messages",
        "nodes",
        "packets",
        "settings",
    ]
    assert len(core["dock_observations"]) == 4
    assert core["excluded_touch_targets"] == []
    assert core["unrejected_excluded_destinations"] == []
    assert core["public_rf_tx"] is False
    assert core["formats_sd"] is False
    assert report["lifecycle_report"]["ok"] is True
    assert report["lifecycle_report"]["release_profile"] == "core_1_0"
    assert report["lifecycle_report"]["completed_transitions"] == len(
        ui_simulator.CORE_LIFECYCLE_TRANSITION_CYCLE
    ) * 2
    assert report["lifecycle_report"]["rf_actions_dispatched"] == 0
    assert report["lifecycle_report"]["format_actions_dispatched"] == 0


def test_core_profile_hides_every_excluded_root_affordance(tmp_path: Path):
    report = ui_simulator.generate(
        tmp_path,
        release_profile=ui_simulator.CORE_RELEASE_PROFILE,
    )
    views = views_by_name(report)

    home_labels = set(views["home"]["labels"])
    assert {"Packets", "Settings", "Storage", "Internal"} <= home_labels
    assert {"Map", "Tools", "Wi-Fi", "BLE", "SD"}.isdisjoint(home_labels)

    settings_labels = set(views["settings"]["labels"])
    assert {
        "Connections",
        "Radio profile",
        "Storage",
        "Retained internal storage",
    } <= settings_labels
    assert {
        "Wi-Fi, Bluetooth, and radio",
        "Storage & maps",
        "Advanced",
    }.isdisjoint(settings_labels)

    packet_actions = {
        target["action"] for target in views["packets"]["touch_targets"]
    }
    assert "open_mesh_roles" not in packet_actions

    for view in report["views"]:
        dock = [
            target
            for target in view["touch_targets"]
            if target["kind"] == "dock_tab"
        ]
        if dock:
            assert [target["destination"] for target in dock] == [
                "home",
                "messages",
                "nodes",
                "packets",
                "settings",
            ]


def test_core_profile_rejects_excluded_destination_inventory(tmp_path: Path):
    inventory = ui_simulator.core_excluded_destination_rejections()
    assert [row["destination"] for row in inventory] == list(
        ui_simulator.CORE_EXCLUDED_DESTINATIONS
    )
    assert all(row["rejected"] is True for row in inventory)
    assert all(
        row["reason"] == "unavailable_in_release_profile"
        for row in inventory
    )
    assert set(ui_simulator.CORE_UNAVAILABLE_CAPABILITIES) <= {
        row["feature"] for row in inventory
    }

    for destination in ui_simulator.CORE_EXCLUDED_DESTINATIONS:
        with pytest.raises(
            ValueError,
            match="Core 1.0 simulator rejects unavailable/non-root views",
        ):
            ui_simulator.generate(
                tmp_path / destination,
                views=(destination,),
                release_profile=ui_simulator.CORE_RELEASE_PROFILE,
            )

        state = ui_simulator.LifecycleState(
            release_profile=ui_simulator.CORE_RELEASE_PROFILE,
        )
        binding = ui_simulator.LifecycleBinding(
            generation=0,
            view="home",
            action=f"deep_link_{destination}",
            destination=destination,
            kind="button",
            enabled=True,
            rf_tx=False,
            public_rf_tx=False,
            dm_tx=False,
            destructive=False,
            formats_sd=False,
        )
        dispatched = ui_simulator.dispatch_lifecycle_binding(state, binding)
        assert dispatched.accepted is False
        assert dispatched.reason == "unavailable_in_release_profile"
        assert dispatched.state == state


def test_full_feature_simulator_remains_the_default(tmp_path: Path):
    report = ui_simulator.generate(tmp_path, views=("home", "packets"))
    views = views_by_name(report)

    assert report["release_profile"] == "full_feature"
    assert "core_surface_report" not in report
    assert {"Map", "Tools", "Wi-Fi", "BLE", "SD"} <= set(
        views["home"]["labels"]
    )
    packet_actions = {
        target["action"] for target in views["packets"]["touch_targets"]
    }
    assert "open_mesh_roles" in packet_actions
