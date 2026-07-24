import base64
import copy
import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

from scripts import core_reboot_persistence_d1l as reboot


COMMIT = "a" * 40
RUN_ID = "123456789"
RUN_ATTEMPT = "1"
WITNESS_BASE = reboot.candidate_witness_identity(
    COMMIT, RUN_ID, RUN_ATTEMPT
)
TOKEN = WITNESS_BASE["token"]
WITNESS_LABEL = WITNESS_BASE["witness_request_label"]
PUBLIC_KEY = (
    "0000000000000001"
    "000000000000000000000000000000000000000000000001"
)
FINGERPRINT = PUBLIC_KEY[:16].upper()
SOURCE_GIT = {
    "commit": COMMIT,
    "short_commit": COMMIT[:7],
    "branch": "release/24h-core",
    "dirty": False,
    "dirty_entries": [],
}


def target_row(
    *,
    device: str = "COM12",
    vid: int = 0x1A86,
    pid: int = 0x7523,
):
    return {
        "device": device,
        "description": "D1L USB serial",
        "hwid": "USB VID:PID=1A86:7523 LOCATION=1-2",
        "serial_number": "D1L-COM12",
        "vid": vid,
        "pid": pid,
        "location": "1-2",
        "manufacturer": "wch.cn",
        "product": "USB Serial",
    }


def valid_port_lister():
    return [target_row()]


def target_snapshot():
    return reboot.resolve_core_target(
        "COM12",
        port_lister=valid_port_lister,
        platform_name="nt",
    )


def alternate_target_snapshot():
    row = target_row()
    row["serial_number"] = "OTHER-D1L"
    row["hwid"] = "USB VID:PID=1A86:7523 SER=OTHER-D1L"
    return reboot.resolve_core_target(
        "COM12",
        port_lister=lambda: [row],
        platform_name="nt",
    )


def posix_target_snapshot(resolved_tty: str):
    requested = reboot.D1L_CORE_POSIX_TARGET
    row = target_row(device=resolved_tty)
    row["serial_number"] = None
    row["hwid"] = "USB VID:PID=1A86:7523 LOCATION=1-2"

    def realpath(value: str):
        return resolved_tty if value == requested else value

    return reboot.resolve_target(
        requested,
        port_lister=lambda: [row],
        platform_name="posix",
        exists=lambda _value: True,
        is_symlink=lambda value: value == requested,
        realpath=realpath,
        access=lambda _value, _mode: True,
        hostname=lambda: "neopi5",
    )


def identity_status():
    return {
        "schema": 1,
        "ok": True,
        "cmd": "identity status",
        "public_key_ready": True,
        "public_key": PUBLIC_KEY,
        "fingerprint": PUBLIC_KEY[:16].upper(),
        "role": "desk_companion",
    }


def raw_line(payload: bytes, observed_at: str = "2026-07-18T12:00:00Z"):
    return {
        "observed_at": observed_at,
        "size": len(payload),
        "sha256": reboot.sha256_bytes(payload),
        "base64": base64.b64encode(payload).decode("ascii"),
    }


def command_receipt(command: str, result: dict):
    payload = (json.dumps(result, separators=(",", ":")) + "\n").encode("ascii")
    return {
        "command": command,
        "expected_cmd": reboot.expected_raw_command_name(command),
        "started_at": "2026-07-18T12:00:00Z",
        "ended_at": "2026-07-18T12:00:01Z",
        "raw_lines": [raw_line(payload)],
        "result": copy.deepcopy(result),
    }


def persistence():
    return {
        "loaded": True,
        "dirty": False,
        "revision": 5,
        "commits": 5,
        "failures": 0,
        "stale_snapshots": 0,
        "sd": {
            "required": False,
            "generation": 0,
            "dirty": False,
            "reconcile_pending": False,
            "commits": 0,
            "failures": 0,
            "last_error": "ESP_OK",
        },
        "nvs": {
            "dirty": False,
            "commits": 5,
            "failures": 0,
            "last_error": "ESP_OK",
        },
    }


def version():
    return {
        "schema": 1,
        "ok": True,
        "cmd": "version",
        "build_commit": COMMIT,
        "idf": "v5.5.4",
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
    }


def health(nonce: int, uptime: int, reset_reason: str):
    return {
        "schema": 1,
        "ok": True,
        "cmd": "health",
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "board_ready": True,
        "ui_ready": True,
        "nvs_ready": True,
        "boot_nonce": nonce,
        "uptime_ms": uptime,
        "reset_reason": reset_reason,
    }


def crashlog(seq: int, reset_reason: str):
    return {
        "schema": 1,
        "ok": True,
        "cmd": "crashlog",
        "total_written": seq,
        "entries": [
            {
                "seq": seq,
                "reset_reason": reset_reason,
                "crash_like": False,
            }
        ],
    }


def settings(name: str = f"D1L-Core-{COMMIT[:7]}"):
    return {
        "schema": 1,
        "ok": True,
        "cmd": "settings get",
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "node_name": name,
        "role": "companion",
        "onboarding_complete": True,
        "wifi_enabled": False,
        "ble_companion_enabled": False,
        "observer_enabled": False,
        "high_contrast": False,
        "night_mode": False,
        "path_hash_bytes": 1,
        "timezone": {
            "settings_ready": True,
            "settings_error": "ESP_OK",
            "schema_version": 1,
            "model": "fixed_utc_offset",
            "offset_minutes": -240,
            "label": "UTC-04:00",
            "auto_dst": False,
        },
        "radio": {
            "frequency_hz": 910525000,
            "bandwidth_khz": 62.5,
            "sf": 7,
            "cr": 5,
            "tx_power_dbm": 22,
            "rx_boost": True,
            "tcxo": "1.8V",
            "applied_to_radio": True,
            "radio_apply_pending": False,
            "radio_apply_error": "ESP_OK",
        },
    }


def message_page(command: str):
    is_dm = command == "messages dm"
    entry = (
        {
            "seq": 20,
            "uptime_ms": 1234,
            "fingerprint": FINGERPRINT,
            "alias": "Core Witness",
            "direction": "rx",
            "text": WITNESS_LABEL,
            "rssi_dbm": -60,
            "snr_tenths": 40,
            "path_hash_bytes": 1,
            "path_hops": 0,
            "attempt": 0,
            "delivered": True,
            "acked": False,
            "ack_hash": 1,
            "ack_response": {
                "identity_valid": True,
                "state": "acked",
                "dispatch_count": 1,
                "last_kind": "ack",
                "last_error": "ESP_OK",
            },
        }
        if is_dm
        else {
            "seq": 10,
            "uptime_ms": 1000,
            "direction": "rx",
            "author": "Core Witness",
            "text": WITNESS_LABEL,
            "rssi_dbm": -65,
            "snr_tenths": 35,
            "path_hash_bytes": 1,
            "path_hops": 0,
            "delivered": True,
        }
    )
    return {
        "schema": 1,
        "ok": True,
        "cmd": command,
        "count": 1,
        "capacity": 32,
        "total_written": 1,
        "dropped_oldest": 0,
        "filtered": False,
        "offset": 0,
        "page_size": 8,
        "page_count": 1,
        "total_matches": 1,
        "has_older": False,
        "next_offset": 0,
        "retained_epoch": 1,
        "content_revision": 1,
        "persistence": persistence(),
        "entries": [
            {
                **entry,
                "retained": True,
                "volatile_preview": False,
            }
        ],
        "persisted": True,
    } | (
        {
            "retained_count": 1,
            "volatile_preview_present": False,
            "volatile_preview_seq": 0,
        }
        if is_dm
        else {
            "retained_store_count": 1,
            "retained_public_count": 1,
            "volatile_preview_present": False,
            "volatile_preview_seq": 0,
        }
    )


def unread():
    return {
        "schema": 1,
        "ok": True,
        "cmd": "messages unread",
        "public_unread": 0,
        "dm_unread": 0,
        "muted_dm_unread": 0,
        "dm_thread_count": 1,
        "last_public_read_seq": 10,
        "last_dm_read_seq": 20,
        "newest_public_rx_seq": 10,
        "newest_dm_rx_seq": 20,
        "mark_read_count": 2,
        "dm_threads": [
            {
                "fingerprint": FINGERPRINT,
                "last_read_seq": 20,
                "newest_rx_seq": 20,
                "unread": 0,
                "muted": False,
            }
        ],
        "persisted": True,
    }


def contacts():
    return {
        "schema": 1,
        "ok": True,
        "cmd": "contacts",
        "count": 1,
        "capacity": 32,
        "total_written": 1,
        "dropped_oldest": 0,
        "persistence_revision": 1,
        "persistence_dirty": False,
        "persistence_last_error": "ESP_OK",
        "entries": [
            {
                "seq": 30,
                "created_ms": 100,
                "updated_ms": 100,
                "fingerprint": FINGERPRINT,
                "public_key": PUBLIC_KEY,
                "alias": "Core Witness",
                "heard_name": "",
                "type": "chat",
                "verification_source": "uri_import",
                "verified_at_ms": 100,
                "signed_advert_timestamp": 0,
                "last_heard_ms": 0,
                "canonical": True,
                "can_dm": True,
                "can_admin": False,
                "last_rssi_dbm": -60,
                "last_snr_tenths": 40,
                "path_hash_bytes": 1,
                "path_hops": 0,
                "out_path_known": True,
                "out_path_len": 0,
                "out_path_updated_ms": 100,
                "favorite": False,
                "muted": False,
            }
        ],
        "persisted": True,
    }


