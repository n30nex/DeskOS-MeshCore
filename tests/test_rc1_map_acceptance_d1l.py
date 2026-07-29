import copy
from pathlib import Path

import pytest

from scripts import d1l_serial_target
from scripts import produce_rc1_bounded_physical_receipt_d1l as aggregate
from scripts import rc1_map_acceptance_d1l as runner


ROOT = Path(__file__).resolve().parents[1]
COMMIT = "a" * 40
RUN = "123456789"
ATTEMPT = "1"
LOCATION = {
    "set": True,
    "lat_e7": 436_531_000,
    "lon_e7": -793_838_000,
    "source": "manual",
}


def target_snapshot() -> dict:
    requested = d1l_serial_target.POSIX_D1L_TARGET
    resolved = "/dev/ttyUSB7"
    return d1l_serial_target.resolve_target(
        requested,
        port_lister=lambda: [
            {
                "device": resolved,
                "vid": d1l_serial_target.EXPECTED_VID,
                "pid": d1l_serial_target.EXPECTED_PID,
                "serial_number": "D1L-MAP-TEST",
                "hwid": "USB VID:PID=1A86:7523 LOCATION=1-2",
                "location": "1-2",
            }
        ],
        platform_name="posix",
        exists=lambda _path: True,
        is_symlink=lambda path: path == requested,
        realpath=lambda path: resolved if path == requested else path,
        access=lambda _path, _mode: True,
        hostname=lambda: runner.PI_HOST,
    )


def provider_status(
    *,
    network_requests: int = 4,
    downloaded_tiles: int = 7,
) -> dict:
    return {
        "schema": 1,
        "ok": True,
        "cmd": "map provider status",
        "configured": True,
        "authorized_provider": True,
        "provider_refresh_ok": True,
        "provider_refresh_code": "ESP_OK",
        "source_id": "licensed-local-tiles",
        "network_url_redacted": True,
        "https": True,
        "network_fetch_allowed": True,
        "offline_storage_permitted": True,
        "background_prefetch_permitted": True,
        "osm_standard_endpoint": False,
        "attribution": "Example licensed tiles",
        "license_url": "https://tiles.example.ca/license",
        "provider_max_zoom": 18,
        "minimum_request_interval_ms": 250,
        "average_tile_bytes": 65536,
        "cache_budget_mb": 18432,
        "device_location": dict(LOCATION),
        "wifi": {
            "setting_enabled": True,
            "profile_saved": True,
            "connected": True,
            "state": "connected",
        },
        "sd": {
            "ready": True,
            "capacity_kb": 31_250_000,
            "free_kb": 30_000_000,
            "backend": "rp2040",
        },
        "node_radius_km": 200,
        "nodes_accounted": True,
        "node_markers": {
            "generation": 9,
            "seen": 2,
            "included": 1,
            "outside_radius": 1,
        },
        "prefetch": {
            "initialized": True,
            "running": True,
            "eligible": True,
            "complete": False,
            "paused_for_visible_map": False,
            "location_set": True,
            "wifi_connected": True,
            "sd_ready": True,
            "provider_configured": True,
            "background_prefetch_permitted": True,
            "source_id": "licensed-local-tiles",
            "storage_reserve_reached": False,
            "selected_max_zoom": 18,
            "total_tiles": 100,
            "visited_tiles": downloaded_tiles,
            "cached_tiles": 0,
            "network_requests": network_requests,
            "downloaded_tiles": downloaded_tiles,
            "failed_tiles": 0,
            "evicted_tiles": 0,
            "downloaded_bytes": downloaded_tiles * 65536,
            "cache_used_bytes": downloaded_tiles * 65536,
            "estimated_bytes": 6_553_600,
            "allocation_bytes": 18_000_000_000,
            "cache_budget_mb": 18432,
            "phase": "downloading",
            "message": "Downloading the local node map in the background",
        },
        "network_requests": network_requests,
        "downloaded_tiles": downloaded_tiles,
        "public_rf_tx": False,
        "formats_sd": False,
        "arbitrary_url_accepted": False,
        "arbitrary_location_accepted": False,
    }


