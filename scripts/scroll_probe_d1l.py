#!/usr/bin/env python3
"""Autonomous scroll probe for MeshCore DeskOS D1L screens."""

from __future__ import annotations

import argparse
import json
import os
import time
from collections.abc import Callable, Iterable
from datetime import datetime, timezone
from pathlib import Path

try:
    from smoke_d1l import open_d1l_serial, send_console_command
    from artifact_metadata import git_metadata, stamp_report
    from core_smoke_d1l import (
        SD_HISTORY_MODES,
        enforce_core_port,
        exact_identity,
        exact_version_identity,
        resolve_core_target,
    )
    from rf_full_acceptance_d1l import (
        d1l_identity_status_ok,
        exact_public_key,
    )
    from smoke_d1l import exact_commit
except ModuleNotFoundError:
    from scripts.artifact_metadata import git_metadata, stamp_report
    from scripts.core_smoke_d1l import (
        SD_HISTORY_MODES,
        enforce_core_port,
        exact_identity,
        exact_version_identity,
        resolve_core_target,
    )
    from scripts.rf_full_acceptance_d1l import (
        d1l_identity_status_ok,
        exact_public_key,
    )
    from scripts.smoke_d1l import (
        exact_commit,
        open_d1l_serial,
        send_console_command,
    )


SCROLL_SURFACES = {
    "home": {"tab": "home", "label": "Home previews"},
    "public_messages": {"tab": "messages", "label": "Public messages"},
    "dm_thread": {"tab": "messages", "label": "DM thread"},
    "nodes": {"tab": "nodes", "label": "Nodes"},
    "packets": {"tab": "packets", "label": "Packets"},
    "settings": {"tab": "settings", "label": "Settings"},
    "storage": {"tab": "settings", "label": "Storage overview"},
    "storage_card": {"tab": "settings", "label": "SD card status"},
    "storage_data": {"tab": "settings", "label": "Data locations"},
    "wifi": {"tab": "settings", "label": "Wi-Fi settings"},
    "map": {"tab": "map", "label": "Actual Map view"},
    "map_options": {"tab": "map", "label": "Map options"},
    "map_location": {"tab": "map", "label": "Map location"},
    "map_cache": {"tab": "map", "label": "Cache status"},
}
SCREEN_ALIASES = {
    "messages": "public_messages",
    "public": "public_messages",
    "public_messages": "public_messages",
    "public-messages": "public_messages",
    "dm": "dm_thread",
    "dm_thread": "dm_thread",
    "dm-thread": "dm_thread",
    "sd": "storage",
    "sd_card": "storage_card",
    "sd-card": "storage_card",
    "storage-card": "storage_card",
    "storage-data": "storage_data",
    "wi_fi": "wifi",
    "wi-fi": "wifi",
    "map-options": "map_options",
    "map_options_page": "map_options",
    "map-options-page": "map_options",
    "map-location": "map_location",
    "map_location_page": "map_location",
    "map-location-page": "map_location",
    "map-cache": "map_cache",
}
DEFAULT_SCREENS = tuple(SCROLL_SURFACES)
VALID_SCREENS = tuple(SCROLL_SURFACES)
MAP_READ_ONLY_SURFACES = frozenset({
    "map",
    "map_options",
    "map_location",
    "map_cache",
})
SCROLL_MOVEMENT_OPTIONAL = {"home", "storage", "storage_card", *MAP_READ_ONLY_SURFACES}
CORE_RELEASE_PROFILE = "core_1_0"
RELEASE_PROFILES = ("development", "full_feature", CORE_RELEASE_PROFILE)
CORE_SCROLL_SEQUENCE = (
    "home",
    "public_messages",
    "dm_thread",
    "nodes",
    "packets",
    "settings",
)
CORE_SCROLL_SURFACES = frozenset(CORE_SCROLL_SEQUENCE)


def _positive_decimal(value: object) -> str | None:
    normalized = str(value).strip() if value is not None else ""
    if not normalized.isdigit() or int(normalized) < 1:
        return None
    return normalized


