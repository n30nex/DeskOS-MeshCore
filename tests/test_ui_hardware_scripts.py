from pathlib import Path

import pytest

from scripts import (
    scroll_probe_d1l,
    smoke_d1l,
    ui_capture_d1l,
    ui_capture_diff_d1l,
    ui_compose_keyboard_capture_d1l,
    ui_corruption_probe_d1l,
)

ROOT = Path(__file__).resolve().parents[1]
PUBLIC_KEY = "ab" * 32


class FakeSerial:
    def __init__(self, responses: list[bytes]):
        self.responses = list(responses)
        self.timeout = 99.0
        self.seen_timeouts: list[float | None] = []

    def readline(self) -> bytes:
        self.seen_timeouts.append(self.timeout)
        if self.responses:
            return self.responses.pop(0)
        return b""


def test_ui_corruption_probe_dry_run_is_explicit_port_safe():
    report = ui_corruption_probe_d1l.dry_run_report(
        rounds=20,
        settle_sec=0.1,
        clear_crashlog_before_start=True,
        skip_data_canary=False,
    )

    assert report["ok"] is True
    assert report["mode"] == "dry-run"
    assert report["hardware_required"] is False
    assert report["explicit_port_required"] is True
    assert report["kind"] == "ui_corruption_probe"
    assert report["rounds"] == 20
    assert report["release_min_rounds"] == 20
    assert report["public_rf_tx"] is False
    assert report["network_tx"] is False
    assert report["map_network_requests"] is False
    assert report["formats_sd"] is False
    assert "heap_free" in report["telemetry_fields"]
    assert "ui_task_stack_free_words" in report["telemetry_fields"]
    assert report["tabs"] == ["home", "messages", "nodes", "map", "packets", "settings"]
    assert "ui status" in report["commands"]
    assert "ui tab packets" in report["commands"]
    assert "ui scroll-probe map" in report["commands"]
    assert "ui tab map" not in report["commands"]
    assert "ui data-canary uiRx0001" in report["commands"]
    assert report["commands"].count("map tiles status") == 2
    assert report["map_network_evidence"]["measured"] is False
    assert report["map_network_evidence"]["declared_no_network"] is True
    assert report["checks"]["data_refresh_exercised"] is True
    assert report["checks"]["no_stuck_pending"] is True
    assert report["checks"]["final_active_tab_known"] is True


def test_ui_corruption_probe_uses_nested_refresh_status_for_final_pending():
    report = ui_corruption_probe_d1l.summarize_telemetry(
        [
            {
                "kind": "tab",
                "statuses": [{"ok": True, "active_tab": "settings", "pending": False}],
                "health": {"ok": True, "uptime_ms": 1000},
            },
            {
                "kind": "data_refresh",
                "packet_tab": {
                    "statuses": [{"ok": True, "active_tab": "packets", "pending": False}],
                },
                "messages_tab": {
                    "statuses": [{"ok": True, "active_tab": "messages", "pending": False}],
                },
                "status": {"ok": True, "active_tab": "messages", "pending": False},
                "health": {"ok": True, "uptime_ms": 1200},
            },
        ]
    )

    assert report["uptime_monotonic"] is True
    assert report["final_active_tab"] == "messages"
    assert report["final_pending"] is False


def test_ui_corruption_probe_requires_token_in_result_entries():
    assert ui_corruption_probe_d1l.result_entries_contain_token(
        {"filter": {"search": "uiRx0001"}, "entries": []},
        "uiRx0001",
    ) is False
    assert ui_corruption_probe_d1l.result_entries_contain_token(
        {"entries": [{"note": "ui-canary uiRx0001"}]},
        "uiRx0001",
    ) is True


