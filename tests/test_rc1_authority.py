import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_rc1_documentation_authority_is_small_and_fail_closed():
    bootstrap = read("CODEX_BOOTSTRAP_PROMPT.md")
    roadmap = read("docs/ROADMAP.md")
    checklist = read("docs/RELEASE_CHECKLIST.md")
    test_plan = read("docs/TEST_PLAN_D1L.md")
    runbook = read("docs/RC1_RELEASE_EXECUTION_D1L.md")
    docs_index = read("docs/README.md")
    workflow = read(".github/workflows/d1l-ci.yml")

    for active in (
        "AGENTS.md",
        "docs/RC1_SCOPE.md",
        "docs/ROADMAP.md",
        "docs/TEST_PLAN_D1L.md",
        "docs/RC1_RELEASE_EXECUTION_D1L.md",
    ):
        assert active in bootstrap
    for obsolete in (
        "SIGUI_MASTER_COMPLETION_ROADMAP_2026-07-12.md",
        "SIGUI_EXECUTION_BACKLOG_2026-07-12.yaml",
        "COMPLETION_LEDGER.yaml",
    ):
        assert obsolete not in bootstrap

    rows = re.findall(r"^\| (D0|R[1-6]) \|", roadmap, flags=re.MULTILINE)
    assert rows == ["D0", "R1", "R2", "R3", "R4", "R5", "R6"]
    assert re.search(r"\b\d+(?:\.\d+)?%", roadmap) is None
    assert re.search(r"\b\d+(?:\.\d+)?%", checklist) is None
    assert "No timed idle, endurance, traffic, listening, or soak gate" in test_plan

    assert "python ./scripts/completion_ledger.py" not in workflow
    assert "python ./scripts/release_gate_audit_d1l.py" not in workflow
    simulator_commands = [
        line.strip()
        for line in workflow.splitlines()
        if "python ./tools/ui_simulator.py" in line
    ]
    assert len(simulator_commands) == 4
    assert all("--release-profile core_1_0" in line for line in simulator_commands)
    assert "shell: bash" in workflow

    for release_doc in (read("README.md"), checklist, runbook):
        assert "rc1_release_gate_audit_d1l.py" in release_doc

    assert re.findall(r"^## .+$", docs_index, flags=re.MULTILINE) == [
        "## Active RC1 authority",
        "## User/reference documentation",
        "## RC2/deferred work",
        "## Historical archive",
    ]
    before_archive, archive_section = docs_index.split("## Historical archive", 1)
    assert "archive/pre-rc1-authority-reset" not in before_archive
    assert "provenance only" in archive_section

    active_docs = (
        "AGENTS.md",
        "CODEX_BOOTSTRAP_PROMPT.md",
        "README.md",
        "docs/RC1_SCOPE.md",
        "docs/ROADMAP.md",
        "docs/TEST_PLAN_D1L.md",
        "docs/RC1_RELEASE_EXECUTION_D1L.md",
        "docs/RELEASE_CHECKLIST.md",
        "docs/KNOWN_LIMITATIONS.md",
        "docs/RC2_BACKLOG.md",
    )
    assert all("archive/pre-rc1-authority-reset" not in read(path) for path in active_docs)

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