def offline_view() -> dict:
    return {
        "schema": 1,
        "ok": True,
        "cmd": "map tiles status",
        "source": "licensed-local-tiles",
        "attribution": "Example licensed tiles",
        "provider_configured": True,
        "background_prefetch_permitted": True,
        "provider_max_zoom": 18,
        "initialized": True,
        "visible": True,
        "worker_running": False,
        "frame_ready": True,
        "sd_cache_ready": True,
        "wifi_connected": False,
        "rate_limited": False,
        "current_view_only": True,
        "generation": 12,
        "frame_revision": 6,
        "lat_e7": LOCATION["lat_e7"],
        "lon_e7": LOCATION["lon_e7"],
        "planned_tiles": 6,
        "attempted_tiles": 6,
        "cache_hits": 6,
        "network_requests": 0,
        "downloaded_tiles": 0,
        "rendered_tiles": 6,
        "failed_tiles": 0,
        "phase": "ready",
        "public_rf_tx": False,
        "formats_sd": False,
    }


def online_view() -> dict:
    row = offline_view()
    row["wifi_connected"] = True
    row["cache_hits"] = 5
    row["network_requests"] = 1
    row["downloaded_tiles"] = 1
    return row


def version() -> dict:
    return {
        "schema": 1,
        "ok": True,
        "cmd": "version",
        "build_commit": COMMIT,
        "release_profile": runner.CORE_RELEASE_PROFILE,
        "sd_history_mode": runner.EXPECTED_SD_HISTORY_MODE,
    }


def health() -> dict:
    return {
        "schema": 1,
        "ok": True,
        "cmd": "health",
        "boot_nonce": 42,
        "board_ready": True,
        "ui_ready": True,
        "release_profile": runner.CORE_RELEASE_PROFILE,
        "sd_history_mode": runner.EXPECTED_SD_HISTORY_MODE,
    }


def crashlog() -> dict:
    return {
        "schema": 1,
        "ok": True,
        "cmd": "crashlog",
        "entries": [],
    }


def test_machine_transcript_is_accepted_by_aggregate_map_gate():
    before = provider_status()
    after = provider_status(network_requests=5, downloaded_tiles=8)
    target = target_snapshot()
    transcript = runner.build_transcript(
        version=version(),
        provider=before,
        baseline=before,
        download=after,
        online_view=online_view(),
        revisit=offline_view(),
        health=health(),
        crashlog=crashlog(),
        target_before=target,
        target_after=copy.deepcopy(target),
        runner_commit=COMMIT,
        runner_source_clean=True,
        expected_commit=COMMIT,
        actions_run=RUN,
        workflow_run_attempt=ATTEMPT,
    )

    outcomes = aggregate.validate_map(
        transcript,
        {
            "firmware_commit": COMMIT,
            "actions_run": RUN,
            "actions_run_attempt": ATTEMPT,
        },
    )

    assert outcomes == {
        "authorized_map_download": True,
        "map_cache_revisit": True,
    }
    assert set(transcript) == aggregate.TRANSCRIPT_KEYS
    assert [step["operation"] for step in transcript["steps"]] == [
        "version",
        "provider",
        "before",
        "download",
        "revisit",
        "health",
        "crashlog",
    ]
    assert transcript["manual_only"] is False
    assert transcript["runner_commit"] == COMMIT
    assert transcript["runner_source_clean"] is True
    assert transcript["port"] == d1l_serial_target.POSIX_D1L_TARGET


@pytest.mark.parametrize(
    ("runner_commit", "runner_source_clean"),
    [
        ("b" * 40, True),
        (COMMIT, False),
    ],
)
def test_machine_transcript_rejects_wrong_runner_source(
    runner_commit: str,
    runner_source_clean: bool,
):
    before = provider_status()
    after = provider_status(network_requests=5, downloaded_tiles=8)
    target = target_snapshot()
    transcript = runner.build_transcript(
        version=version(),
        provider=before,
        baseline=before,
        download=after,
        online_view=online_view(),
        revisit=offline_view(),
        health=health(),
        crashlog=crashlog(),
        target_before=target,
        target_after=copy.deepcopy(target),
        runner_commit=runner_commit,
        runner_source_clean=runner_source_clean,
        expected_commit=COMMIT,
        actions_run=RUN,
        workflow_run_attempt=ATTEMPT,
    )

    with pytest.raises(aggregate.EvidenceError):
        aggregate.validate_map(
            transcript,
            {
                "firmware_commit": COMMIT,
                "actions_run": RUN,
                "actions_run_attempt": ATTEMPT,
            },
        )


