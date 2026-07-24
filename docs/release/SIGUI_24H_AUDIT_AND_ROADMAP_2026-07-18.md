# SIGUI Repository Audit and 24-Hour Core Production Release Roadmap

**Repository:** `n30nex/SIGUI`
**Audit date:** 2026-07-18
**Audited default branch:** `main`
**Audited head:** `846f728dd3faded85451c6d39ba6a07cb8ca7f44`
**Latest code-bearing baseline described by the repository handoff:** `bd6ea0e685442d8a820766f4686395e50ca5397f` through PR #198
**Latest head change:** documentation/completion handoff through PR #192
**Timebox:** hard cap of 24 elapsed hours from execution start
**Recommended release identity:** **MeshCore DeskOS D1L Core 1.0**, tag `v1.0.0`, with package field `release_profile=core_1_0`

> This is a source, issue, pull-request, CI, workflow, and release-contract audit. It does not claim that a new firmware image was built, flashed, or physically qualified during this audit.

### 2026-07-24 execution-target correction

The operator has moved the release-closing D1L to Raspberry Pi 5 host
`neopi5`. Current hardware execution uses the unprivileged, key-only account
`siguidev` and only the stable selector
`/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`, after proving USB VID:PID
`1A86:7523`. The present `/dev/ttyUSB2` resolution is observational only and
must never identify release evidence. `COM12` remains the valid Windows
alternative; existing COM12 evidence is retained as historical evidence for
its named candidate, not rewritten as Pi evidence. `COM8`, `COM11`, and
`COM29` remain forbidden. `COM16` remains restricted to separately authorized
SD/RP2040 work and is never the Core D1L target.

This correction changes the route, not the release standard. Exact-SHA
Actions/package identity, non-erasing flash, UI/manual review, reboots and
retained-state proof, protocol-time migration, controlled RF/DM, 60-minute
active plus 30-minute idle soak, installation review, and final audit remain
fail-closed. Controlled-peer RF/soak is not ready until narrowly scoped peer
status/control access for `siguidev` is provisioned and verified.

---

## Executive decision

The repository is not a nearly finished release that only needs a shorter soak. It is a strong and heavily tested firmware codebase trapped behind an oversized “Full Feature Completion” release contract.

The current contract combines all of the following into one release:

- core MeshCore messaging;
- direct-message reliability and official-peer interoperability;
- multi-channel management;
- contacts and routes;
- authenticated repeater/room administration;
- Map, Wi-Fi, SD cache, and location;
- BLE companion transport;
- signed SD/OTA update, rollback, and recovery;
- broad physical UI evidence;
- a 12-hour soak;
- 21 open P0 issues and multiple umbrella work packages.

That contract cannot be implemented and honestly qualified in less than 24 hours. The only credible path to a public production release inside the requested timebox is to **change the product contract**, freeze a smaller supported surface, hide or reject unfinished features, and qualify that exact surface once.

The recommended target is a stable **Core 1.0** release with:

- board, display, touch, backlight, Home, and core navigation;
- default Public-channel messaging;
- direct messages only if exact-candidate controlled-peer acceptance passes;
- basic verified contacts and heard-node browsing;
- packet diagnostics;
- Canada/USA radio configuration;
- retained NVS storage and reboot persistence;
- optional FAT32 SD-backed history only if the short exact-candidate SD gate passes;
- crashlog, health telemetry, USB recovery, checksums, SBOM, and provenance.

The following must be absent from the supported Core 1.0 surface:

- Map and user-enabled Wi-Fi;
- BLE;
- signed SD/OTA update;
- channel creation/import/export and multi-channel UI;
- repeater/room administration;
- Observer/MQTT;
- mutable terminal/log tools;
- GPS/location workflows;
- advanced QR/emoji workflows;
- user-facing TRACE/PATH tooling beyond internal routing needed by DM.

This is not a recommendation to delete those implementations. They remain in the development branch for later releases. They must simply have no reachable production UI or mutating command path in Core 1.0.

A 24-hour release remains a risk-managed target, not a guarantee. Any core crash, data-loss defect, security defect, exact-candidate RF failure, or inability to produce reproducible artifacts remains a hard no-go.

---

## 1. Repository state at audit time

### 1.1 What is already strong

The project has substantially more production infrastructure than a typical embedded firmware repository:

- ESP-IDF is pinned to `v5.5.4` by exact container digest.
- The build rejects a changed generated dependency lock.
- Host dependencies are hash locked.
- Actions produce firmware, package, checksums, provenance, SPDX SBOM, and conformance evidence.
- The conformance job runs signed-advert checks, parser tests, sanitizers, and large deterministic fuzz suites.
- Firmware builds are already Actions-only.
- Serial scripts bind evidence to exact commits and reject forbidden ports.
- The app is decomposed into app, comms, diagnostics, HAL, Map, platform, MeshCore, storage, and UI modules.
- Failure telemetry is unusually rich: crashlog, boot nonce, heap, PSRAM, task stack, LVGL, route flush, retained stores, and storage state.
- Recent work added bounded UI stress, post-flash matrices, TRACE lifecycle fixes, CI de-duplication, and controlled-peer RF evidence policy.