def validate_core_evidence_inputs(
    *,
    port: object,
    expected_firmware_commit: object,
    github_actions_run: object,
    workflow_run_attempt: object,
    expected_sd_history_mode: object,
    root: Path,
) -> dict:
    normalized_port = enforce_core_port(port)
    normalized_commit = exact_commit(expected_firmware_commit)
    run_id = _positive_decimal(github_actions_run)
    run_attempt = _positive_decimal(workflow_run_attempt)
    if normalized_commit is None:
        raise ValueError(
            "Core scroll evidence requires an exact 40-hex firmware commit"
        )
    if run_id is None:
        raise ValueError(
            "Core scroll evidence requires a positive GitHub Actions run ID"
        )
    if run_attempt is None:
        raise ValueError(
            "Core scroll evidence requires a positive workflow run attempt"
        )
    if expected_sd_history_mode not in SD_HISTORY_MODES:
        raise ValueError(
            "Core scroll evidence requires an exact SD history mode"
        )
    source = git_metadata(root)
    if (
        exact_commit(source.get("commit")) != normalized_commit
        or source.get("dirty") is not False
        or source.get("dirty_entries") != []
    ):
        raise ValueError(
            "Core scroll evidence requires the clean exact candidate source"
        )
    return {
        "port": normalized_port,
        "expected_firmware_commit": normalized_commit,
        "github_actions_run": run_id,
        "workflow_run_attempt": run_attempt,
        "expected_sd_history_mode": expected_sd_history_mode,
        "git": source,
    }


def probe_safety(*, clear_crashlog_before_start: bool) -> dict[str, bool | int]:
    return {
        "read_only": not clear_crashlog_before_start,
        "save_actions": False,
        "clear_actions": clear_crashlog_before_start,
        "map_download": False,
        "network_tx": False,
        "map_network_requests": False,
        "background_download": False,
        "area_download": False,
        "visible_tile_limit": 9,
        "zoom_batch_limit": 1,
        "wifi_mutation": False,
        "rf_tx": False,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "storage_mutation": False,
        "formats_sd": False,
    }


def validate_setup_actions(screens: list[str], clear_crashlog_before_start: bool) -> None:
    if clear_crashlog_before_start and MAP_READ_ONLY_SURFACES.intersection(screens):
        raise ValueError(
            "--clear-crashlog-before-start is not allowed with network-suppressed Map probes"
        )


def validate_release_profile_screens(
    screens: list[str], release_profile: str
) -> None:
    if release_profile not in RELEASE_PROFILES:
        raise ValueError(f"invalid release profile: {release_profile}")
    if release_profile == CORE_RELEASE_PROFILE:
        if tuple(screens) != CORE_SCROLL_SEQUENCE:
            raise ValueError(
                "Core scroll evidence requires the exact ordered surfaces: "
                + ",".join(CORE_SCROLL_SEQUENCE)
            )


def utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def parse_screens(value: str) -> list[str]:
    raw_screens = [item.strip().lower() for item in value.split(",") if item.strip()]
    screens = [SCREEN_ALIASES.get(screen.replace(" ", "_"), screen.replace(" ", "_")) for screen in raw_screens]
    invalid = [raw for raw, screen in zip(raw_screens, screens) if screen not in SCROLL_SURFACES]
    if invalid:
        raise ValueError(f"invalid screen(s): {', '.join(invalid)}")
    return screens


def surface_plan(screens: list[str]) -> list[dict]:
    return [
        {"screen": screen, "tab": SCROLL_SURFACES[screen]["tab"], "label": SCROLL_SURFACES[screen]["label"]}
        for screen in screens
    ]


def map_network_request_count(row: dict | None) -> int | None:
    if not isinstance(row, dict) or row.get("ok") is not True:
        return None
    value = row.get("network_requests")
    if isinstance(value, bool):
        return None
    if isinstance(value, int) and value >= 0:
        return value
    if isinstance(value, str) and value.isdigit():
        return int(value)
    return None


def summarize_map_network_evidence(
    before: dict | None,
    after: dict | None,
    *,
    measured: bool,
) -> dict:
    before_count = map_network_request_count(before)
    after_count = map_network_request_count(after)
    samples_valid = measured and before_count is not None and after_count is not None
    return {
        "command": "map tiles status",
        "measured": measured,
        "before": before,
        "after": after,
        "before_count": before_count,
        "after_count": after_count,
        "delta": after_count - before_count if samples_valid else None,
        "samples_valid": samples_valid,
        "unchanged": samples_valid and before_count == after_count,
    }