def test_provider_and_download_validation_fail_closed_on_osm_or_no_delta():
    before = provider_status()
    runner.validate_provider_status(
        before, location=LOCATION, require_plan=True
    )
    assert "network_url_template" not in before

    osm = copy.deepcopy(before)
    osm["osm_standard_endpoint"] = True
    with pytest.raises(
        runner.AcceptanceFailure, match="non-OSM HTTPS"
    ):
        runner.validate_provider_status(
            osm, location=LOCATION, require_plan=True
        )

    with pytest.raises(
        runner.AcceptanceFailure, match="no fresh authorized"
    ):
        runner.validate_download_progress(
            copy.deepcopy(before), before, location=LOCATION
        )


def test_provider_validation_rejects_budget_mismatch_or_overuse():
    mismatch = provider_status()
    mismatch["prefetch"]["cache_budget_mb"] = 1024
    with pytest.raises(
        runner.AcceptanceFailure, match="200 km node plan"
    ):
        runner.validate_provider_status(
            mismatch, location=LOCATION, require_plan=True
        )

    over_budget = provider_status()
    over_budget["prefetch"]["cache_used_bytes"] = (
        over_budget["cache_budget_mb"] * 1024 * 1024 + 1
    )
    with pytest.raises(
        runner.AcceptanceFailure, match="200 km node plan"
    ):
        runner.validate_provider_status(
            over_budget, location=LOCATION, require_plan=True
        )


def test_offline_revisit_requires_new_generation_complete_cache_and_zero_network():
    row = offline_view()
    assert runner.offline_view_ready(
        row,
        generation_before=11,
        source_id="licensed-local-tiles",
        location=LOCATION,
    )

    networked = copy.deepcopy(row)
    networked["network_requests"] = 1
    assert not runner.offline_view_ready(
        networked,
        generation_before=11,
        source_id="licensed-local-tiles",
        location=LOCATION,
    )

    partial = copy.deepcopy(row)
    partial["rendered_tiles"] = 5
    assert not runner.offline_view_ready(
        partial,
        generation_before=11,
        source_id="licensed-local-tiles",
        location=LOCATION,
    )

    assert runner.online_view_ready(
        online_view(),
        generation_before=11,
        source_id="licensed-local-tiles",
        location=LOCATION,
    )


def test_online_foreground_transition_has_a_fixed_physical_deadline():
    acceptance = (
        ROOT / "scripts/rc1_map_acceptance_d1l.py"
    ).read_text(encoding="utf-8")

    assert runner.FOREGROUND_TRANSITION_TIMEOUT_SECONDS == 120.0
    assert (
        "timeout=min(\n"
        "                    offline_timeout,\n"
        "                    FOREGROUND_TRANSITION_TIMEOUT_SECONDS,\n"
        "                ),"
    ) in acceptance


def test_firmware_exposes_product_map_status_and_blocks_osm_bulk_prefetch():
    console = (ROOT / "main/comms/usb_console.c").read_text(encoding="utf-8")
    provider_c = (ROOT / "main/map/map_tile_provider.c").read_text(
        encoding="utf-8"
    )
    provider_h = (ROOT / "main/map/map_tile_provider.h").read_text(
        encoding="utf-8"
    )
    prefetch_c = (ROOT / "main/map/map_prefetch_service.c").read_text(
        encoding="utf-8"
    )
    prefetch_h = (ROOT / "main/map/map_prefetch_service.h").read_text(
        encoding="utf-8"
    )
    qualification = (ROOT / "main/app/qualification_hooks.h").read_text(
        encoding="utf-8"
    )

    assert '"map provider status"' in console
    assert 'ok_begin("map provider status")' in console
    assert '"map acceptance status"' not in console
    assert "D1L_RELEASE_COMMAND_READ_ONLY" in console
    assert '\\"osm_standard_endpoint\\":' in console
    assert '\\"node_radius_km\\":' in console
    assert "d1l_map_tile_provider_uses_osm_standard" in provider_h
    assert "tile.openstreetmap.org" in provider_c
    assert (
        "if (d1l_map_tile_provider_uses_osm_standard(&provider))"
        in provider_c
    )
    assert "uint64_t network_requests;" in prefetch_h
    assert "++s_network_requests_total;" in prefetch_c
    assert prefetch_c.index("++s_network_requests_total;") < prefetch_c.index(
        "ret = d1l_map_tile_store_fetch_background("
    )
    assert "D1L_ENABLE_QUALIFICATION_HOOKS" in qualification
    assert "map provider status" in runner.COMMANDS
    assert "map acceptance open" not in runner.COMMANDS
    assert "reboot" in runner.COMMANDS