This foundation should be preserved. The 24-hour plan reduces duplicate execution and product scope; it does not remove the strongest automated safety checks.

### 1.2 Current release position

The repository’s current handoff reports:

- capability implementation: approximately 80%;
- last strict verified weighted progress: 74%;
- current fail-closed live score: 64%;
- final release gates: 0 of 11 green;
- strict audit at the last banked candidate: 6 pass / 30 fail;
- 28 failed P0 gates and 2 failed P1 gates;
- 21 open P0 issues plus other open P1/P2 work.

The latest code-bearing main checkpoint has a passing Actions run, but its downloaded/checksum-verified exact artifact bank and frozen-candidate physical evidence are still missing.

### 1.3 Open pull requests

#### PR #197 — retain

`fix(storage): suppress unchanged retained NVS commits`

This is a small, bounded durability improvement. It avoids repeated `nvs_set_blob()` / `nvs_commit()` when the retained payload is unchanged, preserves write-attempt telemetry, and adds a native test proving repeated duplicate writes do not create new commits. It was green before main advanced and now needs a clean rebase.

**24-hour decision:** rebase or cherry-pick this patch early, retain its focused tests, and include it in the one integration candidate.

#### PR #199 — do not merge into Core 1.0

`feat(ble): add secure companion transport foundation`

The draft adds a large NimBLE foundation, queueing, secure-connection policy, and coexistence work. Host and conformance jobs passed, but the ESP-IDF firmware build failed. Even after compilation is repaired, the PR still lacks single-owner Mesh runtime integration, bond/PIN management, official-client hardware acceptance, coexistence proof, repeated connection-cycle proof, and soak evidence.

**24-hour decision:** leave the PR open, rebase it after release, and mark BLE unavailable in Core 1.0. Do not spend the release sprint finishing it.

---

## 2. Audit findings

### Finding A — the release contract is the primary blocker

**Severity:** P0 process/product blocker

The current roadmap explicitly makes BLE, signed update/recovery, Map, multi-channel behavior, administration, and a broad exact-candidate evidence matrix part of the same full release. The root Codex bootstrap instructs the agent not to stop until the complete Full Feature Completion release is tagged.

This guarantees continued expansion instead of release convergence.

**Required correction:** merge an explicit Core 1.0 product contract. The old full-feature contract remains the roadmap for later releases, not the gate for Core 1.0.

---

### Finding B — the existing release audit is intentionally too broad for the 24-hour target

**Severity:** P0 release-engineering blocker

The existing release gate:

- hard-codes a 12-hour soak;
- requires full Map/Wi-Fi/SD surfaces;
- requires broad compose and scroll surfaces;
- requires full RF evidence;
- requires manual evidence;
- requires many SD-specific artifacts;
- includes a separate full MeshCore conformance closure gate beyond the already strong wire/conformance package.

It is correct for the current Full Feature contract. Weakening it in place would destroy audit meaning.

**Required correction:** add a separate profile-aware audit:

```text
scripts/core_release_gate_audit_d1l.py
```

or add a strictly additive profile mechanism:

```text
python scripts/release_gate_audit_d1l.py --profile core_1_0 ...
```

The full default profile must retain every existing gate and the 12-hour soak. `core_1_0` must have a separate gate list, separate result key, separate tests, and a package-visible profile identity.

Required report truth:

```json
{
  "release_profile": "core_1_0",
  "core_release_ready": true,
  "full_feature_release_ready": false
}
```

Never map a Core result into the existing `ready_for_public_release` field without the profile identity.

---

### Finding C — document and evidence drift is creating false workload

**Severity:** P1 process blocker

The current handoff and completion ledger are updated through the newest work, while the test-plan header and portions of the release checklist still describe older checkpoints. The checklist is a useful audit history, but it is not a usable one-day operational checklist.

The issue tracker also contains umbrella P0s whose implementation is advanced or substantially complete but whose physical acceptance remains open. Counting all open P0 labels as equal implementation work produces the wrong execution order.

**Required correction:**

1. Preserve historical documents.
2. Add one short Core 1.0 release checklist generated from the new profile.
3. Classify every open P0 as one of:
   - `core-blocker`;
   - `full-feature-deferred`;
   - `evidence-only`;
   - `stale/close after reconciliation`.
4. Do not rewrite years of evidence during the sprint.
5. Update only the exact candidate’s product contract, capability manifest, known limitations, release notes, and evidence index.

---

### Finding D — CI cost is not the main problem, repeated CI is

**Severity:** P1 execution inefficiency

The current workflow runs:

- the entire host suite;
- completion-ledger checks;
- simulator and dry-run validators;
- checksum tests;
- signed-advert checks;
- multiple 100,000-input fuzz gates;
- remaining fuzz targets;
- conformance;
- the ESP-IDF build and release package.

That is valuable once per release candidate. It is wasteful when every small release-sprint PR triggers the entire workflow.