def full_message_pages(command: str):
    is_dm = command == "messages dm"
    template = message_page(command)
    entry_template = template["entries"][0]
    entries = []
    for index in range(16):
        entry = copy.deepcopy(entry_template)
        entry["seq"] = (101 if is_dm else 1) + index
        entry["text"] = (
            f"retained dm {index + 1}"
            if is_dm
            else f"retained public {index + 1}"
        )
        entries.append(entry)

    pages = []
    for offset, page_entries in ((0, entries[8:]), (8, entries[:8])):
        page = copy.deepcopy(template)
        page.update(
            {
                "count": 16,
                "capacity": 16,
                "total_written": 23,
                "dropped_oldest": 7,
                "offset": offset,
                "page_count": 8,
                "total_matches": 16,
                "has_older": offset == 0,
                "next_offset": 8,
                "content_revision": 42,
                "entries": page_entries,
            }
        )
        if is_dm:
            page["retained_count"] = 16
        else:
            page.update(
                {
                    "retained_store_count": 16,
                    "retained_public_count": 16,
                }
            )
        pages.append(page)
    return pages


def full_contacts():
    result = contacts()
    template = result["entries"][0]
    entries = []
    for index in range(16):
        entry = copy.deepcopy(template)
        entry["seq"] = 201 + index
        entry["fingerprint"] = f"{index + 1:016x}"
        entry["public_key"] = (
            entry["fingerprint"] + f"{index + 1:048x}"
        )
        entry["alias"] = f"Retained {index + 1}"
        entry["verification_source"] = "signed_advert"
        entries.append(entry)
    result.update(
        {
            "count": 16,
            "capacity": 16,
            "total_written": 23,
            "dropped_oldest": 7,
            "persistence_revision": 42,
            "entries": entries,
        }
    )
    return result


def full_unread():
    result = unread()
    result.update(
        {
            "last_public_read_seq": 16,
            "last_dm_read_seq": 116,
            "newest_public_rx_seq": 16,
            "newest_dm_rx_seq": 116,
        }
    )
    result["dm_threads"][0].update(
        {
            "last_read_seq": 116,
            "newest_rx_seq": 116,
        }
    )
    return result


def state_capture(nonce: int, uptime: int, reset_reason: str, crash_seq: int):
    rows = [
        ("version", version()),
        ("health", health(nonce, uptime, reset_reason)),
        ("crashlog", crashlog(crash_seq, reset_reason)),
        ("settings get", settings()),
        ("messages public", message_page("messages public")),
        ("messages dm", message_page("messages dm")),
        ("messages unread", unread()),
        ("contacts", contacts()),
    ]
    return {
        "captured_at": "2026-07-18T12:00:00Z",
        "commands": [command_receipt(command, result) for command, result in rows],
    }


def initial_state_capture(
    nonce: int, uptime: int, reset_reason: str, crash_seq: int
):
    return full_state_capture(
        nonce, uptime, reset_reason, crash_seq, node_name="D1L Desk"
    )


def full_state_capture(
    nonce: int,
    uptime: int,
    reset_reason: str,
    crash_seq: int,
    *,
    node_name: str = WITNESS_BASE["settings_node_name"],
):
    rows = [
        ("version", version()),
        ("health", health(nonce, uptime, reset_reason)),
        ("crashlog", crashlog(crash_seq, reset_reason)),
        ("settings get", settings(node_name)),
    ]
    public_pages = full_message_pages("messages public")
    dm_pages = full_message_pages("messages dm")
    rows.extend(
        [
            ("messages public", public_pages[0]),
            ("messages public offset 8", public_pages[1]),
            ("messages dm", dm_pages[0]),
            ("messages dm offset 8", dm_pages[1]),
            ("messages unread", full_unread()),
            ("contacts", full_contacts()),
        ]
    )
    return {
        "captured_at": "2026-07-18T12:00:00Z",
        "commands": [
            command_receipt(command, result) for command, result in rows
        ],
    }


def port_sample(present: bool, monotonic: float):
    target = target_snapshot() if present else None
    return {
        "observed_at": "2026-07-18T12:00:00Z",
        "monotonic_sec": monotonic,
        "requested_path": "COM12",
        "state": "present" if present else "absent",
        "present": present,
        "valid_absence": not present,
        "d1l_target": target,
        "error": (
            None
            if present
            else "the requested D1L target is not present"
        ),
    }


def reboot_command():
    return command_receipt(
        "reboot",
        {
            "schema": 1,
            "ok": True,
            "cmd": "reboot",
            "rebooting": True,
            "reset_scope": "system",
            "storage_manager_quiesced": True,
            "retained_worker_quiesced": True,
            "rp2040_bridge_quiesced": True,
            "connectivity_prepare": "ESP_OK",
            "retained_flush": "ESP_OK",
            "route_flush": "ESP_OK",
        },
    )


def boot_lines():
    reset = b"rst:0x3 (RTC_SW_SYS_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n"
    help_row = b'{"schema":1,"ok":true,"cmd":"help"}\r\n'
    return [raw_line(b"ESP-ROM:esp32s3-20210327\r\n"), raw_line(reset), raw_line(help_row)]


def retained_witness_result():
    selected_contact = full_contacts()["entries"][-1]
    return {
        "schema": 1,
        "ok": True,
        "cmd": "core retained-witness",
        "token": TOKEN,
        "witness_request_label": WITNESS_LABEL,
        "fingerprint": selected_contact["fingerprint"],
        "public_key": selected_contact["public_key"],
        "public_seq": 16,
        "dm_seq": 116,
        "contact_seq": 216,
        "public_mode": "existing_full_preserved",
        "dm_mode": "existing_full_preserved",
        "contact_mode": "existing_full_preserved",
        "contact_result": "existing_full_preserved",
        "witness_only": True,
        "public_mutated": False,
        "dm_mutated": False,
        "contact_mutated": False,
        "public_evicted": False,
        "dm_evicted": False,
        "contact_evicted": False,
        "public_store_count_before": 16,
        "public_store_count_after": 16,
        "public_retained_count_before": 16,
        "public_retained_count_after": 16,
        "public_capacity": 16,
        "public_total_written_before": 23,
        "public_total_written_after": 23,
        "public_dropped_oldest_before": 7,
        "public_dropped_oldest_after": 7,
        "public_content_revision_before": 42,
        "public_content_revision_after": 42,
        "dm_count_before": 16,
        "dm_count_after": 16,
        "dm_capacity": 16,
        "dm_total_written_before": 23,
        "dm_total_written_after": 23,
        "dm_dropped_oldest_before": 7,
        "dm_dropped_oldest_after": 7,
        "dm_content_revision_before": 42,
        "dm_content_revision_after": 42,
        "contact_count_before": 16,
        "contact_count_after": 16,
        "contact_capacity": 16,
        "contact_total_written_before": 23,
        "contact_total_written_after": 23,
        "contact_dropped_oldest_before": 7,
        "contact_dropped_oldest_after": 7,
        "contact_persistence_revision_before": 42,
        "contact_persistence_revision_after": 42,
        "persisted": True,
        "retention": "nvs",
        "backend_mode": "nvs_disabled",
        "synthetic_local": False,
        "retained_flush": "not_requested_zero_mutation",
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "sd_access": False,
        "rp2040_access": False,
        "formats_sd": False,
        "predecessor_evidence_used": False,
    }


def full_store_projection():
    projection, errors, _ = reboot.recompute_state_capture(
        full_state_capture(100, 600000, "SW", 10), COMMIT
    )
    assert not errors
    assert projection is not None
    return projection


def full_store_witness_result(projection=None):
    projection = projection or full_store_projection()
    selected_contact = projection["contacts"]["entries"][-1]
    result = retained_witness_result()
    result.update(
        {
            "fingerprint": selected_contact["fingerprint"],
            "public_key": selected_contact["public_key"],
            "public_seq": projection["public_messages"]["entries"][-1]["seq"],
            "dm_seq": projection["direct_messages"]["entries"][-1]["seq"],
            "contact_seq": selected_contact["seq"],
            "public_mode": "existing_full_preserved",
            "dm_mode": "existing_full_preserved",
            "contact_mode": "existing_full_preserved",
            "contact_result": "existing_full_preserved",
            "public_mutated": False,
            "dm_mutated": False,
            "contact_mutated": False,
            "public_store_count_before": 16,
            "public_store_count_after": 16,
            "public_retained_count_before": 16,
            "public_retained_count_after": 16,
            "public_capacity": 16,
            "public_total_written_before": 23,
            "public_total_written_after": 23,
            "public_dropped_oldest_before": 7,
            "public_dropped_oldest_after": 7,
            "public_content_revision_before": 42,
            "public_content_revision_after": 42,
            "dm_count_before": 16,
            "dm_count_after": 16,
            "dm_capacity": 16,
            "dm_total_written_before": 23,
            "dm_total_written_after": 23,
            "dm_dropped_oldest_before": 7,
            "dm_dropped_oldest_after": 7,
            "dm_content_revision_before": 42,
            "dm_content_revision_after": 42,
            "contact_count_before": 16,
            "contact_count_after": 16,
            "contact_capacity": 16,
            "contact_total_written_before": 23,
            "contact_total_written_after": 23,
            "contact_dropped_oldest_before": 7,
            "contact_dropped_oldest_after": 7,
            "contact_persistence_revision_before": 42,
            "contact_persistence_revision_after": 42,
        }
    )
    return result


