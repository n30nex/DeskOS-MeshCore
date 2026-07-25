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
    assert json.loads(receipt.read_text(encoding="utf-8"))["formats_sd"] is False
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