**Required correction:**

- Use one integration branch and one integration PR.
- Sub-agents run focused host tests in separate worktrees.
- The lead cherry-picks reviewed commits.
- Run the complete host suite once immediately before candidate push.
- Run the existing full Actions workflow once on the frozen candidate.
- Run it again only if code, build inputs, dependency lock, release scripts, or package contents change.
- Documentation-only evidence receipts after candidate freeze must be stored outside the firmware source commit or prepared before freeze; do not change the candidate SHA after hardware qualification.

Do not spend the sprint designing a new CI platform unless the existing workflow itself is broken.

---

### Finding E — the exact-candidate physical gap is real

**Severity:** P0 release blocker

Most remaining failures are no longer “code does not exist.” They are “the exact final binary has not been shown to work on the device and with a controlled peer.”

The Core release still cannot waive:

- exact Actions artifact identity;
- non-erasing flash bound to the qualified D1L target (`neopi5` stable by-id
  for the current route, or `COM12` on the Windows alternative);
- board/display/touch readiness;
- boot and reboot stability;
- retained-state persistence;
- controlled inbound/outbound DM, ACK/PATH, and direct-route behavior;
- absence of new crash-like resets;
- bounded memory and task-stack health;
- user-visible UI review;
- recovery/installation instructions.

These are the smallest meaningful production gates.

---

### Finding F — SD can fit the timebox only as a conditional capability

**Severity:** P0 if advertised as supported; non-blocking if disabled

Issue #78 remains open because a physically inserted card has intermittently appeared absent. Substantial exact-device work already exists: bounded recovery, asynchronous remount handling, file canaries, retained data across reboot, generation fencing, and short stable windows. The unresolved portion is exact-candidate physical removal/reinsertion and a longer active/idle window.

**Required release rule:**

- Start the candidate with `sd_history=conditional`.
- Run the exact-candidate SD mini-matrix.
- If every SD gate passes, package Core 1.0 with `sd_history=supported_optional`.
- If any SD gate fails or cannot be executed, set `sd_history=disabled`, hide SD data controls, use NVS fallback, omit RP2040 release artifacts, and state that SD support returns in a later release.
- Never publish `sd_history=supported` based on predecessor evidence.
- Never format a card.

This prevents SD from consuming the entire deadline while preserving a path to include it.

---

### Finding G — Map and Wi-Fi should be deferred together

**Severity:** P0 if advertised; non-blocking if unreachable

Wi-Fi memory/boot-loop corrections have strong predecessor evidence, but final physical Map entry, live tile behavior, cache reuse, cancellation, framebuffer responsiveness, and exact-candidate qualification remain open. Map has also shown severe latency on the current RP2040 SD transport.

Because Map depends on Wi-Fi, SD, tile transport, UI responsiveness, and location truth, it multiplies the hardware matrix.

**24-hour decision:** hide Map and Wi-Fi setup in Core 1.0, reject mutating Wi-Fi commands under the Core profile, keep Wi-Fi runtime off, and leave the implementation for the next release.

Read-only `wifi status` may remain in diagnostics if it clearly reports `unsupported_in_release_profile`.

---

### Finding H — do not refactor the entire UI before release

**Severity:** P1 scope risk

`ui_phase1.c` remains in the source list, and issues #6/#16 call for deeper modularization. At the same time, the repository now contains modular controllers, view models, screen files, and a 1,000-transition host stress gate.

A broad UI refactor inside the release sprint would invalidate a large amount of evidence and create new lifecycle risk.

**24-hour decision:** add a central release-profile capability table and hide unsupported destinations. Make only bounded fixes found by the core UI probe. Schedule structural UI cleanup after the release.

---

## 3. Core 1.0 product boundary

The authoritative version is the separate `SIGUI_CORE_1_0_PRODUCT_CONTRACT_2026-07-18.md` in this pack.

### Supported and release-blocking

1. D1L board initialization, stable 480×480 display, touch, backlight.
2. Home screen and navigation among supported destinations.
3. Default Public channel read/compose/send/receive.
4. Direct-message thread, outbound/inbound DM, ACK/PATH, retry/failure truth.
5. Basic verified contact/heard-node selection needed to start a DM.
6. Nodes list and bounded node details.
7. Packet log and read-only route/signal diagnostics.
8. Canada/USA default radio profile and explicit radio settings.
9. Identity generation, retention, and signed advert handling already in the exact CI package.
10. Retained NVS state across controlled reboot and non-erasing upgrade.
11. Crashlog, health, boot nonce, heap/PSRAM/task/LVGL telemetry.
12. USB install, non-erasing upgrade, and recovery documentation.
13. Exact package checksums, provenance, SBOM, license notices.
14. Optional FAT32 SD history only if its candidate-specific gate passes.

### Deferred and unreachable

1. BLE.
2. Map.
3. User-enabled Wi-Fi.
4. Multi-channel create/import/export/select UI.
5. Repeater/room administration.
6. Observer/MQTT.
7. Signed SD update or OTA.
8. GPS/location workflows.
9. Mutable terminal/log UI.
10. Advanced QR/emoji workflows.
11. User-facing TRACE tooling not required by the internal DM route.
12. Any dead button or menu that suggests these features work.