def dry_run_report(
    screens: list[str],
    dwell_sec: float,
    manual_touch: bool,
    release_profile: str = "full_feature",
) -> dict:
    validate_release_profile_screens(screens, release_profile)
    profile_evidence = (
        {
            "release_profile": release_profile,
            "scroll_movement_policy": "positive_raw_overflow",
        }
        if release_profile == CORE_RELEASE_PROFILE
        else {}
    )
    plan = surface_plan(screens)
    commands: list[str] = []
    map_probe_requested = bool(MAP_READ_ONLY_SURFACES.intersection(screens))
    if map_probe_requested:
        commands.append("map tiles status")
    for item in plan:
        if item["screen"] in MAP_READ_ONLY_SURFACES:
            commands.append(f"ui scroll-probe {item['screen']}")
        else:
            commands.extend([f"ui tab {item['tab']}", f"ui scroll-probe {item['screen']}"])
    if map_probe_requested:
        commands.append("map tiles status")
    commands.extend(["ui status", "health", "crashlog"])
    return {
        "schema": 1,
        "mode": "dry-run",
        "ok": True,
        "hardware_required": False,
        "explicit_port_required": True,
        "manual_touch": manual_touch,
        "dwell_sec": dwell_sec,
        "screens": screens,
        "surface_plan": plan,
        "commands": commands,
        "map_network_evidence": {
            "command": "map tiles status",
            "measured": False,
            "declared_no_network": True,
            "before": None,
            "after": None,
        },
        **profile_evidence,
        **probe_safety(clear_crashlog_before_start=False),
    }


def crashlog_has_crash_like_entries(row: dict) -> bool:
    entries = row.get("entries")
    return isinstance(entries, list) and any(
        isinstance(entry, dict) and entry.get("crash_like") is True
        for entry in entries
    )


def _plain_int(value: object) -> bool:
    return type(value) is int


def core_probe_result_ok(probe: object, screen: str) -> bool:
    if not isinstance(probe, dict) or screen not in CORE_SCROLL_SURFACES:
        return False
    fields = (
        "before_y",
        "after_y",
        "scroll_top_before",
        "scroll_bottom_before",
        "scroll_top_after",
        "scroll_bottom_after",
    )
    if not all(_plain_int(probe.get(field)) for field in fields):
        return False
    movement_required = any(
        probe[field] > 0
        for field in (
            "scroll_top_before",
            "scroll_bottom_before",
            "scroll_top_after",
            "scroll_bottom_after",
        )
    )
    moved = (
        probe["before_y"] != probe["after_y"]
        or probe["scroll_top_before"] != probe["scroll_top_after"]
        or probe["scroll_bottom_before"] != probe["scroll_bottom_after"]
    )
    return (
        probe.get("schema") == 1
        and probe.get("ok") is True
        and probe.get("cmd") == "ui scroll-probe"
        and probe.get("surface") == screen
        and probe.get("tab") == SCROLL_SURFACES[screen]["tab"]
        and probe.get("surface_supported") is True
        and probe.get("target_found") is True
        and probe.get("scrollable") is True
        and probe.get("movement_required") is movement_required
        and probe.get("moved") is moved
        and (not movement_required or moved)
    )


def event_failed(
    event: dict,
    release_profile: str = "full_feature",
    expected_firmware_commit: str | None = None,
    expected_sd_history_mode: str | None = None,
) -> bool:
    probe = event.get("probe") if isinstance(event.get("probe"), dict) else {}
    screen = str(event.get("screen") or "")
    if release_profile == CORE_RELEASE_PROFILE:
        movement_ok = core_probe_result_ok(probe, screen)
        probe_ok = movement_ok
        profile_ok = (
            expected_firmware_commit is not None
            and expected_sd_history_mode is not None
            and exact_identity(
                event.get("health"),
                expected_firmware_commit,
                expected_sd_history_mode,
            )
            and event.get("health", {}).get("cmd") == "health"
        )
    else:
        movement_ok = screen in SCROLL_MOVEMENT_OPTIONAL or (
            probe.get("scrollable") is True and probe.get("moved") is True
        )
        probe_ok = probe.get("ok") is True or (
            screen in SCROLL_MOVEMENT_OPTIONAL and movement_ok
        )
        profile_ok = True
    return (
        not event["request"].get("ok")
        or not event["tab_active"]
        or not probe_ok
        or probe.get("surface") != event["screen"]
        or probe.get("tab") != event["tab"]
        or probe.get("target_found") is not True
        or not movement_ok
        or event["status"].get("active_tab") != event["tab"]
        or not event["health"].get("ok")
        or not profile_ok
        or crashlog_has_crash_like_entries(event["crashlog"])
    )


