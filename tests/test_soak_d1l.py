import json
import sys
from pathlib import Path

import pytest

from scripts import soak_d1l


def base_health(uptime_ms=1000):
    return {
        "uptime_ms": uptime_ms,
        "heap_free": 100000,
        "heap_min_free": 99000,
        "psram_free": 200000,
        "psram_min_free": 199000,
        "current_task_stack_free_words": 1200,
        "ui_task_stack_free_words": 1300,
        "retained_task_stack_free_bytes": 5000,
        "lvgl_used_pct": 50,
    }


def sample(label, elapsed, health, mesh, packets=None, signal=None):
    return {
        "label": label,
        "elapsed_sec": elapsed,
        "results": [
            {
                "schema": 1,
                "ok": True,
                "cmd": "health",
                "uptime_ms": health["uptime_ms"],
                "heap_free": health["heap_free"],
                "heap_min_free": health["heap_min_free"],
                "psram_free": health["psram_free"],
                "psram_min_free": health["psram_min_free"],
                "current_task_stack_free_words": health["current_task_stack_free_words"],
                "ui_task_stack_free_words": health["ui_task_stack_free_words"],
                "retained_task_stack_free_bytes": health.get(
                    "retained_task_stack_free_bytes", 5000
                ),
                "lvgl_used_pct": health["lvgl_used_pct"],
                "board_ready": True,
                "ui_ready": True,
            },
            {
                "schema": 1,
                "ok": True,
                "cmd": "mesh status",
                "state": "ready",
                "identity_ready": True,
                "radio_ready": True,
                "rx_packets": mesh["rx_packets"],
                "tx_packets": mesh["tx_packets"],
            },
            {
                "schema": 1,
                "ok": True,
                "cmd": "signal",
                "sample_count": 0 if signal is None else signal["sample_count"],
            },
            {"schema": 1, "ok": True, "cmd": "messages unread"},
            {
                "schema": 1,
                "ok": True,
                "cmd": "packets",
                "total_written": 0 if packets is None else packets["total_written"],
            },
            {"schema": 1, "ok": True, "cmd": "crashlog"},
        ],
    }


def storage_status(
    *,
    state="ready",
    filesystem="fat32",
    protocol_supported=True,
    present=True,
    mounted=True,
    data_root_ready=True,
    file_ops=True,
    atomic_rename=True,
    status_stale=False,
    presence_stale=False,
    refresh_failures=0,
    data_enabled=True,
    data_backend="mixed",
    message_store_backend="nvs",
    dm_store_backend="nvs",
    packet_log_backend="sd",
    route_store_backend="nvs",
):
    return {
        "schema": 1,
        "ok": True,
        "cmd": soak_d1l.STORAGE_STATUS_COMMAND,
        "sd": {
            "state": state,
            "filesystem": filesystem,
            "interface": "rp2040",
            "rp2040_protocol_supported": protocol_supported,
            "present": present,
            "mounted": mounted,
            "data_root_ready": data_root_ready,
            "file_ops": file_ops,
            "atomic_rename": atomic_rename,
            "status_stale": status_stale,
            "presence_stale": presence_stale,
            "refresh_failures": refresh_failures,
            "file_line_max": 512 if file_ops else 0,
            "file_chunk_max": 192 if file_ops else 0,
            "path_max": 96 if file_ops else 0,
        },
        "data_enabled": data_enabled,
        "data_backend": data_backend,
        "message_store_backend": message_store_backend,
        "dm_store_backend": dm_store_backend,
        "packet_log_backend": packet_log_backend,
        "route_store_backend": route_store_backend,
        "map_tile_backend": "sd_pending_store_migration" if file_ops else "unavailable",
        "export_backend": "sd_diagnostic_exports_ready" if file_ops else "serial",
        "stores": {
            "settings": "nvs",
            "identity": "nvs",
            "messages": message_store_backend,
            "dm": dm_store_backend,
            "packets": packet_log_backend,
            "routes": route_store_backend,
            "contacts": "nvs",
            "read_state": "nvs",
            "crashlog": "nvs",
            "map_tiles": "sd_pending_store_migration" if file_ops else "unavailable",
            "exports": "sd_diagnostic_exports_ready" if file_ops else "serial",
        },
    }


def test_file_ops_ready_requires_fat32_filesystem():
    assert soak_d1l.file_ops_ready(storage_status()) is True
    assert soak_d1l.file_ops_ready(storage_status(filesystem="exfat")) is False


def test_dry_run_reports_soak_commands():
    commit = "a" * 40
    report = soak_d1l.dry_run_report(
        duration_sec=60,
        sample_interval_sec=10,
        active_public_text=None,
        active_interval_sec=30,
        require_rx_delta=True,
        min_rx_delta=1,
        min_tx_delta=1,
        clear_crashlog_before_start=True,
        command_retries=1,
        retry_delay_sec=0.5,
        active_dm_fingerprint="0BF0A701D5AE2DB6",
        active_dm_text="test",
        expected_firmware_commit=commit.upper(),
    )

    assert report["ok"] is True
    assert report["commands"] == soak_d1l.SOAK_COMMANDS
    assert report["active_public_text"] is None
    assert report["public_rf_tx"] is False
    assert report["dm_rf_tx"] is True
    assert report["active_command"] == "mesh send dm 0BF0A701D5AE2DB6 test"
    assert report["require_rx_delta"] is True
    assert report["clear_crashlog_before_start"] is True
    assert report["command_retries"] == 1
    assert report["preflight_commands"] == ["version"]
    assert report["expected_firmware_commit"] == commit
    assert report["device_build_commit"] is None
    assert report["firmware_identity_required"] is True
    assert report["firmware_identity_ok"] is None


def test_dry_run_rejects_automated_public_tx():
    with pytest.raises(ValueError, match="automated Public TX is disabled"):
        soak_d1l.dry_run_report(
            duration_sec=60,
            sample_interval_sec=10,
            active_public_text="test",
            active_interval_sec=30,
            require_rx_delta=True,
            min_rx_delta=1,
            min_tx_delta=1,
            clear_crashlog_before_start=False,
            command_retries=1,
            retry_delay_sec=0.5,
        )


def test_dry_run_defaults_do_not_send_public_rf_or_touch_sd_format():
    report = soak_d1l.dry_run_report(
        duration_sec=60,
        sample_interval_sec=10,
        active_public_text=None,
        active_interval_sec=30,
        require_rx_delta=False,
        min_rx_delta=1,
        min_tx_delta=0,
        clear_crashlog_before_start=False,
        command_retries=1,
        retry_delay_sec=0.5,
    )

    assert soak_d1l.SD_FILE_CANARY_COMMAND not in report["commands"]
    assert soak_d1l.STORAGE_STATUS_COMMAND not in report["commands"]
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False


def test_dry_run_sd_canary_is_serial_only_and_non_formatting():
    report = soak_d1l.dry_run_report(
        duration_sec=60,
        sample_interval_sec=10,
        active_public_text=None,
        active_interval_sec=30,
        require_rx_delta=False,
        min_rx_delta=1,
        min_tx_delta=0,
        clear_crashlog_before_start=False,
        command_retries=1,
        retry_delay_sec=0.5,
        sample_storage=True,
        sd_file_canary=True,
        allow_sd_unavailable=True,
    )

    assert report["commands"] == [
        *soak_d1l.SOAK_COMMANDS,
        soak_d1l.STORAGE_STATUS_COMMAND,
        soak_d1l.SD_FILE_CANARY_COMMAND,
    ]
    assert report["public_rf_tx"] is False
    assert report["formats_sd"] is False
    assert report["sample_storage"] is True
    assert report["sd_file_canary"] is True