### Important implementation rule

“Deferred” means all of the following:

- no Home card;
- no dock destination;
- no settings entry;
- no enabled action button;
- no mutating USB command;
- no background task or network stack started by the release profile;
- package manifest says unavailable;
- release notes say deferred.

Merely adding “experimental” text to a reachable, unsafe action is not sufficient.

---

## 4. Required implementation changes

### 4.1 Central release profile

Add:

```text
main/app/release_profile.h
main/app/release_profile.c
```

Recommended shape:

```c
typedef enum {
    D1L_RELEASE_PROFILE_DEVELOPMENT = 0,
    D1L_RELEASE_PROFILE_CORE_1_0,
    D1L_RELEASE_PROFILE_FULL_FEATURE,
} d1l_release_profile_id_t;

typedef struct {
    bool public_messages;
    bool direct_messages;
    bool nodes;
    bool packets;
    bool radio_settings;
    bool retained_nvs;
    bool sd_history;
    bool map;
    bool wifi_user_control;
    bool ble;
    bool multi_channel_management;
    bool admin;
    bool observer_mqtt;
    bool signed_update;
    bool mutable_terminal;
    bool location;
    bool advanced_qr_emoji;
    bool user_trace;
} d1l_release_capabilities_t;
```

The profile must be selected by a build-time constant and exposed in:

- `version`;
- `health`;
- app snapshot;
- UI capability projection;
- USB command admission;
- package manifest;
- SBOM/provenance metadata where appropriate;
- release audit.

Do not duplicate feature booleans across unrelated modules. One immutable capability table is the authority.

### 4.2 UI admission

For Core 1.0:

- dock/Home routes: Home, Messages, Nodes, Packets, Settings;
- remove Map;
- remove Wi-Fi/BLE/Map/Admin/Observer/Update/Terminal/Channel-management actions;
- keep read-only diagnostic truth where useful;
- unsupported deep-link/navigation requests return a bounded error and remain on the current screen;
- no placeholder screen may allocate background work.

### 4.3 USB command admission

Permit read-only status commands even for unavailable features when they report the profile truth.

Reject mutating unavailable commands before side effects:

- `wifi on`, `wifi connect`, `wifi save`, `wifi scan` under Core;
- `ble on` and all BLE pairing/bond actions;
- Map fetch/cache/location mutations;
- channel create/import/export/remove/select beyond the fixed default;
- admin mutations;
- observer/MQTT enable;
- update/install-from-SD/OTA;
- terminal mutation commands not explicitly in the Core support list.

The rejection response must include:

```json
{
  "ok": false,
  "code": "ESP_ERR_NOT_SUPPORTED",
  "release_profile": "core_1_0",
  "feature": "<feature-id>"
}
```

### 4.4 Release package and documentation

Add to the package manifest:

- `release_profile`;
- `supported_capabilities`;
- `unavailable_capabilities`;
- `sd_history_mode`;
- `full_feature_release_ready=false`;
- exact firmware commit and Actions run;
- minimum installation/recovery guide version.

Generate a human-readable `SUPPORTED_FEATURES.md` into the package.

### 4.5 Core-specific test and audit tools

Add, without weakening existing tools. The Core UI probe must use only
`home,messages,nodes,packets,settings`; the existing full probe keeps Map in its
default sequence and therefore must remain unchanged for the Full Feature profile.

```text
scripts/core_smoke_d1l.py
scripts/core_ui_corruption_probe_d1l.py
scripts/core_release_gate_audit_d1l.py
tests/test_release_profile_contract.py
tests/test_core_surface_contract.py
tests/test_core_release_gate_audit_d1l.py
tests/test_core_package_manifest.py
```

The core smoke must test only supported functionality and must also prove that unavailable mutating paths are rejected.

### 4.6 NVS duplicate-write suppression

Rebase/cherry-pick PR #197. Keep its focused native test and telemetry semantics.

### 4.7 BLE

Do not merge PR #199. Add a release-note entry and ensure the Core package reports BLE unavailable.

---

## 5. Team-of-sub-agents execution model

The lead agent owns integration and never delegates final product decisions.

### Lead / Integrator

Owns:

- `release/24h-core` branch;
- release profile contract;
- integration order;
- `main/app/release_profile.*`;
- candidate freeze;
- final Actions run;
- exact-candidate ledger;
- tag/release decision.

The lead must not make broad concurrent edits while sub-agents edit the same files.

### Agent A — product surface and UI

Owns:

- UI route visibility;
- Home/dock/settings feature admission;
- deep-link denial;
- simulator references;
- core UI tests.

Hotspot ownership:

- `main/ui/**`
- only Agent A edits `main/ui/ui_phase1.c` during the sprint.

### Agent B — persistence and storage

Owns:

- PR #197 rebase/cherry-pick;
- retained NVS tests;
- SD conditional-mode plumbing;
- NVS fallback truth;
- storage feature manifest.

