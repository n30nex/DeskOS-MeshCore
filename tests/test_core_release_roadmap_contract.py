import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

from scripts import core_install_recovery_review_d1l as install_review
from scripts import manual_ui_review_d1l as manual_review


ROOT = Path(__file__).resolve().parents[1]
ROADMAP = (
    ROOT
    / "docs"
    / "release"
    / "SIGUI_24H_AUDIT_AND_ROADMAP_2026-07-18.md"
)


def command_sheet() -> str:
    roadmap = ROADMAP.read_text(encoding="utf-8")
    return roadmap.split("## 8. Candidate commands", 1)[1].split(
        "## 9. Release gates", 1
    )[0]


def documented_script_options() -> dict[str, set[str]]:
    options: dict[str, set[str]] = defaultdict(set)
    for block in re.findall(r"```bash\n(.*?)\n```", command_sheet(), re.DOTALL):
        lines = block.splitlines()
        index = 0
        while index < len(lines):
            line = lines[index]
            match = re.match(r"python \./scripts/([a-zA-Z0-9_]+\.py)(.*)", line)
            if match is None:
                index += 1
                continue

            script, command = match.groups()
            while command.rstrip().endswith("\\") and index + 1 < len(lines):
                index += 1
                command += "\n" + lines[index]
            options[script].update(re.findall(r"(?<!\w)(--[a-z0-9-]+)", command))
            index += 1
    return options


def test_core_command_sheet_has_one_sd_disabled_artifact_contract():
    roadmap = ROADMAP.read_text(encoding="utf-8")
    sheet = command_sheet()

    for unsupported_branch in (
        "sd_history=conditional",
        "sd_history=supported_optional",
        "include_sd_bridge=true",
        "SD conditional-mode",
        "Define SD as conditional",
    ):
        assert unsupported_branch not in roadmap
    assert "Build and package Core 1.0 only with `sd_history=disabled`." in roadmap
    assert "include_sd_bridge=false" in sheet
    assert "exactly the five Core archives" in sheet
    assert "An SD-enabled run exposes eight\nartifacts" in sheet
    assert "--expected-sd-history-mode disabled" in sheet
    assert "--sd-history-mode disabled" in sheet
    assert "--sd-file-canary" not in sheet
    assert "supported SD" not in sheet


def test_candidate_commands_are_in_fail_closed_closing_order():
    sheet = command_sheet()
    ordered_markers = (
        "capture_core_actions_run_d1l.py",
        "--phase bootstrap",
        "time_protocol_migration_d1l.py",
        "core_smoke_d1l.py",
        "core_ui_corruption_probe_d1l.py",
        "scroll_probe_d1l.py",
        "manual_ui_review_d1l.py",
        "rf_full_acceptance_d1l.py",
        "  seed \\",
        "--phase retained-reflash",
        "  verify \\",
        "--duration-sec 3600",
        "--duration-sec 1800",
        "core_install_recovery_review_d1l.py",
        '--out "$D1L_PRETAG_AUDIT"',
        "capture_core_github_defects_d1l.py",
        '--out "$D1L_FINAL_AUDIT"',
    )

    positions = [sheet.index(marker) for marker in ordered_markers]
    assert positions == sorted(positions)


def test_rf_receipt_is_derived_and_reused_by_active_soak():
    sheet = command_sheet()

    assert 'export D1L_CONTROLLED_PEER_RECEIPT="$D1L_RF_RECEIPT"' in sheet
    assert "export D1L_PEER_FINGERPRINT='024999DEDFD26763'" in sheet
    assert "--peer-local" in sheet
    assert '--out "$D1L_RF_RECEIPT"' in sheet
    assert 'test -f "$D1L_CONTROLLED_PEER_RECEIPT"' in sheet
    assert (
        '--controlled-peer-receipt "$D1L_CONTROLLED_PEER_RECEIPT"' in sheet
    )


def test_human_review_and_migration_attestations_are_complete_and_explicit():
    sheet = command_sheet()

    for confirmation in manual_review.REQUIRED_CONFIRMATIONS:
        assert f"--confirm-{confirmation.replace('_', '-')}" in sheet
    for confirmation in install_review.INSTALL_REVIEW_CONFIRMATIONS:
        assert f"--confirm-{confirmation.replace('_', '-')}" in sheet

    assert "Set two distinct real people." in sheet
    assert 'test "$D1L_OPERATOR" != "$D1L_REVIEWER"' in sheet
    assert "three real cold power cycles" in sheet
    assert "do not\npre-answer it" in sheet
    assert "--attest-exact-device-upper-bound" in sheet
    assert "Do not pass the attestation flag with guessed, copied, or" in sheet


def test_pre_tag_defect_capture_is_bound_to_recomputed_preliminary_audit():
    sheet = command_sheet()
    preliminary = sheet.split("### Preliminary pre-tag audit", 1)[1].split(
        "### Fresh GitHub defect snapshot", 1
    )[0]

    assert '--out "$D1L_PRETAG_AUDIT"' in sheet
    assert 'test "$D1L_PRETAG_AUDIT_STATUS" -eq 1' in sheet
    assert "--defect-receipt" not in preliminary
    assert '--non-tag-audit "$D1L_PRETAG_AUDIT"' in sheet
    assert "every other non-tag gate green" in sheet
    assert "within 15 minutes of\nthe defect capture" in sheet


def test_final_audit_receives_every_required_closing_receipt():
    sheet = command_sheet()
    final_section = sheet.split("### Final audit", 1)[1]
    audit_command = final_section.split(
        "python ./scripts/core_release_gate_audit_d1l.py", 1
    )[1]
    required_bindings = {
        "--actions-run-receipt": "$D1L_ACTIONS_RECEIPT",
        "--core-smoke": "$D1L_SMOKE_RECEIPT",
        "--core-ui": "$D1L_UI_RECEIPT",
        "--core-scroll": "$D1L_SCROLL_RECEIPT",
        "--manual-review": "$D1L_MANUAL_UI_RECEIPT",
        "--reboot-receipt": "$D1L_REBOOT_RECEIPT",
        "--protocol-migration-receipt": "$D1L_PROTOCOL_RECEIPT",
        "--rf-receipt": "$D1L_RF_RECEIPT",
        "--active-soak": "$D1L_ACTIVE_SOAK_RECEIPT",
        "--idle-soak": "$D1L_IDLE_SOAK_RECEIPT",
        "--install-review": "$D1L_INSTALL_REVIEW_RECEIPT",
        "--defect-receipt": "$D1L_DEFECT_RECEIPT",
    }
    for option, variable in required_bindings.items():
        assert f'{option} "{variable}"' in audit_command


def test_every_documented_python_option_exists_in_cli_help():
    options_by_script = documented_script_options()
    assert options_by_script

    for script, options in sorted(options_by_script.items()):
        if not options:
            continue
        command = [sys.executable, str(ROOT / "scripts" / script), "--help"]
        help_results = [
            subprocess.run(
                command,
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
                timeout=15,
            ).stdout
        ]
        if script == "core_reboot_persistence_d1l.py":
            for subcommand in ("seed", "verify"):
                help_results.append(
                    subprocess.run(
                        command[:-1] + [subcommand, "--help"],
                        cwd=ROOT,
                        check=True,
                        capture_output=True,
                        text=True,
                        timeout=15,
                    ).stdout
                )
        combined_help = "\n".join(help_results)
        missing = sorted(option for option in options if option not in combined_help)
        assert not missing, f"{script} does not expose documented options: {missing}"
