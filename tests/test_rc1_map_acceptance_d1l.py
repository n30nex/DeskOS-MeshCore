import copy
import time
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
        "provider_refresh_generation": 1,
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


def test_retry_progress_must_converge_to_one_complete_bounded_pass():
    poisoned = online_view()
    poisoned.update(
        {
            "worker_running": True,
            "planned_tiles": 9,
            "attempted_tiles": 39,
            "cache_hits": 30,
            "network_requests": 8,
            "downloaded_tiles": 0,
            "rendered_tiles": 30,
            "failed_tiles": 8,
            "phase": "loading_cache",
        }
    )
    assert not runner.online_view_ready(
        poisoned,
        generation_before=11,
        source_id="licensed-local-tiles",
        location=LOCATION,
    )
    assert not runner.offline_view_ready(
        poisoned,
        generation_before=11,
        source_id="licensed-local-tiles",
        location=LOCATION,
    )

    recovered_online = online_view()
    recovered_online.update(
        {
            "planned_tiles": 9,
            "attempted_tiles": 9,
            "cache_hits": 8,
            "network_requests": 1,
            "downloaded_tiles": 1,
            "rendered_tiles": 9,
            "failed_tiles": 0,
        }
    )
    assert runner.online_view_ready(
        recovered_online,
        generation_before=11,
        source_id="licensed-local-tiles",
        location=LOCATION,
    )

    recovered_offline = offline_view()
    recovered_offline.update(
        {
            "planned_tiles": 9,
            "attempted_tiles": 9,
            "cache_hits": 9,
            "network_requests": 0,
            "downloaded_tiles": 0,
            "rendered_tiles": 9,
            "failed_tiles": 0,
        }
    )
    assert runner.offline_view_ready(
        recovered_offline,
        generation_before=11,
        source_id="licensed-local-tiles",
        location=LOCATION,
    )


def test_online_foreground_transition_has_a_fixed_physical_deadline():
    acceptance = (
        ROOT / "scripts/rc1_map_acceptance_d1l.py"
    ).read_text(encoding="utf-8")

    assert runner.FOREGROUND_TRANSITION_TIMEOUT_SECONDS == 240.0
    assert runner.OFFLINE_VIEW_TIMEOUT_SECONDS == 240.0
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


def test_product_reboot_reopen_uses_reset_safe_control_line_order(
    monkeypatch: pytest.MonkeyPatch,
):
    opens = []

    class ExistingPath:
        def __init__(self, _value):
            pass

        def exists(self):
            return True

    class ResetSensitiveSerial:
        def __init__(self):
            self.is_open = True
            self.dtr = False
            self.rts = False

        def close(self):
            self.is_open = False

        def open(self):
            opens.append((self.dtr, self.rts))
            if self.dtr is not True or self.rts is not False:
                raise RuntimeError("unsafe cached DTR/RTS open")
            self.is_open = True

        def reset_input_buffer(self):
            pass

    monkeypatch.setattr(runner, "Path", ExistingPath)
    monkeypatch.setattr(
        runner,
        "wait_for_console_ready",
        lambda *_args, **_kwargs: {**health(), "boot_nonce": 43},
    )
    ser = ResetSensitiveSerial()

    result = runner.reopen_after_product_reboot(
        ser,
        previous_boot_nonce=42,
        timeout=1.0,
        command_timeout=0.1,
        interval=0,
    )

    assert result["boot_nonce"] == 43
    assert opens == [(True, False)]
    assert ser.is_open is True
    assert ser.dtr is False
    assert ser.rts is False


def test_cleanup_reopens_closed_serial_and_proves_expected_boot(
    monkeypatch: pytest.MonkeyPatch,
):
    opens = []
    resets = []

    class ClosedSerial:
        def __init__(self):
            self.is_open = False
            self.dtr = False
            self.rts = False

        def open(self):
            opens.append((self.dtr, self.rts))
            self.is_open = True

        def close(self):
            self.is_open = False

        def reset_input_buffer(self):
            resets.append(True)

    monkeypatch.setattr(
        runner,
        "wait_for_console_ready",
        lambda *_args, **_kwargs: {**health(), "boot_nonce": 43},
    )
    ser = ClosedSerial()

    result = runner.reopen_serial_for_cleanup(
        ser,
        expected_boot_nonce=43,
        timeout=75.0,
        command_timeout=20.0,
        interval=0,
    )

    assert result["boot_nonce"] == 43
    assert opens == [(True, False)]
    assert resets == [True]
    assert ser.is_open is True
    assert ser.dtr is False
    assert ser.rts is False