def test_scroll_probe_dry_run_and_screen_parser():
    screens = scroll_probe_d1l.parse_screens(
        "home,public,dm-thread,nodes,packets,settings,storage,storage-card,storage-data,wi-fi,map,map-options,map-location,map-cache"
    )
    report = scroll_probe_d1l.dry_run_report(screens, dwell_sec=0.5, manual_touch=True)

    assert screens == [
        "home",
        "public_messages",
        "dm_thread",
        "nodes",
        "packets",
        "settings",
        "storage",
        "storage_card",
        "storage_data",
        "wifi",
        "map",
        "map_options",
        "map_location",
        "map_cache",
    ]
    assert report["ok"] is True
    assert report["mode"] == "dry-run"
    assert report["explicit_port_required"] is True
    assert report["manual_touch"] is True
    assert "release_profile" not in report
    assert "scroll_movement_policy" not in report
    assert {"screen": "storage", "tab": "settings", "label": "Storage overview"} in report["surface_plan"]
    assert {"screen": "storage_card", "tab": "settings", "label": "SD card status"} in report["surface_plan"]
    assert {"screen": "storage_data", "tab": "settings", "label": "Data locations"} in report["surface_plan"]
    assert {"screen": "dm_thread", "tab": "messages", "label": "DM thread"} in report["surface_plan"]
    assert "ui tab messages" in report["commands"]
    assert "ui scroll-probe storage" in report["commands"]
    assert "ui scroll-probe storage_card" in report["commands"]
    assert "ui scroll-probe storage_data" in report["commands"]
    assert "ui scroll-probe dm_thread" in report["commands"]
    assert "ui scroll-probe map_options" in report["commands"]
    assert "ui scroll-probe map_location" in report["commands"]
    assert "ui scroll-probe map_cache" in report["commands"]
    assert "ui tab map" not in report["commands"]
    assert report["network_tx"] is False
    assert report["map_network_requests"] is False
    assert report["visible_tile_limit"] == 9
    assert report["zoom_batch_limit"] == 1
    assert report["commands"].count("map tiles status") == 2
    assert report["map_network_evidence"]["measured"] is False
    assert report["map_network_evidence"]["declared_no_network"] is True
    assert "crashlog" in report["commands"]


def test_map_network_counter_evidence_requires_two_equal_valid_samples():
    before = {"ok": True, "network_requests": 4}
    same = {"ok": True, "network_requests": 4}
    grew = {"ok": True, "network_requests": 5}

    evidence = scroll_probe_d1l.summarize_map_network_evidence(
        before, same, measured=True
    )
    assert evidence["samples_valid"] is True
    assert evidence["unchanged"] is True
    assert evidence["delta"] == 0
    assert evidence["before"] is before
    assert evidence["after"] is same

    evidence = scroll_probe_d1l.summarize_map_network_evidence(
        before, grew, measured=True
    )
    assert evidence["samples_valid"] is True
    assert evidence["unchanged"] is False
    assert evidence["delta"] == 1

    evidence = scroll_probe_d1l.summarize_map_network_evidence(
        before, {"ok": False}, measured=True
    )
    assert evidence["samples_valid"] is False
    assert evidence["unchanged"] is False
    assert evidence["delta"] is None


def test_scroll_probe_rejects_unknown_screen():
    try:
        scroll_probe_d1l.parse_screens("messages,unknown")
    except ValueError as exc:
        assert "unknown" in str(exc)
    else:
        raise AssertionError("parse_screens accepted an unknown screen")


def test_scroll_probe_allows_static_home_and_storage_summary_but_requires_data_to_move():
    def event(screen: str) -> dict:
        tab = "home" if screen == "home" else "settings"
        return {
            "screen": screen,
            "tab": tab,
            "request": {"ok": True},
            "tab_active": True,
            "probe": {
                "ok": screen != "home",
                "surface": screen,
                "tab": tab,
                "target_found": True,
                "scrollable": screen == "storage_data",
                "moved": False,
            },
            "status": {"active_tab": tab},
            "health": {"ok": True},
            "crashlog": {"entries": []},
        }

    assert scroll_probe_d1l.event_failed(event("home")) is False
    assert scroll_probe_d1l.event_failed(event("storage")) is False
    assert scroll_probe_d1l.event_failed(event("storage_card")) is False
    assert scroll_probe_d1l.event_failed(event("storage_data")) is True

    moving_data = event("storage_data")
    moving_data["probe"]["moved"] = True
    assert scroll_probe_d1l.event_failed(moving_data) is False