def seed_receipt():
    initial_capture = initial_state_capture(100, 600000, "SW", 10)
    capture = full_state_capture(100, 600000, "SW", 10)
    initial_projection, initial_errors, _ = reboot.recompute_state_capture(
        initial_capture, COMMIT
    )
    assert not initial_errors
    witness_result = retained_witness_result()
    derived_witness, witness_errors = reboot.recompute_retained_witness_proof(
        witness_result,
        expected=WITNESS_BASE,
        initial_projection=initial_projection,
    )
    assert not witness_errors
    assert derived_witness is not None
    projection, errors, _ = reboot.recompute_state_capture(capture, COMMIT)
    assert not errors
    d1l_target = target_snapshot()
    return {
        "schema": 2,
        "kind": "core_retained_state_seed",
        "mode": "hardware",
        "ok": True,
        "closure_eligible": False,
        "hardware_required": True,
        "physical_observed": True,
        "port": "COM12",
        "d1l_target": d1l_target,
        "expected_target_identity_sha256": d1l_target[
            "stable_identity_sha256"
        ],
        "expected_d1l_public_key": PUBLIC_KEY,
        "d1l_identity_status": command_receipt(
            "identity status", identity_status()
        ),
        "d1l_identity_ok": True,
        "baud": 115200,
        "commit": COMMIT,
        "github_actions_run": RUN_ID,
        "workflow_run_attempt": RUN_ATTEMPT,
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "git": SOURCE_GIT,
        "captured_at": "2026-07-18T12:00:00Z",
        "retention_witness": derived_witness,
        "initial_state_capture": initial_capture,
        "retained_witness_proof": command_receipt(
            f"core retained-witness {TOKEN}",
            witness_result,
        ),
        "settings_retention_mutation": command_receipt(
            f"settings set name D1L-Core-{COMMIT[:7]}",
            {
                "schema": 1,
                "ok": True,
                "cmd": "settings set name",
                "persisted": True,
                "node_name": f"D1L-Core-{COMMIT[:7]}",
            },
        ),
        "state_capture": capture,
        "producer_io": {
            "stage": "complete",
            "serial_open_attempted": True,
            "serial_opened": True,
            "physical_observed": True,
            "settings_mutation_may_have_executed": True,
            "settings_mutation_confirmed_persisted": True,
            "mutation_outcome_uncertain": False,
        },
        "mutation_outcome_uncertain": False,
        "projection_sha256": reboot.projection_sha256(projection),
        "checks": {},
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "sd_access": False,
        "rp2040_access": False,
        "formats_sd": False,
        "predecessor_evidence_used": False,
    }


def write_json(path: Path, value: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="ascii")


def row(path: Path, root: Path):
    return {
        "path": path.relative_to(root).as_posix(),
        "size": path.stat().st_size,
        "sha256": reboot.sha256_file(path),
    }


def flash_receipt(root: Path, seed: dict):
    d1l_target = target_snapshot()
    results = [
        version(),
        health(110, 10000, "SW"),
        settings(),
        *full_message_pages("messages public"),
        *full_message_pages("messages dm"),
        full_contacts(),
    ]
    snapshot_projection = {"retention_witness": "bound"}
    snapshots = []
    for phase in ("pre_flash", "post_flash"):
        path = root / f"{phase}.json"
        write_json(
            path,
            {
                "schema": 2,
                "kind": "core_retained_state_snapshot",
                "mode": "hardware",
                "phase": phase,
                "port": "COM12",
                "d1l_target": d1l_target,
                "expected_firmware_commit": COMMIT,
                "results": results,
                "projection": snapshot_projection,
                "projection_sha256": reboot.projection_sha256(snapshot_projection),
            },
        )
        snapshots.append(row(path, root))
    raw = root / "flash.log"
    raw.write_bytes(b"esptool write-flash success\n")
    return {
        "schema": 2,
        "kind": "esp32_flash",
        "mode": "hardware",
        "scope": "core-retained-reflash-only",
        "flash_phase": "retained-reflash",
        "ok": True,
        "closure_eligible": True,
        "hardware_required": True,
        "physical_observed": True,
        "port": "COM12",
        "d1l_target": d1l_target,
        "d1l_target_before": d1l_target,
        "d1l_target_after": d1l_target,
        "target_identity_continuity_ok": True,
        "expected_d1l_public_key": PUBLIC_KEY,
        "pre_flash_identity": identity_status(),
        "post_flash_identity": identity_status(),
        "d1l_public_key_continuity_ok": True,
        "commit": COMMIT,
        "github_actions_run": RUN_ID,
        "workflow_run_attempt": RUN_ATTEMPT,
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "firmware_identity_ok": True,
        "runner_source_identity_ok": True,
        "retained_state_preserved": True,
        "git": SOURCE_GIT,
        "command": [
            "python",
            "-m",
            "esptool",
            "--port",
            "COM12",
            "write-flash",
            "0x0",
            "bootloader.bin",
        ],
        "raw_flash_log": row(raw, root),
        "retained_state_before": snapshots[0],
        "retained_state_after": snapshots[1],
        "erase_flash": False,
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "sd_access": False,
        "rp2040_access": False,
        "formats_sd": False,
        "legacy_suite_ran": False,
    }


def valid_verify_inputs(root: Path):
    seed = seed_receipt()
    seed_path = root / "verify-seed.json"
    write_json(seed_path, seed)
    flash = flash_receipt(root, seed)
    flash_path = root / "verify-flash.json"
    write_json(flash_path, flash)
    return seed_path, flash_path


def cycle_receipt(
    cycle_type: str,
    ordinal: int,
    matrix_id: str,
    seed_hash: str,
    flash_hash: str,
    previous_hash: str,
):
    reset_reason = "SW" if cycle_type == "software" else "POWERON"
    report = reboot._cycle_base(
        matrix_id=matrix_id,
        cycle_type=cycle_type,
        ordinal=ordinal,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        seed_sha256=seed_hash,
        flash_sha256=flash_hash,
        previous_sha256=previous_hash,
        d1l_target=target_snapshot(),
    )
    report["pre"] = full_state_capture(
        1000 + ordinal, 600000, "SW", 100 + ordinal
    )
    report["post"] = full_state_capture(
        2000 + ordinal, 10000, reset_reason, 101 + ordinal
    )
    if cycle_type == "software":
        report["action"] = {
            "kind": "software_reboot",
            "reboot_command": reboot_command(),
            "boot_raw_lines": boot_lines(),
            "boot_analysis": {},
            "port_disappear_required": False,
            "d1l_target_before": target_snapshot(),
            "d1l_target_after": target_snapshot(),
        }
    else:
        report["action"] = {
            "kind": "operator_controlled_cold_power_cycle",
            "operator_interactive": True,
            "minimum_power_off_sec": 2.0,
            "observed_power_off_sec": 2.1,
            "d1l_target_before": target_snapshot(),
            "disappear_samples": [
                port_sample(True, 0.1),
                port_sample(False, 0.2),
            ],
            "power_off_samples": [
                port_sample(False, 0.2),
                port_sample(False, 2.3),
            ],
            "reappear_samples": [
                port_sample(False, 2.4),
                port_sample(True, 2.5),
            ],
            "d1l_target_after": target_snapshot(),
        }
    report["checks"] = {}
    report["ok"] = True
    report["closure_eligible"] = True
    report["physical_observed"] = True
    report["stage"] = "complete"
    report["serial_open_attempted"] = True
    report["serial_opened"] = True
    report["reboot_or_power_action_may_have_executed"] = True
    report["ended_at"] = "2026-07-18T12:01:00Z"
    return report