def test_summarize_soak_tracks_deltas_and_watermarks():
    samples = [
        sample(
            "start",
            0,
            {
                "uptime_ms": 1000,
                "heap_free": 100000,
                "heap_min_free": 99000,
                "psram_free": 200000,
                "psram_min_free": 199000,
                "current_task_stack_free_words": 1200,
                "ui_task_stack_free_words": 1300,
                "lvgl_used_pct": 50,
            },
            {"rx_packets": 5, "tx_packets": 7},
            {"total_written": 20},
            {"sample_count": 3},
        ),
        sample(
            "final",
            60,
            {
                "uptime_ms": 61000,
                "heap_free": 99500,
                "heap_min_free": 98000,
                "psram_free": 199500,
                "psram_min_free": 198000,
                "current_task_stack_free_words": 1180,
                "ui_task_stack_free_words": 1280,
                "lvgl_used_pct": 54,
            },
            {"rx_packets": 8, "tx_packets": 9},
            {"total_written": 24},
            {"sample_count": 6},
        ),
    ]
    active_events = [
        {
            "elapsed_sec": 2,
            "command": "mesh send dm 0BF0A701D5AE2DB6 test",
            "fingerprint": "0BF0A701D5AE2DB6",
            "text": "test",
            "result": {"schema": 1, "ok": True, "cmd": "mesh send dm"},
        }
    ]

    summary = soak_d1l.summarize_soak(
        samples=samples,
        active_events=active_events,
        require_rx_delta=True,
        min_rx_delta=1,
        min_tx_delta=1,
    )

    assert summary["ok"] is True
    assert summary["mesh_rx_packet_delta"] == 3
    assert summary["mesh_tx_packet_delta"] == 2
    assert summary["packet_total_written_delta"] == 4
    assert summary["heap_free_delta"] == -500
    assert summary["current_task_stack_free_words_floor"] == 1180
    assert summary["retained_task_stack_free_bytes_floor"] == 5000
    assert summary["lvgl_used_pct_peak"] == 54
    assert summary["signal_sample_count_peak"] == 6
    assert summary["command_retry_count"] == 0


def test_summarize_soak_rejects_low_retained_worker_stack_margin():
    health = base_health()
    health["retained_task_stack_free_bytes"] = 2048
    samples = [
        sample("start", 0, health, {"rx_packets": 0, "tx_packets": 0}),
        sample("final", 1, health, {"rx_packets": 0, "tx_packets": 0}),
    ]

    summary = soak_d1l.summarize_soak(
        samples=samples,
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=0,
        min_tx_delta=0,
    )

    assert summary["ok"] is False
    assert "retained_task_stack_margin_below_4096_bytes" in summary[
        "threshold_failures"
    ]


def test_summarize_soak_rejects_missing_retained_worker_stack_metric():
    row = sample(
        "start", 0, base_health(), {"rx_packets": 0, "tx_packets": 0}
    )
    del row["results"][0]["retained_task_stack_free_bytes"]
    summary = soak_d1l.summarize_soak(
        samples=[row],
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=0,
        min_tx_delta=0,
    )

    assert summary["ok"] is False
    assert "retained_task_stack_watermark_missing" in summary[
        "threshold_failures"
    ]


def test_summarize_soak_detects_reboot_and_missing_rx_delta():
    samples = [
        sample(
            "start",
            0,
            {
                "uptime_ms": 5000,
                "heap_free": 100000,
                "heap_min_free": 99000,
                "psram_free": 200000,
                "psram_min_free": 199000,
                "current_task_stack_free_words": 1200,
                "ui_task_stack_free_words": 1300,
                "lvgl_used_pct": 50,
            },
            {"rx_packets": 5, "tx_packets": 7},
        ),
        sample(
            "final",
            60,
            {
                "uptime_ms": 1000,
                "heap_free": 99500,
                "heap_min_free": 98000,
                "psram_free": 199500,
                "psram_min_free": 198000,
                "current_task_stack_free_words": 1180,
                "ui_task_stack_free_words": 1280,
                "lvgl_used_pct": 54,
            },
            {"rx_packets": 5, "tx_packets": 7},
        ),
    ]

    summary = soak_d1l.summarize_soak(
        samples=samples,
        active_events=[],
        require_rx_delta=True,
        min_rx_delta=1,
        min_tx_delta=0,
    )

    assert summary["ok"] is False
    assert "uptime_reset_or_reboot_seen" in summary["threshold_failures"]
    assert "rx_delta_below_minimum" in summary["threshold_failures"]


def test_summarize_soak_detects_crash_like_reset_entries():
    row = sample(
        "start",
        0,
        {
            "uptime_ms": 1000,
            "heap_free": 100000,
            "heap_min_free": 99000,
            "psram_free": 200000,
            "psram_min_free": 199000,
            "current_task_stack_free_words": 1200,
            "ui_task_stack_free_words": 1300,
            "lvgl_used_pct": 50,
        },
        {"rx_packets": 5, "tx_packets": 7},
    )
    row["results"][-1] = {
        "schema": 1,
        "ok": True,
        "cmd": "crashlog",
        "count": 1,
        "total_written": 1,
        "entries": [{"seq": 1, "reset_reason": "PANIC", "crash_like": True}],
    }

    summary = soak_d1l.summarize_soak(
        samples=[row],
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=1,
        min_tx_delta=0,
    )

    assert summary["ok"] is False
    assert summary["crashlog_crash_like_count"] == 1
    assert summary["crashlog_count_peak"] == 1
    assert "crash_like_reset_seen" in summary["threshold_failures"]


def test_summarize_soak_counts_recovered_command_retries():
    row = sample(
        "start",
        0,
        {
            "uptime_ms": 1000,
            "heap_free": 100000,
            "heap_min_free": 99000,
            "psram_free": 200000,
            "psram_min_free": 199000,
            "current_task_stack_free_words": 1200,
            "ui_task_stack_free_words": 1300,
            "lvgl_used_pct": 50,
        },
        {"rx_packets": 5, "tx_packets": 7},
    )
    row["results"][0]["attempts"] = 2
    row["results"][0]["recovered_after_retry"] = True
    row["results"][0]["retry_failures"] = [
        {"schema": 1, "ok": False, "cmd": "health", "code": "ESP_ERR_TIMEOUT"}
    ]

    summary = soak_d1l.summarize_soak(
        samples=[row],
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=1,
        min_tx_delta=0,
    )

    assert summary["ok"] is True
    assert summary["command_retry_count"] == 1
    assert summary["command_recovered_after_retry_count"] == 1
    assert summary["command_retry_failure_count"] == 0


def test_summarize_soak_rejects_legacy_recovered_host_timeout():
    row = sample(
        "start", 0, base_health(), {"rx_packets": 0, "tx_packets": 0}
    )
    row["results"][0]["attempts"] = 2
    row["results"][0]["recovered_after_retry"] = True
    row["results"][0]["retry_failures"] = [
        {"schema": 1, "ok": False, "cmd": "health", "code": "TIMEOUT"}
    ]

    summary = soak_d1l.summarize_soak(
        samples=[row],
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=0,
        min_tx_delta=0,
    )

    assert summary["ok"] is False
    assert summary["command_timeout_seen"] is True
    assert "command_timeout_seen" in summary["threshold_failures"]


