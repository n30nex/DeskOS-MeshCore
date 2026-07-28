import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from scripts import test_rc1


COMMIT = "a" * 40


def write_package(root: Path) -> None:
    (root / "SHA256SUMS.txt").unlink(missing_ok=True)
    manifest = {
        "project": test_rc1.EXPECTED_PROJECT,
        "firmware_commit": COMMIT,
        "release_profile": test_rc1.EXPECTED_PROFILE,
        "sd_history_mode": "conditional",
        "actions_run": "123456789",
        "actions_run_attempt": "1",
    }
    (root / "manifest.json").write_text(
        json.dumps(manifest),
        encoding="ascii",
    )
    (root / "payload.bin").write_bytes(b"payload")
    rows = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        rows.append(f"{digest}  ./{path.relative_to(root).as_posix()}")
    (root / "SHA256SUMS.txt").write_text(
        "\n".join(rows) + "\n",
        encoding="ascii",
    )


def passing_results() -> dict:
    identity = {
        "build_commit": COMMIT,
        "release_profile": test_rc1.EXPECTED_PROFILE,
        "sd_history_mode": "conditional",
    }
    return {
        "version": {
            "schema": 1,
            "ok": True,
            "cmd": "version",
            "idf": test_rc1.EXPECTED_IDF,
            **identity,
        },
        "health": {
            "schema": 1,
            "ok": True,
            "cmd": "health",
            "board_ready": True,
            "ui_ready": True,
            **identity,
        },
        "rp2040 ping": {
            "schema": 1,
            "ok": True,
            "cmd": "rp2040 ping",
            "bridge_ready": True,
            "protocol_supported": True,
            "public_rf_tx": False,
            "formats_sd": False,
        },
        "storage status": {
            "schema": 1,
            "ok": True,
            "cmd": "storage status",
            "data_enabled": True,
            "data_backend": "sd",
            "sd": {
                "present": True,
                "mounted": True,
                "filesystem": "fat32",
                "data_root_ready": True,
                "rp2040_bridge_ready": True,
                "rp2040_protocol_supported": True,
                "state": "ready",
            },
            **identity,
        },
    }


def test_complete_package_verification_and_identity(tmp_path):
    write_package(tmp_path)

    verification = test_rc1.verify_complete_package(tmp_path)
    identity = test_rc1.load_package_identity(tmp_path)

    assert verification["ok"] is True
    assert verification["checksummed_files"] == 2
    assert identity == {
        "commit": COMMIT,
        "profile": test_rc1.EXPECTED_PROFILE,
        "sd_history_mode": "conditional",
        "actions_run": "123456789",
        "actions_run_attempt": "1",
    }


def test_complete_package_verification_rejects_tamper_and_extra_file(tmp_path):
    write_package(tmp_path)
    (tmp_path / "payload.bin").write_bytes(b"tampered")
    with pytest.raises(test_rc1.Rc1TestError, match="SHA256 mismatch"):
        test_rc1.verify_complete_package(tmp_path)

    write_package(tmp_path)
    (tmp_path / "unchecksummed.txt").write_text("extra", encoding="ascii")
    with pytest.raises(test_rc1.Rc1TestError, match="not a complete"):
        test_rc1.verify_complete_package(tmp_path)


def test_windows_port_requires_explicit_matching_d1l_usb_identity():
    selected = SimpleNamespace(
        device="COM7",
        vid=test_rc1.EXPECTED_VID,
        pid=test_rc1.EXPECTED_PID,
    )

    target = test_rc1.validate_explicit_port(
        "com7",
        platform_name="windows",
        rows=[selected],
    )

    assert target["requested_path"] == "COM7"
    assert target["target_kind"] == "windows_com_operator_supplied"
    with pytest.raises(test_rc1.Rc1TestError, match="not the D1L"):
        test_rc1.validate_explicit_port(
            "COM7",
            platform_name="windows",
            rows=[SimpleNamespace(device="COM7", vid=0x1234, pid=0x5678)],
        )
    with pytest.raises(test_rc1.Rc1TestError, match="explicitly selected"):
        test_rc1.validate_explicit_port(
            "COM8",
            platform_name="windows",
            rows=[selected],
        )


def test_result_evaluation_requires_exact_firmware_bridge_and_ready_sd():
    identity = {
        "commit": COMMIT,
        "profile": test_rc1.EXPECTED_PROFILE,
        "sd_history_mode": "conditional",
        "actions_run": "123456789",
        "actions_run_attempt": "1",
    }
    results = passing_results()

    assert test_rc1.evaluate_results(results, identity) == {
        "exact_firmware": True,
        "esp32_board_and_ui_ready": True,
        "rp2040_bridge_ready": True,
        "prepared_sd_card_ready": True,
    }

    results["storage status"]["sd"]["filesystem"] = "exfat"
    checks = test_rc1.evaluate_results(results, identity)
    assert checks["prepared_sd_card_ready"] is False
    assert checks["exact_firmware"] is True