def test_read_only_timeout_is_retried_once_and_recorded(
    monkeypatch: pytest.MonkeyPatch,
):
    responses = [
        {
            "schema": 1,
            "ok": False,
            "cmd": "map provider status",
            "code": "TIMEOUT",
        },
        provider_status(),
    ]
    calls = []

    def send(_ser, command, timeout):
        calls.append((command, timeout))
        return responses.pop(0)

    monkeypatch.setattr(runner, "send_console_command", send)

    result = runner.send_checked(
        object(),
        "map provider status",
        20.0,
    )

    assert result["ok"] is True
    assert result["host_timeout_retries"] == 1
    assert calls == [
        ("map provider status", 10.0),
        ("map provider status", runner.READ_ONLY_TIMEOUT_RETRY_SECONDS),
    ]


def test_read_only_timeout_is_never_retried_more_than_once(
    monkeypatch: pytest.MonkeyPatch,
):
    timeout = {
        "schema": 1,
        "ok": False,
        "cmd": "ui status",
        "code": "TIMEOUT",
    }
    calls = []

    def send(_ser, command, command_timeout):
        calls.append((command, command_timeout))
        return dict(timeout)

    monkeypatch.setattr(runner, "send_console_command", send)

    result = runner.send_checked(object(), "ui status", 4.0)

    assert result["ok"] is False
    assert result["code"] == "TIMEOUT"
    assert result["host_timeout_retries"] == 1
    assert calls == [("ui status", 2.0), ("ui status", 2.0)]


def test_mutating_command_timeout_is_not_retried(
    monkeypatch: pytest.MonkeyPatch,
):
    calls = []

    def send(_ser, command, timeout):
        calls.append((command, timeout))
        return {
            "schema": 1,
            "ok": False,
            "cmd": "ui tab",
            "code": "TIMEOUT",
        }

    monkeypatch.setattr(runner, "send_console_command", send)

    result = runner.send_checked(object(), "ui tab map", 20.0)

    assert result["ok"] is False
    assert "host_timeout_retries" not in result
    assert calls == [("ui tab map", 20.0)]


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
    assert calls == [("wifi status", 0.05), ("wifi status", 0.05)]


def test_provider_plan_ready_rejects_failed_retry_generation():
    failed = provider_status()
    failed["prefetch"]["failed_tiles"] = 1

    assert runner.provider_plan_ready(failed) is False


def transient_provider_failure(
    baseline: dict,
    *,
    phase: str = "http_open",
    error: str = "ESP_ERR_HTTP_CONNECT",
) -> dict:
    transient = copy.deepcopy(baseline)
    transient["prefetch"].update(
        {
            "phase": phase,
            "running": False,
            "last_error": error,
            "failed_tiles": 1,
        }
    )
    return transient


def provider_backoff(baseline: dict) -> dict:
    backoff = copy.deepcopy(baseline)
    requests = baseline["prefetch"]["network_requests"]
    backoff["node_markers"] = {
        "generation": 0,
        "seen": 0,
        "included": 0,
        "outside_radius": 0,
    }
    backoff["downloaded_tiles"] = 0
    backoff["network_requests"] = requests
    backoff["prefetch"].update(
        {
            "phase": "backoff",
            "running": False,
            "last_error": "ESP_OK",
            "network_requests": requests,
            "selected_max_zoom": 0,
            "total_tiles": 0,
            "visited_tiles": 0,
            "cached_tiles": 0,
            "downloaded_tiles": 0,
            "failed_tiles": 0,
            "evicted_tiles": 0,
            "downloaded_bytes": 0,
            "cache_used_bytes": 0,
            "estimated_bytes": 0,
            "allocation_bytes": 0,
        }
    )
    return backoff


