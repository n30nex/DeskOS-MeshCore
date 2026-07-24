import pytest

from scripts import core_ui_corruption_probe_d1l as core_ui


PUBLIC_KEY = "ab" * 32


def test_core_ui_sequence_is_exact_and_excludes_unavailable_destinations():
    assert core_ui.CORE_TAB_SEQUENCE == (
        "home",
        "messages",
        "nodes",
        "packets",
        "settings",
    )
    assert core_ui.core_tab_sequence_ok(list(core_ui.CORE_TAB_SEQUENCE))
    assert not core_ui.core_tab_sequence_ok(
        ["home", "messages", "nodes", "map", "packets", "settings"]
    )
    assert not core_ui.core_tab_sequence_ok(
        ["home", "messages", "nodes", "settings", "packets"]
    )


def test_core_ui_plan_is_non_closing_and_only_probes_excluded_ui_fail_closed():
    plan = core_ui.command_plan(20)

    assert plan["schema"] == 2
    assert plan["ok"] is False
    assert plan["closure_eligible"] is False
    assert plan["hardware_required"] is True
    assert plan["tabs"] == list(core_ui.CORE_TAB_SEQUENCE)
    assert "map" not in plan["tabs"]
    assert plan["scroll_surfaces"] == [
        "home",
        "public_messages",
        "dm_thread",
        "nodes",
        "packets",
        "settings",
    ]
    assert plan["compose_targets"] == [
        "public",
        "public-long",
        "dm",
        "dm-long",
        "public-search",
        "dm-search",
        "packet-search",
        "contact-edit",
        "onboarding",
    ]
    assert plan["unavailable_ui_probes"] == [
        {"command": "ui tab map", "feature": "map"},
        {
            "command": "ui scroll-probe wi-fi",
            "feature": "wifi_user_control",
        },
        {"command": "ui scroll-probe map-menu", "feature": "map"},
        {
            "command": "ui scroll-probe contact-route",
            "feature": "user_trace",
        },
        {"command": "ui scroll-probe mesh-roles", "feature": "admin"},
        {
            "command": "ui compose-probe map_location",
            "feature": "location",
        },
        {
            "command": "ui compose-probe wifi-pass",
            "feature": "wifi_user_control",
        },
        {
            "command": "ui scroll-probe storage-card",
            "feature": "sd_history",
        },
    ]
    assert not any(
        command.startswith(("map ", "wifi ", "ble "))
        for command in plan["commands"]
    )
    assert plan["public_rf_tx"] is False
    assert plan["network_tx"] is False
    assert plan["map_network_requests"] is False
    assert plan["formats_sd"] is False


def _status(tab: str = "home") -> dict:
    return {
        "schema": 1,
        "ok": True,
        "cmd": "ui status",
        "build_commit": "a" * 40,
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "active_tab": tab,
        "pending": False,
    }


def _unavailable_events() -> list[dict]:
    return [
        {
            **probe,
            "before": _status(),
            "result": {
                "schema": 1,
                "ok": False,
                "cmd": probe["command"],
                "code": "ESP_ERR_NOT_SUPPORTED",
                "release_profile": "core_1_0",
                "feature": probe["feature"],
            },
            "after": _status(),
        }
        for probe in core_ui.unavailable_ui_probe_plan("disabled")
    ]


def test_unavailable_ui_events_require_exact_rejection_and_stable_tab():
    events = _unavailable_events()
    assert core_ui.unavailable_ui_events_ok(
        events, "a" * 40, "disabled"
    )
    assert not core_ui.unavailable_ui_events_ok(
        events[:-1], "a" * 40, "disabled"
    )

    tampered = _unavailable_events()
    tampered[1]["after"] = _status("messages")
    assert not core_ui.unavailable_ui_events_ok(
        tampered, "a" * 40, "disabled"
    )

    tampered = _unavailable_events()
    tampered[2]["result"]["feature"] = "location"
    assert not core_ui.unavailable_ui_events_ok(
        tampered, "a" * 40, "disabled"
    )