def test_console_readiness_retries_a_command_lost_before_init(
    monkeypatch: pytest.MonkeyPatch,
):
    responses = [
        {"schema": 1, "ok": False, "cmd": "health", "code": "TIMEOUT"},
        health(),
    ]
    calls = []

    def send(_ser, command, timeout):
        calls.append((command, timeout))
        return responses.pop(0)

    monkeypatch.setattr(runner, "send_console_command", send)

    result = runner.wait_for_console_ready(
        object(),
        timeout=1.0,
        command_timeout=0.1,
        interval=0,
    )

    assert result["ok"] is True
    assert [command for command, _timeout in calls] == ["health", "health"]


def test_saved_wifi_readiness_polls_until_saved_profile_connects(
    monkeypatch: pytest.MonkeyPatch,
):
    connecting = {
        "schema": 1,
        "ok": True,
        "cmd": "wifi status",
        "available": True,
        "setting_enabled": True,
        "build_enabled": True,
        "stack_active": True,
        "connected": False,
        "connecting": True,
        "profile_saved": True,
        "password_saved": True,
        "state": "connecting",
        "live_network": True,
        "ssid": "Toddmas2.4",
    }
    connected = {
        **connecting,
        "connected": True,
        "connecting": False,
        "state": "connected",
    }
    responses = [connecting, connected]
    calls = []

    def send(_ser, command, timeout):
        calls.append((command, timeout))
        return responses.pop(0)

    monkeypatch.setattr(runner, "send_console_command", send)

    result = runner.wait_for_saved_wifi(
        object(),
        timeout=1.0,
        command_timeout=0.1,
        interval=0,
    )

    assert result["state"] == "connected"
    assert result["ssid"] == "Toddmas2.4"
    assert calls == [("wifi status", 0.1), ("wifi status", 0.1)]


def test_cli_exposes_bounded_boot_timeout():
    args = runner.parse_args(
        [
            "--expected-firmware-commit", COMMIT,
            "--github-actions-run", RUN,
            "--workflow-run-attempt", ATTEMPT,
            "--output", "map.json",
        ]
    )
    assert args.boot_timeout == runner.BOOT_TIMEOUT_SECONDS


def test_cli_exposes_bounded_wifi_timeout():
    args = runner.parse_args(
        [
            "--expected-firmware-commit", COMMIT,
            "--github-actions-run", RUN,
            "--workflow-run-attempt", ATTEMPT,
            "--output", "map.json",
        ]
    )
    assert args.wifi_timeout == runner.WIFI_TIMEOUT_SECONDS


def test_cli_has_no_manual_dry_run_or_secret_configuration_surface():
    with pytest.raises(SystemExit):
        runner.parse_args(
            [
                "--expected-firmware-commit",
                COMMIT,
                "--github-actions-run",
                RUN,
                "--workflow-run-attempt",
                ATTEMPT,
                "--output",
                "map.json",
                "--dry-run",
            ]
        )
    with pytest.raises(SystemExit):
        runner.parse_args(
            [
                "--expected-firmware-commit",
                COMMIT,
                "--github-actions-run",
                RUN,
                "--workflow-run-attempt",
                ATTEMPT,
                "--output",
                "map.json",
                "--wifi-password",
                "not-accepted",
            ]
        )
