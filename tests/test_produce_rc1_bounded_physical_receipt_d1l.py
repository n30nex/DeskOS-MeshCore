import hashlib
import json
from pathlib import Path

import pytest

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
    )

    receipt_bytes = output.read_bytes()
    assert receipt["outcomes"]["public_send_count"] == 1
    assert sidecar["candidate"] == CANDIDATE
    assert sidecar["receipt"]["sha256"] == hashlib.sha256(receipt_bytes).hexdigest()
    assert set(sidecar["sources"]) == set(producer.SOURCE_ROLES)
    assert len({row["sha256"] for row in sidecar["sources"].values()}) == 8
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