def wait_for_tab(ser, tab: str, timeout: float, poll_sec: float) -> tuple[bool, list[dict]]:
    deadline = time.monotonic() + timeout
    statuses: list[dict] = []
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        status = send_console_command(ser, "ui status", remaining)
        statuses.append(status)
        if status.get("ok") and status.get("active_tab") == tab and status.get("pending") is False:
            return True, statuses
        time.sleep(min(poll_sec, max(0.0, deadline - time.monotonic())))
    return False, statuses


def _core_identity_failure_report(
    *,
    port: str,
    baud: int,
    started_at: datetime,
    d1l_target: dict,
    expected_firmware_commit: str,
    expected_d1l_public_key: str,
    github_actions_run: str,
    workflow_run_attempt: str,
    expected_sd_history_mode: str,
    setup_events: list[dict],
    clear_crashlog_requested: bool,
    git: dict,
) -> dict:
    ended_at = datetime.now(timezone.utc)
    version = (
        setup_events[0].get("result", {})
        if setup_events
        else {}
    )
    identity_status = (
        setup_events[1].get("result", {})
        if len(setup_events) > 1
        else {}
    )
    return {
        "schema": 2,
        "mode": "hardware",
        "ok": False,
        "closure_eligible": False,
        "hardware_required": True,
        "physical_observed": True,
        "identity_preflight_only": True,
        "port": port,
        "d1l_target": d1l_target,
        "baud": baud,
        "started_at": started_at.isoformat().replace("+00:00", "Z"),
        "ended_at": ended_at.isoformat().replace("+00:00", "Z"),
        "release_profile": CORE_RELEASE_PROFILE,
        "expected_firmware_commit": expected_firmware_commit,
        "expected_d1l_public_key": expected_d1l_public_key,
        "d1l_identity_status": identity_status,
        "d1l_identity_ok": False,
        "github_actions_run": github_actions_run,
        "workflow_run_attempt": workflow_run_attempt,
        "expected_sd_history_mode": expected_sd_history_mode,
        "device_build_commit": version.get("build_commit"),
        "firmware_identity_ok": False,
        "clear_crashlog_before_start": False,
        "clear_crashlog_requested": clear_crashlog_requested,
        "failure_count": 1,
        "failures": [
            {
                "kind": "firmware_identity_preflight",
                "code": "IDENTITY_MISMATCH",
            }
        ],
        "setup_events": setup_events,
        "events": [],
        "git": git,
        **probe_safety(clear_crashlog_before_start=False),
    }


