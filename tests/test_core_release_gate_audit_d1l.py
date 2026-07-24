import argparse
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

from scripts import core_release_gate_audit_d1l as audit
from scripts import core_smoke_d1l
from scripts import core_ui_corruption_probe_d1l
from scripts import release_gate_audit_d1l as full_audit
from scripts import soak_d1l as soak_runner


COMMIT = "a" * 40
RUN_ID = "123456789"
RUN_ATTEMPT = "1"


def write_json(path: Path, value: dict) -> Path:
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


def passing_core_smoke() -> dict:
    probes = []
    for probe in core_smoke_d1l.mutation_probe_plan("disabled"):
        probes.append(
            {
                **probe,
                "result": {
                    "ok": False,
                    "cmd": probe["command"],
                    "code": "ESP_ERR_NOT_SUPPORTED",
                    "release_profile": "core_1_0",
                    "feature": probe["feature"],
                },
            }
        )
    status_probes = []
    for probe in core_smoke_d1l.unavailable_status_probe_plan("disabled"):
        status_probes.append(
            {
                **probe,
                "result": {
                    "schema": 1,
                    "ok": True,
                    "cmd": probe["command"],
                    "available": False,
                    "build_commit": COMMIT,
                    "release_profile": "core_1_0",
                    "sd_history_mode": "disabled",
                    "feature": probe["feature"],
                    "mutation_allowed": False,
                    "reason": "unavailable_in_release_profile",
                },
            }
        )
    def health(nonce: int, *, reset_reason: str = "SW") -> dict:
        return {
            "schema": 1,
            "cmd": "health",
            "ok": True,
            "build_commit": COMMIT,
            "release_profile": "core_1_0",
            "sd_history_mode": "disabled",
            "board_ready": True,
            "ui_ready": True,
            "boot_nonce": nonce,
            "reset_reason": reset_reason,
        }

    def settings(name: str) -> dict:
        return {
            "schema": 1,
            "cmd": "settings get",
            "ok": True,
            "node_name": name,
            "path_hash_bytes": 2,
        }

    def reboot() -> dict:
        return {
            "schema": 1,
            "cmd": "reboot",
            "ok": True,
            "rebooting": True,
            "reset_scope": "system",
            "storage_manager_quiesced": True,
            "retained_worker_quiesced": True,
            "rp2040_bridge_quiesced": True,
            "connectivity_prepare": "ESP_OK",
            "retained_flush": "ESP_OK",
            "route_flush": "ESP_OK",
        }

    storage = {
        "schema": 1,
        "cmd": "storage status",
        "ok": True,
        "manager": {
            "running": False,
            "force_nvs": True,
            "state": "READY_NVS",
        },
        "data_enabled": False,
        "data_backend": "nvs",
        "message_store_backend": "nvs",
        "dm_store_backend": "nvs",
        "packet_log_backend": "nvs",
        "route_store_backend": "nvs",
        "map_tile_backend": "unavailable",
        "map_tile_cache_ready": False,
        "map_tile_download_supported": False,
        "sd": {
            "rp2040_bridge_ready": False,
            "rp2040_protocol_supported": False,
            "mounted": False,
            "data_root_ready": False,
            "file_ops": False,
            "atomic_rename": False,
        },
        "stores": {
            "settings": "nvs",
            "identity": "nvs",
            "messages": "nvs",
            "dm": "nvs",
            "packets": "nvs",
            "routes": "nvs",
            "contacts": "nvs",
            "read_state": "nvs",
            "crashlog": "nvs",
            "map_tiles": "unavailable",
            "exports": "serial",
        },
        "fallback": "nvs",
        "retained_nvs": {
            "marker_ready": True,
            "markers_complete": True,
            "anchor_ready": True,
            "sentinel_ready": True,
            "ready": True,
        },
    }
    version = {
        "schema": 1,
        "cmd": "version",
        "ok": True,
        "build_commit": COMMIT,
        "idf": "v5.5.4",
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
    }
    supported_results = []
    for command in core_smoke_d1l.CORE_SMOKE_COMMANDS:
        cmd = audit.expected_command_name(command)
        if cmd == "storage status":
            supported_results.append(storage)
        elif cmd == "crashlog":
            supported_results.append(
                {
                    "schema": 1,
                    "cmd": "crashlog",
                    "ok": True,
                    "entries": [],
                }
            )
        else:
            supported_results.append(
                {"schema": 1, "cmd": cmd, "ok": True}
            )
    original = settings("DeskOS")
    changed = settings("DeskOS-C")
    persistence_steps = [
        {"command": "settings get", "result": original},
        {
            "command": "settings set name DeskOS-C",
            "result": {
                "schema": 1,
                "cmd": "settings set name",
                "ok": True,
            },
        },
        {"command": "settings get", "result": changed},
        {"command": "health", "result": health(10)},
        {"command": "reboot", "result": reboot()},
        {"command": "health", "result": health(11)},
        {"command": "settings get", "result": changed},
        {
            "command": "settings set name DeskOS",
            "result": {
                "schema": 1,
                "cmd": "settings set name",
                "ok": True,
            },
        },
        {"command": "settings get", "result": original},
        {"command": "health", "result": health(11)},
        {"command": "reboot", "result": reboot()},
        {"command": "health", "result": health(12)},
        {"command": "settings get", "result": original},
    ]
    return {
        "kind": "core_smoke",
        "mode": "hardware",
        "ok": True,
        "closure_eligible": True,
        "hardware_required": True,
        "physical_observed": True,
        "port": "COM12",
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "expected_firmware_commit": COMMIT,
        "device_build_commit": COMMIT,
        "github_actions_run": RUN_ID,
        "workflow_run_attempt": RUN_ATTEMPT,
        "firmware_identity_required": True,
        "firmware_identity_ok": True,
        "git": {"commit": COMMIT, "dirty": False, "dirty_entries": []},
        "checks": {
            "exact_candidate": True,
            "esp_idf_v5_5_4": True,
            "core_profile": True,
            "exact_sd_history_mode": True,
            "supported_commands_pass": True,
            "disabled_sd_nvs_authoritative": True,
            "pre_ui_crashlog_clean": True,
            "unavailable_mutations_rejected": True,
            "disabled_sd_status_probes_truthful": True,
            "health_ready": True,
            "persistence_pass": True,
            "no_public_rf": True,
            "no_sd_format": True,
        },
        "supported_commands_executed": list(
            core_smoke_d1l.CORE_SMOKE_COMMANDS
        ),
        "unavailable_mutation_probes": probes,
        "unavailable_status_probes": status_probes,
        "persistence": {
            "schema": 1,
            "kind": "core_settings_persistence",
            "ok": True,
            "mutation_started": True,
            "reboot_count": 2,
            "first_reboot_proven": True,
            "persisted_after_reboot": True,
            "original_restored": True,
            "steps": persistence_steps,
        },
        "public_rf_tx": False,
        "formats_sd": False,
        "results": [version, health(9, reset_reason="POWERON")]
        + supported_results
        + [health(12)],
    }


def test_core_smoke_gate_passes_exact_fixture_and_rejects_dry_or_wrong_sha(
    tmp_path,
):
    path = write_json(tmp_path / "smoke.json", passing_core_smoke())
    assert audit.core_smoke_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok

    failed = passing_core_smoke()
    failed["dry_run"] = True
    write_json(path, failed)
    assert not audit.core_smoke_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok

    failed = passing_core_smoke()
    failed["unavailable_mutation_probes"] = [
        probe
        for probe in failed["unavailable_mutation_probes"]
        if probe["command"] != "packets clear"
    ]
    write_json(path, failed)
    assert not audit.core_smoke_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok

    failed = passing_core_smoke()
    failed["device_build_commit"] = "b" * 40
    write_json(path, failed)
    assert not audit.core_smoke_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok


def passing_core_ui() -> dict:
    def health() -> dict:
        return {
            "schema": 1,
            "ok": True,
            "cmd": "health",
            "build_commit": COMMIT,
            "release_profile": "core_1_0",
            "sd_history_mode": "disabled",
            "board_ready": True,
            "ui_ready": True,
        }

    def status(tab: str = "home") -> dict:
        return {
            "schema": 1,
            "ok": True,
            "cmd": "ui status",
            "build_commit": COMMIT,
            "release_profile": "core_1_0",
            "sd_history_mode": "disabled",
            "active_tab": tab,
            "pending": False,
        }

    def crashlog() -> dict:
        return {
            "schema": 1,
            "ok": True,
            "cmd": "crashlog",
            "entries": [],
        }

    def scroll_result(surface: str) -> dict:
        overflow = {
            "public_messages": 6,
            "nodes": 896,
            "packets": 366,
        }.get(surface, 0)
        bottom = -50 if surface == "settings" else overflow
        return {
            "schema": 1,
            "ok": True,
            "cmd": "ui scroll-probe",
            "surface": surface,
            "tab": core_ui_corruption_probe_d1l.CORE_SCROLL_TABS[
                surface
            ],
            "surface_supported": True,
            "target_found": True,
            "scrollable": True,
            "movement_required": overflow > 0,
            "moved": overflow > 0,
            "before_y": 0,
            "after_y": overflow,
            "scroll_top_before": 0,
            "scroll_bottom_before": bottom,
            "scroll_top_after": overflow,
            "scroll_bottom_after": 0 if overflow > 0 else bottom,
        }

    def compose_result(target: str) -> dict:
        return {
            "schema": 1,
            "ok": True,
            "cmd": "ui compose-probe",
            "target": target.replace("-", "_"),
            "active_tab": (
                core_ui_corruption_probe_d1l.CORE_COMPOSE_TABS[target]
            ),
            "target_supported": True,
            "sheet_visible": True,
            "textarea_visible": True,
            "keyboard_visible": True,
            "onboarding_visible": target == "onboarding",
            "dock_hidden": True,
            "dm_mode": (
                target
                in core_ui_corruption_probe_d1l.CORE_DM_COMPOSE_TARGETS
            ),
            "tx_suppressed": (
                target
                in core_ui_corruption_probe_d1l.CORE_SEND_SUPPRESSED_TARGETS
            ),
            "send_enabled": False,
            "sheet": {"x": 0, "y": 56, "w": 480, "h": 424},
            "textarea": {"x": 16, "y": 58, "w": 448, "h": 78},
            "keyboard": {"x": 16, "y": 158, "w": 448, "h": 258},
            "public_rf_tx": False,
            "formats_sd": False,
        }

    events = []
    for round_number in range(
        1, core_ui_corruption_probe_d1l.RELEASE_MIN_ROUNDS + 1
    ):
        for tab in core_ui_corruption_probe_d1l.CORE_TAB_SEQUENCE:
            events.append(
                {
                    "round": round_number,
                    "kind": "tab",
                    "tab": tab,
                    "request": {"schema": 1, "ok": True},
                    "active": True,
                    "health": health(),
                    "crashlog": crashlog(),
                }
            )
        token = f"CORE-UI-{round_number}"
        events.append(
            {
                "round": round_number,
                "kind": "data_refresh",
                "token": token,
                "data_canary": {"schema": 1, "ok": True},
                "packets_search": {
                    "schema": 1,
                    "ok": True,
                    "entries": [{"note": token}],
                },
                "messages_search": {
                    "schema": 1,
                    "ok": True,
                    "entries": [{"text": token}],
                },
                "health": health(),
                "crashlog": crashlog(),
            }
        )

    unavailable_events = []
    unavailable_plan = (
        core_ui_corruption_probe_d1l.unavailable_ui_probe_plan("disabled")
    )
    for probe in unavailable_plan:
        unavailable_events.append(
            {
                **probe,
                "before": status(),
                "result": {
                    "schema": 1,
                    "ok": False,
                    "cmd": probe["command"],
                    "code": "ESP_ERR_NOT_SUPPORTED",
                    "release_profile": "core_1_0",
                    "feature": probe["feature"],
                },
                "after": status(),
            }
        )

    checks = {
        "exact_candidate": True,
        "esp_idf_v5_5_4": True,
        "core_profile": True,
        "pre_ui_crashlog_clean": True,
        "core_tab_sequence_exact": True,
        "core_scroll_surfaces_exact": True,
        "core_compose_targets_exact": True,
        "tab_switches_settle": True,
        "data_refresh_exercised": True,
        "data_refreshes_pass": True,
        "unavailable_destinations_rejected": True,
        "no_public_rf": True,
        "no_network_tx": True,
        "no_map_network_requests": True,
        "no_formatting": True,
        "uptime_monotonic": True,
        "no_stuck_pending": True,
        "final_active_tab_known": True,
    }
    version = {
        "schema": 1,
        "ok": True,
        "cmd": "version",
        "build_commit": COMMIT,
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "idf": "v5.5.4",
    }
    return {
        "schema": 1,
        "kind": "core_ui_corruption_probe",
        "mode": "hardware",
        "ok": True,
        "closure_eligible": True,
        "hardware_required": True,
        "physical_observed": True,
        "port": "COM12",
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "expected_firmware_commit": COMMIT,
        "device_build_commit": COMMIT,
        "firmware_identity_required": True,
        "firmware_identity_ok": True,
        "github_actions_run": RUN_ID,
        "workflow_run_attempt": RUN_ATTEMPT,
        "git": {"commit": COMMIT, "dirty": False, "dirty_entries": []},
        "rounds": core_ui_corruption_probe_d1l.RELEASE_MIN_ROUNDS,
        "tabs": list(core_ui_corruption_probe_d1l.CORE_TAB_SEQUENCE),
        "scroll_surfaces": list(
            core_ui_corruption_probe_d1l.CORE_SCROLL_SURFACES
        ),
        "scroll_events": [
            {
                "surface": surface,
                "command": f"ui scroll-probe {surface}",
                "result": scroll_result(surface),
            }
            for surface in core_ui_corruption_probe_d1l.CORE_SCROLL_SURFACES
        ],
        "compose_targets": list(
            core_ui_corruption_probe_d1l.CORE_COMPOSE_TARGETS
        ),
        "compose_events": [
            {
                "target": target,
                "command": f"ui compose-probe {target}",
                "result": compose_result(target),
            }
            for target in core_ui_corruption_probe_d1l.CORE_COMPOSE_TARGETS
        ],
        "unavailable_ui_probes": unavailable_plan,
        "unavailable_events": unavailable_events,
        "skip_data_canary": False,
        "data_refresh_events": (
            core_ui_corruption_probe_d1l.RELEASE_MIN_ROUNDS
        ),
        "clear_crashlog_before_start": False,
        "checks": checks,
        "setup_events": [
            {"command": "version", "result": version},
            {"command": "health", "result": health()},
            {"command": "ui status", "result": status()},
            {"command": "crashlog", "result": crashlog()},
        ],
        "final_health": health(),
        "public_rf_tx": False,
        "network_tx": False,
        "map_network_requests": False,
        "formats_sd": False,
        "events": events,
    }


def test_core_ui_gate_recomputes_unavailable_deep_link_events(tmp_path):
    path = write_json(tmp_path / "core-ui.json", passing_core_ui())
    assert audit.core_ui_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok

    missing = passing_core_ui()
    missing["unavailable_events"].pop()
    write_json(path, missing)
    assert not audit.core_ui_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok

    changed_tab = passing_core_ui()
    changed_tab["unavailable_events"][0]["after"]["active_tab"] = "messages"
    write_json(path, changed_tab)
    assert not audit.core_ui_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok

    wrong_feature = passing_core_ui()
    wrong_feature["unavailable_events"][1]["result"]["feature"] = "map"
    write_json(path, wrong_feature)
    assert not audit.core_ui_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok


def test_core_ui_gate_recomputes_raw_scroll_and_compose_results(tmp_path):
    path = write_json(tmp_path / "core-ui.json", passing_core_ui())
    assert audit.core_ui_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok

    forged_scroll = passing_core_ui()
    forged_scroll["scroll_events"][0]["result"][
        "movement_required"
    ] = True
    write_json(path, forged_scroll)
    assert not audit.core_ui_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok

    forged_scroll = passing_core_ui()
    public_result = forged_scroll["scroll_events"][1]["result"]
    public_result["after_y"] = 0
    public_result["scroll_top_after"] = 0
    public_result["moved"] = False
    write_json(path, forged_scroll)
    assert not audit.core_ui_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok

    forged_compose = passing_core_ui()
    forged_compose["compose_events"][2]["result"][
        "tx_suppressed"
    ] = False
    write_json(path, forged_compose)
    assert not audit.core_ui_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok

    forged_compose = passing_core_ui()
    forged_compose["compose_events"][2]["result"][
        "send_enabled"
    ] = True
    write_json(path, forged_compose)
    assert not audit.core_ui_gate(
        path, tmp_path, COMMIT, "disabled", RUN_ID, RUN_ATTEMPT
    ).ok


