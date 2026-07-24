import json
import zlib
from pathlib import Path

import pytest

from scripts import core_release_gate_audit_d1l as audit
from scripts import core_ui_corruption_probe_d1l as core_ui
from scripts import manual_ui_review_d1l as review


COMMIT = "a" * 40
RUN_ID = "123456"
RUN_ATTEMPT = "1"
UTC_START = "2026-07-18T18:00:00Z"
UTC_END = "2026-07-18T18:01:00Z"


def clean_git() -> dict:
    return {
        "commit": COMMIT,
        "short_commit": COMMIT[:7],
        "branch": "release/24h-core",
        "dirty": False,
        "dirty_entries": [],
    }


def d1l_target() -> dict:
    return core_ui.resolve_core_target(
        "COM12",
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


def health() -> dict:
    return {
        "ok": True,
        "cmd": "health",
        "build_commit": COMMIT,
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "board_ready": True,
        "ui_ready": True,
    }


def crashlog() -> dict:
    return {"schema": 1, "ok": True, "cmd": "crashlog", "entries": []}


def scroll_probe_result(surface: str) -> dict:
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
        "tab": core_ui.CORE_SCROLL_TABS[surface],
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


def compose_probe_result(target: str) -> dict:
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


def core_ui_receipt() -> dict:
    checks = {name: True for name in review.REQUIRED_UI_CHECKS}
    events = []
    for round_number in range(1, review.RELEASE_MIN_ROUNDS + 1):
        for tab in review.CORE_TAB_SEQUENCE:
            events.append(
                {
                    "round": round_number,
                    "kind": "tab",
                    "tab": tab,
                    "request": {"ok": True},
                    "active": True,
                    "health": health(),
                    "crashlog": crashlog(),
                }
            )
        token = f"core_ui_test_{round_number}"
        events.append(
            {
                "round": round_number,
                "kind": "data_refresh",
                "token": token,
                "data_canary": {"ok": True},
                "packets_search": {
                    "ok": True,
                    "entries": [{"token": token}],
                },
                "messages_search": {
                    "ok": True,
                    "entries": [{"text": token}],
                },
                "health": health(),
                "crashlog": crashlog(),
            }
        )
    unavailable_plan = core_ui.unavailable_ui_probe_plan("disabled")
    unavailable_events = [
        {
            **probe,
            "before": {
                "schema": 1,
                "ok": True,
                "cmd": "ui status",
                "build_commit": COMMIT,
                "release_profile": "core_1_0",
                "sd_history_mode": "disabled",
                "active_tab": "home",
                "pending": False,
            },
            "result": {
                "schema": 1,
                "ok": False,
                "cmd": probe["command"],
                "code": "ESP_ERR_NOT_SUPPORTED",
                "release_profile": "core_1_0",
                "feature": probe["feature"],
            },
            "after": {
                "schema": 1,
                "ok": True,
                "cmd": "ui status",
                "build_commit": COMMIT,
                "release_profile": "core_1_0",
                "sd_history_mode": "disabled",
                "active_tab": "home",
                "pending": False,
            },
        }
        for probe in unavailable_plan
    ]
    return {
        "schema": 2,
        "kind": "core_ui_corruption_probe",
        "mode": "hardware",
        "ok": True,
        "closure_eligible": True,
        "hardware_required": True,
        "physical_observed": True,
        "identity_preflight_only": False,
        "port": "COM12",
        "d1l_target": d1l_target(),
        "started_at": UTC_START,
        "ended_at": UTC_END,
        "release_profile": "core_1_0",
        "sd_history_mode": "disabled",
        "expected_firmware_commit": COMMIT,
        "device_build_commit": COMMIT,
        "github_actions_run": RUN_ID,
        "workflow_run_attempt": RUN_ATTEMPT,
        "firmware_identity_required": True,
        "firmware_identity_ok": True,
        "rounds": review.RELEASE_MIN_ROUNDS,
        "release_min_rounds": review.RELEASE_MIN_ROUNDS,
        "clear_crashlog_before_start": False,
        "skip_data_canary": False,
        "tabs": list(review.CORE_TAB_SEQUENCE),
        "scroll_surfaces": list(review.CORE_SCROLL_SURFACES),
        "compose_targets": list(review.CORE_COMPOSE_TARGETS),
        "unavailable_ui_probes": unavailable_plan,
        "unavailable_events": unavailable_events,
        "public_rf_tx": False,
        "network_tx": False,
        "map_network_requests": False,
        "formats_sd": False,
        "data_refresh_events": review.RELEASE_MIN_ROUNDS,
        "failure_count": 0,
        "failures": [],
        "identity_failures": [],
        "telemetry_failures": [],
        "checks": checks,
        "setup_events": [
            {
                "command": "version",
                "result": {
                    "ok": True,
                    "cmd": "version",
                    "idf": "v5.5.4",
                    "build_commit": COMMIT,
                    "release_profile": "core_1_0",
                    "sd_history_mode": "disabled",
                },
            },
            {"command": "health", "result": health()},
            {
                "command": "ui status",
                "result": {"ok": True, "cmd": "ui status"},
            },
            {"command": "crashlog", "result": crashlog()},
        ],
        "scroll_events": [
            {
                "surface": surface,
                "command": f"ui scroll-probe {surface}",
                "result": scroll_probe_result(surface),
            }
            for surface in review.CORE_SCROLL_SURFACES
        ],
        "compose_events": [
            {
                "target": target,
                "command": f"ui compose-probe {target}",
                "result": compose_probe_result(target),
            }
            for target in review.CORE_COMPOSE_TARGETS
        ],
        "final_health": health(),
        "events": events,
        "git": clean_git(),
    }


def write_core_ui(root: Path) -> Path:
    path = root / "artifacts" / "hardware" / "com12" / "core_ui.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(core_ui_receipt()), encoding="ascii")
    return path


def capture_review(
    root: Path,
    ui_path: Path,
    *,
    out_name: str = "manual.json",
    operator: str = "Device Operator",
    reviewer: str = "Release Reviewer",
    photo_specs: tuple[str, ...] = (),
) -> Path:
    out_path = root / "artifacts" / "hardware" / "com12" / out_name
    review.capture(
        root=root,
        out_path=out_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
        port="COM12",
        operator=operator,
        reviewer=reviewer,
        confirmed=set(review.REQUIRED_CONFIRMATIONS),
        core_ui_path=ui_path,
        photo_specs=photo_specs,
    )
    return out_path


def png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    checksum = zlib.crc32(chunk_type + data) & 0xFFFFFFFF
    return (
        len(data).to_bytes(4, "big") + chunk_type + data + checksum.to_bytes(4, "big")
    )


def real_png() -> bytes:
    width = 2
    height = 3
    ihdr = (
        width.to_bytes(4, "big") + height.to_bytes(4, "big") + b"\x08\x06\x00\x00\x00"
    )
    pixels = b"".join(b"\x00" + b"\x10\x20\x30\xff" * width for _ in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"tEXt", b"review\x00" + b"x" * 128)
        + png_chunk(b"IDAT", zlib.compress(pixels))
        + png_chunk(b"IEND", b"")
    )