def _scroll_result(
    surface: str,
    *,
    bottom_before: int = 0,
    top_after: int = 0,
    after_y: int = 0,
) -> dict:
    movement_required = bottom_before > 0 or top_after > 0
    bottom_after = 0 if movement_required else bottom_before
    moved = (
        after_y != 0
        or top_after != 0
        or bottom_before != bottom_after
    )
    return {
        "schema": 1,
        "ok": not movement_required or moved,
        "cmd": "ui scroll-probe",
        "surface": surface,
        "tab": core_ui.CORE_SCROLL_TABS[surface],
        "surface_supported": True,
        "target_found": True,
        "scrollable": True,
        "movement_required": movement_required,
        "moved": moved,
        "before_y": 0,
        "after_y": after_y,
        "scroll_top_before": 0,
        "scroll_bottom_before": bottom_before,
        "scroll_top_after": top_after,
        "scroll_bottom_after": bottom_after,
    }


def _compose_result(target: str) -> dict:
    return {
        "schema": 1,
        "ok": True,
        "cmd": "ui compose-probe",
        "target": target.replace("-", "_"),
        "active_tab": core_ui.CORE_COMPOSE_TABS[target],
        "target_supported": True,
        "sheet_visible": True,
        "textarea_visible": True,
        "keyboard_visible": True,
        "onboarding_visible": target == "onboarding",
        "dock_hidden": True,
        "dm_mode": target in core_ui.CORE_DM_COMPOSE_TARGETS,
        "tx_suppressed": target in core_ui.CORE_SEND_SUPPRESSED_TARGETS,
        "send_enabled": False,
        "sheet": {"x": 0, "y": 56, "w": 480, "h": 424},
        "textarea": {"x": 16, "y": 58, "w": 448, "h": 78},
        "keyboard": {"x": 16, "y": 158, "w": 448, "h": 258},
        "public_rf_tx": False,
        "formats_sd": False,
    }


def test_core_scroll_result_recomputes_overflow_and_movement():
    fit_only = _scroll_result("home")
    assert core_ui.core_scroll_result_ok(fit_only, "home")

    compact = _scroll_result("settings", bottom_before=-50)
    assert core_ui.core_scroll_result_ok(compact, "settings")

    empty_dm = _scroll_result("dm_thread")
    assert core_ui.core_scroll_result_ok(empty_dm, "dm_thread")

    overflow = _scroll_result(
        "public_messages",
        bottom_before=6,
        top_after=6,
        after_y=6,
    )
    assert core_ui.core_scroll_result_ok(overflow, "public_messages")

    forged = dict(fit_only, movement_required=True)
    assert not core_ui.core_scroll_result_ok(forged, "home")

    forged = dict(overflow, moved=False)
    assert not core_ui.core_scroll_result_ok(
        forged, "public_messages"
    )

    forged = dict(overflow, after_y=0, scroll_top_after=0, moved=False)
    assert not core_ui.core_scroll_result_ok(
        forged, "public_messages"
    )

    forged = dict(fit_only, before_y=False)
    assert not core_ui.core_scroll_result_ok(forged, "home")


def test_core_compose_result_requires_probe_tx_suppression_and_raw_geometry():
    for target in core_ui.CORE_COMPOSE_TARGETS:
        result = _compose_result(target)
        assert core_ui.core_compose_result_ok(result, target)

    dm = _compose_result("dm")
    dm["tx_suppressed"] = False
    assert not core_ui.core_compose_result_ok(dm, "dm")

    dm = _compose_result("dm")
    dm["send_enabled"] = True
    assert not core_ui.core_compose_result_ok(dm, "dm")

    dm = _compose_result("dm")
    dm["dm_mode"] = False
    assert not core_ui.core_compose_result_ok(dm, "dm")

    public = _compose_result("public")
    public["keyboard"]["h"] = 400
    assert not core_ui.core_compose_result_ok(public, "public")

    public = _compose_result("public")
    public["keyboard"]["h"] = 200
    assert not core_ui.core_compose_result_ok(public, "public")

    public = _compose_result("public")
    public["public_rf_tx"] = True
    assert not core_ui.core_compose_result_ok(public, "public")


