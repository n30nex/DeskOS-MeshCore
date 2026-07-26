import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "prepare_deskos_sd.py"


def run_prepare(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )


def test_prepare_is_plan_only_by_default_and_apply_is_verified(tmp_path):
    target = tmp_path / "card"
    target.mkdir()

    planned = run_prepare(
        "--target", str(target), "--skip-filesystem-check"
    )
    assert planned.returncode == 0, planned.stderr
    plan = json.loads(planned.stdout)
    assert plan["applied"] is False
    assert plan["formats_sd"] is False
    assert not (target / "deskos").exists()

    applied = run_prepare(
        "--target", str(target), "--skip-filesystem-check", "--apply"
    )
    assert applied.returncode == 0, applied.stderr
    result = json.loads(applied.stdout)
    assert result["applied"] is True
    assert result["deletes_files"] is False
    assert result["overwrites_files"] is False
    manifest = target / "deskos" / "manifest.json"
    map_manifest = target / "deskos" / "map" / "manifest.json"
    receipt = target / "deskos" / "card-preparation-receipt.json"
    assert manifest.is_file()
    assert json.loads(map_manifest.read_text(encoding="utf-8"))["schema"] == 2
    receipt_payload = json.loads(receipt.read_text(encoding="utf-8"))
    assert receipt_payload["formats_sd"] is False
    expected_directories = {
        "deskos/stores/messages/public",
        "deskos/stores/messages/dm",
        "deskos/stores/nodes",
        "deskos/stores/contacts",
        "deskos/stores/read_state",
        "deskos/stores/routes",
        "deskos/stores/packet_log",
        "deskos/exports",
        "deskos/map/tiles",
    }
    assert set(result["directories"]) == expected_directories
    assert set(receipt_payload["directories"]) == expected_directories
    for relative in expected_directories:
        assert (target / relative).is_dir()
    repeated = run_prepare(
        "--target", str(target), "--skip-filesystem-check", "--apply"
    )
    assert repeated.returncode == 0, repeated.stderr
    assert all(
        item["state"] == "identical"
        for item in json.loads(repeated.stdout)["file_actions"]
    )


def test_prepare_refuses_overwrite_and_unlicensed_tile_import(tmp_path):
    target = tmp_path / "card"
    target.mkdir()
    conflicting = target / "deskos" / "manifest.json"
    conflicting.parent.mkdir()
    conflicting.write_text("user data\n", encoding="utf-8")

    refused = run_prepare(
        "--target", str(target), "--skip-filesystem-check", "--apply"
    )
    assert refused.returncode == 2
    assert "refusing to overwrite" in refused.stderr
    assert conflicting.read_text(encoding="utf-8") == "user data\n"

    clean_target = tmp_path / "clean-card"
    clean_target.mkdir()
    tiles = tmp_path / "tiles"
    tile = tiles / "z10" / "x1" / "y2.png"
    tile.parent.mkdir(parents=True)
    tile.write_bytes(b"\x89PNG\r\n\x1a\nfake")
    (tiles / "offline-tile-provider.json").write_text(
        json.dumps(
            {
                "schema": 1,
                "source_id": "test",
                "attribution": "test",
                "license_url": "https://example.invalid",
                "offline_storage_permitted": False,
                "tile_template": "z{z}/x{x}/y{y}.png",
            }
        ),
        encoding="utf-8",
    )
    unlicensed = run_prepare(
        "--target", str(clean_target),
        "--skip-filesystem-check",
        "--tiles-from", str(tiles),
    )
    assert unlicensed.returncode == 2
    assert "offline_storage_permitted=true" in unlicensed.stderr


def test_prepare_installs_authorized_network_provider_without_leaking_url(tmp_path):
    target = tmp_path / "card"
    target.mkdir()
    tiles = tmp_path / "provider"
    tiles.mkdir()
    provider = {
        "schema": 1,
        "source_id": "licensed-local",
        "attribution": "(c) Licensed Local Maps",
        "license_url": "https://example.invalid/license",
        "offline_storage_permitted": True,
        "background_prefetch_permitted": True,
        "network_url_template":
            "https://tiles.example.invalid/{z}/{x}/{y}.png?key=topsecret",
        "tile_template": "z{z}/x{x}/y{y}.png",
        "max_zoom": 18,
        "average_tile_bytes": 65536,
        "minimum_request_interval_ms": 500,
    }
    (tiles / "offline-tile-provider.json").write_text(
        json.dumps(provider), encoding="utf-8"
    )

    applied = run_prepare(
        "--target", str(target),
        "--skip-filesystem-check",
        "--tiles-from", str(tiles),
        "--apply",
    )
    assert applied.returncode == 0, applied.stderr
    assert "topsecret" not in applied.stdout
    result = json.loads(applied.stdout)
    assert result["provider"]["network_fetch_configured"] is True
    assert "network_url_template" not in result["provider"]
    installed = json.loads(
        (target / "deskos" / "map" / "offline-provider.json").read_text(
            encoding="utf-8"
        )
    )
    assert installed["network_url_template"] == provider["network_url_template"]
    receipt = json.loads(
        (target / "deskos" / "card-preparation-receipt.json").read_text(
            encoding="utf-8"
        )
    )
    assert "network_url_template" not in receipt["provider"]


def test_prepare_rejects_provider_text_firmware_cannot_parse(tmp_path):
    target = tmp_path / "card"
    target.mkdir()
    tiles = tmp_path / "provider"
    tiles.mkdir()
    (tiles / "offline-tile-provider.json").write_text(
        json.dumps(
            {
                "schema": 1,
                "source_id": "licensed-local",
                "attribution": "Copyright \u00a9 Local Maps",
                "license_url": "https://example.invalid/license",
                "offline_storage_permitted": True,
                "background_prefetch_permitted": True,
                "network_url_template":
                    "https://tiles.example.invalid/{z}/{x}/{y}.png",
                "tile_template": "z{z}/x{x}/y{y}.png",
                "max_zoom": 18,
                "average_tile_bytes": 65536,
            }
        ),
        encoding="utf-8",
    )
    refused = run_prepare(
        "--target", str(target),
        "--skip-filesystem-check",
        "--tiles-from", str(tiles),
    )
    assert refused.returncode == 2
    assert "safe ASCII" in refused.stderr