def valid_matrix_tree(tmp_path: Path):
    seed = seed_receipt()
    seed_path = tmp_path / "seed.json"
    write_json(seed_path, seed)
    seed_row = row(seed_path, tmp_path)
    flash = flash_receipt(tmp_path, seed)
    flash_path = tmp_path / "flash.json"
    write_json(flash_path, flash)
    flash_row = row(flash_path, tmp_path)
    matrix_id = "1" * 32
    previous = flash_row["sha256"]
    cycle_rows = []
    for cycle_type, count in (
        ("software", reboot.SOFTWARE_CYCLE_COUNT),
        ("cold", reboot.COLD_CYCLE_COUNT),
    ):
        for ordinal in range(1, count + 1):
            receipt = cycle_receipt(
                cycle_type,
                ordinal,
                matrix_id,
                seed_row["sha256"],
                flash_row["sha256"],
                previous,
            )
            path = tmp_path / "cycles" / f"{cycle_type}_{ordinal}.json"
            write_json(path, receipt)
            cycle_row = row(path, tmp_path)
            cycle_rows.append(cycle_row)
            previous = cycle_row["sha256"]
    live = full_state_capture(3000, 10000, "SW", 200)
    live_projection, errors, _ = reboot.recompute_state_capture(live, COMMIT)
    assert not errors
    matrix = {
        "schema": 2,
        "kind": "core_reboot_persistence_matrix",
        "mode": "hardware",
        "ok": True,
        "closure_eligible": True,
        "hardware_required": True,
        "physical_observed": True,
        "matrix_id": matrix_id,
        "port": "COM12",
        "d1l_target": target_snapshot(),
        "expected_target_identity_sha256": target_snapshot()[
            "stable_identity_sha256"
        ],
        "expected_d1l_public_key": PUBLIC_KEY,
        "baud": 115200,
        "commit": COMMIT,
        "github_actions_run": RUN_ID,
        "workflow_run_attempt": RUN_ATTEMPT,
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "claim": reboot.CLAIM,
        "cross_version_migration_proven": False,
        "predecessor_evidence_used": False,
        "git": SOURCE_GIT,
        "seed_receipt": seed_row,
        "closing_flash_receipt": flash_row,
        "post_reinstall_d1l_target": target_snapshot(),
        "post_reinstall_identity_status": command_receipt(
            "identity status", identity_status()
        ),
        "post_reinstall_identity_ok": True,
        "post_reinstall_live_capture": live,
        "post_reinstall_projection_sha256": reboot.projection_sha256(
            live_projection
        ),
        "software_cycle_count": 5,
        "cold_cycle_count": 3,
        "cycle_receipts": cycle_rows,
        "all_child_receipts_unique": True,
        "hash_chain_tail": previous,
        "started_at": "2026-07-18T12:00:00Z",
        "ended_at": "2026-07-18T13:00:00Z",
        "public_rf_tx": False,
        "dm_rf_tx": False,
        "formats_sd": False,
    }
    matrix_path = tmp_path / "matrix.json"
    write_json(matrix_path, matrix)
    return matrix_path, matrix


@pytest.mark.parametrize("port", ["COM8", "COM11", "COM16", "COM29", "COM13"])
def test_only_com12_is_admitted(port):
    with pytest.raises(ValueError, match="requires COM12 or the exact"):
        reboot.enforce_core_port(port)


def test_exact_posix_by_id_target_is_admitted():
    assert (
        reboot.enforce_core_port(reboot.D1L_CORE_POSIX_TARGET)
        == reboot.D1L_CORE_POSIX_TARGET
    )

    with pytest.raises(ValueError, match="requires COM12 or the exact"):
        reboot.enforce_core_port("/dev/ttyUSB2")
    with pytest.raises(ValueError, match="requires COM12 or the exact"):
        reboot.enforce_core_port(
            f" {reboot.D1L_CORE_POSIX_TARGET} "
        )


@pytest.mark.parametrize(
    ("port", "rows"),
    [
        ("COM12", [target_row(vid=0x10C4)]),
        ("/dev/ttyUSB2", [target_row(device="/dev/ttyUSB2")]),
    ],
)
def test_invalid_target_fails_before_serial_open(
    tmp_path, monkeypatch, port, rows
):
    opened = False

    def unexpected_open(*_args, **_kwargs):
        nonlocal opened
        opened = True
        raise AssertionError("serial must not open for an invalid target")

    monkeypatch.setattr(reboot, "_open_serial", unexpected_open)
    with pytest.raises(ValueError):
        reboot.seed_retained_state(
            root=tmp_path,
            out=tmp_path / f"invalid-{len(rows)}-{len(port)}.json",
            serial_module=object(),
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            timeout=1.0,
            source_git=SOURCE_GIT,
            port=port,
            port_lister=lambda: rows,
            expected_d1l_public_key=PUBLIC_KEY,
            platform_name="nt" if port == "COM12" else "posix",
        )

    assert opened is False


def test_target_drift_is_rejected_before_next_serial_open(monkeypatch):
    opened = False

    def unexpected_open(*_args, **_kwargs):
        nonlocal opened
        opened = True
        raise AssertionError("serial must not open after target drift")

    monkeypatch.setattr(reboot, "_open_serial", unexpected_open)
    initial = target_snapshot()
    report = reboot._cycle_base(
        matrix_id="1" * 32,
        cycle_type="software",
        ordinal=1,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        seed_sha256="2" * 64,
        flash_sha256="3" * 64,
        previous_sha256="3" * 64,
        d1l_target=initial,
    )

    with pytest.raises(ValueError, match="identity drifted"):
        reboot.run_software_cycle(
            serial_module=object(),
            port_lister=lambda: [
                {
                    **target_row(),
                    "serial_number": "OTHER-D1L",
                    "hwid": "USB VID:PID=1A86:7523 SER=OTHER-D1L",
                }
            ],
            timeout=1.0,
            transition_timeout=1.0,
            commit=COMMIT,
            baseline={},
            report=report,
            requested_target="COM12",
            expected_target_identity_sha256=initial[
                "stable_identity_sha256"
            ],
            platform_name="nt",
        )

    assert opened is False


def test_cold_posix_by_id_allows_tty_renumbering():
    before = posix_target_snapshot("/dev/ttyUSB2")
    after = posix_target_snapshot("/dev/ttyUSB9")
    assert before["stable_identity_sha256"] == after[
        "stable_identity_sha256"
    ]

    def sample(state: str, at: float, snapshot=None, error=None):
        return {
            "observed_at": "2026-07-24T12:00:00Z",
            "monotonic_sec": at,
            "requested_path": reboot.D1L_CORE_POSIX_TARGET,
            "state": state,
            "present": state == "present",
            "valid_absence": state == "absent",
            "d1l_target": snapshot,
            "error": error,
        }

    missing = "POSIX D1L by-id target is missing or dangling"
    action = {
        "kind": "operator_controlled_cold_power_cycle",
        "operator_interactive": True,
        "minimum_power_off_sec": 2.0,
        "observed_power_off_sec": 2.1,
        "d1l_target_before": before,
        "disappear_samples": [
            sample("present", 0.0, before),
            sample("absent", 0.1, error=missing),
        ],
        "power_off_samples": [
            sample("absent", 0.1, error=missing),
            sample("absent", 2.2, error=missing),
        ],
        "reappear_samples": [
            sample("absent", 2.3, error=missing),
            sample("present", 2.4, after),
        ],
        "d1l_target_after": after,
    }

    ok, errors = reboot._target_action_recomputed(
        action,
        cycle_type="cold",
        requested_target=reboot.D1L_CORE_POSIX_TARGET,
        expected_target_identity_sha256=before[
            "stable_identity_sha256"
        ],
    )

    assert ok is True, errors
    assert errors == []


def test_serial_open_uses_posix_by_id_path_never_resolved_tty(monkeypatch):
    opened = []
    sentinel = object()

    def fake_open(
        _serial_module, *, port, baudrate, timeout
    ):
        opened.append((port, baudrate, timeout))
        return sentinel

    monkeypatch.setattr(reboot, "open_d1l_serial", fake_open)

    result = reboot._open_serial(
        object(), 8.0, reboot.D1L_CORE_POSIX_TARGET
    )

    assert result is sentinel
    assert opened == [
        (reboot.D1L_CORE_POSIX_TARGET, reboot.D1L_BAUD, 8.0)
    ]
    assert "/dev/ttyUSB" not in opened[0][0]


def test_cold_reappearance_with_wrong_usb_ids_is_rejected():
    action = cycle_receipt(
        "cold", 1, "1" * 32, "2" * 64, "3" * 64, "3" * 64
    )["action"]
    action["reappear_samples"][-1] = {
        "observed_at": "2026-07-24T12:00:00Z",
        "monotonic_sec": 2.5,
        "requested_path": "COM12",
        "state": "invalid",
        "present": False,
        "valid_absence": False,
        "d1l_target": None,
        "error": "D1L VID must be 0x1A86; got 4292",
    }
    action["d1l_target_after"] = None

    ok, errors = reboot._target_action_recomputed(
        action,
        cycle_type="cold",
        requested_target="COM12",
        expected_target_identity_sha256=target_snapshot()[
            "stable_identity_sha256"
        ],
    )

    assert ok is False
    assert errors == ["cold cycle raw port transition evidence failed"]


def test_candidate_witness_identity_is_exact_run_and_attempt_bound():
    witness = reboot.candidate_witness_identity(
        COMMIT, RUN_ID, RUN_ATTEMPT
    )

    assert witness == WITNESS_BASE
    assert witness["token"].startswith("core-")
    assert len(witness["token"]) == 29
    assert witness["witness_request_label"].endswith(witness["token"])
    assert (
        reboot.candidate_witness_identity(COMMIT, RUN_ID, "2")["token"]
        != witness["token"]
    )


def test_source_guard_rejects_dirty_or_wrong_checkout(tmp_path):
    with pytest.raises(ValueError, match="exact clean candidate"):
        reboot.exact_source_git(
            tmp_path,
            COMMIT,
            metadata={**SOURCE_GIT, "dirty": True, "dirty_entries": ["M file"]},
        )
    with pytest.raises(ValueError, match="exact clean candidate"):
        reboot.exact_source_git(
            tmp_path,
            COMMIT,
            metadata={**SOURCE_GIT, "commit": "b" * 40},
        )


def test_state_projection_is_recomputed_from_raw_and_contains_witnesses():
    capture = full_state_capture(1, 10000, "SW", 10)
    projection, errors, raw = reboot.recompute_state_capture(capture, COMMIT)

    assert errors == []
    assert projection is not None
    assert raw["health"]["boot_nonce"] == 1
    witness, witness_errors = reboot.recompute_retained_witness_proof(
        full_store_witness_result(projection),
        expected=WITNESS_BASE,
        initial_projection=projection,
    )
    assert witness_errors == []
    assert witness is not None
    ok, witness_check_errors = reboot.retention_witness_check(
        projection,
        witness=witness,
    )
    assert ok is True
    assert witness_check_errors == []