Hotspot ownership:

- `main/storage/retained_blob_store.*`
- no other agent edits these files.

### Agent C — MeshCore core behavior

Owns:

- review of DM/ACK/PATH/retry code against current tests;
- focused fixes needed by controlled-peer acceptance;
- basic contact-to-DM path;
- no public-network automation;
- exact RF acceptance runner readiness.

Hotspot ownership:

- `main/mesh/**`
- one named implementation owner for `meshcore_service.c`.

### Agent D — command and capability admission

Owns:

- USB command allow/deny table;
- profile-aware app snapshot;
- version/health profile fields;
- unsupported-feature result contract;
- source tests proving no side effects.

Hotspot ownership:

- `main/comms/usb_command_parser.*`
- `main/comms/usb_console.*`
- coordinates with lead before editing `main/app/app_model.*`.

### Agent E — QA, package, and release audit

Owns:

- `core_smoke_d1l.py`;
- core release audit;
- package manifest/profile binding;
- focused and final test command list;
- release evidence index;
- generated support matrix.

Hotspot ownership:

- `scripts/package_release_d1l.py`;
- new core audit scripts;
- no firmware runtime files.

### Agent F — independent review and operator evidence plan

Owns:

- diff review;
- forbidden-port policy review;
- candidate evidence command sheet;
- hardware receipt validation;
- no implementation edits unless the lead assigns a narrow runner defect.

This agent must report blockers, not convert missing physical evidence into pass claims.

### Concurrency rules

- Each sub-agent uses its own worktree and branch.
- Each work package produces one small reviewed commit.
- No sub-agent rebases main.
- No sub-agent edits completion ledgers or release status independently.
- The lead cherry-picks after tests and diff review.
- Shared hotspots have one owner.
- One integration PR is used for the sprint.
- Every agent is dismissed after its commit/review is accepted.

---

## 6. 24-hour execution schedule

These are maximum elapsed-hour bands, not promises. Move faster when gates pass; never use unused time to add features.

### H0.0–H0.5 — freeze the decision

1. Create `release/24h-core` from current `main`.
2. Record exact starting SHA.
3. Merge the Core 1.0 product contract.
4. Mark PR #199 deferred.
5. Define SD as conditional.
6. Create a live `docs/release/24H_STATUS.md`.
7. Assign file ownership to agents.

**Exit:** no ambiguity about supported features.

### H0.5–H3.0 — parallel implementation

- Agent A: hide/deactivate unsupported UI.
- Agent B: rebase/cherry-pick #197; add SD conditional mode.
- Agent C: run DM-focused tests; repair only confirmed core failures.
- Agent D: command/profile admission.
- Agent E: core audit/package/test support.
- Agent F: independent baseline review and hardware command sheet.

**Exit:** all work packages have focused tests and review notes.

### H3.0–H5.0 — integration and local validation

1. Lead cherry-picks in dependency order.
2. Resolve conflicts centrally.
3. Run:
   - focused tests for every changed subsystem;
   - profile/package/audit tests;
   - `git diff --check`;
   - completion/package manifest validation;
   - full `python -m pytest tests -q` once.
4. Generate simulator references for supported screens.
5. Fix only failures caused by the candidate.

**Exit:** one clean integration commit; no known core software P0.

### H5.0–H7.0 — one full candidate Actions run

Use the existing workflow.

- `include_sd_bridge=false` initially.
- Use `include_sd_bridge=true` only after deciding to qualify SD in the same candidate.
- Require host, conformance, firmware build, package, checksums, provenance, and SBOM.
- Download all artifacts and verify every checksum.
- Record run ID, exact SHA, image digest, package digest, and manifests.

**Exit:** frozen downloadable candidate.

### H7.0–H9.0 — exact flash and core physical sweep

1. On `neopi5`, prove the stable by-id link, USB identity `1A86:7523`, and
   read/write access, then perform the non-erasing exact Actions flash. Use
   `COM12` only if the D1L has been intentionally moved to the Windows
   alternative.
2. Confirm `version` exact 40-hex SHA, IDF `v5.5.4`, and `release_profile=core_1_0`.
3. Run profile-aware core smoke with persistence.
4. Manually confirm display bars and touch.
5. Run 20 UI corruption/navigation rounds on supported screens.
6. Run supported compose and scroll surfaces.
7. Run five software reboots and three cold power cycles.
8. Confirm settings/messages/contact state and boot nonce transitions.
9. Capture health/crashlog before and after.

**Exit:** exact binary survives the basic device matrix.

### H9.0–H10.5 — controlled RF/DM

1. Use only the qualified D1L target: the `neopi5` stable by-id link for the
   current route, or `COM12` on the Windows alternative.
2. Before any RF command, prove that `siguidev` has narrowly scoped access to
   the exact local peer status/control resources. SSH and D1L serial access do
   not satisfy this prerequisite.
