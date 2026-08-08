import json
import re
from pathlib import Path

import pytest

from scripts import (
    audit_production_firmware_payload_d1l as payload_audit,
    package_release_d1l,
)


ROOT = Path(__file__).resolve().parents[1]


def production_source_projection(text: str) -> str:
    """Project source with only the customer value of the hook macro."""

    output: list[str] = []
    active = True
    stack: list[tuple[bool, bool | None]] = []
    for line in text.splitlines():
        directive = line.strip()
        if directive.startswith("#if"):
            known: bool | None = None
            if not (
                directive.startswith("#ifdef")
                or directive.startswith("#ifndef")
            ) and "D1L_ENABLE_QUALIFICATION_HOOKS" in directive:
                negated = re.search(
                    r"!\s*D1L_ENABLE_QUALIFICATION_HOOKS"
                    r"|D1L_ENABLE_QUALIFICATION_HOOKS\s*==\s*0",
                    directive,
                )
                known = bool(negated)
            stack.append((active, known))
            if known is not None:
                active = active and known
            continue
        if directive.startswith("#else") and stack:
            parent_active, known = stack[-1]
            active = (
                parent_active
                if known is None
                else parent_active and not known
            )
            continue
        if directive.startswith("#elif") and stack:
            parent_active, known = stack[-1]
            active = parent_active if known is None else False
            continue
        if directive.startswith("#endif") and stack:
            active = stack.pop()[0]
            continue
        if active:
            output.append(line)
    return "\n".join(output)


def write_payload_inputs(tmp_path: Path) -> tuple[Path, Path, Path, Path]:
    esp_bin = tmp_path / "app.bin"
    esp_bin.write_bytes(
        b"touch raw\0routes probe\0user channel #test\0"
    )
    esp_elf = tmp_path / "app.elf"
    esp_elf.write_bytes(b"ELF")
    sdkconfig = tmp_path / "sdkconfig"
    sdkconfig.write_text(
        "\n".join(
            f"# {key} is not set"
            for key in payload_audit.FORBIDDEN_CONFIG_VALUES
        )
        + "\n",
        encoding="utf-8",
    )
    rp2040 = tmp_path / "deskos_sd_bridge.ino.uf2"
    rp2040.write_bytes(b"DESKOS_SD_STATUS\0card_detect\0mount\0reinsert\0")
    return esp_bin, esp_elf, sdkconfig, rp2040


def test_audit_allows_product_diagnostics_and_explicit_test_channel(tmp_path):
    esp_bin, esp_elf, sdkconfig, rp2040 = write_payload_inputs(tmp_path)

    assert (
        payload_audit.audit_binary_markers(
            esp_bin,
            payload_audit.ESP_FORBIDDEN_MARKERS,
            kind="forbidden_esp_marker",
        )
        == []
    )
    assert payload_audit.audit_sdkconfig(sdkconfig) == []
    assert payload_audit.audit_symbols(
        esp_elf,
        [
            "cmd_touch_raw",
            "cmd_routes_probe",
            "d1l_rp2040_bridge_sd_diag",
        ],
    ) == []
    assert (
        payload_audit.audit_binary_markers(
            rp2040,
            payload_audit.RP2040_FORBIDDEN_MARKERS,
            kind="forbidden_rp2040_marker",
        )
        == []
    )


@pytest.mark.parametrize("marker", payload_audit.ESP_FORBIDDEN_MARKERS)
def test_audit_rejects_each_esp_qualification_marker(tmp_path, marker):
    payload = tmp_path / "app.bin"
    payload.write_bytes(b"prefix\0" + marker + b"\0suffix")

    findings = payload_audit.audit_binary_markers(
        payload,
        payload_audit.ESP_FORBIDDEN_MARKERS,
        kind="forbidden_esp_marker",
    )

    assert any(row["detail"] == marker.decode("ascii") for row in findings)


@pytest.mark.parametrize("marker", payload_audit.RP2040_FORBIDDEN_MARKERS)
def test_audit_rejects_each_rp2040_qualification_marker(tmp_path, marker):
    payload = tmp_path / "bridge.uf2"
    payload.write_bytes(b"prefix\0" + marker + b"\0suffix")

    findings = payload_audit.audit_binary_markers(
        payload,
        payload_audit.RP2040_FORBIDDEN_MARKERS,
        kind="forbidden_rp2040_marker",
    )

    assert any(row["detail"] == marker.decode("ascii") for row in findings)


@pytest.mark.parametrize("key", payload_audit.FORBIDDEN_CONFIG_VALUES)
def test_audit_rejects_enabled_test_or_ocd_sdkconfig(tmp_path, key):
    sdkconfig = tmp_path / "sdkconfig"
    sdkconfig.write_text(f"{key}=y\n", encoding="utf-8")

    assert payload_audit.audit_sdkconfig(sdkconfig) == [
        {
            "kind": "forbidden_sdkconfig",
            "path": str(sdkconfig),
            "detail": f"{key}=y",
        }
    ]


@pytest.mark.parametrize(
    "symbol",
    [
        "cmd_ui_scroll_probe",
        "cmd_display_test",
        "cmd_touch_test",
        "d1l_board_display_color_test",
        "cmd_map_acceptance_open",
        "cmd_storage_retained_canary",
        "cmd_core_retained_witness",
        "d1l_export_store_write_canary",
        "d1l_message_store_append_public_volatile",
        "d1l_dm_store_append_volatile",
        "d1l_packet_log_append_raw_volatile",
        "process_pending_compose_probe",
    ],
)
def test_audit_rejects_qualification_symbols(tmp_path, symbol):
    elf = tmp_path / "app.elf"
    elf.write_bytes(b"ELF")

    assert payload_audit.audit_symbols(elf, [symbol]) == [
        {
            "kind": "forbidden_symbol",
            "path": str(elf),
            "detail": symbol,
        }
    ]


