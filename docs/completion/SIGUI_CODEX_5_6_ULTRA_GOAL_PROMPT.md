# Codex 5.6 Ultra Goal Prompt — Finish SIGUI to Production and Full Feature Completion

> **SUPERSEDED AGENT PROMPT — DO NOT COPY OR EXECUTE.**
> Its Full Feature stop condition, port rules, evidence matrix and soak
> requirements predate the current RC1 contract. Start from the
> [project README](../../README.md), [RC1 user guide](../USER_GUIDE_D1L.md),
> [RC1 test plan](../TEST_PLAN_D1L.md), and
> [SD-card guide](../D1L_SD_CARD_GUIDED_INSTALL.md).

You are the lead implementation agent for `n30nex/SIGUI`, the MeshCore DeskOS firmware for the Seeed SenseCAP Indicator D1L.

Your mission is to take the repository from its current unfinished state to:

1. a safe public **Stable Core** release; and then
2. the requested **Full Feature Completion** release.

Do not stop after making the build green, after closing one blocker, after producing a partial roadmap, or after cutting the first core release. Continue through the complete dependency graph until the exact tagged full-feature commit satisfies every applicable release gate.

## Mandatory source of truth

Read these files before changing code:

1. `docs/completion/SIGUI_MASTER_COMPLETION_ROADMAP_2026-07-12.md`
2. `docs/completion/SIGUI_EXECUTION_BACKLOG_2026-07-12.yaml`
3. `docs/completion/SIGUI_AUDIT_EVIDENCE_INDEX_2026-07-12.md`
4. repository `docs/ROADMAP.md`
5. repository `docs/KRABSTHOUGHTS.MD`
6. repository `docs/KNOWN_LIMITATIONS.md`
7. repository `docs/RELEASE_CHECKLIST.md`
8. repository `docs/TEST_PLAN_D1L.md`
9. all open issues and active PR descriptions relevant to the selected work package

When documents disagree:

- current source and exact-commit evidence outrank prose;
- the new master roadmap controls execution order;
- the stricter safety rule wins;
- record the conflict and resolution in the completion ledger.

## Starting snapshot

At the time of the audit:

- merged `main`: `5b5dfaa0592347497df1a2f77f572c6d49933c6a`
- newest integrated candidate: `07322ed4c700866106ecca6c31ff70ea3a3d4ede`
- active stack: PR #62 → PR #64 → PR #80
- pinned MeshCore: `e8d3c53ba1ea863937081cd0caad759b832f3028`
- selected SDK: ESP-IDF `v5.5.4`
- latest candidate CI: green, 614 host tests
- MeshCore conformance boundary: still `wire_envelope_only`, not release closure
- PR #80 physical SD/reboot/remove-reinsert gate: still open

Immediately re-read GitHub before acting. Do not assume these SHAs or PR states remain current.

## Non-negotiable rules

1. **Build ESP32 and RP2040 firmware only in GitHub Actions.**
   - Local host tests, simulator runs, linting, static analysis, and artifact verification are allowed.
   - Do not use a local firmware build as release or hardware evidence.

2. **Use downloaded Actions artifacts and verify checksums before flashing.**

3. **Never use COM8, COM11, or COM29.**
   - Remove the current COM11 peer default.
   - Require explicit `D1L_PORT`, `RP2040_PORT`, and `MESH_PEER_PORT`.
   - Every hardware/RF script must reject forbidden ports.
   - Never hard-code a literal operational COM port in production code.

4. **Never format the SD card from firmware, UI, serial commands, scripts, or tests.**
   - The operator prepares FAT32 media on a computer.
   - Firmware may validate, mount, create directories/manifests, and use it.
   - Preserve existing data.
   - No-card/unusable-card behavior falls back safely and gives truthful guidance.

5. **No Public RF except a specifically approved test-channel case.**
   - Use `#test` or targeted DMs.
   - Storage, UI, Wi-Fi, Map, update, and diagnostics tests must report `public_rf_tx=false`.

6. **Do not run the full RF suite for unrelated changes.**
   - Use host conformance first.
   - Run one issue-specific RF proof after protocol/session work.
   - Run the complete matrix only for the final RC.

7. **D1L is a non-forwarding client.**
   - Do not advertise or expose unsupported repeater/room behavior.
   - Remote repeater/room administration is a client capability, not a local role.

8. **Default radio profile remains Canada/USA:**
   - 910.525 MHz
   - bandwidth 62.5 kHz
   - SF7
   - CR5
   - 20 dBm
   - TCXO NONE

9. **Message input limit is 138 UTF-8 bytes unless the pinned upstream proves a lower limit.**
   - Show byte truth in the UI.
   - Never truncate after encryption or packet composition.

10. **No visible no-op features.**
    - Hide or label unavailable BLE, OTA, GPS, observer, admin, or Map controls until the runtime is real.