def test_summarize_soak_rejects_ignored_boot_marker():
    samples = [
        sample("start", 0, base_health(), {"rx_packets": 0, "tx_packets": 0}),
        sample("final", 1, base_health(2000), {"rx_packets": 0, "tx_packets": 0}),
    ]
    samples[0]["results"][0]["ignored_json"] = [
        {"cmd": "help", "ok": True}
    ]

    summary = soak_d1l.summarize_soak(
        samples=samples,
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=0,
        min_tx_delta=0,
    )

    assert summary["unexpected_console_restart_seen"] is True
    assert "unexpected_console_restart_seen" in summary["threshold_failures"]
    assert summary["ok"] is False


def test_summarize_soak_tracks_stable_sd_file_op_gate_and_store_backends():
    rows = [
        sample("start", 0, base_health(1000), {"rx_packets": 5, "tx_packets": 7}),
        sample("final", 60, base_health(61000), {"rx_packets": 8, "tx_packets": 7}),
    ]
    for row in rows:
        row["results"].append(storage_status())

    summary = soak_d1l.summarize_soak(
        samples=rows,
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=1,
        min_tx_delta=0,
        sample_storage=True,
    )

    assert summary["ok"] is True
    assert summary["storage_status_count"] == 2
    assert summary["storage_states"] == ["ready"]
    assert summary["storage_data_backends"] == ["mixed"]
    assert summary["storage_packet_log_backends"] == ["sd"]
    assert summary["storage_file_capability_ready_all"] is True
    assert summary["storage_file_ops_ready_all"] is True
    assert summary["storage_store_backends"]["packets"] == ["sd"]
    assert summary["storage_store_backends"]["exports"] == ["sd_diagnostic_exports_ready"]
    assert summary["storage_store_backend_stable_all"] is True


def test_summarize_soak_accepts_stable_retained_history_sd_backends():
    rows = [
        sample("start", 0, base_health(1000), {"rx_packets": 5, "tx_packets": 7}),
        sample("final", 60, base_health(61000), {"rx_packets": 8, "tx_packets": 7}),
    ]
    for row in rows:
        row["results"].append(
            storage_status(
                message_store_backend="sd",
                dm_store_backend="sd",
                packet_log_backend="sd",
                route_store_backend="sd",
            )
        )

    summary = soak_d1l.summarize_soak(
        samples=rows,
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=1,
        min_tx_delta=0,
        sample_storage=True,
    )

    assert summary["ok"] is True
    assert summary["storage_store_backends"]["messages"] == ["sd"]
    assert summary["storage_store_backends"]["dm"] == ["sd"]
    assert summary["storage_store_backends"]["packets"] == ["sd"]
    assert summary["storage_store_backends"]["routes"] == ["sd"]
    assert summary["storage_store_backend_stable_all"] is True


def test_summarize_soak_rejects_stale_storage_telemetry():
    row = sample("start", 0, base_health(), {"rx_packets": 5, "tx_packets": 7})
    row["results"].append(storage_status(status_stale=True, refresh_failures=3))

    summary = soak_d1l.summarize_soak(
        samples=[row],
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=1,
        min_tx_delta=0,
        sample_storage=True,
    )

    assert summary["ok"] is False
    assert summary["storage_status_stale_count"] == 1
    assert summary["storage_refresh_failures_max"] == 3
    assert "storage_status_stale" in summary["threshold_failures"]
    assert "storage_refresh_failures" in summary["threshold_failures"]


def test_summarize_soak_allows_pre_flash_sd_filecanary_unavailable():
    row = sample("start", 0, base_health(), {"rx_packets": 5, "tx_packets": 7})
    row["results"].append(
        storage_status(
            state="protocol_pending",
            protocol_supported=False,
            present=False,
            mounted=False,
            data_root_ready=False,
            file_ops=False,
            atomic_rename=False,
            data_enabled=False,
            data_backend="nvs",
            packet_log_backend="nvs",
        )
    )
    row["results"].append(
        {
            "schema": 1,
            "ok": False,
            "cmd": soak_d1l.SD_FILE_CANARY_COMMAND,
            "code": "ESP_ERR_NOT_SUPPORTED",
            "step": "preflight",
        }
    )

    summary = soak_d1l.summarize_soak(
        samples=[row],
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=1,
        min_tx_delta=0,
        sample_storage=True,
        sd_file_canary=True,
        allow_sd_unavailable=True,
    )

    assert summary["ok"] is True
    assert summary["command_failure_count"] == 0
    assert summary["sd_file_canary_unavailable_count"] == 1
    assert summary["storage_file_ops_ready_all"] is False


def test_summarize_soak_fails_store_backend_flip():
    start = sample("start", 0, base_health(1000), {"rx_packets": 5, "tx_packets": 7})
    final = sample("final", 60, base_health(61000), {"rx_packets": 8, "tx_packets": 7})
    start["results"].append(storage_status())
    changed = storage_status()
    changed["stores"] = dict(changed["stores"])
    changed["stores"]["messages"] = "sd"
    final["results"].append(changed)

    summary = soak_d1l.summarize_soak(
        samples=[start, final],
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=1,
        min_tx_delta=0,
        sample_storage=True,
    )

    assert summary["ok"] is False
    assert summary["storage_store_backend_stable"]["messages"] is False
    assert "storage_store_backend_changed" in summary["threshold_failures"]


def test_summarize_soak_fails_pre_flash_sd_filecanary_without_allow_flag():
    row = sample("start", 0, base_health(), {"rx_packets": 5, "tx_packets": 7})
    row["results"].append(storage_status(state="protocol_pending", file_ops=False))
    row["results"].append(
        {
            "schema": 1,
            "ok": False,
            "cmd": soak_d1l.SD_FILE_CANARY_COMMAND,
            "code": "ESP_ERR_NOT_SUPPORTED",
            "step": "preflight",
        }
    )

    summary = soak_d1l.summarize_soak(
        samples=[row],
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=1,
        min_tx_delta=0,
        sample_storage=True,
        sd_file_canary=True,
        allow_sd_unavailable=False,
    )

    assert summary["ok"] is False
    assert summary["command_failure_count"] == 1
    assert "sd_file_canary_failed" in summary["threshold_failures"]


def test_summarize_soak_fails_malformed_storage_status_when_requested():
    row = sample("start", 0, base_health(), {"rx_packets": 5, "tx_packets": 7})
    row["results"].append(
        {
            "schema": 1,
            "ok": True,
            "cmd": soak_d1l.STORAGE_STATUS_COMMAND,
            "sd": {"state": "ready"},
            "data_backend": "mixed",
        }
    )

    summary = soak_d1l.summarize_soak(
        samples=[row],
        active_events=[],
        require_rx_delta=False,
        min_rx_delta=1,
        min_tx_delta=0,
        sample_storage=True,
    )

    assert summary["ok"] is False
    assert summary["storage_status_malformed_count"] == 1
    assert "storage_status_malformed" in summary["threshold_failures"]


def test_send_soak_command_does_not_retry_allowed_sd_unavailable(monkeypatch):
    calls = []

    def fake_send_console_command(_ser, command, _timeout):
        calls.append(command)
        return {
            "schema": 1,
            "ok": False,
            "cmd": soak_d1l.SD_FILE_CANARY_COMMAND,
            "code": "ESP_ERR_NOT_SUPPORTED",
            "step": "preflight",
        }

    monkeypatch.setattr(soak_d1l, "send_console_command", fake_send_console_command)

    result = soak_d1l.send_soak_command(
        None,
        soak_d1l.SD_FILE_CANARY_COMMAND,
        timeout=1.0,
        retries=3,
        retry_delay_sec=0.0,
        terminal_failure_ok=lambda row: soak_d1l.allowed_sd_unavailable(row, True),
    )

    assert len(calls) == 1
    assert result["allowed_failure"] is True
    assert "attempts" not in result