3. Use one explicitly assigned, distinct, non-forbidden controlled peer.
4. Run exact-candidate full RF acceptance:
   - identity;
   - outbound DM;
   - inbound DM;
   - ACK/PATH;
   - direct route;
   - retained thread;
   - health;
   - `public_rf_tx=false`.
5. Exercise one absent-peer/final-failure case if the existing runner supports it without broad code changes.
6. Do not automate default Public-channel transmission. Use controlled DM or the configured `#test` channel only.

**Exit:** DM is supported, or the release stops. DM is part of Core 1.0 and cannot be papered over.

### H10.5–H12.0 — SD decision

When SD is being considered:

1. Use exact paired ESP32/RP2040 artifacts.
2. Verify ready FAT32 state and file canary.
3. Reboot/remount.
4. Physically remove card and prove truthful no-card.
5. Reinsert and prove fresh ready state.
6. Verify retained data and no formatting.
7. Run a 30-minute idle/storage sample as part of the final soak.

If any check fails or cannot be performed, flip the final candidate to `sd_history=disabled`, remove the SD release package, rerun only the tests/build affected by that profile change, and proceed with NVS.

**Exit:** one truthful SD support decision.

### H12.0–H14.0 — minimum production soak

Run on the exact final SHA:

- 60-minute mixed active soak with controlled DM traffic and five-minute samples;
- immediately followed by a 30-minute idle/listening soak.

Require:

- no crash-like reset;
- monotonic uptime;
- board/UI/Mesh ready throughout;
- no command timeout or unexpected console restart;
- retained task stack at least 4096 bytes;
- no queue saturation/failure;
- no negative unbounded heap/PSRAM trend;
- successful controlled DM deltas;
- stable NVS/SD backend according to final profile;
- no SD stale/presence-stale/refresh failure if SD is supported.

This 90-minute gate is an explicit Core 1.0 risk acceptance. It does not satisfy or replace the Full Feature 12-hour gate.

### H14.0–H20.0 — bounded repair loop

Only when a Core gate fails:

1. Record exact failure artifact.
2. Assign one owner.
3. Make the smallest fix.
4. Run focused tests.
5. Run the full suite once.
6. Produce a new Actions candidate.
7. Repeat only the directly affected hardware gate plus the full 90-minute soak for any runtime code change.

Do not add deferred features.

### H20.0–H23.0 — final audit and publication

1. Run `core_release_gate_audit_d1l.py`.
2. Require `core_release_ready=true`.
3. Preserve `full_feature_release_ready=false`.
4. Verify package contents, checksums, provenance, SBOM, notices, support matrix, install/recovery guide, and known limitations.
5. Tag exact commit.
6. Create GitHub release using the exact Actions package.
7. Publish SHA-256 values and release profile.
8. Close only issues truly satisfied; relabel deferred work.

### H23.0–H24.0 — contingency only

Use this band only for artifact verification, release-note correction that does not change firmware, or a final no-go decision. Do not start another feature.

---

## 7. Minimal test policy

### 7.1 Per sub-agent commit

Required:

- subsystem-focused tests;
- new contract tests;
- native compilation for changed C/C++ modules where already supported;
- `git diff --check`.

Not required:

- entire host suite;
- full fuzz suite;
- firmware build;
- RP2040 build;
- hardware sweep.

### 7.2 Before candidate push

Required once:

```powershell
python -m pytest tests -q
python scripts/completion_ledger.py validate --check-generated
python scripts/completion_pack_manifest.py check
python scripts/core_release_gate_audit_d1l.py --dry-run
git diff --check
```

Adjust command names only to match the implementation actually added.

### 7.3 Frozen candidate CI

Keep the existing full host, sanitizer, fuzz, conformance, firmware, package, checksum, provenance, and SBOM jobs.

Do not reduce fuzz counts for the final candidate. They already run in a bounded CI job and are not the 24-hour bottleneck.

### 7.4 Exact-candidate hardware minimum

Mandatory Core gates:

1. exact flash receipt;
2. core smoke and persistence;
3. display/touch manual confirmation;
4. 20-round supported UI corruption/navigation;
5. supported compose/scroll check;
6. five software reboots and three cold boots;
7. controlled RF/DM acceptance;
8. 60-minute active + 30-minute idle soak;
9. install/recovery instructions checked against package.

Conditional SD gates:

1. exact paired artifacts;
2. ready/card/file canary;
3. reboot/remount;
4. removal/reinsertion;
5. 30-minute stable storage window;
6. no-format evidence.

Explicitly not required for Core 1.0:

- 12-hour soak;
- 500-cycle or 1,000 physical UI abuse;
- BLE connection cycles;
- Map tile performance;
- Wi-Fi AP/reconnect matrix;
- OTA/update/rollback matrix;
- full multi-channel matrix;
- admin mutation matrix;
- observer/MQTT matrix;
- RP2040 test when SD is disabled.

---

## 8. Candidate commands

Use the repository’s scripts and exact candidate identifiers. Replace
placeholders only with recorded values. On the current Pi route, establish the
stable selector once per shell and fail before any script unless all checks
pass:

