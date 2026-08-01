from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_no_on_device_sd_format_command_remains():
    retired_request = "DESKOS_SD_" + "FORMAT"
    retired_confirmation = "FORMAT-" + "DESKOS-SD"
    retired_api = "d1l_rp2040_bridge_" + "format_sd"
    retired_storage_api = "d1l_storage_" + "format_sd_confirmed"

    checked_roots = ("main", "firmware", "scripts", "tools", "docs")
    offenders: list[str] = []
    for root_name in checked_roots:
        for path in (ROOT / root_name).rglob("*"):
            if not path.is_file() or path.suffix.lower() not in {".c", ".h", ".ino", ".py", ".md", ".ps1", ".sh"}:
                continue
            text = path.read_text(encoding="utf-8", errors="ignore")
            if any(token in text for token in (retired_request, retired_confirmation, retired_api, retired_storage_api)):
                offenders.append(str(path.relative_to(ROOT)))

    assert offenders == []


def test_sd_user_policy_copy_is_fat32_only():
    ui = read("main/ui/ui_phase1.c")
    console = read("main/comms/usb_console.c")
    storage = read("main/storage/storage_status.c")
    bridge = read("firmware/rp2040_sd_bridge/deskos_sd_bridge/deskos_sd_bridge.ino")

    assert "FAT32 only - This device never formats cards." in ui
    assert '\\"policy\\":\\"no_device_format\\"' in console
    assert "prepare_fat32_on_computer" in storage
    assert "not_fat32_or_unmountable" in bridge
    assert "needs_fat32=" in bridge


def test_release_docs_match_no_format_sd_policy():
    release_docs = [
        "README.md",
        "docs/ROADMAP.md",
        "docs/KNOWN_LIMITATIONS.md",
        "docs/RELEASE_CHECKLIST.md",
        "docs/RP2040_SD_BRIDGE_FLASH_D1L.md",
        "docs/SD_BRIDGE_PROTOCOL_D1L.md",
        "docs/TEST_PLAN_D1L.md",
    ]
    forbidden = [
        "status/format",
        "format once",
        "format cards on boot",
        "auto_format_unformatted",
        "auto-prepare clearly blank",
        "auto-prepare unformatted/new cards",
        "format would risk existing user data",
        "Fresh blank/unformatted card: format",
        "formats/prepares according to setting",
        "user-confirmed format flow",
        "format with internal confirmation",
        "Require confirmation for ambiguous/existing-data formats",
        "Blank/unformatted card: prepare according to production policy",
        "never reformat a card that already",
        "Download and cache allowed attributed map tiles after Wi-Fi",
        "download one center tile when Wi-Fi",
        "touch Map Tiles provider/download sheet",
        "provider/center-tile download UX",
    ]

    offenders: list[str] = []
    for rel in release_docs:
        lowered = read(rel).lower()
        for phrase in forbidden:
            if phrase.lower() in lowered:
                offenders.append(f"{rel}: {phrase}")
    assert offenders == []

    combined = "\n".join(read(rel) for rel in release_docs).lower()
    for phrase in [
        "fat32",
        "never formats",
        "nvs fallback",
        "live-only",
        "conditional",
    ]:
        assert phrase in combined