def test_scroll_probe_core_profile_recomputes_positive_overflow():
    def event(screen: str, bottom: int = 0, after_y: int = 0) -> dict:
        tab = scroll_probe_d1l.SCROLL_SURFACES[screen]["tab"]
        moved = after_y != 0 or bottom > 0
        return {
            "screen": screen,
            "tab": tab,
            "request": {"ok": True},
            "tab_active": True,
            "probe": {
                "schema": 1,
                "ok": True,
                "cmd": "ui scroll-probe",
                "surface": screen,
                "tab": tab,
                "surface_supported": True,
                "target_found": True,
                "scrollable": True,
                "movement_required": bottom > 0,
                "moved": moved,
                "before_y": 0,
                "after_y": after_y,
                "scroll_top_before": 0,
                "scroll_bottom_before": bottom,
                "scroll_top_after": after_y,
                "scroll_bottom_after": 0 if bottom > 0 else bottom,
            },
            "status": {"active_tab": tab},
            "health": {
                "ok": True,
                "cmd": "health",
                "build_commit": "a" * 40,
                "release_profile": "core_1_0",
                "sd_history_mode": "conditional",
            },
            "crashlog": {"entries": []},
        }

    for screen in ("home", "dm_thread", "settings"):
        compact = event(screen)
        assert scroll_probe_d1l.event_failed(
            compact, "core_1_0", "a" * 40, "conditional"
        ) is False
    compact_not_scrollable = event("settings")
    compact_not_scrollable["probe"]["scrollable"] = False
    assert scroll_probe_d1l.event_failed(
        compact_not_scrollable,
        "core_1_0",
        "a" * 40,
        "conditional",
    ) is False
    assert scroll_probe_d1l.event_failed(
        event("dm_thread"), "full_feature"
    ) is True

    moving = event("public_messages", bottom=6, after_y=6)
    assert scroll_probe_d1l.event_failed(
        moving, "core_1_0", "a" * 40, "conditional"
    ) is False

    blocked = event("public_messages", bottom=6, after_y=0)
    blocked["probe"]["moved"] = False
    blocked["probe"]["scroll_top_after"] = 0
    blocked["probe"]["scroll_bottom_after"] = 6
    assert scroll_probe_d1l.event_failed(
        blocked, "core_1_0", "a" * 40, "conditional"
    ) is True

    forged = event("settings")
    forged["probe"]["movement_required"] = True
    assert scroll_probe_d1l.event_failed(
        forged, "core_1_0", "a" * 40, "conditional"
    ) is True

    invalid_scrollable = event("settings")
    invalid_scrollable["probe"]["scrollable"] = None
    assert scroll_probe_d1l.event_failed(
        invalid_scrollable,
        "core_1_0",
        "a" * 40,
        "conditional",
    ) is True

    wrong_profile = event("settings")
    wrong_profile["health"]["release_profile"] = "full_feature"
    assert scroll_probe_d1l.event_failed(
        wrong_profile, "core_1_0", "a" * 40, "conditional"
    ) is True

    report = scroll_probe_d1l.dry_run_report(
        list(scroll_probe_d1l.CORE_SCROLL_SEQUENCE),
        dwell_sec=0.0,
        manual_touch=False,
        release_profile="core_1_0",
    )
    assert report["release_profile"] == "core_1_0"
    assert report["scroll_movement_policy"] == "positive_raw_overflow"
    assert report["screens"] == [
        "home",
        "public_messages",
        "dm_thread",
        "nodes",
        "packets",
        "settings",
        "storage",
        "wifi",
        "map",
        "map_options",
        "map_location",
        "map_cache",
    ]

    try:
        scroll_probe_d1l.validate_release_profile_screens(
            ["home", "map"], "core_1_0"
        )
    except ValueError as exc:
        assert "exact ordered surfaces" in str(exc)
    else:
        raise AssertionError("Core scroll accepted a partial surface list")

    for invalid in (
        [],
        ["home"],
        list(reversed(scroll_probe_d1l.CORE_SCROLL_SEQUENCE)),
    ):
        try:
            scroll_probe_d1l.validate_release_profile_screens(
                invalid, "core_1_0"
            )
        except ValueError as exc:
            assert "exact ordered surfaces" in str(exc)
        else:
            raise AssertionError(
                f"Core scroll accepted non-exact sequence: {invalid}"
            )