```bash
export D1L_PORT='/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
test -L "$D1L_PORT" && test -r "$D1L_PORT" && test -w "$D1L_PORT"
D1L_DEVICE_PROPERTIES="$(udevadm info --query=property --name="$D1L_PORT")"
grep -qx 'ID_VENDOR_ID=1a86' <<<"$D1L_DEVICE_PROPERTIES"
grep -qx 'ID_MODEL_ID=7523' <<<"$D1L_DEVICE_PROPERTIES"
export D1L_COMMIT='<40-hex-sha>'
export D1L_ACTIONS_RUN='<numeric-run-id>'
export D1L_RUN_ATTEMPT='<numeric-attempt>'
export D1L_PUBLIC_KEY='<64-hex-D1L-public-key>'
export D1L_PEER_FINGERPRINT='<16-hex-controlled-peer>'
export D1L_CONTROLLED_PEER_RECEIPT='<qualified-controlled-peer-receipt.json>'
```

If the device is intentionally moved to Windows, set
`$env:D1L_PORT = "COM12"` instead. Never substitute `/dev/ttyUSB2` or another
raw `/dev/ttyUSB*` name.

### Actions

```powershell
gh workflow run d1l-ci.yml --ref release/24h-core -f include_sd_bridge=false
gh run watch <run-id> --exit-status
python .\scripts\capture_core_actions_run_d1l.py `
  --github-run-id <run-id> `
  --commit <sha> `
  --github-run-dir artifacts\github\<run-id>-<sha>
python .\scripts\verify_checksums.py artifacts\github\<run-id>-<sha>\d1l-firmware-artifacts
python .\scripts\verify_checksums.py artifacts\github\<run-id>-<sha>\d1l-release-package
```

Only a successful `workflow_dispatch` run whose `head_sha` is the exact
candidate is qualifying release evidence. A pull-request run is useful CI, but
its synthetic merge SHA is not flashable candidate evidence. The capture tool
requires exactly the five Core archives, verifies their GitHub API digests and
extracted trees, and records the run attempt. Use `include_sd_bridge=true` only
for a candidate that will advertise supported SD history.

### Core smoke

After adding the profile-aware runner, execute on `neopi5`:

```bash
python ./scripts/core_smoke_d1l.py \
  --port "$D1L_PORT" \
  --expected-firmware-commit "$D1L_COMMIT" \
  --expected-d1l-public-key "$D1L_PUBLIC_KEY" \
  --github-run-id "$D1L_ACTIONS_RUN" \
  --github-run-attempt "$D1L_RUN_ATTEMPT" \
  --expected-sd-history-mode disabled \
  --persistence-test \
  --manual-touch \
  --out artifacts/hardware/neopi5/core_smoke_${D1L_COMMIT}_by-id.json
```

### UI

```bash
python ./scripts/core_ui_corruption_probe_d1l.py \
  --port "$D1L_PORT" \
  --expected-firmware-commit "$D1L_COMMIT" \
  --expected-d1l-public-key "$D1L_PUBLIC_KEY" \
  --github-run-id "$D1L_ACTIONS_RUN" \
  --github-run-attempt "$D1L_RUN_ATTEMPT" \
  --expected-sd-history-mode disabled \
  --rounds 20 \
  --clear-crashlog-before-start \
  --out artifacts/hardware/neopi5/core_ui_corruption_probe_${D1L_COMMIT}_by-id.json

python ./scripts/scroll_probe_d1l.py \
  --port "$D1L_PORT" \
  --release-profile core_1_0 \
  --expected-firmware-commit "$D1L_COMMIT" \
  --expected-d1l-public-key "$D1L_PUBLIC_KEY" \
  --github-actions-run "$D1L_ACTIONS_RUN" \
  --workflow-run-attempt "$D1L_RUN_ATTEMPT" \
  --expected-sd-history-mode disabled \
  --screens home,public_messages,dm_thread,nodes,packets,settings \
  --manual-touch \
  --clear-crashlog-before-start \
  --out artifacts/hardware/neopi5/core_scroll_probe_${D1L_COMMIT}_by-id.json
```

Use the existing compose capture with only Core callers or add a profile-aware
target list.

### RF

Do not run RF acceptance until the narrow local peer access prerequisite is
proven. Use only the runner's reviewed, explicit local controlled-peer mode;
do not invent a peer serial port or SSH back into `neopi5`:

Inspect the merged runner interface first:

```bash
python ./scripts/rf_full_acceptance_d1l.py --help
```

The release invocation must then use the reviewed local controlled-peer mode,
`--port "$D1L_PORT"`, the exact candidate in `$D1L_COMMIT`, and the explicit
peer fingerprint in `$D1L_PEER_FINGERPRINT`. Do not record or run a guessed
argument shape while that local mode or its narrow access is pending.

Never use COM8, COM11, or COM29. Never use COM16 or a peer endpoint as the
Core D1L target. Never automate default Public-channel transmission.

### Active soak

```bash
python ./scripts/soak_d1l.py \
  --port "$D1L_PORT" \
  --expected-firmware-commit "$D1L_COMMIT" \
  --expected-d1l-public-key "$D1L_PUBLIC_KEY" \
  --github-run-id "$D1L_ACTIONS_RUN" \
  --github-run-attempt "$D1L_RUN_ATTEMPT" \
  --expected-release-profile core_1_0 \
  --expected-sd-history-mode disabled \
  --duration-sec 3600 \
  --sample-interval-sec 300 \
  --active-dm-fingerprint "$D1L_PEER_FINGERPRINT" \
  --active-dm-text "core_soak_test_${D1L_COMMIT:0:7}" \
  --active-interval-sec 300 \
  --controlled-peer-receipt "$D1L_CONTROLLED_PEER_RECEIPT" \
  --require-rx-delta \
  --min-rx-delta 1 \
  --min-tx-delta 6 \
  --sample-storage \
  --allow-sd-unavailable \
  --out artifacts/soak/core_active_60m_${D1L_COMMIT}_neopi5_by-id.json
