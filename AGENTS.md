# SIGUI RC1 operating rules

Read these active files first, in order:

1. `docs/RC1_SCOPE.md`
2. `docs/ROADMAP.md`
3. `docs/RC1_RELEASE_EXECUTION_D1L.md`
4. `docs/RELEASE_CHECKLIST.md`

The following rules are non-negotiable:

- RC1 means `core_1_0`; do not expand it.
- Historical completion files are provenance only and are not executable.
- The release deliverable is the production package: working ESP32 BIN files,
  the production RP2040 UF2, checksums, and end-user instructions.
- Internal CI and developer checks do not create public release requirements and
  are never shipped in the production package.
- Run only focused local checks for changed code. GitHub Actions builds firmware.
- Never format an SD card.
- Never probe arbitrary serial ports. Final hardware work uses only the stable,
  authorized D1L identity and its required VID/PID.
- Do not create another roadmap, ledger, evidence schema, dashboard, or release
  framework.
- At most one implementation PR may be active.
- A reproducible product defect produces a code fix; otherwise continue the
  shortest path to the published user package.