def run_scroll_probe(
    *,
    port: str,
    baud: int,
    timeout: float,
    screens: list[str],
    dwell_sec: float,
    manual_touch: bool,
    clear_crashlog_before_start: bool,
    poll_sec: float,
    release_profile: str = "full_feature",
    expected_firmware_commit: str | None = None,
    github_actions_run: str | None = None,
    workflow_run_attempt: str | None = None,
    expected_sd_history_mode: str | None = None,
    expected_d1l_public_key: str | None = None,
    port_lister: Callable[[], Iterable[object]] | None = None,
    platform_name: str | None = None,
) -> dict:
    validate_setup_actions(screens, clear_crashlog_before_start)
    validate_release_profile_screens(screens, release_profile)
    normalized_public_key: str | None = None
    if release_profile == CORE_RELEASE_PROFILE:
        normalized_public_key = exact_public_key(
            expected_d1l_public_key
        )
        if normalized_public_key is None:
            raise ValueError(
                "Core scroll evidence requires an exact 64-hex D1L "
                "public key"
            )
    root = Path(__file__).resolve().parents[1]
    core_evidence: dict = {}
    if release_profile == CORE_RELEASE_PROFILE:
        core_evidence = validate_core_evidence_inputs(
            port=port,
            expected_firmware_commit=expected_firmware_commit,
            github_actions_run=github_actions_run,
            workflow_run_attempt=workflow_run_attempt,
            expected_sd_history_mode=expected_sd_history_mode,
            root=root,
        )
        port = core_evidence["port"]
        expected_firmware_commit = core_evidence[
            "expected_firmware_commit"
        ]
        github_actions_run = core_evidence["github_actions_run"]
        workflow_run_attempt = core_evidence["workflow_run_attempt"]
        expected_sd_history_mode = core_evidence[
            "expected_sd_history_mode"
        ]
    try:
        import serial
        from serial.tools import list_ports
    except ImportError as exc:
        raise SystemExit("pyserial is required for hardware scroll probe: python -m pip install pyserial") from exc
    d1l_target = None
    if release_profile == CORE_RELEASE_PROFILE:
        d1l_target = resolve_core_target(
            port,
            port_lister=port_lister
            or (lambda: list_ports.comports(include_links=True)),
            platform_name=platform_name,
        )
        port = d1l_target["requested_path"]

    setup_events: list[dict] = []
    events: list[dict] = []
    map_tiles_before: dict | None = None
    map_tiles_after: dict | None = None
    started_at = datetime.now(timezone.utc)
    map_indices = [
        index for index, surface in enumerate(surface_plan(screens))
        if surface["screen"] in MAP_READ_ONLY_SURFACES
    ]
    first_map_index = map_indices[0] if map_indices else None
    last_map_index = map_indices[-1] if map_indices else None

    with open_d1l_serial(serial, port=port, baudrate=baud, timeout=timeout) as ser:
        time.sleep(1.0)
        ser.reset_input_buffer()

        if release_profile == CORE_RELEASE_PROFILE:
            version = send_console_command(ser, "version", timeout)
            setup_events.append(
                {"cmd": "version", "result": version}
            )
            if not exact_version_identity(
                version,
                expected_firmware_commit,
                expected_sd_history_mode,
            ):
                return _core_identity_failure_report(
                    port=port,
                    baud=baud,
                    started_at=started_at,
                    d1l_target=d1l_target,
                    expected_firmware_commit=expected_firmware_commit,
                    expected_d1l_public_key=normalized_public_key,
                    github_actions_run=github_actions_run,
                    workflow_run_attempt=workflow_run_attempt,
                    expected_sd_history_mode=expected_sd_history_mode,
                    setup_events=setup_events,
                    clear_crashlog_requested=clear_crashlog_before_start,
                    git=core_evidence["git"],
                )
            d1l_identity_status = send_console_command(
                ser, "identity status", timeout
            )
            setup_events.append(
                {
                    "cmd": "identity status",
                    "result": d1l_identity_status,
                }
            )
            if not d1l_identity_status_ok(
                d1l_identity_status,
                normalized_public_key,
            ):
                return _core_identity_failure_report(
                    port=port,
                    baud=baud,
                    started_at=started_at,
                    d1l_target=d1l_target,
                    expected_firmware_commit=expected_firmware_commit,
                    expected_d1l_public_key=normalized_public_key,
                    github_actions_run=github_actions_run,
                    workflow_run_attempt=workflow_run_attempt,
                    expected_sd_history_mode=expected_sd_history_mode,
                    setup_events=setup_events,
                    clear_crashlog_requested=clear_crashlog_before_start,
                    git=core_evidence["git"],
                )
            health_preflight = send_console_command(ser, "health", timeout)
            setup_events.append(
                {"cmd": "health", "result": health_preflight}
            )
            preflight_ok = (
                exact_identity(
                    health_preflight,
                    expected_firmware_commit,
                    expected_sd_history_mode,
                )
                and health_preflight.get("cmd") == "health"
            )
            if not preflight_ok:
                return _core_identity_failure_report(
                    port=port,
                    baud=baud,
                    started_at=started_at,
                    d1l_target=d1l_target,
                    expected_firmware_commit=expected_firmware_commit,
                    expected_d1l_public_key=normalized_public_key,
                    github_actions_run=github_actions_run,
                    workflow_run_attempt=workflow_run_attempt,
                    expected_sd_history_mode=expected_sd_history_mode,
                    setup_events=setup_events,
                    clear_crashlog_requested=clear_crashlog_before_start,
                    git=core_evidence["git"],
                )

        if clear_crashlog_before_start:
            setup_events.append(
                {"cmd": "crashlog clear", "result": send_console_command(ser, "crashlog clear", timeout)}
            )

        for surface_index, surface in enumerate(surface_plan(screens)):
            screen = surface["screen"]
            tab = surface["tab"]
            label = surface["label"]
            if surface_index == first_map_index:
                map_tiles_before = send_console_command(ser, "map tiles status", timeout)
            if screen in MAP_READ_ONLY_SURFACES:
                # The probe command arms firmware-side network suppression before
                # navigating to Map. Never open the live Map with `ui tab map`
                # from an evidence probe.
                probe = send_console_command(ser, f"ui scroll-probe {screen}", max(timeout, 15.0))
                request = probe
                time.sleep(dwell_sec)
                tab_active, statuses = wait_for_tab(ser, tab, max(timeout, 15.0), poll_sec)
            else:
                request = send_console_command(ser, f"ui tab {tab}", timeout)
                time.sleep(dwell_sec)
                tab_active, statuses = wait_for_tab(ser, tab, max(timeout, 15.0), poll_sec)
                probe = send_console_command(ser, f"ui scroll-probe {screen}", max(timeout, 15.0))
            if manual_touch:
                print(
                    f"Manual check: inspect the {label} surface and scroll "
                    "if content permits, then press Enter."
                )
                input()
            status = send_console_command(ser, "ui status", timeout)
            health = send_console_command(ser, "health", timeout)
            crashlog = send_console_command(ser, "crashlog", timeout)
            events.append(
                {
                    "screen": screen,
                    "tab": tab,
                    "label": label,
                    "request": request,
                    "tab_active": tab_active,
                    "statuses": statuses,
                    "probe": probe,
                    "status": status,
                    "health": health,
                    "crashlog": crashlog,
                    "manual_touch_confirmed": manual_touch,
                }
            )
            if surface_index == last_map_index:
                map_tiles_after = send_console_command(ser, "map tiles status", timeout)

    ended_at = datetime.now(timezone.utc)
    failures = [
        {
            "screen": event["screen"],
            "request_ok": event["request"].get("ok"),
            "tab_active": event["tab_active"],
            "probe_ok": event["probe"].get("ok"),
            "probe_surface": event["probe"].get("surface"),
            "probe_tab": event["probe"].get("tab"),
            "target_found": event["probe"].get("target_found"),
            "scrollable": event["probe"].get("scrollable"),
            "moved": event["probe"].get("moved"),
            "active_tab": event["status"].get("active_tab"),
            "expected_active_tab": event["tab"],
            "health_ok": event["health"].get("ok"),
            "crashlog_crash_like_entries": crashlog_has_crash_like_entries(event["crashlog"]),
            "movement_required": (
                event["probe"].get("movement_required")
                if release_profile == CORE_RELEASE_PROFILE
                else event["screen"] not in SCROLL_MOVEMENT_OPTIONAL
            ),
        }
        for event in events
        if event_failed(
            event,
            release_profile,
            expected_firmware_commit,
            expected_sd_history_mode,
        )
    ]
    map_network_evidence = summarize_map_network_evidence(
        map_tiles_before,
        map_tiles_after,
        measured=bool(map_indices),
    )
    map_network_failures = []
    if map_indices and map_network_evidence.get("unchanged") is not True:
        map_network_failures.append(
            {
                "kind": "map_network_counter",
                "samples_valid": map_network_evidence.get("samples_valid"),
                "before_count": map_network_evidence.get("before_count"),
                "after_count": map_network_evidence.get("after_count"),
                "delta": map_network_evidence.get("delta"),
            }
        )
    failures.extend(map_network_failures)
    setup_failures = [
        event for event in setup_events if not event.get("result", {}).get("ok")
    ]
    network_assertion = (
        False if not map_indices or map_network_evidence.get("unchanged") is True
        else True if map_network_evidence.get("samples_valid") is True
        else None
    )
    safety = probe_safety(clear_crashlog_before_start=clear_crashlog_before_start)
    safety["network_tx"] = network_assertion
    safety["map_network_requests"] = network_assertion
    profile_evidence = {}
    if release_profile == CORE_RELEASE_PROFILE:
        firmware_identity_ok = all(
            exact_identity(
                event.get("health"),
                expected_firmware_commit,
                expected_sd_history_mode,
            )
            and event.get("health", {}).get("cmd") == "health"
            for event in events
        )
        profile_evidence = {
            "closure_eligible": True,
            "hardware_required": True,
            "physical_observed": True,
            "identity_preflight_only": False,
            "release_profile": release_profile,
            "expected_firmware_commit": expected_firmware_commit,
            "github_actions_run": github_actions_run,
            "workflow_run_attempt": workflow_run_attempt,
            "expected_sd_history_mode": expected_sd_history_mode,
            "device_build_commit": (
                setup_events[0]
                .get("result", {})
                .get("build_commit")
            ),
            "firmware_identity_ok": firmware_identity_ok,
            "expected_d1l_public_key": normalized_public_key,
            "d1l_identity_status": d1l_identity_status,
            "d1l_identity_ok": True,
            "scroll_movement_policy": "positive_raw_overflow",
            "d1l_target": d1l_target,
        }
    return {
        "schema": (
            2 if release_profile == CORE_RELEASE_PROFILE else 1
        ),
        "mode": "hardware",
        "port": port,
        "baud": baud,
        "started_at": started_at.isoformat().replace("+00:00", "Z"),
        "ended_at": ended_at.isoformat().replace("+00:00", "Z"),
        "screens": screens,
        "surface_plan": surface_plan(screens),
        "dwell_sec": dwell_sec,
        "manual_touch": manual_touch,
        **profile_evidence,
        "clear_crashlog_before_start": clear_crashlog_before_start,
        "scroll_movement_optional": (
            []
            if release_profile == CORE_RELEASE_PROFILE
            else sorted(SCROLL_MOVEMENT_OPTIONAL)
        ),
        "ok": not failures and not setup_failures,
        "failure_count": len(failures) + len(setup_failures),
        "failures": failures,
        "setup_events": setup_events,
        "probe_results": {event["screen"]: event["probe"] for event in events},
        "events": events,
        "map_network_evidence": map_network_evidence,
        **safety,
    }