def test_audit_allows_read_only_capture_symbols(tmp_path):
    elf = tmp_path / "app.elf"
    elf.write_bytes(b"ELF")

    assert payload_audit.audit_symbols(
        elf,
        ["cmd_ui_capture_begin", "d1l_ui_capture_chunk", "s_capture_shadow"],
    ) == []


def test_cli_report_is_fail_closed_and_machine_readable(
    tmp_path, monkeypatch, capsys
):
    esp_bin, esp_elf, sdkconfig, rp2040 = write_payload_inputs(tmp_path)
    monkeypatch.setattr(
        payload_audit,
        "load_defined_symbols",
        lambda _elf, _nm_tool: ["cmd_touch_raw"],
    )

    result = payload_audit.main(
        [
            "--esp-bin",
            str(esp_bin),
            "--esp-elf",
            str(esp_elf),
            "--sdkconfig",
            str(sdkconfig),
            "--rp2040-payload",
            str(rp2040),
            "--nm-tool",
            "unused-in-test",
        ]
    )

    report = json.loads(capsys.readouterr().out)
    assert result == 0
    assert report["ok"] is True
    assert report["qualification_hooks_absent"] is True
    assert report["findings"] == []


def test_package_surface_allows_capture_but_rejects_qualification_hooks(tmp_path):
    package = tmp_path / "customer-package"
    firmware = package / "firmware"
    firmware.mkdir(parents=True)
    app = firmware / "app.bin"
    app.write_bytes(b"normal product payload\0user channel #test\0")

    package_release_d1l.validate_production_package_surface(package)

    app.write_bytes(b"normal product payload\0ui capture begin\0")
    package_release_d1l.validate_production_package_surface(package)

    app.write_bytes(b"normal product payload\0ui data-canary\0")
    with pytest.raises(
        ValueError,
        match="Production firmware payload contains an internal qualification marker",
    ):
        package_release_d1l.validate_production_package_surface(package)


def test_package_surface_allows_requested_test_channel_in_reader_docs(tmp_path):
    package = tmp_path / "customer-package"
    package.mkdir()
    (package / "START_HERE.md").write_text(
        "Starting channels: Public, #bot, and #test.\n",
        encoding="utf-8",
    )

    package_release_d1l.validate_production_package_surface(package)

    (package / "START_HERE.md").write_text(
        "Run the production test before use.\n",
        encoding="utf-8",
    )
    with pytest.raises(
        ValueError,
        match="Production reader document contains internal qualification language",
    ):
        package_release_d1l.validate_production_package_surface(package)


def test_package_and_exact_payload_audits_share_forbidden_markers():
    assert (
        package_release_d1l.PRODUCTION_FORBIDDEN_ESP_PAYLOAD_MARKERS
        == payload_audit.ESP_FORBIDDEN_MARKERS
    )
    assert (
        package_release_d1l.PRODUCTION_FORBIDDEN_RP2040_PAYLOAD_MARKERS
        == payload_audit.RP2040_FORBIDDEN_MARKERS
    )


def test_ci_runs_exact_payload_audit_before_packaging():
    workflow = (
        ROOT / ".github" / "workflows" / "d1l-ci.yml"
    ).read_text(encoding="utf-8")

    audit_step = workflow.index("- name: Audit production firmware payload")
    package_step = workflow.index("- name: Package D1L release")
    assert audit_step < package_step
    assert "scripts/audit_production_firmware_payload_d1l.py" in workflow
    assert "--esp-bin build/meshcore_deskos_d1l.bin" in workflow
    assert "--esp-elf build/meshcore_deskos_d1l.elf" in workflow
    assert "--sdkconfig sdkconfig" in workflow
    assert '--rp2040-payload "${production_uf2[0]}"' in workflow


def test_customer_profiles_compile_qualification_hooks_out():
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")

    assert "set(D1L_ENABLE_QUALIFICATION_HOOKS_DEFINE 1)" in cmake
    assert cmake.count(
        "set(D1L_ENABLE_QUALIFICATION_HOOKS_DEFINE 0)"
    ) == 2
    assert (
        "D1L_ENABLE_QUALIFICATION_HOOKS="
        "${D1L_ENABLE_QUALIFICATION_HOOKS_DEFINE}"
    ) in cmake
    for key in payload_audit.FORBIDDEN_CONFIG_VALUES:
        assert f"# {key} is not set" in defaults


def test_customer_source_projection_contains_no_qualification_surface():
    projected = "\n".join(
        production_source_projection(
            path.read_text(encoding="utf-8", errors="replace")
        )
        for path in sorted((ROOT / "main").rglob("*"))
        if path.is_file() and path.suffix.lower() in {".c", ".cc", ".cpp", ".h"}
    )
    projected_lower = projected.lower().encode("utf-8")
    assert [
        marker.decode("ascii")
        for marker in payload_audit.ESP_FORBIDDEN_MARKERS
        if marker.lower() in projected_lower
    ] == []

    identifiers = set(re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", projected))
    assert payload_audit.audit_symbols(
        Path("customer-source-projection"), identifiers
    ) == []
