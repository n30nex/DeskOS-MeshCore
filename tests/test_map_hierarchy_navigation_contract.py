from pathlib import Path

from scripts import scroll_probe_d1l


ROOT = Path(__file__).resolve().parents[1]
MAP_SURFACES = ("map", "map_options", "map_location", "map_cache")


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def test_map_probe_aliases_normalize_to_canonical_map_surfaces():
    navigation = read("main/ui/ui_navigation.c")
    mapping = function_body(
        navigation,
        "bool d1l_ui_scroll_surface_from_name",
        "void d1l_ui_navigation_request",
    )

    for surface in MAP_SURFACES:
        branch = mapping.split(f'surface = "{surface}";', 1)[0].rsplit("} else", 1)[-1]
        assert f'strcmp(normalized, "{surface}")' in branch
        assert "screen = D1L_UI_SCREEN_MAP;" in mapping.split(
            f'surface = "{surface}";', 1
        )[1].split("} else", 1)[0]

    for alias in ("map_options_page", "map_location_page", "map_cache_page"):
        assert f'strcmp(normalized, "{alias}")' in mapping

    for removed in ("offline_maps", "map_tiles", "map_tile_source", "remove_source_confirm"):
        assert f'strcmp(normalized, "{removed}")' not in mapping


def test_console_advertises_only_network_suppressed_map_page_probes():
    console = read("main/comms/usb_console.c")
    probe = function_body(console, "static void cmd_ui_scroll_probe", "static void cmd_ui_compose_probe")
    help_body = function_body(console, "static void cmd_help", "static void handle_line")

    assert (
        '\\"map_probe_surfaces\\":[\\"map\\",\\"map_options\\",'
        '\\"map_location\\",\\"map_cache\\"]'
    ) in help_body
    assert '\\"map_probes_read_only\\":true' in help_body
    for surface in MAP_SURFACES:
        assert surface in probe

    assert "d1l_ui_phase1_scroll_probe(surface, &probe)" in probe
    for forbidden in (
        "d1l_app_model_",
        "d1l_connectivity_",
        "d1l_storage_",
        "d1l_meshcore_",
        "map_tile_download",
        "map_center_set",
        "map_center_clear",
        "crashlog clear",
    ):
        assert forbidden not in probe


def test_map_probe_script_is_static_read_only_and_never_opens_live_map_first():
    aliases = scroll_probe_d1l.parse_screens(
        "map,map-options,map-location,map-cache"
    )
    assert aliases == list(MAP_SURFACES)

    report = scroll_probe_d1l.dry_run_report(aliases, dwell_sec=0.0, manual_touch=False)
    assert report["ok"] is True
    assert report["read_only"] is True
    assert report["save_actions"] is False
    assert report["clear_actions"] is False
    assert report["map_download"] is False
    assert report["network_tx"] is False
    assert report["map_network_requests"] is False
    assert report["background_download"] is False
    assert report["area_download"] is False
    assert report["visible_tile_limit"] == 9
    assert report["zoom_batch_limit"] == 1
    assert report["wifi_mutation"] is False
    assert report["rf_tx"] is False
    assert report["storage_mutation"] is False
    assert report["formats_sd"] is False
    assert [item["screen"] for item in report["surface_plan"]] == list(MAP_SURFACES)
    assert {item["tab"] for item in report["surface_plan"]} == {"map"}

    expected_commands = ["map tiles status"]
    expected_commands.extend(f"ui scroll-probe {surface}" for surface in MAP_SURFACES)
    expected_commands.extend(("map tiles status", "ui status", "health", "crashlog"))
    assert report["commands"] == expected_commands
    assert report["map_network_evidence"]["measured"] is False
    assert report["map_network_evidence"]["declared_no_network"] is True
    assert "ui tab map" not in report["commands"]


def test_static_map_probe_results_do_not_require_synthetic_scroll_movement():
    for surface in MAP_SURFACES:
        event = {
            "screen": surface,
            "tab": "map",
            "request": {"ok": True},
            "tab_active": True,
            "probe": {
                "ok": True,
                "surface": surface,
                "tab": "map",
                "target_found": True,
                "scrollable": False,
                "moved": False,
            },
            "status": {"active_tab": "map"},
            "health": {"ok": True},
            "crashlog": {"entries": []},
        }
        assert surface in scroll_probe_d1l.SCROLL_MOVEMENT_OPTIONAL
        assert scroll_probe_d1l.event_failed(event) is False


def test_map_probe_rejects_clear_setup_action():
    scroll_probe_d1l.validate_setup_actions(list(MAP_SURFACES), False)
    try:
        scroll_probe_d1l.validate_setup_actions(["map_cache"], True)
    except ValueError as exc:
        assert "network-suppressed Map probes" in str(exc)
    else:
        raise AssertionError("Map probe accepted a crashlog Clear setup action")

    scroll_probe_d1l.validate_setup_actions(["packets"], True)


def test_map_docs_define_the_bounded_interactive_policy_and_probe_safety():
    guide = read("docs/USER_GUIDE_D1L.md")
    test_plan = read("docs/TEST_PLAN_D1L.md").split("## Historical", 1)[0]
    roadmap = read("docs/ROADMAP.md")
    attributions = read("docs/ATTRIBUTIONS.md")

    assert "visible current-view 3×3" in guide
    assert "at one zoom per visible generation" in guide
    assert "Completed tiles are reused from SD" in guide
    assert "Read-only Map probes" in guide
    assert "never request tiles" in guide
    assert "Authorized-provider background prefetch" in guide
    assert "signed nodes no farther" in guide
    assert "200 km" in guide
    assert "8–18" in guide
    assert "at most 60%" in guide
    assert "at least 8 GiB" in guide
    assert "(c) OpenStreetMap contributors" in guide
    assert "one Map source" in " ".join(test_plan.split())
    assert "Authorized fresh tile download plus cache revisit from SD" in roadmap
    assert "example manifest intentionally uses a non-routable placeholder" in attributions
    assert "must not be pointed at OpenStreetMap Standard" in attributions