def resolve_out_path(out_arg: str | None, mode: str) -> Path:
    root = Path(__file__).resolve().parents[1]
    if out_arg:
        out_path = Path(out_arg)
        if not out_path.is_absolute():
            out_path = root / out_path
        return out_path
    return root / "artifacts" / "scroll-probe" / f"d1l-scroll-probe-{mode}-{utc_stamp()}.json"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--screens", default=",".join(DEFAULT_SCREENS))
    parser.add_argument("--dwell-sec", type=float, default=0.5)
    parser.add_argument("--poll-sec", type=float, default=0.05)
    parser.add_argument("--manual-touch", action="store_true")
    parser.add_argument("--clear-crashlog-before-start", action="store_true")
    parser.add_argument(
        "--release-profile",
        choices=RELEASE_PROFILES,
        default="full_feature",
    )
    parser.add_argument("--expected-firmware-commit")
    parser.add_argument("--github-actions-run")
    parser.add_argument("--workflow-run-attempt")
    parser.add_argument(
        "--expected-sd-history-mode",
        choices=sorted(SD_HISTORY_MODES),
    )
    parser.add_argument("--expected-d1l-public-key")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    if args.dwell_sec < 0:
        parser.error("--dwell-sec cannot be negative")
    if args.poll_sec <= 0:
        parser.error("--poll-sec must be positive")
    try:
        screens = parse_screens(args.screens)
    except ValueError as exc:
        parser.error(str(exc))
    if not screens:
        parser.error("--screens must include at least one screen")
    try:
        validate_setup_actions(screens, args.clear_crashlog_before_start)
        validate_release_profile_screens(screens, args.release_profile)
    except ValueError as exc:
        parser.error(str(exc))

    if args.dry_run:
        report = dry_run_report(
            screens,
            args.dwell_sec,
            args.manual_touch,
            args.release_profile,
        )
        mode = "dry-run"
    else:
        if not args.port:
            parser.error("No D1L port supplied. Set D1L_PORT or pass --port.")
        try:
            report = run_scroll_probe(
                port=args.port,
                baud=args.baud,
                timeout=args.timeout,
                screens=screens,
                dwell_sec=args.dwell_sec,
                manual_touch=args.manual_touch,
                clear_crashlog_before_start=args.clear_crashlog_before_start,
                poll_sec=args.poll_sec,
                release_profile=args.release_profile,
                expected_firmware_commit=(
                    args.expected_firmware_commit
                ),
                github_actions_run=args.github_actions_run,
                workflow_run_attempt=args.workflow_run_attempt,
                expected_sd_history_mode=(
                    args.expected_sd_history_mode
                ),
                expected_d1l_public_key=args.expected_d1l_public_key,
            )
        except ValueError as exc:
            parser.error(str(exc))
        mode = "hardware"

    out_path = resolve_out_path(args.out, mode)
    stamp_report(report, Path(__file__).resolve().parents[1])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2), encoding="ascii")
    print(json.dumps(report))
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