def test_scroll_probe_core_evidence_inputs_fail_closed(
    monkeypatch, tmp_path
):
    clean = {
        "commit": "a" * 40,
        "short_commit": "a" * 7,
        "branch": "release/24h-core",
        "dirty": False,
        "dirty_entries": [],
    }
    monkeypatch.setattr(
        scroll_probe_d1l, "git_metadata", lambda root: clean
    )
    validated = scroll_probe_d1l.validate_core_evidence_inputs(
        port="COM12",
        expected_firmware_commit="a" * 40,
        github_actions_run="123",
        workflow_run_attempt="1",
        expected_sd_history_mode="conditional",
        root=tmp_path,
    )
    assert validated["port"] == "COM12"
    assert validated["expected_firmware_commit"] == "a" * 40
    assert validated["expected_sd_history_mode"] == "conditional"

    with pytest.raises(ValueError, match="conditional SD history mode"):
        scroll_probe_d1l.validate_core_evidence_inputs(
            port="COM12",
            expected_firmware_commit="a" * 40,
            github_actions_run="123",
            workflow_run_attempt="1",
            expected_sd_history_mode="disabled",
            root=tmp_path,
        )

    for forbidden in ("COM8", "COM11", "COM29", "COM16"):
        try:
            scroll_probe_d1l.validate_core_evidence_inputs(
                port=forbidden,
                expected_firmware_commit="a" * 40,
                github_actions_run="123",
                workflow_run_attempt="1",
                expected_sd_history_mode="conditional",
                root=tmp_path,
            )
        except ValueError as exc:
            assert "requires COM12" in str(exc)
        else:
            raise AssertionError(f"Core scroll accepted {forbidden}")

    monkeypatch.setattr(
        scroll_probe_d1l,
        "git_metadata",
        lambda root: {**clean, "dirty": True, "dirty_entries": [" M x"]},
    )
    try:
        scroll_probe_d1l.validate_core_evidence_inputs(
            port="COM12",
            expected_firmware_commit="a" * 40,
            github_actions_run="123",
            workflow_run_attempt="1",
            expected_sd_history_mode="conditional",
            root=tmp_path,
        )
    except ValueError as exc:
        assert "clean exact candidate source" in str(exc)
    else:
        raise AssertionError("Core scroll accepted dirty source")


def test_scroll_probe_core_identity_preflight_precedes_mutation(
    monkeypatch
):
    class SerialContext:
        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, traceback):
            return False

        def reset_input_buffer(self):
            pass

    clean = {
        "commit": "a" * 40,
        "short_commit": "a" * 7,
        "branch": "release/24h-core",
        "dirty": False,
        "dirty_entries": [],
    }
    commands = []

    def response(_ser, command, _timeout):
        commands.append(command)
        return {
            "schema": 1,
            "ok": True,
            "cmd": command,
            "build_commit": "b" * 40,
            "release_profile": "core_1_0",
            "sd_history_mode": "conditional",
            "idf": "v5.5.4",
        }

    monkeypatch.setattr(
        scroll_probe_d1l, "git_metadata", lambda root: clean
    )
    monkeypatch.setattr(
        scroll_probe_d1l,
        "open_d1l_serial",
        lambda *args, **kwargs: SerialContext(),
    )
    monkeypatch.setattr(
        scroll_probe_d1l, "send_console_command", response
    )
    monkeypatch.setattr(scroll_probe_d1l.time, "sleep", lambda _value: None)

    report = scroll_probe_d1l.run_scroll_probe(
        port="COM12",
        baud=115200,
        timeout=1.0,
        screens=list(scroll_probe_d1l.CORE_SCROLL_SEQUENCE),
        dwell_sec=0.0,
        manual_touch=False,
        clear_crashlog_before_start=True,
        poll_sec=0.01,
        release_profile="core_1_0",
        expected_firmware_commit="a" * 40,
        github_actions_run="123",
        workflow_run_attempt="1",
        expected_sd_history_mode="conditional",
        expected_d1l_public_key=PUBLIC_KEY,
        platform_name="nt",
        port_lister=lambda: [
            {
                "device": "COM12",
                "vid": 0x1A86,
                "pid": 0x7523,
                "serial_number": None,
                "hwid": "USB VID:PID=1A86:7523 LOCATION=1-2",
                "location": "1-2",
            }
        ],
    )
    assert commands == ["version"]
    assert report["ok"] is False
    assert report["closure_eligible"] is False
    assert report["clear_crashlog_before_start"] is False
    assert report["clear_crashlog_requested"] is True
    assert report["firmware_identity_ok"] is False
    assert report["events"] == []
    assert report["schema"] == 2
    assert report["d1l_target"]["requested_path"] == "COM12"


