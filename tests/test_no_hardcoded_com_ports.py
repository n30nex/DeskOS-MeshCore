import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCAN_DIRS = ["scripts", ".github/workflows", "main", "boards"]
COM_PATTERN = re.compile(r"\bCOM\d+\b", re.IGNORECASE)

# Core 1.0 intentionally binds its physical evidence to the exact ports in the
# product contract.  Keep that exception reviewable and closed: a new literal,
# a new file, or a changed port set must update this policy explicitly.  The
# Core runner tests separately prove that COM8/COM11/COM29 are rejection values,
# COM12 is the only D1L target, COM16 is not a Core D1L/RF target, and COM15 is
# the assigned read-only/controlled-peer listener.
REVIEWED_CORE_RELEASE_PORT_LITERALS = {
    "scripts/d1l_serial_target.py": {
        "COM8", "COM11", "COM12", "COM16", "COM29",
    },
    "scripts/core_flash_only_d1l.py": {
        "COM8", "COM11", "COM12", "COM16", "COM29",
    },
    "scripts/core_install_recovery_review_d1l.py": {"COM12"},
    "scripts/core_reboot_persistence_d1l.py": {
        "COM8", "COM11", "COM12", "COM16", "COM29",
    },
    "scripts/core_release_gate_audit_d1l.py": {"COM12", "COM16"},
    "scripts/core_smoke_d1l.py": {"COM12"},
    "scripts/manual_ui_review_d1l.py": {"COM12"},
    "scripts/package_release_d1l.py": {
        "COM8", "COM11", "COM12", "COM16", "COM29",
    },
    "scripts/rf_full_acceptance_d1l.py": {"COM12", "COM15", "COM16"},
    "scripts/soak_d1l.py": {"COM12", "COM15"},
    "scripts/time_protocol_migration_d1l.py": {"COM12"},
}


def test_only_reviewed_core_release_port_literals_are_hardcoded():
    observed = {}
    for rel in SCAN_DIRS:
        path = ROOT / rel
        if not path.exists():
            continue
        for file in path.rglob("*"):
            if file.is_file() and file.suffix.lower() not in {".png", ".bin", ".elf", ".pyc", ".pyo"}:
                text = file.read_text(encoding="utf-8", errors="ignore")
                matches = {
                    match.group(0).upper()
                    for match in COM_PATTERN.finditer(text)
                }
                if matches:
                    observed[file.relative_to(ROOT).as_posix()] = matches
    assert observed == REVIEWED_CORE_RELEASE_PORT_LITERALS
