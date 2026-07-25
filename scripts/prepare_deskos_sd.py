#!/usr/bin/env python3
"""Safely prepare an already-formatted FAT32 card for DeskOS.

The tool never formats, deletes, or overwrites files. Its default mode is a
read-only plan; --apply copies the checked-in DeskOS payload and verifies every
written byte. Optional tiles must come with an explicit provider manifest that
permits offline storage.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
from urllib.parse import urlsplit
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


MIN_CARD_BYTES = 28_000_000_000
BUNDLE_ROOT = Path(__file__).resolve().parents[1] / "sdcard"
DESKOS_ROOT_NAME = "deskos"
PROVIDER_MANIFEST_NAME = "offline-tile-provider.json"
RECEIPT_NAME = "card-preparation-receipt.json"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class PreparationError(RuntimeError):
    pass


@dataclass(frozen=True)
class CopyItem:
    source: Path
    relative: Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def filesystem_type(path: Path) -> str:
    if os.name == "nt":
        root = Path(path.anchor or path)
        name_buffer = ctypes.create_unicode_buffer(261)
        fs_buffer = ctypes.create_unicode_buffer(261)
        serial = ctypes.c_ulong()
        max_component = ctypes.c_ulong()
        flags = ctypes.c_ulong()
        ok = ctypes.windll.kernel32.GetVolumeInformationW(
            str(root),
            name_buffer,
            len(name_buffer),
            ctypes.byref(serial),
            ctypes.byref(max_component),
            ctypes.byref(flags),
            fs_buffer,
            len(fs_buffer),
        )
        return fs_buffer.value.lower() if ok else ""

    result = subprocess.run(
        ["findmnt", "-n", "-o", "FSTYPE", "--target", str(path)],
        check=False,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip().lower() if result.returncode == 0 else ""


def card_capacity_bytes(path: Path) -> int:
    usage = shutil.disk_usage(path)
    return int(usage.total)


def checked_bundle_items() -> list[CopyItem]:
    source_root = BUNDLE_ROOT / DESKOS_ROOT_NAME
    if not source_root.is_dir():
        raise PreparationError(f"checked-in DeskOS payload missing: {source_root}")
    return [
        CopyItem(source=path, relative=path.relative_to(BUNDLE_ROOT))
        for path in sorted(source_root.rglob("*"))
        if path.is_file()
    ]


def load_provider_manifest(tile_source: Path) -> dict:
    manifest_path = tile_source / PROVIDER_MANIFEST_NAME
    if not manifest_path.is_file():
        raise PreparationError(
            f"tile source must contain {PROVIDER_MANIFEST_NAME}"
        )
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PreparationError(f"invalid tile provider manifest: {exc}") from exc
    required_text = ("source_id", "attribution", "license_url")
    if manifest.get("schema") != 1:
        raise PreparationError("tile provider manifest schema must be 1")
    if manifest.get("offline_storage_permitted") is not True:
        raise PreparationError(
            "tile provider manifest must explicitly set "
            "offline_storage_permitted=true"
        )
    if any(not isinstance(manifest.get(key), str) or not manifest[key].strip()
           for key in required_text):
        raise PreparationError(
            "tile provider manifest requires source_id, attribution, and license_url"
        )
    if manifest.get("tile_template") != "z{z}/x{x}/y{y}.png":
        raise PreparationError(
            "tile provider manifest tile_template must be z{z}/x{x}/y{y}.png"
        )
    source_id = manifest["source_id"]
    if len(source_id) > 24 or any(
        not (
            character.isascii()
            and (character.isalnum() or character in "-_")
        )
        for character in source_id
    ):
        raise PreparationError(
            "tile provider source_id must be 1-24 letters, numbers, '-' or '_'"
        )
    if source_id == "openstreetmap-standard":
        raise PreparationError("tile provider source_id is reserved")
    for key, maximum in (("attribution", 64), ("license_url", 128)):
        value = manifest[key]
        if len(value) > maximum or any(
            not character.isascii()
            or ord(character) < 32
            or ord(character) > 126
            or character in {'"', "\\"}
            for character in value
        ):
            raise PreparationError(
                f"tile provider {key} must be 1-{maximum} safe ASCII characters"
            )
    license_url = urlsplit(manifest["license_url"])
    if license_url.scheme != "https" or not license_url.netloc:
        raise PreparationError("tile provider license_url must be HTTPS")
    if not isinstance(manifest.get("background_prefetch_permitted"), bool):
        raise PreparationError(
            "tile provider manifest requires background_prefetch_permitted"
        )
    max_zoom = manifest.get("max_zoom")
    average_tile_bytes = manifest.get("average_tile_bytes")
    if not isinstance(max_zoom, int) or not 14 <= max_zoom <= 18:
        raise PreparationError("tile provider max_zoom must be 14 through 18")
    if (
        not isinstance(average_tile_bytes, int)
        or not 4096 <= average_tile_bytes <= 196 * 1024
    ):
        raise PreparationError(
            "tile provider average_tile_bytes must be 4096 through 200704"
        )
    network_template = manifest.get("network_url_template")
    if network_template is not None:
        if (
            not isinstance(network_template, str)
            or len(network_template) > 192
            or not network_template.isascii()
            or '"' in network_template
            or "\\" in network_template
            or urlsplit(network_template).scheme != "https"
            or not urlsplit(network_template).netloc
            or any(network_template.count(token) != 1 for token in ("{z}", "{x}", "{y}"))
            or "#" in network_template
        ):
            raise PreparationError(
                "network_url_template must be HTTPS with one {z}, {x}, and {y}"
            )
    if manifest["background_prefetch_permitted"] and not network_template:
        raise PreparationError(
            "background prefetch permission requires network_url_template"
        )
    request_interval_ms = manifest.get(
        "minimum_request_interval_ms", 250
    )
    if (
        not isinstance(request_interval_ms, int)
        or not 100 <= request_interval_ms <= 5000
    ):
        raise PreparationError(
            "minimum_request_interval_ms must be 100 through 5000"
        )
    manifest["minimum_request_interval_ms"] = request_interval_ms
    encoded = (
        json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    ).encode("utf-8")
    if len(encoded) > 1024:
        raise PreparationError(
            "tile provider manifest exceeds the 1024-byte firmware limit"
        )
    return manifest


def tile_items(tile_source: Path) -> tuple[dict, list[CopyItem]]:
    manifest = load_provider_manifest(tile_source)
    items: list[CopyItem] = []
    for path in sorted(tile_source.rglob("*.png")):
        relative = path.relative_to(tile_source)
        parts = relative.parts
        if len(parts) != 3 or not parts[0].startswith("z") or \
                not parts[1].startswith("x") or not parts[2].startswith("y"):
            raise PreparationError(
                f"tile path must be z<zoom>/x<x>/y<y>.png: {relative}"
            )
        numeric = (parts[0][1:], parts[1][1:], Path(parts[2]).stem[1:])
        if any(not value.isdigit() for value in numeric):
            raise PreparationError(f"non-numeric tile coordinate: {relative}")
        with path.open("rb") as handle:
            if handle.read(len(PNG_SIGNATURE)) != PNG_SIGNATURE:
                raise PreparationError(f"tile is not a PNG: {relative}")
        items.append(
            CopyItem(
                source=path,
                relative=Path(
                    DESKOS_ROOT_NAME, "map", "tiles", manifest["source_id"]
                ) /
                relative,
            )
        )
    if not items and not manifest.get("network_url_template"):
        raise PreparationError("tile source contains no PNG tiles")
    return manifest, items


def ensure_target(target: Path, skip_filesystem_check: bool) -> dict:
    if not target.exists() or not target.is_dir():
        raise PreparationError(f"target directory does not exist: {target}")
    resolved = target.resolve()
    if resolved == Path.home().resolve() or resolved == Path.cwd().resolve():
        raise PreparationError("refusing a home or working-directory target")
    fs_type = filesystem_type(resolved)
    capacity = card_capacity_bytes(resolved)
    if not skip_filesystem_check and fs_type not in {"fat32", "vfat", "msdos"}:
        raise PreparationError(
            f"target filesystem is {fs_type or 'unknown'}, not FAT32"
        )
    if not skip_filesystem_check and capacity < MIN_CARD_BYTES:
        raise PreparationError(
            f"target capacity is {capacity} bytes; a 32GB-class card is required"
        )
    return {
        "target": str(resolved),
        "filesystem": fs_type or "unknown",
        "capacity_bytes": capacity,
        "filesystem_check_skipped": bool(skip_filesystem_check),
    }


def destination_state(target: Path, items: Iterable[CopyItem]) -> list[dict]:
    result: list[dict] = []
    for item in items:
        destination = target / item.relative
        source_hash = sha256_file(item.source)
        state = "copy"
        if destination.exists():
            if not destination.is_file():
                raise PreparationError(
                    f"destination exists and is not a file: {destination}"
                )
            if sha256_file(destination) != source_hash:
                raise PreparationError(
                    f"refusing to overwrite different file: {destination}"
                )
            state = "identical"
        result.append(
            {
                "source": str(item.source),
                "relative": item.relative.as_posix(),
                "sha256": source_hash,
                "bytes": item.source.stat().st_size,
                "state": state,
            }
        )
    return result


def write_provider_metadata(target: Path, manifest: dict, apply: bool) -> dict:
    relative = Path(DESKOS_ROOT_NAME, "map", "offline-provider.json")
    destination = target / relative
    payload = (
        json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    ).encode("utf-8")
    digest = hashlib.sha256(payload).hexdigest()
    state = "copy"
    if destination.exists():
        if not destination.is_file() or destination.read_bytes() != payload:
            raise PreparationError(
                f"refusing to overwrite different provider metadata: {destination}"
            )
        state = "identical"
    elif apply:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(payload)
        if destination.read_bytes() != payload:
            raise PreparationError(f"provider metadata verification failed: {destination}")
    return {
        "relative": relative.as_posix(),
        "sha256": digest,
        "bytes": len(payload),
        "state": state,
    }


def apply_items(target: Path, planned: list[dict]) -> None:
    for entry in planned:
        if entry["state"] == "identical":
            continue
        source = Path(entry["source"])
        destination = target / Path(entry["relative"])
        destination.parent.mkdir(parents=True, exist_ok=True)
        with source.open("rb") as source_handle, destination.open("xb") as dest_handle:
            shutil.copyfileobj(source_handle, dest_handle, length=1024 * 1024)
        if sha256_file(destination) != entry["sha256"]:
            raise PreparationError(f"copy verification failed: {destination}")


def write_receipt(target: Path, payload: dict) -> Path:
    destination = target / DESKOS_ROOT_NAME / RECEIPT_NAME
    encoded = (
        json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    ).encode("utf-8")
    if destination.exists():
        if destination.read_bytes() == encoded:
            return destination
        raise PreparationError(
            f"refusing to overwrite different receipt: {destination}"
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(encoded)
    if destination.read_bytes() != encoded:
        raise PreparationError(f"receipt verification failed: {destination}")
    return destination


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prepare an already-formatted FAT32 SD card for DeskOS."
    )
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument(
        "--apply",
        action="store_true",
        help="copy and verify files; without this flag the command is read-only",
    )
    parser.add_argument(
        "--tiles-from",
        type=Path,
        help=(
            "optional zN/xN/yN.png tree containing offline-tile-provider.json "
            "with offline_storage_permitted=true"
        ),
    )
    parser.add_argument(
        "--skip-filesystem-check",
        action="store_true",
        help="staging/test directories only; never use this to bypass a real card check",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        target_info = ensure_target(args.target, args.skip_filesystem_check)
        items = checked_bundle_items()
        provider = None
        if args.tiles_from:
            tile_source = args.tiles_from.resolve()
            if not tile_source.is_dir():
                raise PreparationError(f"tile source does not exist: {tile_source}")
            provider, imported = tile_items(tile_source)
            items.extend(imported)
        planned = destination_state(Path(target_info["target"]), items)
        receipt = {
            "schema": 1,
            "kind": "deskos_sd_preparation",
            "applied": bool(args.apply),
            "formats_sd": False,
            "deletes_files": False,
            "overwrites_files": False,
            **target_info,
            "provider": (
                {
                    "source_id": provider["source_id"],
                    "attribution": provider["attribution"],
                    "license_url": provider["license_url"],
                    "offline_storage_permitted": True,
                    "background_prefetch_permitted":
                        provider["background_prefetch_permitted"],
                    "network_fetch_configured":
                        bool(provider.get("network_url_template")),
                    "max_zoom": provider["max_zoom"],
                    "average_tile_bytes": provider["average_tile_bytes"],
                }
                if provider else None
            ),
            "files": [
                {
                    key: entry[key]
                    for key in ("relative", "sha256", "bytes")
                }
                for entry in planned
            ],
        }
        result = {**receipt, "file_actions": [
            {
                key: entry[key]
                for key in ("relative", "sha256", "bytes", "state")
            }
            for entry in planned
        ]}
        if args.apply:
            apply_items(Path(target_info["target"]), planned)
            if provider:
                provider_entry = write_provider_metadata(
                    Path(target_info["target"]), provider, True
                )
                receipt["files"].append(
                    {
                        key: provider_entry[key]
                        for key in ("relative", "sha256", "bytes")
                    }
                )
                result["file_actions"].append(provider_entry)
            receipt["applied"] = True
            result["applied"] = True
            result["files"] = receipt["files"]
            receipt_path = write_receipt(Path(target_info["target"]), receipt)
            result["receipt"] = str(receipt_path)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (OSError, PreparationError) as exc:
        print(
            json.dumps(
                {
                    "schema": 1,
                    "kind": "deskos_sd_preparation",
                    "ok": False,
                    "formats_sd": False,
                    "error": str(exc),
                },
                sort_keys=True,
            ),
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