def test_scroll_probe_invalid_core_target_fails_before_serial_open(
    monkeypatch,
):
    clean = {
        "commit": "a" * 40,
        "short_commit": "a" * 7,
        "branch": "release/24h-core",
        "dirty": False,
        "dirty_entries": [],
    }
    monkeypatch.setattr(
        scroll_probe_d1l, "git_metadata", lambda _root: clean
    )
    monkeypatch.setattr(
        scroll_probe_d1l,
        "open_d1l_serial",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            AssertionError("invalid target must fail before serial open")
        ),
    )

    with pytest.raises(ValueError, match="VID"):
        scroll_probe_d1l.run_scroll_probe(
            port="COM12",
            baud=115200,
            timeout=1.0,
            screens=list(scroll_probe_d1l.CORE_SCROLL_SEQUENCE),
            dwell_sec=0.0,
            manual_touch=False,
            clear_crashlog_before_start=False,
            poll_sec=0.01,
            release_profile="core_1_0",
            expected_firmware_commit="a" * 40,
            github_actions_run="123",
            workflow_run_attempt="1",
            expected_sd_history_mode="conditional",
            expected_d1l_public_key=PUBLIC_KEY,
            platform_name="nt",
            port_lister=lambda: [
                {
                    "device": "COM12",
                    "vid": 0x10C4,
                    "pid": 0xEA60,
                    "serial_number": "wrong",
                    "hwid": "wrong",
                    "location": "1-9",
                }
            ],
        )