```

For supported SD, replace `--allow-sd-unavailable` with `--sd-file-canary`.
The active soak has the same narrow controlled-peer prerequisite as RF
acceptance.

### Idle soak

```bash
python ./scripts/soak_d1l.py \
  --port "$D1L_PORT" \
  --expected-firmware-commit "$D1L_COMMIT" \
  --expected-d1l-public-key "$D1L_PUBLIC_KEY" \
  --github-run-id "$D1L_ACTIONS_RUN" \
  --github-run-attempt "$D1L_RUN_ATTEMPT" \
  --expected-release-profile core_1_0 \
  --expected-sd-history-mode disabled \
  --duration-sec 1800 \
  --sample-interval-sec 300 \
  --sample-storage \
  --allow-sd-unavailable \
  --out artifacts/soak/core_idle_30m_${D1L_COMMIT}_neopi5_by-id.json
```

For supported SD, replace `--allow-sd-unavailable` with `--sd-file-canary`.

---

## 9. Release gates

### Hard pass

All must be true:

- exact candidate SHA and Actions run;
- full candidate workflow green;
- artifacts downloaded and checksums verified;
- build/profile/package identity aligned;
- Core unsupported paths unreachable and side-effect-free;
- exact non-erasing flash receipt bound to the qualified D1L identity
  (`neopi5` stable by-id plus `1A86:7523`, or Windows `COM12`);
- board/display/touch/core UI green;
- controlled bidirectional DM/ACK/PATH/direct route green;
- retained state survives required reboots;
- no crash-like resets;
- 90-minute Core soak green;
- install/recovery docs packaged;
- zero known Core P0;
- zero known Core crash/data-loss/security P1;
- `core_release_ready=true`.

### Conditional SD pass

All SD gates green, or SD must be disabled. There is no partial claim.

### Hard no-go

Any of:

- stale or predecessor hardware evidence;
- firmware SHA mismatch;
- checksum mismatch;
- Actions failure;
- unexpected reset/boot loop;
- data loss;
- missing inbound or outbound DM;
- false ACK/delivery state;
- unsupported feature still reachable;
- heap/task-stack failure;
- SD advertised despite a failed or missing SD gate;
- missing recovery path;
- evidence manually edited to pass;
- use of forbidden ports;
- on-device SD formatting.

---

## 10. Issues and pull requests after Core release

### Close only when proven

Likely candidates after exact Core evidence:

- portions of #7, #63, #66, #69, #70, #71, #74, #75, #76;
- #78 only if SD is supported and its full stated acceptance is actually met.

Do not close an umbrella issue merely because the Core profile excludes part of it. Instead add a Core-release comment and create/move deferred work to the Full Feature milestone.

### Relabel/defer

- #13 Wi-Fi;
- #14 Map;
- #20 QR polish;
- #21 OTA;
- #67 multi-channel/contact breadth;
- #68 user-facing PATH/TRACE breadth;
- #73 Map location;
- #77 administration;
- BLE PR #199 and its associated work.

### Retain full-release roadmap

After the release, reset priority to:

1. BLE compilation and runtime ownership;
2. signed update/recovery;
3. Map/Wi-Fi/SD performance;
4. multi-channel and contact breadth;
5. authenticated administration;
6. full 12-hour soak and Full Feature gate.

---

## 11. Final recommendation

Do not ask Codex to “finish all open P0s in 24 hours.” That repeats the failure mode that produced the current sprawling completion pack.

Give Codex one product contract, one integration branch, one shared backlog, one candidate workflow, one exact device sweep, and one release gate.

The fastest honest production path is:

1. explicitly redefine 1.0 as Core 1.0;
2. hide and reject incomplete surfaces;
3. retain the strongest automated pipeline;
4. rebase the low-risk NVS write-suppression patch;
5. qualify DM, persistence, UI, boot, and diagnostics on one exact binary;
6. include SD only if its short exact-candidate matrix passes;
7. replace the 12-hour Core gate with a documented 90-minute risk-accepted gate while preserving the original 12-hour Full Feature gate;
8. publish only when the separate Core audit is fully green.