def passing_soak(*, active: bool) -> dict:
    duration = 3600 if active else 1800
    interval = 300
    samples = []
    for index in range(duration // interval + 1):
        tx_packets = index * (1 if active else 0)
        rx_packets = index if active else 0
        storage = {
            "schema": 1,
            "cmd": "storage status",
            "ok": True,
            "manager": {
                "running": False,
                "force_nvs": True,
                "state": "READY_NVS",
            },
            "data_enabled": False,
            "data_backend": "nvs",
            "message_store_backend": "nvs",
            "dm_store_backend": "nvs",
            "packet_log_backend": "nvs",
            "route_store_backend": "nvs",
            "map_tile_backend": "unavailable",
            "map_tile_cache_ready": False,
            "map_tile_download_supported": False,
            "sd": {
                "state": "disabled",
                "filesystem": "unavailable",
                "interface": "disabled",
                "rp2040_bridge_ready": False,
                "rp2040_protocol_supported": False,
                "mounted": False,
                "data_root_ready": False,
                "file_ops": False,
                "atomic_rename": False,
                "file_line_max": 0,
                "file_chunk_max": 0,
                "path_max": 0,
                "status_stale": False,
                "presence_stale": False,
                "refresh_failures": 0,
            },
            "stores": {
                "settings": "nvs",
                "identity": "nvs",
                "messages": "nvs",
                "dm": "nvs",
                "packets": "nvs",
                "routes": "nvs",
                "contacts": "nvs",
                "read_state": "nvs",
                "crashlog": "nvs",
                "map_tiles": "unavailable",
                "exports": "serial",
            },
            "fallback": "nvs",
            "retained_nvs": {
                "marker_ready": True,
                "markers_complete": True,
                "anchor_ready": True,
                "sentinel_ready": True,
                "ready": True,
            },
        }
        samples.append(
            {
                "label": "start" if index == 0 else f"sample-{index}",
                "elapsed_sec": index * interval,
                "aborted_after_timeout": None,
                "results": [
                    {
                        "schema": 1,
                        "cmd": "health",
                        "ok": True,
                        "build_commit": COMMIT,
                        "release_profile": "core_1_0",
                        "sd_history_mode": "disabled",
                        "board_ready": True,
                        "ui_ready": True,
                        "boot_nonce": 99,
                        "uptime_ms": 1000 + index * interval * 1000,
                        "heap_free": 500000 - index * 100,
                        "psram_free": 1000000 - index * 100,
                        "heap_min_free": 450000,
                        "psram_min_free": 900000,
                        "current_task_stack_free_words": 4096,
                        "ui_task_stack_free_words": 4096,
                        "retained_task_stack_free_bytes": 8192,
                        "lvgl_used_pct": 20,
                    },
                    {
                        "schema": 1,
                        "cmd": "mesh status",
                        "ok": True,
                        "state": "ready",
                        "identity_ready": True,
                        "radio_ready": True,
                        "rx_packets": rx_packets,
                        "tx_packets": tx_packets,
                        "runtime": {
                            "queue_drops": 0,
                            "callback_event_drops": 0,
                            "command_queue_saturation": 0,
                            "priority_queue_saturation": 0,
                            "command_queue_depth": 0,
                            "priority_queue_depth": 0,
                            "event_queue_depth": 0,
                        },
                    },
                    {
                        "schema": 1,
                        "cmd": "signal",
                        "ok": True,
                        "sample_count": index + 1,
                    },
                    {
                        "schema": 1,
                        "cmd": "messages unread",
                        "ok": True,
                    },
                    {
                        "schema": 1,
                        "cmd": "packets",
                        "ok": True,
                        "total_written": tx_packets + rx_packets,
                    },
                    {
                        "schema": 1,
                        "cmd": "crashlog",
                        "ok": True,
                        "entries": [],
                        "count": 0,
                        "total_written": 0,
                    },
                    storage,
                ],
            }
        )
    active_events = (
        [
            {
                "elapsed_sec": 1 + index * 600,
                "command": (
                    "mesh send dm 0123456789ABCDEF core_soak"
                ),
                "fingerprint": "0123456789ABCDEF",
                "text": "core_soak",
                "result": {
                    "schema": 1,
                    "cmd": "mesh send dm",
                    "ok": True,
                },
            }
            for index in range(6)
        ]
        if active
        else []
    )
    summary = soak_runner.summarize_soak(
        samples=samples,
        active_events=active_events,
        require_rx_delta=active,
        min_rx_delta=1,
        min_tx_delta=6 if active else 0,
        sample_storage=True,
        sd_file_canary=False,
        allow_sd_unavailable=True,
    )
    return {
        "schema": 1,
        "mode": "hardware",
        "ok": True,
        "closure_eligible": True,
        "physical_observed": True,
        "port": "COM12",
        "expected_firmware_commit": COMMIT,
        "device_build_commit": COMMIT,
        "github_actions_run": RUN_ID,
        "workflow_run_attempt": RUN_ATTEMPT,
        "firmware_identity_required": True,
        "firmware_identity_ok": True,
        "git": {"commit": COMMIT, "dirty": False, "dirty_entries": []},
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "device_idf_version": "v5.5.4",
        "device_release_profile": "core_1_0",
        "device_sd_history_mode": "disabled",
        "started_at": "2026-07-18T00:00:00Z",
        "ended_at": (
            "2026-07-18T01:00:00Z"
            if active
            else "2026-07-18T00:30:00Z"
        ),
        "duration_sec": duration,
        "sample_interval_sec": interval,
        "preflight_commands": ["version"],
        "preflight_failure": None,
        "version_preflight": {
            "schema": 1,
            "cmd": "version",
            "ok": True,
            "build_commit": COMMIT,
            "idf": "v5.5.4",
            "release_profile": "core_1_0",
            "sd_history_mode": "disabled",
        },
        "setup_events": [
            {
                "elapsed_sec": 0.0,
                "cmd": "version",
                "result": {
                    "schema": 1,
                    "cmd": "version",
                    "ok": True,
                    "build_commit": COMMIT,
                    "idf": "v5.5.4",
                    "release_profile": "core_1_0",
                    "sd_history_mode": "disabled",
                },
            }
        ],
        "commands": list(soak_runner.SOAK_COMMANDS)
        + [soak_runner.STORAGE_STATUS_COMMAND],
        "sample_storage": True,
        "sd_file_canary": False,
        "active_dm_fingerprint": "0123456789ABCDEF" if active else None,
        "active_dm_text": "core_soak" if active else None,
        "active_command": (
            "mesh send dm 0123456789ABCDEF core_soak" if active else None
        ),
        "dm_rf_tx": active,
        "active_events": active_events,
        "allow_sd_unavailable": True,
        "require_rx_delta": active,
        "min_rx_delta": 1,
        "min_tx_delta": 6 if active else 0,
        "clear_crashlog_before_start": False,
        "public_rf_tx": False,
        "formats_sd": False,
        "aborted_after_timeout": None,
        "samples": samples,
        "summary": summary,
    }


def test_core_soak_requires_full_duration_exact_identity_and_nvs():
    active = passing_soak(active=True)
    idle = passing_soak(active=False)

    assert audit.soak_artifact_ok(
        active,
        commit=COMMIT,
        sd_history_mode="disabled",
        active=True,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    assert audit.soak_artifact_ok(
        idle,
        commit=COMMIT,
        sd_history_mode="disabled",
        active=False,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    active["duration_sec"] = 3599
    assert not audit.soak_artifact_ok(
        active,
        commit=COMMIT,
        sd_history_mode="disabled",
        active=True,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    active = passing_soak(active=True)
    active["summary"]["storage_data_backends"] = ["sd"]
    assert not audit.soak_artifact_ok(
        active,
        commit=COMMIT,
        sd_history_mode="disabled",
        active=True,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    active = passing_soak(active=True)
    active["samples"][-1]["results"][1]["runtime"][
        "command_queue_saturation"
    ] = 1
    assert not audit.soak_artifact_ok(
        active,
        commit=COMMIT,
        sd_history_mode="disabled",
        active=True,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )


def test_active_soak_peer_flow_recomputes_raw_listener_sidecars(
    tmp_path: Path,
):
    peer_public_key = "0123456789abcdef" + "22" * 24
    d1l_public_key = "abcdef012345" + "33" * 26
    fingerprint = peer_public_key[:16].upper()

    def status(
        *,
        rx: int,
        tx: int,
        reply: int,
        ack_miss: int,
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
                "rx_dm_total": rx,
                "tx_dm_total": tx,
                "local_fast_reply_total": reply,
                "tx_dm_ack_miss_total": ack_miss,
            },
        }

    before = status(
        rx=10,
        tx=20,
        reply=30,
        ack_miss=1,
        rx_at="before-rx",
        tx_at="before-tx",
    )
    after = status(
        rx=16,
        tx=26,
        reply=36,
        ack_miss=1,
        rx_at="after-rx",
        tx_at="after-tx",
    )
    before_path = write_json(tmp_path / "before.json", before)
    after_path = write_json(tmp_path / "after.json", after)
    source_path = str(
        audit.rf_acceptance.RADIO_LISTENER_STATUS_PATH.resolve()
    )

    def row(path: Path) -> dict:
        return {
            "path": path.relative_to(tmp_path).as_posix(),
            "size": path.stat().st_size,
            "sha256": audit.sha256_file(path),
            "source_path": source_path,
        }

    data = passing_soak(active=True)
    data.update(
        {
            "active_dm_text": "core_soak_test",
            "active_interval_sec": 600,
            "controlled_peer_before": (
                audit.rf_acceptance.status_snapshot(before)
            ),
            "controlled_peer_after": (
                audit.rf_acceptance.status_snapshot(after)
            ),
            "controlled_peer_before_receipt": row(before_path),
            "controlled_peer_after_receipt": row(after_path),
            "controlled_peer_counter_deltas": {
                "rx_dm_total": 6,
                "tx_dm_total": 6,
                "local_fast_reply_total": 6,
                "tx_dm_ack_miss_total": 0,
            },
            "controlled_peer_successful_send_count": 6,
            "controlled_peer_expected_send_count": 6,
            "controlled_peer_flow_ok": True,
        }
    )
    rf = {
        "target_fingerprint": fingerprint,
        "d1l_public_key": d1l_public_key,
        "controlled_peer_adapter": "openclaw_radio_listener",
        "controlled_peer": {
            "port": "COM15",
            "fingerprint": fingerprint,
            "public_key": peer_public_key,
            "status_path": source_path,
        },
    }

    ok, details = audit.active_soak_peer_flow_ok(data, rf, tmp_path)
    assert ok is True
    assert details["successful_send_count"] == 6
    assert details["expected_send_count"] == 6

    data["controlled_peer_counter_deltas"]["rx_dm_total"] = 5
    assert audit.active_soak_peer_flow_ok(data, rf, tmp_path)[0] is False


def test_reboot_gate_delegates_to_strict_r11_validator(
    tmp_path: Path, monkeypatch
):
    matrix_path = write_json(tmp_path / "matrix.json", {})
    calls = []

    def strict_validator(path, **kwargs):
        calls.append((path, kwargs))
        return (
            True,
            [],
            {
                "software_cycle_count": 5,
                "cold_cycle_count": 3,
                "claim": (
                    "same_exact_candidate_non_erasing_reinstall"
                ),
                "github_actions_run": RUN_ID,
                "workflow_run_attempt": RUN_ATTEMPT,
            },
        )

    monkeypatch.setattr(
        audit,
        "validate_core_reboot_persistence_receipt",
        strict_validator,
    )
    gate = audit.reboot_persistence_gate(
        matrix_path,
        tmp_path,
        COMMIT,
        "disabled",
        RUN_ID,
        RUN_ATTEMPT,
    )

    assert gate.ok is True
    assert calls == [
        (
            matrix_path,
            {
                "root": tmp_path,
                "expected_commit": COMMIT,
                "expected_run_id": RUN_ID,
                "expected_run_attempt": RUN_ATTEMPT,
            },
        )
    ]
    monkeypatch.setattr(
        audit, "validate_core_reboot_persistence_receipt", None
    )
    assert not audit.reboot_persistence_gate(
        matrix_path,
        tmp_path,
        COMMIT,
        "disabled",
        RUN_ID,
        RUN_ATTEMPT,
    ).ok


def protocol_migration_receipt() -> dict:
    return {
        "mode": "hardware",
        "commit": COMMIT,
        "github_actions_run": RUN_ID,
        "workflow_run_attempt": RUN_ATTEMPT,
    }


def test_protocol_migration_gate_delegates_to_strict_validator(
    tmp_path: Path, monkeypatch
):
    receipt = protocol_migration_receipt()
    receipt_path = write_json(
        tmp_path / "time_protocol_migration.json",
        receipt,
    )
    calls = []

    def strict_validator(value, *, root):
        calls.append((value, root))
        return True, []

    monkeypatch.setattr(
        audit.time_protocol_migration,
        "validate_receipt",
        strict_validator,
    )
    gate = audit.protocol_migration_gate(
        receipt_path,
        tmp_path,
        COMMIT,
        RUN_ID,
        RUN_ATTEMPT,
    )

    assert gate.ok is True
    assert calls == [(receipt, tmp_path)]
    assert gate.details["reasons"] == []
    assert gate.details["validation_errors"] == []
    assert gate.details["receipt_sha256"] == audit.sha256_file(receipt_path)


@pytest.mark.parametrize(
    ("field", "value"),
    (
        ("commit", "b" * 40),
        ("github_actions_run", "987654321"),
        ("workflow_run_attempt", "2"),
    ),
)
def test_protocol_migration_gate_rejects_external_binding_mismatch(
    tmp_path: Path, monkeypatch, field: str, value: str
):
    receipt = protocol_migration_receipt()
    receipt[field] = value
    receipt_path = write_json(
        tmp_path / "time_protocol_migration.json",
        receipt,
    )
    monkeypatch.setattr(
        audit.time_protocol_migration,
        "validate_receipt",
        lambda *_args, **_kwargs: (True, []),
    )

    gate = audit.protocol_migration_gate(
        receipt_path,
        tmp_path,
        COMMIT,
        RUN_ID,
        RUN_ATTEMPT,
    )

    assert gate.ok is False
    assert (
        "protocol_migration_exact_candidate_binding_failed"
        in gate.details["reasons"]
    )


def test_protocol_migration_gate_fails_closed_for_missing_invalid_or_rejected(
    tmp_path: Path, monkeypatch
):
    missing = audit.protocol_migration_gate(
        None,
        tmp_path,
        COMMIT,
        RUN_ID,
        RUN_ATTEMPT,
    )
    assert missing.ok is False
    assert "protocol_migration_receipt_missing" in missing.details["reasons"]

    invalid_path = tmp_path / "time_protocol_migration_invalid.json"
    invalid_path.write_text("{", encoding="utf-8")
    invalid = audit.protocol_migration_gate(
        invalid_path,
        tmp_path,
        COMMIT,
        RUN_ID,
        RUN_ATTEMPT,
    )
    assert invalid.ok is False
    assert (
        "protocol_migration_receipt_invalid_json"
        in invalid.details["reasons"]
    )

    receipt_path = write_json(
        tmp_path / "time_protocol_migration.json",
        protocol_migration_receipt(),
    )
    monkeypatch.setattr(
        audit.time_protocol_migration,
        "validate_receipt",
        lambda *_args, **_kwargs: (False, ["tampered"]),
    )
    rejected = audit.protocol_migration_gate(
        receipt_path,
        tmp_path,
        COMMIT,
        RUN_ID,
        RUN_ATTEMPT,
    )
    assert rejected.ok is False
    assert rejected.details["validation_errors"] == ["tampered"]

    def explode(*_args, **_kwargs):
        raise RuntimeError("validator failed")

    monkeypatch.setattr(
        audit.time_protocol_migration,
        "validate_receipt",
        explode,
    )
    failed = audit.protocol_migration_gate(
        receipt_path,
        tmp_path,
        COMMIT,
        RUN_ID,
        RUN_ATTEMPT,
    )
    assert failed.ok is False
    assert failed.details["validation_errors"] == [
        "strict_protocol_migration_validator_failed:RuntimeError"
    ]


def write_remote_rf_receipt(
    tmp_path: Path, *, local: bool = False
) -> Path:
    rf = audit.rf_acceptance
    observed = datetime(2026, 7, 23, 15, 0, tzinfo=timezone.utc)
    config = (
        rf.local_peer_config()
        if local
        else rf.remote_peer_config(
            ssh_host="neonx@192.168.0.24"
        )
    )

    def status(*, after: bool) -> dict:
        return {
            "service": "openclaw-radio-listener",
            "run_id": "pi5-peer-run",
            "status_written_at": (
                observed - timedelta(seconds=1 if after else 5)
            ).isoformat(),
            "serial": {
                "port": rf.REMOTE_PEER_DEVICE,
                "mesh_connected": True,
                "self_prefix": rf.REMOTE_PEER_PUBLIC_KEY[:12],
                "public_key": rf.REMOTE_PEER_PUBLIC_KEY,
            },
            "mesh": {
                "last_fetch_ok_at": (
                    observed - timedelta(seconds=1 if after else 4)
                ).isoformat(),
                "last_rx_at": "after-rx" if after else "before-rx",
                "last_rx_kind": "dm",
                "last_rx_sender": rf.DEFAULT_D1L_PUBLIC_KEY[:12],
                "last_tx_at": "after-tx" if after else "before-tx",
                "last_tx_kind": "control_dm",
            },
            "startup_self_test": {"enabled": True, "ok": True},
            "counters": {
                "rx_dm_total": 11 if after else 10,
                "tx_dm_total": 21 if after else 20,
                "local_fast_reply_total": 4,
                "tx_dm_ack_miss_total": 1,
            },
        }

    before = status(after=False)
    after = status(after=True)
    sidecar_dir = tmp_path / "artifacts" / "hardware" / "com12" / "rf-peer"
    sidecar_dir.mkdir(parents=True)
    before_path = write_json(sidecar_dir / "before.json", before)
    after_path = write_json(sidecar_dir / "after.json", after)

    def status_row(path: Path, value: dict) -> dict:
        digest = audit.sha256_file(path)
        status_written = rf.parse_aware_timestamp(
            value["status_written_at"]
        )
        assert status_written is not None
        return {
            "path": path.relative_to(tmp_path).as_posix(),
            "size": path.stat().st_size,
            "sha256": digest,
            "source_path": rf.REMOTE_PEER_STATUS_PATH,
            "source_host": (
                config["hostname"] if local else config["ssh_host"]
            ),
            "source_hostname": config["hostname"],
            "transport": (
                rf.LOCAL_PEER_STATUS_TRANSPORT
                if local
                else "ssh"
            ),
            "captured_at": observed.isoformat(),
            **(
                {
                    "source_mtime_ns": int(
                        status_written.timestamp() * 1_000_000_000
                    ),
                    "source_sha256": digest,
                }
                if local
                else {
                    "remote_mtime_ns": 1,
                    "remote_sha256": digest,
                }
            ),
        }

    before_row = status_row(before_path, before)
    after_row = status_row(after_path, after)
    status_validator = (
        rf.validate_local_peer_status
        if local
        else rf.validate_remote_peer_status
    )
    if local:
        before_validation = status_validator(
            before,
            config,
            observed_at=observed,
            source_mtime_ns=before_row["source_mtime_ns"],
        )
        after_validation = status_validator(
            after,
            config,
            observed_at=observed,
            source_mtime_ns=after_row["source_mtime_ns"],
        )
    else:
        before_validation = status_validator(
            before, config, observed_at=observed
        )
        after_validation = status_validator(
            after, config, observed_at=observed
        )
    token = "rf_remote"
    inbound_token = f"{token}_in"
    request, request_raw = rf.remote_control_request(
        rf.DEFAULT_D1L_PUBLIC_KEY, inbound_token
    )
    response = {
        "id": request["id"],
        "op": "radio.send_dm",
        "ok": True,
        "cached": False,
        "duration_ms": 123,
        "result": {
            "target": rf.DEFAULT_D1L_PUBLIC_KEY[:12],
            "name": "D1L",
            "utf8_bytes": len(inbound_token.encode("utf-8")),
            "delivery": {
                "event": "CONTACT_MSG_RECV",
                "payload": {"ack": True},
                "acknowledged": True,
            },
        },
        "error": None,
    }
    response_raw = (
        json.dumps(
            response, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
        + b"\n"
    )
    request_path = sidecar_dir / "request.jsonl"
    response_path = sidecar_dir / "response.jsonl"
    request_path.write_bytes(request_raw)
    response_path.write_bytes(response_raw)

    def control_row(path: Path, transport: str) -> dict:
        return {
            "path": path.relative_to(tmp_path).as_posix(),
            "size": path.stat().st_size,
            "sha256": audit.sha256_file(path),
            "source_path": rf.REMOTE_PEER_CONTROL_SOCKET,
            "source_host": (
                config["hostname"] if local else config["ssh_host"]
            ),
            "source_hostname": config["hostname"],
            "transport": transport,
            "captured_at": observed.isoformat(),
            **(
                {
                    "source_peer_pid": 7896,
                    "source_peer_uid": (
                        rf.LOCAL_PEER_CONTROL_UID
                    ),
                    "source_peer_gid": (
                        rf.LOCAL_PEER_CONTROL_GID
                    ),
                }
                if local
                else {}
            ),
        }

    control_validation = rf.validate_remote_control_exchange(
        request_raw,
        response_raw,
        d1l_public_key=rf.DEFAULT_D1L_PUBLIC_KEY,
        token=inbound_token,
    )
    control = {
        "op": "radio.send_dm",
        "socket_path": rf.REMOTE_PEER_CONTROL_SOCKET,
        "request_id": request["id"],
        "request": request,
        "response": response,
        "request_receipt": control_row(
            request_path,
            rf.LOCAL_PEER_CONTROL_REQUEST_TRANSPORT
            if local
            else "ssh-unix-socket-request",
        ),
        "response_receipt": control_row(
            response_path,
            rf.LOCAL_PEER_CONTROL_RESPONSE_TRANSPORT
            if local
            else "ssh-unix-socket-response",
        ),
        "request_sha256": control_validation["request_sha256"],
        "response_sha256": control_validation["response_sha256"],
        "validation": control_validation,
    }
    fingerprint = rf.REMOTE_PEER_FINGERPRINT
    import_command = rf.contact_import_command(
        rf.REMOTE_PEER_PUBLIC_KEY
    )
    contact = {
        "fingerprint": fingerprint,
        "public_key": rf.REMOTE_PEER_PUBLIC_KEY,
        "alias": rf.RADIO_LISTENER_CONTACT_NAME,
        "type": "chat",
        "verification_source": "uri_import",
        "canonical": True,
        "can_dm": True,
        "can_admin": False,
    }
    import_result = {
        "ok": True,
        "cmd": "contacts import",
        "persisted": True,
        "result": "created",
        **contact,
    }
    ack_hash = 1234567890
    baseline_messages = {
        "ok": True,
        "fingerprint": fingerprint,
        "entries": [{"seq": 1, "direction": "tx", "text": "older"}],
    }
    final_messages = {
        "ok": True,
        "fingerprint": fingerprint,
        "entries": [
            *baseline_messages["entries"],
            {
                "seq": 2,
                "fingerprint": fingerprint,
                "direction": "tx",
                "text": "core acceptance test rf_remote_out",
                "acked": True,
                "delivered": True,
                "ack_hash": ack_hash,
                "ack_response": {
                    "identity_valid": False,
                    "state": "legacy_unverified",
                    "dispatch_count": 0,
                    "last_kind": "none",
                    "last_error": "ESP_OK",
                },
            },
            {
                "seq": 3,
                "fingerprint": fingerprint,
                "direction": "rx",
                "text": inbound_token,
                "ack_response": {
                    "identity_valid": True,
                    "state": "sent",
                    "dispatch_count": 1,
                    "last_kind": "direct_ack",
                    "last_error": "ESP_OK",
                },
            },
        ],
    }
    baseline_packets = {
        "ok": True,
        "entries": [{"seq": 10, "kind": "other", "direction": "rx"}],
    }
    final_packets = {
        "ok": True,
        "entries": [
            *baseline_packets["entries"],
            {
                "seq": 11,
                "direction": "rx",
                "kind": "dm_ack",
                "note": (
                    f"ack {ack_hash} {rf.RADIO_LISTENER_CONTACT_NAME}"
                ),
                "rssi_dbm": -70,
                "snr_tenths": 80,
                "path_hash_bytes": 1,
                "path_hops": 0,
                "payload_len": 12,
            },
        ],
    }
    baseline_route = {
        "ok": True,
        "fingerprint": fingerprint,
        "entries": [
            {
                "seq": 20,
                "target": fingerprint,
                "kind": "other",
                "direction": "rx",
                "route": "direct",
            }
        ],
    }
    final_route = {
        "ok": True,
        "fingerprint": fingerprint,
        "entries": [
            {
                "seq": 21,
                "target": fingerprint,
                "kind": "dm_ack",
                "direction": "rx",
                "route": "direct",
                "last_rssi_dbm": -70,
                "last_snr_tenths": 80,
                "path_hash_bytes": 1,
                "path_hops": 0,
                "payload_len": 12,
            }
        ],
    }
    version = {
        "ok": True,
        "cmd": "version",
        "build_commit": COMMIT,
        "idf": "v5.5.4",
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "time": {
            "protocol_tx_ready": True,
            "protocol_tx_block": "none",
        },
    }
    steps = [
        {"command": "version", "result": version},
        {
            "command": "identity status",
            "result": {
                "ok": True,
                "cmd": "identity status",
                "public_key": rf.DEFAULT_D1L_PUBLIC_KEY,
                "fingerprint": rf.DEFAULT_D1L_PUBLIC_KEY[
                    :16
                ].upper(),
            },
        },
        {"command": "contacts", "result": {"ok": True, "entries": []}},
        {"command": import_command, "result": import_result},
        {
            "command": "contacts",
            "result": {"ok": True, "entries": [contact]},
        },
        {
            "command": f"messages dm {fingerprint}",
            "result": baseline_messages,
        },
        {"command": "packets", "result": baseline_packets},
        {
            "command": f"routes trace {fingerprint}",
            "result": baseline_route,
        },
        {
            "command": (
                f"mesh send dm {fingerprint} "
                "core acceptance test rf_remote_out"
            ),
            "result": {"ok": True},
        },
        {
            "command": "packets search rf_remote_out",
            "result": {"ok": True, "entries": [{"note": "rf_remote_out"}]},
        },
        {
            "command": f"messages dm {fingerprint}",
            "result": final_messages,
        },
        {"command": "packets", "result": final_packets},
        {
            "command": f"routes trace {fingerprint}",
            "result": final_route,
        },
        {
            "command": f"messages dm {fingerprint}",
            "result": final_messages,
        },
        {"command": "packets", "result": final_packets},
        {
            "command": f"routes trace {fingerprint}",
            "result": final_route,
        },
        {
            "command": "health",
            "result": {
                "ok": True,
                "cmd": "health",
                "build_commit": COMMIT,
                "release_profile": "core_1_0",
                "sd_history_mode": "disabled",
                "board_ready": True,
                "ui_ready": True,
            },
        },
    ]
    report = rf.build_report(
        port="COM12",
        baud=115200,
        peer_status_path=None,
        peer_port=None,
        fingerprint=fingerprint,
        public_key=rf.DEFAULT_D1L_PUBLIC_KEY,
        token=token,
        send_outbound=True,
        steps=steps,
        peer_before=before,
        peer_after=after,
        inbound_seen_at=observed.isoformat(),
        expected_commit=COMMIT,
        peer_before_receipt=before_row,
        peer_after_receipt=after_row,
        github_run_id=RUN_ID,
        workflow_run_attempt=RUN_ATTEMPT,
        remote_peer=None if local else config,
        local_peer=config if local else None,
        remote_before_validation=before_validation,
        remote_after_validation=after_validation,
        remote_control=control,
    )
    report["git"] = {
        "commit": COMMIT,
        "dirty": False,
        "dirty_entries": [],
    }
    path = tmp_path / "artifacts" / "hardware" / "com12" / "rf.json"
    return write_json(path, report)


def test_remote_rf_gate_recomputes_raw_status_and_control_sidecars(
    tmp_path: Path,
):
    receipt = write_remote_rf_receipt(tmp_path)

    gate = audit.rf_gate(
        receipt,
        tmp_path,
        COMMIT,
        "disabled",
        RUN_ID,
        RUN_ATTEMPT,
    )

    assert gate.ok is True
    assert gate.details["raw_status_ok"] is True
    assert gate.details["control_exchange_ok"] is True
    assert gate.details["peer_binding_ok"] is True
    assert len(gate.evidence) == 5

    report = json.loads(receipt.read_text(encoding="utf-8"))
    response_path = (
        tmp_path
        / report["controlled_peer_control"]["response_receipt"]["path"]
    )
    response = json.loads(response_path.read_text(encoding="utf-8"))
    response["cached"] = True
    response_path.write_text(
        json.dumps(response, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    assert (
        audit.rf_gate(
            receipt,
            tmp_path,
            COMMIT,
            "disabled",
            RUN_ID,
            RUN_ATTEMPT,
        ).ok
        is False
    )


def test_local_rf_gate_recomputes_raw_status_and_control_sidecars(
    tmp_path: Path,
):
    receipt = write_remote_rf_receipt(tmp_path, local=True)

    gate = audit.rf_gate(
        receipt,
        tmp_path,
        COMMIT,
        "disabled",
        RUN_ID,
        RUN_ATTEMPT,
    )

    assert gate.ok is True
    assert gate.details["raw_status_ok"] is True
    assert gate.details["control_exchange_ok"] is True
    assert gate.details["source_rows_ok"] is True
    assert gate.details["peer_binding_ok"] is True
    report = json.loads(receipt.read_text(encoding="utf-8"))
    assert report["controlled_peer"]["access_mode"] == "local"
    assert "ssh_host" not in report["controlled_peer"]
    assert (
        report["controlled_peer_before_receipt"]["transport"]
        == audit.rf_acceptance.LOCAL_PEER_STATUS_TRANSPORT
    )


@pytest.mark.parametrize(
    "source_mtime_ns",
    [
        0,
        10**100,
        int(
            datetime(
                2026, 7, 23, 15, 0, 31, tzinfo=timezone.utc
            ).timestamp()
            * 1_000_000_000
        ),
        int(
            datetime(
                2026, 7, 23, 14, 59, 0, tzinfo=timezone.utc
            ).timestamp()
            * 1_000_000_000
        ),
    ],
)
def test_local_core_rf_gate_rejects_invalid_source_mtime(
    tmp_path: Path,
    source_mtime_ns: int,
):
    receipt = write_remote_rf_receipt(tmp_path, local=True)
    report = json.loads(receipt.read_text(encoding="utf-8"))
    report["controlled_peer_before_receipt"][
        "source_mtime_ns"
    ] = source_mtime_ns
    receipt.write_text(json.dumps(report), encoding="utf-8")

    gate = audit.rf_gate(
        receipt,
        tmp_path,
        COMMIT,
        "disabled",
        RUN_ID,
        RUN_ATTEMPT,
    )

    assert gate.ok is False
    assert gate.details["raw_status_ok"] is False
    assert (
        gate.details["derived_checks"][
            "controlled_peer_observed"
        ]
        is False
    )


@pytest.mark.parametrize(
    ("section", "field", "value"),
    [
        ("controlled_peer", "hostname", "forged-pi"),
        ("controlled_peer", "status_path", "/tmp/status.json"),
        ("controlled_peer", "control_socket", "/tmp/control.sock"),
        ("controlled_peer", "access_mode", "ssh"),
        ("controlled_peer", "ssh_host", "neonx@192.168.0.24"),
        (
            "controlled_peer_before_receipt",
            "source_path",
            "/tmp/status.json",
        ),
        (
            "controlled_peer_before_receipt",
            "source_host",
            "forged-pi",
        ),
        (
            "controlled_peer_before_receipt",
            "source_hostname",
            "forged-pi",
        ),
        (
            "controlled_peer_before_receipt",
            "transport",
            "ssh",
        ),
        (
            "controlled_peer_control.request_receipt",
            "source_path",
            "/tmp/control.sock",
        ),
        (
            "controlled_peer_control.request_receipt",
            "source_hostname",
            "forged-pi",
        ),
        (
            "controlled_peer_control.request_receipt",
            "transport",
            "ssh-unix-socket-request",
        ),
        (
            "controlled_peer_control.request_receipt",
            "source_peer_uid",
            1000,
        ),
    ],
)
def test_local_core_rf_gate_rejects_forged_source_binding(
    tmp_path: Path,
    section: str,
    field: str,
    value,
):
    receipt = write_remote_rf_receipt(tmp_path, local=True)
    report = json.loads(receipt.read_text(encoding="utf-8"))
    target = report
    for part in section.split("."):
        target = target[part]
    target[field] = value
    receipt.write_text(json.dumps(report), encoding="utf-8")

    gate = audit.rf_gate(
        receipt,
        tmp_path,
        COMMIT,
        "disabled",
        RUN_ID,
        RUN_ATTEMPT,
    )
    assert gate.ok is False
    assert (
        gate.details["source_rows_ok"] is False
        or gate.details["peer_binding_ok"] is False
    )

@pytest.mark.parametrize(
    ("ready", "block"),
    [
        (False, "none"),
        (True, "legacy_protocol_lower_bound_unconfirmed"),
    ],
)
def test_remote_rf_gate_rejects_protocol_tx_not_ready_before_rf(
    tmp_path: Path,
    ready,
    block,
):
    receipt = write_remote_rf_receipt(tmp_path)
    report = json.loads(receipt.read_text(encoding="utf-8"))
    report["steps"][0]["result"]["time"] = {
        "protocol_tx_ready": ready,
        "protocol_tx_block": block,
    }
    write_json(receipt, report)

    gate = audit.rf_gate(
        receipt,
        tmp_path,
        COMMIT,
        "disabled",
        RUN_ID,
        RUN_ATTEMPT,
    )

    assert gate.ok is False
    assert gate.details["derived_checks"][
        "protocol_tx_ready_before_rf"
    ] is False


@pytest.mark.parametrize(
    ("section", "field", "value"),
    [
        ("controlled_peer", "ssh_host", "neonx@192.168.0.25"),
        ("controlled_peer", "hostname", "forged-pi"),
        ("controlled_peer", "status_path", "/tmp/status.json"),
        ("controlled_peer", "max_status_age_sec", 120.001),
        (
            "controlled_peer_before_receipt",
            "source_hostname",
            "forged-pi",
        ),
        (
            "controlled_peer_control.request_receipt",
            "source_hostname",
            "forged-pi",
        ),
    ],
)
def test_remote_core_rf_gate_rejects_forged_peer_binding(
    tmp_path: Path,
    section: str,
    field: str,
    value,
):
    receipt = write_remote_rf_receipt(tmp_path)
    report = json.loads(receipt.read_text(encoding="utf-8"))
    target = report
    for part in section.split("."):
        target = target[part]
    target[field] = value
    receipt.write_text(json.dumps(report), encoding="utf-8")

    assert (
        audit.rf_gate(
            receipt,
            tmp_path,
            COMMIT,
            "disabled",
            RUN_ID,
            RUN_ATTEMPT,
        ).ok
        is False
    )


def test_remote_core_rf_gate_recomputes_after_coordinated_raw_digest_tamper(
    tmp_path: Path,
):
    receipt = write_remote_rf_receipt(tmp_path)
    report = json.loads(receipt.read_text(encoding="utf-8"))
    after_row = report["controlled_peer_after_receipt"]
    after_path = tmp_path / after_row["path"]
    after = json.loads(after_path.read_text(encoding="utf-8"))
    after["counters"]["tx_dm_total"] += 1
    after_path.write_text(json.dumps(after), encoding="utf-8")
    digest = audit.sha256_file(after_path)
    after_row.update(
        {
            "size": after_path.stat().st_size,
            "sha256": digest,
            "remote_sha256": digest,
        }
    )
    receipt.write_text(json.dumps(report), encoding="utf-8")

    gate = audit.rf_gate(
        receipt,
        tmp_path,
        COMMIT,
        "disabled",
        RUN_ID,
        RUN_ATTEMPT,
    )

    assert gate.ok is False
    assert gate.details["raw_status_ok"] is False


@pytest.mark.parametrize(
    ("section", "field", "value"),
    [
        (
            "controlled_peer_before_receipt",
            "transport",
            "ssh",
        ),
        (
            "controlled_peer_control.request_receipt",
            "transport",
            "ssh-unix-socket-request",
        ),
        (
            "controlled_peer",
            "ssh_host",
            "neonx@192.168.0.24",
        ),
    ],
)
def test_active_soak_rejects_mixed_local_rf_receipt_before_capture(
    tmp_path: Path,
    section: str,
    field: str,
    value,
):
    receipt = write_remote_rf_receipt(tmp_path, local=True)
    report = json.loads(receipt.read_text(encoding="utf-8"))
    target = report
    for part in section.split("."):
        target = target[part]
    target[field] = value
    receipt.write_text(json.dumps(report), encoding="utf-8")

    with pytest.raises(
        ValueError,
        match="does not match the exact qualified",
    ):
        soak_runner.qualified_controlled_peer_receipt(
            path=receipt,
            root=tmp_path,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            fingerprint=(
                audit.rf_acceptance.REMOTE_PEER_FINGERPRINT
            ),
        )


@pytest.mark.parametrize("local", [False, True])
def test_pinned_active_soak_recomputes_status_sidecars_and_binding(
    tmp_path: Path,
    local: bool,
):
    rf_receipt = write_remote_rf_receipt(tmp_path, local=local)
    rf_report = json.loads(rf_receipt.read_text(encoding="utf-8"))
    rf = audit.rf_acceptance
    peer = rf_report["controlled_peer"]
    common_config = {
        "hostname": peer["hostname"],
        "status_path": peer["status_path"],
        "control_socket": peer["control_socket"],
        "device": peer["device"],
        "public_key": peer["public_key"],
        "max_status_age_sec": peer["max_status_age_sec"],
    }
    config = (
        rf.validate_local_peer_config(common_config)
        if local
        else rf.validate_remote_peer_config(
            {"ssh_host": peer["ssh_host"], **common_config}
        )
    )
    qualified, _ = soak_runner.qualified_controlled_peer_receipt(
        path=rf_receipt,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        fingerprint=rf.REMOTE_PEER_FINGERPRINT,
    )
    assert qualified == rf_report

    before_observed = datetime(
        2026, 7, 23, 16, 0, tzinfo=timezone.utc
    )
    after_observed = before_observed + timedelta(hours=1)

    def status(*, after: bool) -> dict:
        observed = after_observed if after else before_observed
        return {
            "service": "openclaw-radio-listener",
            "run_id": "pi5-peer-soak-run",
            "status_written_at": (
                observed - timedelta(seconds=1)
            ).isoformat(),
            "serial": {
                "port": rf.REMOTE_PEER_DEVICE,
                "mesh_connected": True,
                "self_prefix": rf.REMOTE_PEER_PUBLIC_KEY[:12],
                "public_key": rf.REMOTE_PEER_PUBLIC_KEY,
            },
            "mesh": {
                "last_fetch_ok_at": (
                    observed - timedelta(seconds=1)
                ).isoformat(),
                "last_rx_at": (
                    "2026-07-23T16:59:58Z"
                    if after
                    else "2026-07-23T15:59:58Z"
                ),
                "last_rx_kind": "dm",
                "last_rx_sender": rf.DEFAULT_D1L_PUBLIC_KEY[:12],
                "last_tx_at": (
                    "2026-07-23T16:59:59Z"
                    if after
                    else "2026-07-23T15:59:59Z"
                ),
                "last_tx_kind": "dm",
            },
            "startup_self_test": {"enabled": True, "ok": True},
            "counters": {
                "rx_dm_total": 16 if after else 10,
                "tx_dm_total": 26 if after else 20,
                "local_fast_reply_total": 36 if after else 30,
                "tx_dm_ack_miss_total": 2,
            },
        }

    before = status(after=False)
    after = status(after=True)
    sidecar_dir = tmp_path / "artifacts" / "soak" / "rf-peer"
    sidecar_dir.mkdir(parents=True)
    before_path = write_json(sidecar_dir / "before.json", before)
    after_path = write_json(sidecar_dir / "after.json", after)

    def row(path: Path, observed: datetime, value: dict) -> dict:
        digest = audit.sha256_file(path)
        written = rf.parse_aware_timestamp(
            value["status_written_at"]
        )
        assert written is not None
        mtime_ns = int(written.timestamp() * 1_000_000_000)
        return {
            "path": path.relative_to(tmp_path).as_posix(),
            "size": path.stat().st_size,
            "sha256": digest,
            "source_path": config["status_path"],
            "source_host": (
                config["hostname"] if local else config["ssh_host"]
            ),
            "source_hostname": config["hostname"],
            "transport": (
                rf.LOCAL_PEER_STATUS_TRANSPORT
                if local
                else "ssh"
            ),
            "captured_at": observed.isoformat(),
            **(
                {
                    "source_mtime_ns": mtime_ns,
                    "source_sha256": digest,
                }
                if local
                else {
                    "remote_mtime_ns": mtime_ns,
                    "remote_sha256": digest,
                }
            ),
        }

    before_row = row(before_path, before_observed, before)
    after_row = row(after_path, after_observed, after)
    if local:
        before_validation = rf.validate_local_peer_status(
            before,
            config,
            observed_at=before_observed,
            source_mtime_ns=before_row["source_mtime_ns"],
        )
        after_validation = rf.validate_local_peer_status(
            after,
            config,
            observed_at=after_observed,
            source_mtime_ns=after_row["source_mtime_ns"],
        )
    else:
        before_validation = rf.validate_remote_peer_status(
            before,
            config,
            observed_at=before_observed,
        )
        after_validation = rf.validate_remote_peer_status(
            after,
            config,
            observed_at=after_observed,
        )

    data = passing_soak(active=True)
    text = "core soak test"
    fingerprint = rf.REMOTE_PEER_FINGERPRINT
    command = f"mesh send dm {fingerprint} {text}"
    for event in data["active_events"]:
        event.update(
            {
                "command": command,
                "fingerprint": fingerprint,
                "text": text,
            }
        )
    data.update(
        {
            "active_dm_fingerprint": fingerprint,
            "active_dm_text": text,
            "active_command": command,
            "active_interval_sec": 600,
            "controlled_peer_before": rf.status_snapshot(before),
            "controlled_peer_after": rf.status_snapshot(after),
            "controlled_peer_before_receipt": before_row,
            "controlled_peer_after_receipt": after_row,
            "controlled_peer_remote": {
                "before_validation": before_validation,
                "after_validation": after_validation,
            },
            "controlled_peer_counter_deltas": {
                "rx_dm_total": 6,
                "tx_dm_total": 6,
                "local_fast_reply_total": 6,
                "tx_dm_ack_miss_total": 0,
            },
            "controlled_peer_successful_send_count": 6,
            "controlled_peer_expected_send_count": 6,
            "controlled_peer_flow_ok": True,
        }
    )

    ok, details = audit.active_soak_peer_flow_ok(
        data,
        rf_report,
        tmp_path,
    )
    assert ok is True
    assert details["remote_mode"] is (not local)
    assert details["local_mode"] is local
    assert details["pinned_status_validation_ok"] is True
    assert details["remote_status_validation_ok"] is True
    assert details["canonical_status_sources"] is True

    data["controlled_peer_before_receipt"][
        "source_hostname"
    ] = "forged-pi"
    ok, details = audit.active_soak_peer_flow_ok(
        data,
        rf_report,
        tmp_path,
    )
    assert ok is False
    assert details["canonical_status_sources"] is False
    data["controlled_peer_before_receipt"][
        "source_hostname"
    ] = rf.REMOTE_PEER_HOSTNAME

    if local:
        data["controlled_peer_before_receipt"][
            "transport"
        ] = "ssh"
        ok, details = audit.active_soak_peer_flow_ok(
            data,
            rf_report,
            tmp_path,
        )
        assert ok is False
        assert details["canonical_status_sources"] is False
        data["controlled_peer_before_receipt"][
            "transport"
        ] = rf.LOCAL_PEER_STATUS_TRANSPORT

        rf_report["controlled_peer"][
            "ssh_host"
        ] = rf.REMOTE_PEER_SSH_HOST
        ok, details = audit.active_soak_peer_flow_ok(
            data,
            rf_report,
            tmp_path,
        )
        assert ok is False
        assert details["binding_exact"] is False
        del rf_report["controlled_peer"]["ssh_host"]

    after["serial"]["port"] = "/dev/krab-other"
    write_json(after_path, after)
    tampered_digest = audit.sha256_file(after_path)
    data["controlled_peer_after_receipt"].update(
        {
            "size": after_path.stat().st_size,
            "sha256": tampered_digest,
            (
                "source_sha256"
                if local
                else "remote_sha256"
            ): tampered_digest,
        }
    )
    ok, details = audit.active_soak_peer_flow_ok(
        data,
        rf_report,
        tmp_path,
    )
    assert ok is False
    assert details["remote_status_validation_ok"] is False


class DummyImportedGate:
    def __init__(self, ok=True):
        self.ok = ok

    def to_dict(self):
        return {
            "id": "imported",
            "severity": "P0",
            "ok": self.ok,
            "title": "imported",
            "evidence": [],
            "details": {},
        }


def audit_args(tmp_path: Path) -> argparse.Namespace:
    return argparse.Namespace(
        root=str(tmp_path),
        dry_run=False,
        github_run_id=RUN_ID,
        github_run_attempt=RUN_ATTEMPT,
        github_run_dir=str(tmp_path / "github"),
        commit=COMMIT,
        d1l_port="COM12",
        sd_history_mode="disabled",
        hardware_dir=str(tmp_path / "hardware"),
        soak_dir=str(tmp_path / "soak"),
        actions_run_receipt=None,
        core_smoke=None,
        core_ui=None,
        manual_review=None,
        reboot_receipt=None,
        protocol_migration_receipt=None,
        rf_receipt=None,
        active_soak=None,
        idle_soak=None,
        sd_receipt=None,
        install_review=None,
        defect_receipt=None,
        out=None,
    )


def patch_all_gates(monkeypatch, tmp_path: Path, *, one_failure=False):
    package = tmp_path / "package"
    package.mkdir()
    (package / "manifest.json").write_text("{}", encoding="ascii")
    monkeypatch.setattr(audit, "find_release_package", lambda _root: package)
    monkeypatch.setattr(
        audit,
        "actions_inventory_gate",
        lambda *_args: audit.CoreGate(
            "inventory", not one_failure, "inventory"
        ),
    )
    for name in (
        "audit_runner_source_gate",
        "actions_run_metadata_gate",
        "core_immutable_source_inputs_gate",
        "core_flash_receipt_gate",
    ):
        monkeypatch.setattr(
            audit,
            name,
            lambda *_args, **_kwargs: audit.CoreGate(
                name, True, name
            ),
        )
    for name in (
        "immutable_release_source_inputs_gate",
        "host_checks_success_gate",
        "checksum_gate",
        "meshcore_conformance_evidence_gate",
        "meshcore_signed_advert_evidence_gate",
        "esp32_flash_receipt_gate",
        "notices_gate",
    ):
        monkeypatch.setattr(
            audit, name, lambda *_args, **_kwargs: DummyImportedGate(True)
        )
    monkeypatch.setattr(
        audit,
        "package_gate",
        lambda *_args: audit.CoreGate("package", True, "package"),
    )
    for name in (
        "core_smoke_gate",
        "core_ui_gate",
        "manual_review_gate",
        "reboot_persistence_gate",
        "protocol_migration_gate",
        "rf_gate",
        "sd_decision_gate",
        "soak_gate",
        "install_review_gate",
        "defect_gate",
    ):
        monkeypatch.setattr(
            audit,
            name,
            lambda *_args, **_kwargs: audit.CoreGate(name, True, name),
        )


def test_core_audit_ready_field_is_independent_and_full_remains_false(
    tmp_path, monkeypatch
):
    patch_all_gates(monkeypatch, tmp_path)
    report = audit.build_audit(audit_args(tmp_path))

    assert report["core_release_ready"] is True
    assert report["full_feature_release_ready"] is False
    assert report["github_actions_run_attempt"] == int(RUN_ATTEMPT)
    assert report["workflow_run_attempt"] == int(RUN_ATTEMPT)
    assert (
        report["github_actions_run_attempt"]
        == report["workflow_run_attempt"]
    )
    assert "ready_for_public_release" not in report


def test_core_audit_fails_closed_when_one_core_gate_fails(
    tmp_path, monkeypatch
):
    patch_all_gates(monkeypatch, tmp_path, one_failure=True)
    report = audit.build_audit(audit_args(tmp_path))

    assert report["core_release_ready"] is False
    assert report["full_feature_release_ready"] is False
    assert report["p0_failed_count"] == 1


def test_core_audit_fails_closed_when_protocol_migration_gate_fails(
    tmp_path,
    monkeypatch,
):
    patch_all_gates(monkeypatch, tmp_path)
    monkeypatch.setattr(
        audit,
        "protocol_migration_gate",
        lambda *_args, **_kwargs: audit.CoreGate(
            "protocol_timestamp_migration",
            False,
            "protocol migration",
        ),
    )

    report = audit.build_audit(audit_args(tmp_path))

    assert report["core_release_ready"] is False
    assert report["p0_failed_count"] == 1
    assert any(
        gate["id"] == "protocol_timestamp_migration" and not gate["ok"]
        for gate in report["gates"]
    )

def test_core_audit_fails_closed_for_red_non_p0_gate(
    tmp_path, monkeypatch
):
    patch_all_gates(monkeypatch, tmp_path)
    monkeypatch.setattr(
        audit,
        "actions_inventory_gate",
        lambda *_args: audit.CoreGate(
            "inventory", False, "inventory", severity="P1"
        ),
    )
    report = audit.build_audit(audit_args(tmp_path))

    assert report["core_release_ready"] is False
    assert report["failed_count"] == 1
    assert report["p0_failed_count"] == 0


def test_core_audit_rejects_conditional_final_sd_mode(tmp_path):
    args = audit_args(tmp_path)
    args.sd_history_mode = "conditional"
    with pytest.raises(ValueError, match="disabled with NVS"):
        audit.build_audit(args)


def test_defect_gate_delegates_to_strict_raw_api_validator(
    tmp_path, monkeypatch
):
    path = write_json(tmp_path / "issues.json", {})
    calls = []

    def validator(receipt, **kwargs):
        calls.append((receipt, kwargs))
        return True, [], {
            "raw_capture_ok": True,
            "release_gate_ok": True,
        }

    monkeypatch.setattr(
        audit, "validate_core_github_defect_receipt", validator
    )
    gate = audit.defect_gate(
        path, tmp_path, COMMIT, RUN_ID, RUN_ATTEMPT
    )

    assert gate.ok is True
    assert calls == [
        (
            path,
            {
                "root": tmp_path,
                "commit": COMMIT,
                "run_id": RUN_ID,
                "run_attempt": RUN_ATTEMPT,
                "max_age_sec": audit.DEFECT_RECEIPT_MAX_AGE_SEC,
                "max_future_skew_sec": (
                    audit.DEFECT_RECEIPT_MAX_FUTURE_SKEW_SEC
                ),
            },
        )
    ]
    monkeypatch.setattr(
        audit,
        "validate_core_github_defect_receipt",
        lambda *_args, **_kwargs: (
            True,
            [],
            {"release_gate_ok": False},
        ),
    )
    assert not audit.defect_gate(
        path, tmp_path, COMMIT, RUN_ID, RUN_ATTEMPT
    ).ok
    monkeypatch.setattr(
        audit,
        "validate_core_github_defect_receipt",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            ValueError("tampered")
        ),
    )
    raised = audit.defect_gate(
        path, tmp_path, COMMIT, RUN_ID, RUN_ATTEMPT
    )
    assert raised.ok is False
    assert raised.details["reasons"] == [
        "strict_r8_defect_validator_failed:ValueError"
    ]
    monkeypatch.setattr(
        audit, "validate_core_github_defect_receipt", None
    )
    assert not audit.defect_gate(
        path, tmp_path, COMMIT, RUN_ID, RUN_ATTEMPT
    ).ok


def test_existing_full_default_audit_contract_is_unchanged():
    args = full_audit.parse_args([])

    assert full_audit.FULL_SOAK_SECONDS == 12 * 60 * 60
    assert args.commit is None
    assert args.github_run_id is None
    assert args.fail_on_open_p0 is False
    assert not hasattr(args, "release_profile")