def test_public_offset_page_matches_base_response_command():
    page = message_page("messages public")
    page.update(
        {
            "offset": 8,
            "total_matches": 9,
            "has_older": False,
            "next_offset": 8,
        }
    )
    row = command_receipt("messages public offset 8", page)

    result, errors = reboot.recompute_raw_command(row)

    assert errors == []
    assert result == page
    assert row["expected_cmd"] == "messages public"


@pytest.mark.parametrize("command", ["messages public", "messages dm"])
def test_page_projection_excludes_volatile_preview(command):
    page = message_page(command)
    volatile = copy.deepcopy(page["entries"][0])
    volatile.update(
        {
            "seq": 999,
            "text": "volatile UI preview",
            "retained": False,
            "volatile_preview": True,
        }
    )
    page.update(
        {
            "page_count": 2,
            "total_matches": 2,
            "volatile_preview_present": True,
            "volatile_preview_seq": 999,
            "entries": [page["entries"][0], volatile],
        }
    )

    projection, errors = reboot._page_projection(command, [page])

    assert errors == []
    assert projection is not None
    assert projection["count"] == 1
    assert [entry["seq"] for entry in projection["entries"]] == [
        page["entries"][0]["seq"]
    ]


def test_raw_command_tampering_is_rejected():
    row = command_receipt("version", version())
    row["result"]["build_commit"] = "b" * 40

    result, errors = reboot.recompute_raw_command(row)

    assert result["build_commit"] == COMMIT
    assert "parsed result does not match raw result" in errors[0]


def test_contact_snapshot_must_not_be_truncated():
    capture = state_capture(1, 10000, "SW", 10)
    contact = capture["commands"][-1]["result"]
    contact["count"] = 2
    capture["commands"][-1] = command_receipt("contacts", contact)

    projection, errors, _ = reboot.recompute_state_capture(capture, COMMIT)

    assert projection is not None
    assert any("snapshot is truncated" in error for error in errors)


def test_full_capacity_contact_snapshot_is_complete():
    result = contacts()
    template = result["entries"][0]
    entries = []
    for index in range(16):
        entry = copy.deepcopy(template)
        entry["seq"] = index + 1
        entry["fingerprint"] = f"{index + 1:016x}"
        entry["public_key"] = f"{index + 1:064x}"
        entries.append(entry)
    result.update(
        {
            "count": 16,
            "capacity": 16,
            "total_written": 16,
            "entries": entries,
        }
    )

    projection, errors = reboot._contact_projection(result)

    assert errors == []
    assert projection is not None
    assert projection["count"] == 16
    assert len(projection["entries"]) == 16


def test_full_stores_use_exact_existing_durable_witnesses_without_mutation():
    initial = full_store_projection()
    result = full_store_witness_result(initial)

    witness, errors = reboot.recompute_retained_witness_proof(
        result,
        expected=WITNESS_BASE,
        initial_projection=initial,
    )

    assert errors == []
    assert witness is not None
    assert witness["public_witness"] == initial["public_messages"]["entries"][-1]
    assert witness["dm_witness"] == initial["direct_messages"]["entries"][-1]
    assert witness["contact_witness"] == initial["contacts"]["entries"][-1]
    observed = copy.deepcopy(initial)
    observed["settings"]["node_name"] = WITNESS_BASE["settings_node_name"]
    ok, witness_errors = reboot.retention_witness_check(
        observed, witness=witness
    )
    assert ok is True
    assert witness_errors == []
    preserved, transition_errors = reboot.seed_store_transition_preserved(
        initial,
        observed,
        witness=witness,
    )
    assert preserved is True
    assert transition_errors == []


def test_full_store_witness_mode_is_rejected_when_capacity_exists():
    initial = full_store_projection()
    initial["public_messages"]["retained_store_count"] = 15
    result = full_store_witness_result()
    result["public_store_count_before"] = 15
    result["public_store_count_after"] = 15

    witness, errors = reboot.recompute_retained_witness_proof(
        result,
        expected=WITNESS_BASE,
        initial_projection=initial,
    )

    assert witness is None
    assert any(
        "Public witness full-store mode is unsafe" in error
        for error in errors
    )


def test_public_witness_rejects_one_public_row_in_full_mixed_shared_store():
    initial = full_store_projection()
    public = initial["public_messages"]
    public["count"] = 1
    public["retained_store_count"] = 16
    public["entries"] = [public["entries"][-1]]
    result = full_store_witness_result()
    result["public_store_count_before"] = 16
    result["public_store_count_after"] = 16
    result["public_retained_count_before"] = 1
    result["public_retained_count_after"] = 1
    result["public_seq"] = public["entries"][0]["seq"]

    witness, errors = reboot.recompute_retained_witness_proof(
        result,
        expected=WITNESS_BASE,
        initial_projection=initial,
    )

    assert witness is None
    assert any(
        "Public witness full-store mode is unsafe" in error
        for error in errors
    )


@pytest.mark.parametrize(
    ("projection_key", "count_key", "result_key"),
    [
        ("public_messages", "retained_store_count", "public_store_count"),
        ("direct_messages", "count", "dm_count"),
        ("contacts", "count", "contact_count"),
    ],
)
def test_witness_proof_rejects_any_non_full_store_without_mutation(
    projection_key, count_key, result_key
):
    initial = full_store_projection()
    initial[projection_key][count_key] = 15
    result = full_store_witness_result()
    result[f"{result_key}_before"] = 15
    result[f"{result_key}_after"] = 15

    witness, errors = reboot.recompute_retained_witness_proof(
        result,
        expected=WITNESS_BASE,
        initial_projection=initial,
    )

    assert witness is None
    assert result["public_mutated"] is False
    assert result["dm_mutated"] is False
    assert result["contact_mutated"] is False
    assert any("full-store mode is unsafe" in error for error in errors)


def test_witness_proof_rejects_legacy_mutation_mode():
    initial = full_store_projection()
    result = full_store_witness_result(initial)
    result["public_mode"] = "legacy_mutation_persisted"
    result["public_mutated"] = True

    witness, errors = reboot.recompute_retained_witness_proof(
        result,
        expected=WITNESS_BASE,
        initial_projection=initial,
    )

    assert witness is None
    assert any("public_mutated mismatch" in error for error in errors)
    assert any("Public proof must be witness-only" in error for error in errors)


def test_witness_proof_retry_is_idempotent_and_zero_mutation():
    initial = full_store_projection()
    original = copy.deepcopy(initial)
    result = full_store_witness_result(initial)

    first, first_errors = reboot.recompute_retained_witness_proof(
        copy.deepcopy(result),
        expected=WITNESS_BASE,
        initial_projection=initial,
    )
    second, second_errors = reboot.recompute_retained_witness_proof(
        copy.deepcopy(result),
        expected=WITNESS_BASE,
        initial_projection=initial,
    )

    assert first_errors == []
    assert second_errors == []
    assert first == second
    assert initial == original
    assert result["witness_only"] is True
    assert result["public_mutated"] is False
    assert result["dm_mutated"] is False
    assert result["contact_mutated"] is False


def test_volatile_preview_cannot_be_selected_as_full_store_witness():
    initial = full_store_projection()
    result = full_store_witness_result(initial)
    result["public_seq"] = 999

    witness, errors = reboot.recompute_retained_witness_proof(
        result,
        expected=WITNESS_BASE,
        initial_projection=initial,
    )

    assert witness is None
    assert any(
        "Public durable witness is not unique" in error for error in errors
    )


@pytest.mark.parametrize(
    ("field", "value", "error_fragment"),
    [
        ("public_evicted", True, "public_evicted mismatch"),
        ("dm_dropped_oldest_after", 8, "DM witness dropped_oldest changed"),
        ("contact_mutated", True, "contact_mutated mismatch"),
    ],
)
def test_full_store_witness_rejects_eviction_or_mutation(
    field, value, error_fragment
):
    initial = full_store_projection()
    result = full_store_witness_result(initial)
    result[field] = value

    witness, errors = reboot.recompute_retained_witness_proof(
        result,
        expected=WITNESS_BASE,
        initial_projection=initial,
    )

    assert witness is None
    assert any(error_fragment in error for error in errors)


def test_full_store_witness_rejects_contact_not_in_initial_snapshot():
    initial = full_store_projection()
    result = full_store_witness_result(initial)
    result["fingerprint"] = "f" * 16
    result["public_key"] = "f" * 64

    witness, errors = reboot.recompute_retained_witness_proof(
        result,
        expected=WITNESS_BASE,
        initial_projection=initial,
    )

    assert witness is None
    assert any(
        "selected contact was not the exact initial retained witness" in error
        for error in errors
    )


def test_full_store_witness_requires_exact_final_preservation():
    initial = full_store_projection()
    witness, errors = reboot.recompute_retained_witness_proof(
        full_store_witness_result(initial),
        expected=WITNESS_BASE,
        initial_projection=initial,
    )
    assert not errors
    assert witness is not None
    observed = copy.deepcopy(initial)
    observed["contacts"]["entries"].pop()

    ok, transition_errors = reboot.seed_store_transition_preserved(
        initial,
        observed,
        witness=witness,
    )

    assert ok is False
    assert "contacts: full-store witness state changed" in transition_errors


