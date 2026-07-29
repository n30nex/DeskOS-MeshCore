import hashlib
import json
from pathlib import Path

import pytest

from scripts import core_flash_only_d1l as core_flash
from scripts import produce_rc1_bounded_physical_receipt_d1l as producer


COMMIT = "a" * 40
CANDIDATE = {
    "firmware_commit": COMMIT,
    "actions_run": "123",
    "actions_run_attempt": "1",
    "manifest_sha256": "b" * 64,
    "checksum_manifest_sha256": "c" * 64,
    "app_path": "firmware/meshcore_deskos_d1l.bin",
    "app_sha256": "d" * 64,
}
ROLE_OUTCOMES = {
    role: {
        outcome: (1 if outcome == "public_send_count" else True)
        for outcome, source_role in producer.COVERAGE.items()
        if outcome not in {"target", "flash"} and source_role == role
    }
    for role in producer.SOURCE_ROLES
}


def test_closing_contract_has_exactly_four_fresh_sources():
    assert producer.SOURCE_ROLES == ("flash", "rf", "protocol", "map")
    assert "sd_degraded_notice" not in producer.OUTCOME_KEYS
    assert set(producer.VALIDATORS) == set(producer.SOURCE_ROLES)


def test_flash_validator_does_not_duplicate_settings_preserved_outcome(
    monkeypatch: pytest.MonkeyPatch,
):
    target_field_calls = []
    data = {
        "schema": 2,
        "kind": "esp32_flash",
        "mode": "hardware",
        "physical_observed": True,
        "simulated": False,
        "dry_run": False,
        "manual_only": False,
        "ok": True,
        "closure_eligible": True,
        "release_profile": producer.RELEASE_PROFILE,
        "sd_history_mode": producer.SD_HISTORY_MODE,
        "commit": COMMIT,
        "github_actions_run": "123",
        "workflow_run_attempt": "1",
        "pre_flash_build_commit": "e" * 40,
        "device_build_commit": COMMIT,
        "erase_flash": False,
        "formats_sd": False,
        "retained_state_preserved": True,
        "d1l_target": {
            "stable_identity_sha256": "f" * 64,
        },
        "d1l_target_before": {
            "stable_identity_sha256": "f" * 64,
        },
        "pre_flash_target_after_open": {
            "stable_identity_sha256": "f" * 64,
        },
        "post_flash_reset_target_before_open": {
            "stable_identity_sha256": "f" * 64,
        },
        "post_flash_reset_target_after_open": {
            "stable_identity_sha256": "f" * 64,
        },
        "post_flash_target_after_settle": {
            "stable_identity_sha256": "f" * 64,
        },
        "d1l_target_after": {
            "stable_identity_sha256": "f" * 64,
        },
        "target_identity_continuity_ok": True,
        "post_flash_reset_required": True,
        "post_flash_reset_ok": True,
        "post_flash_reset": {
            "schema": 1,
            "kind": "d1l_post_flash_reset",
            "ok": True,
            "method": "bound_posix_rts_en_pulse",
            "same_admitted_handle": True,
            "dtr_deasserted": True,
            "dtr_reaffirmed_after_release": True,
            "line_sequence": list(
                core_flash.POST_FLASH_RESET_LINE_SEQUENCE
            ),
            "reset_assert_seconds": (
                core_flash.POST_FLASH_RESET_ASSERT_SECONDS
            ),
            "post_release_seconds": (
                core_flash.POST_FLASH_RESET_RELEASE_SECONDS
            ),
            "admitted_target_stable_identity_sha256": "f" * 64,
        },
        "post_flash_reset_error": None,
        "post_flash_reset_binding": core_flash.POST_FLASH_RESET_BINDING,
        "post_flash_reset_binding_ok": True,
        "post_flash_boot_settle": {
            "schema": 1,
            "kind": "d1l_post_flash_boot_settle",
            "ok": True,
            "method": "fresh_reset_handle_hold_no_console_io",
            "same_admitted_handle": True,
            "separate_from_flash_handle": True,
            "flash_handle_closed": True,
            "console_io_attempted": False,
            "settle_seconds": (
                core_flash.MIN_POSIX_POST_FLASH_BOOT_SETTLE_SECONDS
            ),
            "admitted_target_stable_identity_sha256": "f" * 64,
            "settled_target_stable_identity_sha256": "f" * 64,
        },
        "post_flash_capture": {
            "schema": 1,
            "kind": "d1l_post_flash_capture",
            "ok": True,
            "method": "same_fresh_reset_settle_handle",
            "separate_from_flash_handle": True,
            "same_as_reset_settle_handle": True,
            "flash_handle_closed": True,
            "baudrate": core_flash.POST_FLASH_CAPTURE_BAUD,
            "commands": list(core_flash.RETAINED_STATE_COMMANDS),
            "recovery_target_stable_identity_sha256": "f" * 64,
            "settled_target_stable_identity_sha256": "f" * 64,
        },
        "post_flash_capture_binding": core_flash.POST_FLASH_CAPTURE_BINDING,
        "post_flash_capture_binding_ok": True,
        "post_flash_capture_error": None,
        "result": {"name": "esp32_flash", "ok": True},
    }
    monkeypatch.setattr(producer, "_machine_physical", lambda *_args, **_kwargs: True)
    monkeypatch.setattr(producer, "_candidate_binding", lambda *_args, **_kwargs: True)
    monkeypatch.setattr(
        producer,
        "_target_pair",
        lambda _data, fields: target_field_calls.append(fields) or True,
    )
    monkeypatch.setattr(producer, "_find_app_row", lambda *_args, **_kwargs: True)

    assert core_flash.post_flash_reset_contract_ok(data) is True
    assert core_flash.post_flash_capture_contract_ok(data) is True
    assert producer.validate_flash(data, CANDIDATE) == {}
    assert target_field_calls == [producer.FLASH_TARGET_FIELDS]

    data["pre_flash_build_commit"] = "not-a-commit"
    with pytest.raises(producer.EvidenceError):
        producer.validate_flash(data, CANDIDATE)

    data["pre_flash_build_commit"] = "e" * 40
    data["post_flash_reset_binding"] = "same_admitted_handle"
    with pytest.raises(producer.EvidenceError):
        producer.validate_flash(data, CANDIDATE)

    data["post_flash_reset_binding"] = core_flash.POST_FLASH_RESET_BINDING
    data["post_flash_reset"]["method"] = "unbound-reset"
    with pytest.raises(producer.EvidenceError):
        producer.validate_flash(data, CANDIDATE)

    data["post_flash_reset"]["method"] = "bound_posix_rts_en_pulse"
    data["post_flash_capture_binding"] = "same_admitted_handle"
    with pytest.raises(producer.EvidenceError):
        producer.validate_flash(data, CANDIDATE)