def test_sd_filecanary_timeout_uses_long_window_and_is_not_retried(monkeypatch):
    calls = []

    def fake_send_console_command(_ser, command, timeout):
        calls.append((command, timeout))
        return {
            "schema": 1,
            "ok": False,
            "cmd": command,
            "code": "TIMEOUT",
        }

    monkeypatch.setattr(soak_d1l, "send_console_command", fake_send_console_command)
    result = soak_d1l.send_soak_command(
        None,
        soak_d1l.SD_FILE_CANARY_COMMAND,
        timeout=1.0,
        retries=3,
        retry_delay_sec=0.0,
    )

    assert calls == [(soak_d1l.SD_FILE_CANARY_COMMAND, 120.0)]
    assert result["code"] == "TIMEOUT"
    assert result["attempts"] == 1
    assert result["retry_failures"] == []


def test_send_soak_command_stops_on_ignored_boot_marker(monkeypatch):
    calls = []

    def fake_send_console_command(_ser, command, timeout):
        calls.append((command, timeout))
        return {
            "schema": 1,
            "ok": True,
            "cmd": command,
            "ignored_boot_help_seen": True,
            "ignored_json": [{"cmd": "noise-5", "ok": True}],
        }

    monkeypatch.setattr(soak_d1l, "send_console_command", fake_send_console_command)
    result = soak_d1l.send_soak_command(
        None,
        "health",
        timeout=1.0,
        retries=3,
        retry_delay_sec=0.0,
    )

    assert calls == [("health", 1.0)]
    assert result["ok"] is False
    assert result["code"] == "UNEXPECTED_RESTART"
    assert result["unexpected_console_restart"] is True


def test_collect_sample_aborts_after_timeout_without_followup_commands(monkeypatch):
    calls = []

    def fake_send_soak_command(_ser, command, *_args, **_kwargs):
        calls.append(command)
        return {
            "schema": 1,
            "ok": False,
            "cmd": command,
            "code": "TIMEOUT",
        }

    monkeypatch.setattr(soak_d1l, "send_soak_command", fake_send_soak_command)
    row = soak_d1l.collect_sample(
        None,
        timeout=1.0,
        label="start",
        elapsed_sec=0.0,
        command_retries=3,
        retry_delay_sec=0.0,
        commands=["health", "storage status", "crashlog"],
    )

    assert calls == ["health"]
    assert row["aborted_after_timeout"] == "health"
    assert len(row["results"]) == 1


def test_active_listener_flow_requires_exact_counters_and_12_hex_sender():
    peer_public_key = "0123456789abcdef" + "22" * 24
    d1l_public_key = "abcdef012345" + "33" * 26

    def listener_status(
        *,
        rx_dm: int,
        tx_dm: int,
        replies: int,
        ack_misses: int,
        rx_at: str,
        tx_at: str,
    ) -> dict:
        return {
            "run_id": "listener-run-1",
            "service": "openclaw-radio-listener",
            "serial": {
                "port": "COM15",
                "mesh_connected": True,
                "public_key": peer_public_key,
            },
            "mesh": {
                "last_rx_sender": d1l_public_key[:12],
                "last_rx_kind": "dm",
                "last_tx_kind": "dm",
                "last_rx_at": rx_at,
                "last_tx_at": tx_at,
            },
            "counters": {
                "rx_dm_total": rx_dm,
                "tx_dm_total": tx_dm,
                "local_fast_reply_total": replies,
                "tx_dm_ack_miss_total": ack_misses,
            },
        }

    before = listener_status(
        rx_dm=10,
        tx_dm=20,
        replies=30,
        ack_misses=2,
        rx_at="before-rx",
        tx_at="before-tx",
    )
    after = listener_status(
        rx_dm=16,
        tx_dm=26,
        replies=36,
        ack_misses=2,
        rx_at="after-rx",
        tx_at="after-tx",
    )
    ok, deltas = soak_d1l.active_listener_flow_ok(
        before,
        after,
        successful_send_count=6,
        d1l_public_key=d1l_public_key,
        peer_fingerprint=peer_public_key[:16].upper(),
        peer_public_key=peer_public_key,
        minimum_send_count=6,
    )

    assert ok is True
    assert deltas == {
        "rx_dm_total": 6,
        "tx_dm_total": 6,
        "local_fast_reply_total": 6,
        "tx_dm_ack_miss_total": 0,
    }

    wrong_sender = json.loads(json.dumps(after))
    wrong_sender["mesh"]["last_rx_sender"] = d1l_public_key[:16]
    assert soak_d1l.active_listener_flow_ok(
        before,
        wrong_sender,
        successful_send_count=6,
        d1l_public_key=d1l_public_key,
        peer_fingerprint=peer_public_key[:16].upper(),
        peer_public_key=peer_public_key,
        minimum_send_count=6,
    )[0] is False
    assert soak_d1l.active_listener_flow_ok(
        before,
        after,
        successful_send_count=5,
        d1l_public_key=d1l_public_key,
        peer_fingerprint=peer_public_key[:16].upper(),
        peer_public_key=peer_public_key,
        minimum_send_count=6,
    )[0] is False


@pytest.mark.parametrize("local", [False, True])
def test_active_listener_flow_accepts_only_exact_pinned_peer_binding(
    local,
):
    rf = soak_d1l.rf_acceptance
    config = (
        rf.local_peer_config()
        if local
        else rf.remote_peer_config()
    )
    d1l_public_key = rf.DEFAULT_D1L_PUBLIC_KEY

    def status(
        *,
        rx: int,
        tx: int,
        replies: int,
        rx_at: str,
        tx_at: str,
    ) -> dict:
        return {
            "run_id": "pi5-peer-run",
            "service": "openclaw-radio-listener",
            "serial": {
                "port": rf.REMOTE_PEER_DEVICE,
                "mesh_connected": True,
                "public_key": rf.REMOTE_PEER_PUBLIC_KEY,
            },
            "mesh": {
                "last_rx_sender": d1l_public_key[:12],
                "last_rx_kind": "dm",
                "last_tx_kind": "dm",
                "last_rx_at": rx_at,
                "last_tx_at": tx_at,
            },
            "counters": {
                "rx_dm_total": rx,
                "tx_dm_total": tx,
                "local_fast_reply_total": replies,
                "tx_dm_ack_miss_total": 2,
            },
        }

    before = status(
        rx=10,
        tx=20,
        replies=30,
        rx_at="before-rx",
        tx_at="before-tx",
    )
    after = status(
        rx=16,
        tx=26,
        replies=36,
        rx_at="after-rx",
        tx_at="after-tx",
    )
    arguments = {
        "successful_send_count": 6,
        "d1l_public_key": d1l_public_key,
        "peer_fingerprint": rf.REMOTE_PEER_FINGERPRINT,
        "peer_public_key": rf.REMOTE_PEER_PUBLIC_KEY,
        "minimum_send_count": 6,
        (
            "local_config" if local else "remote_config"
        ): config,
    }

    ok, deltas = soak_d1l.active_listener_flow_ok(
        before,
        after,
        **arguments,
    )

    assert ok is True
    assert deltas["rx_dm_total"] == 6

    wrong_device = json.loads(json.dumps(after))
    wrong_device["serial"]["port"] = "/dev/krab-other"
    assert (
        soak_d1l.active_listener_flow_ok(
            before,
            wrong_device,
            **arguments,
        )[0]
        is False
    )

    mixed_arguments = {
        **arguments,
        "remote_config": rf.remote_peer_config(),
        "local_config": rf.local_peer_config(),
    }
    assert (
        soak_d1l.active_listener_flow_ok(
            before,
            after,
            **mixed_arguments,
        )[0]
        is False
    )

    disconnected = json.loads(json.dumps(after))
    disconnected["serial"]["mesh_connected"] = False
    assert (
        soak_d1l.active_listener_flow_ok(
            before,
            disconnected,
            **arguments,
        )[0]
        is False
    )