def test_scroll_probe_wrong_full_key_stops_before_health_or_mutation(
    monkeypatch,
):
    class SerialContext:
        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, traceback):
            return False

        def reset_input_buffer(self):
            pass

    commit = "a" * 40
    wrong_key = PUBLIC_KEY[:16] + "cd" * 24
    commands = []

    monkeypatch.setattr(
        scroll_probe_d1l,
        "git_metadata",
        lambda _root: {
            "commit": commit,
            "short_commit": commit[:7],
            "branch": "release/24h-core",
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(
        scroll_probe_d1l,
        "open_d1l_serial",
        lambda *_args, **_kwargs: SerialContext(),
    )

    def response(_ser, command, _timeout):
        commands.append(command)
        if command == "version":
            return {
                "schema": 1,
                "ok": True,
                "cmd": "version",
                "build_commit": commit,
                "release_profile": "core_1_0",
                "sd_history_mode": "conditional",
                "idf": "v5.5.4",
            }
        if command == "identity status":
            return {
                "schema": 1,
                "ok": True,
                "cmd": "identity status",
                "public_key_ready": True,
                "public_key": wrong_key,
                "fingerprint": wrong_key[:16].upper(),
                "role": "desk_companion",
            }
        pytest.fail(f"unexpected command after identity mismatch: {command}")

    monkeypatch.setattr(
        scroll_probe_d1l, "send_console_command", response
    )
    monkeypatch.setattr(
        scroll_probe_d1l.time, "sleep", lambda _value: None
    )

    report = scroll_probe_d1l.run_scroll_probe(
        port="COM12",
        baud=115200,
        timeout=1.0,
        screens=list(scroll_probe_d1l.CORE_SCROLL_SEQUENCE),
        dwell_sec=0.0,
        manual_touch=False,
        clear_crashlog_before_start=True,
        poll_sec=0.01,
        release_profile="core_1_0",
        expected_firmware_commit=commit,
        github_actions_run="123",
        workflow_run_attempt="1",
        expected_sd_history_mode="conditional",
        expected_d1l_public_key=PUBLIC_KEY,
        platform_name="nt",
        port_lister=lambda: [
            {
                "device": "COM12",
                "vid": 0x1A86,
                "pid": 0x7523,
                "serial_number": None,
                "hwid": "USB VID:PID=1A86:7523 LOCATION=1-2",
                "location": "1-2",
            }
        ],
    )

    assert commands == ["version", "identity status"]
    assert report["expected_d1l_public_key"] == PUBLIC_KEY
    assert report["d1l_identity_status"]["public_key"] == wrong_key
    assert report["d1l_identity_ok"] is False
    assert report["identity_preflight_only"] is True
    assert report["clear_crashlog_before_start"] is False
    assert report["clear_crashlog_requested"] is True
    assert report["events"] == []
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert report["closure_eligible"] is False
    assert report["ok"] is False


def test_scroll_probe_rejects_missing_key_before_source_or_serial_io(
    monkeypatch,
):
    monkeypatch.setattr(
        scroll_probe_d1l,
        "git_metadata",
        lambda _root: pytest.fail("source access must not begin"),
    )
    monkeypatch.setattr(
        scroll_probe_d1l,
        "open_d1l_serial",
        lambda *_args, **_kwargs: pytest.fail("serial must not open"),
    )

    with pytest.raises(ValueError, match="exact 64-hex"):
        scroll_probe_d1l.run_scroll_probe(
            port="COM12",
            baud=115200,
            timeout=1.0,
            screens=list(scroll_probe_d1l.CORE_SCROLL_SEQUENCE),
            dwell_sec=0.0,
            manual_touch=False,
            clear_crashlog_before_start=False,
            poll_sec=0.01,
            release_profile="core_1_0",
            expected_firmware_commit="a" * 40,
            github_actions_run="123",
            workflow_run_attempt="1",
            expected_sd_history_mode="conditional",
            expected_d1l_public_key="short",
        )


def test_scroll_probe_ignores_normal_power_on_history_but_rejects_crash_like_resets():
    event = {
        "screen": "map",
        "tab": "map",
        "request": {"ok": True},
        "tab_active": True,
        "probe": {
            "ok": True,
            "surface": "map",
            "tab": "map",
            "target_found": True,
            "scrollable": True,
            "moved": False,
        },
        "status": {"active_tab": "map"},
        "health": {"ok": True},
        "crashlog": {
            "entries": [
                {"reset_reason": "POWERON", "crash_like": False},
                {"reset_reason": "SW", "crash_like": False},
            ]
        },
    }

    assert scroll_probe_d1l.event_failed(event) is False

    event["crashlog"]["entries"].append(
        {"reset_reason": "PANIC", "crash_like": True}
    )
    assert scroll_probe_d1l.event_failed(event) is True


def test_active_release_docs_require_one_bounded_gate_without_soak():
    release_docs = [
        (ROOT / rel).read_text(encoding="utf-8").split("## Historical", 1)[0]
        for rel in (
            "docs/ROADMAP.md",
            "docs/KNOWN_LIMITATIONS.md",
            "docs/RELEASE_CHECKLIST.md",
            "docs/TEST_PLAN_D1L.md",
        )
    ]
    active = "\n".join(release_docs)

    assert "one bounded" in active
    assert "No soak" in active
    assert "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0" in active
    assert "VID:PID 1a86:7523" in active
    assert "100-cycle" not in active
    assert "500-cycle tab-abuse" not in active
    assert "500-cycle tab abuse" not in active


def test_active_release_docs_keep_ui_acceptance_on_exact_candidate():
    roadmap = (ROOT / "docs/ROADMAP.md").read_text(encoding="utf-8").split(
        "## Historical", 1
    )[0]
    checklist = (ROOT / "docs/RELEASE_CHECKLIST.md").read_text(
        encoding="utf-8"
    ).split("## Historical", 1)[0]
    test_plan = (ROOT / "docs/TEST_PLAN_D1L.md").read_text(
        encoding="utf-8"
    ).split("## Historical", 1)[0]

    assert "boot/UI" in roadmap
    assert "operator-completed physical display/touch/keyboard/scroll" in roadmap
    assert "automated 12-surface navigation receipt" in roadmap
    assert "boot/five-root UI" in checklist
    assert (
        "operator accepted the physical display/touch/keyboard/scroll"
        in checklist
    )
    assert "boot and five-root navigation" in test_plan
    assert (
        "operator's completed physical display/touch/keyboard/scroll"
        in test_plan
    )
    assert "automated 12-surface navigation receipt" in test_plan
    assert "same exact commit" in test_plan
    assert "No soak" in test_plan


def test_smoke_knows_ui_console_commands():
    assert "ui status" in smoke_d1l.SMOKE_COMMANDS
    assert smoke_d1l.expected_command_name("ui tab packets") == "ui tab"
    assert smoke_d1l.expected_command_name("ui scroll-probe storage") == "ui scroll-probe"
    assert smoke_d1l.expected_command_name("ui compose-probe public-long") == "ui compose-probe"
    assert smoke_d1l.expected_command_name("ui data-canary uiRx0001") == "ui data-canary"
    assert smoke_d1l.expected_command_name("ui capture chunk 0 1024") == "ui capture chunk"


def test_ui_capture_dry_run_and_png_writer(tmp_path):
    report = ui_capture_d1l.dry_run_report(
        chunk_size=2048,
        prep_commands=["ui tab home"],
        reference_png="artifacts/ui-sim-reference/home.png",
        reference_view="home",
    )

    assert report["ok"] is True
    assert report["kind"] == "ui_pixel_capture"
    assert report["mode"] == "dry-run"
    assert report["explicit_port_required"] is True
    assert report["width"] == 480
    assert report["height"] == 480
    assert report["chunk_size"] == 1024
    assert report["prep_commands"] == ["ui tab home"]
    assert report["simulator_diff_required"] is True
    assert report["reference_view"] == "home"
    assert "ui tab home" in report["commands"]
    assert "ui capture begin" in report["commands"]
    assert "ui capture chunk 0 1024" in report["commands"]
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False

    raw = bytearray()
    for _ in range(480 * 480):
        raw.extend((0x00, 0xF8))
    png = tmp_path / "capture.png"
    ui_capture_d1l.write_png_from_rgb565_le(bytes(raw), png, 480, 480)

    assert png.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")


def test_ui_capture_diff_accepts_matching_reference_and_rejects_material_difference(tmp_path):
    raw = bytearray()
    for _ in range(480 * 480):
        raw.extend((0x00, 0xF8))
    capture = tmp_path / "capture.png"
    reference = tmp_path / "reference.png"
    ui_capture_d1l.write_png_from_rgb565_le(bytes(raw), capture, 480, 480)
    ui_capture_d1l.write_png_from_rgb565_le(bytes(raw), reference, 480, 480)

    matching = ui_capture_diff_d1l.compare_pngs(capture, reference, reference_view="home")

    assert matching["ok"] is True
    assert matching["kind"] == "ui_capture_simulator_diff"
    assert matching["reference_view"] == "home"
    assert matching["different_pixel_ratio"] == 0
    assert matching["public_rf_tx"] is False
    assert matching["formats_sd"] is False

    changed = bytearray(raw)
    for pixel in range(0, 220 * 480):
        offset = pixel * 2
        changed[offset] = 0x1F
        changed[offset + 1] = 0x00
    bad_reference = tmp_path / "bad-reference.png"
    ui_capture_d1l.write_png_from_rgb565_le(bytes(changed), bad_reference, 480, 480)

    different = ui_capture_diff_d1l.compare_pngs(
        capture,
        bad_reference,
        reference_view="home",
        max_different_ratio=0.10,
        max_tile_mismatch_ratio=0.10,
    )

    assert different["ok"] is False
    assert different["error"] == "material_pixel_difference"
    assert different["tile_mismatch_ratio"] > 0.10


def test_ui_capture_ready_wait_requires_fresh_non_pending_frame():
    baseline = {"ok": True, "shadow_ready": True, "frame_seq": 10, "flush_count": 20}

    assert ui_capture_d1l.capture_status_is_ready(
        {"ok": True, "shadow_ready": True, "frame_seq": 10, "flush_count": 20},
        baseline,
    ) is False
    assert ui_capture_d1l.capture_status_is_ready(
        {
            "ok": True,
            "shadow_ready": True,
            "frame_seq": 11,
            "flush_count": 20,
            "render_pending": True,
        },
        baseline,
    ) is False
    assert ui_capture_d1l.capture_status_is_ready(
        {"ok": True, "shadow_ready": True, "frame_seq": 11, "flush_count": 20},
        baseline,
    ) is True
    assert ui_capture_d1l.capture_status_is_ready(
        {"ok": True, "shadow_ready": True, "frame_seq": 1, "flush_count": 1},
        {"ok": False, "code": "TIMEOUT"},
    ) is True


def test_ui_compose_keyboard_capture_dry_run_is_targeted_and_safe():
    report = ui_compose_keyboard_capture_d1l.dry_run_report(
        targets=ui_compose_keyboard_capture_d1l.parse_targets("all"),
        chunk_size=2048,
    )

    assert report["ok"] is True
    assert report["kind"] == "ui_compose_keyboard_capture"
    assert report["mode"] == "dry-run"
    assert report["explicit_port_required"] is True
    assert report["targets"] == ui_compose_keyboard_capture_d1l.ALL_TARGETS
    assert "ui compose-probe public" in report["commands"]
    assert "ui compose-probe dm-long" in report["commands"]
    assert "ui compose-probe public-search" in report["commands"]
    assert "ui compose-probe dm-search" in report["commands"]
    assert "ui compose-probe packet-search" in report["commands"]
    assert "ui compose-probe contact-edit" in report["commands"]
    assert "ui compose-probe onboarding" in report["commands"]
    assert "ui compose-probe map-location" in report["commands"]
    assert "map-provider" not in report["commands"]
    assert "ui compose-probe wifi-ssid" in report["commands"]
    assert "ui compose-probe wifi-password" in report["commands"]
    assert "ui capture chunk 0 1024" in report["commands"]
    assert report["chunk_size"] == 1024
    assert report["public_rf_tx"] is False
    assert report["network_tx"] is False
    assert report["map_network_requests"] is False
    assert report["formats_sd"] is False


def test_ui_compose_keyboard_capture_accepts_all_and_underscore_aliases():
    assert ui_compose_keyboard_capture_d1l.parse_targets("all") == ui_compose_keyboard_capture_d1l.ALL_TARGETS
    assert ui_compose_keyboard_capture_d1l.parse_targets("wifi_password,map_location") == [
        "wifi-password",
        "map-location",
    ]


def test_ui_compose_keyboard_capture_preserves_pixels_when_probe_geometry_fails(tmp_path, monkeypatch):
    def fake_capture_frame(**kwargs):
        assert kwargs["prep_commands"] == ["ui compose-probe public"]
        assert kwargs["prep_ok_required"] is False
        assert kwargs["post_prep_settle_sec"] == 0.25
        return {
            "schema": 1,
            "kind": "ui_pixel_capture",
            "mode": "hardware",
            "ok": False,
            "pixel_capture_ok": True,
            "prep_ok": False,
            "prep_failures": [
                {
                    "schema": 1,
                    "ok": False,
                    "cmd": "ui compose-probe",
                    "target": "public",
                    "keyboard": {"x": 16, "y": 322, "w": 448, "h": 258},
                }
            ],
            "events": [
                {
                    "schema": 1,
                    "ok": False,
                    "cmd": "ui compose-probe",
                    "target": "public",
                    "keyboard": {"x": 16, "y": 322, "w": 448, "h": 258},
                }
            ],
            "width": 2,
            "height": 1,
            "bytes_per_pixel": 2,
            "pixel_format": "rgb565-le",
            "total_bytes": 4,
            "onboarding_visible": False,
            "raw_bytes": bytes([0x00, 0xF8, 0xE0, 0x07]),
            "public_rf_tx": False,
            "formats_sd": False,
        }

    monkeypatch.setattr(ui_compose_keyboard_capture_d1l, "capture_frame", fake_capture_frame)

    result = ui_compose_keyboard_capture_d1l.capture_target(
        "public",
        "COM12",
        115200,
        0.1,
        1024,
        tmp_path,
        "compose-bad-geometry",
    )

    assert result["ok"] is False
    assert result["capture"]["pixel_capture_ok"] is True
    assert result["capture"]["prep_ok"] is False
    assert result["target_visible"] is False
    assert result["png_path"].endswith("compose-bad-geometry-public.png")
    assert result["raw_path"].endswith("compose-bad-geometry-public.rgb565")
    assert Path(result["png_path"]).read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
    assert Path(result["raw_path"]).read_bytes() == bytes([0x00, 0xF8, 0xE0, 0x07])


def test_smoke_command_reader_bounds_readline_timeout_and_restores_serial_timeout():
    ser = FakeSerial(
        [
            b'{"schema":1,"ok":true,"cmd":"noise"}\n',
            b'{"schema":1,"ok":true,"cmd":"ui status","active_tab":"home"}\n',
        ]
    )

    result = smoke_d1l.read_command_result(ser, "ui status", timeout=0.25)

    assert result["ok"] is True
    assert result["cmd"] == "ui status"
    assert ser.timeout == 99.0
    assert ser.seen_timeouts
    assert max(timeout for timeout in ser.seen_timeouts if timeout is not None) <= 0.25


def test_smoke_command_reader_reports_ignored_json_on_timeout():
    ser = FakeSerial([b'{"schema":1,"ok":true,"cmd":"stale"}\n'])

    result = smoke_d1l.read_command_result(ser, "health", timeout=0.01)

    assert result["ok"] is False
    assert result["code"] == "TIMEOUT"
    assert result["ignored_json_count"] == 1
    assert result["ignored_json"] == [{"cmd": "stale", "ok": True}]
    assert ser.timeout == 99.0