def test_core_ui_invalid_target_fails_before_serial_open(monkeypatch):
    commit = "a" * 40
    monkeypatch.setattr(
        core_ui,
        "git_metadata",
        lambda _root: {
            "commit": commit,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(
        core_ui,
        "open_d1l_serial",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            AssertionError("invalid target must fail before serial open")
        ),
    )

    with pytest.raises(ValueError, match="PID"):
        core_ui.run_probe(
            port="COM12",
            baud=115200,
            timeout=1.0,
            rounds=20,
            settle_sec=0.0,
            poll_sec=0.01,
            clear_crashlog_before_start=False,
            skip_data_canary=False,
            expected_commit=commit,
            expected_d1l_public_key=PUBLIC_KEY,
            expected_sd_history_mode="disabled",
            github_run_id="123",
            workflow_run_attempt="1",
            platform_name="nt",
            port_lister=lambda: [
                {
                    "device": "COM12",
                    "vid": 0x1A86,
                    "pid": 0x0001,
                    "serial_number": "wrong",
                    "hwid": "wrong",
                    "location": "1-9",
                }
            ],
        )


def test_core_ui_wrong_full_key_stops_before_health_or_ui_actions(
    monkeypatch,
):
    commit = "a" * 40
    wrong_key = PUBLIC_KEY[:16] + "cd" * 24
    commands = []

    class FakePort:
        def reset_input_buffer(self):
            pass

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            return False

    monkeypatch.setattr(
        core_ui,
        "git_metadata",
        lambda _root: {
            "commit": commit,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(
        core_ui,
        "open_d1l_serial",
        lambda *_args, **_kwargs: FakePort(),
    )

    def fake_send(_ser, command, _timeout):
        commands.append(command)
        if command == "version":
            return {
                "schema": 1,
                "ok": True,
                "cmd": "version",
                "build_commit": commit,
                "release_profile": "core_1_0",
                "sd_history_mode": "disabled",
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

    monkeypatch.setattr(core_ui, "send_console_command", fake_send)
    monkeypatch.setattr(core_ui.time, "sleep", lambda _seconds: None)

    report = core_ui.run_probe(
        port="COM12",
        baud=115200,
        timeout=1.0,
        rounds=20,
        settle_sec=0.0,
        poll_sec=0.01,
        clear_crashlog_before_start=True,
        skip_data_canary=False,
        expected_commit=commit,
        expected_d1l_public_key=PUBLIC_KEY,
        expected_sd_history_mode="disabled",
        github_run_id="123",
        workflow_run_attempt="1",
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
    assert report["events"] == []
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert report["closure_eligible"] is False
    assert report["ok"] is False


def test_core_ui_rejects_missing_key_before_serial_or_source_io(monkeypatch):
    monkeypatch.setattr(
        core_ui,
        "git_metadata",
        lambda _root: pytest.fail("source access must not begin"),
    )
    monkeypatch.setattr(
        core_ui,
        "open_d1l_serial",
        lambda *_args, **_kwargs: pytest.fail("serial must not open"),
    )

    with pytest.raises(ValueError, match="exact 64-hex"):
        core_ui.run_probe(
            port="COM12",
            baud=115200,
            timeout=1.0,
            rounds=20,
            settle_sec=0.0,
            poll_sec=0.01,
            clear_crashlog_before_start=False,
            skip_data_canary=False,
            expected_commit="a" * 40,
            expected_d1l_public_key="short",
            expected_sd_history_mode="disabled",
            github_run_id="123",
            workflow_run_attempt="1",
        )


def test_core_ui_pi_output_path_uses_safe_slug():
    path = core_ui.resolve_out_path(None, core_ui.D1L_CORE_POSIX_TARGET)
    assert "dev-serial-by-id-usb-1a86-usb-serial-if00-port0" in str(path)
    assert core_ui.D1L_CORE_POSIX_TARGET not in str(path)