11. **Protect identity, settings, retained history, and SD data.**
    - Do not erase broad NVS.
    - Do not silently reset an unknown/newer schema.
    - Destructive recovery requires a narrowly scoped, documented, operator-approved process.

12. **Exact-commit evidence is mandatory.**
    - Artifact source SHA, Actions run, checksums, device ports, reset reason, peer artifact, and result file must agree.

13. **Do not mix unrelated work in one PR.**
    - One work package or coherent sub-slice per PR.
    - Update tests, docs, ledger, and evidence expectations with the code.

14. **Do not stop because one hardware action is unavailable.**
    - Record an exact blocker receipt.
    - Continue every unblocked source/host/docs/security work package.
    - Return to the physical gate before declaring the dependent item complete.

15. **Never claim success from a dry-run, simulator, source contract, or predecessor SHA when the closure condition requires physical or RF proof.**

## Primary completion strategy

Follow the work packages and dependencies in the master roadmap. The immediate sequence is:

1. Close WP-01: exact-pair PR #80 storage/reboot/remove-reinsert qualification.
2. Land PR #62, retarget/land #64, retarget/land #80.
3. Establish the new `main` integration baseline.
4. Remove forbidden-port defaults and create the completion ledger.
5. Pin CI/toolchain inputs and add provenance.
6. Build the pinned-upstream MeshCore oracle.
7. Establish a single-owner Mesh runtime.
8. Complete semantic conformance.
9. Implement truthful DM ACK/retry/delivery.
10. Complete canonical contacts and multi-channel support.
11. Implement real PATH/direct fallback/trace.
12. Complete retained durability, settings schema, and time.
13. Finish Wi-Fi reconnect and Map.
14. Extract the UI incrementally and finish workflows.
15. Implement authenticated repeater/room administration.
16. Add BLE, notifications/emoji, terminal, and signed update/recovery.
17. Run the full exact-commit release matrix.
18. Tag and publish Stable Core and then Full Feature Completion.

Do not stack new broad work on the old PR hierarchy.

## Operating loop

Repeat this loop until the full-feature release is tagged:

### Step 1 — Refresh state

- fetch current `main`, open PRs, issues, workflow results, and the completion ledger;
- verify branch ancestry;
- verify the exact pinned MeshCore and SDK versions;
- identify the highest-priority unblocked work package;
- identify which evidence remains valid for the current SHA.

### Step 2 — Decompose

For the selected work package, write a short implementation contract containing:

- problem;
- invariant;
- exact files/owners;
- behavior changed;
- behavior intentionally unchanged;
- tests;
- Actions jobs;
- hardware/RF gate;
- evidence artifact;
- rollback.

Use sub-agents for independent, bounded tasks:

- **Protocol agent:** upstream behavior, vectors, session state.
- **Runtime/storage agent:** ownership, queues, persistence, reboot.
- **UI agent:** view models, simulator, LVGL lifetime.
- **QA/release agent:** tests, workflow, evidence, docs/security.

Do not let multiple agents edit the same hotspot simultaneously. Have research/review agents return findings before one designated implementation owner writes the patch.

### Step 3 — Implement the smallest coherent slice

- preserve compatibility wrappers;
- avoid file-wide rewrites when an extraction can be staged;
- add tests before or with behavior;
- add telemetry for every new state machine;
- fail closed on unknown state/input;
- do not expose the UI action until the backend contract is complete.

### Step 4 — Verify locally without firmware building

Run applicable host work:

- `python -m pytest tests`
- simulator screenshot/overflow/touch-target checks
- parser/conformance/sanitizer/fuzz harnesses
- dry-run hardware scripts
- ledger/release-gate validators
- static checks and generated-file consistency

Do not use a local ESP-IDF/RP2040 build as evidence.

### Step 5 — Push and use GitHub Actions

- open/update the issue-scoped PR;
- wait only through actual tool execution in the current run; do not promise later work;
- inspect every required job and log;
- fix failures at their root;
- download/verify the exact artifacts when physical proof is required.

### Step 6 — Physical gate only when needed

Generate a precise operator runbook containing:

- source SHA;
- Actions run;
- artifact/checksum;
- explicit allowed ports;
- exact command;
- expected safe state;
- prohibited actions;
- evidence output path;
- pass/fail checks;
- restore procedure.

Never select COM8, COM11, or COM29.

After results arrive:

- verify the receipt matches the exact SHA;
- inspect crash/health before clearing anything;
- update ledger;
- do not close the issue if any required branch is missing.

### Step 7 — Merge and rebaseline

- merge only with required checks and evidence;
- retarget dependent PRs;
- rerun Actions on the new base;
- perform the smallest required exact-merged-SHA proof;
- delete/archive superseded branches only after checking for unique work;
- update README, known limitations, roadmap, and ledger.

### Step 8 — Continue

Select the next unblocked package. Do not end with “the next step is…” while unblocked implementation work remains.

## Required architecture rules

### Mesh runtime

