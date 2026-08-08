import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_release_authority_is_product_delivery_not_a_lab_gate():
    bootstrap = read("CODEX_BOOTSTRAP_PROMPT.md")
    roadmap = read("docs/ROADMAP.md")
    checklist = read("docs/RELEASE_CHECKLIST.md")
    runbook = read("docs/RC1_RELEASE_EXECUTION_D1L.md")
    docs_index = read("docs/README.md")
    workflow = read(".github/workflows/d1l-ci.yml")

    for active in (
        "AGENTS.md",
        "docs/RC1_SCOPE.md",
        "docs/ROADMAP.md",
        "docs/RC1_RELEASE_EXECUTION_D1L.md",
        "docs/RELEASE_CHECKLIST.md",
    ):
        assert active in bootstrap

    rows = re.findall(r"^\| (R[1-4]) \|", roadmap, flags=re.MULTILINE)
    assert rows == ["R1", "R2", "R3", "R4"]
    assert re.search(r"\b\d+(?:\.\d+)?%", roadmap) is None
    assert re.search(r"\b\d+(?:\.\d+)?%", checklist) is None

    active_release_text = "\n".join(
        (read("README.md"), roadmap, checklist, runbook, read("AGENTS.md"))
    )
    for obsolete_requirement in (
        "PEER_STATUS",
        "ADMIN_PASSWORD_FILE",
        "produce_rc1_protocol_acceptance_d1l.py",
        "produce_rc1_bounded_physical_receipt_d1l.py",
        "rc1_release_gate_audit_d1l.py",
        "four machine sources",
    ):
        assert obsolete_requirement not in active_release_text

    for public_file in (
        "firmware/meshcore_deskos_d1l.bin",
        "full-flash/meshcore_deskos_d1l-full-8mb.bin",
        "deskos_sd_bridge.ino.uf2",
        "START_HERE.md",
        "SHA256SUMS-$VERSION.txt",
    ):
        assert public_file in runbook or public_file in checklist

    assert 'grep -F \'Existing DeskOS: preserving update BIN\'' in runbook
    assert "flash_full_8mb.sh" in runbook
    assert "TAG=v1.0.1" in runbook

    assert "python ./scripts/completion_ledger.py" not in workflow
    assert "python ./scripts/release_gate_audit_d1l.py" not in workflow
    simulator_commands = [
        line.strip()
        for line in workflow.splitlines()
        if "python ./tools/ui_simulator.py" in line
    ]
    assert len(simulator_commands) == 4
    assert all("--release-profile core_1_0" in line for line in simulator_commands)

    assert re.findall(r"^## .+$", docs_index, flags=re.MULTILINE) == [
        "## Current release truth",
        "## User documentation",
        "## Developer-only material",
        "## Historical archive",
    ]
    before_archive, archive_section = docs_index.split("## Historical archive", 1)
    assert "archive/pre-rc1-authority-reset" not in before_archive
    assert "provenance only" in archive_section


def test_production_package_contract_excludes_qualification_material():
    package = read("scripts/package_release_d1l.py")
    cmake = read("main/CMakeLists.txt")

    assert "set(D1L_ENABLE_QUALIFICATION_HOOKS_DEFINE 0)" in cmake
    for marker in (
        'b"display test"',
        'b"ui scroll-probe"',
        'b"storage filecanary"',
        'b"core retained-witness"',
    ):
        assert marker in package
    assert '"release_status": "production"' in package


def test_archived_plans_remain_non_executable_history():
    archived_plans = (
        "docs/archive/pre-rc1-authority-reset/COMPLETION_LEDGER.yaml",
        "docs/archive/pre-rc1-authority-reset/COMPLETION_STATUS.md",
        "docs/archive/pre-rc1-authority-reset/FAST_RELEASE_WORKFLOW_D1L.md",
        "docs/archive/pre-rc1-authority-reset/SIGUI_CORE_1_0_PRODUCT_CONTRACT_2026-07-18.md",
        "docs/archive/pre-rc1-authority-reset/SIGUI_RC1_DOCUMENTATION_AND_ROADMAP_RESET.md",
        "docs/completion/SIGUI_CODEX_5_6_ULTRA_GOAL_PROMPT.md",
        "docs/completion/SIGUI_MASTER_COMPLETION_ROADMAP_2026-07-12.md",
        "docs/completion/SIGUI_EXECUTION_BACKLOG_2026-07-12.yaml",
        "docs/completion/SIGUI_AUDIT_EVIDENCE_INDEX_2026-07-12.md",
    )
    for path in archived_plans:
        assert "HISTORICAL RECORD — DO NOT EXECUTE" in read(path)[:700]
