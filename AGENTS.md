# SIGUI RC1 operating rules

Read these active files first, in order:

1. `docs/RC1_SCOPE.md`
2. `docs/ROADMAP.md`
3. `docs/TEST_PLAN_D1L.md`
4. `docs/RC1_RELEASE_EXECUTION_D1L.md`
5. `docs/RELEASE_CHECKLIST.md`

The following rules are non-negotiable:

- RC1 means `core_1_0`; do not expand it.
- Historical completion files are provenance only and are not executable.
- `scripts/rc1_release_gate_audit_d1l.py` is the final release authority.
- Run focused local tests for the changed slice. GitHub Actions builds firmware.
- RC1 has no timed soak requirement.
- Never format an SD card.
- Never probe arbitrary serial ports. Final hardware work uses only the stable,
  authorized D1L identity and its required VID/PID.
- Do not create another roadmap, ledger, evidence schema, dashboard, or test
  framework unless an observed release defect cannot use the existing path.
- At most one implementation PR may be active.
- A failed check produces a code fix or a clear operator blocker, not another
  planning document.