def test_state_preservation_rejects_removed_message_and_read_state_change():
    baseline, errors, _ = reboot.recompute_state_capture(
        state_capture(1, 10000, "SW", 10), COMMIT
    )
    assert not errors
    observed = copy.deepcopy(baseline)
    observed["direct_messages"]["entries"] = []
    observed["read_state"]["last_dm_read_seq"] = 0

    ok, reasons = reboot.state_preserved(baseline, observed)

    assert ok is False
    assert "direct_messages lost retained rows" in reasons
    assert "message read-state changed or was lost" in reasons


def test_software_cycle_is_recomputed_from_raw_not_checks():
    baseline, errors, _ = reboot.recompute_state_capture(
        full_state_capture(1, 10000, "SW", 10), COMMIT
    )
    assert not errors
    receipt = cycle_receipt(
        "software", 1, "1" * 32, "2" * 64, "3" * 64, "3" * 64
    )
    receipt["checks"] = {"all_green": False}

    ok, reasons = reboot.recompute_cycle(
        receipt,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        matrix_id="1" * 32,
        baseline=baseline,
        expected_target="COM12",
        expected_target_identity_sha256=target_snapshot()[
            "stable_identity_sha256"
        ],
    )

    assert ok is True
    assert reasons == []


def test_software_cycle_rejects_raw_poweron_banner():
    baseline, errors, _ = reboot.recompute_state_capture(
        full_state_capture(1, 10000, "SW", 10), COMMIT
    )
    assert not errors
    receipt = cycle_receipt(
        "software", 1, "1" * 32, "2" * 64, "3" * 64, "3" * 64
    )
    receipt["action"]["boot_raw_lines"][1] = raw_line(
        b"rst:0x1 (POWERON_RESET),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n"
    )

    ok, reasons = reboot.recompute_cycle(
        receipt,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        matrix_id="1" * 32,
        baseline=baseline,
        expected_target="COM12",
        expected_target_identity_sha256=target_snapshot()[
            "stable_identity_sha256"
        ],
    )

    assert ok is False
    assert "software reboot raw boot/reset evidence failed" in reasons


def test_cycle_validator_rejects_unclosed_physical_outcome():
    baseline, errors, _ = reboot.recompute_state_capture(
        full_state_capture(1, 10000, "SW", 10), COMMIT
    )
    assert not errors
    receipt = cycle_receipt(
        "software", 1, "1" * 32, "2" * 64, "3" * 64, "3" * 64
    )
    receipt["physical_state_outcome_uncertain"] = True

    ok, reasons = reboot.recompute_cycle(
        receipt,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        matrix_id="1" * 32,
        baseline=baseline,
        expected_target="COM12",
        expected_target_identity_sha256=target_snapshot()[
            "stable_identity_sha256"
        ],
    )

    assert ok is False
    assert "cycle: physical_state_outcome_uncertain mismatch" in reasons


def test_cold_cycle_duration_is_derived_from_raw_monotonic_samples():
    action = cycle_receipt(
        "cold", 1, "1" * 32, "2" * 64, "3" * 64, "3" * 64
    )["action"]
    action["observed_power_off_sec"] = 99.0

    ok, errors = reboot._target_action_recomputed(
        action,
        cycle_type="cold",
        requested_target="COM12",
        expected_target_identity_sha256=target_snapshot()[
            "stable_identity_sha256"
        ],
    )

    assert ok is False
    assert errors == ["cold cycle raw port transition evidence failed"]


def test_valid_matrix_recomputes_all_eight_unique_chained_children(tmp_path):
    matrix_path, _ = valid_matrix_tree(tmp_path)

    ok, errors, _ = reboot.validate_core_reboot_persistence_receipt(
        matrix_path,
        root=tmp_path,
        expected_commit=COMMIT,
        expected_run_id=RUN_ID,
        expected_run_attempt=RUN_ATTEMPT,
    )

    assert ok is True, errors
    assert errors == []


def test_matrix_validator_rejects_zero_actions_run_id(tmp_path):
    matrix_path, _ = valid_matrix_tree(tmp_path)

    ok, errors, _ = reboot.validate_core_reboot_persistence_receipt(
        matrix_path,
        root=tmp_path,
        expected_commit=COMMIT,
        expected_run_id="0",
        expected_run_attempt=RUN_ATTEMPT,
    )

    assert ok is False
    assert errors == ["validator expected identity is invalid"]


def test_matrix_rejects_copied_cycle_receipt(tmp_path):
    matrix_path, matrix = valid_matrix_tree(tmp_path)
    matrix["cycle_receipts"][1] = copy.deepcopy(matrix["cycle_receipts"][0])
    write_json(matrix_path, matrix)

    ok, errors, _ = reboot.validate_core_reboot_persistence_receipt(
        matrix_path,
        root=tmp_path,
        expected_commit=COMMIT,
        expected_run_id=RUN_ID,
        expected_run_attempt=RUN_ATTEMPT,
    )

    assert ok is False
    assert any("duplicate cycle path" in error for error in errors)
    assert any("duplicate cycle content/hash" in error for error in errors)


def test_matrix_rejects_cross_version_migration_claim(tmp_path):
    matrix_path, matrix = valid_matrix_tree(tmp_path)
    matrix["claim"] = "upgrade_migration"
    matrix["cross_version_migration_proven"] = True
    write_json(matrix_path, matrix)

    ok, errors, _ = reboot.validate_core_reboot_persistence_receipt(
        matrix_path,
        root=tmp_path,
        expected_commit=COMMIT,
        expected_run_id=RUN_ID,
        expected_run_attempt=RUN_ATTEMPT,
    )

    assert ok is False
    assert "matrix: claim mismatch" in errors
    assert "matrix: cross_version_migration_proven mismatch" in errors


def test_flash_validator_rejects_bootstrap_or_erasing_flash(tmp_path):
    seed = seed_receipt()
    flash = flash_receipt(tmp_path, seed)
    flash["flash_phase"] = "bootstrap"
    flash["command"].insert(-2, "erase-flash")

    errors = reboot.validate_closing_flash_receipt(
        flash,
        root=tmp_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        witness=seed["retention_witness"],
        expected_target="COM12",
        expected_target_identity_sha256=target_snapshot()[
            "stable_identity_sha256"
        ],
        expected_d1l_public_key=PUBLIC_KEY,
    )

    assert "flash: flash_phase mismatch" in errors
    assert any("non-erasing" in error for error in errors)


