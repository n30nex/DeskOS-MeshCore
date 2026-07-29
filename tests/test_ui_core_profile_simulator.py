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
        "map",
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
    graph = report["core_navigation_graph_report"]
    assert graph["ok"] is True
    assert graph["mode"] == "simulation"
    assert graph["closure_eligible"] is False
    assert graph["physical_observed"] is False
    assert graph["excluded_touch_targets"] == []
    assert graph["dead_touch_targets"] == []
    assert graph["format_touch_targets"] == []
    assert all(
        claim["advert_location"] is not None
        for claim in graph["location_claims"]
    )
    assert graph["trapped_states"] == []
    assert graph["rf_actions_dispatched"] == 0
    assert graph["format_actions_dispatched"] == 0


def test_core_profile_exposes_production_root_affordances(tmp_path: Path):
    report = ui_simulator.generate(
        tmp_path,
        release_profile=ui_simulator.CORE_RELEASE_PROFILE,
    )
    views = views_by_name(report)

    home_labels = set(views["home"]["labels"])
    assert {"Map", "Tools", "Wi-Fi", "SD"} <= home_labels
    assert "BLE" not in home_labels

    settings_labels = set(views["settings"]["labels"])
    assert {
        "Settings",
        "Tools",
        "Packets",
        "Diagnostics",
        "Connections",
        "Wi-Fi",
    } <= settings_labels
    assert "Bluetooth" not in settings_labels

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
                "map",
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
    assert "settings_advanced_expanded" not in (
        ui_simulator.CORE_EXCLUDED_DESTINATIONS
    )

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


def test_core_profile_all_dispatch_accepted_renderer_aliases_render():
    snapshot = ui_simulator.project_core_snapshot(ui_simulator.sample_snapshot())
    state = ui_simulator.LifecycleState(
        generation=1,
        release_profile=ui_simulator.CORE_RELEASE_PROFILE,
    )

    for destination, renderer in ui_simulator.RENDERERS.items():
        binding = ui_simulator.LifecycleBinding(
            generation=1,
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
        if destination in ui_simulator.CORE_EXCLUDED_DESTINATIONS:
            assert dispatched.accepted is False
            assert dispatched.reason == "unavailable_in_release_profile"
            continue

        assert dispatched.accepted is True
        surface = ui_simulator.Surface(
            destination,
            release_profile=ui_simulator.CORE_RELEASE_PROFILE,
        )
        renderer(surface, snapshot)
        assert all(
            not target["enabled"]
            or target["destination"]
            not in ui_simulator.CORE_EXCLUDED_DESTINATIONS
            for target in surface.touch_targets
        )
        assert all(
            target["formats_sd"] is False
            for target in surface.touch_targets
        )
        alias_state = ui_simulator.LifecycleState(
            current_view=destination,
            active_tab="home",
            generation=1,
            release_profile=ui_simulator.CORE_RELEASE_PROFILE,
        )
        for target in surface.touch_targets:
            if (
                not target["enabled"]
                or target["destination"] is None
                or target["rf_tx"]
                or target["destructive"]
                or target["formats_sd"]
            ):
                continue
            alias_dispatch = ui_simulator.dispatch_lifecycle_binding(
                alias_state,
                ui_simulator.lifecycle_binding(
                    1,
                    destination,
                    target,
                ),
            )
            assert alias_dispatch.accepted is True


def test_core_profile_exposes_trace_and_location_but_omits_export():
    snapshot = ui_simulator.project_core_snapshot(
        ui_simulator.SCENARIOS["map-ready"]()
    )
    assert any(
        node.advert_lat_e6 is not None
        and node.advert_lon_e6 is not None
        and node.location_advert_timestamp > 0
        for node in snapshot.heard
    )

    options = ui_simulator.Surface(
        "contact_options_page",
        release_profile=ui_simulator.CORE_RELEASE_PROFILE,
    )
    ui_simulator.RENDERERS["contact_options_page"](options, snapshot)
    option_actions = {target["action"] for target in options.touch_targets}
    option_destinations = {
        target["destination"]
        for target in options.touch_targets
        if target["destination"]
    }
    assert "open_route_trace" in option_actions
    assert "route_trace_sheet" in option_destinations
    assert "open_contact_export" not in option_actions
    assert "contact_export_sheet" not in option_destinations
    assert {"Route", "Trace path"} <= set(options.labels)
    assert {"Export", "Share QR"}.isdisjoint(set(options.labels))

    node_detail = ui_simulator.Surface(
        "node_detail_sheet",
        release_profile=ui_simulator.CORE_RELEASE_PROFILE,
    )
    ui_simulator.RENDERERS["node_detail_sheet"](node_detail, snapshot)
    close = next(
        target
        for target in node_detail.touch_targets
        if target["action"] == "close_node_detail"
    )
    assert close["destination"] == "map"
    assert any(label.startswith("Advert location ") for label in node_detail.labels)
    assert node_detail.metrics["node_detail_advert_location"] is not None
    assert node_detail.metrics["node_detail_location_provenance"] == "advert"


@pytest.mark.parametrize("scenario", tuple(ui_simulator.SCENARIOS))
def test_core_profile_reachable_graph_is_safe_in_every_scenario(scenario: str):
    report = ui_simulator.build_core_navigation_graph_report(
        ui_simulator.SCENARIOS[scenario](),
        scenario=scenario,
    )

    assert report["ok"] is True
    assert report["scenario"] == scenario
    assert report["render_errors"] == []
    assert report["render_invariant_issues"] == []
    assert report["excluded_touch_targets"] == []
    assert report["dead_touch_targets"] == []
    assert report["format_touch_targets"] == []
    assert all(
        claim["advert_location"] is not None
        for claim in report["location_claims"]
    )
    assert report["trapped_states"] == []
    assert "map" in report["rendered_views"]
    assert report["rf_actions_dispatched"] == 0
    assert report["format_actions_dispatched"] == 0


def test_core_profile_map_ready_lifecycle_covers_map_navigation():
    report = ui_simulator.run_lifecycle_stress(
        ui_simulator.SCENARIOS["map-ready"](),
        transitions=len(ui_simulator.CORE_LIFECYCLE_TRANSITION_CYCLE),
        scenario="map-ready",
        release_profile=ui_simulator.CORE_RELEASE_PROFILE,
    )

    assert report["ok"] is True
    assert report["completed_transitions"] == len(
        ui_simulator.CORE_LIFECYCLE_TRANSITION_CYCLE
    )
    assert report["failures"] == []


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
