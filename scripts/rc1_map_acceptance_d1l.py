#!/usr/bin/env python3
"""Produce the bounded RC1 Map acceptance transcript on the Pi-hosted D1L.

This runner accepts no provider URL, location, SSID, or password. Those inputs
must already be configured on the device and SD card. It opens only the stable
D1L by-id endpoint on ``neopi5`` and proves one fresh authorized background
HTTPS tile fetch followed by an offline, SD-cached revisit of the configured
current Map view. OpenStreetMap Standard is never accepted for bulk prefetch.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

try:
    from artifact_metadata import git_metadata
    from core_smoke_d1l import CORE_RELEASE_PROFILE, resolve_core_target
    from d1l_serial_target import POSIX_D1L_TARGET, validate_snapshot
    from produce_rc1_bounded_physical_receipt_d1l import (
        EvidenceError,
        validate_map,
    )
    from scroll_probe_d1l import crashlog_has_crash_like_entries
    from smoke_d1l import exact_commit, open_d1l_serial, send_console_command
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import git_metadata
    from scripts.core_smoke_d1l import CORE_RELEASE_PROFILE, resolve_core_target
    from scripts.d1l_serial_target import POSIX_D1L_TARGET, validate_snapshot
    from scripts.produce_rc1_bounded_physical_receipt_d1l import (
        EvidenceError,
        validate_map,
    )
    from scripts.scroll_probe_d1l import crashlog_has_crash_like_entries
    from scripts.smoke_d1l import (
        exact_commit,
        open_d1l_serial,
        send_console_command,
    )


TRANSCRIPT_KIND = "d1l_rc1_map_acceptance_transcript"
PI_HOST = "neopi5"
EXPECTED_SD_HISTORY_MODE = "conditional"
OSM_STANDARD_SOURCE = "openstreetmap-standard"
MIN_32GB_CLASS_CAPACITY_KB = 28_000_000
MIN_PROVIDER_ZOOM = 14
MAX_PREFETCH_ZOOM = 18
NODE_RADIUS_KM = 200
CONSOLE_BAUD = 115200
COMMAND_TIMEOUT_SECONDS = 20.0
BOOT_TIMEOUT_SECONDS = 45.0
POLL_INTERVAL_SECONDS = 1.0
PREFETCH_TIMEOUT_SECONDS = 180.0
OFFLINE_VIEW_TIMEOUT_SECONDS = 120.0
WIFI_TIMEOUT_SECONDS = 45.0
UI_TIMEOUT_SECONDS = 20.0

COMMANDS = frozenset(
    {
        "version",
        "health",
        "crashlog",
        "ui status",
        "map center",
        "map acceptance status",
        "map acceptance open",
        "map tiles status",
        "wifi status",
        "wifi off",
        "wifi on",
        "wifi connect",
        "ui tab home",
        "ui tab messages",
        "ui tab nodes",
        "ui tab map",
        "ui tab packets",
        "ui tab settings",
    }
)
UI_TABS = frozenset({"home", "messages", "nodes", "map", "packets", "settings"})
COMMIT_RE = re.compile(r"[0-9a-f]{40}\Z")
POSITIVE_DECIMAL_RE = re.compile(r"[1-9][0-9]*\Z")


class AcceptanceFailure(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def _integer(value: object, *, minimum: int | None = None) -> int | None:
    if type(value) is not int:
        return None
    if minimum is not None and value < minimum:
        return None
    return value


def positive_decimal(value: object, label: str) -> str:
    normalized = str(value or "").strip()
    if not POSITIVE_DECIMAL_RE.fullmatch(normalized):
        raise ValueError(f"{label} must be a positive decimal identifier")
    return normalized


def exact_expected_commit(value: object) -> str:
    normalized = exact_commit(value)
    if normalized is None or not COMMIT_RE.fullmatch(normalized):
        raise ValueError("expected firmware commit must be exactly 40 hex digits")
    return normalized


def require_result(result: object, command: str) -> dict[str, Any]:
    if type(result) is not dict or result.get("schema") != 1:
        raise AcceptanceFailure(
            "response_schema_invalid",
            f"{command} did not return a schema-1 object",
        )
    if result.get("cmd") != command:
        raise AcceptanceFailure(
            "response_identity_mismatch",
            f"{command} returned a different command identity",
        )
    if result.get("ok") is not True:
        code = result.get("code")
        raise AcceptanceFailure(
            "command_failed",
            f"{command} failed with {code if isinstance(code, str) else 'DEVICE_ERROR'}",
        )
    return result


def validate_version(result: object, expected_commit: str) -> dict[str, Any]:
    row = require_result(result, "version")
    if (
        exact_commit(row.get("build_commit")) != expected_commit
        or row.get("release_profile") != CORE_RELEASE_PROFILE
        or row.get("sd_history_mode") != EXPECTED_SD_HISTORY_MODE
    ):
        raise AcceptanceFailure(
            "firmware_identity_mismatch",
            "device firmware is not the exact conditional-SD Core candidate",
        )
    return row


def validate_health(result: object, *, boot_nonce: int | None = None) -> dict[str, Any]:
    row = require_result(result, "health")
    nonce = _integer(row.get("boot_nonce"), minimum=1)
    if (
        nonce is None
        or row.get("board_ready") is not True
        or row.get("ui_ready") is not True
        or row.get("release_profile") != CORE_RELEASE_PROFILE
        or row.get("sd_history_mode") != EXPECTED_SD_HISTORY_MODE
    ):
        raise AcceptanceFailure(
            "health_invalid", "device health is not release-ready"
        )
    if boot_nonce is not None and nonce != boot_nonce:
        raise AcceptanceFailure(
            "unexpected_reboot", "device boot nonce changed during Map acceptance"
        )
    return row


def validate_crashlog(result: object) -> dict[str, Any]:
    row = require_result(result, "crashlog")
    if crashlog_has_crash_like_entries(row):
        raise AcceptanceFailure(
            "crash_evidence_present", "crashlog contains a crash-like entry"
        )
    return row


def validate_connected_wifi(result: object, *, expected_ssid: str | None = None) -> dict[str, Any]:
    row = require_result(result, "wifi status")
    ssid = row.get("ssid")
    if (
        row.get("available") is not True
        or row.get("setting_enabled") is not True
        or row.get("build_enabled") is not True
        or row.get("stack_active") is not True
        or row.get("connected") is not True
        or row.get("connecting") is not False
        or row.get("profile_saved") is not True
        or row.get("password_saved") is not True
        or row.get("state") != "connected"
        or row.get("live_network") is not True
        or not isinstance(ssid, str)
        or not ssid
        or (expected_ssid is not None and ssid != expected_ssid)
    ):
        raise AcceptanceFailure(
            "saved_wifi_not_connected",
            "a connected, pre-provisioned saved Wi-Fi profile is required",
        )
    return row


def wifi_is_offline(result: object) -> bool:
    row = require_result(result, "wifi status")
    return (
        row.get("setting_enabled") is False
        and row.get("connected") is False
        and row.get("connecting") is False
        and row.get("live_network") is False
    )


def wifi_is_connected_to(result: object, expected_ssid: str | None) -> bool:
    if type(result) is not dict:
        return False
    return (
        result.get("available") is True
        and result.get("setting_enabled") is True
        and result.get("build_enabled") is True
        and result.get("stack_active") is True
        and result.get("connected") is True
        and result.get("connecting") is False
        and result.get("profile_saved") is True
        and result.get("password_saved") is True
        and result.get("state") == "connected"
        and result.get("live_network") is True
        and isinstance(result.get("ssid"), str)
        and bool(result["ssid"])
        and (
            expected_ssid is None
            or result.get("ssid") == expected_ssid
        )
    )


def validate_map_center(result: object) -> dict[str, Any]:
    row = require_result(result, "map center")
    location = row.get("map_location")
    if type(location) is not dict:
        raise AcceptanceFailure(
            "location_missing", "configured device location is required"
        )
    lat_e7 = _integer(location.get("lat_e7"))
    lon_e7 = _integer(location.get("lon_e7"))
    if (
        location.get("set") is not True
        or lat_e7 is None
        or lon_e7 is None
        or not -900_000_000 <= lat_e7 <= 900_000_000
        or not -1_800_000_000 <= lon_e7 <= 1_800_000_000
        or location.get("source") not in {"manual", "authenticated_companion"}
    ):
        raise AcceptanceFailure(
            "location_invalid", "configured device location is invalid"
        )
    return {
        "set": True,
        "lat_e7": lat_e7,
        "lon_e7": lon_e7,
        "source": location["source"],
    }


def validate_provider_status(
    result: object,
    *,
    location: dict[str, Any],
    require_plan: bool,
) -> dict[str, Any]:
    row = require_result(result, "map acceptance status")
    source_id = row.get("source_id")
    attribution = row.get("attribution")
    license_url = row.get("license_url")
    device_location = row.get("device_location")
    wifi = row.get("wifi")
    sd = row.get("sd")
    markers = row.get("node_markers")
    prefetch = row.get("prefetch")
    if (
        row.get("configured") is not True
        or row.get("authorized_provider") is not True
        or row.get("provider_refresh_ok") is not True
        or row.get("https") is not True
        or row.get("network_fetch_allowed") is not True
        or row.get("offline_storage_permitted") is not True
        or row.get("background_prefetch_permitted") is not True
        or row.get("osm_standard_endpoint") is not False
        or row.get("network_url_redacted") is not True
        or "network_url_template" in row
        or not isinstance(source_id, str)
        or not source_id
        or source_id == OSM_STANDARD_SOURCE
        or not isinstance(attribution, str)
        or not attribution.strip()
        or not isinstance(license_url, str)
        or not license_url.startswith("https://")
    ):
        raise AcceptanceFailure(
            "provider_not_authorized",
            "SD provider is not an authorized non-OSM HTTPS offline source",
        )
    if (
        type(device_location) is not dict
        or device_location.get("set") is not True
        or device_location.get("lat_e7") != location["lat_e7"]
        or device_location.get("lon_e7") != location["lon_e7"]
        or device_location.get("source") != location["source"]
    ):
        raise AcceptanceFailure(
            "provider_location_mismatch",
            "background Map center does not match the configured device location",
        )
    if (
        type(wifi) is not dict
        or wifi.get("setting_enabled") is not True
        or wifi.get("profile_saved") is not True
        or wifi.get("connected") is not True
        or type(sd) is not dict
        or sd.get("ready") is not True
        or _integer(sd.get("capacity_kb"), minimum=MIN_32GB_CLASS_CAPACITY_KB)
        is None
    ):
        raise AcceptanceFailure(
            "map_runtime_prerequisite_missing",
            "connected Wi-Fi and a prepared 32 GB-class SD card are required",
        )
    if row.get("node_radius_km") != NODE_RADIUS_KM:
        raise AcceptanceFailure(
            "node_radius_invalid", "Map node radius must be capped at 200 km"
        )
    if type(markers) is not dict or type(prefetch) is not dict:
        raise AcceptanceFailure(
            "prefetch_schema_invalid", "Map prefetch evidence is incomplete"
        )
    if require_plan:
        seen = _integer(markers.get("seen"), minimum=1)
        included = _integer(markers.get("included"), minimum=1)
        outside = _integer(markers.get("outside_radius"), minimum=0)
        selected_zoom = _integer(
            prefetch.get("selected_max_zoom"), minimum=MIN_PROVIDER_ZOOM
        )
        provider_zoom = _integer(
            row.get("provider_max_zoom"), minimum=MIN_PROVIDER_ZOOM
        )
        if (
            seen is None
            or included is None
            or outside is None
            or included + outside != seen
            or row.get("nodes_accounted") is not True
            or selected_zoom is None
            or provider_zoom is None
            or selected_zoom > min(provider_zoom, MAX_PREFETCH_ZOOM)
            or prefetch.get("initialized") is not True
            or prefetch.get("eligible") is not True
            or prefetch.get("location_set") is not True
            or prefetch.get("wifi_connected") is not True
            or prefetch.get("sd_ready") is not True
            or prefetch.get("provider_configured") is not True
            or prefetch.get("background_prefetch_permitted") is not True
            or prefetch.get("source_id") != source_id
            or prefetch.get("storage_reserve_reached") is not False
            or _integer(prefetch.get("total_tiles"), minimum=1) is None
            or _integer(prefetch.get("network_requests"), minimum=0) is None
            or _integer(prefetch.get("downloaded_tiles"), minimum=0) is None
            or _integer(prefetch.get("failed_tiles"), minimum=0) != 0
        ):
            raise AcceptanceFailure(
                "prefetch_plan_invalid",
                "200 km node plan is not ready at the highest supported detail",
            )
    return row


def provider_plan_ready(result: object) -> bool:
    if type(result) is not dict:
        return False
    markers = result.get("node_markers")
    prefetch = result.get("prefetch")
    return (
        type(markers) is dict
        and type(prefetch) is dict
        and prefetch.get("initialized") is True
        and prefetch.get("eligible") is True
        and _integer(markers.get("seen"), minimum=1) is not None
        and _integer(markers.get("included"), minimum=1) is not None
        and _integer(prefetch.get("selected_max_zoom"), minimum=MIN_PROVIDER_ZOOM)
        is not None
        and _integer(prefetch.get("total_tiles"), minimum=1) is not None
    )


def validate_download_progress(
    result: object,
    baseline: dict[str, Any],
    *,
    location: dict[str, Any],
) -> dict[str, Any]:
    row = validate_provider_status(
        result, location=location, require_plan=True
    )
    before = baseline["prefetch"]
    after = row["prefetch"]
    if (
        row.get("source_id") != baseline.get("source_id")
        or row.get("provider_max_zoom") != baseline.get("provider_max_zoom")
        or row.get("node_markers", {}).get("generation")
        != baseline.get("node_markers", {}).get("generation")
        or after.get("selected_max_zoom") != before.get("selected_max_zoom")
        or after.get("total_tiles") != before.get("total_tiles")
    ):
        raise AcceptanceFailure(
            "prefetch_plan_changed",
            "provider or node plan changed during online acceptance",
        )
    before_requests = _integer(before.get("network_requests"), minimum=0)
    after_requests = _integer(after.get("network_requests"), minimum=0)
    before_downloaded = _integer(before.get("downloaded_tiles"), minimum=0)
    after_downloaded = _integer(after.get("downloaded_tiles"), minimum=0)
    if None in {
        before_requests,
        after_requests,
        before_downloaded,
        after_downloaded,
    }:
        raise AcceptanceFailure(
            "prefetch_counter_invalid", "Map prefetch counters are invalid"
        )
    if (
        after_requests <= before_requests
        or after_downloaded <= before_downloaded
    ):
        raise AcceptanceFailure(
            "fresh_download_not_observed",
            "no fresh authorized background tile was fetched and cached",
        )
    return row


def offline_view_ready(
    result: object,
    *,
    generation_before: int,
    source_id: str,
    location: dict[str, Any],
) -> bool:
    row = require_result(result, "map tiles status")
    planned = _integer(row.get("planned_tiles"), minimum=1)
    return (
        row.get("visible") is True
        and row.get("worker_running") is False
        and row.get("frame_ready") is True
        and row.get("sd_cache_ready") is True
        and row.get("wifi_connected") is False
        and row.get("rate_limited") is False
        and row.get("current_view_only") is True
        and row.get("provider_configured") is True
        and row.get("background_prefetch_permitted") is True
        and row.get("source") == source_id
        and _integer(row.get("generation"), minimum=generation_before + 1)
        is not None
        and row.get("lat_e7") == location["lat_e7"]
        and row.get("lon_e7") == location["lon_e7"]
        and planned is not None
        and row.get("attempted_tiles") == planned
        and row.get("rendered_tiles") == planned
        and _integer(row.get("cache_hits"), minimum=1) is not None
        and row.get("network_requests") == 0
        and row.get("downloaded_tiles") == 0
        and row.get("failed_tiles") == 0
    )


def online_view_ready(
    result: object,
    *,
    generation_before: int,
    source_id: str,
    location: dict[str, Any],
) -> bool:
    row = require_result(result, "map tiles status")
    planned = _integer(row.get("planned_tiles"), minimum=1)
    cache_hits = _integer(row.get("cache_hits"), minimum=0)
    downloaded = _integer(row.get("downloaded_tiles"), minimum=0)
    return (
        row.get("visible") is True
        and row.get("worker_running") is False
        and row.get("frame_ready") is True
        and row.get("sd_cache_ready") is True
        and row.get("wifi_connected") is True
        and row.get("rate_limited") is False
        and row.get("current_view_only") is True
        and row.get("provider_configured") is True
        and row.get("background_prefetch_permitted") is True
        and row.get("source") == source_id
        and _integer(row.get("generation"), minimum=generation_before + 1)
        is not None
        and row.get("lat_e7") == location["lat_e7"]
        and row.get("lon_e7") == location["lon_e7"]
        and planned is not None
        and cache_hits is not None
        and downloaded is not None
        and cache_hits + downloaded == planned
        and row.get("attempted_tiles") == planned
        and row.get("rendered_tiles") == planned
        and row.get("failed_tiles") == 0
    )


def validate_acceptance_open(result: object) -> dict[str, Any]:
    row = require_result(result, "map acceptance open")
    if (
        row.get("configured_current_view_only") is not True
        or row.get("configured_device_center") is not True
        or row.get("forced_sd_reload") is not True
        or row.get("arbitrary_url_accepted") is not False
        or row.get("arbitrary_location_accepted") is not False
        or row.get("public_rf_tx") is not False
        or row.get("formats_sd") is not False
    ):
        raise AcceptanceFailure(
            "map_open_safety_invalid",
            "Map acceptance opener did not preserve its safety boundary",
        )
    return row


def build_transcript(
    *,
    version: dict[str, Any],
    provider: dict[str, Any],
    baseline: dict[str, Any],
    download: dict[str, Any],
    online_view: dict[str, Any],
    revisit: dict[str, Any],
    health: dict[str, Any],
    crashlog: dict[str, Any],
    target_before: dict[str, Any],
    target_after: dict[str, Any],
    runner_commit: str,
    runner_source_clean: bool,
    expected_commit: str,
    actions_run: str,
    workflow_run_attempt: str,
) -> dict[str, Any]:
    before_prefetch = baseline["prefetch"]
    download_prefetch = download["prefetch"]
    before_requests = int(before_prefetch["network_requests"])
    download_requests = int(download_prefetch["network_requests"])
    downloaded_delta = (
        int(download_prefetch["downloaded_tiles"])
        - int(before_prefetch["downloaded_tiles"])
    )
    revisit_response = {
        "ok": True,
        "offline": True,
        "frame_ready": revisit["frame_ready"],
        "cache_hits": revisit["cache_hits"],
        "network_requests": download_requests,
        "view_network_requests": revisit["network_requests"],
        "source_id": revisit["source"],
        "raw": revisit,
    }
    step_rows = (
        ("version", "version", version),
        ("provider", "map acceptance status", provider),
        (
            "before",
            "map acceptance status",
            {
                "ok": True,
                "network_requests": before_requests,
                "downloaded_tiles": before_prefetch["downloaded_tiles"],
                "raw": baseline,
            },
        ),
        (
            "download",
            "map acceptance status",
            {
                "ok": True,
                "online": True,
                "sd_cache_ready": download["sd"]["ready"],
                "network_requests": download_requests,
                "downloaded_tiles": downloaded_delta,
                "total_downloaded_tiles": download_prefetch["downloaded_tiles"],
                "current_view_cache_fill": online_view,
                "raw": download,
            },
        ),
        ("revisit", "map acceptance open", revisit_response),
        ("health", "health", health),
        ("crashlog", "crashlog", crashlog),
    )
    return {
        "schema": 1,
        "kind": TRANSCRIPT_KIND,
        "mode": "hardware",
        "physical_observed": True,
        "simulated": False,
        "dry_run": False,
        "manual_only": False,
        "port": POSIX_D1L_TARGET,
        "d1l_target": target_before,
        "d1l_target_after": target_after,
        "runner_commit": runner_commit,
        "runner_source_clean": runner_source_clean,
        "expected_firmware_commit": expected_commit,
        "github_actions_run": actions_run,
        "workflow_run_attempt": workflow_run_attempt,
        "steps": [
            {
                "sequence": sequence,
                "operation": operation,
                "command": command,
                "response": response,
            }
            for sequence, (operation, command, response) in enumerate(
                step_rows, start=1
            )
        ],
    }


def send_checked(
    ser: Any,
    command: str,
    timeout: float,
) -> dict[str, Any]:
    if command not in COMMANDS:
        raise AcceptanceFailure(
            "command_not_allowlisted",
            f"refusing command outside Map acceptance allowlist: {command}",
        )
    return send_console_command(ser, command, timeout)


def wait_for_console_ready(
    ser: Any,
    *,
    timeout: float,
    command_timeout: float,
    interval: float,
) -> dict[str, Any]:
    """Retry health because a command sent before console init is discarded."""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AcceptanceFailure(
                "console_not_ready",
                "D1L console did not become ready after cold boot",
            )
        result = send_checked(
            ser,
            "health",
            min(command_timeout, remaining),
        )
        if (
            type(result) is dict
            and result.get("ok") is True
            and result.get("cmd") == "health"
            and result.get("board_ready") is True
            and result.get("ui_ready") is True
        ):
            return validate_health(result)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AcceptanceFailure(
                "console_not_ready",
                "D1L console did not become ready after cold boot",
            )
        time.sleep(min(interval, remaining))


def poll_command(
    ser: Any,
    command: str,
    *,
    timeout: float,
    command_timeout: float,
    interval: float,
    predicate: Callable[[dict[str, Any]], bool],
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] = {}
    while True:
        last = require_result(
            send_checked(ser, command, command_timeout), command
        )
        if predicate(last):
            return last
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AcceptanceFailure(
                "acceptance_timeout",
                f"{command} did not reach the required state",
            )
        time.sleep(min(interval, remaining))


def wait_for_ui_tab(
    ser: Any,
    tab: str,
    *,
    command_timeout: float,
    interval: float,
) -> dict[str, Any]:
    return poll_command(
        ser,
        "ui status",
        timeout=UI_TIMEOUT_SECONDS,
        command_timeout=command_timeout,
        interval=interval,
        predicate=lambda row: (
            row.get("started") is True
            and row.get("pending") is False
            and row.get("active_tab") == tab
        ),
    )


def _resolve_pi_target(port_lister: Callable[[], Any]) -> dict[str, Any]:
    if os.name != "posix":
        raise AcceptanceFailure(
            "pi_required", "Map acceptance must run directly on neopi5"
        )
    snapshot = resolve_core_target(
        POSIX_D1L_TARGET,
        port_lister=port_lister,
        platform_name="posix",
    )
    validate_snapshot(snapshot, POSIX_D1L_TARGET)
    if snapshot.get("hostname") != PI_HOST or socket.gethostname() != PI_HOST:
        raise AcceptanceFailure(
            "pi_identity_mismatch", "Map acceptance host must be neopi5"
        )
    return snapshot


def run_acceptance(
    *,
    runner_commit: str,
    runner_source_clean: bool,
    expected_commit: str,
    actions_run: str,
    workflow_run_attempt: str,
    command_timeout: float,
    boot_timeout: float,
    poll_interval: float,
    prefetch_timeout: float,
    offline_timeout: float,
    wifi_timeout: float,
) -> dict[str, Any]:
    try:
        import serial
        from serial.tools import list_ports
    except ImportError as exc:
        raise AcceptanceFailure(
            "pyserial_required", "install pyserial on neopi5"
        ) from exc

    target_before = _resolve_pi_target(list_ports.comports)
    original_tab: str | None = None
    original_ssid: str | None = None
    map_was_hidden = False
    wifi_restore_needed = False
    primary_error: BaseException | None = None
    cleanup_errors: list[str] = []
    version: dict[str, Any] = {}
    provider: dict[str, Any] = {}
    baseline: dict[str, Any] = {}
    download: dict[str, Any] = {}
    online_view: dict[str, Any] = {}
    revisit: dict[str, Any] = {}
    final_health: dict[str, Any] = {}
    final_crashlog: dict[str, Any] = {}
    initial_nonce: int | None = None

    with open_d1l_serial(
        serial,
        port=POSIX_D1L_TARGET,
        baudrate=CONSOLE_BAUD,
        timeout=command_timeout,
    ) as ser:
        ser.reset_input_buffer()
        try:
            initial_health = wait_for_console_ready(
                ser,
                timeout=boot_timeout,
                command_timeout=command_timeout,
                interval=poll_interval,
            )
            version = validate_version(
                send_checked(ser, "version", command_timeout),
                expected_commit,
            )
            initial_nonce = int(initial_health["boot_nonce"])
            validate_crashlog(
                send_checked(ser, "crashlog", command_timeout)
            )
            ui = poll_command(
                ser,
                "ui status",
                timeout=UI_TIMEOUT_SECONDS,
                command_timeout=command_timeout,
                interval=poll_interval,
                predicate=lambda row: (
                    row.get("started") is True
                    and row.get("pending") is False
                    and row.get("active_tab") in UI_TABS
                ),
            )
            original_tab = str(ui["active_tab"])
            wifi = validate_connected_wifi(
                send_checked(ser, "wifi status", command_timeout)
            )
            original_ssid = str(wifi["ssid"])
            location = validate_map_center(
                send_checked(ser, "map center", command_timeout)
            )

            if original_tab == "map":
                require_result(
                    send_checked(ser, "ui tab home", command_timeout),
                    "ui tab",
                )
                wait_for_ui_tab(
                    ser,
                    "home",
                    command_timeout=command_timeout,
                    interval=poll_interval,
                )
                map_was_hidden = True

            provider = poll_command(
                ser,
                "map acceptance status",
                timeout=UI_TIMEOUT_SECONDS,
                command_timeout=command_timeout,
                interval=poll_interval,
                predicate=provider_plan_ready,
            )
            provider = validate_provider_status(
                provider, location=location, require_plan=True
            )
            baseline = provider
            if baseline["prefetch"].get("complete") is True:
                raise AcceptanceFailure(
                    "fresh_tile_required",
                    "prefetch is already complete; provide one uncached authorized tile",
                )

            deadline = time.monotonic() + prefetch_timeout
            while True:
                candidate = send_checked(
                    ser, "map acceptance status", command_timeout
                )
                try:
                    download = validate_download_progress(
                        candidate, baseline, location=location
                    )
                    break
                except AcceptanceFailure as exc:
                    if exc.code != "fresh_download_not_observed":
                        raise
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AcceptanceFailure(
                        "prefetch_timeout",
                        "no fresh authorized background tile completed in time",
                    )
                time.sleep(min(poll_interval, remaining))

            before_online = require_result(
                send_checked(ser, "map tiles status", command_timeout),
                "map tiles status",
            )
            online_generation_before = _integer(
                before_online.get("generation"), minimum=0
            )
            if online_generation_before is None:
                raise AcceptanceFailure(
                    "map_generation_invalid",
                    "Map view generation counter is invalid",
                )
            validate_acceptance_open(
                send_checked(ser, "map acceptance open", command_timeout)
            )
            online_view = poll_command(
                ser,
                "map tiles status",
                timeout=offline_timeout,
                command_timeout=command_timeout,
                interval=poll_interval,
                predicate=lambda row: online_view_ready(
                    row,
                    generation_before=online_generation_before,
                    source_id=str(provider["source_id"]),
                    location=location,
                ),
            )

            wifi_restore_needed = True
            require_result(
                send_checked(ser, "wifi off", command_timeout), "wifi off"
            )
            poll_command(
                ser,
                "wifi status",
                timeout=wifi_timeout,
                command_timeout=command_timeout,
                interval=poll_interval,
                predicate=wifi_is_offline,
            )
            require_result(
                send_checked(ser, "ui tab home", command_timeout),
                "ui tab",
            )
            wait_for_ui_tab(
                ser,
                "home",
                command_timeout=command_timeout,
                interval=poll_interval,
            )
            before_open = require_result(
                send_checked(ser, "map tiles status", command_timeout),
                "map tiles status",
            )
            generation_before = _integer(
                before_open.get("generation"), minimum=0
            )
            if generation_before is None:
                raise AcceptanceFailure(
                    "map_generation_invalid",
                    "Map view generation counter is invalid",
                )
            validate_acceptance_open(
                send_checked(ser, "map acceptance open", command_timeout),
            )
            revisit = poll_command(
                ser,
                "map tiles status",
                timeout=offline_timeout,
                command_timeout=command_timeout,
                interval=poll_interval,
                predicate=lambda row: offline_view_ready(
                    row,
                    generation_before=generation_before,
                    source_id=str(provider["source_id"]),
                    location=location,
                ),
            )
        except BaseException as exc:
            primary_error = exc
        finally:
            if original_tab is not None:
                try:
                    current = require_result(
                        send_checked(ser, "ui status", command_timeout),
                        "ui status",
                    )
                    if (
                        current.get("active_tab") != original_tab
                        or current.get("pending") is True
                    ):
                        require_result(
                            send_checked(
                                ser,
                                f"ui tab {original_tab}",
                                command_timeout,
                            ),
                            "ui tab",
                        )
                        wait_for_ui_tab(
                            ser,
                            original_tab,
                            command_timeout=command_timeout,
                            interval=poll_interval,
                        )
                    map_was_hidden = False
                except BaseException as exc:
                    cleanup_errors.append(f"ui:{exc}")
            if wifi_restore_needed:
                try:
                    require_result(
                        send_checked(ser, "wifi on", command_timeout),
                        "wifi on",
                    )
                    poll_command(
                        ser,
                        "wifi status",
                        timeout=wifi_timeout,
                        command_timeout=command_timeout,
                        interval=poll_interval,
                        predicate=lambda row: wifi_is_connected_to(
                            row, original_ssid
                        ),
                    )
                    wifi_restore_needed = False
                except BaseException as exc:
                    cleanup_errors.append(f"wifi:{exc}")
            if not cleanup_errors and primary_error is None:
                try:
                    final_health = validate_health(
                        send_checked(ser, "health", command_timeout),
                        boot_nonce=initial_nonce,
                    )
                    final_crashlog = validate_crashlog(
                        send_checked(ser, "crashlog", command_timeout)
                    )
                except BaseException as exc:
                    primary_error = exc

    if cleanup_errors:
        raise AcceptanceFailure(
            "cleanup_failed",
            "acceptance cleanup did not restore device state: "
            + "; ".join(cleanup_errors),
        )
    if primary_error is not None:
        raise primary_error
    if map_was_hidden or wifi_restore_needed:
        raise AcceptanceFailure(
            "cleanup_incomplete", "acceptance cleanup state is incomplete"
        )

    target_after = _resolve_pi_target(list_ports.comports)
    if (
        target_after.get("stable_identity_sha256")
        != target_before.get("stable_identity_sha256")
    ):
        raise AcceptanceFailure(
            "target_changed", "D1L stable USB identity changed during acceptance"
        )

    transcript = build_transcript(
        version=version,
        provider=provider,
        baseline=baseline,
        download=download,
        online_view=online_view,
        revisit=revisit,
        health=final_health,
        crashlog=final_crashlog,
        target_before=target_before,
        target_after=target_after,
        runner_commit=runner_commit,
        runner_source_clean=runner_source_clean,
        expected_commit=expected_commit,
        actions_run=actions_run,
        workflow_run_attempt=workflow_run_attempt,
    )
    candidate = {
        "firmware_commit": expected_commit,
        "actions_run": actions_run,
        "actions_run_attempt": workflow_run_attempt,
    }
    try:
        validate_map(transcript, candidate)
    except EvidenceError as exc:
        raise AcceptanceFailure(
            "aggregate_gate_rejected",
            "generated Map transcript was rejected by the aggregate gate",
        ) from exc
    return transcript


def write_transcript_atomic(path: Path, transcript: dict[str, Any]) -> None:
    path = Path(path)
    if path.suffix.lower() != ".json":
        raise ValueError("output path must end in .json")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise FileExistsError(f"refusing to overwrite existing output: {path}")
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        with temporary.open("x", encoding="utf-8", newline="\n") as handle:
            json.dump(transcript, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.link(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument(
        "--port",
        default=POSIX_D1L_TARGET,
        choices=(POSIX_D1L_TARGET,),
        help="exact stable D1L by-id endpoint; no ttyUSB fallback is accepted",
    )
    parser.add_argument("--expected-firmware-commit", required=True)
    parser.add_argument("--github-actions-run", required=True)
    parser.add_argument("--workflow-run-attempt", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--command-timeout",
        type=float,
        default=COMMAND_TIMEOUT_SECONDS,
    )
    parser.add_argument(
        "--boot-timeout",
        type=float,
        default=BOOT_TIMEOUT_SECONDS,
    )
    parser.add_argument(
        "--poll-interval",
        type=float,
        default=POLL_INTERVAL_SECONDS,
    )
    parser.add_argument(
        "--prefetch-timeout",
        type=float,
        default=PREFETCH_TIMEOUT_SECONDS,
    )
    parser.add_argument(
        "--offline-timeout",
        type=float,
        default=OFFLINE_VIEW_TIMEOUT_SECONDS,
    )
    parser.add_argument(
        "--wifi-timeout",
        type=float,
        default=WIFI_TIMEOUT_SECONDS,
    )
    args = parser.parse_args(argv)
    for name in (
        "command_timeout",
        "boot_timeout",
        "poll_interval",
        "prefetch_timeout",
        "offline_timeout",
        "wifi_timeout",
    ):
        if not 0.05 <= getattr(args, name) <= 600.0:
            parser.error(f"--{name.replace('_', '-')} must be 0.05..600 seconds")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        expected_commit = exact_expected_commit(
            args.expected_firmware_commit
        )
        source = git_metadata(args.root.resolve())
        if not (
            source.get("commit") == expected_commit
            and source.get("dirty") is False
            and source.get("dirty_entries") == []
        ):
            raise AcceptanceFailure(
                "runner_source_mismatch",
                "Map acceptance must run from the exact clean candidate",
            )
        actions_run = positive_decimal(
            args.github_actions_run, "github actions run"
        )
        workflow_run_attempt = positive_decimal(
            args.workflow_run_attempt, "workflow run attempt"
        )
        transcript = run_acceptance(
            runner_commit=source["commit"],
            runner_source_clean=True,
            expected_commit=expected_commit,
            actions_run=actions_run,
            workflow_run_attempt=workflow_run_attempt,
            command_timeout=args.command_timeout,
            boot_timeout=args.boot_timeout,
            poll_interval=args.poll_interval,
            prefetch_timeout=args.prefetch_timeout,
            offline_timeout=args.offline_timeout,
            wifi_timeout=args.wifi_timeout,
        )
        write_transcript_atomic(args.output, transcript)
    except (AcceptanceFailure, EvidenceError, OSError, ValueError) as exc:
        code = exc.code if isinstance(exc, AcceptanceFailure) else "host_error"
        print(
            json.dumps(
                {
                    "schema": 1,
                    "ok": False,
                    "kind": TRANSCRIPT_KIND,
                    "code": code,
                    "message": str(exc),
                    "accepted_transcript_written": False,
                },
                sort_keys=True,
            ),
            file=sys.stderr,
        )
        return 1
    print(
        json.dumps(
            {
                "schema": 1,
                "ok": True,
                "kind": TRANSCRIPT_KIND,
                "output": str(args.output),
                "manual_only": False,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