def test_openclaw_active_soak_text_and_send_floor_are_fail_fast():
    assert soak_d1l.listener_test_text_ok("Core acceptance TEST 1")
    assert soak_d1l.listener_test_text_ok("core_soak_test")
    assert not soak_d1l.listener_test_text_ok("core soak")
    assert not soak_d1l.listener_test_text_ok("contest")
    assert soak_d1l.expected_active_send_count(3600, 600) == 6
    assert soak_d1l.expected_active_send_count(3600, 601) == 6
    assert soak_d1l.expected_active_send_count(3600, 0) is None


@pytest.mark.parametrize(
    "port",
    ["COM8", "COM11", "COM16", "COM29", "/dev/ttyUSB2"],
)
def test_core_disabled_soak_rejects_unsafe_target_before_serial_open(port):
    with pytest.raises(ValueError, match="requires COM12"):
        run_soak_for_timeout_test(
            port=port,
            expected_release_profile="core_1_0",
            expected_sd_history_mode="disabled",
            sample_storage=True,
            allow_sd_unavailable=True,
        )


@pytest.mark.parametrize(
    ("release_profile", "sd_history_mode"),
    [
        (None, None),
        ("core_1_0", None),
        (None, "disabled"),
        ("full_feature", "disabled"),
        ("core_1_0", "enabled"),
    ],
)
def test_release_bound_soak_requires_exact_core_contract_before_io(
    release_profile,
    sd_history_mode,
    monkeypatch,
):
    monkeypatch.setattr(
        soak_d1l,
        "git_metadata",
        lambda _root: pytest.fail(
            "source or hardware I/O must not begin without the Core contract"
        ),
    )
    monkeypatch.setattr(
        soak_d1l,
        "open_d1l_serial",
        lambda *_args, **_kwargs: pytest.fail(
            "serial must not open without the Core contract"
        ),
    )

    with pytest.raises(ValueError, match="release-bound hardware soak"):
        run_soak_for_timeout_test(
            expected_firmware_commit="a" * 40,
            github_run_id="123",
            workflow_run_attempt="1",
            expected_release_profile=release_profile,
            expected_sd_history_mode=sd_history_mode,
        )