def test_flash_target_pair_requires_every_recovery_epoch_snapshot(monkeypatch):
    data = {
        field: {"stable_identity_sha256": "a" * 64}
        for field in producer.FLASH_TARGET_FIELDS
    }
    monkeypatch.setattr(
        producer,
        "_target",
        lambda value, field="d1l_target": value[field],
    )

    assert producer._target_pair(data, producer.FLASH_TARGET_FIELDS) is True

    for field in producer.FLASH_TARGET_FIELDS:
        tampered = {
            key: dict(value)
            for key, value in data.items()
        }
        tampered[field]["stable_identity_sha256"] = "b" * 64
        assert (
            producer._target_pair(
                tampered,
                producer.FLASH_TARGET_FIELDS,
            )
            is False
        )


def test_producer_bundles_unique_machine_sources_and_hashes_receipt(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
):
    sources = {}
    for index, role in enumerate(producer.SOURCE_ROLES, start=1):
        path = tmp_path / "inputs" / f"{role}.json"
        path.parent.mkdir(exist_ok=True)
        path.write_text(
            json.dumps({"role": role, "nonce": index}) + "\n",
            encoding="ascii",
        )
        sources[role] = path

    monkeypatch.setattr(producer, "package_candidate", lambda _path: dict(CANDIDATE))
    monkeypatch.setattr(
        producer,
        "_target",
        lambda _data, field="d1l_target": {
            "vid": int(producer.USB_VID, 16),
            "pid": int(producer.USB_PID, 16),
        },
    )
    for role in producer.SOURCE_ROLES:
        monkeypatch.setitem(
            producer.VALIDATORS,
            role,
            lambda _data, _candidate, role=role: dict(ROLE_OUTCOMES[role]),
        )

    output = tmp_path / "bundle" / "physical.json"
    receipt, sidecar = producer.produce(
        package_dir=tmp_path / "package",
        sources=sources,
        output=output,
        evidence_root=tmp_path,
    )

    receipt_bytes = output.read_bytes()
    assert receipt["outcomes"]["public_send_count"] == 1
    assert sidecar["candidate"] == CANDIDATE
    assert sidecar["receipt"]["sha256"] == hashlib.sha256(receipt_bytes).hexdigest()
    assert set(sidecar["sources"]) == set(producer.SOURCE_ROLES)
    assert len({row["sha256"] for row in sidecar["sources"].values()}) == len(
        producer.SOURCE_ROLES
    )
    assert sidecar["coverage"] == producer.COVERAGE


def test_protocol_transcript_rejects_dry_run_before_accepting_outcomes():
    transcript = {
        key: None for key in producer.TRANSCRIPT_KEYS
    }
    transcript.update(
        {
            "schema": 1,
            "kind": producer.PROTOCOL_KIND,
            "mode": "hardware",
            "physical_observed": True,
            "simulated": False,
            "dry_run": True,
            "manual_only": False,
            "port": producer.POSIX_D1L_TARGET,
            "expected_firmware_commit": COMMIT,
            "github_actions_run": "123",
            "workflow_run_attempt": "1",
            "steps": [],
        }
    )

    with pytest.raises(producer.EvidenceError):
        producer.validate_protocol(transcript, CANDIDATE)