@pytest.fixture(autouse=True)
def exact_clean_candidate(monkeypatch):
    monkeypatch.setattr(review, "git_metadata", lambda _root: clean_git())


def test_producer_validator_and_integrated_manual_gate_accept_exact_receipt(
    tmp_path: Path,
):
    ui_path = write_core_ui(tmp_path)
    assert audit.core_ui_gate(
        ui_path,
        tmp_path,
        COMMIT,
        "disabled",
        RUN_ID,
        RUN_ATTEMPT,
    ).ok

    out_path = capture_review(tmp_path, ui_path)
    valid, reasons, details = review.validate_core_manual_ui_review_receipt(
        out_path,
        root=tmp_path,
        core_ui_path=ui_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    report = json.loads(out_path.read_text(encoding="ascii"))

    assert valid is True
    assert reasons == []
    assert details["automated_ui_file_row_ok"] is True
    assert report["schema"] == 4
    assert report["d1l_target"] == core_ui_receipt()["d1l_target"]
    assert report["github_actions_run_attempt"] == RUN_ATTEMPT
    assert report["workflow_run_attempt"] == RUN_ATTEMPT
    assert report["dm_rf_tx"] is False
    assert all(report[flag] is False for flag in review.REJECTED_FALSE_FLAGS)
    assert audit.manual_review_gate(
        out_path,
        tmp_path,
        COMMIT,
        RUN_ID,
        RUN_ATTEMPT,
        ui_path,
    ).ok


def test_capture_rejects_casefolded_same_person_and_missing_confirmation(
    tmp_path: Path,
):
    ui_path = write_core_ui(tmp_path)
    out_path = tmp_path / "manual.json"
    with pytest.raises(ValueError, match="distinct people"):
        review.capture(
            root=tmp_path,
            out_path=out_path,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            port="COM12",
            operator="Same Person",
            reviewer="same person",
            confirmed=set(review.REQUIRED_CONFIRMATIONS),
            core_ui_path=ui_path,
        )
    assert not out_path.exists()

    with pytest.raises(ValueError, match="every exact Core manual confirmation"):
        review.capture(
            root=tmp_path,
            out_path=out_path,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            port="COM12",
            operator="Operator",
            reviewer="Reviewer",
            confirmed=set(review.REQUIRED_CONFIRMATIONS[:-1]),
            core_ui_path=ui_path,
        )
    assert not out_path.exists()


def test_validator_rejects_attempt_flag_and_file_hash_tampering(tmp_path: Path):
    ui_path = write_core_ui(tmp_path)
    out_path = capture_review(tmp_path, ui_path)
    report = json.loads(out_path.read_text(encoding="ascii"))
    report["github_actions_run_attempt"] = "2"
    report["dm_rf_tx"] = True
    out_path.write_text(json.dumps(report), encoding="ascii")

    valid, reasons, _details = review.validate_core_manual_ui_review_receipt(
        out_path,
        root=tmp_path,
        core_ui_path=ui_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    assert valid is False
    assert "run_attempt_aliases_invalid" in reasons
    assert "receipt_contract_invalid" in reasons
    assert not audit.manual_review_gate(
        out_path,
        tmp_path,
        COMMIT,
        RUN_ID,
        RUN_ATTEMPT,
        ui_path,
    ).ok

    report["github_actions_run_attempt"] = RUN_ATTEMPT
    report["dm_rf_tx"] = False
    out_path.write_text(json.dumps(report), encoding="ascii")
    ui_path.write_text(ui_path.read_text(encoding="ascii") + "\n", encoding="ascii")
    valid, reasons, _details = review.validate_core_manual_ui_review_receipt(
        out_path,
        root=tmp_path,
        core_ui_path=ui_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    assert valid is False
    assert "automated_ui_file_row_mismatch" in reasons


def test_capture_never_overwrites_existing_receipt(tmp_path: Path):
    ui_path = write_core_ui(tmp_path)
    out_path = capture_review(tmp_path, ui_path)
    before = out_path.read_bytes()

    with pytest.raises(FileExistsError, match="refusing to overwrite"):
        capture_review(tmp_path, ui_path)
    assert out_path.read_bytes() == before


def test_optional_png_rows_are_recomputed_and_tamper_evident(tmp_path: Path):
    ui_path = write_core_ui(tmp_path)
    photo_path = tmp_path / "artifacts" / "hardware" / "com12" / "display.png"
    photo_path.write_bytes(real_png())
    spec = f"display_touch_backlight,home={photo_path}"
    out_path = capture_review(
        tmp_path,
        ui_path,
        out_name="manual-with-photo.json",
        photo_specs=(spec,),
    )
    valid, reasons, details = review.validate_core_manual_ui_review_receipt(
        out_path,
        root=tmp_path,
        core_ui_path=ui_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    assert valid is True
    assert reasons == []
    assert details["recomputed_photo_receipts"][0]["width"] == 2
    assert details["recomputed_photo_receipts"][0]["height"] == 3

    raw = bytearray(photo_path.read_bytes())
    raw[-13] ^= 1
    photo_path.write_bytes(raw)
    valid, reasons, _details = review.validate_core_manual_ui_review_receipt(
        out_path,
        root=tmp_path,
        core_ui_path=ui_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    assert valid is False
    assert "photo_receipt_0_invalid" in reasons


def test_manual_review_target_is_derived_and_tamper_evident(tmp_path: Path):
    ui_path = write_core_ui(tmp_path)
    out_path = capture_review(tmp_path, ui_path)
    report = json.loads(out_path.read_text(encoding="ascii"))
    report["d1l_target"]["location"] = "wrong-port"
    out_path.write_text(json.dumps(report), encoding="ascii")

    valid, reasons, _details = review.validate_core_manual_ui_review_receipt(
        out_path,
        root=tmp_path,
        core_ui_path=ui_path,
        commit=COMMIT,
        run_id=RUN_ID,
        run_attempt=RUN_ATTEMPT,
    )
    assert valid is False
    assert "manual_ui_target_binding_invalid" in reasons


def test_manual_review_rejects_target_not_matching_automated_receipt(
    tmp_path: Path,
):
    ui_path = write_core_ui(tmp_path)
    out_path = tmp_path / "manual.json"
    with pytest.raises(ValueError, match="requires COM12"):
        review.capture(
            root=tmp_path,
            out_path=out_path,
            commit=COMMIT,
            run_id=RUN_ID,
            run_attempt=RUN_ATTEMPT,
            port="/dev/ttyUSB2",
            operator="Operator",
            reviewer="Reviewer",
            confirmed=set(review.REQUIRED_CONFIRMATIONS),
            core_ui_path=ui_path,
        )
    assert not out_path.exists()


def test_manual_review_pi_default_path_uses_safe_slug(tmp_path: Path):
    path = review.default_out_path(
        tmp_path,
        COMMIT,
        RUN_ID,
        core_ui.D1L_CORE_POSIX_TARGET,
    )
    assert "dev-serial-by-id-usb-1a86-usb-serial-if00-port0" in str(path)
    assert core_ui.D1L_CORE_POSIX_TARGET not in str(path)
