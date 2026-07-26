#!/usr/bin/env python3
"""Fail-closed physical SD remove/reinsert acceptance for MeshCore DeskOS D1L.

The runner performs one bounded no-soak cycle.  Each physical card action needs
an explicit, timed operator acknowledgement.  It never opens the RP2040 USB
port or UF2 volume, formats the card, erases NVS, or sends Public or DM RF
traffic.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import select
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

try:
    from artifact_metadata import stamp_report
    from smoke_d1l import (
        exact_commit,
        firmware_identity_matches,
        health_boot_nonce,
        open_d1l_serial,
        send_console_command,
    )
    from sd_export_canary_d1l import export_canary_passed
    from sd_file_canary_d1l import filecanary_passed
    from sd_map_tile_canary_d1l import map_tile_canary_passed
    from sd_retained_history_acceptance_d1l import (
        entry_matches,
        exact_page_counts,
        fingerprint_for_token,
        positive_int,
        response_entries,
        retained_canary_hash,
        retained_readback_commands,
    )
except ImportError:  # pragma: no cover - package import path used by pytest
    from scripts.artifact_metadata import stamp_report
    from scripts.smoke_d1l import (
        exact_commit,
        firmware_identity_matches,
        health_boot_nonce,
        open_d1l_serial,
        send_console_command,
    )
    from scripts.sd_export_canary_d1l import export_canary_passed
    from scripts.sd_file_canary_d1l import filecanary_passed
    from scripts.sd_map_tile_canary_d1l import map_tile_canary_passed
    from scripts.sd_retained_history_acceptance_d1l import (
        entry_matches,
        exact_page_counts,
        fingerprint_for_token,
        positive_int,
        response_entries,
        retained_canary_hash,
        retained_readback_commands,
    )


KIND = "d1l_sd_remove_reinsert_source"
REQUIRED_CYCLES = 1
STORE_NAMES = ("messages", "dm", "routes", "packets")
HISTORY_BACKEND_FIELDS = (
    "message_store_backend",
    "dm_store_backend",
    "route_store_backend",
    "packet_log_backend",
    "node_store_backend",
    "contact_store_backend",
    "read_state_backend",
)
SD_MODE = "sd"
VOLATILE_NO_CARD_MODE = "volatile_no_card"
LIVE_ONLY_EVIDENCE_MODE = "live_only_no_card"
EXPECTED_RELEASE_PROFILE = "core_1_0"
EXPECTED_SD_HISTORY_MODE = "conditional"
TOKEN_RE = re.compile(r"^[A-Za-z0-9_.-]{1,15}$")
RETAINED_CANARY_TIMEOUT_SEC = 180.0
SD_OPERATION_TIMEOUT_SEC = 120.0
PERSISTENCE_POLL_ATTEMPTS = 30
PERSISTENCE_POLL_INTERVAL_SEC = 1.0
STATE_POLL_INTERVAL_SEC = 1.0
STATE_WAIT_SEC = 120.0
OPERATOR_ACK_TIMEOUT_SEC = 120.0
MAX_OPERATOR_ACK_TIMEOUT_SEC = 300.0
OPERATOR_ACK_TOKENS = {
    "remove": "removed",
    "reinsert": "reinserted",
}


def cycle_token(base: str, cycle: int, phase: str) -> str:
    suffix = {"before": "pre", "absent": "nvs", "after": "post"}[phase]
    token = f"{base[:15]}-{cycle:02d}-{suffix}"
    if len(token) > 31:
        raise ValueError("generated retained-canary token exceeds firmware limit")
    return token


def persistence_commands(token: str) -> list[str]:
    fingerprint = fingerprint_for_token(token)
    return [
        f"messages public search {token}",
        f"messages dm {fingerprint}",
        "routes",
        f"packets search {token}",
        "storage status",
    ]


def _zero(value: object) -> bool:
    return type(value) is int and value == 0


def _positive(value: object) -> int | None:
    return positive_int(value)


def _unexpected_restart(result: dict) -> bool:
    return result.get("cmd") == "help" or result.get("ignored_boot_help_seen") is True


def _terminal_result(result: dict) -> bool:
    return result.get("code") == "TIMEOUT" or _unexpected_restart(result)


def storage_mode_matches(result: dict | None, mode: str) -> bool:
    if not isinstance(result, dict):
        return False
    manager = result.get("manager")
    sd = result.get("sd")
    expected_backend = "sd" if mode == SD_MODE else "volatile"
    common = (
        result.get("ok") is True
        and result.get("cmd") == "storage status"
        and isinstance(manager, dict)
        and manager.get("running") is True
        and _positive(manager.get("attempt")) is not None
        and isinstance(sd, dict)
        and sd.get("rp2040_bridge_ready") is True
        and sd.get("rp2040_protocol_supported") is True
        and sd.get("status_stale") is False
        and sd.get("presence_stale") is False
        and all(
            result.get(field) == expected_backend
            for field in HISTORY_BACKEND_FIELDS
        )
    )
    if not common:
        return False
    if mode == SD_MODE:
        return (
            manager.get("state") == "READY_SD"
            and sd.get("state") == "ready"
            and sd.get("present") is True
            and sd.get("mounted") is True
            and sd.get("data_root_ready") is True
            and sd.get("file_ops") is True
            and sd.get("atomic_rename") is True
        )
    return (
        mode == VOLATILE_NO_CARD_MODE
        and manager.get("state") == "NO_CARD"
        and sd.get("state") == "no_card"
        and sd.get("present") is False
        and sd.get("mounted") is False
        and sd.get("data_root_ready") is False
        and result.get("data_backend") == "volatile"
        and result.get("live_mesh_without_sd") == [
            "rf_tx",
            "rf_rx",
            "chat",
        ]
        and result.get("setup_required") is True
        and result.get("setup_action") == "insert_card"
        and isinstance(result.get("note"), str)
        and "history is not saved" in result["note"].lower()
    )


def storage_health_clean(result: dict | None, mode: str) -> bool:
    if not storage_mode_matches(result, mode):
        return False
    retained = result.get("retained_sd")
    stores = retained.get("stores") if isinstance(retained, dict) else None
    if (
        not isinstance(stores, dict)
        or retained.get("degraded") is not False
        or retained.get("backup_degraded") is not False
    ):
        return False
    for name in STORE_NAMES:
        store = stores.get(name)
        if not isinstance(store, dict):
            return False
        if any(not _zero(store.get(field)) for field in (
            "sd_read_fail_count", "sd_write_fail_count", "sd_rename_fail_count",
            "nvs_mirror_fail_count",
        )):
            return False
        if (
            store.get("sd_last_error") != "ESP_OK"
            or store.get("nvs_mirror_last_error") != "ESP_OK"
            or store.get("sd_degraded_latched") is not False
        ):
            return False
    return True


def retained_canary_metadata(
    result: dict | None, token: str, mode: str
) -> dict | None:
    if not isinstance(result, dict):
        return None
    expected_backend = "sd" if mode == SD_MODE else "volatile"
    expected_mode = SD_MODE if mode == SD_MODE else VOLATILE_NO_CARD_MODE
    history_persisted = mode == SD_MODE
    backends = result.get("backends")
    generations = result.get("backend_generations")
    if (
        result.get("ok") is not True
        or result.get("cmd") != "storage retained-canary"
        or result.get("token") != token
        or result.get("fingerprint") != fingerprint_for_token(token)
        or result.get("backend_mode") != expected_mode
        or result.get("history_persisted") is not history_persisted
        or result.get("storage_manager_quiesced") is not True
        or result.get("retained_worker_quiesced") is not True
        or result.get("public_rf_tx") is not False
        or result.get("dm_rf_tx") is not False
        or result.get("formats_sd") is not False
        or not isinstance(backends, dict)
        or any(backends.get(name) != expected_backend for name in STORE_NAMES)
        or not isinstance(generations, dict)
        or any(_positive(generations.get(name)) is None for name in STORE_NAMES)
    ):
        return None
    sequences = {
        name: _positive(result.get(f"{name}_seq"))
        for name in ("public", "dm", "route", "packet")
    }
    if any(value is None for value in sequences.values()):
        return None
    return {
        "token": token,
        "fingerprint": fingerprint_for_token(token),
        "sequences": sequences,
        "generations": {name: generations[name] for name in STORE_NAMES},
    }


def retained_readbacks_pass(
    results: dict[str, dict], token: str, canary: dict | None, mode: str
) -> bool:
    metadata = retained_canary_metadata(canary, token, mode)
    if metadata is None:
        return False
    fingerprint = metadata["fingerprint"]
    sequences = metadata["sequences"]
    text = f"sd-retained-canary {token}"

    public = results.get(f"messages public search {token}", {})
    public_entries = response_entries(public)
    public_ok = (
        public.get("ok") is True
        and public.get("cmd") == "messages public"
        and public.get("filtered") is True
        and public.get("search") == token
        and exact_page_counts(public, public_entries)
        and entry_matches(public_entries, sequences["public"], {
            "direction": "tx", "author": "SD Canary", "text": text,
            "delivered": True,
        })
    )
    dm = results.get(f"messages dm {fingerprint}", {})
    dm_entries = response_entries(dm)
    dm_ok = (
        dm.get("ok") is True
        and dm.get("cmd") == "messages dm"
        and dm.get("filtered") is True
        and dm.get("fingerprint") == fingerprint
        and exact_page_counts(dm, dm_entries, thread=True)
        and entry_matches(dm_entries, sequences["dm"], {
            "fingerprint": fingerprint, "alias": "SD Canary", "direction": "tx",
            "text": text, "delivered": True, "acked": True,
            "ack_hash": retained_canary_hash(token),
        })
    )
    route = results.get(f"routes trace {fingerprint}", {})
    route_entries = response_entries(route)
    route_ok = (
        route.get("ok") is True
        and route.get("cmd") == "routes trace"
        and route.get("fingerprint") == fingerprint
        and _positive(route.get("route_count")) == len(route_entries)
        and entry_matches(route_entries, sequences["route"], {
            "target": fingerprint, "label": "SD Canary", "kind": "sd_canary",
            "route": "local", "direction": "tx", "payload_len": len(text),
        })
    )
    packet = results.get(f"packets search {token}", {})
    packet_entries = response_entries(packet)
    packet_ok = (
        packet.get("ok") is True
        and packet.get("cmd") == "packets search"
        and packet.get("filter") == {
            "direction": "any", "kind": "any", "search": token,
        }
        and entry_matches(packet_entries, sequences["packet"], {
            "direction": "tx", "kind": "sd_canary", "payload_len": len(text),
            "note": f"sd-canary {token}",
        })
    )
    return public_ok and dm_ok and route_ok and packet_ok


def _message_persistence_clean(result: dict | None, cmd: str, sd_required: bool) -> bool:
    if not isinstance(result, dict):
        return False
    persistence = result.get("persistence")
    sd = persistence.get("sd") if isinstance(persistence, dict) else None
    nvs = persistence.get("nvs") if isinstance(persistence, dict) else None
    deferred_sd_sync = not sd_required
    return (
        result.get("ok") is True and result.get("cmd") == cmd
        and result.get("persisted") is True and isinstance(persistence, dict)
        and persistence.get("loaded") is True and persistence.get("dirty") is False
        and _zero(persistence.get("failures")) and isinstance(sd, dict)
        and sd.get("required") is sd_required
        and sd.get("dirty") is deferred_sd_sync
        and sd.get("reconcile_pending") is deferred_sd_sync
        and _zero(sd.get("failures"))
        and sd.get("last_error") == "ESP_OK" and isinstance(nvs, dict)
        and nvs.get("dirty") is False and _zero(nvs.get("failures"))
        and nvs.get("last_error") == "ESP_OK"
    )


def persistence_generations(results: dict[str, dict]) -> dict[str, int] | None:
    paths = {
        "messages": (next((v for k, v in results.items() if k.startswith("messages public search ")), {}), "sd", "generation"),
        "dm": (next((v for k, v in results.items() if k.startswith("messages dm ")), {}), "sd", "generation"),
        "routes": (results.get("routes", {}), "sd_primary", "backend_generation"),
        "packets": (next((v for k, v in results.items() if k.startswith("packets search ")), {}), "sd", "generation"),
    }
    generations: dict[str, int] = {}
    for name, (result, section, field) in paths.items():
        persistence = result.get("persistence") if isinstance(result, dict) else None
        backend = persistence.get(section) if isinstance(persistence, dict) else None
        generation = _positive(backend.get(field) if isinstance(backend, dict) else None)
        if generation is None:
            return None
        generations[name] = generation
    return generations


def persistence_snapshot_clean(results: dict[str, dict], token: str, mode: str) -> bool:
    if mode != SD_MODE:
        return False
    sd_required = True
    deferred_sd_sync = False
    fingerprint = fingerprint_for_token(token)
    route = results.get("routes")
    route_persistence = route.get("persistence") if isinstance(route, dict) else None
    route_sd = route_persistence.get("sd_primary") if isinstance(route_persistence, dict) else None
    route_nvs = route_persistence.get("nvs_fallback") if isinstance(route_persistence, dict) else None
    route_ok = (
        isinstance(route, dict) and route.get("ok") is True and route.get("cmd") == "routes"
        and route.get("persisted") is True and isinstance(route_persistence, dict)
        and route_persistence.get("dirty") is False and _zero(route_persistence.get("fail_count"))
        and route_persistence.get("clear_failure_latched") is False
        and _zero(route_persistence.get("clear_fail_count"))
        and route_persistence.get("clear_last_error") == "ESP_OK"
        and isinstance(route_sd, dict) and route_sd.get("required") is sd_required
        and route_sd.get("dirty") is deferred_sd_sync
        and route_sd.get("reconcile_pending") is deferred_sd_sync
        and _zero(route_sd.get("fail_count")) and route_sd.get("last_error") == "ESP_OK"
        and isinstance(route_nvs, dict) and route_nvs.get("dirty") is False
        and _zero(route_nvs.get("fail_count")) and route_nvs.get("last_error") == "ESP_OK"
    )
    packet = results.get(f"packets search {token}")
    pp = packet.get("persistence") if isinstance(packet, dict) else None
    psd = pp.get("sd") if isinstance(pp, dict) else None
    pnvs = pp.get("nvs") if isinstance(pp, dict) else None
    journal = pp.get("journal") if isinstance(pp, dict) else None
    reconcile = pp.get("reconcile") if isinstance(pp, dict) else None
    packet_ok = (
        isinstance(packet, dict) and packet.get("ok") is True
        and packet.get("cmd") == "packets search" and packet.get("persisted") is True
        and isinstance(pp, dict) and pp.get("loaded") is True and pp.get("dirty") is False
        and _zero(pp.get("failures")) and isinstance(reconcile, dict)
        and reconcile.get("pending") is False and _zero(reconcile.get("failures"))
        and reconcile.get("last_error") == "ESP_OK" and isinstance(psd, dict)
        and psd.get("required") is sd_required and psd.get("dirty") is False
        and _zero(psd.get("failures")) and psd.get("last_error") == "ESP_OK"
        and isinstance(pnvs, dict) and pnvs.get("dirty") is False
        and _zero(pnvs.get("failures")) and pnvs.get("last_error") == "ESP_OK"
        and isinstance(journal, dict) and journal.get("dirty") is False
        and _zero(journal.get("failures")) and journal.get("last_error") == "ESP_OK"
    )
    return (
        _message_persistence_clean(results.get(f"messages public search {token}"), "messages public", sd_required)
        and _message_persistence_clean(results.get(f"messages dm {fingerprint}"), "messages dm", sd_required)
        and route_ok and packet_ok and storage_health_clean(results.get("storage status"), mode)
        and persistence_generations(results) is not None
    )


class SessionAbort(RuntimeError):
    def __init__(self, code: str, phase: str, detail: str = ""):
        super().__init__(detail or code)
        self.code = code
        self.phase = phase
        self.detail = detail or code


def read_operator_ack(action: str, timeout_sec: float) -> str | None:
    """Read one bounded acknowledgement from the Pi terminal."""
    try:
        ready, _, _ = select.select(
            [sys.stdin],
            [],
            [],
            timeout_sec,
        )
    except (OSError, TypeError, ValueError):
        return None
    if not ready:
        return None
    value = sys.stdin.readline()
    return value.strip().lower() if value else None


class EventSession:
    def __init__(
        self,
        ser,
        timeout: float,
        events: list[dict],
        *,
        operator_ack_timeout_sec: float,
        operator_ack_reader: Callable[[str, float], str | None],
    ):
        self.ser = ser
        self.timeout = timeout
        self.events = events
        self.operator_ack_timeout_sec = operator_ack_timeout_sec
        self.operator_ack_reader = operator_ack_reader

    def prompt(self, cycle: int, phase: str, action: str, message: str) -> None:
        expected = OPERATOR_ACK_TOKENS[action]
        prompt = (
            f"{message}. Type {expected.upper()} and press Enter within "
            f"{self.operator_ack_timeout_sec:g} seconds"
        )
        print(prompt, flush=True)
        event = {
            "sequence": len(self.events) + 1, "kind": "prompt", "cycle": cycle,
            "phase": phase, "action": action, "message": prompt,
            "expected_ack": expected,
            "ack_timeout_sec": self.operator_ack_timeout_sec,
            "acknowledged": False,
            "ack_result": "pending",
        }
        self.events.append(event)
        received = self.operator_ack_reader(
            action,
            self.operator_ack_timeout_sec,
        )
        normalized = received.strip().lower() if isinstance(received, str) else None
        if normalized is None:
            event["ack_result"] = "timeout"
            raise SessionAbort("operator_ack_timeout", phase, action)
        if normalized != expected:
            event["ack_result"] = "mismatch"
            raise SessionAbort("operator_ack_mismatch", phase, action)
        event["acknowledged"] = True
        event["ack_result"] = "accepted"

    def command(self, cycle: int, phase: str, command: str, timeout: float | None = None) -> dict:
        result = send_console_command(self.ser, command, timeout or self.timeout)
        self.events.append({
            "sequence": len(self.events) + 1, "kind": "command", "cycle": cycle,
            "phase": phase, "command": command, "result": result,
        })
        if result.get("code") == "TIMEOUT":
            raise SessionAbort("command_timeout", phase, command)
        if _unexpected_restart(result):
            raise SessionAbort("unexpected_restart", phase, command)
        return result


def poll_stable_state(
    session: EventSession, cycle: int, phase: str, mode: str,
    *, wait_sec: float, interval_sec: float,
) -> list[dict]:
    deadline = time.monotonic() + max(0.0, wait_sec)
    stable: list[dict] = []
    while True:
        result = session.command(cycle, phase, "storage status")
        if storage_mode_matches(result, mode):
            attempt = result["manager"]["attempt"]
            if not stable or stable[-1]["manager"]["attempt"] != attempt:
                stable.append(result)
            if len(stable) >= 2:
                return stable[-2:]
        else:
            stable.clear()
        if time.monotonic() >= deadline:
            raise SessionAbort("state_timeout", phase, mode)
        time.sleep(max(0.0, interval_sec))


def run_readbacks(session: EventSession, cycle: int, phase: str, token: str) -> dict[str, dict]:
    return {
        command: session.command(cycle, phase, command)
        for command in retained_readback_commands(token)
    }


def run_persistence_poll(
    session: EventSession, cycle: int, phase: str, token: str, mode: str,
    *, attempts: int, interval_sec: float,
) -> tuple[dict[str, dict], int]:
    latest: dict[str, dict] = {}
    for attempt in range(max(1, attempts)):
        latest = {
            command: session.command(cycle, phase, command)
            for command in persistence_commands(token)
        }
        if persistence_snapshot_clean(latest, token, mode):
            return latest, attempt + 1
        if attempt + 1 < max(1, attempts):
            time.sleep(max(0.0, interval_sec))
    raise SessionAbort("persistence_not_clean", phase, token)


def run_canary_bundle(
    session: EventSession, cycle: int, phase: str, token: str, mode: str,
    *, persistence_attempts: int, persistence_interval_sec: float,
) -> dict:
    canary = session.command(
        cycle, f"{phase}_canary", f"storage retained-canary {token}",
        RETAINED_CANARY_TIMEOUT_SEC,
    )
    if retained_canary_metadata(canary, token, mode) is None:
        raise SessionAbort("retained_canary_failed", f"{phase}_canary", token)
    readbacks = run_readbacks(session, cycle, f"{phase}_readback", token)
    if not retained_readbacks_pass(readbacks, token, canary, mode):
        raise SessionAbort("retained_readback_failed", f"{phase}_readback", token)
    metadata = retained_canary_metadata(canary, token, mode)
    if metadata is None:
        raise SessionAbort("retained_canary_failed", f"{phase}_canary", token)
    if mode == SD_MODE:
        persistence, attempts_used = run_persistence_poll(
            session, cycle, f"{phase}_persistence", token, mode,
            attempts=persistence_attempts,
            interval_sec=persistence_interval_sec,
        )
        generations = persistence_generations(persistence)
        evidence_mode = SD_MODE
    else:
        persistence = None
        attempts_used = 0
        generations = metadata["generations"]
        evidence_mode = LIVE_ONLY_EVIDENCE_MODE
    return {
        "token": token,
        "mode": evidence_mode,
        "storage_mode": mode,
        "canary": canary,
        "readbacks": readbacks,
        "persistence": persistence,
        "persistence_attempts_used": attempts_used,
        "generations": generations,
        "ok": True,
    }


def generations_changed(before: dict | None, after: dict | None) -> bool:
    return (
        isinstance(before, dict) and isinstance(after, dict)
        and set(before) == set(STORE_NAMES) and set(after) == set(STORE_NAMES)
        and all(_positive(before[name]) is not None and _positive(after[name]) is not None
                and before[name] != after[name] for name in STORE_NAMES)
    )


def crashlog_unchanged(before: dict | None, after: dict | None) -> bool:
    return (
        isinstance(before, dict) and isinstance(after, dict)
        and before.get("ok") is True and before.get("cmd") == "crashlog"
        and after.get("ok") is True and after.get("cmd") == "crashlog"
        and type(before.get("total_written")) is int
        and after.get("total_written") == before.get("total_written")
    )


def degraded_notice_visible(
    no_card_samples: object,
    ui_probe: object,
) -> bool:
    if (
        not isinstance(no_card_samples, list)
        or len(no_card_samples) != 2
        or not all(
            storage_mode_matches(sample, VOLATILE_NO_CARD_MODE)
            for sample in no_card_samples
        )
        or not isinstance(ui_probe, dict)
    ):
        return False
    return (
        ui_probe.get("ok") is True
        and ui_probe.get("cmd") == "ui scroll-probe"
        and ui_probe.get("surface") == "storage"
        and ui_probe.get("tab") == "settings"
        and ui_probe.get("surface_supported") is True
        and ui_probe.get("target_found") is True
    )


def run_cycle(
    session: EventSession, cycle: int, base_token: str,
    initial_nonce: int, initial_crashlog: dict,
    *, state_wait_sec: float, state_interval_sec: float,
    persistence_attempts: int, persistence_interval_sec: float,
) -> dict:
    tokens = {
        phase: cycle_token(base_token, cycle, phase)
        for phase in ("before", "absent", "after")
    }
    baseline = poll_stable_state(
        session, cycle, "baseline_ready_poll", SD_MODE,
        wait_sec=state_wait_sec, interval_sec=state_interval_sec,
    )
    before = run_canary_bundle(
        session, cycle, "before_remove", tokens["before"], SD_MODE,
        persistence_attempts=persistence_attempts,
        persistence_interval_sec=persistence_interval_sec,
    )

    session.prompt(cycle, "prompt_remove", "remove", f"Cycle {cycle}/{REQUIRED_CYCLES}: REMOVE SD NOW")
    no_card = poll_stable_state(
        session, cycle, "no_card_poll", VOLATILE_NO_CARD_MODE,
        wait_sec=state_wait_sec, interval_sec=state_interval_sec,
    )
    notice_probe = session.command(
        cycle,
        "degraded_notice_probe",
        "ui scroll-probe storage",
        max(session.timeout, 15.0),
    )
    absent = run_canary_bundle(
        session,
        cycle,
        "absent",
        tokens["absent"],
        VOLATILE_NO_CARD_MODE,
        persistence_attempts=persistence_attempts,
        persistence_interval_sec=persistence_interval_sec,
    )
    absent["degraded_notice_visible"] = degraded_notice_visible(
        no_card,
        notice_probe,
    )
    absent["degraded_notice"] = {
        "storage_status": no_card[-1],
        "ui_probe": notice_probe,
        "live_mesh_without_sd": no_card[-1].get(
            "live_mesh_without_sd"
        ),
    }
    if absent["degraded_notice_visible"] is not True:
        raise SessionAbort(
            "degraded_notice_not_visible",
            "degraded_notice_probe",
            str(cycle),
        )

    session.prompt(cycle, "prompt_reinsert", "reinsert", f"Cycle {cycle}/{REQUIRED_CYCLES}: REINSERT SD NOW")
    ready = poll_stable_state(
        session, cycle, "ready_poll", SD_MODE,
        wait_sec=state_wait_sec, interval_sec=state_interval_sec,
    )

    retained_before = run_readbacks(
        session,
        cycle,
        "post_reinsert_readback_before",
        tokens["before"],
    )
    if not retained_readbacks_pass(
        retained_before,
        tokens["before"],
        before["canary"],
        SD_MODE,
    ):
        raise SessionAbort(
            "post_reinsert_readback_failed",
            "post_reinsert_readback_before",
            str(cycle),
        )

    after = run_canary_bundle(
        session, cycle, "after_reinsert", tokens["after"], SD_MODE,
        persistence_attempts=persistence_attempts,
        persistence_interval_sec=persistence_interval_sec,
    )
    file_result = session.command(cycle, "post_file_canary", "storage filecanary", SD_OPERATION_TIMEOUT_SEC)
    map_result = session.command(
        cycle, "post_map_canary", f"storage map-tile-canary {tokens['after']}",
        SD_OPERATION_TIMEOUT_SEC,
    )
    export_result = session.command(
        cycle, "post_export_canary", f"storage export-canary {tokens['after']}",
        SD_OPERATION_TIMEOUT_SEC,
    )
    health = session.command(cycle, "post_health", "health")
    crashlog = session.command(cycle, "post_crashlog", "crashlog")
    post_checks_ok = (
        filecanary_passed(file_result)
        and map_tile_canary_passed(map_result, tokens["after"])
        and export_canary_passed(export_result, tokens["after"])
        and health.get("ok") is True
        and health_boot_nonce(health) == initial_nonce
        and crashlog_unchanged(initial_crashlog, crashlog)
    )
    if not post_checks_ok:
        raise SessionAbort("post_cycle_check_failed", "post_checks", str(cycle))
    return {
        "cycle": cycle, "tokens": tokens, "baseline_ready_samples": baseline,
        "before_remove": before, "no_card_samples": no_card, "absent": absent,
        "ready_samples": ready,
        "post_reinsert_readback_before": retained_before,
        "after_reinsert": after,
        "post_canaries": {"file": file_result, "map": map_result, "export": export_result},
        "health": health, "crashlog": crashlog, "ok": True,
    }


def _allowed_command(command: str) -> bool:
    exact = {"version", "health", "crashlog", "storage status", "storage filecanary", "routes"}
    prefixes = (
        "storage retained-canary ", "messages public search ", "messages dm ",
        "routes trace ", "packets search ", "storage map-tile-canary ",
        "storage export-canary ", "ui scroll-probe ",
    )
    return command in exact or command.startswith(prefixes)


def _stable_samples(samples: object, mode: str) -> bool:
    return (
        isinstance(samples, list) and len(samples) == 2
        and all(storage_mode_matches(sample, mode) for sample in samples)
        and samples[0]["manager"]["attempt"] != samples[1]["manager"]["attempt"]
    )


def validate_report(report: dict, *, require_final: bool = True) -> bool:
    expected_commit = exact_commit(report.get("expected_firmware_commit"))
    port = report.get("port")
    git = report.get("git")
    version = report.get("version")
    ack_timeout = report.get("operator_ack_timeout_sec")
    if (
        report.get("schema") != 1 or report.get("kind") != KIND
        or report.get("mode") != "hardware" or expected_commit is None
        or not firmware_identity_matches(version, expected_commit)
        or not isinstance(version, dict)
        or version.get("release_profile") != EXPECTED_RELEASE_PROFILE
        or version.get("sd_history_mode") != EXPECTED_SD_HISTORY_MODE
        or report.get("firmware_identity") is not True
        or not isinstance(port, str) or not port.strip()
        or port.strip().upper() in {f"COM{number}" for number in (8, 11, 29)}
        or type(report.get("baud")) is not int or report.get("baud") <= 0
        or report.get("strict_evidence") is not True
        or report.get("physical_observed") is not True
        or report.get("dry_run") is not False
        or report.get("simulated") is not False
        or report.get("manual_only") is not False
        or report.get("operator_ack_required") is not True
        or not isinstance(ack_timeout, (int, float))
        or isinstance(ack_timeout, bool)
        or ack_timeout <= 0
        or ack_timeout > MAX_OPERATOR_ACK_TIMEOUT_SEC
        or report.get("cycles_required") != REQUIRED_CYCLES
        or report.get("cycles_completed") != REQUIRED_CYCLES
        or report.get("public_rf_tx") is not False or report.get("dm_rf_tx") is not False
        or report.get("formats_sd") is not False
        or report.get("erases_nvs") is not False
    ):
        return False
    if require_final and (
        report.get("ok") is not True
        or not isinstance(git, dict)
        or git.get("dirty") is not False
        or exact_commit(git.get("commit")) != expected_commit
    ):
        return False
    cycles = report.get("cycles")
    events = report.get("events")
    if not isinstance(cycles, list) or len(cycles) != REQUIRED_CYCLES or not isinstance(events, list):
        return False
    if any(event.get("sequence") != index for index, event in enumerate(events, 1)):
        return False
    for event in events:
        if event.get("kind") == "command":
            command = event.get("command")
            result = event.get("result")
            if not isinstance(command, str) or not _allowed_command(command) or not isinstance(result, dict):
                return False
            if result.get("ok") is not True or _terminal_result(result) or result.get("public_rf_tx") is True or result.get("dm_rf_tx") is True or result.get("formats_sd") is True or result.get("format_performed") is True:
                return False
        elif event.get("kind") == "prompt":
            action = event.get("action")
            if (
                action not in OPERATOR_ACK_TOKENS
                or event.get("expected_ack") != OPERATOR_ACK_TOKENS[action]
                or event.get("ack_timeout_sec") != ack_timeout
                or event.get("acknowledged") is not True
                or event.get("ack_result") != "accepted"
            ):
                return False
        else:
            return False

    seen_tokens: set[str] = set()
    initial_nonce = report.get("initial_boot_nonce")
    initial_crashlog = report.get("initial_crashlog")
    if _positive(initial_nonce) is None:
        return False
    for index, cycle in enumerate(cycles, 1):
        if not isinstance(cycle, dict) or cycle.get("cycle") != index or cycle.get("ok") is not True:
            return False
        tokens = cycle.get("tokens")
        if not isinstance(tokens, dict) or set(tokens) != {"before", "absent", "after"}:
            return False
        values = list(tokens.values())
        if any(not isinstance(token, str) or token in seen_tokens for token in values):
            return False
        seen_tokens.update(values)
        before = cycle.get("before_remove")
        absent = cycle.get("absent")
        after = cycle.get("after_reinsert")
        if not (_stable_samples(cycle.get("baseline_ready_samples"), "sd")
                and _stable_samples(
                    cycle.get("no_card_samples"),
                    VOLATILE_NO_CARD_MODE,
                )
                and _stable_samples(cycle.get("ready_samples"), "sd")):
            return False
        for bundle, token in (
            (before, tokens["before"]),
            (after, tokens["after"]),
        ):
            if (
                not isinstance(bundle, dict) or bundle.get("ok") is not True
                or bundle.get("token") != token
                or bundle.get("mode") != SD_MODE
                or bundle.get("storage_mode") != SD_MODE
                or retained_canary_metadata(
                    bundle.get("canary"), token, SD_MODE
                )
                is None
                or not retained_readbacks_pass(
                    bundle.get("readbacks", {}),
                    token,
                    bundle.get("canary"),
                    SD_MODE,
                )
                or not persistence_snapshot_clean(
                    bundle.get("persistence", {}),
                    token,
                    SD_MODE,
                )
                or bundle.get("generations") != persistence_generations(bundle.get("persistence", {}))
            ):
                return False
        if (
            not isinstance(absent, dict)
            or absent.get("ok") is not True
            or absent.get("token") != tokens["absent"]
            or absent.get("mode") != LIVE_ONLY_EVIDENCE_MODE
            or absent.get("storage_mode") != VOLATILE_NO_CARD_MODE
            or absent.get("persistence") is not None
            or absent.get("persistence_attempts_used") != 0
            or retained_canary_metadata(
                absent.get("canary"),
                tokens["absent"],
                VOLATILE_NO_CARD_MODE,
            )
            is None
            or not retained_readbacks_pass(
                absent.get("readbacks", {}),
                tokens["absent"],
                absent.get("canary"),
                VOLATILE_NO_CARD_MODE,
            )
            or absent.get("degraded_notice_visible") is not True
            or not isinstance(absent.get("degraded_notice"), dict)
            or not degraded_notice_visible(
                cycle.get("no_card_samples"),
                absent["degraded_notice"].get("ui_probe"),
            )
            or absent["degraded_notice"].get("storage_status")
            != cycle["no_card_samples"][-1]
            or absent["degraded_notice"].get("live_mesh_without_sd")
            != ["rf_tx", "rf_rx", "chat"]
            or not retained_readbacks_pass(
                cycle.get("post_reinsert_readback_before", {}),
                tokens["before"],
                before.get("canary"),
                SD_MODE,
            )
        ):
            return False
        post = cycle.get("post_canaries")
        if (
            not isinstance(post, dict) or not filecanary_passed(post.get("file", {}))
            or not map_tile_canary_passed(post.get("map", {}), tokens["after"])
            or not export_canary_passed(post.get("export", {}), tokens["after"])
            or health_boot_nonce(cycle.get("health")) != initial_nonce
            or not crashlog_unchanged(initial_crashlog, cycle.get("crashlog"))
        ):
            return False
        cycle_events = [event for event in events if event.get("cycle") == index]
        phases = [event.get("phase") for event in cycle_events]
        required_order = [
            "before_remove_canary", "prompt_remove", "no_card_poll",
            "degraded_notice_probe", "absent_canary", "prompt_reinsert",
            "ready_poll",
            "post_reinsert_readback_before", "after_reinsert_canary",
            "post_file_canary", "post_map_canary", "post_export_canary", "post_health",
            "post_crashlog",
        ]
        try:
            positions = [phases.index(phase) for phase in required_order]
        except ValueError:
            return False
        if positions != sorted(positions):
            return False
    return True


def run_acceptance(
    port: str, baud: int, timeout: float, expected_firmware_commit: str, base_token: str,
    *, state_wait_sec: float = STATE_WAIT_SEC,
    state_interval_sec: float = STATE_POLL_INTERVAL_SEC,
    persistence_attempts: int = PERSISTENCE_POLL_ATTEMPTS,
    persistence_interval_sec: float = PERSISTENCE_POLL_INTERVAL_SEC,
    operator_ack_timeout_sec: float = OPERATOR_ACK_TIMEOUT_SEC,
    operator_ack_reader: Callable[
        [str, float], str | None
    ] = read_operator_ack,
) -> dict:
    if (
        not isinstance(operator_ack_timeout_sec, (int, float))
        or isinstance(operator_ack_timeout_sec, bool)
        or operator_ack_timeout_sec <= 0
        or operator_ack_timeout_sec > MAX_OPERATOR_ACK_TIMEOUT_SEC
    ):
        raise ValueError("operator acknowledgement timeout is out of bounds")
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("pyserial is required: python -m pip install pyserial") from exc

    events: list[dict] = []
    cycles: list[dict] = []
    report = {
        "schema": 1, "kind": KIND, "mode": "hardware", "port": port, "baud": baud,
        "expected_firmware_commit": expected_firmware_commit,
        "cycles_required": REQUIRED_CYCLES, "cycles_completed": 0,
        "strict_evidence": True,
        "physical_observed": True,
        "dry_run": False,
        "simulated": False,
        "manual_only": False,
        "operator_ack_required": True,
        "operator_ack_timeout_sec": operator_ack_timeout_sec,
        "public_rf_tx": False, "dm_rf_tx": False, "formats_sd": False,
        "erases_nvs": False,
        "events": events, "cycles": cycles, "ok": False,
    }
    with open_d1l_serial(serial, port=port, baudrate=baud, timeout=timeout) as ser:
        ser.reset_input_buffer()
        session = EventSession(
            ser,
            timeout,
            events,
            operator_ack_timeout_sec=operator_ack_timeout_sec,
            operator_ack_reader=operator_ack_reader,
        )
        try:
            version = session.command(0, "initial_version", "version")
            health = session.command(0, "initial_health", "health")
            crashlog = session.command(0, "initial_crashlog", "crashlog")
            nonce = health_boot_nonce(health)
            report.update({
                "version": version, "initial_health": health, "initial_crashlog": crashlog,
                "initial_boot_nonce": nonce,
                "firmware_identity": (
                    firmware_identity_matches(
                        version,
                        expected_firmware_commit,
                    )
                    and version.get("release_profile")
                    == EXPECTED_RELEASE_PROFILE
                    and version.get("sd_history_mode")
                    == EXPECTED_SD_HISTORY_MODE
                ),
            })
            if report["firmware_identity"] is not True or nonce is None or crashlog.get("ok") is not True:
                raise SessionAbort("initial_identity_or_health_failed", "initial", "version/health/crashlog")
            for cycle in range(1, REQUIRED_CYCLES + 1):
                cycles.append(run_cycle(
                    session, cycle, base_token, nonce, crashlog,
                    state_wait_sec=state_wait_sec, state_interval_sec=state_interval_sec,
                    persistence_attempts=persistence_attempts,
                    persistence_interval_sec=persistence_interval_sec,
                ))
                report["cycles_completed"] = len(cycles)
        except SessionAbort as exc:
            report["blocker"] = {
                "code": exc.code, "phase": exc.phase, "detail": exc.detail,
                "cycle": len(cycles) + 1,
                "last_event_sequence": len(events),
            }
    report["ok"] = validate_report(report, require_final=False)
    return report


def dry_run_report(
    expected_firmware_commit: str,
    base_token: str,
    *,
    strict_evidence: bool = False,
    operator_ack_timeout_sec: float = OPERATOR_ACK_TIMEOUT_SEC,
) -> dict:
    return {
        "schema": 1, "kind": KIND, "mode": "dry-run", "hardware_required": True,
        "physical_observed": False, "dry_run": True, "simulated": False,
        "manual_only": False,
        "expected_firmware_commit": expected_firmware_commit,
        "base_token": base_token, "cycles_required": REQUIRED_CYCLES,
        "strict_evidence": strict_evidence,
        "command_templates": [
            "version", "health", "crashlog", "storage status",
            "storage retained-canary <unique-token>",
            "messages public search <unique-token>",
            "messages dm <derived-fingerprint>", "routes trace <derived-fingerprint>",
            "packets search <unique-token>", "routes",
            "ui scroll-probe storage", "storage filecanary",
            "storage map-tile-canary <unique-token>",
            "storage export-canary <unique-token>",
        ],
        "operator_actions": [
            "REMOVE SD NOW; type REMOVED",
            "REINSERT SD NOW; type REINSERTED",
        ],
        "automatic_polling": True,
        "operator_ack_required": True,
        "operator_ack_timeout_sec": operator_ack_timeout_sec,
        "operator_ack_tokens": dict(OPERATOR_ACK_TOKENS),
        "waits_for_enter": True,
        "noninteractive": False,
        "rp2040_usb_used": False, "uf2_volume_used": False,
        "public_rf_tx": False, "dm_rf_tx": False, "formats_sd": False,
        "erases_nvs": False,
        "ok": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=os.environ.get("D1L_PORT"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--expected-firmware-commit", required=True)
    parser.add_argument("--token", default=datetime.now(timezone.utc).strftime("rr%y%m%d%H%M"))
    parser.add_argument("--state-wait-sec", type=float, default=STATE_WAIT_SEC)
    parser.add_argument("--state-poll-interval-sec", type=float, default=STATE_POLL_INTERVAL_SEC)
    parser.add_argument("--persistence-poll-attempts", type=int, default=PERSISTENCE_POLL_ATTEMPTS)
    parser.add_argument("--persistence-poll-interval-sec", type=float, default=PERSISTENCE_POLL_INTERVAL_SEC)
    parser.add_argument(
        "--operator-ack-timeout-sec",
        type=float,
        default=OPERATOR_ACK_TIMEOUT_SEC,
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--strict-evidence", action="store_true")
    parser.add_argument("--out")
    args = parser.parse_args()
    if exact_commit(args.expected_firmware_commit) is None:
        parser.error("--expected-firmware-commit must be a full 40-character SHA")
    if not TOKEN_RE.fullmatch(args.token):
        parser.error("--token must be 1-15 chars: A-Z a-z 0-9 _ . -")
    if not (
        0 < args.operator_ack_timeout_sec
        <= MAX_OPERATOR_ACK_TIMEOUT_SEC
    ):
        parser.error(
            "--operator-ack-timeout-sec must be greater than 0 and no "
            f"more than {MAX_OPERATOR_ACK_TIMEOUT_SEC:g}"
        )
    if args.dry_run:
        report = dry_run_report(
            args.expected_firmware_commit, args.token,
            strict_evidence=args.strict_evidence,
            operator_ack_timeout_sec=args.operator_ack_timeout_sec,
        )
    else:
        if not args.port:
            parser.error("No D1L port supplied. Set D1L_PORT or pass --port.")
        report = run_acceptance(
            args.port, args.baud, args.timeout, args.expected_firmware_commit, args.token,
            state_wait_sec=args.state_wait_sec,
            state_interval_sec=args.state_poll_interval_sec,
            persistence_attempts=args.persistence_poll_attempts,
            persistence_interval_sec=args.persistence_poll_interval_sec,
            operator_ack_timeout_sec=args.operator_ack_timeout_sec,
        )
    root = Path(__file__).resolve().parents[1]
    stamp_report(report, root)
    if report.get("mode") == "hardware":
        report["ok"] = report.get("ok") is True and validate_report(
            report, require_final=True
        )
    out_path = Path(args.out) if args.out else (
        root / "artifacts" / "sd-remove-reinsert" /
        f"d1l-sd-remove-reinsert-{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}.json"
    )
    if not out_path.is_absolute():
        out_path = root / out_path
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2), encoding="ascii")
    print(json.dumps(report))
    return 0 if report.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
