#!/usr/bin/env python3
"""Reject qualification hooks while allowing the read-only UI capture API."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Sequence


# These are exact qualification surfaces, not generic words such as "test".
# In particular, the user-created MeshCore channel name "#test" is valid data
# and must never be rejected by this build-time audit.
ESP_FORBIDDEN_MARKERS: tuple[bytes, ...] = (
    b"display test",
    b"touch test",
    b"ui scroll-probe",
    b"ui compose-probe",
    b"ui data-canary",
    b"map acceptance status",
    b"map acceptance open",
    b"storage filecanary",
    b"storage map-tile-canary",
    b"storage map-tile-check",
    b"storage export-canary",
    b"storage retained-canary",
    b"core retained-witness",
    b"synthetic ui refresh canary",
    b"synthetic retained-history canary",
    b"test from deskos d1l",
    b"ui-data-canary",
    b"ui_canary",
    b"ui canary",
    b"sd-retained-canary",
    b"sd_canary",
    b"sd canary",
    b"canary/fc-",
    b"d1l desk",
    b"c0dec0dec0dec0de",
    b"contact probe",
    b"scroll probe dm",
    b"probe dm",
    b"probe contact",
    b"deskos probe",
    b"43.6532",
    b"probenet",
    b"probe-pass",
)

RP2040_FORBIDDEN_MARKERS: tuple[bytes, ...] = (
    b"/deskos/canary",
    b"deskos_canary_dir_unavailable",
    b"/deskos/probe.tmp",
    b"/deskos/probe.json",
    b"d1l-sd-file-ops-ready",
    b'{"schema":1,"probe":"d1l"}',
    b"deskos_sd_bridge_smoke",
    b"deskos_sd_bridge_official_smoke",
)

FORBIDDEN_CONFIG_VALUES: tuple[str, ...] = (
    "CONFIG_BT_NIMBLE_DTM_MODE_TEST",
    "CONFIG_BT_CTRL_DTM_ENABLE",
    "CONFIG_ESP_DEBUG_OCDAWARE",
    "CONFIG_FREERTOS_DEBUG_OCDAWARE",
    "CONFIG_ESP32S3_DEBUG_OCDAWARE",
)

FORBIDDEN_SYMBOL_PATTERNS: tuple[re.Pattern[str], ...] = tuple(
    re.compile(pattern)
    for pattern in (
        r"^cmd_(?:display|touch)_test$",
        r"^d1l_board_display_color_test$",
        r"^cmd_ui_(?:scroll_probe|compose_probe|data_canary)$",
        r"^cmd_map_acceptance_",
        r"^cmd_storage_(?:filecanary|map_tile_canary|map_tile_check|export_canary|retained_canary)",
        r"^cmd_core_retained_witness$",
        r"^d1l_ui_phase1_(?:scroll_probe|compose_probe|request_map_acceptance)$",
        r"^d1l_ui_map_viewport_prepare_acceptance$",
        r"^d1l_export_store_write_canary$",
        r"^d1l_map_tile_store_(?:write|check)_canary$",
        r"^d1l_message_store_append_(?:public|channel)_volatile$",
        r"^d1l_dm_store_append_volatile$",
        r"^d1l_packet_log_append_raw_volatile$",
        r"^(?:s_)?(?:scroll|compose)_probe(?:_|$)",
        r"^process_pending_(?:scroll|compose)_probe$",
        r"^probe_gate_",
        r"^(?:retained_canary|core_retained_witness|storage_filecanary)",
    )
)


def _finding(kind: str, path: Path, detail: str) -> dict[str, str]:
    return {"kind": kind, "path": str(path), "detail": detail}


def audit_binary_markers(
    path: Path,
    markers: Sequence[bytes],
    *,
    kind: str,
) -> list[dict[str, str]]:
    data = path.read_bytes().lower()
    findings: list[dict[str, str]] = []
    for marker in markers:
        if marker.lower() in data:
            findings.append(
                _finding(kind, path, marker.decode("ascii", errors="replace"))
            )
    return findings


def parse_nm_symbols(output: str) -> list[str]:
    symbols: list[str] = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line or line.endswith(":"):
            continue
        fields = line.split()
        if len(fields) < 2:
            continue
        candidate = fields[-1]
        if candidate and candidate not in {"U", "u"}:
            symbols.append(candidate)
    return symbols


def load_defined_symbols(elf: Path, nm_tool: str) -> list[str]:
    completed = subprocess.run(
        [nm_tool, "--defined-only", str(elf)],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(
            f"{nm_tool} failed for {elf} with exit {completed.returncode}: {detail}"
        )
    return parse_nm_symbols(completed.stdout)


def audit_symbols(elf: Path, symbols: Iterable[str]) -> list[dict[str, str]]:
    findings: list[dict[str, str]] = []
    for symbol in sorted(set(symbols)):
        if any(pattern.search(symbol) for pattern in FORBIDDEN_SYMBOL_PATTERNS):
            findings.append(_finding("forbidden_symbol", elf, symbol))
    return findings


def audit_sdkconfig(path: Path) -> list[dict[str, str]]:
    enabled: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in FORBIDDEN_CONFIG_VALUES and value.strip().lower() not in {
            "",
            "0",
            "n",
            "no",
            "false",
        }:
            enabled[key] = value.strip()
    return [
        _finding("forbidden_sdkconfig", path, f"{key}={enabled[key]}")
        for key in sorted(enabled)
    ]


def audit_production_payload(
    *,
    esp_bin: Path,
    esp_elf: Path,
    sdkconfig: Path,
    rp2040_payloads: Sequence[Path],
    nm_tool: str,
) -> list[dict[str, str]]:
    findings = audit_binary_markers(
        esp_bin, ESP_FORBIDDEN_MARKERS, kind="forbidden_esp_marker"
    )
    findings.extend(audit_sdkconfig(sdkconfig))
    findings.extend(audit_symbols(esp_elf, load_defined_symbols(esp_elf, nm_tool)))
    for payload in rp2040_payloads:
        findings.extend(
            audit_binary_markers(
                payload,
                RP2040_FORBIDDEN_MARKERS,
                kind="forbidden_rp2040_marker",
            )
        )
    return findings


def _existing_file(value: str) -> Path:
    path = Path(value)
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"file does not exist: {path}")
    return path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp-bin", required=True, type=_existing_file)
    parser.add_argument("--esp-elf", required=True, type=_existing_file)
    parser.add_argument("--sdkconfig", required=True, type=_existing_file)
    parser.add_argument(
        "--rp2040-payload",
        action="append",
        default=[],
        type=_existing_file,
        help="Production RP2040 UF2/bin to inspect; repeat for multiple payloads.",
    )
    parser.add_argument(
        "--nm-tool",
        default="xtensa-esp32s3-elf-nm",
        help="Exact ESP32-S3 nm executable used to inspect defined ELF symbols.",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        findings = audit_production_payload(
            esp_bin=args.esp_bin,
            esp_elf=args.esp_elf,
            sdkconfig=args.sdkconfig,
            rp2040_payloads=args.rp2040_payload,
            nm_tool=args.nm_tool,
        )
    except (OSError, RuntimeError) as exc:
        print(
            json.dumps(
                {
                    "schema": 1,
                    "ok": False,
                    "code": "AUDIT_EXECUTION_FAILED",
                    "error": str(exc),
                },
                sort_keys=True,
            )
        )
        return 2

    report = {
        "schema": 1,
        "ok": not findings,
        "qualification_hooks_absent": not findings,
        "esp_bin": str(args.esp_bin),
        "esp_elf": str(args.esp_elf),
        "sdkconfig": str(args.sdkconfig),
        "rp2040_payloads": [str(path) for path in args.rp2040_payload],
        "findings": findings,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if not findings else 1


if __name__ == "__main__":
    raise SystemExit(main())