def test_seed_validator_requires_raw_settings_retention_mutation():
    seed = seed_receipt()
    seed["settings_retention_mutation"]["result"]["persisted"] = False

    projection, errors = reboot.validate_seed_receipt(
        seed,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    assert projection is not None
    assert any("parsed result does not match raw result" in error for error in errors)


def test_seed_validator_requires_raw_full_store_witness_set():
    seed = seed_receipt()
    seed["retained_witness_proof"]["result"]["sd_access"] = True

    projection, errors = reboot.validate_seed_receipt(
        seed,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    assert projection is not None
    assert any("parsed result does not match raw result" in error for error in errors)


def test_seed_validator_rejects_unclosed_mutation_outcome():
    seed = seed_receipt()
    seed["mutation_outcome_uncertain"] = True
    seed["producer_io"]["mutation_outcome_uncertain"] = True

    projection, errors = reboot.validate_seed_receipt(
        seed,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )

    assert projection is not None
    assert "seed: mutation_outcome_uncertain mismatch" in errors
    assert "seed: producer I/O and mutation outcome are not closed" in errors


def test_seed_producer_proves_run_bound_witness_before_settings(
    tmp_path, monkeypatch
):
    class SerialContext:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def reset_input_buffer(self):
            return None

    commands = []
    captures = iter(
        [
            initial_state_capture(1, 10000, "SW", 10),
            full_state_capture(1, 11000, "SW", 10),
        ]
    )
    monkeypatch.setattr(
        reboot, "_open_serial", lambda *_args: SerialContext()
    )
    monkeypatch.setattr(
        reboot,
        "capture_state",
        lambda *_args, **_kwargs: next(captures),
    )

    def fake_command(_serial, command, *_args, **_kwargs):
        commands.append(command)
        if command == "identity status":
            return command_receipt(command, identity_status())
        if command.startswith("core retained-witness "):
            return command_receipt(command, retained_witness_result())
        return command_receipt(
            command,
            {
                "schema": 1,
                "ok": True,
                "cmd": "settings set name",
                "persisted": True,
                "node_name": WITNESS_BASE["settings_node_name"],
            },
        )

    monkeypatch.setattr(reboot, "read_raw_command", fake_command)
    out = tmp_path / "seed.json"

    report = reboot.seed_retained_state(
        root=tmp_path,
        out=out,
        serial_module=object(),
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        timeout=1.0,
        source_git=SOURCE_GIT,
        port="COM12",
        port_lister=valid_port_lister,
        expected_d1l_public_key=PUBLIC_KEY,
        platform_name="nt",
    )

    assert report["ok"] is True, report["checks"]["errors"]
    assert report["checks"]["candidate_full_store_witness_set_proven"] is True
    assert commands == [
        "identity status",
        f"core retained-witness {TOKEN}",
        f"settings set name {WITNESS_BASE['settings_node_name']}",
    ]
    witness = json.loads(out.read_text(encoding="ascii"))[
        "retention_witness"
    ]
    for key in ("token", "witness_request_label", "settings_node_name"):
        assert witness[key] == WITNESS_BASE[key]
    assert witness["contact_fingerprint"] == "0000000000000010"
    assert witness["contact_public_key"] == (
        "0000000000000010000000000000000000000000000000000000000000000010"
    )
    assert witness["public_seq"] == 16
    assert witness["dm_seq"] == 116
    assert witness["contact_seq"] == 216
    assert witness["public_mode"] == "existing_full_preserved"
    assert witness["dm_mode"] == "existing_full_preserved"
    assert witness["contact_mode"] == "existing_full_preserved"


def test_seed_producer_does_not_mutate_wrong_candidate(tmp_path, monkeypatch):
    class SerialContext:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

    wrong = state_capture(1, 10000, "SW", 10)
    wrong_version = version()
    wrong_version["build_commit"] = "b" * 40
    wrong["commands"][0] = command_receipt("version", wrong_version)
    monkeypatch.setattr(
        reboot, "_open_serial", lambda *_args: SerialContext()
    )
    monkeypatch.setattr(
        reboot,
        "capture_state",
        lambda *_args, **_kwargs: wrong,
    )

    def unexpected_mutation(*_args, **_kwargs):
        raise AssertionError("wrong candidate must not receive a mutation")

    monkeypatch.setattr(reboot, "read_raw_command", unexpected_mutation)

    report = reboot.seed_retained_state(
        root=tmp_path,
        out=tmp_path / "wrong-seed.json",
        serial_module=object(),
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        timeout=1.0,
        source_git=SOURCE_GIT,
        port="COM12",
        port_lister=valid_port_lister,
        expected_d1l_public_key=PUBLIC_KEY,
        platform_name="nt",
    )

    assert report["ok"] is False
    assert report["retained_witness_proof"] is None
    assert report["settings_retention_mutation"] is None


def test_seed_producer_does_not_mutate_wrong_live_public_key(
    tmp_path, monkeypatch
):
    class SerialContext:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def reset_input_buffer(self):
            return None

    captures = iter(
        [
            initial_state_capture(1, 10000, "SW", 10),
            initial_state_capture(1, 11000, "SW", 10),
        ]
    )
    monkeypatch.setattr(
        reboot, "_open_serial", lambda *_args: SerialContext()
    )
    monkeypatch.setattr(
        reboot,
        "capture_state",
        lambda *_args, **_kwargs: next(captures),
    )

    def identity_only(_serial, command, *_args, **_kwargs):
        assert command == "identity status"
        wrong = identity_status()
        wrong["public_key"] = "b" * 64
        wrong["fingerprint"] = ("b" * 64)[:16].upper()
        return command_receipt(command, wrong)

    monkeypatch.setattr(reboot, "read_raw_command", identity_only)

    report = reboot.seed_retained_state(
        root=tmp_path,
        out=tmp_path / "wrong-key-seed.json",
        serial_module=object(),
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        timeout=1.0,
        source_git=SOURCE_GIT,
        port="COM12",
        port_lister=valid_port_lister,
        expected_d1l_public_key=PUBLIC_KEY,
        platform_name="nt",
    )

    assert report["ok"] is False
    assert report["d1l_identity_ok"] is False
    assert report["retained_witness_proof"] is None
    assert report["settings_retention_mutation"] is None


def test_output_writer_refuses_overwrite(tmp_path):
    path = tmp_path / "receipt.json"
    reboot.write_json_once(path, tmp_path, {"schema": 1})

    with pytest.raises(ValueError, match="refusing to overwrite"):
        reboot.write_json_once(path, tmp_path, {"schema": 1})


def test_output_writer_reservation_has_one_concurrent_winner(tmp_path):
    path = tmp_path / "concurrent-receipt.json"

    def attempt(index):
        try:
            reboot.write_json_once(
                path, tmp_path, {"schema": 1, "writer": index}
            )
            return index
        except ValueError:
            return None

    with ThreadPoolExecutor(max_workers=8) as pool:
        winners = [
            index
            for index in pool.map(attempt, range(16))
            if index is not None
        ]

    assert len(winners) == 1
    assert json.loads(path.read_text(encoding="ascii")) == {
        "schema": 1,
        "writer": winners[0],
    }


def test_seed_reserves_output_before_opening_serial(tmp_path, monkeypatch):
    path = tmp_path / "seed.json"
    sentinel = b'{"existing":true}\n'
    path.write_bytes(sentinel)
    opened = False

    def unexpected_open(*_args, **_kwargs):
        nonlocal opened
        opened = True
        raise AssertionError("serial must not open when output reservation fails")

    monkeypatch.setattr(reboot, "_open_serial", unexpected_open)

    with pytest.raises(ValueError, match="refusing to overwrite"):
        reboot.seed_retained_state(
            root=tmp_path,
            out=path,
            serial_module=object(),
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            timeout=1.0,
            source_git=SOURCE_GIT,
            port="COM12",
            port_lister=valid_port_lister,
            expected_d1l_public_key=PUBLIC_KEY,
            platform_name="nt",
        )

    assert opened is False
    assert path.read_bytes() == sentinel


def test_seed_exception_finalizes_reserved_failure_receipt(
    tmp_path, monkeypatch
):
    path = tmp_path / "seed-failure.json"

    def fail_open(*_args, **_kwargs):
        raise RuntimeError("serial unavailable")

    monkeypatch.setattr(reboot, "_open_serial", fail_open)

    with pytest.raises(RuntimeError, match="serial unavailable"):
        reboot.seed_retained_state(
            root=tmp_path,
            out=path,
            serial_module=object(),
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            timeout=1.0,
            source_git=SOURCE_GIT,
            port="COM12",
            port_lister=valid_port_lister,
            expected_d1l_public_key=PUBLIC_KEY,
            platform_name="nt",
        )

    receipt = json.loads(path.read_text(encoding="ascii"))
    assert receipt["ok"] is False
    assert receipt["physical_observed"] is False
    assert receipt["failure"] == {
        "stage": "opening_seed_serial",
        "type": "RuntimeError",
        "detail": "serial unavailable",
    }
    assert receipt["public_rf_tx"] is False
    assert receipt["dm_rf_tx"] is False
    assert receipt["sd_access"] is False


def test_inside_output_rejects_lexical_reparse_parent(
    tmp_path, monkeypatch
):
    parent = tmp_path / "junction-parent"
    parent.mkdir()

    monkeypatch.setattr(
        reboot,
        "is_link_or_reparse",
        lambda path: Path(path) == parent,
    )

    with pytest.raises(ValueError, match="link/reparse point"):
        reboot._inside_output(
            tmp_path, parent / "receipt.json", "test output"
        )


def test_output_reservation_rechecks_parent_after_mkdir(
    tmp_path, monkeypatch
):
    parent = tmp_path / "became-reparse"
    output = parent / "receipt.json"

    monkeypatch.setattr(
        reboot,
        "is_link_or_reparse",
        lambda path: Path(path) == parent and parent.exists(),
    )

    with pytest.raises(ValueError, match="link/reparse point"):
        reboot.reserve_json_output(output, tmp_path)

    assert output.exists() is False


def test_seed_failure_after_possible_settings_write_preserves_uncertainty(
    tmp_path, monkeypatch
):
    class SerialContext:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def reset_input_buffer(self):
            return None

    monkeypatch.setattr(
        reboot, "_open_serial", lambda *_args: SerialContext()
    )
    monkeypatch.setattr(
        reboot,
        "capture_state",
        lambda *_args, **_kwargs: initial_state_capture(
            1, 10000, "SW", 10
        ),
    )

    def command_then_fail(_serial, command, *_args, **kwargs):
        command_log = kwargs.get("command_log")
        if command == "identity status":
            receipt = command_receipt(command, identity_status())
            command_log.append(receipt)
            return receipt
        if command.startswith("core retained-witness "):
            receipt = command_receipt(command, retained_witness_result())
            command_log.append(receipt)
            return receipt
        partial = {
            "command": command,
            "expected_cmd": "settings set name",
            "started_at": "2026-07-18T12:00:00Z",
            "ended_at": None,
            "write_attempted": True,
            "raw_lines": [],
            "result": None,
        }
        command_log.append(partial)
        raise RuntimeError("serial failed after settings write")

    monkeypatch.setattr(reboot, "read_raw_command", command_then_fail)
    path = tmp_path / "seed-settings-uncertain.json"

    with pytest.raises(
        RuntimeError, match="serial failed after settings write"
    ):
        reboot.seed_retained_state(
            root=tmp_path,
            out=path,
            serial_module=object(),
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            timeout=1.0,
            source_git=SOURCE_GIT,
            port="COM12",
            port_lister=valid_port_lister,
            expected_d1l_public_key=PUBLIC_KEY,
            platform_name="nt",
        )

    receipt = json.loads(path.read_text(encoding="ascii"))
    assert receipt["physical_observed"] is True
    assert receipt["producer_io"]["serial_opened"] is True
    assert (
        receipt["producer_io"]["settings_mutation_may_have_executed"]
        is True
    )
    assert receipt["mutation_outcome_uncertain"] is True
    assert len(receipt["partial_command_receipts"]) == 3
    assert receipt["partial_command_receipts"][-1]["write_attempted"] is True


def test_verify_final_collision_causes_zero_device_io(
    tmp_path, monkeypatch
):
    out = tmp_path / "matrix.json"
    out.write_text("{}\n", encoding="ascii")
    opened = False

    def unexpected_open(*_args, **_kwargs):
        nonlocal opened
        opened = True
        raise AssertionError("verify must reserve outputs before serial")

    monkeypatch.setattr(reboot, "_open_serial", unexpected_open)

    with pytest.raises(ValueError, match="refusing to overwrite"):
        reboot.verify_reboot_matrix(
            root=tmp_path,
            out=out,
            seed_path=tmp_path / "missing-seed.json",
            flash_path=tmp_path / "missing-flash.json",
            serial_module=object(),
            port_lister=lambda: (_ for _ in ()).throw(
                AssertionError("port listing must not run")
            ),
            prompt=lambda _text: (_ for _ in ()).throw(
                AssertionError("prompt must not run")
            ),
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            timeout=1.0,
            transition_timeout=1.0,
            port_timeout=1.0,
            port_poll_sec=0.1,
            minimum_power_off_sec=2.0,
            source_git=SOURCE_GIT,
            port="COM12",
            expected_d1l_public_key=PUBLIC_KEY,
            platform_name="nt",
        )

    assert opened is False


def test_verify_cycle_directory_collision_causes_zero_device_io(
    tmp_path, monkeypatch
):
    out = tmp_path / "matrix.json"
    child_dir = tmp_path / "matrix_cycles"
    child_dir.mkdir()
    (child_dir / "software_1.json").write_text(
        "{}\n", encoding="ascii"
    )
    opened = False

    def unexpected_open(*_args, **_kwargs):
        nonlocal opened
        opened = True
        raise AssertionError("verify must reserve outputs before serial")

    monkeypatch.setattr(reboot, "_open_serial", unexpected_open)

    with pytest.raises(FileExistsError):
        reboot.verify_reboot_matrix(
            root=tmp_path,
            out=out,
            seed_path=tmp_path / "missing-seed.json",
            flash_path=tmp_path / "missing-flash.json",
            serial_module=object(),
            port_lister=lambda: [],
            prompt=lambda _text: "",
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            timeout=1.0,
            transition_timeout=1.0,
            port_timeout=1.0,
            port_poll_sec=0.1,
            minimum_power_off_sec=2.0,
            source_git=SOURCE_GIT,
            port="COM12",
            expected_d1l_public_key=PUBLIC_KEY,
            platform_name="nt",
        )

    assert opened is False
    assert out.exists()
    assert out.stat().st_size == 0


def test_verify_reserves_matrix_and_all_eight_children_before_serial(
    tmp_path, monkeypatch
):
    seed_path, flash_path = valid_verify_inputs(tmp_path)
    out = tmp_path / "reserved" / "matrix.json"
    child_dir = out.parent / "matrix_cycles"
    checked = False

    def fail_after_check(*_args, **_kwargs):
        nonlocal checked
        expected = [
            child_dir / f"software_{ordinal}.json"
            for ordinal in range(1, reboot.SOFTWARE_CYCLE_COUNT + 1)
        ] + [
            child_dir / f"cold_{ordinal}.json"
            for ordinal in range(1, reboot.COLD_CYCLE_COUNT + 1)
        ]
        assert out.exists() and out.stat().st_size == 0
        assert all(path.exists() and path.stat().st_size == 0 for path in expected)
        checked = True
        raise RuntimeError("stop after reservation proof")

    monkeypatch.setattr(reboot, "_open_serial", fail_after_check)

    with pytest.raises(RuntimeError, match="reservation proof"):
        reboot.verify_reboot_matrix(
            root=tmp_path,
            out=out,
            seed_path=seed_path,
            flash_path=flash_path,
            serial_module=object(),
            port_lister=valid_port_lister,
            prompt=lambda _text: "",
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            timeout=1.0,
            transition_timeout=1.0,
            port_timeout=1.0,
            port_poll_sec=0.1,
            minimum_power_off_sec=2.0,
            source_git=SOURCE_GIT,
            port="COM12",
            expected_d1l_public_key=PUBLIC_KEY,
            platform_name="nt",
        )

    assert checked is True
    matrix = json.loads(out.read_text(encoding="ascii"))
    assert matrix["physical_observed"] is False
    children = [
        json.loads(path.read_text(encoding="ascii"))
        for path in child_dir.glob("*.json")
    ]
    assert len(children) == 8
    assert all(row["execution_state"] == "not_executed" for row in children)


def test_verify_exception_after_reboot_action_is_physically_uncertain(
    tmp_path, monkeypatch
):
    seed_path, flash_path = valid_verify_inputs(tmp_path)
    out = tmp_path / "cycle-failure" / "matrix.json"

    class SerialContext:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def reset_input_buffer(self):
            return None

    monkeypatch.setattr(
        reboot, "_open_serial", lambda *_args: SerialContext()
    )
    monkeypatch.setattr(
        reboot,
        "capture_state",
        lambda *_args, **_kwargs: full_state_capture(
            3000, 10000, "SW", 200
        ),
    )
    monkeypatch.setattr(
        reboot,
        "read_raw_command",
        lambda _ser, command, *_args, **_kwargs: command_receipt(
            command, identity_status()
        ),
    )

    def fail_cycle(*, report, **_kwargs):
        report["serial_open_attempted"] = True
        report["serial_opened"] = True
        report["physical_observed"] = True
        report["stage"] = "software_reboot_command"
        report["reboot_or_power_action_may_have_executed"] = True
        report["partial_command_receipts"].append(
            {
                "command": "reboot",
                "write_attempted": True,
                "raw_lines": [],
                "result": None,
            }
        )
        raise RuntimeError("lost serial after reboot write")

    monkeypatch.setattr(reboot, "run_software_cycle", fail_cycle)

    with pytest.raises(RuntimeError, match="lost serial after reboot write"):
        reboot.verify_reboot_matrix(
            root=tmp_path,
            out=out,
            seed_path=seed_path,
            flash_path=flash_path,
            serial_module=object(),
            port_lister=valid_port_lister,
            prompt=lambda _text: "",
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            timeout=1.0,
            transition_timeout=1.0,
            port_timeout=1.0,
            port_poll_sec=0.1,
            minimum_power_off_sec=2.0,
            source_git=SOURCE_GIT,
            port="COM12",
            expected_d1l_public_key=PUBLIC_KEY,
            platform_name="nt",
        )

    matrix = json.loads(out.read_text(encoding="ascii"))
    cycle_path = out.parent / "matrix_cycles" / "software_1.json"
    cycle = json.loads(cycle_path.read_text(encoding="ascii"))
    assert matrix["physical_observed"] is True
    assert matrix["physical_state_outcome_uncertain"] is True
    assert matrix["reboot_or_power_action_may_have_executed"] is True
    assert cycle["physical_observed"] is True
    assert cycle["physical_state_outcome_uncertain"] is True
    assert cycle["mutation_outcome_uncertain"] is True
    assert cycle["partial_command_receipts"][0]["write_attempted"] is True


def test_verify_producer_validates_before_final_matrix_write(
    tmp_path, monkeypatch
):
    seed_path, flash_path = valid_verify_inputs(tmp_path)
    out = tmp_path / "successful-verify" / "matrix.json"

    class SerialContext:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def reset_input_buffer(self):
            return None

    monkeypatch.setattr(
        reboot, "_open_serial", lambda *_args: SerialContext()
    )
    monkeypatch.setattr(
        reboot,
        "capture_state",
        lambda *_args, **_kwargs: full_state_capture(
            3000, 10000, "SW", 200
        ),
    )
    monkeypatch.setattr(
        reboot,
        "read_raw_command",
        lambda _ser, command, *_args, **_kwargs: command_receipt(
            command, identity_status()
        ),
    )

    def completed_cycle(*, report, **_kwargs):
        return cycle_receipt(
            report["cycle_type"],
            report["ordinal"],
            report["matrix_id"],
            report["seed_receipt_sha256"],
            report["closing_flash_receipt_sha256"],
            report["previous_receipt_sha256"],
        )

    monkeypatch.setattr(reboot, "run_software_cycle", completed_cycle)
    monkeypatch.setattr(reboot, "run_cold_cycle", completed_cycle)

    report = reboot.verify_reboot_matrix(
        root=tmp_path,
        out=out,
        seed_path=seed_path,
        flash_path=flash_path,
        serial_module=object(),
        port_lister=valid_port_lister,
        prompt=lambda _text: "",
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        timeout=1.0,
        transition_timeout=1.0,
        port_timeout=1.0,
        port_poll_sec=0.1,
        minimum_power_off_sec=2.0,
        source_git=SOURCE_GIT,
        port="COM12",
        expected_d1l_public_key=PUBLIC_KEY,
        platform_name="nt",
    )

    assert report["ok"] is True
    assert len(report["cycle_receipts"]) == 8
    validated, errors, written = (
        reboot.validate_core_reboot_persistence_receipt(
            out,
            root=tmp_path,
            expected_commit=COMMIT,
            expected_run_id=RUN_ID,
            expected_run_attempt=RUN_ATTEMPT,
        )
    )
    assert validated is True, errors
    assert errors == []
    assert written == report
