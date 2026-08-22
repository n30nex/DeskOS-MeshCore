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

## Shared Canadaverse hygiene

- Inspect `git status --short --branch` and recent commits before editing; preserve unrelated and concurrent work.
- Use a scoped `codex/<task>` branch or isolated worktree. Never force-push or reset shared work.
- Keep builds, caches, worktrees, and temporary files on `F:`; do not commit generated caches, logs, device backups, or downloaded artifacts.
- Never commit credentials, `.env` files, Wi-Fi/MQTT passwords, API keys, Cloudflare tokens, MeshCore private keys, live databases, packet captures, or user messages.
- Resolve the exact device, build environment, artifact, offset, host, service, and rollback boundary before hardware or deployment work.
- Preserve firmware identity/settings and unrelated Pi services by default.
- Run focused checks plus `git diff --check`; use the repository's existing CI/release path rather than creating a parallel one.
- Release and deployment claims require exact-commit artifacts and observable verification. Mark anything not physically or publicly tested as unverified.