def test_fresh_download_recovers_after_observed_failure_and_counter_reset(
    monkeypatch: pytest.MonkeyPatch,
):
    baseline = provider_status(network_requests=4, downloaded_tiles=7)
    failure = transient_provider_failure(baseline)
    backoff = provider_backoff(baseline)
    recovered = provider_status(network_requests=4, downloaded_tiles=0)
    progressed = provider_status(network_requests=5, downloaded_tiles=1)
    responses = [failure, backoff, recovered, progressed]
    calls = []

    def send(_ser, command, timeout):
        calls.append((command, timeout))
        return responses.pop(0)

    monkeypatch.setattr(runner, "send_console_command", send)

    result = runner.wait_for_fresh_download(
        object(),
        baseline,
        location=LOCATION,
        deadline=time.monotonic() + 1.0,
        command_timeout=0.1,
        interval=0,
    )

    assert result["prefetch"]["downloaded_tiles"] == 1
    assert result["host_prefetch_counter_reset_observed"] is True
    assert result["host_witnessed_downloaded_tiles"] == 1
    assert [command for command, _timeout in calls] == [
        "map provider status",
        "map provider status",
        "map provider status",
        "map provider status",
    ]

    target = target_snapshot()
    transcript = runner.build_transcript(
        version=version(),
        provider=baseline,
        baseline=baseline,
        download=result,
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
    download_step = next(
        step for step in transcript["steps"]
        if step["operation"] == "download"
    )
    assert download_step["response"]["downloaded_tiles"] == 1
    assert (
        download_step["response"]["prefetch_counter_reset_observed"]
        is True
    )
    assert aggregate.validate_map(
        transcript,
        {
            "firmware_commit": COMMIT,
            "actions_run": RUN,
            "actions_run_attempt": ATTEMPT,
        },
    ) == {
        "authorized_map_download": True,
        "map_cache_revisit": True,
    }


def test_provider_plan_wait_does_not_mask_storage_reserve(
    monkeypatch: pytest.MonkeyPatch,
):
    invalid = provider_status(network_requests=4, downloaded_tiles=7)
    invalid["prefetch"]["storage_reserve_reached"] = True
    calls = []

    def send(_ser, command, timeout):
        calls.append((command, timeout))
        return invalid

    monkeypatch.setattr(runner, "send_console_command", send)

    with pytest.raises(runner.AcceptanceFailure) as caught:
        runner.wait_for_provider_plan(
            object(),
            location=LOCATION,
            deadline=time.monotonic() + 1.0,
            command_timeout=0.1,
            interval=0,
        )

    assert caught.value.code == "prefetch_plan_invalid"
    assert [command for command, _timeout in calls] == [
        "map provider status"
    ]


@pytest.mark.parametrize(
    ("phase", "error"),
    sorted(runner.TRANSIENT_PREFETCH_PHASE_ERRORS),
)
def test_transient_failure_pairs_match_firmware(phase: str, error: str):
    baseline = provider_status(network_requests=4, downloaded_tiles=7)
    transient = transient_provider_failure(
        baseline, phase=phase, error=error
    )

    assert runner.transient_network_failure(
        transient, location=LOCATION
    ) is not None


def test_transient_failure_rejects_impossible_pair():
    baseline = provider_status(network_requests=4, downloaded_tiles=7)
    impossible = transient_provider_failure(
        baseline, phase="http_read", error="ESP_ERR_HTTP_CONNECT"
    )
    assert runner.transient_network_failure(
        impossible, location=LOCATION
    ) is None


def test_fresh_download_rejects_changed_transient_plan(
    monkeypatch: pytest.MonkeyPatch,
):
    baseline = provider_status(network_requests=4, downloaded_tiles=7)
    changed = transient_provider_failure(baseline)
    changed["prefetch"]["allocation_bytes"] += 1
    monkeypatch.setattr(
        runner,
        "send_console_command",
        lambda _ser, _command, _timeout: changed,
    )

    with pytest.raises(runner.AcceptanceFailure) as caught:
        runner.wait_for_fresh_download(
            object(),
            baseline,
            location=LOCATION,
            deadline=time.monotonic() + 1.0,
            command_timeout=0.1,
            interval=0,
        )

    assert caught.value.code == "prefetch_plan_changed"


def test_fresh_download_adopts_clean_live_node_plan_then_requires_progress(
    monkeypatch: pytest.MonkeyPatch,
):
    baseline = provider_status(network_requests=4, downloaded_tiles=7)
    adopted = provider_status(network_requests=0, downloaded_tiles=0)
    adopted["node_markers"].update(
        {"generation": 10, "seen": 3, "included": 2, "outside_radius": 1}
    )
    adopted["prefetch"].update(
        {
            "total_tiles": 120,
            "estimated_bytes": 7_864_320,
        }
    )
    progressed = copy.deepcopy(adopted)
    progressed["network_requests"] = 1
    progressed["downloaded_tiles"] = 1
    progressed["prefetch"].update(
        {
            "visited_tiles": 1,
            "network_requests": 1,
            "downloaded_tiles": 1,
            "downloaded_bytes": 65_536,
            "cache_used_bytes": 65_536,
        }
    )
    responses = [adopted, progressed]
    calls = []

    def send(_ser, command, timeout):
        calls.append((command, timeout))
        return responses.pop(0)

    monkeypatch.setattr(runner, "send_console_command", send)

    result = runner.wait_for_fresh_download(
        object(),
        baseline,
        location=LOCATION,
        deadline=time.monotonic() + 1.0,
        command_timeout=0.1,
        interval=0,
    )

    assert result["node_markers"]["generation"] == 10
    assert result["prefetch"]["downloaded_tiles"] == 1
    assert len(calls) == 2


def test_fresh_download_rejects_provider_identity_change(
    monkeypatch: pytest.MonkeyPatch,
):
    baseline = provider_status(network_requests=4, downloaded_tiles=7)
    changed = provider_status(network_requests=5, downloaded_tiles=8)
    changed["attribution"] = "Different licensed provider"
    monkeypatch.setattr(
        runner,
        "send_console_command",
        lambda _ser, _command, _timeout: changed,
    )

    with pytest.raises(runner.AcceptanceFailure) as caught:
        runner.wait_for_fresh_download(
            object(),
            baseline,
            location=LOCATION,
            deadline=time.monotonic() + 1.0,
            command_timeout=0.1,
            interval=0,
        )

    assert caught.value.code == "prefetch_identity_changed"


def test_fresh_download_recovers_when_zeroed_backoff_poll_is_missed(
    monkeypatch: pytest.MonkeyPatch,
):
    baseline = provider_status(network_requests=4, downloaded_tiles=7)
    failure = transient_provider_failure(baseline)
    recovered = provider_status(network_requests=4, downloaded_tiles=0)
    progressed = provider_status(network_requests=5, downloaded_tiles=1)
    responses = [failure, recovered, progressed]
    monkeypatch.setattr(
        runner,
        "send_console_command",
        lambda _ser, _command, _timeout: responses.pop(0),
    )

    result = runner.wait_for_fresh_download(
        object(),
        baseline,
        location=LOCATION,
        deadline=time.monotonic() + 1.0,
        command_timeout=0.1,
        interval=0,
    )

    assert result["host_prefetch_counter_reset_observed"] is True
    assert result["host_witnessed_downloaded_tiles"] == 1


def test_context_free_backoff_is_not_accepted(
    monkeypatch: pytest.MonkeyPatch,
):
    baseline = provider_status(network_requests=4, downloaded_tiles=7)
    backoff = provider_backoff(baseline)
    calls = []

    def send(_ser, command, timeout):
        calls.append((command, timeout))
        return backoff

    monkeypatch.setattr(runner, "send_console_command", send)

    with pytest.raises(runner.AcceptanceFailure) as caught:
        runner.wait_for_fresh_download(
            object(),
            baseline,
            location=LOCATION,
            deadline=time.monotonic() + 1.0,
            command_timeout=0.1,
            interval=0,
        )

    assert caught.value.code == "prefetch_plan_invalid"
    assert len(calls) == 1


def test_malformed_backoff_counter_copy_is_not_accepted(
    monkeypatch: pytest.MonkeyPatch,
):
    baseline = provider_status(network_requests=4, downloaded_tiles=7)
    failure = transient_provider_failure(baseline)
    backoff = provider_backoff(baseline)
    backoff["network_requests"] += 1
    responses = [failure, backoff]
    monkeypatch.setattr(
        runner,
        "send_console_command",
        lambda _ser, _command, _timeout: responses.pop(0),
    )

    with pytest.raises(runner.AcceptanceFailure) as caught:
        runner.wait_for_fresh_download(
            object(),
            baseline,
            location=LOCATION,
            deadline=time.monotonic() + 1.0,
            command_timeout=0.1,
            interval=0,
        )

    assert caught.value.code == "prefetch_plan_invalid"


def test_fresh_download_transient_wait_is_bounded(
    monkeypatch: pytest.MonkeyPatch,
):
    baseline = provider_status(network_requests=4, downloaded_tiles=7)
    failure = transient_provider_failure(baseline)
    backoff = provider_backoff(baseline)
    calls = []

    def send(_ser, command, timeout):
        calls.append((command, timeout))
        return failure if len(calls) == 1 else backoff

    now = [100.0]
    monkeypatch.setattr(runner, "send_console_command", send)
    monkeypatch.setattr(runner.time, "monotonic", lambda: now[0])
    monkeypatch.setattr(
        runner.time,
        "sleep",
        lambda seconds: now.__setitem__(0, now[0] + seconds),
    )

    with pytest.raises(runner.AcceptanceFailure) as caught:
        runner.wait_for_fresh_download(
            object(),
            baseline,
            location=LOCATION,
            deadline=100.03,
            command_timeout=0.01,
            interval=0.01,
        )

    assert caught.value.code == "prefetch_timeout"
    assert len(calls) >= 2


def test_download_validation_rejects_plan_and_counter_copy_drift():
    baseline = provider_status(network_requests=4, downloaded_tiles=7)
    changed = provider_status(network_requests=5, downloaded_tiles=8)
    changed["node_markers"].update(
        {"seen": 3, "included": 2, "outside_radius": 1}
    )
    with pytest.raises(runner.AcceptanceFailure) as plan:
        runner.validate_download_progress(
            changed, baseline, location=LOCATION
        )
    assert plan.value.code == "prefetch_plan_changed"

    stale_copy = provider_status(network_requests=5, downloaded_tiles=8)
    stale_copy["network_requests"] = 4
    with pytest.raises(runner.AcceptanceFailure) as counter:
        runner.validate_download_progress(
            stale_copy, baseline, location=LOCATION
        )
    assert counter.value.code == "prefetch_plan_invalid"


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
    assert runner.BOOT_TIMEOUT_SECONDS == 75.0


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
