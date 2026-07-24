#!/usr/bin/env python3
"""Exact-candidate Core 1.0 UI probe for the verified D1L target."""

from __future__ import annotations

import argparse
import json
import os
import time
from collections.abc import Callable, Iterable
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from artifact_metadata import git_metadata, stamp_report
    from core_smoke_d1l import (
        CORE_RELEASE_PROFILE,
        D1L_CORE_PORT,
        D1L_CORE_POSIX_TARGET,
        SD_HISTORY_MODES,
        crashlog_clean,
        enforce_core_port,
        exact_identity,
        exact_unsupported_result,
        exact_version_identity,
        resolve_core_target,
        send_exact_console_command,
    )
    from d1l_serial_target import safe_slug
    from smoke_d1l import exact_commit, open_d1l_serial, send_console_command
    from ui_corruption_probe_d1l import (
        data_refresh_step,
        event_failures,
        summarize_telemetry,
        tab_step,
        token_for_round,
        token_prefix_for_run,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import git_metadata, stamp_report
    from scripts.core_smoke_d1l import (
        CORE_RELEASE_PROFILE,
        D1L_CORE_PORT,
        D1L_CORE_POSIX_TARGET,
        SD_HISTORY_MODES,
        crashlog_clean,
        enforce_core_port,
        exact_identity,
        exact_unsupported_result,
        exact_version_identity,
        resolve_core_target,
        send_exact_console_command,
    )
    from scripts.d1l_serial_target import safe_slug
    from scripts.smoke_d1l import exact_commit, open_d1l_serial, send_console_command
    from scripts.ui_corruption_probe_d1l import (
        data_refresh_step,
        event_failures,
        summarize_telemetry,
        tab_step,
        token_for_round,
        token_prefix_for_run,
    )


CORE_TAB_SEQUENCE = ("home", "messages", "nodes", "packets", "settings")
CORE_SCROLL_SURFACES = (
    "home",
    "public_messages",
    "dm_thread",
    "nodes",
    "packets",
    "settings",
)
CORE_COMPOSE_TARGETS = (
    "public",
    "public-long",
    "dm",
    "dm-long",
    "public-search",
    "dm-search",
    "packet-search",
    "contact-edit",
    "onboarding",
)
CORE_SCROLL_TABS = {
    "home": "home",
    "public_messages": "messages",
    "dm_thread": "messages",
    "nodes": "nodes",
    "packets": "packets",
    "settings": "settings",
}
CORE_COMPOSE_TABS = {
    "public": "messages",
    "public-long": "messages",
    "dm": "messages",
    "dm-long": "messages",
    "public-search": "messages",
    "dm-search": "messages",
    "packet-search": "packets",
    "contact-edit": "nodes",
    "onboarding": "home",
}
CORE_DM_COMPOSE_TARGETS = frozenset({"dm", "dm-long", "dm-search"})
CORE_SEND_SUPPRESSED_TARGETS = frozenset(
    {"public", "public-long", "dm", "dm-long"}
)
CORE_COMPOSE_MIN_KEYBOARD = {
    "public": (440, 250),
    "public-long": (440, 250),
    "dm": (440, 250),
    "dm-long": (440, 250),
    "public-search": (400, 180),
    "dm-search": (400, 180),
    "packet-search": (400, 180),
    "contact-edit": (400, 170),
    "onboarding": (400, 150),
}
RELEASE_MIN_ROUNDS = 20
CORE_UNAVAILABLE_UI_PROBES = (
    ("ui tab map", "map"),
    ("ui scroll-probe wi-fi", "wifi_user_control"),
    ("ui scroll-probe map-menu", "map"),
    ("ui scroll-probe contact-route", "user_trace"),
    ("ui scroll-probe mesh-roles", "admin"),
    ("ui compose-probe map_location", "location"),
    ("ui compose-probe wifi-pass", "wifi_user_control"),
)
DISABLED_SD_UNAVAILABLE_UI_PROBES = (
    ("ui scroll-probe storage-card", "sd_history"),
)


def _plain_int(value: object) -> bool:
    return type(value) is int


def core_scroll_result_ok(result: object, surface: str) -> bool:
    if not isinstance(result, dict) or surface not in CORE_SCROLL_TABS:
        return False
    coordinate_fields = (
        "before_y",
        "after_y",
        "scroll_top_before",
        "scroll_bottom_before",
        "scroll_top_after",
        "scroll_bottom_after",
    )
    if not all(_plain_int(result.get(field)) for field in coordinate_fields):
        return False
    movement_required = any(
        result[field] > 0
        for field in (
            "scroll_top_before",
            "scroll_bottom_before",
            "scroll_top_after",
            "scroll_bottom_after",
        )
    )
    moved = (
        result["before_y"] != result["after_y"]
        or result["scroll_top_before"] != result["scroll_top_after"]
        or result["scroll_bottom_before"] != result["scroll_bottom_after"]
    )
    return (
        result.get("schema") == 1
        and result.get("ok") is True
        and result.get("cmd") == "ui scroll-probe"
        and result.get("surface") == surface
        and result.get("tab") == CORE_SCROLL_TABS[surface]
        and result.get("surface_supported") is True
        and result.get("target_found") is True
        and result.get("scrollable") is True
        and result.get("movement_required") is movement_required
        and result.get("moved") is moved
        and (not movement_required or moved)
    )


def core_compose_result_ok(result: object, target: str) -> bool:
    if not isinstance(result, dict) or target not in CORE_COMPOSE_TABS:
        return False
    canonical = target.replace("-", "_")
    geometry: dict[str, dict] = {}
    for name in ("sheet", "textarea", "keyboard"):
        row = result.get(name)
        if (
            not isinstance(row, dict)
            or set(row) != {"x", "y", "w", "h"}
            or not all(_plain_int(row.get(field)) for field in row)
        ):
            return False
        geometry[name] = row
    sheet = geometry["sheet"]
    textarea = geometry["textarea"]
    keyboard = geometry["keyboard"]
    sheet_inside_screen = (
        sheet["w"] > 0
        and sheet["h"] > 0
        and sheet["x"] >= 0
        and sheet["y"] >= 0
        and sheet["x"] + sheet["w"] <= 480
        and sheet["y"] + sheet["h"] <= 480
    )
    keyboard_inside_sheet = (
        keyboard["w"] > 0
        and keyboard["h"] > 0
        and keyboard["x"] >= 0
        and keyboard["y"] >= 0
        and keyboard["x"] + keyboard["w"] <= sheet["w"]
        and keyboard["y"] + keyboard["h"] <= sheet["h"]
    )
    textarea_inside_sheet = (
        textarea["w"] > 0
        and textarea["h"] > 0
        and textarea["x"] >= 0
        and textarea["y"] >= 0
        and textarea["x"] + textarea["w"] <= sheet["w"]
        and textarea["y"] + textarea["h"] <= keyboard["y"]
    )
    min_keyboard_width, min_keyboard_height = (
        CORE_COMPOSE_MIN_KEYBOARD[target]
    )
    send_probe = target in CORE_SEND_SUPPRESSED_TARGETS
    return (
        result.get("schema") == 1
        and result.get("ok") is True
        and result.get("cmd") == "ui compose-probe"
        and result.get("target") == canonical
        and result.get("active_tab") == CORE_COMPOSE_TABS[target]
        and result.get("target_supported") is True
        and result.get("sheet_visible") is True
        and result.get("textarea_visible") is True
        and result.get("keyboard_visible") is True
        and result.get("onboarding_visible") is (target == "onboarding")
        and result.get("dock_hidden") is True
        and result.get("dm_mode") is (target in CORE_DM_COMPOSE_TARGETS)
        and result.get("tx_suppressed") is send_probe
        and result.get("send_enabled") is False
        and result.get("public_rf_tx") is False
        and result.get("formats_sd") is False
        and keyboard["w"] >= min_keyboard_width
        and keyboard["h"] >= min_keyboard_height
        and sheet_inside_screen
        and keyboard_inside_sheet
        and textarea_inside_sheet
    )


def core_tab_sequence_ok(tabs: object) -> bool:
    return (
        isinstance(tabs, list)
        and tuple(tabs) == CORE_TAB_SEQUENCE
        and "map" not in tabs
    )


def unavailable_ui_probe_plan(sd_history_mode: str) -> list[dict]:
    if sd_history_mode not in SD_HISTORY_MODES:
        raise ValueError(f"invalid SD history mode: {sd_history_mode}")
    probes = list(CORE_UNAVAILABLE_UI_PROBES)
    if sd_history_mode == "disabled":
        probes.extend(DISABLED_SD_UNAVAILABLE_UI_PROBES)
    return [
        {"command": command, "feature": feature}
        for command, feature in probes
    ]


def unavailable_ui_event_ok(
    event: object,
    expected: dict,
    expected_commit: str,
    expected_sd_history_mode: str,
) -> bool:
    if not isinstance(event, dict):
        return False
    before = event.get("before")
    after = event.get("after")
    command = expected.get("command")
    feature = expected.get("feature")
    return (
        event.get("command") == command
        and event.get("feature") == feature
        and exact_unsupported_result(
            event.get("result"), command, feature
        )
        and exact_identity(
            before, expected_commit, expected_sd_history_mode
        )
        and exact_identity(
            after, expected_commit, expected_sd_history_mode
        )
        and before.get("cmd") == "ui status"
        and after.get("cmd") == "ui status"
        and before.get("pending") is False
        and after.get("pending") is False
        and before.get("active_tab") in CORE_TAB_SEQUENCE
        and after.get("active_tab") == before.get("active_tab")
    )


def unavailable_ui_events_ok(
    events: object,
    expected_commit: str,
    expected_sd_history_mode: str,
) -> bool:
    expected_plan = unavailable_ui_probe_plan(expected_sd_history_mode)
    return (
        isinstance(events, list)
        and len(events) == len(expected_plan)
        and all(
            unavailable_ui_event_ok(
                event,
                expected,
                expected_commit,
                expected_sd_history_mode,
            )
            for event, expected in zip(events, expected_plan)
        )
    )


def command_plan(
    rounds: int = RELEASE_MIN_ROUNDS,
    sd_history_mode: str = "disabled",
) -> dict:
    unavailable_plan = unavailable_ui_probe_plan(sd_history_mode)
    commands = ["version", "health", "ui status"]
    commands.extend(
        f"ui scroll-probe {surface}" for surface in CORE_SCROLL_SURFACES
    )
    for round_number in range(1, rounds + 1):
        commands.extend(f"ui tab {tab}" for tab in CORE_TAB_SEQUENCE)
        commands.extend(
            (
                f"ui data-canary {token_for_round(round_number)}",
                "ui tab packets",
                f"packets search {token_for_round(round_number)}",
                "ui tab messages",
                f"messages public search {token_for_round(round_number)}",
                "ui status",
                "health",
                "crashlog",
            )
        )
    commands.extend(
        f"ui compose-probe {target}" for target in CORE_COMPOSE_TARGETS
    )
    for probe in unavailable_plan:
        commands.extend(("ui status", probe["command"], "ui status"))
    return {
        "schema": 2,
        "kind": "core_ui_corruption_probe_plan",
        "mode": "plan",
        "ok": False,
        "closure_eligible": False,
        "hardware_required": True,
        "physical_observed": False,
        "port": D1L_CORE_PORT,
        "supported_targets": [
            D1L_CORE_PORT,
            D1L_CORE_POSIX_TARGET,
        ],
        "release_profile": CORE_RELEASE_PROFILE,
        "rounds": rounds,
        "release_min_rounds": RELEASE_MIN_ROUNDS,
        "tabs": list(CORE_TAB_SEQUENCE),
        "scroll_surfaces": list(CORE_SCROLL_SURFACES),
        "compose_targets": list(CORE_COMPOSE_TARGETS),
        "unavailable_ui_probes": unavailable_plan,
        "commands": commands,
        "public_rf_tx": False,
        "network_tx": False,
        "map_network_requests": False,
        "formats_sd": False,
    }


def _identity_failure_report(
    *,
    port: str,
    d1l_target: dict[str, Any],
    expected_commit: str,
    expected_sd_history_mode: str | None,
    version: dict,
    github_run_id: str,
    workflow_run_attempt: str,
    health: dict | None = None,
) -> dict:
    observed_sd_mode = version.get("sd_history_mode")
    return {
        "schema": 2,
        "kind": "core_ui_corruption_probe",
        "mode": "hardware",
        "ok": False,
        "closure_eligible": False,
        "hardware_required": True,
        "physical_observed": True,
        "identity_preflight_only": True,
        "port": port,
        "d1l_target": d1l_target,
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": observed_sd_mode,
        "expected_sd_history_mode": expected_sd_history_mode,
        "expected_firmware_commit": expected_commit,
        "github_actions_run": github_run_id,
        "workflow_run_attempt": workflow_run_attempt,
        "device_build_commit": version.get("build_commit"),
        "firmware_identity_required": True,
        "firmware_identity_ok": False,
        "rounds": 0,
        "release_min_rounds": RELEASE_MIN_ROUNDS,
        "tabs": list(CORE_TAB_SEQUENCE),
        "events": [],
        "unavailable_events": [],
        "identity_results": [version] + ([health] if health else []),
        "public_rf_tx": False,
        "network_tx": False,
        "map_network_requests": False,
        "formats_sd": False,
    }


def run_probe(
    *,
    port: str,
    baud: int,
    timeout: float,
    rounds: int,
    settle_sec: float,
    poll_sec: float,
    clear_crashlog_before_start: bool,
    skip_data_canary: bool,
    expected_commit: str,
    expected_sd_history_mode: str | None,
    github_run_id: str,
    workflow_run_attempt: str,
    port_lister: Callable[[], Iterable[object]] | None = None,
    platform_name: str | None = None,
) -> dict:
    try:
        import serial
        from serial.tools import list_ports
    except ImportError as exc:
        raise SystemExit(
            "pyserial is required for Core UI hardware validation: "
            "python -m pip install pyserial"
        ) from exc

    port = enforce_core_port(port)
    normalized_commit = exact_commit(expected_commit)
    if normalized_commit is None:
        raise ValueError("expected_commit must be an exact 40-character hexadecimal SHA")
    if (
        expected_sd_history_mode is not None
        and expected_sd_history_mode not in SD_HISTORY_MODES
    ):
        raise ValueError("expected_sd_history_mode is invalid")
    if rounds < RELEASE_MIN_ROUNDS:
        raise ValueError(f"Core UI release evidence requires at least {RELEASE_MIN_ROUNDS} rounds")
    if skip_data_canary:
        raise ValueError("Core UI release evidence cannot skip the data canary")
    if (
        not str(github_run_id).isdigit()
        or int(github_run_id) < 1
        or not str(workflow_run_attempt).isdigit()
        or int(workflow_run_attempt) < 1
    ):
        raise ValueError(
            "GitHub run id and run attempt must be positive integers"
        )
    root = Path(__file__).resolve().parents[1]
    source = git_metadata(root)
    if not (
        exact_commit(source.get("commit")) == normalized_commit
        and source.get("dirty") is False
        and source.get("dirty_entries") == []
    ):
        raise ValueError(
            "Core UI probe must run from the exact clean candidate source"
        )
    d1l_target = resolve_core_target(
        port,
        port_lister=port_lister
        or (lambda: list_ports.comports(include_links=True)),
        platform_name=platform_name,
    )
    port = d1l_target["requested_path"]

    events: list[dict] = []
    setup_events: list[dict] = []
    scroll_events: list[dict] = []
    compose_events: list[dict] = []
    unavailable_events: list[dict] = []
    started_at = datetime.now(timezone.utc)
    run_prefix = token_prefix_for_run(started_at)

    with open_d1l_serial(
        serial, port=port, baudrate=baud, timeout=timeout
    ) as ser:
        time.sleep(1.0)
        ser.reset_input_buffer()

        # Exact candidate/profile admission precedes every UI action.
        version = send_console_command(ser, "version", timeout)
        observed_sd_mode = version.get("sd_history_mode")
        required_sd_mode = expected_sd_history_mode or observed_sd_mode
        if (
            required_sd_mode not in SD_HISTORY_MODES
            or not exact_version_identity(
                version, normalized_commit, required_sd_mode
            )
        ):
            return _identity_failure_report(
                port=port,
                d1l_target=d1l_target,
                expected_commit=normalized_commit,
                expected_sd_history_mode=expected_sd_history_mode,
                version=version,
                github_run_id=str(github_run_id),
                workflow_run_attempt=str(workflow_run_attempt),
            )
        health_preflight = send_console_command(ser, "health", timeout)
        if not exact_identity(
            health_preflight, normalized_commit, required_sd_mode
        ):
            return _identity_failure_report(
                port=port,
                d1l_target=d1l_target,
                expected_commit=normalized_commit,
                expected_sd_history_mode=expected_sd_history_mode,
                version=version,
                health=health_preflight,
                github_run_id=str(github_run_id),
                workflow_run_attempt=str(workflow_run_attempt),
            )

        setup_events.extend(
            (
                {"command": "version", "result": version},
                {"command": "health", "result": health_preflight},
                {
                    "command": "ui status",
                    "result": send_console_command(ser, "ui status", timeout),
                },
                {
                    "command": "crashlog",
                    "result": send_console_command(ser, "crashlog", timeout),
                },
            )
        )
        if not crashlog_clean(setup_events[-1]["result"]):
            return {
                **_identity_failure_report(
                    expected_commit=normalized_commit,
                    expected_sd_history_mode=expected_sd_history_mode,
                    version=version,
                    health=health_preflight,
                    github_run_id=str(github_run_id),
                    workflow_run_attempt=str(workflow_run_attempt),
                ),
                "identity_preflight_only": False,
                "pre_ui_crashlog_clean": False,
                "setup_events": setup_events,
            }
        if clear_crashlog_before_start:
            setup_events.append(
                {
                    "command": "crashlog clear",
                    "result": send_console_command(ser, "crashlog clear", timeout),
                }
            )

        for surface in CORE_SCROLL_SURFACES:
            command = f"ui scroll-probe {surface}"
            scroll_events.append(
                {
                    "surface": surface,
                    "command": command,
                    "result": send_console_command(ser, command, timeout),
                }
            )

        for probe in unavailable_ui_probe_plan(required_sd_mode):
            unavailable_events.append(
                {
                    **probe,
                    "before": send_console_command(
                        ser, "ui status", timeout
                    ),
                    "result": send_exact_console_command(
                        ser, probe["command"], timeout
                    ),
                    "after": send_console_command(
                        ser, "ui status", timeout
                    ),
                }
            )
        for round_number in range(1, rounds + 1):
            for tab in CORE_TAB_SEQUENCE:
                events.append(
                    tab_step(
                        ser,
                        tab,
                        timeout,
                        settle_sec,
                        poll_sec,
                        round_number,
                    )
                )
            if not skip_data_canary:
                events.append(
                    data_refresh_step(
                        ser,
                        token_for_round(round_number, run_prefix),
                        timeout,
                        settle_sec,
                        poll_sec,
                        round_number,
                    )
                )

        # Compose probes run only after the 20-round visible navigation loop.
        # The onboarding target intentionally leaves its modal visible and
        # must never cover the tab-switch evidence being qualified.
        for target in CORE_COMPOSE_TARGETS:
            command = f"ui compose-probe {target}"
            compose_events.append(
                {
                    "target": target,
                    "command": command,
                    "result": send_console_command(ser, command, timeout),
                }
            )

        health_final = send_console_command(ser, "health", timeout)

    ended_at = datetime.now(timezone.utc)
    setup_failures = [
        event
        for event in setup_events
        if event.get("result", {}).get("ok") is not True
    ]
    scroll_failures = [
        event
        for event in scroll_events
        if not core_scroll_result_ok(
            event.get("result"), event.get("surface", "")
        )
    ]
    compose_failures = [
        event
        for event in compose_events
        if not core_compose_result_ok(
            event.get("result"), event.get("target", "")
        )
    ]
    unavailable_failures = [
        {
            "kind": "unavailable_ui_probe",
            "command": expected["command"],
            "feature": expected["feature"],
        }
        for event, expected in zip(
            unavailable_events,
            unavailable_ui_probe_plan(required_sd_mode),
        )
        if not unavailable_ui_event_ok(
            event,
            expected,
            normalized_commit,
            required_sd_mode,
        )
    ]
    if len(unavailable_events) != len(
        unavailable_ui_probe_plan(required_sd_mode)
    ):
        unavailable_failures.append(
            {
                "kind": "unavailable_ui_probe",
                "code": "PROBE_COUNT_MISMATCH",
            }
        )
    failures = event_failures(events, skip_data_canary=skip_data_canary)
    identity_failures = []
    for event in events:
        for candidate in (
            event.get("health"),
            event.get("packet_tab", {}).get("health")
            if isinstance(event.get("packet_tab"), dict)
            else None,
            event.get("messages_tab", {}).get("health")
            if isinstance(event.get("messages_tab"), dict)
            else None,
        ):
            if isinstance(candidate, dict) and not exact_identity(
                candidate, normalized_commit, required_sd_mode
            ):
                identity_failures.append(
                    {
                        "round": event.get("round"),
                        "kind": event.get("kind"),
                        "code": "IDENTITY_DRIFT",
                    }
                )
    if (
        not exact_identity(
            health_final, normalized_commit, required_sd_mode
        )
        or health_final.get("cmd") != "health"
        or health_final.get("board_ready") is not True
        or health_final.get("ui_ready") is not True
    ):
        identity_failures.append({"kind": "final_health", "code": "IDENTITY_DRIFT"})

    telemetry = summarize_telemetry(events)
    telemetry_failures = []
    if telemetry.get("uptime_monotonic") is not True:
        telemetry_failures.append({"check": "uptime_monotonic", "ok": False})
    if telemetry.get("final_pending") is not False:
        telemetry_failures.append({"check": "no_stuck_pending", "ok": False})
    if telemetry.get("final_active_tab") not in CORE_TAB_SEQUENCE:
        telemetry_failures.append({"check": "final_active_tab_known", "ok": False})

    all_failures = (
        setup_failures
        + scroll_failures
        + compose_failures
        + unavailable_failures
        + failures
        + identity_failures
        + telemetry_failures
    )
    report = {
        "schema": 2,
        "kind": "core_ui_corruption_probe",
        "mode": "hardware",
        "ok": not all_failures,
        "closure_eligible": True,
        "hardware_required": True,
        "physical_observed": True,
        "identity_preflight_only": False,
        "port": port,
        "d1l_target": d1l_target,
        "baud": baud,
        "started_at": started_at.isoformat().replace("+00:00", "Z"),
        "ended_at": ended_at.isoformat().replace("+00:00", "Z"),
        "release_profile": CORE_RELEASE_PROFILE,
        "sd_history_mode": required_sd_mode,
        "expected_sd_history_mode": expected_sd_history_mode,
        "expected_firmware_commit": normalized_commit,
        "github_actions_run": str(github_run_id),
        "workflow_run_attempt": str(workflow_run_attempt),
        "device_build_commit": version.get("build_commit"),
        "firmware_identity_required": True,
        "firmware_identity_ok": not identity_failures,
        "rounds": rounds,
        "release_min_rounds": RELEASE_MIN_ROUNDS,
        "settle_sec": settle_sec,
        "poll_sec": poll_sec,
        "clear_crashlog_before_start": clear_crashlog_before_start,
        "skip_data_canary": skip_data_canary,
        "data_canary_prefix": run_prefix,
        "tabs": list(CORE_TAB_SEQUENCE),
        "scroll_surfaces": list(CORE_SCROLL_SURFACES),
        "compose_targets": list(CORE_COMPOSE_TARGETS),
        "unavailable_ui_probes": unavailable_ui_probe_plan(
            required_sd_mode
        ),
        "public_rf_tx": False,
        "network_tx": False,
        "map_network_requests": False,
        "formats_sd": False,
        "data_refresh_events": sum(
            1 for event in events if event.get("kind") == "data_refresh"
        ),
        "failure_count": len(all_failures),
        "failures": failures,
        "identity_failures": identity_failures,
        "telemetry_failures": telemetry_failures,
        "telemetry": telemetry,
        "checks": {
            "exact_candidate": not identity_failures,
            "esp_idf_v5_5_4": version.get("idf") == "v5.5.4",
            "core_profile": not identity_failures,
            "pre_ui_crashlog_clean": crashlog_clean(
                setup_events[3]["result"]
            ),
            "core_tab_sequence_exact": True,
            "core_scroll_surfaces_exact": not scroll_failures,
            "core_compose_targets_exact": not compose_failures,
            "tab_switches_settle": not any(
                failure.get("kind") == "tab" for failure in failures
            ),
            "data_refresh_exercised": skip_data_canary
            or sum(1 for event in events if event.get("kind") == "data_refresh")
            == rounds,
            "data_refreshes_pass": not any(
                failure.get("kind") == "data_refresh" for failure in failures
            ),
            "unavailable_destinations_rejected": not unavailable_failures,
            "no_public_rf": True,
            "no_network_tx": True,
            "no_map_network_requests": True,
            "no_formatting": True,
            "uptime_monotonic": telemetry.get("uptime_monotonic") is True,
            "no_stuck_pending": telemetry.get("final_pending") is False,
            "final_active_tab_known": telemetry.get("final_active_tab")
            in CORE_TAB_SEQUENCE,
        },
        "setup_events": setup_events,
        "scroll_events": scroll_events,
        "compose_events": compose_events,
        "unavailable_events": unavailable_events,
        "final_health": health_final,
        "events": events,
    }
    return report


def resolve_out_path(
    out_arg: str | None,
    target: str = D1L_CORE_PORT,
) -> Path:
    root = Path(__file__).resolve().parents[1]
    if out_arg:
        path = Path(out_arg)
        return path if path.is_absolute() else root / path
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    slug = safe_slug(enforce_core_port(target))
    return (
        root
        / "artifacts"
        / "hardware"
        / slug
        / f"core_ui_corruption_probe_{stamp}_{slug}.json"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--rounds", type=int, default=RELEASE_MIN_ROUNDS)
    parser.add_argument("--settle-sec", type=float, default=0.1)
    parser.add_argument("--poll-sec", type=float, default=0.05)
    parser.add_argument("--clear-crashlog-before-start", action="store_true")
    parser.add_argument("--skip-data-canary", action="store_true")
    parser.add_argument("--expected-firmware-commit", required=True)
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-attempt", required=True)
    parser.add_argument(
        "--expected-sd-history-mode",
        choices=sorted(SD_HISTORY_MODES),
    )
    parser.add_argument("--out")
    args = parser.parse_args(argv)

    try:
        port = enforce_core_port(args.port)
    except ValueError as exc:
        parser.error(str(exc))
    commit = exact_commit(args.expected_firmware_commit)
    if commit is None:
        parser.error(
            "--expected-firmware-commit must be an exact 40-character hexadecimal SHA"
        )
    if args.rounds < RELEASE_MIN_ROUNDS:
        parser.error(f"--rounds must be at least {RELEASE_MIN_ROUNDS}")
    if args.settle_sec < 0:
        parser.error("--settle-sec cannot be negative")
    if args.poll_sec <= 0:
        parser.error("--poll-sec must be positive")
    if args.skip_data_canary:
        parser.error("Core UI release evidence cannot use --skip-data-canary")
    if (
        not str(args.github_run_id).isdigit()
        or int(args.github_run_id) < 1
        or not str(args.github_run_attempt).isdigit()
        or int(args.github_run_attempt) < 1
    ):
        parser.error(
            "--github-run-id and --github-run-attempt must be positive integers"
        )

    report = run_probe(
        port=port,
        baud=args.baud,
        timeout=args.timeout,
        rounds=args.rounds,
        settle_sec=args.settle_sec,
        poll_sec=args.poll_sec,
        clear_crashlog_before_start=args.clear_crashlog_before_start,
        skip_data_canary=args.skip_data_canary,
        expected_commit=commit,
        expected_sd_history_mode=args.expected_sd_history_mode,
        github_run_id=str(args.github_run_id),
        workflow_run_attempt=str(args.github_run_attempt),
    )
    root = Path(__file__).resolve().parents[1]
    stamp_report(report, root)
    out_path = resolve_out_path(args.out, report["port"])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2) + "\n", encoding="ascii")
    print(json.dumps({"ok": report["ok"], "out": str(out_path)}, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