def test_release_bound_active_soak_rejects_raw_tty_before_peer_or_serial(
    monkeypatch,
):
    external_calls = []

    def unexpected_external(*_args, **_kwargs):
        external_calls.append(True)
        raise AssertionError("unsafe target must fail before external I/O")

    monkeypatch.setattr(
        soak_d1l,
        "git_metadata",
        lambda _root: {
            "commit": "a" * 40,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(
        soak_d1l,
        "qualified_controlled_peer_receipt",
        unexpected_external,
    )
    monkeypatch.setattr(soak_d1l, "open_d1l_serial", unexpected_external)
    monkeypatch.setattr(
        soak_d1l.rf_acceptance,
        "capture_remote_peer_status",
        unexpected_external,
    )

    with pytest.raises(ValueError, match="requires COM12"):
        run_soak_for_timeout_test(
            port="/dev/ttyUSB2",
            active_dm_fingerprint=(
                soak_d1l.rf_acceptance.REMOTE_PEER_FINGERPRINT
            ),
            active_dm_text="core soak test",
            expected_firmware_commit="a" * 40,
            github_run_id="123",
            workflow_run_attempt="1",
            expected_release_profile="core_1_0",
            expected_sd_history_mode="disabled",
            sample_storage=True,
            allow_sd_unavailable=True,
        )

    assert external_calls == []


class FakeSoakPort:
    def reset_input_buffer(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False


def run_soak_for_timeout_test(**overrides):
    args = {
        "port": "COM12",
        "baud": 115200,
        "timeout": 1.0,
        "duration_sec": 10.0,
        "sample_interval_sec": 60.0,
        "active_public_text": None,
        "active_dm_fingerprint": None,
        "active_dm_text": None,
        "active_interval_sec": 60.0,
        "startup_settle_sec": 0.0,
        "require_rx_delta": False,
        "min_rx_delta": 0,
        "min_tx_delta": 0,
        "clear_crashlog_before_start": False,
        "command_retries": 1,
        "retry_delay_sec": 0.0,
        "sample_storage": False,
        "sd_file_canary": False,
        "allow_sd_unavailable": False,
        "expected_firmware_commit": None,
    }
    args.update(overrides)
    return soak_d1l.run_serial_soak(**args)


def windows_target_row(*, vid=0x1A86, pid=0x7523, location="1-2"):
    return {
        "device": "COM12",
        "vid": vid,
        "pid": pid,
        "serial_number": None,
        "hwid": f"USB VID:PID={vid:04X}:{pid:04X} LOCATION={location}",
        "location": location,
    }


def windows_target_snapshot():
    return soak_d1l.resolve_core_target(
        "COM12",
        port_lister=lambda: [windows_target_row()],
        platform_name="nt",
    )


def test_core_soak_binds_target_before_and_after_serial(monkeypatch):
    lister_calls = []
    opened = []

    def list_target():
        lister_calls.append(True)
        return [windows_target_row()]

    def fake_collect(_ser, _timeout, label, elapsed_sec, *_args, **_kwargs):
        row = sample(
            label,
            elapsed_sec,
            base_health(),
            {"rx_packets": 0, "tx_packets": 0},
        )
        row["aborted_after_timeout"] = None
        return row

    monkeypatch.setattr(
        soak_d1l,
        "open_d1l_serial",
        lambda *_args, **kwargs: (
            opened.append(kwargs["port"]) or FakeSoakPort()
        ),
    )
    monkeypatch.setattr(soak_d1l, "collect_sample", fake_collect)
    monkeypatch.setattr(soak_d1l.time, "sleep", lambda _seconds: None)

    report = run_soak_for_timeout_test(
        duration_sec=0.001,
        expected_release_profile="core_1_0",
        expected_sd_history_mode="disabled",
        sample_storage=True,
        allow_sd_unavailable=True,
        port_lister=list_target,
        platform_name="nt",
    )

    assert report["schema"] == 2
    assert report["port"] == "COM12"
    assert report["d1l_target"]["requested_path"] == "COM12"
    assert report["d1l_target_after"]["requested_path"] == "COM12"
    assert report["target_identity_continuity_ok"] is True
    assert (
        report["d1l_target"]["stable_identity_sha256"]
        == report["d1l_target_after"]["stable_identity_sha256"]
    )
    assert opened == ["COM12"]
    assert len(lister_calls) == 2


def test_qualified_rf_receipt_consumes_strict_schema2_target_binding(
    tmp_path,
    monkeypatch,
):
    rf = soak_d1l.rf_acceptance
    commit = "a" * 40
    target = windows_target_snapshot()
    config = rf.remote_peer_config()
    report = {
        "schema": 2,
        "mode": "rf-full-acceptance",
        "ok": True,
        "closure_eligible": True,
        "expected_firmware_commit": commit,
        "github_actions_run": "123",
        "workflow_run_attempt": "1",
        "target_fingerprint": rf.REMOTE_PEER_FINGERPRINT,
        "controlled_peer": {
            "evidence_source": rf.REMOTE_PEER_EVIDENCE_SOURCE,
            "port": None,
            "fingerprint": rf.REMOTE_PEER_FINGERPRINT,
            **config,
        },
        "controlled_peer_adapter": rf.REMOTE_PEER_ADAPTER,
        "d1l_public_key": rf.DEFAULT_D1L_PUBLIC_KEY,
        "public_rf_tx": False,
        "port": "COM12",
        "d1l_target": target,
        "d1l_target_after": json.loads(json.dumps(target)),
        "target_identity_continuity_ok": True,
        "checks": {"d1l_target_identity_continuity": True},
    }
    receipt = tmp_path / "rf.json"
    receipt.write_text(json.dumps(report), encoding="utf-8")
    monkeypatch.setattr(
        soak_d1l,
        "pinned_peer_evidence_metadata_ok",
        lambda *_args, **_kwargs: True,
    )
    monkeypatch.setattr(
        rf,
        "remote_peer_report_shape_ok",
        lambda _data: True,
    )

    qualified, row = soak_d1l.qualified_controlled_peer_receipt(
        path=receipt,
        root=tmp_path,
        commit=commit,
        run_id="123",
        run_attempt="1",
        fingerprint=rf.REMOTE_PEER_FINGERPRINT,
        expected_d1l_target_sha256=target[
            "stable_identity_sha256"
        ],
    )

    assert qualified == report
    assert row["path"] == "rf.json"

    forged = json.loads(json.dumps(report))
    forged["d1l_target_after"]["vid"] = 0x10C4
    receipt.unlink()
    receipt.write_text(json.dumps(forged), encoding="utf-8")
    with pytest.raises(
        ValueError,
        match="not bound to the exact D1L serial target",
    ):
        soak_d1l.qualified_controlled_peer_receipt(
            path=receipt,
            root=tmp_path,
            commit=commit,
            run_id="123",
            run_attempt="1",
            fingerprint=rf.REMOTE_PEER_FINGERPRINT,
            expected_d1l_target_sha256=target[
                "stable_identity_sha256"
            ],
        )


def test_core_soak_rejects_wrong_usb_identity_before_serial(monkeypatch):
    monkeypatch.setattr(
        soak_d1l,
        "open_d1l_serial",
        lambda *_args, **_kwargs: pytest.fail(
            "serial must not open for the wrong USB identity"
        ),
    )

    with pytest.raises(ValueError, match="VID"):
        run_soak_for_timeout_test(
            expected_release_profile="core_1_0",
            expected_sd_history_mode="disabled",
            sample_storage=True,
            allow_sd_unavailable=True,
            port_lister=lambda: [windows_target_row(vid=0x10C4)],
            platform_name="nt",
        )


def test_core_soak_rejects_target_drift_after_serial(monkeypatch):
    locations = iter(("1-2", "1-9"))

    def fake_collect(_ser, _timeout, label, elapsed_sec, *_args, **_kwargs):
        row = sample(
            label,
            elapsed_sec,
            base_health(),
            {"rx_packets": 0, "tx_packets": 0},
        )
        row["aborted_after_timeout"] = None
        return row

    monkeypatch.setattr(
        soak_d1l,
        "open_d1l_serial",
        lambda *_args, **_kwargs: FakeSoakPort(),
    )
    monkeypatch.setattr(soak_d1l, "collect_sample", fake_collect)
    monkeypatch.setattr(soak_d1l.time, "sleep", lambda _seconds: None)

    with pytest.raises(ValueError, match="identity changed"):
        run_soak_for_timeout_test(
            duration_sec=0.001,
            expected_release_profile="core_1_0",
            expected_sd_history_mode="disabled",
            sample_storage=True,
            allow_sd_unavailable=True,
            port_lister=lambda: [
                windows_target_row(location=next(locations))
            ],
            platform_name="nt",
        )


def test_release_active_soak_rejects_non_test_text_before_peer_capture(
    monkeypatch,
):
    commit = "a" * 40

    class FakeSerialModule:
        pass

    monkeypatch.setitem(sys.modules, "serial", FakeSerialModule())
    monkeypatch.setattr(
        soak_d1l,
        "git_metadata",
        lambda _root: {
            "commit": commit,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    monkeypatch.setattr(
        soak_d1l,
        "qualified_controlled_peer_receipt",
        lambda **_kwargs: (
            {
                "controlled_peer": {
                    "public_key": "0123456789abcdef" + "22" * 24,
                },
                "d1l_public_key": "abcdef012345" + "33" * 26,
            },
            {"path": "rf.json", "size": 1, "sha256": "0" * 64},
        ),
    )
    monkeypatch.setattr(
        soak_d1l.rf_acceptance,
        "capture_peer_status",
        lambda *_args, **_kwargs: pytest.fail(
            "peer status must not be captured for invalid test text"
        ),
    )
    monkeypatch.setattr(
        soak_d1l,
        "open_d1l_serial",
        lambda *_args, **_kwargs: pytest.fail(
            "COM12 must not open for invalid test text"
        ),
    )

    with pytest.raises(ValueError, match="word 'test'"):
        run_soak_for_timeout_test(
            active_dm_fingerprint="0123456789ABCDEF",
            active_dm_text="core soak",
            expected_firmware_commit=commit,
            github_run_id="123",
            workflow_run_attempt="1",
            expected_release_profile="core_1_0",
            expected_sd_history_mode="disabled",
            sample_storage=True,
            allow_sd_unavailable=True,
            controlled_peer_receipt=Path("ignored.json"),
            port_lister=lambda: [windows_target_row()],
            platform_name="nt",
        )


def test_release_active_soak_wrong_full_d1l_key_has_no_peer_or_rf_io(
    tmp_path,
    monkeypatch,
):
    rf = soak_d1l.rf_acceptance
    commit = "a" * 40
    expected_key = rf.DEFAULT_D1L_PUBLIC_KEY
    wrong_suffix = (
        "0" * 48 if expected_key[16:] != "0" * 48 else "1" * 48
    )
    wrong_key = expected_key[:16] + wrong_suffix
    commands = []
    peer_calls = []

    (tmp_path / "scripts").mkdir()
    monkeypatch.setattr(
        soak_d1l,
        "__file__",
        str(tmp_path / "scripts" / "soak_d1l.py"),
    )
    monkeypatch.setattr(
        soak_d1l,
        "git_metadata",
        lambda _root: {
            "commit": commit,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    config = rf.remote_peer_config()
    peer_report = {
        "controlled_peer": {
            "evidence_source": rf.REMOTE_PEER_EVIDENCE_SOURCE,
            "port": None,
            "fingerprint": rf.REMOTE_PEER_FINGERPRINT,
            **config,
        },
        "controlled_peer_adapter": rf.REMOTE_PEER_ADAPTER,
        "d1l_public_key": expected_key,
    }
    monkeypatch.setattr(
        soak_d1l,
        "qualified_controlled_peer_receipt",
        lambda **_kwargs: (peer_report, {"path": "rf.json"}),
    )
    monkeypatch.setattr(
        soak_d1l,
        "open_d1l_serial",
        lambda *_args, **_kwargs: FakeSoakPort(),
    )

    def fake_send(_ser, command, *_args, **_kwargs):
        commands.append(command)
        if command == "version":
            return {
                "schema": 1,
                "ok": True,
                "cmd": "version",
                "build_commit": commit,
                "release_profile": "core_1_0",
                "sd_history_mode": "disabled",
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

    def unexpected_peer(*_args, **_kwargs):
        peer_calls.append(True)
        raise AssertionError("peer I/O must not begin for the wrong D1L key")

    monkeypatch.setattr(soak_d1l, "send_soak_command", fake_send)
    monkeypatch.setattr(
        rf,
        "capture_remote_peer_status",
        unexpected_peer,
    )
    monkeypatch.setattr(
        soak_d1l,
        "collect_sample",
        lambda *_args, **_kwargs: pytest.fail(
            "sampling must not begin for the wrong D1L key"
        ),
    )
    monkeypatch.setattr(soak_d1l.time, "sleep", lambda _seconds: None)

    report = run_soak_for_timeout_test(
        duration_sec=0.001,
        active_dm_fingerprint=rf.REMOTE_PEER_FINGERPRINT,
        active_dm_text="core soak test",
        expected_firmware_commit=commit,
        github_run_id="123",
        workflow_run_attempt="1",
        expected_release_profile="core_1_0",
        expected_sd_history_mode="disabled",
        sample_storage=True,
        allow_sd_unavailable=True,
        controlled_peer_receipt=tmp_path / "rf.json",
        peer_capture_dir=tmp_path / "artifacts" / "soak" / "rf-peer",
        port_lister=lambda: [windows_target_row()],
        platform_name="nt",
    )

    assert commands == ["version", "identity status"]
    assert peer_calls == []
    assert report["preflight_commands"] == [
        "version",
        "identity status",
    ]
    assert report["expected_d1l_public_key"] == expected_key
    assert report["d1l_identity_status"]["public_key"] == wrong_key
    assert report["d1l_identity_ok"] is False
    assert report["preflight_failure"] == "d1l_identity_mismatch"
    assert report["active_events"] == []
    assert report["samples"] == []
    assert report["controlled_peer_before"] == {}
    assert report["controlled_peer_after"] == {}
    assert report["dm_rf_tx"] is False
    assert report["closure_eligible"] is False
    assert report["ok"] is False


def test_soak_stops_after_crashlog_clear_timeout(monkeypatch):
    calls = []

    def fake_send(_ser, command, *_args, **_kwargs):
        calls.append(command)
        return {"schema": 1, "ok": False, "cmd": command, "code": "TIMEOUT"}

    class FakeSerialModule:
        pass

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(soak_d1l, "open_d1l_serial", lambda *_args, **_kwargs: FakeSoakPort())
    monkeypatch.setattr(soak_d1l, "send_soak_command", fake_send)
    monkeypatch.setattr(soak_d1l.time, "sleep", lambda _seconds: None)
    report = run_soak_for_timeout_test(clear_crashlog_before_start=True)

    assert calls == ["crashlog clear"]
    assert report["aborted_after_timeout"] == "crashlog clear"
    assert report["samples"] == []
    assert report["ok"] is False
    assert report["commands"] == soak_d1l.SOAK_COMMANDS


def test_soak_stops_after_active_command_timeout(monkeypatch):
    calls = []
    collected = []

    def fake_send(_ser, command, *_args, **_kwargs):
        calls.append(command)
        return {"schema": 1, "ok": False, "cmd": command, "code": "TIMEOUT"}

    def fake_collect(*_args, **_kwargs):
        collected.append(True)
        row = sample(
            "start", 0, base_health(), {"rx_packets": 0, "tx_packets": 0}
        )
        row["aborted_after_timeout"] = None
        return row

    class FakeSerialModule:
        pass

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(soak_d1l, "open_d1l_serial", lambda *_args, **_kwargs: FakeSoakPort())
    monkeypatch.setattr(soak_d1l, "send_soak_command", fake_send)
    monkeypatch.setattr(soak_d1l, "collect_sample", fake_collect)
    monkeypatch.setattr(soak_d1l.time, "sleep", lambda _seconds: None)
    report = run_soak_for_timeout_test(
        active_dm_fingerprint="0BF0A701D5AE2DB6",
        active_dm_text="test",
    )

    assert len(collected) == 1
    assert calls == ["mesh send dm 0BF0A701D5AE2DB6 test"]
    assert report["aborted_after_timeout"] == "mesh send dm 0BF0A701D5AE2DB6 test"
    assert len(report["samples"]) == 1
    assert report["ok"] is False


def test_final_sample_timeout_is_reported_at_top_level(monkeypatch):
    collected = []

    def fake_collect(_ser, _timeout, label, elapsed_sec, *_args, **_kwargs):
        collected.append(label)
        if label == "final":
            return {
                "label": label,
                "elapsed_sec": elapsed_sec,
                "aborted_after_timeout": "health",
                "results": [
                    {
                        "schema": 1,
                        "ok": False,
                        "cmd": "health",
                        "code": "TIMEOUT",
                    }
                ],
            }
        row = sample(
            label,
            elapsed_sec,
            base_health(),
            {"rx_packets": 0, "tx_packets": 0},
        )
        row["aborted_after_timeout"] = None
        return row

    class FakeSerialModule:
        pass

    monkeypatch.setitem(__import__("sys").modules, "serial", FakeSerialModule())
    monkeypatch.setattr(
        soak_d1l, "open_d1l_serial", lambda *_args, **_kwargs: FakeSoakPort()
    )
    monkeypatch.setattr(soak_d1l, "collect_sample", fake_collect)
    monkeypatch.setattr(soak_d1l.time, "sleep", lambda _seconds: None)
    report = run_soak_for_timeout_test(duration_sec=0.001)

    assert collected == ["start", "final"]
    assert report["aborted_after_timeout"] == "health"
    assert report["ok"] is False


def test_hardware_soak_preflights_exact_firmware_identity_before_sampling(monkeypatch):
    commit = "a" * 40
    calls = []

    def fake_send(_ser, command, *_args, **_kwargs):
        calls.append(command)
        return {
            "schema": 1,
            "ok": True,
            "cmd": command,
            "build_commit": commit.upper(),
        }

    def fake_collect(_ser, _timeout, label, elapsed_sec, *_args, **_kwargs):
        calls.append(f"sample:{label}")
        row = sample(label, elapsed_sec, base_health(), {"rx_packets": 0, "tx_packets": 0})
        row["aborted_after_timeout"] = None
        return row

    class FakeSerialModule:
        pass

    monkeypatch.setitem(sys.modules, "serial", FakeSerialModule())
    monkeypatch.setattr(soak_d1l, "open_d1l_serial", lambda *_args, **_kwargs: FakeSoakPort())
    monkeypatch.setattr(soak_d1l, "send_soak_command", fake_send)
    monkeypatch.setattr(soak_d1l, "collect_sample", fake_collect)
    monkeypatch.setattr(soak_d1l.time, "sleep", lambda _seconds: None)

    report = run_soak_for_timeout_test(
        duration_sec=0.001,
        expected_firmware_commit=commit,
    )

    assert calls[0] == "version"
    assert calls[1].startswith("sample:")
    assert report["preflight_commands"] == ["version"]
    assert report["expected_firmware_commit"] == commit
    assert report["device_build_commit"] == commit.upper()
    assert report["firmware_identity_required"] is True
    assert report["firmware_identity_ok"] is True
    assert report["version_preflight"]["cmd"] == "version"
    assert report["preflight_failure"] is None
    assert report["ok"] is True


def test_hardware_soak_fails_closed_before_sampling_on_commit_mismatch(monkeypatch):
    expected = "a" * 40
    wrong = "b" * 40
    calls = []

    def fake_send(_ser, command, *_args, **_kwargs):
        calls.append(command)
        return {
            "schema": 1,
            "ok": True,
            "cmd": command,
            "build_commit": wrong,
        }

    class FakeSerialModule:
        pass

    monkeypatch.setitem(sys.modules, "serial", FakeSerialModule())
    monkeypatch.setattr(soak_d1l, "open_d1l_serial", lambda *_args, **_kwargs: FakeSoakPort())
    monkeypatch.setattr(soak_d1l, "send_soak_command", fake_send)
    monkeypatch.setattr(
        soak_d1l,
        "collect_sample",
        lambda *_args, **_kwargs: pytest.fail("sampling must not start after identity mismatch"),
    )
    monkeypatch.setattr(soak_d1l.time, "sleep", lambda _seconds: None)

    report = run_soak_for_timeout_test(
        clear_crashlog_before_start=True,
        expected_firmware_commit=expected,
    )

    assert calls == ["version"]
    assert report["samples"] == []
    assert report["device_build_commit"] == wrong
    assert report["firmware_identity_ok"] is False
    assert report["preflight_failure"] == "firmware_identity_mismatch"
    assert "firmware_identity_mismatch" in report["summary"]["threshold_failures"]
    assert report["ok"] is False


def test_hardware_soak_rejects_non_exact_expected_commit_before_opening_port():
    with pytest.raises(ValueError, match="exact 40-character hexadecimal SHA"):
        run_soak_for_timeout_test(expected_firmware_commit="abc1234")


def test_main_requires_exact_firmware_commit_for_hardware(monkeypatch):
    monkeypatch.setattr(sys, "argv", ["soak_d1l.py", "--port", "COM12"])

    with pytest.raises(SystemExit) as exc_info:
        soak_d1l.main()

    assert exc_info.value.code == 2


def test_main_rejects_malformed_expected_firmware_commit(monkeypatch):
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "soak_d1l.py",
            "--port",
            "COM12",
            "--expected-firmware-commit",
            "abc1234",
        ],
    )

    with pytest.raises(SystemExit) as exc_info:
        soak_d1l.main()

    assert exc_info.value.code == 2


def test_soak_report_collision_prevents_serial_and_peer_access(
    tmp_path,
    monkeypatch,
):
    (tmp_path / "scripts").mkdir()
    monkeypatch.setattr(
        soak_d1l,
        "__file__",
        str(tmp_path / "scripts" / "soak_d1l.py"),
    )
    report_path = tmp_path / "soak-report.json"
    report_path.write_bytes(b"sentinel")
    calls = []

    def unexpected_hardware(**_kwargs):
        calls.append(True)
        raise AssertionError("report collision must fail before hardware")

    monkeypatch.setattr(
        soak_d1l,
        "run_serial_soak",
        unexpected_hardware,
    )
    monkeypatch.setattr(
        soak_d1l.rf_acceptance,
        "run_remote_peer_operation",
        unexpected_hardware,
    )
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "soak_d1l.py",
            "--port",
            "COM12",
            "--expected-firmware-commit",
            "a" * 40,
            "--github-run-id",
            "1",
            "--github-run-attempt",
            "1",
            "--out",
            str(report_path),
        ],
    )

    with pytest.raises(SystemExit) as exc_info:
        soak_d1l.main()

    assert exc_info.value.code == 2
    assert calls == []
    assert report_path.read_bytes() == b"sentinel"


def test_soak_dry_run_writes_the_exclusively_reserved_report(
    tmp_path,
    monkeypatch,
):
    (tmp_path / "scripts").mkdir()
    monkeypatch.setattr(
        soak_d1l,
        "__file__",
        str(tmp_path / "scripts" / "soak_d1l.py"),
    )
    monkeypatch.setattr(
        soak_d1l,
        "stamp_report",
        lambda report, _root: report,
    )
    report_path = tmp_path / "soak-dry-run.json"
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "soak_d1l.py",
            "--dry-run",
            "--out",
            str(report_path),
        ],
    )

    assert soak_d1l.main() == 0
    report = json.loads(report_path.read_text(encoding="ascii"))
    assert report["mode"] == "dry-run"
    assert report["hardware_required"] is False
    assert report_path.read_bytes().endswith(b"\n")


def test_soak_peer_sidecar_collision_prevents_serial_and_ssh(
    tmp_path,
    monkeypatch,
):
    rf = soak_d1l.rf_acceptance
    (tmp_path / "scripts").mkdir()
    monkeypatch.setattr(
        soak_d1l,
        "__file__",
        str(tmp_path / "scripts" / "soak_d1l.py"),
    )
    real_datetime = soak_d1l.datetime

    class FixedDateTime(real_datetime):
        @classmethod
        def now(cls, tz=None):
            return real_datetime(
                2026,
                7,
                23,
                17,
                0,
                0,
                123456,
                tzinfo=tz,
            )

    monkeypatch.setattr(soak_d1l, "datetime", FixedDateTime)
    monkeypatch.setitem(sys.modules, "serial", object())
    monkeypatch.setattr(
        soak_d1l,
        "git_metadata",
        lambda _root: {
            "commit": "a" * 40,
            "dirty": False,
            "dirty_entries": [],
        },
    )
    config = rf.remote_peer_config()
    peer_report = {
        "controlled_peer": {
            "evidence_source": rf.REMOTE_PEER_EVIDENCE_SOURCE,
            "port": None,
            "fingerprint": rf.REMOTE_PEER_FINGERPRINT,
            **config,
        },
        "controlled_peer_adapter": rf.REMOTE_PEER_ADAPTER,
        "d1l_public_key": rf.DEFAULT_D1L_PUBLIC_KEY,
    }
    monkeypatch.setattr(
        soak_d1l,
        "qualified_controlled_peer_receipt",
        lambda **_kwargs: (peer_report, {"path": "rf.json"}),
    )
    external_calls = []

    def unexpected_external(*_args, **_kwargs):
        external_calls.append(True)
        raise AssertionError("collision must fail before external I/O")

    monkeypatch.setattr(
        soak_d1l,
        "open_d1l_serial",
        unexpected_external,
    )
    monkeypatch.setattr(
        rf,
        "capture_remote_peer_status",
        unexpected_external,
    )
    capture_dir = tmp_path / "artifacts" / "soak" / "rf-peer"
    capture_dir.mkdir(parents=True)
    token = (
        "core-soak-aaaaaaaaaaaa-1-1-"
        "20260723T170000123456Z"
    )
    collision = capture_dir / f"{token}_peer_after.json"
    collision.write_bytes(b"sentinel")

    with pytest.raises(ValueError, match="refusing to overwrite"):
        run_soak_for_timeout_test(
            active_dm_fingerprint=rf.REMOTE_PEER_FINGERPRINT,
            active_dm_text="core soak test",
            active_interval_sec=10.0,
            expected_firmware_commit="a" * 40,
            github_run_id="1",
            workflow_run_attempt="1",
            expected_release_profile="core_1_0",
            expected_sd_history_mode="disabled",
            sample_storage=True,
            allow_sd_unavailable=True,
            controlled_peer_receipt=tmp_path / "rf.json",
            peer_capture_dir=capture_dir,
            port_lister=lambda: [windows_target_row()],
            platform_name="nt",
        )

    assert external_calls == []
    assert collision.read_bytes() == b"sentinel"
    assert not (capture_dir / f"{token}_peer_before.json").exists()