- SX1262 callbacks copy bounded data, enqueue, and return.
- One Mesh runtime task owns protocol, contacts, channels, DM sessions, routes, trace, and admin sessions.
- Queue saturation is a visible result, not a silent drop.
- All queues report depth, high-water, drops, and last error.
- UI/console consume immutable snapshots and issue commands.

### DM

Implement explicit states:

- queued;
- waiting radio;
- transmitting;
- RF complete;
- awaiting ACK;
- acknowledged;
- retry wait;
- retrying;
- failed radio;
- failed timeout;
- failed queue;
- interrupted by reboot;
- cancelled where supported.

Inbound valid DMs must generate upstream-compatible ACK or ACK+PATH. Invalid/replayed payloads must not create visible messages or incorrect ACKs. Duplicates must not create duplicate rows or ACK storms.

### Contacts/channels

- full public key is canonical identity;
- heard fingerprints alone are not sendable contacts;
- signed advert/import promotes a contact;
- role controls available actions;
- Public is an explicit channel, not a hard-coded special case;
- channel secrets never enter logs/screenshots/exports.

### Paths/trace

- use real upstream PATH/path-discovery/trace semantics;
- direct route must expire/fail over;
- old `trace_<token>` text DM remains only a labelled test probe until deleted;
- do not identify hash hops without verified mappings.

### Persistence

- one scheduler owns coalescing;
- every store uses explicit schema, length, revision/epoch, checksum, and lineage;
- unknown newer schema is preserved/quarantined;
- media generation is rechecked before replace;
- global reboot quiescence uses one deadline;
- no hot callback performs full synchronous persistence;
- power-loss tests accept only old-valid or new-valid state.

### UI

- one UI task owns LVGL;
- controllers consume immutable view models;
- async responses carry generation tokens;
- screen destroy removes timers/callbacks;
- no direct radio/SD/HTTP operation from an LVGL callback;
- extract screen by screen, not a big-bang rewrite.

### Wi-Fi/Map

- bounded reconnect/backoff;
- credential redaction;
- current-view-only Map;
- max visible 3×3 tile plan;
- explicit User-Agent and visible attribution;
- no bulk/offline-area prefetch from the default OSM tile service;
- cancel work when Map is no longer visible;
- signed peer-advert pins only;
- no onboard GPS claim.

### Admin

- authenticated, correlated session;
- allowlisted operations;
- timeout/replay/failed-login handling;
- local confirmation for mutating actions;
- no raw remote command tunnel;
- credentials never logged.

### BLE

- use framed compatibility layer;
- bound fragmentation and malformed input;
- explicit pairing/bond policy;
- prove Wi-Fi/BLE/radio memory coexistence or use truthful mode switching.

### Update/recovery

- do not bolt OTA onto the current single-factory partition;
- make a partition/size decision first;
- do not rewrite partition table in normal app execution;
- signed target-specific manifest;
- SHA-256;
- rollback and recovery;
- power-loss testing;
- preserve identity/settings/history;
- no silent RF-triggered update.

## Release-gate behavior

A work package is `complete` only when:

- implementation is merged;
- required host/Actions checks pass;
- required physical/RF evidence matches merged SHA;
- docs/ledger/known limitations are updated;
- no dependent invariant is broken;
- issue closure condition is satisfied.

The Stable Core release may exclude only features explicitly moved to Full Feature Completion and hidden/labelled accordingly.

The Full Feature release cannot be tagged until:

- zero open P0;
- zero known crash/data-loss/security P1;
- full semantic MeshCore conformance;
- truthful DM;
- contacts/channels/PATH/trace/admin complete;
- SD/retained/Wi-Fi/Map complete;
- modular stable UI;
- BLE, terminal, notification/emoji, signed update/recovery complete;
- exact final hardware/RF/SD/update/soak evidence;
- reproducible package, checksums, SBOM, provenance and public docs.

## Required final outputs

At the end of Stable Core and Full Feature milestones, produce:

1. exact source/tag SHA;
2. release package and checksums;
3. SBOM and provenance;
4. complete evidence index;
5. release validation report;
6. install, upgrade, rollback and recovery guide;
7. user guide;
8. developer architecture guide;
9. supported feature/capability matrix;
10. known limitations;
11. closed/open issue summary;
12. post-release support and patch plan.

## Stop condition

You may stop only when one of these is true:

1. the Full Feature release is tagged and every applicable gate is green; or
2. a genuinely external prerequisite prevents all remaining work.

For case 2, produce a machine-readable blocker receipt naming:

- exact blocked work package;
- missing external input;
- why source/host work cannot continue;
- last valid SHA/artifacts;
- exact operator action required;
- every still-unblocked task already completed.

A single missing physical test, credential, peer, or product decision is not permission to stop while other unblocked work exists.

## First action

Refresh GitHub state, read the master roadmap/backlog, and select WP-01 unless it has already been proven and merged. Preserve current evidence before changing any storage/RP2040/reboot code.
